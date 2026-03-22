#ifndef __SCHED_H__
#define __SCHED_H__

#include "asm/types.h"
#include "smp.h"

#define TASK_COMM_LEN       16
#define TASK_STACK_SIZE     16384
#define CONFIG_MAX_TASKS    16
#define SCHED_LATENCY_TICKS 4

typedef void (*task_entry_t)(void *arg);
typedef void (*task_cleanup_t)(void *arg);

enum task_state
{
    TASK_UNUSED = 0,
    TASK_RUNNING,
    TASK_RUNNABLE,
    TASK_SLEEPING,
    TASK_DEAD,
};

/*
 * cpu_context 只保存 AArch64 C ABI 规定的被调用者保存寄存器，
 * 这正是普通 C 函数切换上下文时最小必须保存的状态。
 */
struct cpu_context
{
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t x29;
    uint64_t lr;
    uint64_t sp;
};

struct task_struct
{
    int32_t pid;
    uint32_t cpu;
    bool is_idle;
    enum task_state state;
    uint64_t switches;
    uint64_t vruntime;
    uint64_t exec_start;
    uint64_t weight;
    uint64_t sleep_until;
    task_entry_t entry;
    void *arg;
    task_cleanup_t cleanup;
    void *cleanup_arg;
    uint64_t exec_base;
    uint64_t exec_end;
    bool has_exec_image;
    int8_t comm[TASK_COMM_LEN];
    struct cpu_context cpu_ctx;
    uint8_t stack[TASK_STACK_SIZE] __attribute__((aligned(16)));
};

/*
 * 每个 CPU 拥有自己的 runqueue。
 * 当前实现还是最小版本，但接口已经按后续 CFS/SMP 调度需要来组织：
 * - curr: 当前正在运行的任务
 * - need_resched: 定时器 tick 请求重新调度
 * - min_vruntime: 当前 CPU 运行队列上的最小虚拟运行时间
 */
struct rq
{
    struct task_struct *curr;
    uint64_t min_vruntime;
    uint32_t nr_running;
    bool need_resched;
};

void sched_init(void);
void sched_init_secondary(uint32_t cpu_id);
int kthread_create(const int8_t *name, task_entry_t entry, void *arg);
void sched_yield(void);
void sched_sleep_until(uint64_t wake_jiffies);
void sched_sleep_ms(uint32_t sleep_ms);
void scheduler_tick(void);
void sched_maybe_resched(void);
void sched_show_tasks(void);
void task_exit(void) __attribute__((noreturn));
struct task_struct *task_current(void);
struct task_struct *task_by_pid(int32_t pid);
void task_set_cleanup(task_cleanup_t cleanup, void *arg);

/*
 * 汇编实现的上下文切换例程。
 * prev 和 next 都指向 task_struct 里的 cpu_context。
 */
void cpu_switch_to(struct cpu_context *prev, struct cpu_context *next);

#endif
