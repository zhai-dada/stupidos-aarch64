#include "syscall.h"

#include "driver/uart.h"
#include "errno.h"
#include "fs/vfs.h"
#include "printk.h"
#include "sched.h"
#include "timer.h"

static int64_t sys_read(int64_t fd, int64_t buf, int64_t len)
{
    return vfs_read((int)fd, (void *)buf, (size_t)len);
}

static int64_t sys_write(int64_t fd, int64_t buf, int64_t len)
{
    return vfs_write((int)fd, (const void *)buf, (size_t)len);
}

static int64_t sys_open(int64_t path, int64_t flags)
{
    return vfs_open((const int8_t *)path, (int)flags);
}

static int64_t sys_close(int64_t fd)
{
    return vfs_close((int)fd);
}

static int64_t sys_lseek(int64_t fd, int64_t offset, int64_t whence)
{
    return vfs_lseek((int)fd, offset, (int)whence);
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
    default:
        return -ENOSYS;
    }
}
