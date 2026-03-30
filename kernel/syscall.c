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
#include "ui.h"

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

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

static size_t sys_u64_to_dec(uint64_t value, int8_t *buf, size_t buf_len)
{
    size_t pos;

    if (!buf || buf_len == 0)
    {
        return 0;
    }

    pos = buf_len;
    buf[--pos] = '\0';
    if (value == 0)
    {
        if (pos == 0)
        {
            return 0;
        }
        buf[--pos] = '0';
        return buf_len - pos - 1U;
    }

    while (value > 0 && pos > 0)
    {
        buf[--pos] = (int8_t)('0' + (value % 10ULL));
        value /= 10ULL;
    }

    return buf_len - pos - 1U;
}

static int32_t sys_stdio_target_fd(int64_t fd);

static int sys_build_path_from_dirfd(int64_t dirfd, const int8_t *path, int8_t *out, size_t out_len)
{
    int32_t target_fd;
    int8_t base[VFS_PATH_MAX];
    size_t base_len;
    size_t path_len;

    if (!path || !out || out_len < 2)
    {
        return -EINVAL;
    }

    if (path[0] == '/')
    {
        return vfs_canonicalize_path(path, out, out_len);
    }

    if (dirfd == AT_FDCWD)
    {
        const int8_t *cwd;
        size_t cwd_len;

        cwd = task_cwd();
        if (!cwd || cwd[0] == '\0')
        {
            cwd = (const int8_t *)"/";
        }

        cwd_len = strlen((int8_t *)cwd);
        if (cwd_len + 1U > sizeof(base))
        {
            return -ENAMETOOLONG;
        }
        memcpy(base, cwd, cwd_len + 1U);
    }
    else
    {
        struct vfs_stat st;

        target_fd = sys_stdio_target_fd(dirfd);
        if (target_fd >= 0)
        {
            dirfd = target_fd;
        }

        if (dirfd < 3)
        {
            return -EBADF;
        }

        if (vfs_fstat((int)(dirfd - 3), &st) < 0)
        {
            return -EBADF;
        }

        if ((st.mode & VFS_S_IFMT) != VFS_S_IFDIR)
        {
            return -ENOTDIR;
        }

        if (vfs_fd_path((int)(dirfd - 3), base, sizeof(base)) < 0)
        {
            return -EBADF;
        }
    }

    base_len = strlen((int8_t *)base);
    path_len = strlen((int8_t *)path);
    if (base_len + 1U + path_len + 1U > out_len)
    {
        return -ENAMETOOLONG;
    }

    memset(out, 0, out_len);
    memcpy(out, base, base_len);
    if (base_len == 0 || out[base_len - 1] != '/')
    {
        out[base_len++] = '/';
    }
    memcpy(out + base_len, path, path_len + 1U);
    return vfs_canonicalize_path(out, out, out_len);
}

static int sys_fd_path_link(int64_t fd, int8_t *out, size_t len)
{
    int32_t target_fd;

    if (!out || len == 0)
    {
        return -EINVAL;
    }

    target_fd = sys_stdio_target_fd(fd);
    if (target_fd >= 0)
    {
        fd = target_fd;
    }

    if (fd >= 3)
    {
        int ret = vfs_fd_path((int)(fd - 3), out, len);
        if (ret == 0)
        {
            return 0;
        }
    }

    if (fd >= 0 && fd <= 2)
    {
        /*
         * 标准流默认映射到控制终端，给 /proc/self/fd/0 这类探测一个稳定结果。
         */
        if (len < sizeof("/dev/tty"))
        {
            return -ENAMETOOLONG;
        }
        memcpy(out, (const int8_t *)"/dev/tty", sizeof("/dev/tty"));
        return 0;
    }

    return -ENOENT;
}

struct futex_waiter
{
    bool used;
    uint64_t addr;
    struct task_struct *task;
};

static spinlock_t sys_futex_lock = SPINLOCK_INIT;
static struct futex_waiter sys_futex_waiters[16];

#define SYS_SOCKET_FD_BASE 64
#define SYS_SOCKET_FD_MAX  8
#define SYS_PIPE_FD_BASE   80
#define SYS_PIPE_FD_MAX    16

#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif
#ifndef SO_ERROR
#define SO_ERROR 4
#endif

#ifndef KERNEL_O_RDONLY
#define KERNEL_O_RDONLY    0
#endif
#ifndef KERNEL_O_WRONLY
#define KERNEL_O_WRONLY    1
#endif
#ifndef KERNEL_O_NONBLOCK
#define KERNEL_O_NONBLOCK  0x800
#endif

struct sys_socket_slot
{
    bool used;
    int flags;
    int domain;
    int type;
    int protocol;
    struct net_socket sock;
};

struct sys_sockaddr_in
{
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
} __attribute__((packed));

static struct sys_socket_slot sys_socket_slots[SYS_SOCKET_FD_MAX];

struct sys_pipe_slot
{
    bool used;
    int read_fd;
    int write_fd;
    bool read_open;
    bool write_open;
    bool read_nonblock;
    bool write_nonblock;
    uint32_t read_refs;
    uint32_t write_refs;
    size_t head;
    size_t tail;
    size_t used_bytes;
    uint8_t buf[4096U];
};

static spinlock_t sys_pipe_lock = SPINLOCK_INIT;
static struct sys_pipe_slot sys_pipe_slots[SYS_PIPE_FD_MAX];

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

static bool sys_socket_fd_in_range(int64_t fd)
{
    return fd >= SYS_SOCKET_FD_BASE && fd < (SYS_SOCKET_FD_BASE + SYS_SOCKET_FD_MAX);
}

static struct sys_socket_slot *sys_socket_slot_from_fd(int64_t fd)
{
    struct sys_socket_slot *slot;

    if (!sys_socket_fd_in_range(fd))
    {
        return 0;
    }

    slot = &sys_socket_slots[fd - SYS_SOCKET_FD_BASE];
    if (!slot->used)
    {
        return 0;
    }
    return slot;
}

static int sys_socket_fd_alloc(struct sys_socket_slot **out_slot)
{
    uint32_t i;

    if (!out_slot)
    {
        return -EINVAL;
    }

    for (i = 0; i < SYS_SOCKET_FD_MAX; i++)
    {
        if (!sys_socket_slots[i].used)
        {
            memset((int8_t *)&sys_socket_slots[i], 0, sizeof(sys_socket_slots[i]));
            sys_socket_slots[i].used = true;
            *out_slot = &sys_socket_slots[i];
            return (int)(SYS_SOCKET_FD_BASE + i);
        }
    }

    return -EMFILE;
}

static void sys_socket_slot_release(struct sys_socket_slot *slot)
{
    if (!slot || !slot->used)
    {
        return;
    }

    net_socket_close(&slot->sock);
    memset((int8_t *)slot, 0, sizeof(*slot));
}

static bool sys_pipe_fd_in_range(int64_t fd)
{
    return fd >= SYS_PIPE_FD_BASE && fd < (SYS_PIPE_FD_BASE + (SYS_PIPE_FD_MAX * 2));
}

static struct sys_pipe_slot *sys_pipe_slot_from_fd(int64_t fd, bool *is_write_end)
{
    int64_t idx;
    struct sys_pipe_slot *slot;

    if (!sys_pipe_fd_in_range(fd))
    {
        return 0;
    }

    idx = (fd - SYS_PIPE_FD_BASE) / 2;
    if (idx < 0 || idx >= SYS_PIPE_FD_MAX)
    {
        return 0;
    }

    slot = &sys_pipe_slots[idx];
    if (!slot->used)
    {
        return 0;
    }

    if (fd == slot->read_fd)
    {
        if (is_write_end)
        {
            *is_write_end = false;
        }
        return slot;
    }

    if (fd == slot->write_fd)
    {
        if (is_write_end)
        {
            *is_write_end = true;
        }
        return slot;
    }

    return 0;
}

static void sys_pipe_slot_get(struct sys_pipe_slot *slot, bool is_write_end)
{
    uint64_t daif;

    if (!slot || !slot->used)
    {
        return;
    }

    daif = read_daif();
    disable_irq();
    spin_lock(&sys_pipe_lock);
    if (is_write_end)
    {
        slot->write_refs++;
    }
    else
    {
        slot->read_refs++;
    }
    spin_unlock(&sys_pipe_lock);
    write_daif(daif);
}

static void sys_pipe_slot_put(struct sys_pipe_slot *slot, bool is_write_end)
{
    uint64_t daif;

    if (!slot || !slot->used)
    {
        return;
    }

    daif = read_daif();
    disable_irq();
    spin_lock(&sys_pipe_lock);
    if (is_write_end)
    {
        if (slot->write_refs > 0)
        {
            slot->write_refs--;
        }
        if (slot->write_refs == 0)
        {
            slot->write_open = false;
        }
    }
    else
    {
        if (slot->read_refs > 0)
        {
            slot->read_refs--;
        }
        if (slot->read_refs == 0)
        {
            slot->read_open = false;
        }
    }

    if (!slot->read_open && !slot->write_open &&
        slot->read_refs == 0 && slot->write_refs == 0)
    {
        memset((int8_t *)slot, 0, sizeof(*slot));
    }
    spin_unlock(&sys_pipe_lock);
    write_daif(daif);
}

static int32_t sys_stdio_target_fd(int64_t fd)
{
    struct task_struct *task;

    if (fd < 0 || fd > 2)
    {
        return (int32_t)fd;
    }

    task = task_current();
    if (!task)
    {
        return -1;
    }

    return task->stdio_fd[fd];
}

static int64_t sys_close(int64_t fd);
static int sys_stdio_set_fd(int64_t fd, int32_t target_fd);

static void sys_stdio_reset_fd(int64_t fd)
{
    struct task_struct *task;

    if (fd < 0 || fd > 2)
    {
        return;
    }

    task = task_current();
    if (!task)
    {
        return;
    }

    task->stdio_fd[fd] = -1;
}

static int sys_stdio_set_fd(int64_t fd, int32_t target_fd)
{
    struct task_struct *task;
    int32_t old_target;

    if (fd < 0 || fd > 2)
    {
        return -EINVAL;
    }

    task = task_current();
    if (!task)
    {
        return -ESRCH;
    }

    old_target = task->stdio_fd[fd];
    if (old_target == target_fd)
    {
        return 0;
    }

    if (old_target >= 3)
    {
        (void)sys_close(old_target);
    }

    task->stdio_fd[fd] = target_fd;
    return 0;
}

static bool sys_fd_is_stdio_redirected(int64_t fd)
{
    return fd >= 0 && fd <= 2 && sys_stdio_target_fd(fd) >= 0;
}

static int sys_pipe_fd_alloc(struct sys_pipe_slot **out_slot)
{
    uint32_t i;

    if (!out_slot)
    {
        return -EINVAL;
    }

    for (i = 0; i < SYS_PIPE_FD_MAX; i++)
    {
        if (!sys_pipe_slots[i].used)
        {
            memset((int8_t *)&sys_pipe_slots[i], 0, sizeof(sys_pipe_slots[i]));
            sys_pipe_slots[i].used = true;
            sys_pipe_slots[i].read_fd = SYS_PIPE_FD_BASE + (int)(i * 2U);
            sys_pipe_slots[i].write_fd = sys_pipe_slots[i].read_fd + 1;
            sys_pipe_slots[i].read_open = true;
            sys_pipe_slots[i].write_open = true;
            sys_pipe_slots[i].read_refs = 1;
            sys_pipe_slots[i].write_refs = 1;
            *out_slot = &sys_pipe_slots[i];
            return sys_pipe_slots[i].read_fd;
        }
    }

    return -EMFILE;
}

static int64_t sys_read(int64_t fd, int64_t buf, int64_t len)
{
    uint8_t *bytes;
    int64_t i;
    int32_t ch;
    uint64_t wait_daif;
    bool canonical;
    struct sys_socket_slot *sock;

    if (fd >= 0 && fd <= 2)
    {
        int32_t target_fd;

        if (len <= 0)
        {
            return 0;
        }

        target_fd = sys_stdio_target_fd(fd);
        if (target_fd >= 0)
        {
            return sys_read(target_fd, buf, len);
        }

        bytes = (uint8_t *)buf;
        canonical = tty_is_canonical_mode() != 0;
        /*
         * stdin 支持最小 termios 语义：
         * - 规范模式（ICANON）：按行返回，适合 shell；
         * - 非规范模式：读到首字节就尽快返回，适合 vi/vim raw 输入。
         */
        for (i = 0; i < len; )
        {
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
                if (canonical && (ch == '\n' || ch == '\r'))
                {
                    return i;
                }
                if (!canonical)
                {
                    /*
                     * raw 模式下优先保证低延迟：拿到首字节后再顺手收一波现有队列，
                     * 然后立刻返回给用户态（vi/vim 会自行驱动后续读取）。
                     */
                    while (i < len)
                    {
                        ch = tty_try_getc();
                        if (ch < 0)
                        {
                            break;
                        }
                        if (!sys_user_mem_valid((const void *)(bytes + i), 1))
                        {
                            return i;
                        }
                        bytes[i++] = (uint8_t)ch;
                    }
                    return i;
                }
                continue;
            }

            wait_daif = read_daif();
            enable_irq();
            wfe();
            write_daif(wait_daif);
        }

        return i;
    }

    sock = sys_socket_slot_from_fd(fd);
    if (sock)
    {
        if (!sys_user_mem_valid((const void *)buf, (size_t)len))
        {
            return -EFAULT;
        }

        return net_socket_read(&sock->sock, (void *)buf, (size_t)len, 0U);
    }

    {
        bool is_write_end;
        struct sys_pipe_slot *pipe;
        uint8_t *dst;
        uint64_t copied;
        uint64_t first;

        pipe = sys_pipe_slot_from_fd(fd, &is_write_end);
        if (pipe)
        {
            if (is_write_end)
            {
                return -EBADF;
            }

            if (!sys_user_mem_valid((const void *)buf, (size_t)len))
            {
                return -EFAULT;
            }

            if (len <= 0)
            {
                return 0;
            }

            dst = (uint8_t *)buf;
            for (;;)
            {
                uint64_t daif;

                daif = read_daif();
                disable_irq();
                spin_lock(&sys_pipe_lock);
                if (pipe->used_bytes > 0)
                {
                    copied = (uint64_t)len;
                    if (copied > pipe->used_bytes)
                    {
                        copied = pipe->used_bytes;
                    }
                    first = copied;
                    if (pipe->head + first > sizeof(pipe->buf))
                    {
                        first = sizeof(pipe->buf) - pipe->head;
                    }
                    memcpy((int8_t *)dst, (int8_t *)&pipe->buf[pipe->head], (size_t)first);
                    if (copied > first)
                    {
                        memcpy((int8_t *)dst + first, (int8_t *)&pipe->buf[0], (size_t)(copied - first));
                    }
                    pipe->head = (pipe->head + copied) % sizeof(pipe->buf);
                    pipe->used_bytes -= (size_t)copied;
                    spin_unlock(&sys_pipe_lock);
                    write_daif(daif);
                    return (int64_t)copied;
                }

                if (!pipe->write_open)
                {
                    spin_unlock(&sys_pipe_lock);
                    write_daif(daif);
                    return 0;
                }

                if (pipe->read_nonblock)
                {
                    spin_unlock(&sys_pipe_lock);
                    write_daif(daif);
                    return -EAGAIN;
                }

                spin_unlock(&sys_pipe_lock);
                write_daif(daif);
                sched_yield();
            }
        }
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
    struct sys_socket_slot *sock;

    if (len < 0)
    {
        return -EINVAL;
    }

    if (fd >= 0 && fd <= 2)
    {
        int32_t target_fd;

        if (!sys_user_mem_valid((const void *)buf, (size_t)len))
        {
            return -EFAULT;
        }

        target_fd = sys_stdio_target_fd(fd);
        if (target_fd >= 0)
        {
            return sys_write(target_fd, buf, len);
        }

        bytes = (const uint8_t *)buf;
        tty_write_bytes(bytes, (size_t)len);
        return len;
    }

    sock = sys_socket_slot_from_fd(fd);
    if (sock)
    {
        if (!sys_user_mem_valid((const void *)buf, (size_t)len))
        {
            return -EFAULT;
        }

        return net_socket_write(&sock->sock, (const void *)buf, (size_t)len, 0U);
    }

    {
        bool is_write_end;
        struct sys_pipe_slot *pipe;
        const uint8_t *src;
        uint64_t written;
        uint64_t first;

        pipe = sys_pipe_slot_from_fd(fd, &is_write_end);
        if (pipe)
        {
            if (!is_write_end)
            {
                return -EBADF;
            }

            if (!sys_user_mem_valid((const void *)buf, (size_t)len))
            {
                return -EFAULT;
            }

            if (len <= 0)
            {
                return 0;
            }

            if (!pipe->read_open)
            {
                return -EPIPE;
            }

            src = (const uint8_t *)buf;
            written = 0;
            for (;;)
            {
                uint64_t daif;
                uint64_t space;
                uint64_t chunk;
                uint64_t second;

                daif = read_daif();
                disable_irq();
                spin_lock(&sys_pipe_lock);
                if (!pipe->read_open)
                {
                    spin_unlock(&sys_pipe_lock);
                    write_daif(daif);
                    return (written > 0) ? (int64_t)written : -EPIPE;
                }

                if (pipe->used_bytes < sizeof(pipe->buf))
                {
                    space = (uint64_t)sizeof(pipe->buf) - (uint64_t)pipe->used_bytes;
                    chunk = (uint64_t)len - written;
                    if (chunk > space)
                    {
                        chunk = space;
                    }
                    first = chunk;
                    if (pipe->tail + first > sizeof(pipe->buf))
                    {
                        first = sizeof(pipe->buf) - pipe->tail;
                    }
                    memcpy((int8_t *)&pipe->buf[pipe->tail], (int8_t *)src + written, (size_t)first);
                    if (chunk > first)
                    {
                        second = chunk - first;
                        memcpy((int8_t *)&pipe->buf[0], (int8_t *)src + written + first, (size_t)second);
                        pipe->tail = (pipe->tail + first + second) % sizeof(pipe->buf);
                        pipe->used_bytes += (size_t)(first + second);
                        written += first + second;
                    }
                    else
                    {
                        pipe->tail = (pipe->tail + first) % sizeof(pipe->buf);
                        pipe->used_bytes += (size_t)first;
                        written += first;
                    }
                    spin_unlock(&sys_pipe_lock);
                    write_daif(daif);
                    if (written >= (uint64_t)len)
                    {
                        return (int64_t)written;
                    }
                    continue;
                }

                if (pipe->write_nonblock)
                {
                    spin_unlock(&sys_pipe_lock);
                    write_daif(daif);
                    return (written > 0) ? (int64_t)written : -EAGAIN;
                }

                spin_unlock(&sys_pipe_lock);
                write_daif(daif);
                sched_yield();
            }
        }
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
    bool is_write_end;
    struct sys_pipe_slot *pipe;
    struct sys_socket_slot *sock;

    if (fd >= 0 && fd <= 2)
    {
        int32_t target_fd;

        target_fd = sys_stdio_target_fd(fd);
        if (target_fd >= 0)
        {
            sys_stdio_reset_fd(fd);
            if (target_fd >= 3)
            {
                return sys_close(target_fd);
            }
            return 0;
        }
        return 0;
    }

    sock = sys_socket_slot_from_fd(fd);
    if (sock)
    {
        sys_socket_slot_release(sock);
        return 0;
    }

    pipe = sys_pipe_slot_from_fd(fd, &is_write_end);
    if (pipe)
    {
        sys_pipe_slot_put(pipe, is_write_end);
        return 0;
    }

    return vfs_close((int)(fd - 3));
}

static int64_t sys_lseek(int64_t fd, int64_t offset, int64_t whence)
{
    int32_t target_fd;

    target_fd = sys_stdio_target_fd(fd);
    if (target_fd >= 0)
    {
        fd = target_fd;
    }

    if (sys_socket_slot_from_fd(fd))
    {
        return -ESPIPE;
    }

    if (sys_pipe_slot_from_fd(fd, 0))
    {
        return -ESPIPE;
    }

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
    task_set_exit_code((int32_t)code);
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

static int64_t sys_waitpid_common(int64_t pid, int32_t *out_status)
{
    struct task_struct *self;
    int32_t child_pid;
    int32_t status;
    int ret;

    self = task_current();
    if (!self)
    {
        return -ESRCH;
    }

    if (pid < -1 || pid == 0)
    {
        return -EINVAL;
    }

    for (;;)
    {
        child_pid = 0;
        status = 0;
        ret = sched_wait_child(self->pid,
                               (pid == -1) ? -1 : (int32_t)pid,
                               &child_pid,
                               &status);
        if (ret == 0)
        {
            if (out_status)
            {
                *out_status = status;
            }
            return (int64_t)child_pid;
        }

        if (ret != -EAGAIN)
        {
            return (int64_t)ret;
        }

        enable_irq();
        wfe();
        disable_irq();
    }
}

static int64_t sys_waitpid(int64_t pid)
{
    return sys_waitpid_common(pid, 0);
}

static int64_t sys_waitpid_status(int64_t pid, int64_t status_ptr)
{
    int32_t status;
    int64_t ret;

    ret = sys_waitpid_common(pid, &status);
    if (ret < 0)
    {
        return ret;
    }

    if (!status_ptr)
    {
        return ret;
    }

    if (!sys_user_mem_valid((const void *)status_ptr, sizeof(status)))
    {
        return -EFAULT;
    }

    if (sys_copy_to_user((void *)status_ptr, &status, sizeof(status)) < 0)
    {
        return -EFAULT;
    }

    return ret;
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
    struct sys_socket_slot *sock;

    if (fd < 0)
    {
        return -EBADF;
    }

    sock = sys_socket_slot_from_fd(fd);
    if (sock)
    {
        memset((int8_t *)&st, 0, sizeof(st));
        st.ino = (uint32_t)(fd - SYS_SOCKET_FD_BASE + 1);
        st.mode = VFS_S_IFSOCK | 0600;
        st.nlink = 1;
        st.uid = 0;
        st.gid = 0;
        st.size = 0;
        st.blocks = 0;
        st.blksize = 4096;
        return sys_copy_to_user((void *)out, &st, sizeof(st));
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
        int32_t target_fd;

        target_fd = sys_stdio_target_fd(fd);
        if (target_fd >= 0)
        {
            return vfs_isatty_fd(target_fd) ? 1 : 0;
        }
        return 1;
    }

    return vfs_isatty_fd((int)fd) ? 1 : 0;
}

static int64_t sys_dup2(int64_t oldfd, int64_t newfd)
{
    bool is_write_end;
    struct sys_pipe_slot *pipe;
    int32_t resolved_oldfd;

    if (newfd < 0)
    {
        return -EBADF;
    }

    resolved_oldfd = sys_stdio_target_fd(oldfd);
    if (resolved_oldfd >= 0)
    {
        oldfd = resolved_oldfd;
    }

    if (newfd >= 0 && newfd <= 2)
    {
        if (oldfd < 3 && !sys_fd_is_stdio_redirected(oldfd))
        {
            return newfd;
        }

        pipe = sys_pipe_slot_from_fd(oldfd, &is_write_end);
        if (pipe)
        {
            sys_pipe_slot_get(pipe, is_write_end);
            if (sys_stdio_set_fd(newfd, (int32_t)oldfd) < 0)
            {
                sys_pipe_slot_put(pipe, is_write_end);
                return -EBADF;
            }
            return newfd;
        }

        if (oldfd >= 3)
        {
            int hidden_fd;

            hidden_fd = vfs_dup((int)(oldfd - 3));
            if (hidden_fd < 0)
            {
                return hidden_fd;
            }

            hidden_fd += 3;
            if (sys_stdio_set_fd(newfd, (int32_t)hidden_fd) < 0)
            {
                (void)sys_close(hidden_fd);
                return -EBADF;
            }
            return newfd;
        }

        return -EBADF;
    }

    if (oldfd < 3)
    {
        return -EBADF;
    }

    return vfs_dup2((int)(oldfd - 3), (int)newfd - 3) + 3;
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
    int8_t kpath[VFS_PATH_MAX];
    int8_t resolved[VFS_PATH_MAX];
    int fd;
    int64_t ret;

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    ret = sys_build_path_from_dirfd(dirfd, kpath, resolved, sizeof(resolved));
    if (ret < 0)
    {
        return ret;
    }

    fd = vfs_open(resolved, (int)flags);
    if (fd < 0)
    {
        return fd;
    }

    return fd + 3;
}

static int64_t sys_fstatat(int64_t dirfd, int64_t path, int64_t out, int64_t flags)
{
    int8_t kpath[VFS_PATH_MAX];
    int8_t resolved[VFS_PATH_MAX];
    struct vfs_stat st;
    int64_t ret;

    if (flags != 0 && flags != AT_SYMLINK_NOFOLLOW)
    {
        return -EINVAL;
    }

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    ret = sys_build_path_from_dirfd(dirfd, kpath, resolved, sizeof(resolved));
    if (ret < 0)
    {
        return ret;
    }

    if (flags & AT_SYMLINK_NOFOLLOW)
    {
        ret = vfs_lstat(resolved, &st);
    }
    else
    {
        ret = vfs_stat(resolved, &st);
    }
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
    int64_t fd_value;
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
    else if (strncmp((const char *)kpath, "/proc/self/fd/", 14) == 0)
    {
        const int8_t *digits;
        bool have_digit;

        digits = &kpath[14];
        fd_value = 0;
        have_digit = false;
        while (*digits)
        {
            if (*digits < '0' || *digits > '9')
            {
                return -ENOENT;
            }

            have_digit = true;
            fd_value = (fd_value * 10) + (int64_t)(*digits - '0');
            if (fd_value > 0x7fffffff)
            {
                return -ENOENT;
            }
            digits++;
        }

        if (!have_digit)
        {
            return -ENOENT;
        }

        ret = sys_fd_path_link(fd_value, target, sizeof(target));
        if (ret < 0)
        {
            return ret;
        }
    }
    else
    {
        ret = vfs_readlink(kpath, target, sizeof(target));
        if (ret < 0)
        {
            return ret;
        }
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

static int64_t sys_symlink(int64_t target, int64_t linkpath)
{
    int8_t ktarget[VFS_PATH_MAX];
    int8_t klink[VFS_PATH_MAX];
    int64_t ret;

    ret = sys_copy_path_from_user(ktarget, sizeof(ktarget), (const int8_t *)target);
    if (ret < 0)
    {
        return ret;
    }

    ret = sys_copy_path_from_user(klink, sizeof(klink), (const int8_t *)linkpath);
    if (ret < 0)
    {
        return ret;
    }

    return vfs_symlink(ktarget, klink);
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

static int64_t sys_link(int64_t old_path, int64_t new_path)
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

    return vfs_link(old_kpath, new_kpath);
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
    int8_t resolved[VFS_PATH_MAX];
    struct vfs_timespec ts[2];
    struct vfs_timespec *atime;
    struct vfs_timespec *mtime;
    struct vfs_stat st;
    int64_t ret;

    if (flags != 0 && flags != AT_SYMLINK_NOFOLLOW)
    {
        return -EINVAL;
    }

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    ret = sys_build_path_from_dirfd(dirfd, kpath, resolved, sizeof(resolved));
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

    if (flags == AT_SYMLINK_NOFOLLOW)
    {
        ret = vfs_lstat(resolved, &st);
        if (ret < 0)
        {
            return ret;
        }

        if ((st.mode & VFS_S_IFMT) == VFS_S_IFLNK)
        {
            /*
             * 目前内核还不提供“修改 symlink 自身时间戳”的真正写路径，
             * 但对大多数用户态程序来说，AT_SYMLINK_NOFOLLOW 只要不报错即可。
             */
            return 0;
        }
    }

    return vfs_utimens(resolved, atime, mtime);
}

static int64_t sys_httpget(int64_t ipv4, int64_t port, int64_t path, int64_t outfd, int64_t timeout_ms)
{
    int8_t kpath[VFS_PATH_MAX];
    struct net_device *dev;
    int64_t ret;

    dev = net_default_device();
    if (!dev)
    {
        return -ENODEV;
    }

    ret = sys_copy_path_from_user(kpath, sizeof(kpath), (const int8_t *)path);
    if (ret < 0)
    {
        return ret;
    }

    return net_http_get(dev, (uint32_t)ipv4, (uint16_t)port, kpath, (int)outfd, (uint32_t)timeout_ms);
}

static int64_t sys_dnslookup(int64_t hostname, int64_t out_ipv4, int64_t timeout_ms)
{
    int8_t khost[VFS_PATH_MAX];
    struct net_device *dev;
    uint32_t resolved_ip;
    int64_t ret;

    dev = net_default_device();
    if (!dev)
    {
        return -ENODEV;
    }

    ret = sys_copy_path_from_user(khost, sizeof(khost), (const int8_t *)hostname);
    if (ret < 0)
    {
        return ret;
    }

    ret = net_dns_lookup(dev, khost, &resolved_ip, (uint32_t)timeout_ms);
    if (ret < 0)
    {
        return ret;
    }

    ret = sys_copy_to_user((void *)out_ipv4, &resolved_ip, sizeof(resolved_ip));
    if (ret < 0)
    {
        return ret;
    }

    return 0;
}

static int64_t sys_socket(int64_t domain, int64_t type, int64_t protocol)
{
    struct sys_socket_slot *slot;
    int fd;

    if (domain != 2)
    {
        return -EAFNOSUPPORT;
    }
    if (type != 1)
    {
        return -EPROTONOSUPPORT;
    }
    if (protocol != 0 && protocol != 6)
    {
        return -EPROTONOSUPPORT;
    }

    fd = sys_socket_fd_alloc(&slot);
    if (fd < 0)
    {
        return fd;
    }

    net_socket_init(&slot->sock);
    slot->domain = (int)domain;
    slot->type = (int)type;
    slot->protocol = (int)protocol;
    slot->flags = 0;
    return fd;
}

static int64_t sys_connect(int64_t fd, int64_t addr, int64_t addrlen)
{
    struct sys_socket_slot *sock;
    struct sys_sockaddr_in sin;
    struct net_device *dev;
    uint32_t ipv4;
    uint16_t port;
    int ret;

    if (!sys_socket_fd_in_range(fd))
    {
        return -EBADF;
    }

    sock = sys_socket_slot_from_fd(fd);
    if (!sock)
    {
        return -EBADF;
    }
    if (sock->sock.priv)
    {
        return -EISCONN;
    }
    if (!addr || addrlen < (int64_t)sizeof(sin))
    {
        return -EINVAL;
    }
    if (!sys_user_mem_valid((const void *)addr, sizeof(sin)))
    {
        return -EFAULT;
    }

    memcpy((int8_t *)&sin, (const int8_t *)addr, sizeof(sin));
    if (sin.sin_family != 2)
    {
        return -EAFNOSUPPORT;
    }

    dev = net_default_device();
    if (!dev)
    {
        return -ENODEV;
    }

    port = __builtin_bswap16(sin.sin_port);
    ipv4 = __builtin_bswap32(sin.sin_addr);
    ret = net_socket_connect_begin(&sock->sock, dev, ipv4, port);
    if (ret == -EINPROGRESS)
    {
        return -EINPROGRESS;
    }
    return ret;
}

static int64_t sys_getsockopt(int64_t fd, int64_t level, int64_t optname, int64_t optval, int64_t optlen)
{
    struct sys_socket_slot *sock;
    int value;
    int32_t value_len;

    sock = sys_socket_slot_from_fd(fd);
    if (!sock)
    {
        return -EBADF;
    }

    if (level != SOL_SOCKET || optname != SO_ERROR)
    {
        return -ENOPROTOOPT;
    }

    if (!optval || !optlen)
    {
        return -EINVAL;
    }
    if (!sys_user_mem_valid((const void *)optval, sizeof(value)) ||
        !sys_user_mem_valid((const void *)optlen, sizeof(value_len)))
    {
        return -EFAULT;
    }

    value = net_socket_so_error(&sock->sock);
    value_len = (int32_t)sizeof(value);
    if (sys_copy_to_user((void *)optval, &value, sizeof(value)) < 0)
    {
        return -EFAULT;
    }
    if (sys_copy_to_user((void *)optlen, &value_len, sizeof(value_len)) < 0)
    {
        return -EFAULT;
    }
    return 0;
}

static int64_t sys_shutdown(int64_t fd, int64_t how)
{
    struct sys_socket_slot *sock;

    sock = sys_socket_slot_from_fd(fd);
    if (!sock)
    {
        return -EBADF;
    }

    return net_socket_shutdown(&sock->sock, (int)how);
}

static int64_t sys_pipe2(int64_t fds, int64_t flags)
{
    struct sys_pipe_slot *slot;
    int pipe_read_fd;
    int pipe_write_fd;
    int32_t out[2];
    uint64_t bad_flags;

    bad_flags = (uint64_t)flags & ~(KERNEL_O_NONBLOCK | 0x80000ULL);
    if (bad_flags)
    {
        return -EINVAL;
    }

    if (!fds || !sys_user_mem_valid((const void *)fds, sizeof(out)))
    {
        return -EFAULT;
    }

    spin_lock(&sys_pipe_lock);
    pipe_read_fd = sys_pipe_fd_alloc(&slot);
    if (pipe_read_fd < 0)
    {
        spin_unlock(&sys_pipe_lock);
        return pipe_read_fd;
    }

    pipe_write_fd = slot->write_fd;
    slot->used = true;
    slot->read_open = true;
    slot->write_open = true;
    slot->read_nonblock = (flags & KERNEL_O_NONBLOCK) != 0;
    slot->write_nonblock = (flags & KERNEL_O_NONBLOCK) != 0;
    slot->head = 0;
    slot->tail = 0;
    slot->used_bytes = 0;
    spin_unlock(&sys_pipe_lock);

    out[0] = pipe_read_fd;
    out[1] = pipe_write_fd;
    if (sys_copy_to_user((void *)fds, &out, sizeof(out)) < 0)
    {
        sys_pipe_slot_put(slot, false);
        sys_pipe_slot_put(slot, true);
        return -EFAULT;
    }

    return 0;
}

static int64_t sys_fbinfo(int64_t out)
{
    struct stupidos_fbinfo info;

    if (!out)
    {
        return -EINVAL;
    }

    ui_fb_info(&info);
    return sys_copy_to_user((void *)out, &info, sizeof(info));
}

static int64_t sys_fbfill(int64_t x, int64_t y, int64_t w, int64_t h, int64_t color)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0)
    {
        return -EINVAL;
    }

    ui_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, (uint32_t)color);
    return 0;
}

static int64_t sys_fbtext(int64_t x, int64_t y, int64_t fg, int64_t bg, int64_t text)
{
    int8_t buf[512];
    int64_t ret;

    if (x < 0 || y < 0 || !text)
    {
        return -EINVAL;
    }

    ret = sys_copy_path_from_user(buf, sizeof(buf), (const int8_t *)text);
    if (ret < 0)
    {
        return ret;
    }

    ui_draw_text((uint32_t)x, (uint32_t)y, (uint32_t)fg, (uint32_t)bg, (const uint8_t *)buf);
    return 0;
}

static int64_t sys_mouseinfo(int64_t out)
{
    struct tty_mouse_state mouse;

    if (!out)
    {
        return -EINVAL;
    }

    /*
     * 这里直接复用 TTY 层维护的鼠标状态。
     * 用户态 UI 只需要一个只读快照，不需要接触输入驱动内部细节。
     */
    tty_mouse_get_state(&mouse);
    return sys_copy_to_user((void *)out, &mouse, sizeof(mouse));
}

static int64_t sys_ioctl(int64_t fd, int64_t request, int64_t argp)
{
    /*
     * 最小终端 ioctl 兼容层：
     * - TCGETS/TCSETS*: 与 tty termios 状态联动
     * - TIOCGWINSZ/FIONREAD: 给编辑器和 shell 做能力探测
     */
    if (fd >= 0 && fd <= 2)
    {
        struct tty_termios_state tios;
        int32_t target_fd;

        target_fd = sys_stdio_target_fd(fd);
        if (target_fd >= 0)
        {
            return sys_ioctl(target_fd, request, argp);
        }

        /* TCGETS */
        if ((uint64_t)request == 0x5401ULL)
        {
            if (!argp)
            {
                return -EINVAL;
            }

            tty_get_termios(&tios);
            return sys_copy_to_user((void *)argp, &tios, sizeof(tios));
        }

        /* TCSETS / TCSETSW / TCSETSF */
        if ((uint64_t)request == 0x5402ULL ||
            (uint64_t)request == 0x5403ULL ||
            (uint64_t)request == 0x5404ULL)
        {
            if (!argp)
            {
                return -EINVAL;
            }
            if (!sys_user_mem_valid((const void *)argp, sizeof(tios)))
            {
                return -EFAULT;
            }

            memcpy((int8_t *)&tios, (int8_t *)argp, sizeof(tios));
            if (tty_set_termios(&tios, ((uint64_t)request == 0x5404ULL) ? 1 : 0) < 0)
            {
                return -EINVAL;
            }
            return 0;
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

            pending = tty_pending_count();
            return sys_copy_to_user((void *)argp, &pending, sizeof(pending));
        }
    }

    if (sys_socket_fd_in_range(fd))
    {
        if ((uint64_t)request == 0x541BULL)
        {
            int32_t pending;
            struct sys_socket_slot *sock;

            if (!argp)
            {
                return -EINVAL;
            }

            sock = sys_socket_slot_from_fd(fd);
            if (!sock)
            {
                return -EBADF;
            }
            pending = net_socket_pending(&sock->sock);
            return sys_copy_to_user((void *)argp, &pending, sizeof(pending));
        }
    }

    if (vfs_isatty_fd((int)fd))
    {
        struct tty_termios_state tios;

        /*
         * /dev/tty 这类伪字符设备也要表现得像真正终端，
         * 否则 Dropbear 的密码提示和终端模式切换会退化成不可用。
         */
        if ((uint64_t)request == 0x5401ULL)
        {
            if (!argp)
            {
                return -EINVAL;
            }

            tty_get_termios(&tios);
            return sys_copy_to_user((void *)argp, &tios, sizeof(tios));
        }

        if ((uint64_t)request == 0x5402ULL ||
            (uint64_t)request == 0x5403ULL ||
            (uint64_t)request == 0x5404ULL)
        {
            if (!argp)
            {
                return -EINVAL;
            }
            if (!sys_user_mem_valid((const void *)argp, sizeof(tios)))
            {
                return -EFAULT;
            }

            memcpy((int8_t *)&tios, (int8_t *)argp, sizeof(tios));
            if (tty_set_termios(&tios, ((uint64_t)request == 0x5404ULL) ? 1 : 0) < 0)
            {
                return -EINVAL;
            }
            return 0;
        }

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

        if ((uint64_t)request == 0x541BULL)
        {
            int32_t pending;

            if (!argp)
            {
                return -EINVAL;
            }

            pending = tty_pending_count();
            return sys_copy_to_user((void *)argp, &pending, sizeof(pending));
        }
    }

    {
        bool is_write_end;
        struct sys_pipe_slot *pipe;

        pipe = sys_pipe_slot_from_fd(fd, &is_write_end);
        if (pipe)
        {
            if ((uint64_t)request == 0x541BULL)
            {
                int32_t pending;
                uint64_t daif;

                if (!argp)
                {
                    return -EINVAL;
                }

                daif = read_daif();
                disable_irq();
                spin_lock(&sys_pipe_lock);
                pending = (int32_t)pipe->used_bytes;
                spin_unlock(&sys_pipe_lock);
                write_daif(daif);
                return sys_copy_to_user((void *)argp, &pending, sizeof(pending));
            }
            return -ENOTTY;
        }
    }

    return -ENOTTY;
}

static int64_t sys_dup(int64_t oldfd)
{
    int fd;
    if (sys_socket_slot_from_fd(oldfd))
    {
        return -ENOTSUP;
    }

    if (oldfd < 3)
    {
        /*
         * 最小 stdio dup 兼容（中文）：
         * CPython 启动时会用 dup(fd) 探测 0/1/2 是否有效。
         * 当前内核尚未给 tty 实现完整的“可分配副本 fd”语义，
         * 这里先返回原 fd，让探测通过；close(0/1/2) 在内核里本来就是 no-op。
         */
        if (oldfd >= 0)
        {
            return oldfd;
        }
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
    task_set_exit_code((int32_t)code);
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

    if (sys_socket_slot_from_fd(fd))
    {
        return -ESPIPE;
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

    if (sys_socket_slot_from_fd(fd))
    {
        return -ESPIPE;
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
    bool is_write_end;
    struct sys_pipe_slot *pipe;
    struct sys_socket_slot *sock;
    int32_t target_fd;

    if (fd >= 0 && fd <= 2)
    {
        target_fd = sys_stdio_target_fd(fd);
        if (target_fd >= 0)
        {
            return sys_fcntl(target_fd, cmd, arg);
        }
    }

    pipe = sys_pipe_slot_from_fd(fd, &is_write_end);
    if (pipe)
    {
        switch ((int)cmd)
        {
        case VFS_F_GETFD:
            return 0;
        case VFS_F_SETFD:
            return 0;
        case VFS_F_GETFL:
            return (is_write_end ? KERNEL_O_WRONLY : KERNEL_O_RDONLY) |
                   ((is_write_end ? pipe->write_nonblock : pipe->read_nonblock) ? KERNEL_O_NONBLOCK : 0);
        case VFS_F_SETFL:
            if (is_write_end)
            {
                pipe->write_nonblock = ((uint64_t)arg & KERNEL_O_NONBLOCK) != 0;
            }
            else
            {
                pipe->read_nonblock = ((uint64_t)arg & KERNEL_O_NONBLOCK) != 0;
            }
            return 0;
        default:
            return -ENOTSUP;
        }
    }

    sock = sys_socket_slot_from_fd(fd);
    if (sock)
    {
        switch ((int)cmd)
        {
        case VFS_F_GETFD:
            return 0;
        case VFS_F_SETFD:
            return 0;
        case VFS_F_GETFL:
            return sock->flags;
        case VFS_F_SETFL:
            sock->flags = (int)arg;
            return 0;
        default:
            return -ENOTSUP;
        }
    }

    if (fd < 3)
    {
        /*
         * stdio 最小 fcntl 兼容（中文）：
         * CPython 在初始化 sys.stdin/stdout/stderr 时会先做 F_GETFD/F_GETFL 探测。
         * 之前对 0/1/2 直接返回 EBADF，导致 Python 认为标准流无效并把 sys.stdout 置为 None。
         */
        switch ((int)cmd)
        {
        case VFS_F_GETFD:
            return 0;
        case VFS_F_SETFD:
            (void)arg;
            return 0;
        case VFS_F_GETFL:
            if (fd == 0)
            {
                return VFS_O_RDONLY;
            }
            return VFS_O_WRONLY;
        case VFS_F_SETFL:
            (void)arg;
            return 0;
        default:
            return -EINVAL;
        }
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
    case SYS_WAITPID_STATUS:
        return sys_waitpid_status((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
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
    case SYS_LINK:
        return sys_link((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_SYMLINK:
        return sys_symlink((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_TRUNCATE:
        return sys_truncate((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_FTRUNCATE:
        return sys_ftruncate((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_UTIMENSAT:
        return sys_utimensat((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1],
                             (int64_t)regs->s_reg[2], (int64_t)regs->s_reg[3]);
    case SYS_HTTPGET:
        return sys_httpget((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1],
                           (int64_t)regs->s_reg[2], (int64_t)regs->s_reg[3],
                           (int64_t)regs->s_reg[4]);
    case SYS_DNSLOOKUP:
        return sys_dnslookup((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1],
                             (int64_t)regs->s_reg[2]);
    case SYS_SOCKET:
        return sys_socket((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1],
                          (int64_t)regs->s_reg[2]);
    case SYS_CONNECT:
        return sys_connect((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1],
                           (int64_t)regs->s_reg[2]);
    case SYS_SHUTDOWN:
        return sys_shutdown((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    case SYS_GETSOCKOPT:
        return sys_getsockopt((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1],
                              (int64_t)regs->s_reg[2], (int64_t)regs->s_reg[3],
                              (int64_t)regs->s_reg[4]);
    case SYS_FBINFO:
        return sys_fbinfo((int64_t)regs->s_reg[0]);
    case SYS_FBFILL:
        return sys_fbfill((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1],
                          (int64_t)regs->s_reg[2], (int64_t)regs->s_reg[3],
                          (int64_t)regs->s_reg[4]);
    case SYS_FBTEXT:
        return sys_fbtext((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1],
                          (int64_t)regs->s_reg[2], (int64_t)regs->s_reg[3],
                          (int64_t)regs->s_reg[4]);
    case SYS_MOUSEINFO:
        return sys_mouseinfo((int64_t)regs->s_reg[0]);
    case SYS_PIPE2:
        return sys_pipe2((int64_t)regs->s_reg[0], (int64_t)regs->s_reg[1]);
    default:
        return -ENOSYS;
    }
}
