#include "syscall.h"

#include "driver/uart.h"
#include "errno.h"
#include "fs/vfs.h"
#include "exec.h"
#include "printk.h"
#include "lib/libasm.h"
#include "lib/libirq.h"
#include "lib/libmem.h"
#include "mm/mm.h"
#include "mm/page_alloc.h"
#include "mmu.h"
#include "sched.h"
#include "spinlock.h"
#include "tty.h"
#include "timer.h"
#include "net/net.h"

static uint64_t sys_monotonic_usec(void)
{
    uint64_t cnt;
    uint64_t freq;

    asm volatile("mrs %0, cntpct_el0" : "=r"(cnt) : : "memory");
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq) : : "memory");
    if (!freq)
    {
        return 0;
    }

    return (cnt * 1000000ULL) / freq;
}

static uint64_t sys_random_state = 0x9e3779b97f4a7c15ULL;
static uint32_t sys_getrandom_trace_count;

struct futex_waiter
{
    bool used;
    uint64_t addr;
    struct task_struct *task;
};

static spinlock_t sys_futex_lock = SPINLOCK_INIT;
static struct futex_waiter sys_futex_waiters[16];

static uint32_t sys_order_for_size(uint64_t size)
{
    uint32_t order;
    uint64_t bytes;

    order = 0;
    bytes = PAGE_SIZE;
    while (bytes < size && order < PAGE_ALLOC_MAX_ORDER)
    {
        order++;
        bytes <<= 1;
    }

    return order;
}

static void sys_task_heap_ensure(struct task_struct *task)
{
    void *heap;
    uint64_t heap_bytes;

    if (!task || task->heap_base)
    {
        return;
    }

    heap = alloc_pages(TASK_HEAP_ORDER);
    if (!heap)
    {
        return;
    }

    heap_bytes = (uint64_t)PAGE_SIZE << TASK_HEAP_ORDER;
    task->heap_order = TASK_HEAP_ORDER;
    task->heap_base = (uint64_t)heap;
    task->heap_brk = task->heap_base;
    task->heap_end = task->heap_base + heap_bytes;
    memset((int8_t *)heap, 0, (size_t)heap_bytes);
}

static int64_t sys_task_mmap_alloc(struct task_struct *task, uint64_t size)
{
    uint32_t i;
    uint32_t order;
    void *addr;
    uint64_t bytes;

    if (!task || !size)
    {
        return -EINVAL;
    }

    for (i = 0; i < TASK_MAX_MMAPS; i++)
    {
        if (!task->mmaps[i].used)
        {
            break;
        }
    }

    if (i == TASK_MAX_MMAPS)
    {
        return -ENOMEM;
    }

    order = sys_order_for_size(size);
    addr = alloc_pages(order);
    if (!addr)
    {
        return -ENOMEM;
    }

    bytes = (uint64_t)PAGE_SIZE << order;
    memset((int8_t *)addr, 0, (size_t)bytes);
    task->mmaps[i].used = true;
    task->mmaps[i].addr = (uint64_t)addr;
    task->mmaps[i].size = bytes;
    task->mmaps[i].order = order;
    return (int64_t)(uint64_t)addr;
}

static bool sys_range_inside(uint64_t addr, size_t len, uint64_t start, uint64_t end)
{
    uint64_t last;

    if (!start || end <= start)
    {
        return false;
    }

    if (addr < start || addr >= end)
    {
        return false;
    }

    if (!len)
    {
        return true;
    }

    if (len - 1 > end - 1 - addr)
    {
        return false;
    }

    last = addr + (uint64_t)len - 1ULL;
    return last < end;
}

static bool sys_user_mem_valid(const void *user_ptr, size_t len)
{
    struct task_struct *task;
    uint64_t addr;
    uint64_t stack_base;
    uint64_t stack_phys;
    uint64_t stack_linear;
    uint64_t stack_kimage;
    uint32_t i;

    if (!user_ptr)
    {
        return false;
    }

    addr = (uint64_t)user_ptr;
    if (len > 0 && addr + (uint64_t)len < addr)
    {
        return false;
    }

    task = task_current();
    if (!task || !task->has_exec_image)
    {
        return false;
    }

    stack_base = (uint64_t)&task->stack[0];
    stack_phys = kernel_virt_to_phys(stack_base);
    stack_linear = linear_phys_to_virt(stack_phys);
    stack_kimage = kimage_phys_to_virt(stack_phys);

    if (sys_range_inside(addr, len, task->exec_base, task->exec_end) ||
        /*
         * Python 等静态用户态程序目前仍可能保留少量低地址绝对引用。
         * exec 层会给这段低地址做 alias 映射，这里也要把该范围视为合法
         * 用户地址，否则 getrandom/read/write 这类 syscall 会误判 EFAULT。
         */
        sys_range_inside(addr, len, task->exec_alias_base, task->exec_alias_end) ||
        sys_range_inside(addr, len, task->heap_base, task->heap_end) ||
        /*
         * 栈地址兼容修复（中文）：
         * task->stack 在调度器里常以“低地址别名”保存，
         * 但用户态实际运行时 SP 可能落在 KIMAGE 高地址别名。
         * 这里只认低地址会把合法 stdin/read/write 缓冲误判为 EFAULT。
         */
        sys_range_inside(addr, len, stack_base, stack_base + TASK_STACK_SIZE) ||
        sys_range_inside(addr, len, stack_linear, stack_linear + TASK_STACK_SIZE) ||
        sys_range_inside(addr, len, stack_kimage, stack_kimage + TASK_STACK_SIZE))
    {
        return true;
    }

    for (i = 0; i < TASK_MAX_MMAPS; i++)
    {
        if (!task->mmaps[i].used)
        {
            continue;
        }

        if (sys_range_inside(addr, len, task->mmaps[i].addr, task->mmaps[i].addr + task->mmaps[i].size))
        {
            return true;
        }
    }

    return false;
}

static int64_t sys_copy_from_user(void *dst, const void *user_src, size_t len)
{
    if (!len)
    {
        return 0;
    }

    if (!dst || !sys_user_mem_valid(user_src, len))
    {
        return -EFAULT;
    }

    memcpy((int8_t *)dst, (int8_t *)user_src, len);
    return 0;
}

static int64_t sys_copy_to_user(void *user_dst, const void *src, size_t len)
{
    if (!len)
    {
        return 0;
    }

    if (!src || !sys_user_mem_valid(user_dst, len))
    {
        return -EFAULT;
    }

    memcpy((int8_t *)user_dst, (int8_t *)src, len);
    return 0;
}

static int64_t sys_copy_path_from_user(int8_t *dst, size_t dst_len, const int8_t *user_path)
{
    size_t n;
    int8_t ch;

    if (!dst || !dst_len)
    {
        return -EINVAL;
    }

    if (!user_path)
    {
        return -EINVAL;
    }

    /*
     * 关键修复（中文）：
     * 旧实现直接要求 `user_path` 起始处连续 `dst_len` 字节都可访问，
     * 对短字符串/argv 边界很不友好，容易把合法路径误判为 EFAULT。
     *
     * 新实现改为“按字节探测+拷贝，直到遇到 '\\0'”：
     * - 每次只验证当前 1 字节可访问
     * - 既保留防护，也避免 `ls /bin` 这类短路径被误杀
     */
    n = 0;
    while (n + 1 < dst_len)
    {
        if (!sys_user_mem_valid((const void *)(user_path + n), 1))
        {
            return -EFAULT;
        }

        ch = user_path[n];
        dst[n] = ch;
        if (ch == '\0')
        {
            return 0;
        }

        n++;
    }

    dst[dst_len - 1] = '\0';
    return -ENAMETOOLONG;
}

static int64_t sys_read(int64_t fd, int64_t buf, int64_t len)
{
    uint8_t *bytes;
    int64_t i;
    int32_t ch;
    uint64_t wait_daif;

    if (fd == 0)
    {
        if (len <= 0)
        {
            return 0;
        }

        bytes = (uint8_t *)buf;
        /*
         * stdin 这里改成“行缓冲阻塞等待”：
         * - 字符一到就由 tty 层立刻回显
         * - read() 只负责把整行收齐，再一次性交给用户态 shell
         *
         * 这样可以把“按键 -> 回显”与“读取命令行”分离开，
         * 避免 shell 为了每个字符都做一次 syscall / 调度往返。
         */
        for (i = 0; i < len; )
        {
            /*
             * 输入路径尽量只做一次快速探测。
             * 之前这里的多次 yield 会引入可见的调度抖动，
             * 反而让“按键 -> 回显”变慢。
             */
            ch = tty_try_getc();
            if (ch >= 0)
            {
                /*
                 * 按字节校验用户缓冲（中文）：
                 * 避免一次性校验整段 len 导致“跨边界误判 EFAULT”，
                 * 进而出现“输入有回显但命令不执行”的假死体验。
                 */
                if (!sys_user_mem_valid((const void *)(bytes + i), 1))
                {
                    return (i > 0) ? i : -EFAULT;
                }
                bytes[i++] = (uint8_t)ch;
                if (ch == '\n' || ch == '\r')
                {
                    /*
                     * 串口终端和虚拟键盘可能送回车或换行两种形式。
                     * 这里统一把行结束条件放宽，避免用户按 Enter 后还要等下一次事件。
                     */
                    return i;
                }
                continue;
            }

            /*
             * 没有新字符时进入事件等待。
             * 这里不要在读到半行后就返回，避免把 shell 重新拖回
             * “每个字符一次 read/syscall”的低效路径。
             */
            wait_daif = read_daif();
            enable_irq();
            wfe();
            write_daif(wait_daif);
        }

        return i;
    }

    if (fd >= 3)
    {
        if (!sys_user_mem_valid((const void *)buf, (size_t)len))
        {
            return -EFAULT;
        }

        return vfs_read((int)(fd - 3), (void *)buf, (size_t)len);
    }

    return -ENOSYS;
}

static int64_t sys_write(int64_t fd, int64_t buf, int64_t len)
{
    const uint8_t *bytes;

    if (len < 0)
    {
        return -EINVAL;
    }

    if (fd == 1 || fd == 2)
    {
        if (!sys_user_mem_valid((const void *)buf, (size_t)len))
        {
            return -EFAULT;
        }

        bytes = (const uint8_t *)buf;
        tty_write_bytes(bytes, (size_t)len);
        return len;
    }

    if (fd >= 3)
    {
        if (!sys_user_mem_valid((const void *)buf, (size_t)len))
        {
            return -EFAULT;
        }

        return vfs_write((int)(fd - 3), (const void *)buf, (size_t)len);
    }

    return -EBADF;
}

static int64_t sys_open(int64_t path, int64_t flags)
{
    int8_t kpath[VFS_PATH_MAX];
    int fd;
    int64_t ret;

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    fd = vfs_open(kpath, (int)flags);
    if (fd < 0)
    {
        return fd;
    }

    return fd + 3;
}

static int64_t sys_close(int64_t fd)
{
    if (fd < 3)
    {
        return 0;
    }

    return vfs_close((int)(fd - 3));
}

static int64_t sys_lseek(int64_t fd, int64_t offset, int64_t whence)
{
    if (fd < 3)
    {
        return -ESPIPE;
    }

    return vfs_lseek((int)(fd - 3), offset, (int)whence);
}

static int64_t sys_yield(void)
{
    sched_yield();
    return 0;
}

static int64_t sys_getpid(void)
{
    struct task_struct *task;

    task = task_current();
    if (!task)
    {
        return -ESRCH;
    }

    return task->pid;
}

static int64_t sys_time(void)
{
    return (int64_t)jiffies;
}

static int64_t sys_exit(int64_t code)
{
    (void)code;
    task_exit();
    __builtin_unreachable();
}

static int64_t sys_readdir(int64_t path, int64_t index, int64_t out)
{
    int8_t kpath[VFS_PATH_MAX];
    struct vfs_dirent ent;
    int64_t ret;

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    ret = vfs_readdir(kpath, (uint32_t)index, &ent);
    if (ret < 0)
    {
        return ret;
    }

    return sys_copy_to_user((void *)out, &ent, sizeof(ent));
}

static int64_t sys_nettest(void)
{
    return net_selftest();
}

static int64_t sys_netping(int64_t target_ip, int64_t seq, int64_t timeout_ms)
{
    struct task_struct *task;
    struct net_device *dev;

    task = task_current();
    dev = net_default_device();
    if (!dev)
    {
        return -ENODEV;
    }

    if (target_ip < 0 || (uint64_t)target_ip > 0xffffffffUL)
    {
        return -EINVAL;
    }

    if (seq < 0 || seq > 0xffff)
    {
        return -EINVAL;
    }

    if (timeout_ms <= 0)
    {
        timeout_ms = 1000;
    }

    return net_ping(dev,
                    (uint32_t)target_ip,
                    task ? (uint16_t)task->pid : 0,
                    (uint16_t)seq,
                    (uint32_t)timeout_ms);
}

static int64_t sys_waitpid(int64_t pid)
{
    struct task_struct *self;
    struct task_struct *task;
    bool has_child;
    uint32_t idx;

    self = task_current();
    if (!self)
    {
        return -ESRCH;
    }

    /*
     * 兼容语义补齐（中文）：
     * - waitpid(-1): 等待“任意一个子进程”
     * - waitpid(pid): 等待指定子进程
     *
     * 当前仍是最小实现：不引入僵尸队列，只用 task 状态轮询 + wfe/sev。
     */
    if (pid == -1)
    {
        for (;;)
        {
            has_child = false;

            /*
             * 关键优化（中文）：
             * 旧实现按 PID 范围线性探测，PID 变大后会把 waitpid 开销放大到
             * 每次几万次查找，直接拖慢前台 shell 交互。
             *
             * 这里改成按调度器 task 槽位扫描（固定 CONFIG_MAX_TASKS），
             * 开销稳定且与历史 PID 大小无关。
             */
            for (idx = 0; idx < task_slot_count(); idx++)
            {
                task = task_slot_get(idx);
                if (!task)
                {
                    continue;
                }

                if (task->ppid != self->pid)
                {
                    continue;
                }

                has_child = true;
                if (task->state == TASK_DEAD)
                {
                    return task->pid;
                }
            }

            if (!has_child)
            {
                return -ECHILD;
            }

            enable_irq();
            wfe();
            disable_irq();
        }
    }

    if (pid < -1)
    {
        return -EINVAL;
    }

    task = task_by_pid((int32_t)pid);
    if (!task)
    {
        return -ESRCH;
    }

    if (task == self)
    {
        return -EINVAL;
    }

    if (task->ppid != self->pid)
    {
        return -ECHILD;
    }

    while (task->state != TASK_DEAD)
    {
        enable_irq();
        wfe();
        disable_irq();
    }

    return task->pid;
}

static int64_t sys_sleep(int64_t ms)
{
    if (ms < 0)
    {
        return -EINVAL;
    }

    if (ms == 0)
    {
        return 0;
    }

    /*
     * 直接把睡眠交给调度器：
     * - 当前任务进入 TASK_SLEEPING
     * - timer tick 到期后自动唤醒
     * - 比 wfe 自旋更像真正的内核睡眠
     */
    sched_sleep_ms((uint32_t)ms);
    return 0;
}

static int64_t sys_netcfg(int64_t ipv4, int64_t netmask, int64_t gateway)
{
    if (ipv4 < 0 || netmask < 0 || gateway < 0)
    {
        return -EINVAL;
    }

    if ((uint64_t)ipv4 > 0xffffffffUL ||
        (uint64_t)netmask > 0xffffffffUL ||
        (uint64_t)gateway > 0xffffffffUL)
    {
        return -EINVAL;
    }

    return net_set_default_config((uint32_t)ipv4, (uint32_t)netmask, (uint32_t)gateway);
}

static int64_t sys_chdir(int64_t path)
{
    int8_t kpath[VFS_PATH_MAX];
    int64_t ret;

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    return vfs_chdir(kpath);
}

static int64_t sys_getcwd(int64_t buf, int64_t len)
{
    const int8_t *cwd;
    size_t cwd_len;
    size_t i;

    if (buf == 0 || len <= 0)
    {
        return -EINVAL;
    }

    cwd = task_cwd();
    if (!cwd || cwd[0] == '\0')
    {
        cwd = (const int8_t *)"/";
    }

    /*
     * cwd 只应该是一个固定长度的小字符串。
     * 这里用有上限的扫描替换无界 strlen，避免 cwd 状态被破坏时
     * 继续往后读到奇怪地址，把 shell 直接带进同步异常。
     */
    cwd_len = 0;
    for (i = 0; i < TASK_CWD_LEN; i++)
    {
        if (cwd[i] == '\0')
        {
            cwd_len = i + 1;
            break;
        }
    }

    if (cwd_len == 0)
    {
        cwd = (const int8_t *)"/";
        cwd_len = 2;
    }

    if (cwd_len > (size_t)len)
    {
        return -ERANGE;
    }

    if (!sys_user_mem_valid((const void *)buf, cwd_len))
    {
        return -EFAULT;
    }

    memcpy((int8_t *)buf, (int8_t *)cwd, cwd_len);
    return (int64_t)(cwd_len - 1);
}

static int64_t sys_stat(int64_t path, int64_t out)
{
    int8_t kpath[VFS_PATH_MAX];
    struct vfs_stat st;
    int64_t ret;

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    ret = vfs_stat(kpath, &st);
    if (ret < 0)
    {
        return ret;
    }

    return sys_copy_to_user((void *)out, &st, sizeof(st));
}

static int64_t sys_fstat(int64_t fd, int64_t out)
{
    struct vfs_stat st;
    int64_t kfd;
    int64_t ret;

    if (fd < 0)
    {
        return -EBADF;
    }

    kfd = (fd >= 3) ? (fd - 3) : fd;
    ret = vfs_fstat((int)kfd, &st);
    if (ret < 0)
    {
        return ret;
    }

    return sys_copy_to_user((void *)out, &st, sizeof(st));
}

static int64_t sys_uname(int64_t out)
{
    struct stupidos_utsname uts;

    if (!out)
    {
        return -EINVAL;
    }

    memset((int8_t *)&uts, 0, sizeof(uts));
    memcpy((int8_t *)uts.sysname, (int8_t *)"Stupidos", sizeof("Stupidos"));
    memcpy((int8_t *)uts.nodename, (int8_t *)"stupidos", sizeof("stupidos"));
    memcpy((int8_t *)uts.release, (int8_t *)"0.1", sizeof("0.1"));
    memcpy((int8_t *)uts.version, (int8_t *)"stupidos-aarch64", sizeof("stupidos-aarch64"));
    memcpy((int8_t *)uts.machine, (int8_t *)"aarch64", sizeof("aarch64"));
    memcpy((int8_t *)uts.domainname, (int8_t *)"localdomain", sizeof("localdomain"));
    return sys_copy_to_user((void *)out, &uts, sizeof(uts));
}

static int64_t sys_gettimeofday(int64_t out)
{
    struct stupidos_timeval tv;
    uint64_t usec;

    if (!out)
    {
        return -EINVAL;
    }

    usec = sys_monotonic_usec();
    tv.tv_sec = (int64_t)(usec / 1000000ULL);
    tv.tv_usec = (int64_t)(usec % 1000000ULL);
    return sys_copy_to_user((void *)out, &tv, sizeof(tv));
}

static int64_t sys_isatty(int64_t fd)
{
    if (fd >= 0 && fd <= 2)
    {
        return 1;
    }

    return 0;
}

static int64_t sys_dup2(int64_t oldfd, int64_t newfd)
{
    if (oldfd < 3 || newfd < 3)
    {
        /*
         * 当前 stdio 还走 tty 的特殊路径，不在 VFS fd 表里。
         * 先只支持普通文件的 dup2，避免把 tty 语义和文件表强行混在一起。
         */
        return -EBADF;
    }

    return vfs_dup2((int)(oldfd - 3), (int)(newfd - 3)) + 3;
}

static int64_t sys_brk(int64_t addr)
{
    struct task_struct *task;

    task = task_current();
    if (!task)
    {
        return -ESRCH;
    }

    sys_task_heap_ensure(task);
    if (!task->heap_base || !task->heap_end)
    {
        return -ENOMEM;
    }

    if (addr == 0)
    {
        return (int64_t)task->heap_brk;
    }

    if ((uint64_t)addr < task->heap_base || (uint64_t)addr > task->heap_end)
    {
        return (int64_t)task->heap_brk;
    }

    task->heap_brk = (uint64_t)addr;
    return addr;
}

static int64_t sys_mmap(int64_t addr, int64_t len, int64_t prot, int64_t flags, int64_t fd, int64_t off)
{
    struct task_struct *task;
    int64_t mapped;
    uint32_t used_slots;
    uint32_t i;

    (void)addr;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)off;

    if (len <= 0)
    {
        return -EINVAL;
    }

    task = task_current();
    if (!task)
    {
        return -ESRCH;
    }

    mapped = sys_task_mmap_alloc(task, (uint64_t)len);
    if (mapped < 0 && mapped >= -4095)
    {
        used_slots = 0;
        for (i = 0; i < TASK_MAX_MMAPS; i++)
        {
            if (task->mmaps[i].used)
            {
                used_slots++;
            }
        }

        printk("[syscall\tfault]: mmap fail pid=%d len=%ld ret=%ld used=%u/%u heap=%#lx..%#lx\n",
               task->pid, len, mapped, used_slots, TASK_MAX_MMAPS, task->heap_base, task->heap_end);
    }
    return mapped;
}

static int64_t sys_munmap(int64_t addr, int64_t len)
{
    struct task_struct *task;
    uint32_t i;

    if (addr == 0 || len <= 0)
    {
        return -EINVAL;
    }

    task = task_current();
    if (!task)
    {
        return -ESRCH;
    }

    for (i = 0; i < TASK_MAX_MMAPS; i++)
    {
        if (!task->mmaps[i].used || task->mmaps[i].addr != (uint64_t)addr)
        {
            continue;
        }

        free_pages((void *)task->mmaps[i].addr, task->mmaps[i].order);
        task->mmaps[i].used = false;
        task->mmaps[i].addr = 0;
        task->mmaps[i].size = 0;
        task->mmaps[i].order = 0;
        return 0;
    }

    return -EINVAL;
}

static int64_t sys_mprotect(int64_t addr, int64_t len, int64_t prot)
{
    (void)addr;
    (void)len;
    (void)prot;
    return 0;
}

static int64_t sys_clock_gettime(int64_t clockid, int64_t out)
{
    struct stupidos_timespec ts;
    uint64_t usec;

    (void)clockid;

    if (!out)
    {
        return -EINVAL;
    }

    usec = sys_monotonic_usec();
    ts.tv_sec = (int64_t)(usec / 1000000ULL);
    ts.tv_nsec = (int64_t)((usec % 1000000ULL) * 1000ULL);
    return sys_copy_to_user((void *)out, &ts, sizeof(ts));
}

static int64_t sys_nanosleep(int64_t req, int64_t rem)
{
    struct stupidos_timespec req_ts;
    struct stupidos_timespec rem_ts;
    uint64_t ms;

    if (!req)
    {
        return -EINVAL;
    }

    if (sys_copy_from_user(&req_ts, (const void *)req, sizeof(req_ts)) < 0)
    {
        return -EFAULT;
    }

    if (req_ts.tv_sec < 0 || req_ts.tv_nsec < 0)
    {
        return -EINVAL;
    }

    ms = (uint64_t)req_ts.tv_sec * 1000ULL + ((uint64_t)req_ts.tv_nsec + 999999ULL) / 1000000ULL;
    if (rem)
    {
        rem_ts.tv_sec = 0;
        rem_ts.tv_nsec = 0;
        if (sys_copy_to_user((void *)rem, &rem_ts, sizeof(rem_ts)) < 0)
        {
            return -EFAULT;
        }
    }

    sched_sleep_ms((uint32_t)ms);
    return 0;
}

static int64_t sys_getuid(void)
{
    return 0;
}

static int64_t sys_getgid(void)
{
    return 0;
}

static int64_t sys_geteuid(void)
{
    return 0;
}

static int64_t sys_getegid(void)
{
    return 0;
}

static int64_t sys_access(int64_t path, int64_t mode)
{
    int8_t kpath[VFS_PATH_MAX];
    struct vfs_stat st;
    int ret;

    (void)mode;
    if (!path)
    {
        return -EINVAL;
    }

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    ret = vfs_stat(kpath, &st);
    if (ret < 0)
    {
        return ret;
    }

    return 0;
}

static int64_t sys_openat(int64_t dirfd, int64_t path, int64_t flags)
{
    /*
     * 当前 VFS 还没有“按目录 fd 做相对路径解析”的能力。
     * 先兼容最常见的 AT_FDCWD（-100）语义，其他 dirfd 返回 ENOTSUP。
     */
    if (dirfd != -100)
    {
        return -ENOTSUP;
    }

    return sys_open(path, flags);
}

static int64_t sys_fstatat(int64_t dirfd, int64_t path, int64_t out, int64_t flags)
{
    int8_t kpath[VFS_PATH_MAX];
    struct vfs_stat st;
    int64_t ret;

    if (flags != 0 && flags != 0x100 /* AT_SYMLINK_NOFOLLOW */)
    {
        return -EINVAL;
    }

    if (dirfd != -100)
    {
        return -ENOTSUP;
    }

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    ret = vfs_stat(kpath, &st);
    if (ret < 0)
    {
        return ret;
    }

    return sys_copy_to_user((void *)out, &st, sizeof(st));
}

static int64_t sys_readlink(int64_t path, int64_t buf, int64_t len)
{
    int8_t kpath[VFS_PATH_MAX];
    int8_t target[VFS_PATH_MAX];
    const int8_t *cwd;
    struct task_struct *task;
    size_t n;
    int64_t ret;

    if (!path || !buf || len <= 0)
    {
        return -EINVAL;
    }

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    memset(target, 0, sizeof(target));
    if (strcmp((const char *)kpath, "/proc/self/exe") == 0)
    {
        size_t base_len;
        size_t comm_len;

        task = task_current();
        if (!task || task->comm[0] == '\0')
        {
            return -ENOENT;
        }

        base_len = 5U;
        comm_len = strlen(task->comm);
        if (base_len + comm_len >= sizeof(target))
        {
            comm_len = sizeof(target) - base_len - 1U;
        }

        memcpy((int8_t *)target, (int8_t *)"/bin/", base_len);
        memcpy((int8_t *)&target[base_len], (int8_t *)task->comm, comm_len);
        target[base_len + comm_len] = '\0';
    }
    else if (strcmp((const char *)kpath, "/proc/self/cwd") == 0)
    {
        size_t cwd_len;

        cwd = task_cwd();
        if (!cwd || cwd[0] == '\0')
        {
            cwd = (const int8_t *)"/";
        }

        cwd_len = strlen((int8_t *)cwd);
        if (cwd_len >= sizeof(target))
        {
            cwd_len = sizeof(target) - 1U;
        }
        memcpy((int8_t *)target, (int8_t *)cwd, cwd_len);
        target[cwd_len] = '\0';
    }
    else
    {
        return -ENOENT;
    }

    n = strlen((int8_t *)target);
    if (n > (size_t)len)
    {
        n = (size_t)len;
    }

    if (!sys_user_mem_valid((const void *)buf, n))
    {
        return -EFAULT;
    }

    memcpy((void *)buf, target, n);
    return (int64_t)n;
}

static int64_t sys_mkdir(int64_t path, int64_t mode)
{
    int8_t kpath[VFS_PATH_MAX];
    int64_t ret;

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    return vfs_mkdir(kpath, (uint16_t)(VFS_S_IFDIR | (mode & 0777)));
}

static int64_t sys_rmdir(int64_t path)
{
    int8_t kpath[VFS_PATH_MAX];
    int64_t ret;

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    return vfs_unlink(kpath, true);
}

static int64_t sys_unlink(int64_t path)
{
    int8_t kpath[VFS_PATH_MAX];
    int64_t ret;

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    return vfs_unlink(kpath, false);
}

static int64_t sys_rename(int64_t old_path, int64_t new_path)
{
    int8_t old_kpath[VFS_PATH_MAX];
    int8_t new_kpath[VFS_PATH_MAX];
    int64_t ret;

    ret = sys_copy_path_from_user(old_kpath, sizeof(old_kpath), (const int8_t *)old_path);
    if (ret < 0)
    {
        return ret;
    }

    ret = sys_copy_path_from_user(new_kpath, sizeof(new_kpath), (const int8_t *)new_path);
    if (ret < 0)
    {
        return ret;
    }

    return vfs_rename(old_kpath, new_kpath);
}

static int64_t sys_truncate(int64_t path, int64_t length)
{
    int8_t kpath[VFS_PATH_MAX];
    int64_t ret;

    if (length < 0)
    {
        return -EINVAL;
    }

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    return vfs_truncate(kpath, (uint64_t)length);
}

static int64_t sys_ftruncate(int64_t fd, int64_t length)
{
    if (length < 0)
    {
        return -EINVAL;
    }

    if (fd < 3)
    {
        return -EINVAL;
    }

    return vfs_ftruncate((int)(fd - 3), (uint64_t)length);
}

static int64_t sys_utimensat(int64_t dirfd, int64_t path, int64_t times, int64_t flags)
{
    int8_t kpath[VFS_PATH_MAX];
    struct vfs_timespec ts[2];
    struct vfs_timespec *atime;
    struct vfs_timespec *mtime;
    int64_t ret;

    if (dirfd != -100)
    {
        return -ENOTSUP;
    }

    if (flags != 0 && flags != 0x100 /* AT_SYMLINK_NOFOLLOW */)
    {
        return -EINVAL;
    }

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    atime = 0;
    mtime = 0;
    if (times)
    {
        if (!sys_user_mem_valid((const void *)times, sizeof(ts)))
        {
            return -EFAULT;
        }

        memcpy((int8_t *)ts, (int8_t *)times, sizeof(ts));
        atime = &ts[0];
        mtime = &ts[1];
    }

    return vfs_utimens(kpath, atime, mtime);
}

static int64_t sys_ioctl(int64_t fd, int64_t request, int64_t argp)
{
    /*
     * 先补一组最常见的终端 ioctl，方便用户态工具链/解释器探测 TTY。
     * 目前只做“可运行”语义，不实现完整 termios 配置。
     */
    if (fd >= 0 && fd <= 2)
    {
        /* TCGETS */
        if ((uint64_t)request == 0x5401ULL)
        {
            uint8_t termios_blob[44];

            if (!argp)
            {
                return -EINVAL;
            }

            memset((int8_t *)termios_blob, 0, sizeof(termios_blob));
            return sys_copy_to_user((void *)argp, termios_blob, sizeof(termios_blob));
        }

        /* TIOCGWINSZ */
        if ((uint64_t)request == 0x5413ULL)
        {
            struct
            {
                uint16_t rows;
                uint16_t cols;
                uint16_t xpixel;
                uint16_t ypixel;
            } ws;

            if (!argp)
            {
                return -EINVAL;
            }

            ws.rows = 40;
            ws.cols = 120;
            ws.xpixel = 0;
            ws.ypixel = 0;
            return sys_copy_to_user((void *)argp, &ws, sizeof(ws));
        }

        /* FIONREAD */
        if ((uint64_t)request == 0x541BULL)
        {
            int32_t pending;

            if (!argp)
            {
                return -EINVAL;
            }

            pending = 0;
            return sys_copy_to_user((void *)argp, &pending, sizeof(pending));
        }
    }

    return -ENOTTY;
}

static int64_t sys_dup(int64_t oldfd)
{
    int fd;

    if (oldfd < 3)
    {
        return -EBADF;
    }

    fd = vfs_dup((int)(oldfd - 3));
    if (fd < 0)
    {
        return fd;
    }

    return fd + 3;
}

static int64_t sys_readv(int64_t fd, int64_t iov, int64_t iovcnt)
{
    const struct stupidos_iovec *vec;
    size_t vec_bytes;
    ssize_t total;
    int64_t i;
    ssize_t n;

    if (!iov || iovcnt < 0)
    {
        return -EINVAL;
    }

    if ((uint64_t)iovcnt > ((uint64_t)-1 / sizeof(*vec)))
    {
        return -EINVAL;
    }

    vec_bytes = (size_t)iovcnt * sizeof(*vec);
    if (!sys_user_mem_valid((const void *)iov, vec_bytes))
    {
        return -EFAULT;
    }

    vec = (const struct stupidos_iovec *)iov;
    total = 0;
    for (i = 0; i < iovcnt; i++)
    {
        if (!vec[i].iov_base || !vec[i].iov_len)
        {
            continue;
        }

        n = sys_read(fd, (int64_t)vec[i].iov_base, (int64_t)vec[i].iov_len);
        if (n < 0)
        {
            return (total > 0) ? total : n;
        }
        total += n;
        if ((uint64_t)n < vec[i].iov_len)
        {
            break;
        }
    }

    return total;
}

static int64_t sys_writev(int64_t fd, int64_t iov, int64_t iovcnt)
{
    const struct stupidos_iovec *vec;
    size_t vec_bytes;
    ssize_t total;
    int64_t i;
    ssize_t n;

    if (!iov || iovcnt < 0)
    {
        return -EINVAL;
    }

    if ((uint64_t)iovcnt > ((uint64_t)-1 / sizeof(*vec)))
    {
        return -EINVAL;
    }

    vec_bytes = (size_t)iovcnt * sizeof(*vec);
    if (!sys_user_mem_valid((const void *)iov, vec_bytes))
    {
        return -EFAULT;
    }

    vec = (const struct stupidos_iovec *)iov;
    total = 0;
    for (i = 0; i < iovcnt; i++)
    {
        if (!vec[i].iov_base || !vec[i].iov_len)
        {
            continue;
        }

        n = sys_write(fd, (int64_t)vec[i].iov_base, (int64_t)vec[i].iov_len);
        if (n < 0)
        {
            return (total > 0) ? total : n;
        }
        total += n;
        if ((uint64_t)n < vec[i].iov_len)
        {
            break;
        }
    }

    return total;
}

static uint64_t sys_random_next(void)
{
    uint64_t now;
    struct task_struct *task;

    task = task_current();
    now = sys_monotonic_usec();
    sys_random_state ^= now + 0x9e3779b97f4a7c15ULL + (sys_random_state << 6) + (sys_random_state >> 2);
    sys_random_state ^= task ? ((uint64_t)(uint32_t)task->pid << 32) ^ (uint64_t)task->switches : 0x12345678ULL;
    sys_random_state ^= (sys_random_state << 13);
    sys_random_state ^= (sys_random_state >> 7);
    sys_random_state ^= (sys_random_state << 17);
    return sys_random_state;
}

static void sys_fill_random(void *buf, size_t len)
{
    uint8_t *out;
    size_t i;
    uint64_t value;
    size_t chunk;

    if (!buf || !len)
    {
        return;
    }

    out = (uint8_t *)buf;
    for (i = 0; i < len; )
    {
        value = sys_random_next();
        chunk = len - i;
        if (chunk > sizeof(value))
        {
            chunk = sizeof(value);
        }

        memcpy((int8_t *)&out[i], (int8_t *)&value, chunk);
        i += chunk;
    }
}

static int64_t sys_exec(int64_t path, int64_t argc, int64_t argv)
{
    /*
     * 用户态和内核当前共享同一地址空间下的可访问映射，
     * 这里直接把用户传入的字符串数组当作普通指针使用即可。
     * exec_program() 会把路径和参数复制进新任务自己的缓冲区，
     * 不会长期依赖这些指针。
     */
    return exec_program((const int8_t *)path, (int)argc, (const int8_t **)argv);
}

static int64_t sys_gettid(void)
{
    struct task_struct *task;

    task = task_current();
    if (!task)
    {
        return -ESRCH;
    }

    return task->pid;
}

static int64_t sys_getppid(void)
{
    struct task_struct *task;

    task = task_current();
    if (!task)
    {
        return -ESRCH;
    }

    return task->ppid;
}

static int64_t sys_exit_group(int64_t code)
{
    (void)code;
    task_exit();
    __builtin_unreachable();
}

static int64_t sys_getrandom(int64_t buf, int64_t len, int64_t flags)
{
    struct task_struct *task;
    uint64_t stack_base;
    bool trace_this_call;

    trace_this_call = sys_getrandom_trace_count < 4U;
    if (!buf || len < 0)
    {
        if (trace_this_call)
        {
            printk("[syscall\tgetrandom]: invalid pid=%d buf=%#lx len=%ld flags=%#lx\n",
                   task_current() ? task_current()->pid : -1,
                   (uint64_t)buf, (long)len, (uint64_t)flags);
            sys_getrandom_trace_count++;
        }
        return -EINVAL;
    }

    task = task_current();
    stack_base = task ? (uint64_t)&task->stack[0] : 0;
    if (!sys_user_mem_valid((const void *)buf, (size_t)len))
    {
        printk("[syscall\tgetrandom]: EFAULT pid=%d buf=%#lx len=%ld exec=[%#lx,%#lx) alias=[%#lx,%#lx) heap=[%#lx,%#lx) stack=[%#lx,%#lx)\n",
               task ? task->pid : -1, (uint64_t)buf, (long)len,
               task ? task->exec_base : 0, task ? task->exec_end : 0,
               task ? task->exec_alias_base : 0, task ? task->exec_alias_end : 0,
               task ? task->heap_base : 0, task ? task->heap_end : 0,
               stack_base, stack_base ? (stack_base + TASK_STACK_SIZE) : 0);
        if (trace_this_call)
        {
            sys_getrandom_trace_count++;
        }
        return -EFAULT;
    }

    (void)flags;
    sys_fill_random((void *)buf, (size_t)len);
    return len;
}

static int64_t sys_set_tid_address(int64_t tidptr)
{
    struct task_struct *task;

    task = task_current();
    if (!task)
    {
        return -ESRCH;
    }

    if (tidptr)
    {
        if (!sys_user_mem_valid((const void *)tidptr, sizeof(int32_t)))
        {
            return -EFAULT;
        }
        *(int32_t *)tidptr = task->pid;
    }

    return task->pid;
}

static int64_t sys_rt_sigaction(int64_t signum, int64_t act, int64_t oldact, int64_t sigsetsize)
{
    (void)act;
    (void)oldact;
    (void)sigsetsize;

    if (signum <= 0 || signum > 64)
    {
        return -EINVAL;
    }

    return 0;
}

static int64_t sys_rt_sigprocmask(int64_t how, int64_t set, int64_t oldset, int64_t sigsetsize)
{
    (void)how;
    (void)set;
    (void)oldset;
    (void)sigsetsize;
    return 0;
}

static int64_t sys_sigaltstack(int64_t ss, int64_t old_ss)
{
    uint8_t zero[32];

    (void)ss;
    if (old_ss)
    {
        memset((int8_t *)zero, 0, sizeof(zero));
        if (sys_copy_to_user((void *)old_ss, zero, sizeof(zero)) < 0)
        {
            return -EFAULT;
        }
    }
    return 0;
}

static int64_t sys_futex(int64_t uaddr, int64_t op, int64_t val, int64_t timeout, int64_t uaddr2, int64_t val3)
{
    uint64_t addr;
    uint32_t i;
    uint32_t wake_count;
    struct task_struct *curr;
    struct task_struct *to_wake[16];
    uint32_t to_wake_count;
    uint32_t futex_op;
    uint32_t *u32;

    (void)timeout;
    (void)uaddr2;
    (void)val3;

    if (!uaddr)
    {
        return -EINVAL;
    }

    addr = (uint64_t)uaddr;
    if (!sys_user_mem_valid((const void *)addr, sizeof(uint32_t)))
    {
        return -EFAULT;
    }

    futex_op = (uint32_t)op & ~STUPIDOS_FUTEX_PRIVATE_FLAG;
    u32 = (uint32_t *)addr;
    curr = task_current();
    if (!curr)
    {
        return -ESRCH;
    }

    switch (futex_op)
    {
    case STUPIDOS_FUTEX_WAIT:
        if ((uint32_t)val != *u32)
        {
            return -EAGAIN;
        }

        disable_irq();
        spin_lock(&sys_futex_lock);
        for (i = 0; i < (uint32_t)(sizeof(sys_futex_waiters) / sizeof(sys_futex_waiters[0])); i++)
        {
            if (!sys_futex_waiters[i].used)
            {
                sys_futex_waiters[i].used = true;
                sys_futex_waiters[i].addr = addr;
                sys_futex_waiters[i].task = curr;
                curr->state = TASK_SLEEPING;
                curr->sleep_until = (uint64_t)-1;
                break;
            }
        }
        spin_unlock(&sys_futex_lock);
        if (i == (uint32_t)(sizeof(sys_futex_waiters) / sizeof(sys_futex_waiters[0])))
        {
            enable_irq();
            return -ENOMEM;
        }
        sched_sleep_until((uint64_t)-1);
        enable_irq();
        return 0;

    case STUPIDOS_FUTEX_WAKE:
        to_wake_count = 0;
        wake_count = (val < 0) ? 0 : (uint32_t)val;
        if (wake_count > (uint32_t)(sizeof(to_wake) / sizeof(to_wake[0])))
        {
            wake_count = (uint32_t)(sizeof(to_wake) / sizeof(to_wake[0]));
        }
        if (!wake_count)
        {
            return 0;
        }

        disable_irq();
        spin_lock(&sys_futex_lock);
        for (i = 0; i < (uint32_t)(sizeof(sys_futex_waiters) / sizeof(sys_futex_waiters[0])); i++)
        {
            if (!sys_futex_waiters[i].used || sys_futex_waiters[i].addr != addr)
            {
                continue;
            }

            to_wake[to_wake_count++] = sys_futex_waiters[i].task;
            sys_futex_waiters[i].used = false;
            sys_futex_waiters[i].addr = 0;
            sys_futex_waiters[i].task = 0;
            if (to_wake_count >= wake_count)
            {
                break;
            }
        }
        spin_unlock(&sys_futex_lock);

        for (i = 0; i < to_wake_count; i++)
        {
            task_wake(to_wake[i]);
        }
        enable_irq();
        return (int64_t)to_wake_count;

    default:
        return -ENOSYS;
    }
}

static int64_t sys_pread64(int64_t fd, int64_t buf, int64_t len, int64_t off)
{
    int64_t kfd;

    if (len < 0 || off < 0)
    {
        return -EINVAL;
    }

    if (fd < 3)
    {
        return -EBADF;
    }

    if (!sys_user_mem_valid((const void *)buf, (size_t)len))
    {
        return -EFAULT;
    }

    kfd = fd - 3;
    return vfs_pread((int)kfd, (void *)buf, (size_t)len, (uint64_t)off);
}

static int64_t sys_pwrite64(int64_t fd, int64_t buf, int64_t len, int64_t off)
{
    int64_t kfd;

    if (len < 0 || off < 0)
    {
        return -EINVAL;
    }

    if (fd < 3)
    {
        return -EBADF;
    }

    if (!sys_user_mem_valid((const void *)buf, (size_t)len))
    {
        return -EFAULT;
    }

    kfd = fd - 3;
    return vfs_pwrite((int)kfd, (const void *)buf, (size_t)len, (uint64_t)off);
}

static int64_t sys_fcntl(int64_t fd, int64_t cmd, int64_t arg)
{
    if (fd < 3)
    {
        return -EBADF;
    }

    return vfs_fcntl((int)(fd - 3), (int)cmd, (uint64_t)arg);
}

static int64_t sys_sched_getaffinity(int64_t pid, int64_t cpusetsize, int64_t mask)
{
    uint8_t *bytes;
    size_t i;
    size_t need;
    uint32_t cpu_count;

    (void)pid;

    if (!mask || cpusetsize <= 0)
    {
        return -EINVAL;
    }

    cpu_count = smp_cpu_count();
    need = (cpu_count + 7U) / 8U;
    if ((size_t)cpusetsize < need)
    {
        return -EINVAL;
    }

    bytes = (uint8_t *)mask;
    if (!sys_user_mem_valid(bytes, (size_t)cpusetsize))
    {
        return -EFAULT;
    }

    memset((int8_t *)bytes, 0, (size_t)cpusetsize);
    for (i = 0; i < cpu_count; i++)
    {
        bytes[i / 8U] |= (uint8_t)(1U << (i % 8U));
    }
    return 0;
}

static int64_t sys_sysinfo(int64_t info)
{
    struct stupidos_sysinfo si;

    if (!info)
    {
        return -EINVAL;
    }

    memset((int8_t *)&si, 0, sizeof(si));
    si.uptime = (int64_t)(jiffies / STUPIDOS_TIMER_HZ);
    si.totalram = (uint64_t)page_alloc_total_pages() * PAGE_SIZE;
    si.freeram = (uint64_t)page_alloc_free_pages() * PAGE_SIZE;
    si.mem_unit = 1;
    si.procs = 1;
    return sys_copy_to_user((void *)info, &si, sizeof(si));
}

static int64_t sys_prlimit64(int64_t pid, int64_t resource, int64_t new_limit, int64_t old_limit)
{
    struct stupidos_rlimit *old_rlim;
    struct stupidos_rlimit *new_rlim;

    (void)pid;
    (void)resource;

    if (new_limit)
    {
        new_rlim = (struct stupidos_rlimit *)new_limit;
        (void)new_rlim;
    }

    if (old_limit)
    {
        struct stupidos_rlimit tmp;

        old_rlim = (struct stupidos_rlimit *)old_limit;
        tmp.rlim_cur = 0xffffffffffffffffULL;
        tmp.rlim_max = 0xffffffffffffffffULL;
        if (sys_copy_to_user(old_rlim, &tmp, sizeof(tmp)) < 0)
        {
            return -EFAULT;
        }
    }

    return 0;
}

static int64_t sys_getdents64(int64_t fd, int64_t dirp, int64_t count)
{
    if (fd < 3)
    {
        return -EBADF;
    }

    if (!dirp || count <= 0)
    {
        return -EINVAL;
    }

    if (!sys_user_mem_valid((const void *)dirp, (size_t)count))
    {
        return -EFAULT;
    }

    return vfs_getdents64((int)(fd - 3), (void *)dirp, (size_t)count);
}

void syscall_init(void)
{
    printk("[syscall\tinit]: syscall ABI ready\n");
}

int64_t syscall_dispatch(pt_regs_t *regs)
{
    uint64_t nr;

    nr = regs->s_reg[8];
    switch (nr)
    {
    case SYS_READ:
        return sys_read((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_WRITE:
        return sys_write((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_OPEN:
        return sys_open((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_CLOSE:
        return sys_close((int64_t)regs->s_reg[0]);
    case SYS_LSEEK:
        return sys_lseek((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_YIELD:
        return sys_yield();
    case SYS_GETPID:
        return sys_getpid();
    case SYS_TIME:
        return sys_time();
    case SYS_EXIT:
        return sys_exit((int64_t)regs->s_reg[0]);
    case SYS_READDIR:
        return sys_readdir((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_NETTEST:
        return sys_nettest();
    case SYS_EXEC:
        return sys_exec((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_NETPING:
        return sys_netping((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_WAITPID:
        return sys_waitpid((int64_t)regs->s_reg[0]);
    case SYS_SLEEP:
        return sys_sleep((int64_t)regs->s_reg[0]);
    case SYS_NETCFG:
        return sys_netcfg((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_CHDIR:
        return sys_chdir((int64_t)regs->s_reg[0]);
    case SYS_GETCWD:
        return sys_getcwd((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_STAT:
        return sys_stat((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_FSTAT:
        return sys_fstat((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_UNAME:
        return sys_uname((int64_t)regs->s_reg[0]);
    case SYS_GETTIMEOFDAY:
        return sys_gettimeofday((int64_t)regs->s_reg[0]);
    case SYS_ISATTY:
        return sys_isatty((int64_t)regs->s_reg[0]);
    case SYS_DUP2:
        return sys_dup2((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_BRK:
        return sys_brk((int64_t)regs->s_reg[0]);
    case SYS_MMAP:
        return sys_mmap((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2],
                        (int64_t)regs->s_reg[3], (int64_t)regs->s_reg[4], (int64_t)regs->s_reg[5]);
    case SYS_MUNMAP:
        return sys_munmap((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_MPROTECT:
        return sys_mprotect((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_CLOCK_GETTIME:
        return sys_clock_gettime((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_NANOSLEEP:
        return sys_nanosleep((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_GETUID:
        return sys_getuid();
    case SYS_GETGID:
        return sys_getgid();
    case SYS_GETEUID:
        return sys_geteuid();
    case SYS_GETEGID:
        return sys_getegid();
    case SYS_ACCESS:
        return sys_access((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_OPENAT:
        return sys_openat((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_FSTATAT:
        return sys_fstatat((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2], (int64_t)regs->s_reg[3]);
    case SYS_READLINK:
        return sys_readlink((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_IOCTL:
        return sys_ioctl((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_DUP:
        return sys_dup((int64_t)regs->s_reg[0]);
    case SYS_READV:
        return sys_readv((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_WRITEV:
        return sys_writev((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_GETTID:
        return sys_gettid();
    case SYS_GETPPID:
        return sys_getppid();
    case SYS_EXIT_GROUP:
        return sys_exit_group((int64_t)regs->s_reg[0]);
    case SYS_GETRANDOM:
        return sys_getrandom((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_SET_TID_ADDRESS:
        return sys_set_tid_address((int64_t)regs->s_reg[0]);
    case SYS_RT_SIGACTION:
        return sys_rt_sigaction((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2], (int64_t)regs->s_reg[3]);
    case SYS_RT_SIGPROCMASK:
        return sys_rt_sigprocmask((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2], (int64_t)regs->s_reg[3]);
    case SYS_SIGALTSTACK:
        return sys_sigaltstack((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_FUTEX:
        return sys_futex((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2],
                         (int64_t)regs->s_reg[3], (int64_t)regs->s_reg[4], (int64_t)regs->s_reg[5]);
    case SYS_PREAD64:
        return sys_pread64((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2], (int64_t)regs->s_reg[3]);
    case SYS_PWRITE64:
        return sys_pwrite64((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2], (int64_t)regs->s_reg[3]);
    case SYS_FCNTL:
        return sys_fcntl((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_SCHED_GETAFFINITY:
        return sys_sched_getaffinity((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_SYSINFO:
        return sys_sysinfo((int64_t)regs->s_reg[0]);
    case SYS_PRLIMIT64:
        return sys_prlimit64((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2], (int64_t)regs->s_reg[3]);
    case SYS_GETDENTS64:
        return sys_getdents64((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1], (int64_t)regs->s_reg[2]);
    case SYS_MKDIR:
        return sys_mkdir((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_RMDIR:
        return sys_rmdir((int64_t)regs->s_reg[0]);
    case SYS_UNLINK:
        return sys_unlink((int64_t)regs->s_reg[0]);
    case SYS_RENAME:
        return sys_rename((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_TRUNCATE:
        return sys_truncate((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_FTRUNCATE:
        return sys_ftruncate((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_UTIMENSAT:
        return sys_utimensat((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1],
                             (int64_t)regs->s_reg[2], (int64_t)regs->s_reg[3]);
    default:
        return -ENOSYS;
    }
}
