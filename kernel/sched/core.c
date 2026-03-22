#include "sched.h"
#include "asm/barrier.h"
#include "lib/libirq.h"
#include "lib/libasm.h"
#include "lib/libmem.h"
#include "mmu.h"
#include "printk.h"
#include "smp.h"
#include "timer.h"
#include "shell.h"
#include "spinlock.h"

struct sched_state
{
    spinlock_t lock;
    struct task_struct tasks[CONFIG_MAX_TASKS];
    struct task_struct idle[CONFIG_MAX_CPUS];
    struct rq rq[CONFIG_MAX_CPUS];
    int32_t next_pid;
};

static struct sched_state sched_state;

static void copy_task_name(int8_t *dst, const int8_t *src);
static void init_boot_task(struct task_struct *task, uint32_t cpu);
static void init_idle_task(struct task_struct *idle, uint32_t cpu)
{
    memset((int8_t *)idle, 0, sizeof(*idle));
    idle->pid = -1;
    idle->cpu = cpu;
    idle->is_idle = true;
    idle->state = TASK_RUNNING;
    idle->weight = 1024;
    idle->sleep_until = 0;
    idle->exec_base = 0;
    idle->exec_end = 0;
    idle->has_exec_image = false;
    copy_task_name(idle->comm, (const int8_t *)"idle");
}

static void init_boot_task(struct task_struct *task, uint32_t cpu)
{
    memset((int8_t *)task, 0, sizeof(*task));
    task->pid = 0;
    task->cpu = cpu;
    task->is_idle = false;
    task->state = TASK_RUNNING;
    task->weight = 1024;
    task->sleep_until = 0;
    task->exec_base = 0;
    task->exec_end = 0;
    task->has_exec_image = false;
    copy_task_name(task->comm, (const int8_t *)"boot");
}

static void copy_task_name(int8_t *dst, const int8_t *src)
{
    uint32_t i;

    if (!src)
    {
        src = (const int8_t *)"kthread";
    }

    for (i = 0; i < TASK_COMM_LEN - 1 && src[i] != '\0'; i++)
    {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static struct rq *this_rq(void)
{
    return &sched_state.rq[smp_cpu_id()];
}

struct task_struct *task_by_pid(int32_t pid)
{
    uint32_t i;

    for (i = 0; i < CONFIG_MAX_TASKS; i++)
    {
        if (sched_state.tasks[i].state != TASK_UNUSED &&
            sched_state.tasks[i].pid == pid)
        {
            return &sched_state.tasks[i];
        }
    }

    return 0;
}

static bool task_is_runnable_on_cpu(const struct task_struct *task, uint32_t cpu)
{
    if (task->cpu != cpu)
    {
        return false;
    }

    return task->state == TASK_RUNNABLE || task->state == TASK_RUNNING;
}

static bool task_has_valid_context(const struct task_struct *task)
{
    uint64_t lr;

    if (!task)
    {
        return false;
    }

    /*
     * cpu_switch_to() 依赖 next->cpu_ctx 里已经有一份可恢复的 sp/lr。
     * 一旦这里被异常写坏，继续切过去就会直接跳进数据区或空地址。
     * 先做最小合法性检查，宁可回落到 idle，也不要把整颗 CPU 交给坏上下文。
     */
    if (!task->cpu_ctx.sp || !task->cpu_ctx.lr)
    {
        return false;
    }

    if ((task->cpu_ctx.sp & 0xfUL) != 0)
    {
        return false;
    }

    /*
     * 返回地址必须落在“当前可解释的代码区域”里：
     * - kernel thread / syscall 路径：kernel text
     * - exec 出来的 ELF 任务：自己的装载区
     *
     * 这样即使某次栈被写坏，也不会把 next task 直接跳进 .bss / framebuffer。
     */
    lr = task->cpu_ctx.lr;
    if ((lr >= (uint64_t)&__text_start && lr < (uint64_t)&__text_end) ||
        (lr >= kimage_phys_to_virt((uint64_t)&__text_start) &&
         lr < kimage_phys_to_virt((uint64_t)&__text_end)))
    {
        return true;
    }

    if (task->has_exec_image &&
        lr >= task->exec_base &&
        lr < task->exec_end)
    {
        return true;
    }

    return false;
}

static void rq_recalc_min_vruntime(struct rq *rq, uint32_t cpu)
{
    uint32_t i;
    bool found;

    found = false;
    rq->min_vruntime = 0;

    for (i = 0; i < CONFIG_MAX_TASKS; i++)
    {
        struct task_struct *task = &sched_state.tasks[i];

        if (!task_is_runnable_on_cpu(task, cpu))
        {
            continue;
        }

        if (!found || task->vruntime < rq->min_vruntime)
        {
            rq->min_vruntime = task->vruntime;
            found = true;
        }
    }
}

static uint32_t sched_pick_target_cpu(void)
{
    uint32_t cpu;
    uint32_t best_cpu;
    uint32_t best_load;

    best_cpu = smp_cpu_id();
    best_load = sched_state.rq[best_cpu].nr_running;

    for (cpu = 0; cpu < smp_cpu_count(); cpu++)
    {
        uint32_t load = sched_state.rq[cpu].nr_running;

        if (load < best_load)
        {
            best_cpu = cpu;
            best_load = load;
        }
    }

    return best_cpu;
}

static const int8_t *task_state_name(enum task_state state)
{
    switch (state)
    {
    case TASK_UNUSED:
        return (const int8_t *)"unused";
    case TASK_RUNNING:
        return (const int8_t *)"running";
    case TASK_RUNNABLE:
        return (const int8_t *)"runnable";
    case TASK_SLEEPING:
        return (const int8_t *)"sleeping";
    case TASK_DEAD:
        return (const int8_t *)"dead";
    default:
        return (const int8_t *)"?";
    }
}

static struct task_struct *pick_next_task(struct rq *rq, struct task_struct *prev)
{
    struct task_struct *best;
    uint32_t cpu;
    int32_t i;

    cpu = smp_cpu_id();
    best = 0;

    for (i = 0; i < CONFIG_MAX_TASKS; i++)
    {
        struct task_struct *task = &sched_state.tasks[i];

        if (!task_is_runnable_on_cpu(task, cpu))
        {
            continue;
        }

        if (task != prev && !task_has_valid_context(task))
        {
            continue;
        }

        if (!best || task->vruntime < best->vruntime)
        {
            best = task;
        }
    }

    rq_recalc_min_vruntime(rq, cpu);
    if (best)
    {
        return best;
    }

    /*
     * tasks[] 里只放普通任务，per-cpu idle 任务单独存放。
     * 如果当前 CPU 上暂时没有任何 runnable 普通任务，就明确回退到
     * 本 CPU 的 idle task，而不是把“上一个任务”硬留在 rq->curr。
     */
    return &sched_state.idle[cpu];
}

static void sched_prepare_yield(struct rq *rq)
{
    struct task_struct *curr;

    curr = rq->curr;
    if (!curr || curr->is_idle)
    {
        return;
    }

    /*
     * 主动 yield 表示“我愿意把 CPU 让出去”。
     * 这里把当前任务的 vruntime 至少推进到当前 runqueue 最小值之后，
     * 这样下一次挑选时，其他同权重任务就有机会先运行。
     */
    rq_recalc_min_vruntime(rq, smp_cpu_id());
    if (curr->vruntime <= rq->min_vruntime)
    {
        curr->vruntime = rq->min_vruntime + 1;
        return;
    }

    curr->vruntime++;
}

static void sched_wake_sleepers_locked(void)
{
    uint32_t i;

    /*
     * 任务数很少，先用线性扫描实现睡眠唤醒。
     * 这比复杂的时间轮更适合当前阶段，也方便后续扩展到 futex / timeout read。
     */
    for (i = 0; i < CONFIG_MAX_TASKS; i++)
    {
        struct task_struct *task = &sched_state.tasks[i];
        struct rq *rq;

        if (task->state != TASK_SLEEPING)
        {
            continue;
        }

        if (task->sleep_until > jiffies)
        {
            continue;
        }

        rq = &sched_state.rq[task->cpu];
        task->state = TASK_RUNNABLE;
        task->sleep_until = 0;
        rq->nr_running++;
        rq->need_resched = true;
    }
}

static void __schedule_locked(struct rq *rq)
{
    struct task_struct *prev;
    struct task_struct *next;

    prev = rq->curr;
    next = pick_next_task(rq, prev);
    if (next == prev)
    {
        rq->need_resched = false;
        return;
    }

    if (prev->state == TASK_RUNNING)
    {
        prev->state = TASK_RUNNABLE;
    }

    next->state = TASK_RUNNING;
    prev->switches++;
    next->switches++;
    rq->curr = next;
    rq->need_resched = false;

    /*
     * 这里必须在真正切栈前释放 runqueue 锁。
     * 否则新任务运行后再进入调度器，会把同一把锁锁死。
     */
    spin_unlock(&sched_state.lock);
    cpu_switch_to(&prev->cpu_ctx, &next->cpu_ctx);
    spin_lock(&sched_state.lock);
}

static void task_trampoline(void) __attribute__((noreturn));

static void task_trampoline(void)
{
    struct task_struct *task;

    enable_irq();

    task = task_current();
    task->entry(task->arg);
    task_exit();
}

struct task_struct *task_current(void)
{
    return this_rq()->curr;
}

void task_set_cleanup(task_cleanup_t cleanup, void *arg)
{
    struct task_struct *task;
    uint64_t daif;

    daif = read_daif();
    disable_irq();
    task = task_current();
    if (task)
    {
        task->cleanup = cleanup;
        task->cleanup_arg = arg;
    }
    write_daif(daif);
}

void sched_init(void)
{
    struct task_struct *boot_task;
    struct rq *rq;
    uint32_t cpu;
    uint32_t boot_cpu;

    memset((int8_t *)&sched_state, 0, sizeof(sched_state));
    spin_lock_init(&sched_state.lock);
    boot_cpu = smp_cpu_id();

    for (cpu = 0; cpu < CONFIG_MAX_CPUS; cpu++)
    {
        sched_state.rq[cpu].curr = 0;
        sched_state.rq[cpu].min_vruntime = 0;
        sched_state.rq[cpu].nr_running = 0;
        sched_state.rq[cpu].need_resched = false;
        init_idle_task(&sched_state.idle[cpu], cpu);
        sched_state.rq[cpu].curr = &sched_state.idle[cpu];
    }

    /*
     * 这里必须把当前正在执行 kernel_main_high() 的启动线程显式接管成
     * 一个真实 task_struct。
     *
     * 否则第一次 sched_yield() 会把当前 CPU 上下文保存进 idle task，
     * 而 idle task 又不在普通可调度任务集合里，boot 线程就再也回不来了，
     * 后续文件系统 / PCI / 网络 / UI 初始化会永远停在 yield 之前。
     */
    boot_task = &sched_state.tasks[0];
    init_boot_task(boot_task, boot_cpu);

    rq = this_rq();
    rq->curr = boot_task;
    rq->nr_running = 1;
    rq->min_vruntime = 0;
    sched_state.next_pid = 1;

    printk("[sched\tinit]: scheduler ready, boot cpu=%u boot-task=pid%d\n",
           boot_cpu, boot_task->pid);
}

void sched_init_secondary(uint32_t cpu_id)
{
    init_idle_task(&sched_state.idle[cpu_id], cpu_id);
    sched_state.rq[cpu_id].curr = &sched_state.idle[cpu_id];
    sched_state.rq[cpu_id].min_vruntime = 0;
    sched_state.rq[cpu_id].nr_running = 0;
    sched_state.rq[cpu_id].need_resched = false;
}

int kthread_create(const int8_t *name, task_entry_t entry, void *arg)
{
    struct task_struct *task;
    uint64_t sp;
    int32_t pid;
    int32_t slot;

    if (!entry)
    {
        return -1;
    }

    disable_irq();
    spin_lock(&sched_state.lock);

    for (slot = 0; slot < CONFIG_MAX_TASKS; slot++)
    {
        if (sched_state.tasks[slot].state == TASK_UNUSED ||
            sched_state.tasks[slot].state == TASK_DEAD)
        {
            break;
        }
    }

    if (slot == CONFIG_MAX_TASKS)
    {
        spin_unlock(&sched_state.lock);
        enable_irq();
        return -1;
    }

    task = &sched_state.tasks[slot];
    memset((int8_t *)task, 0, sizeof(*task));

    pid = sched_state.next_pid++;
    task->pid = pid;
    task->cpu = sched_pick_target_cpu();
    task->state = TASK_RUNNABLE;
    task->weight = 1024;
    task->entry = entry;
    task->arg = arg;
    task->exec_base = 0;
    task->exec_end = 0;
    task->has_exec_image = false;
    copy_task_name(task->comm, name);

    sp = (uint64_t)&task->stack[TASK_STACK_SIZE];
    sp &= ~0xfUL;
    task->cpu_ctx.sp = sp;
    task->cpu_ctx.lr = (uint64_t)task_trampoline;
    sched_state.rq[task->cpu].nr_running++;
    rq_recalc_min_vruntime(&sched_state.rq[task->cpu], task->cpu);
    sched_state.rq[task->cpu].need_resched = true;

    spin_unlock(&sched_state.lock);
    enable_irq();
    return pid;
}

void sched_yield(void)
{
    struct rq *rq;

    disable_irq();
    spin_lock(&sched_state.lock);
    rq = this_rq();
    if (rq->nr_running > 1)
    {
        sched_prepare_yield(rq);
    }
    rq->need_resched = true;
    __schedule_locked(rq);
    spin_unlock(&sched_state.lock);
    enable_irq();
}

void sched_sleep_until(uint64_t wake_jiffies)
{
    struct rq *rq;
    struct task_struct *curr;

    disable_irq();
    spin_lock(&sched_state.lock);

    rq = this_rq();
    curr = rq->curr;
    if (!curr || curr->is_idle)
    {
        spin_unlock(&sched_state.lock);
        enable_irq();
        return;
    }

    /*
     * 睡眠时直接从当前 CPU 的 runnable 计数中移除，
     * 到期后由 timer tick 重新放回 runnable 集合。
     */
    curr->state = TASK_SLEEPING;
    curr->sleep_until = wake_jiffies;
    if (rq->nr_running > 0)
    {
        rq->nr_running--;
    }
    rq->need_resched = true;
    __schedule_locked(rq);

    spin_unlock(&sched_state.lock);
    enable_irq();
}

void sched_sleep_ms(uint32_t sleep_ms)
{
    uint64_t wake_jiffies;

    if (!sleep_ms)
    {
        return;
    }

    wake_jiffies = (uint64_t)jiffies + ((uint64_t)sleep_ms * STUPIDOS_TIMER_HZ + 999ULL) / 1000ULL;
    sched_sleep_until(wake_jiffies);
}

void scheduler_tick(void)
{
    struct task_struct *task;
    struct rq *rq;

    rq = this_rq();
    task = rq->curr;
    if (!task)
    {
        return;
    }

    /*
     * 这里先实现一个最小 CFS 雏形：
     * 每个 tick 增加当前任务的 vruntime；
     * 当它领先当前 runqueue 最小 vruntime 足够多时，请求重新调度。
    */
    task->exec_start++;
    task->vruntime += 1024 / task->weight;
    /*
     * 让当前 CPU 的最小虚拟运行时间保持新鲜。
     * 这样后续 need_resched 的判断不会一直拿着旧账本，前台输入和短任务
     * 的抢占会更及时。
     */
    rq_recalc_min_vruntime(rq, smp_cpu_id());

    spin_lock(&sched_state.lock);
    sched_wake_sleepers_locked();
    spin_unlock(&sched_state.lock);

    if (rq->nr_running > 1 &&
        task->vruntime >= rq->min_vruntime + SCHED_LATENCY_TICKS)
    {
        rq->need_resched = true;
    }
}

void sched_show_tasks(void)
{
    uint32_t i;
    uint32_t cpu;

    printk("[sched\tps  ]: tasks:\n");
    for (i = 0; i < CONFIG_MAX_TASKS; i++)
    {
        struct task_struct *task = &sched_state.tasks[i];

        if (task->state == TASK_UNUSED)
        {
            continue;
        }

        printk("  pid=%d cpu=%u state=%s idle=%u switches=%lu vruntime=%lu comm=%s\n",
               task->pid, task->cpu, task_state_name(task->state), task->is_idle,
               task->switches, task->vruntime, task->comm);
    }

    printk("[sched\tps  ]: runqueues:\n");
    for (cpu = 0; cpu < smp_cpu_count(); cpu++)
    {
        printk("  cpu=%u curr=%d nr_running=%u min_vruntime=%lu need_resched=%u\n",
               cpu,
               sched_state.rq[cpu].curr ? sched_state.rq[cpu].curr->pid : -1,
               sched_state.rq[cpu].nr_running,
               sched_state.rq[cpu].min_vruntime,
               sched_state.rq[cpu].need_resched);
    }
}

void sched_maybe_resched(void)
{
    struct rq *rq;

    rq = this_rq();
    if (!rq->need_resched)
    {
        return;
    }

    sched_yield();
}

void task_exit(void)
{
    task_cleanup_t cleanup;
    void *cleanup_arg;
    struct rq *rq;
    struct task_struct *curr;

    curr = task_current();

    disable_irq();
    cleanup = 0;
    cleanup_arg = 0;
    if (curr)
    {
        cleanup = curr->cleanup;
        cleanup_arg = curr->cleanup_arg;
        curr->cleanup = 0;
        curr->cleanup_arg = 0;
    }
    if (cleanup)
    {
        cleanup(cleanup_arg);
    }

    spin_lock(&sched_state.lock);

    rq = this_rq();

    rq->curr->state = TASK_DEAD;
    /*
     * 退出事件不是中断，所以不能只靠 wfi 等 timer。
     * 这里发一个 event，把所有在 wfe 上等“进程退出 / 输入到达”的线程
     * 一起唤醒，父 shell 就能马上继续执行。
     */
    sev();
    if (rq->nr_running)
    {
        rq->nr_running--;
    }
    rq_recalc_min_vruntime(rq, rq->curr->cpu);
    __schedule_locked(rq);

    spin_unlock(&sched_state.lock);
    while (1)
    {
        asm volatile("wfi");
    }
}
