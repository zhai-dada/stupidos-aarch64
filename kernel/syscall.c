#include "syscall.h"

#include "driver/uart.h"
#include "errno.h"
#include "fs/vfs.h"
#include "exec.h"
#include "printk.h"
#include "lib/libasm.h"
#include "lib/libirq.h"
#include "sched.h"
#include "tty.h"
#include "timer.h"
#include "net/net.h"

static int64_t sys_read(int64_t fd, int64_t buf, int64_t len)
{
    uint8_t *bytes;
    int64_t i;
    int32_t ch;

    if (fd == 0)
    {
        if (len <= 0)
        {
            return 0;
        }

        bytes = (uint8_t *)buf;
        /*
         * stdin 这里改成“内核内阻塞等待”，而不是立即返回 -EAGAIN。
         *
         * 这样 shell 不需要在用户态反复 yield，能明显减少调度抖动；
         * 同时输入字符到来后会直接在这次 read 里返回，更接近 Linux 的交互体验。
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
                bytes[i++] = (uint8_t)ch;
                if (ch == '\n')
                {
                    return i;
                }
                continue;
            }

            /*
             * 如果当前已经读到一部分输入，就别继续死等下一批字符了。
             * 这样 shell 可以在每个按键到来时尽快回显，而不是非要攒到整行。
             */
            if (i > 0)
            {
                return i;
            }

            /*
             * 交互式 stdin 等待阶段改用事件等待。
             * 比 wfi 更适合“输入刚到、马上唤醒”的场景，
             * 也避免 IRQ 刚进来又在睡眠入口处错过的竞态。
             */
            enable_irq();
            wfe();
            disable_irq();
        }

        return i;
    }

    if (fd >= 3)
    {
        return vfs_read((int)(fd - 3), (void *)buf, (size_t)len);
    }

    return -ENOSYS;
}

static int64_t sys_write(int64_t fd, int64_t buf, int64_t len)
{
    const uint8_t *bytes;
    int64_t i;

    if (fd == 1 || fd == 2)
    {
        bytes = (const uint8_t *)buf;
        for (i = 0; i < len; i++)
        {
            tty_putc(bytes[i]);
        }
        return len;
    }

    if (fd >= 3)
    {
        return vfs_write((int)(fd - 3), (const void *)buf, (size_t)len);
    }

    return -EBADF;
}

static int64_t sys_open(int64_t path, int64_t flags)
{
    int fd;

    fd = vfs_open((const int8_t *)path, (int)flags);
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
    task_exit();
}

static int64_t sys_readdir(int64_t path, int64_t index, int64_t out)
{
    return vfs_readdir((const int8_t *)path, (uint32_t)index, (struct vfs_dirent *)out);
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
    struct task_struct *task;

    if (pid < 0)
    {
        return -EINVAL;
    }

    task = task_by_pid((int32_t)pid);
    if (!task)
    {
        return -ESRCH;
    }

    if (task == task_current())
    {
        return -EINVAL;
    }

    /*
     * 最小 waitpid：
     * - shell 在前台执行 ELF 时，阻塞等子任务真正退出
     * - 不引入复杂的子进程表和僵尸回收，先把交互顺序修正过来
     */
    while (task->state != TASK_DEAD)
    {
        enable_irq();
        /*
         * 这里不能再用 wfi。
         * waitpid 等的是“子任务状态变化”这种软件事件，不是硬中断；
         * 用 wfe + task_exit() 里的 sev()，子进程一退出父进程就能立刻醒。
         */
        wfe();
        disable_irq();
    }

    return 0;
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
    default:
        return -ENOSYS;
    }
}
