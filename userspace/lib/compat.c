#include "stupidos_user.h"
#include "errno.h"
#include "stdlib.h"
#include "string.h"
#include "ctype.h"
#include "stdio.h"
#include "fcntl.h"
#include "pthread.h"
#include "time.h"
#include <wchar.h>
#include <locale.h>
#include <pwd.h>
#include <grp.h>
#include <dirent.h>
#include <langinfo.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <sys/syscall.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/wait.h>
#include <utime.h>
#include <stdarg.h>
#include <setjmp.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <termios.h>
#include <spawn.h>
#include <sched.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <sys/sendfile.h>
#include <semaphore.h>
#include <math.h>

#ifndef __OFF64_TYPEDEF_STUPIDOS
typedef off_t off64_t;
#define __OFF64_TYPEDEF_STUPIDOS 1
#endif

#ifndef TCGETS
#define TCGETS 0x5401
#endif
#ifndef TCSETS
#define TCSETS 0x5402
#endif
#ifndef TCSETSW
#define TCSETSW 0x5403
#endif
#ifndef TCSETSF
#define TCSETSF 0x5404
#endif

/*
 * 某些交叉工具链在 freestanding 配置下不会暴露 stat64/rlimit64 的 tag 定义。
 * compat 层只透传这两个结构体指针，不依赖具体字段布局；
 * 这里前置声明，避免函数原型被编译器当成“临时局部 struct”。
 */
struct stat64;
struct rlimit64;

/*
 * compat.c 内部会在定义这些包装函数之前先调用它们。
 * 这里前置声明，避免 GCC 把调用当成隐式声明并产生噪音。
 */
int chdir(const char *path);
int close(int fd);
int dup2(int oldfd, int newfd);
pid_t fork(void);
int setsid(void);

/*
 * 这是给后续 CPython / 其他 POSIX 用户态程序准备的最小 libc 兼容层。
 * 当前目标不是一次性把标准库做完整，而是先补齐最常见的：
 * - 内存分配
 * - 字符串 / 字符分类
 * - 环境变量
 * - 常用系统调用包装
 *
 * 这些能力够了之后，Python3 才能继续往上走。
 */

#define U_HEAP_MAGIC       0x55484541504d4147ULL
#define U_ALIGN_DEFAULT    16ULL
#define U_PAGE_MIN         65536ULL

struct u_heap_chunk
{
    uint64_t magic;
    uint64_t size;
    bool free;
    struct u_heap_chunk *prev;
    struct u_heap_chunk *next;
    struct u_heap_arena *arena;
};

struct u_heap_arena
{
    void *base;
    uint64_t size;
    struct u_heap_arena *next;
    struct u_heap_chunk *head;
};

struct u_aligned_cookie
{
    uint64_t magic;
    void *orig;
};

static struct u_heap_arena *u_heap_arenas;
static char *u_environ_items[16];
char **environ;
int errno;
static char u_locale_name[16] = "C";
static struct lconv u_locale_conv;
static char u_langinfo_codeset[] = "UTF-8";
static struct passwd u_passwd_root;
static struct passwd u_passwd_user;
static struct group u_group_root;
static struct group u_group_user;
static char *u_group_root_members[] = { (char *)"root", 0 };
static char *u_group_user_members[] = { (char *)"user", 0 };
static int u_passwd_iter_index;
static char u_passwd_root_name[] = "root";
static char u_passwd_root_passwd[] = "x";
static char u_passwd_root_dir[] = "/";
static char u_passwd_root_shell[] = "/bin/sh";
static char u_passwd_user_name[] = "user";
static char u_passwd_user_passwd[] = "x";
static char u_passwd_user_dir[] = "/home/user";
static char u_passwd_user_shell[] = "/bin/sh";
static mode_t u_umask = 0022;
struct u_dir_stream
{
    char path[STUPIDOS_PATH_MAX];
    uint32_t index;
};
static struct dirent u_dirent_entry;
static struct termios u_termios_state;
static bool u_termios_state_ready;
static void (*u_atexit_handlers[32])(void);
static size_t u_atexit_count;
struct u_sem_slot
{
    sem_t *sem;
    int value;
    bool used;
};
static struct u_sem_slot u_sem_slots[64];
static int u_select_sleep_slice(void);

/*
 * 让 *at 系列接口真正可用：
 * - AT_FDCWD 直接沿用传入路径
 * - 其他 dirfd 通过 /proc/self/fd/<n> 取回目录路径
 * - 再把相对路径拼到目录后面
 *
 * 这样像 BusyBox、vi、tcc 这类工具在调用 mkdirat/openat/readlinkat 时，
 * 就不用先回退成手写绝对路径，行为会更接近 Linux。
 */
static int u_resolve_at_path(int dirfd, const char *path, char *out, size_t out_len)
{
    struct stupidos_stat st;
    char proc[32];
    char base[STUPIDOS_PATH_MAX];
    ssize_t base_len;
    size_t path_len;
    int proc_len;

    if (!path || !out || out_len < 2)
    {
        errno = EINVAL;
        return -1;
    }

    if (path[0] == '/' || dirfd == AT_FDCWD)
    {
        path_len = strlen(path);
        if (path_len + 1U > out_len)
        {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(out, path, path_len + 1U);
        return 0;
    }

    if (u_fstat(dirfd, &st) != 0)
    {
        return -1;
    }
    if ((st.mode & STUPIDOS_VFS_S_IFMT) != STUPIDOS_VFS_S_IFDIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    proc_len = snprintf(proc, sizeof(proc), "/proc/self/fd/%d", dirfd);
    if (proc_len < 0 || (size_t)proc_len >= sizeof(proc))
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    base_len = u_readlink((const int8_t *)proc, (int8_t *)base, sizeof(base) - 1U);
    if (base_len < 0)
    {
        return -1;
    }
    base[base_len] = '\0';

    path_len = strlen(path);
    base_len = (ssize_t)strlen(base);
    if ((size_t)base_len + 1U + path_len + 1U > out_len)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(out, base, (size_t)base_len);
    if (base_len == 0 || out[base_len - 1] != '/')
    {
        out[base_len++] = '/';
    }
    memcpy(out + base_len, path, path_len + 1U);
    return 0;
}

/*
 * Dropbear / BusyBox 这类程序需要最小 pipe 支持。
 * 这里先在 libc 层做一套“自管管道”：
 * - pipe() 返回一对用户态 fd
 * - read/write/ioctl/fcntl/select/poll 识别这些 fd
 * - 足够支撑 ssh 的 signal pipe、以及少量简单管道用法
 *
 * 这不是完整的内核 pipe，但能先把最关键的用户体验跑通。
 */
#define U_PIPE_SLOT_MAX     32
/*
 * 把 pipe 的保留 fd 段整体抬高一点，尽量避开普通 open/dup 使用的低位 fd。
 * 这样 Dropbear / BusyBox 之类程序在启动时创建 self-pipe 时更不容易撞号。
 *
 * 这里仍然要控制在 FD_SETSIZE 范围内，否则 select()/poll() 的 fd_set 无法承载。
 */
#define U_PIPE_FD_BASE      512
#define U_PIPE_FD_MAX       1024
#define U_PIPE_BUF_SIZE     4096U

struct u_pipe_slot
{
    bool used;
    int read_fd;
    int write_fd;
    bool read_open;
    bool write_open;
    bool read_nonblock;
    bool write_nonblock;
    size_t head;
    size_t tail;
    size_t used_bytes;
    uint8_t buf[U_PIPE_BUF_SIZE];
};

static struct u_pipe_slot u_pipe_slots[U_PIPE_SLOT_MAX];

static struct u_pipe_slot *u_pipe_slot_from_fd(int fd, bool *is_write_end)
{
    size_t i;

    if (fd < U_PIPE_FD_BASE || fd >= U_PIPE_FD_MAX)
    {
        return 0;
    }

    for (i = 0; i < U_PIPE_SLOT_MAX; i++)
    {
        struct u_pipe_slot *slot = &u_pipe_slots[i];

        if (!slot->used)
        {
            continue;
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
    }

    return 0;
}

static int u_pipe_alloc_fd(void)
{
    int fd;
    size_t i;
    bool taken;

    for (fd = U_PIPE_FD_BASE; fd < U_PIPE_FD_MAX; fd++)
    {
        taken = false;
        for (i = 0; i < U_PIPE_SLOT_MAX; i++)
        {
            if (!u_pipe_slots[i].used)
            {
                continue;
            }
            if (u_pipe_slots[i].read_fd == fd || u_pipe_slots[i].write_fd == fd)
            {
                taken = true;
                break;
            }
        }
        if (!taken)
        {
            return fd;
        }
    }

    return -1;
}

static void u_pipe_release_if_unused(struct u_pipe_slot *slot)
{
    if (!slot)
    {
        return;
    }

    if (!slot->read_open && !slot->write_open)
    {
        memset((int8_t *)slot, 0, sizeof(*slot));
    }
}

static ssize_t u_pipe_read(int fd, void *buf, size_t len)
{
    struct u_pipe_slot *slot;
    bool is_write_end;
    size_t copied;
    size_t first;

    slot = u_pipe_slot_from_fd(fd, &is_write_end);
    if (!slot || is_write_end)
    {
        errno = EBADF;
        return -1;
    }
    if (!buf && len)
    {
        errno = EFAULT;
        return -1;
    }
    if (len == 0)
    {
        return 0;
    }

    for (;;)
    {
        if (slot->used_bytes > 0)
        {
            copied = (len < slot->used_bytes) ? len : slot->used_bytes;
            first = copied;
            if (slot->head + first > U_PIPE_BUF_SIZE)
            {
                first = U_PIPE_BUF_SIZE - slot->head;
            }
            memcpy((int8_t *)buf, (int8_t *)&slot->buf[slot->head], first);
            if (copied > first)
            {
                memcpy((int8_t *)buf + first, (int8_t *)&slot->buf[0], copied - first);
            }
            slot->head = (slot->head + copied) % U_PIPE_BUF_SIZE;
            slot->used_bytes -= copied;
            return (ssize_t)copied;
        }

        if (!slot->write_open)
        {
            return 0;
        }

        if (slot->read_nonblock)
        {
            errno = EAGAIN;
            return -1;
        }

        if (u_select_sleep_slice() < 0)
        {
            errno = EINTR;
            return -1;
        }
    }
}

static ssize_t u_pipe_write(int fd, const void *buf, size_t len)
{
    struct u_pipe_slot *slot;
    bool is_write_end;
    size_t written;
    size_t space;
    size_t first;

    slot = u_pipe_slot_from_fd(fd, &is_write_end);
    if (!slot || !is_write_end)
    {
        errno = EBADF;
        return -1;
    }
    if (!buf && len)
    {
        errno = EFAULT;
        return -1;
    }
    if (len == 0)
    {
        return 0;
    }

    if (!slot->read_open)
    {
        errno = EPIPE;
        return -1;
    }

    written = 0;
    while (written < len)
    {
        space = U_PIPE_BUF_SIZE - slot->used_bytes;
        if (space == 0)
        {
            if (slot->write_nonblock)
            {
                errno = EAGAIN;
                return (written > 0) ? (ssize_t)written : -1;
            }

            if (u_select_sleep_slice() < 0)
            {
                errno = EINTR;
                return (written > 0) ? (ssize_t)written : -1;
            }
            continue;
        }

        first = len - written;
        if (first > space)
        {
            first = space;
        }
        if (slot->tail + first > U_PIPE_BUF_SIZE)
        {
            first = U_PIPE_BUF_SIZE - slot->tail;
        }

        memcpy((int8_t *)&slot->buf[slot->tail], (int8_t *)buf + written, first);
        if ((len - written) > first && (space - first) > 0)
        {
            size_t second = (len - written - first);
            if (second > (space - first))
            {
                second = space - first;
            }
            memcpy((int8_t *)&slot->buf[0], (int8_t *)buf + written + first, second);
            slot->tail = second;
            slot->used_bytes += first + second;
            written += first + second;
        }
        else
        {
            slot->tail = (slot->tail + first) % U_PIPE_BUF_SIZE;
            slot->used_bytes += first;
            written += first;
        }
    }

    return (ssize_t)written;
}

static int u_pipe_close(int fd)
{
    struct u_pipe_slot *slot;
    bool is_write_end;

    slot = u_pipe_slot_from_fd(fd, &is_write_end);
    if (!slot)
    {
        errno = EBADF;
        return -1;
    }

    if (is_write_end)
    {
        slot->write_open = false;
    }
    else
    {
        slot->read_open = false;
    }
    u_pipe_release_if_unused(slot);
    return 0;
}

static int u_pipe_ioctl(int fd, unsigned long request, void *argp)
{
    struct u_pipe_slot *slot;
    bool is_write_end;

    slot = u_pipe_slot_from_fd(fd, &is_write_end);
    if (!slot)
    {
        errno = EBADF;
        return -1;
    }

    if (request == FIONREAD)
    {
        if (!argp)
        {
            errno = EINVAL;
            return -1;
        }
        *(int *)argp = (int)slot->used_bytes;
        return 0;
    }

    errno = ENOTTY;
    return -1;
}

static int u_pipe_fcntl(int fd, int cmd, unsigned long arg)
{
    struct u_pipe_slot *slot;
    bool is_write_end;
    bool *nonblock_flag;

    slot = u_pipe_slot_from_fd(fd, &is_write_end);
    if (!slot)
    {
        return 0;
    }

    nonblock_flag = is_write_end ? &slot->write_nonblock : &slot->read_nonblock;
    switch (cmd)
    {
    case F_GETFD:
        return 0;
    case F_SETFD:
        return 0;
    case F_GETFL:
        return (int)(is_write_end ? O_WRONLY : O_RDONLY) | (*nonblock_flag ? O_NONBLOCK : 0);
    case F_SETFL:
        *nonblock_flag = (arg & O_NONBLOCK) ? true : false;
        return 0;
    default:
        errno = ENOTSUP;
        return -1;
    }
}

int pipe(int fds[2]);
int pipe2(int fds[2], int flags);

void u_lib_early_init(void)
{
    /*
     * 关键修复：不要用全局静态初始化把 environ 指向 u_environ_items。
     * 该初始化会在可执行文件链接虚拟地址下固化成低地址常量，加载到高地址后失效。
     * 这里在运行时用真实地址重新绑定，避免 Python/stdlib 在 getenv/setenv 路径崩溃。
     */
    environ = u_environ_items;
}

int *__errno_location(void)
{
    return &errno;
}

/*
 * Dropbear 以及部分 POSIX 代码会通过 glibc 风格的内部符号
 * __xpg_basename() / dirname() 访问路径处理逻辑。
 * 我们这里给一组最小实现，避免第三方程序在 freestanding 链接时
 * 继续依赖宿主 libc。
 */
char *__xpg_basename(char *path)
{
    char *start;
    char *end;

    if (!path || !*path)
    {
        return (char *)".";
    }

    start = path;
    end = path + strlen(path);
    while (end > start + 1 && end[-1] == '/')
    {
        *--end = '\0';
    }

    if (end == start + 1 && start[0] == '/')
    {
        return start;
    }

    while (end > start && end[-1] != '/')
    {
        end--;
    }

    return end;
}

char *dirname(char *path)
{
    char *start;
    char *end;

    if (!path || !*path)
    {
        return (char *)".";
    }

    start = path;
    end = path + strlen(path);
    while (end > start + 1 && end[-1] == '/')
    {
        *--end = '\0';
    }

    if (end == start + 1 && start[0] == '/')
    {
        return start;
    }

    while (end > start && end[-1] != '/')
    {
        end--;
    }

    if (end == start)
    {
        return (char *)".";
    }

    while (end > start + 1 && end[-1] == '/')
    {
        end--;
    }
    *end = '\0';
    return start;
}

static bool u_sysret_is_error(int64_t value)
{
    return value < 0 && value >= -4095;
}

static int u_sysret_int(int64_t value)
{
    if (u_sysret_is_error(value))
    {
        errno = (int)(-value);
        return -1;
    }
    return (int)value;
}

static ssize_t u_sysret_ssize(int64_t value)
{
    if (u_sysret_is_error(value))
    {
        errno = (int)(-value);
        return -1;
    }
    return (ssize_t)value;
}

static off_t u_sysret_off(int64_t value)
{
    if (u_sysret_is_error(value))
    {
        errno = (int)(-value);
        return (off_t)-1;
    }
    return (off_t)value;
}

static struct u_sem_slot *u_sem_slot_find(sem_t *sem)
{
    size_t i;

    for (i = 0; i < sizeof(u_sem_slots) / sizeof(u_sem_slots[0]); i++)
    {
        if (u_sem_slots[i].used && u_sem_slots[i].sem == sem)
        {
            return &u_sem_slots[i];
        }
    }
    return 0;
}

static struct u_sem_slot *u_sem_slot_alloc(sem_t *sem)
{
    size_t i;

    for (i = 0; i < sizeof(u_sem_slots) / sizeof(u_sem_slots[0]); i++)
    {
        if (!u_sem_slots[i].used)
        {
            u_sem_slots[i].used = true;
            u_sem_slots[i].sem = sem;
            u_sem_slots[i].value = 0;
            return &u_sem_slots[i];
        }
    }
    return 0;
}

static int u_errno_rofs(void)
{
    /*
     * 语义选择（中文）：
     * 这批接口属于“会修改文件系统元数据/目录结构”的操作。
     * 当前内核 VFS 还没有完整的 create/unlink/rename/chmod 管线，
     * 用 EROFS 比 ENOSYS 更贴近真实场景，也更利于上层工具优雅降级。
     */
    errno = EROFS;
    return -1;
}

static int u_errno_notsup(void)
{
    errno = ENOTSUP;
    return -1;
}

static ssize_t u_errno_notsup_ssize(void)
{
    errno = ENOTSUP;
    return -1;
}

static uint64_t u_align_up(uint64_t value, uint64_t align)
{
    if (!align)
    {
        return value;
    }

    return (value + align - 1ULL) & ~(align - 1ULL);
}

static uint64_t u_heap_round_size(uint64_t size)
{
    uint64_t need;

    need = size + sizeof(struct u_heap_chunk) + 64ULL;
    if (need < U_PAGE_MIN)
    {
        need = U_PAGE_MIN;
    }
    return u_align_up(need, 4096ULL);
}

static struct u_heap_arena *u_heap_new_arena(uint64_t need)
{
    struct u_heap_arena *arena;
    struct u_heap_chunk *chunk;
    int64_t map_ret;
    uint64_t size;

    size = u_heap_round_size(need);
    map_ret = (int64_t)u_mmap(0, size, 0, 0, -1, 0);
    if (u_sysret_is_error(map_ret))
    {
        errno = (int)(-map_ret);
        return 0;
    }

    arena = (struct u_heap_arena *)(uint64_t)map_ret;
    if (!arena)
    {
        errno = ENOMEM;
        return 0;
    }

    memset((int8_t *)arena, 0, (size_t)size);
    arena->base = arena;
    arena->size = size;
    arena->next = u_heap_arenas;
    u_heap_arenas = arena;

    chunk = (struct u_heap_chunk *)((uint8_t *)arena + sizeof(struct u_heap_arena));
    chunk->magic = U_HEAP_MAGIC;
    chunk->size = size - sizeof(struct u_heap_arena) - sizeof(struct u_heap_chunk);
    chunk->free = true;
    chunk->prev = 0;
    chunk->next = 0;
    chunk->arena = arena;
    arena->head = chunk;
    return arena;
}

static struct u_heap_chunk *u_chunk_from_ptr(void *ptr)
{
    if (!ptr || (uint64_t)ptr < 0x1000ULL)
    {
        return 0;
    }

    return (struct u_heap_chunk *)((uint8_t *)ptr - sizeof(struct u_heap_chunk));
}

static void u_chunk_split(struct u_heap_chunk *chunk, uint64_t need)
{
    struct u_heap_chunk *tail;
    uint64_t remain;

    if (!chunk || chunk->size <= need + sizeof(struct u_heap_chunk) + 32ULL)
    {
        return;
    }

    remain = chunk->size - need - sizeof(struct u_heap_chunk);
    tail = (struct u_heap_chunk *)((uint8_t *)chunk + sizeof(struct u_heap_chunk) + need);
    tail->magic = U_HEAP_MAGIC;
    tail->size = remain;
    tail->free = true;
    tail->prev = chunk;
    tail->next = chunk->next;
    tail->arena = chunk->arena;
    if (tail->next)
    {
        tail->next->prev = tail;
    }
    chunk->next = tail;
    chunk->size = need;
}

static struct u_heap_chunk *u_heap_find_chunk(uint64_t need)
{
    struct u_heap_arena *arena;
    struct u_heap_chunk *chunk;

    for (arena = u_heap_arenas; arena; arena = arena->next)
    {
        for (chunk = arena->head; chunk; chunk = chunk->next)
        {
            if (chunk->magic != U_HEAP_MAGIC)
            {
                continue;
            }
            if (chunk->free && chunk->size >= need)
            {
                return chunk;
            }
        }
    }

    return 0;
}

static void *u_heap_alloc(uint64_t size)
{
    struct u_heap_chunk *chunk;
    uint64_t need;

    if (!size)
    {
        size = 1;
    }

    need = u_align_up(size, U_ALIGN_DEFAULT);
    chunk = u_heap_find_chunk(need);
    if (!chunk)
    {
        if (!u_heap_new_arena(need))
        {
            return 0;
        }
        chunk = u_heap_find_chunk(need);
        if (!chunk)
        {
            return 0;
        }
    }

    u_chunk_split(chunk, need);
    chunk->free = false;
    return (uint8_t *)chunk + sizeof(struct u_heap_chunk);
}

static void u_heap_merge(struct u_heap_chunk *chunk)
{
    if (!chunk)
    {
        return;
    }

    if (chunk->next && chunk->next->free && chunk->next->magic == U_HEAP_MAGIC)
    {
        struct u_heap_chunk *next = chunk->next;

        chunk->size += sizeof(struct u_heap_chunk) + next->size;
        chunk->next = next->next;
        if (chunk->next)
        {
            chunk->next->prev = chunk;
        }
    }

    if (chunk->prev && chunk->prev->free && chunk->prev->magic == U_HEAP_MAGIC)
    {
        struct u_heap_chunk *prev = chunk->prev;

        prev->size += sizeof(struct u_heap_chunk) + chunk->size;
        prev->next = chunk->next;
        if (chunk->next)
        {
            chunk->next->prev = prev;
        }
    }
}

void *malloc(size_t size)
{
    return u_heap_alloc((uint64_t)size);
}

void free(void *ptr)
{
    struct u_heap_chunk *chunk;
    struct u_aligned_cookie *cookie;

    if (!ptr)
    {
        return;
    }

    chunk = u_chunk_from_ptr(ptr);
    if (chunk && chunk->magic == U_HEAP_MAGIC)
    {
        chunk->free = true;
        u_heap_merge(chunk);
        return;
    }

    cookie = (struct u_aligned_cookie *)((uint8_t *)ptr - sizeof(struct u_aligned_cookie));
    if (cookie->magic == U_HEAP_MAGIC)
    {
        free(cookie->orig);
    }
}

void *calloc(size_t nmemb, size_t size)
{
    uint64_t total;
    void *ptr;

    total = (uint64_t)nmemb * (uint64_t)size;
    ptr = malloc((size_t)total);
    if (ptr)
    {
        memset((int8_t *)ptr, 0, (size_t)total);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    void *nptr;
    struct u_heap_chunk *chunk;
    uint64_t copy_len;

    if (!ptr)
    {
        return malloc(size);
    }

    if (size == 0)
    {
        free(ptr);
        return 0;
    }

    chunk = u_chunk_from_ptr(ptr);
    if (!chunk || chunk->magic != U_HEAP_MAGIC)
    {
        return 0;
    }

    if (chunk->size >= size)
    {
        return ptr;
    }

    nptr = malloc(size);
    if (!nptr)
    {
        return 0;
    }

    copy_len = chunk->size;
    if (copy_len > size)
    {
        copy_len = size;
    }
    memcpy((int8_t *)nptr, (int8_t *)ptr, (size_t)copy_len);
    free(ptr);
    return nptr;
}

void *aligned_alloc(size_t alignment, size_t size)
{
    uint64_t need;
    uint8_t *raw;
    uint8_t *aligned;
    struct u_aligned_cookie *cookie;

    if (alignment < sizeof(void *))
    {
        alignment = sizeof(void *);
    }
    need = (uint64_t)size + alignment + sizeof(struct u_aligned_cookie);
    raw = (uint8_t *)malloc((size_t)need);
    if (!raw)
    {
        return 0;
    }

    aligned = (uint8_t *)u_align_up((uint64_t)(raw + sizeof(struct u_aligned_cookie)), (uint64_t)alignment);
    cookie = (struct u_aligned_cookie *)(aligned - sizeof(struct u_aligned_cookie));
    cookie->magic = U_HEAP_MAGIC;
    cookie->orig = raw;
    return aligned;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    void *ptr;

    if (!memptr || alignment == 0 || (alignment & (alignment - 1U)) != 0)
    {
        return EINVAL;
    }

    ptr = aligned_alloc(alignment, size);
    if (!ptr)
    {
        return ENOMEM;
    }

    *memptr = ptr;
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *ret;

    ret = dst;
    while ((*dst++ = *src++) != '\0')
    {
    }
    return ret;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        if (src[i] == '\0')
        {
            while (i < n)
            {
                dst[i++] = '\0';
            }
            return dst;
        }
        dst[i] = src[i];
    }
    return dst;
}

size_t strlen(const char *s)
{
    return u_strlen((const int8_t *)s);
}

size_t strnlen(const char *s, size_t max)
{
    return u_strnlen((const int8_t *)s, max);
}

int strcmp(const char *a, const char *b)
{
    return u_strcmp((const int8_t *)a, (const int8_t *)b);
}

int strncmp(const char *a, const char *b, size_t n)
{
    size_t i;

    if (!n)
    {
        return 0;
    }

    for (i = 0; i < n; i++)
    {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb || ca == '\0' || cb == '\0')
        {
            return (int)ca - (int)cb;
        }
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    while (*s)
    {
        if (*s == (char)c)
        {
            return (char *)s;
        }
        s++;
    }
    return (c == '\0') ? (char *)s : 0;
}

char *strrchr(const char *s, int c)
{
    const char *last = 0;

    while (*s)
    {
        if (*s == (char)c)
        {
            last = s;
        }
        s++;
    }
    if (c == '\0')
    {
        return (char *)s;
    }
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t nlen;
    size_t i;

    if (!needle || !needle[0])
    {
        return (char *)haystack;
    }

    nlen = strlen(needle);
    for (i = 0; haystack[i] != '\0'; i++)
    {
        if (strncmp(&haystack[i], needle, nlen) == 0)
        {
            return (char *)&haystack[i];
        }
    }
    return 0;
}

size_t strcspn(const char *s, const char *reject)
{
    const char *p;
    const char *r;

    if (!s || !reject)
    {
        return 0;
    }

    for (p = s; *p; p++)
    {
        for (r = reject; *r; r++)
        {
            if (*p == *r)
            {
                return (size_t)(p - s);
            }
        }
    }

    return (size_t)(p - s);
}

void *memcpy(void *dst, const void *src, size_t len)
{
    return u_memcpy(dst, src, len);
}

void *memset(void *dst, int value, size_t len)
{
    return u_memset(dst, value, len);
}

void *memmove(void *dst, const void *src, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || !len)
    {
        return dst;
    }
    if (d < s)
    {
        while (len--)
        {
            *d++ = *s++;
        }
    }
    else
    {
        d += len;
        s += len;
        while (len--)
        {
            *--d = *--s;
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t len)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    size_t i;

    for (i = 0; i < len; i++)
    {
        if (pa[i] != pb[i])
        {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t i;

    for (i = 0; i < n; i++)
    {
        if (p[i] == (uint8_t)c)
        {
            return (void *)&p[i];
        }
    }
    return 0;
}

char *strdup(const char *s)
{
    size_t len;
    char *out;

    if (!s)
    {
        return 0;
    }

    len = strlen(s) + 1;
    out = (char *)malloc(len);
    if (!out)
    {
        return 0;
    }
    memcpy(out, s, len);
    return out;
}

char *strndup(const char *s, size_t n)
{
    size_t len;
    char *out;

    if (!s)
    {
        return 0;
    }

    len = strnlen(s, n);
    out = (char *)malloc(len + 1);
    if (!out)
    {
        return 0;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

int isspace(int ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

int isblank(int ch)
{
    return ch == ' ' || ch == '\t';
}

int isascii(int ch)
{
    return ((unsigned int)ch & ~0x7FU) == 0U;
}

int isdigit(int ch)
{
    return ch >= '0' && ch <= '9';
}

int isxdigit(int ch)
{
    return isdigit(ch)
        || (ch >= 'a' && ch <= 'f')
        || (ch >= 'A' && ch <= 'F');
}

int isalpha(int ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

int isalnum(int ch)
{
    return isalpha(ch) || isdigit(ch);
}

int islower(int ch)
{
    return ch >= 'a' && ch <= 'z';
}

int isupper(int ch)
{
    return ch >= 'A' && ch <= 'Z';
}

int isprint(int ch)
{
    return ch >= 0x20 && ch <= 0x7e;
}

int ispunct(int ch)
{
    return isprint(ch) && !isalnum(ch) && !isspace(ch);
}

int tolower(int ch)
{
    if (isupper(ch))
    {
        return ch - 'A' + 'a';
    }
    return ch;
}

int toupper(int ch)
{
    if (islower(ch))
    {
        return ch - 'a' + 'A';
    }
    return ch;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    size_t i;

    if (!a || !b)
    {
        return a ? 1 : (b ? -1 : 0);
    }

    for (i = 0; i < n; i++)
    {
        unsigned char ca = (unsigned char)tolower((unsigned char)a[i]);
        unsigned char cb = (unsigned char)tolower((unsigned char)b[i]);
        if (ca != cb || ca == '\0' || cb == '\0')
        {
            return (int)ca - (int)cb;
        }
    }
    return 0;
}

int strcasecmp(const char *a, const char *b)
{
    size_t lena = a ? strlen(a) : 0;
    size_t lenb = b ? strlen(b) : 0;
    size_t n = lena > lenb ? lena : lenb;
    return strncasecmp(a, b, n + 1U);
}

static int64_t u_parse_num(const char *nptr, int base, bool signed_mode, int64_t *out_signed, uint64_t *out_unsigned)
{
    const char *p;
    int neg;
    uint64_t value;
    uint64_t digit;

    if (!nptr)
    {
        return -EINVAL;
    }

    p = nptr;
    while (isspace((unsigned char)*p))
    {
        p++;
    }

    neg = 0;
    if (*p == '+' || *p == '-')
    {
        neg = (*p == '-');
        p++;
    }

    if (base == 0)
    {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        {
            base = 16;
            p += 2;
        }
        else if (p[0] == '0')
        {
            base = 8;
        }
        else
        {
            base = 10;
        }
    }

    value = 0;
    while (*p)
    {
        if (*p >= '0' && *p <= '9')
        {
            digit = (uint64_t)(*p - '0');
        }
        else if (*p >= 'a' && *p <= 'z')
        {
            digit = 10ULL + (uint64_t)(*p - 'a');
        }
        else if (*p >= 'A' && *p <= 'Z')
        {
            digit = 10ULL + (uint64_t)(*p - 'A');
        }
        else
        {
            break;
        }

        if (digit >= (uint64_t)base)
        {
            break;
        }

        value = value * (uint64_t)base + digit;
        p++;
    }

    if (signed_mode)
    {
        if (neg)
        {
            *out_signed = -(int64_t)value;
        }
        else
        {
            *out_signed = (int64_t)value;
        }
    }
    else
    {
        *out_unsigned = neg ? 0 : value;
    }

    return 0;
}

int atoi(const char *nptr)
{
    int64_t v;

    if (u_parse_num(nptr, 10, true, &v, 0) < 0)
    {
        return 0;
    }
    return (int)v;
}

long atol(const char *nptr)
{
    return strtol(nptr, 0, 10);
}

long strtol(const char *nptr, char **endptr, int base)
{
    int64_t v;
    const char *p = nptr;

    if (u_parse_num(nptr, base, true, &v, 0) < 0)
    {
        if (endptr)
        {
            *endptr = (char *)nptr;
        }
        return 0;
    }
    if (endptr)
    {
        while (p && *p)
        {
            p++;
        }
        *endptr = (char *)p;
    }
    return (long)v;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    uint64_t v;
    const char *p = nptr;

    if (u_parse_num(nptr, base, false, 0, &v) < 0)
    {
        if (endptr)
        {
            *endptr = (char *)nptr;
        }
        return 0;
    }
    if (endptr)
    {
        while (p && *p)
        {
            p++;
        }
        *endptr = (char *)p;
    }
    return (unsigned long)v;
}

long long strtoll(const char *nptr, char **endptr, int base)
{
    return (long long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
    return (unsigned long long)strtoul(nptr, endptr, base);
}

char *strerror(int errnum)
{
    switch (errnum)
    {
        case 0: return "Success";
        case E2BIG: return "Argument list too long";
        case EACCES: return "Permission denied";
        case EADDRINUSE: return "Address already in use";
        case EADDRNOTAVAIL: return "Cannot assign requested address";
        case EAFNOSUPPORT: return "Address family not supported";
        case EAGAIN: return "Resource temporarily unavailable";
        case EBADF: return "Bad file descriptor";
        case EBUSY: return "Device or resource busy";
        case ECHILD: return "No child processes";
        case ECONNABORTED: return "Software caused connection abort";
        case ECONNREFUSED: return "Connection refused";
        case ECONNRESET: return "Connection reset by peer";
        case EEXIST: return "File exists";
        case EFAULT: return "Bad address";
        case EINVAL: return "Invalid argument";
        case EIO: return "Input/output error";
        case EISDIR: return "Is a directory";
        case ENOENT: return "No such file or directory";
        case ENOMEM: return "Out of memory";
        case ENOSPC: return "No space left on device";
        case ENOSYS: return "Function not implemented";
        case ENOTDIR: return "Not a directory";
        case ENOTEMPTY: return "Directory not empty";
        case ENOTTY: return "Inappropriate ioctl for device";
        case EPERM: return "Operation not permitted";
        case EPIPE: return "Broken pipe";
        case ERANGE: return "Numerical result out of range";
        case EROFS: return "Read-only file system";
        case ETIMEDOUT: return "Connection timed out";
        default: return "Unknown error";
    }
}

void explicit_bzero(void *s, size_t n)
{
    /*
     * 保持和 libc 语义一致：即使编译器优化，也不要把这段清零折叠掉。
     */
    volatile unsigned char *p = (volatile unsigned char *)s;

    while (n--)
    {
        *p++ = 0;
    }
}

char *getpass(const char *prompt)
{
    static char passbuf[256];
    char *nl;
    FILE *tty;
    struct termios old_tio;
    struct termios noecho_tio;
    int have_tty_termios;

    if (prompt && *prompt)
    {
        tty = fopen("/dev/tty", "r+");
        if (tty)
        {
            have_tty_termios = (tcgetattr(fileno(tty), &old_tio) == 0);
            if (have_tty_termios)
            {
                noecho_tio = old_tio;
                noecho_tio.c_lflag &= (tcflag_t)~ECHO;
                (void)tcsetattr(fileno(tty), TCSANOW, &noecho_tio);
            }
            fputs(prompt, tty);
            fflush(tty);
            if (!fgets(passbuf, sizeof(passbuf), tty))
            {
                passbuf[0] = '\0';
                if (have_tty_termios)
                {
                    (void)tcsetattr(fileno(tty), TCSANOW, &old_tio);
                }
                fclose(tty);
                return passbuf;
            }
            if (have_tty_termios)
            {
                (void)tcsetattr(fileno(tty), TCSANOW, &old_tio);
            }
            fputs("\n", tty);
            fflush(tty);
            fclose(tty);
        }
        else
        {
            fputs(prompt, stderr);
            fflush(stderr);
            if (!fgets(passbuf, sizeof(passbuf), stdin))
            {
                passbuf[0] = '\0';
                return passbuf;
            }
        }
    }

    nl = strchr(passbuf, '\n');
    if (nl)
    {
        *nl = '\0';
    }
    return passbuf;
}

int daemon(int nochdir, int noclose)
{
    pid_t pid;
    int fd;

    pid = fork();
    if (pid < 0)
    {
        return -1;
    }
    if (pid > 0)
    {
        _exit(0);
    }

    if (setsid() < 0)
    {
        return -1;
    }

    if (!nochdir)
    {
        (void)chdir("/");
    }

    if (!noclose)
    {
        fd = open("/dev/null", O_RDWR, 0);
        if (fd >= 0)
        {
            (void)dup2(fd, 0);
            (void)dup2(fd, 1);
            (void)dup2(fd, 2);
            if (fd > 2)
            {
                close(fd);
            }
        }
    }

    return 0;
}

size_t wcslen(const wchar_t *s)
{
    size_t len = 0;

    if (!s)
    {
        return 0;
    }

    while (s[len] != L'\0')
    {
        len++;
    }
    return len;
}

wchar_t *wcscpy(wchar_t *dst, const wchar_t *src)
{
    size_t i = 0;

    while (src[i] != L'\0')
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = L'\0';
    return dst;
}

wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n)
{
    size_t i = 0;

    while (i < n && src[i] != L'\0')
    {
        dst[i] = src[i];
        i++;
    }
    while (i < n)
    {
        dst[i++] = L'\0';
    }
    return dst;
}

wchar_t *wcscat(wchar_t *dst, const wchar_t *src)
{
    return wcscpy(dst + wcslen(dst), src);
}

wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n)
{
    size_t dlen = wcslen(dst);
    size_t i = 0;

    while (i < n && src[i] != L'\0')
    {
        dst[dlen + i] = src[i];
        i++;
    }
    dst[dlen + i] = L'\0';
    return dst;
}

int wcscmp(const wchar_t *a, const wchar_t *b)
{
    while (*a && *b && *a == *b)
    {
        a++;
        b++;
    }
    return (int)(*a) - (int)(*b);
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        if (a[i] != b[i] || a[i] == L'\0' || b[i] == L'\0')
        {
            return (int)a[i] - (int)b[i];
        }
    }
    return 0;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c)
{
    while (*s)
    {
        if (*s == c)
        {
            return (wchar_t *)s;
        }
        s++;
    }
    return (c == L'\0') ? (wchar_t *)s : 0;
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c)
{
    const wchar_t *last = 0;

    while (*s)
    {
        if (*s == c)
        {
            last = s;
        }
        s++;
    }
    if (c == L'\0')
    {
        return (wchar_t *)s;
    }
    return (wchar_t *)last;
}

wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **saveptr)
{
    wchar_t *start;

    if (!saveptr || !delim)
    {
        return 0;
    }

    start = s ? s : *saveptr;
    if (!start)
    {
        return 0;
    }

    while (*start && wcschr(delim, *start))
    {
        start++;
    }
    if (!*start)
    {
        *saveptr = 0;
        return 0;
    }

    s = start;
    while (*s && !wcschr(delim, *s))
    {
        s++;
    }
    if (*s)
    {
        *s++ = L'\0';
    }
    *saveptr = s;
    return start;
}

long wcstol(const wchar_t *nptr, wchar_t **endptr, int base)
{
    long result = 0;
    int neg = 0;
    const wchar_t *p = nptr;

    if (!p)
    {
        if (endptr)
        {
            *endptr = 0;
        }
        return 0;
    }

    while (*p == L' ' || *p == L'\t' || *p == L'\n' || *p == L'\r' || *p == L'\v' || *p == L'\f')
    {
        p++;
    }
    if (*p == L'+' || *p == L'-')
    {
        neg = (*p == L'-');
        p++;
    }
    if (base == 0)
    {
        base = 10;
        if (p[0] == L'0')
        {
            base = 8;
            if (p[1] == L'x' || p[1] == L'X')
            {
                base = 16;
                p += 2;
            }
        }
    }
    while (*p)
    {
        int digit;

        if (*p >= L'0' && *p <= L'9')
        {
            digit = *p - L'0';
        }
        else if (*p >= L'a' && *p <= L'z')
        {
            digit = 10 + (*p - L'a');
        }
        else if (*p >= L'A' && *p <= L'Z')
        {
            digit = 10 + (*p - L'A');
        }
        else
        {
            break;
        }
        if (digit >= base)
        {
            break;
        }
        result = result * base + digit;
        p++;
    }

    if (endptr)
    {
        *endptr = (wchar_t *)p;
    }
    return neg ? -result : result;
}

size_t mbstowcs(wchar_t *dst, const char *src, size_t len)
{
    size_t i = 0;
    uintptr_t src_addr;

    src_addr = (uintptr_t)src;
    if (!src ||
        src_addr == (uintptr_t)-1 ||
        src_addr == (uintptr_t)0xffffffffU ||
        (src_addr >= (uintptr_t)0x80000000ULL &&
         src_addr < (uintptr_t)0xffff000000000000ULL) ||
        src_addr < 0x1000U)
    {
        /*
         * 某些兼容路径（尤其是 Python 启动早期探测）可能把错误码哨兵值
         * 当成字符串指针传进来。这里统一返回 DECODE_ERROR，避免用户态
         * 直接触发 data abort。
         */
        return (size_t)-1;
    }
    while (src[i] && (!dst || i < len))
    {
        if (dst)
        {
            dst[i] = (wchar_t)(unsigned char)src[i];
        }
        i++;
    }
    if (dst && i < len)
    {
        dst[i] = L'\0';
    }
    return i;
}

size_t wcstombs(char *dst, const wchar_t *src, size_t len)
{
    size_t i = 0;
    uintptr_t src_addr;

    src_addr = (uintptr_t)src;
    if (!src ||
        src_addr == (uintptr_t)-1 ||
        src_addr == (uintptr_t)0xffffffffU ||
        (src_addr >= (uintptr_t)0x80000000ULL &&
         src_addr < (uintptr_t)0xffff000000000000ULL) ||
        src_addr < 0x1000U)
    {
        return (size_t)-1;
    }
    while (src[i] && (!dst || i < len))
    {
        if (dst)
        {
            dst[i] = (char)(src[i] & 0x7F);
        }
        i++;
    }
    if (dst && i < len)
    {
        dst[i] = '\0';
    }
    return i;
}

int wmemcmp(const wchar_t *lhs, const wchar_t *rhs, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        if (lhs[i] != rhs[i])
        {
            return (lhs[i] < rhs[i]) ? -1 : 1;
        }
    }
    return 0;
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps)
{
    (void)ps;

    if (!s)
    {
        return 0;
    }
    if (n == 0)
    {
        return (size_t)-2;
    }
    if (*s == '\0')
    {
        if (pwc)
        {
            *pwc = L'\0';
        }
        return 0;
    }
    if (pwc)
    {
        *pwc = (wchar_t)(unsigned char)*s;
    }
    return 1;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *ps)
{
    return mbrtowc(0, s, n, ps);
}

size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n)
{
    size_t len;
    size_t i;

    len = wcslen(src);
    if (!dst || n == 0)
    {
        return len;
    }

    for (i = 0; i + 1 < n && src[i] != L'\0'; i++)
    {
        dst[i] = src[i];
    }
    dst[i] = L'\0';
    return len;
}

int wcscoll(const wchar_t *lhs, const wchar_t *rhs)
{
    return wcscmp(lhs, rhs);
}

size_t wcsftime(wchar_t *s, size_t max, const wchar_t *fmt, const struct tm *tm)
{
    size_t i;

    (void)tm;
    if (!s || max == 0 || !fmt)
    {
        return 0;
    }

    /*
     * 先做最小实现：直接把格式串透传给调用方，避免依赖完整 strftime。
     * 这足够让 CPython 的 time 模块基本路径继续运行，不会在链接期缺符号。
     */
    for (i = 0; i + 1 < max && fmt[i] != L'\0'; i++)
    {
        s[i] = fmt[i];
    }
    s[i] = L'\0';
    return i;
}

int abs(int x)
{
    return x < 0 ? -x : x;
}

long labs(long x)
{
    return x < 0 ? -x : x;
}

long long llabs(long long x)
{
    return x < 0 ? -x : x;
}

void sincos(double x, double *sinx, double *cosx)
{
    if (sinx)
    {
        *sinx = sin(x);
    }
    if (cosx)
    {
        *cosx = cos(x);
    }
}

char *getenv(const char *name)
{
    size_t i;
    size_t nlen;

    if (!name)
    {
        return 0;
    }

    nlen = strlen(name);
    for (i = 0; i < 16 && environ[i]; i++)
    {
        char *entry = environ[i];
        if (strncmp(entry, name, nlen) == 0 && entry[nlen] == '=')
        {
            return &entry[nlen + 1];
        }
    }
    return 0;
}

char *setlocale(int category, const char *locale)
{
    (void)category;

    if (!locale)
    {
        return u_locale_name;
    }

    if (strcmp(locale, "C") != 0 && strcmp(locale, "POSIX") != 0)
    {
        errno = EINVAL;
        return 0;
    }

    strncpy(u_locale_name, locale, sizeof(u_locale_name) - 1U);
    u_locale_name[sizeof(u_locale_name) - 1U] = '\0';
    return u_locale_name;
}

struct lconv *localeconv(void)
{
    memset(&u_locale_conv, 0, sizeof(u_locale_conv));
    u_locale_conv.decimal_point = (char *)".";
    u_locale_conv.thousands_sep = (char *)"";
    u_locale_conv.grouping = (char *)"";
    u_locale_conv.int_curr_symbol = (char *)"";
    u_locale_conv.currency_symbol = (char *)"";
    u_locale_conv.mon_decimal_point = (char *)".";
    u_locale_conv.mon_thousands_sep = (char *)"";
    u_locale_conv.mon_grouping = (char *)"";
    return &u_locale_conv;
}

char *bindtextdomain(const char *domainname, const char *dirname)
{
    (void)domainname;
    return (char *)dirname;
}

char *bind_textdomain_codeset(const char *domainname, const char *codeset)
{
    (void)domainname;
    if (!codeset)
    {
        return u_langinfo_codeset;
    }
    return (char *)codeset;
}

char *textdomain(const char *domainname)
{
    return (char *)(domainname ? domainname : "messages");
}

char *dgettext(const char *domainname, const char *msgid)
{
    (void)domainname;
    return (char *)msgid;
}

char *dcgettext(const char *domainname, const char *msgid, int category)
{
    (void)category;
    return dgettext(domainname, msgid);
}

char *gettext(const char *msgid)
{
    return dgettext(0, msgid);
}

char *nl_langinfo(nl_item item)
{
    /*
     * CPython 启动时最常查询的是 CODESET。
     * 先返回 UTF-8，避免它把整个解释器当成 ASCII 环境。
     * 其它条目暂时返回空串即可。
     */
    if (item == CODESET)
    {
        return u_langinfo_codeset;
    }
    return (char *)"";
}

static void u_init_passwd_entry(struct passwd *pw,
                               char *name,
                               char *passwd,
                               uid_t uid,
                               gid_t gid,
                               char *dir,
                               char *shell)
{
    pw->pw_name = name;
    pw->pw_passwd = passwd;
    pw->pw_uid = uid;
    pw->pw_gid = gid;
    pw->pw_gecos = (char *)"";
    pw->pw_dir = dir;
    pw->pw_shell = shell;
}

struct passwd *getpwuid(uid_t uid)
{
    if (uid == 0)
    {
        u_init_passwd_entry(&u_passwd_root,
                            u_passwd_root_name,
                            u_passwd_root_passwd,
                            0,
                            0,
                            u_passwd_root_dir,
                            u_passwd_root_shell);
        return &u_passwd_root;
    }

    u_init_passwd_entry(&u_passwd_user,
                        u_passwd_user_name,
                        u_passwd_user_passwd,
                        uid,
                        1000,
                        u_passwd_user_dir,
                        u_passwd_user_shell);
    return &u_passwd_user;
}

struct passwd *getpwnam(const char *name)
{
    if (!name)
    {
        errno = EINVAL;
        return 0;
    }
    if (strcmp(name, "root") == 0)
    {
        return getpwuid(0);
    }
    if (strcmp(name, "user") == 0)
    {
        return getpwuid(1000);
    }

    errno = ENOENT;
    return 0;
}

static int u_passwd_copy_to_buf(const struct passwd *src, struct passwd *dst, char *buf, size_t buflen)
{
    size_t need;
    char *p;

    if (!src || !dst || !buf)
    {
        return EINVAL;
    }

    need = strlen(src->pw_name) + 1U
           + strlen(src->pw_passwd) + 1U
           + strlen(src->pw_gecos) + 1U
           + strlen(src->pw_dir) + 1U
           + strlen(src->pw_shell) + 1U;
    if (need > buflen)
    {
        return ERANGE;
    }

    p = buf;
    strcpy(p, src->pw_name);
    dst->pw_name = p;
    p += strlen(p) + 1U;

    strcpy(p, src->pw_passwd);
    dst->pw_passwd = p;
    p += strlen(p) + 1U;

    strcpy(p, src->pw_gecos);
    dst->pw_gecos = p;
    p += strlen(p) + 1U;

    strcpy(p, src->pw_dir);
    dst->pw_dir = p;
    p += strlen(p) + 1U;

    strcpy(p, src->pw_shell);
    dst->pw_shell = p;

    dst->pw_uid = src->pw_uid;
    dst->pw_gid = src->pw_gid;
    return 0;
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    struct passwd *found;
    int ret;

    if (!pwd || !buf || !result)
    {
        return EINVAL;
    }

    found = getpwuid(uid);
    if (!found)
    {
        *result = 0;
        return errno ? errno : ENOENT;
    }

    ret = u_passwd_copy_to_buf(found, pwd, buf, buflen);
    if (ret != 0)
    {
        *result = 0;
        return ret;
    }
    *result = pwd;
    return 0;
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    struct passwd *found;
    int ret;

    if (!pwd || !buf || !result)
    {
        return EINVAL;
    }

    found = getpwnam(name);
    if (!found)
    {
        *result = 0;
        return errno ? errno : ENOENT;
    }

    ret = u_passwd_copy_to_buf(found, pwd, buf, buflen);
    if (ret != 0)
    {
        *result = 0;
        return ret;
    }
    *result = pwd;
    return 0;
}

void setpwent(void)
{
    u_passwd_iter_index = 0;
}

void endpwent(void)
{
    u_passwd_iter_index = 0;
}

struct passwd *getpwent(void)
{
    if (u_passwd_iter_index == 0)
    {
        u_passwd_iter_index = 1;
        return getpwuid(0);
    }
    if (u_passwd_iter_index == 1)
    {
        u_passwd_iter_index = 2;
        return getpwuid(1000);
    }
    return 0;
}

struct group *getgrgid(gid_t gid)
{
    if (gid == 0)
    {
        u_group_root.gr_name = (char *)"root";
        u_group_root.gr_passwd = (char *)"x";
        u_group_root.gr_gid = 0;
        u_group_root.gr_mem = u_group_root_members;
        return &u_group_root;
    }

    u_group_user.gr_name = (char *)"user";
    u_group_user.gr_passwd = (char *)"x";
    u_group_user.gr_gid = gid;
    u_group_user.gr_mem = u_group_user_members;
    return &u_group_user;
}

struct group *getgrnam(const char *name)
{
    if (!name)
    {
        errno = EINVAL;
        return 0;
    }
    if (strcmp(name, "root") == 0)
    {
        return getgrgid(0);
    }
    if (strcmp(name, "user") == 0)
    {
        return getgrgid(1000);
    }
    errno = ENOENT;
    return 0;
}

int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups)
{
    (void)user;
    if (!ngroups || *ngroups <= 0)
    {
        return -1;
    }
    if (!groups)
    {
        return -1;
    }

    groups[0] = group;
    *ngroups = 1;
    return 1;
}

DIR *opendir(const char *name)
{
    struct u_dir_stream *ds;

    if (!name)
    {
        errno = EINVAL;
        return 0;
    }

    ds = (struct u_dir_stream *)malloc(sizeof(*ds));
    if (!ds)
    {
        errno = ENOMEM;
        return 0;
    }

    memset(ds, 0, sizeof(*ds));
    strncpy(ds->path, name, sizeof(ds->path) - 1U);
    return (DIR *)ds;
}

struct dirent *readdir(DIR *dirp)
{
    struct u_dir_stream *ds;
    struct stupidos_dirent raw;
    int ret;

    if (!dirp)
    {
        errno = EBADF;
        return 0;
    }

    ds = (struct u_dir_stream *)dirp;
    ret = u_readdir((const int8_t *)ds->path, ds->index, &raw);
    if (ret < 0)
    {
        return 0;
    }

    memset(&u_dirent_entry, 0, sizeof(u_dirent_entry));
    u_dirent_entry.d_ino = raw.ino;
    strncpy(u_dirent_entry.d_name, (const char *)raw.name, sizeof(u_dirent_entry.d_name) - 1U);
    ds->index++;
    return &u_dirent_entry;
}

struct dirent64 *readdir64(DIR *dirp)
{
    /*
     * 64 位目录接口先直接复用 32 位版本的结果。
     * stupidos 当前的 inode/目录条目模型本来就是 64 位友好的，
     * 这里先把符号补齐，避免 CPython 的大文件路径卡住。
     */
    return (struct dirent64 *)readdir(dirp);
}

int closedir(DIR *dirp)
{
    if (!dirp)
    {
        errno = EBADF;
        return -1;
    }

    free(dirp);
    return 0;
}

void rewinddir(DIR *dirp)
{
    if (dirp)
    {
        ((struct u_dir_stream *)dirp)->index = 0;
    }
}

static int u_env_set(const char *name, const char *value, bool overwrite)
{
    size_t i;
    size_t nlen;
    size_t vlen;
    size_t len;
    char *entry;

    if (!name || !value || !name[0] || strchr(name, '='))
    {
        return EINVAL;
    }

    if (!overwrite && getenv(name))
    {
        return 0;
    }

    nlen = strlen(name);
    vlen = strlen(value);
    len = nlen + 1 + vlen + 1;
    entry = (char *)malloc(len);
    if (!entry)
    {
        return ENOMEM;
    }

    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(&entry[nlen + 1], value, vlen);
    entry[len - 1] = '\0';

    for (i = 0; i < 16; i++)
    {
        if (!environ[i] || strstr(environ[i], name) == environ[i])
        {
            environ[i] = entry;
            return 0;
        }
    }

    return ENOSPC;
}

int setenv(const char *name, const char *value, int overwrite)
{
    return u_env_set(name, value, overwrite != 0);
}

int unsetenv(const char *name)
{
    size_t i;
    size_t nlen;

    if (!name)
    {
        return EINVAL;
    }

    nlen = strlen(name);
    for (i = 0; i < 16; i++)
    {
        if (environ[i] && strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=')
        {
            environ[i] = 0;
        }
    }

    return 0;
}

int putenv(char *string)
{
    char *eq;

    if (!string)
    {
        return EINVAL;
    }

    eq = strchr(string, '=');
    if (!eq)
    {
        return EINVAL;
    }

    *eq = '\0';
    return u_env_set(string, eq + 1, true);
}

int getpid(void)
{
    return u_getpid();
}

int getppid(void)
{
    return u_getppid();
}

char *getlogin(void)
{
    return (char *)"user";
}

int getloadavg(double loadavg[], int nelem)
{
    int i;

    if (!loadavg || nelem <= 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (nelem > 3)
    {
        nelem = 3;
    }
    for (i = 0; i < nelem; i++)
    {
        loadavg[i] = 0.0;
    }
    return nelem;
}

pid_t gettid(void)
{
    return (pid_t)u_sysret_int((int64_t)u_gettid());
}

int getuid(void)
{
    return u_getuid();
}

int getgid(void)
{
    return u_getgid();
}

int geteuid(void)
{
    return u_geteuid();
}

int getegid(void)
{
    return u_getegid();
}

int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask)
{
    return u_sysret_int((int64_t)u_sched_getaffinity((int)pid, cpusetsize, mask));
}

int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask)
{
    (void)pid;
    (void)cpusetsize;
    (void)mask;
    errno = ENOSYS;
    return -1;
}

int sched_get_priority_max(int policy)
{
    (void)policy;
    return 0;
}

int sched_get_priority_min(int policy)
{
    (void)policy;
    return 0;
}

int sched_getscheduler(pid_t pid)
{
    (void)pid;
    return SCHED_OTHER;
}

int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param)
{
    (void)pid;
    (void)policy;
    (void)param;
    errno = ENOSYS;
    return -1;
}

int sched_getparam(pid_t pid, struct sched_param *param)
{
    (void)pid;
    if (param)
    {
        param->sched_priority = 0;
    }
    return 0;
}

int sched_setparam(pid_t pid, const struct sched_param *param)
{
    (void)pid;
    (void)param;
    errno = ENOSYS;
    return -1;
}

int sched_rr_get_interval(pid_t pid, struct timespec *tp)
{
    (void)pid;
    if (!tp)
    {
        errno = EINVAL;
        return -1;
    }
    tp->tv_sec = 0;
    tp->tv_nsec = 0;
    return 0;
}

int sched_yield(void)
{
    return 0;
}

int sysinfo(struct sysinfo *info)
{
    return u_sysret_int((int64_t)u_sysinfo((struct stupidos_sysinfo *)info));
}

int setuid(uid_t uid)
{
    /*
     * stupidos 当前还没有完整的权限模型。
     * 为了让 Python / POSIX 用户态尽量继续跑，
     * 这里先只允许“设置成当前 uid”的无害操作。
     */
    if (uid != (uid_t)u_getuid())
    {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int setgid(gid_t gid)
{
    if (gid != (gid_t)u_getgid())
    {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int seteuid(uid_t uid)
{
    return setuid(uid);
}

int setegid(gid_t gid)
{
    return setgid(gid);
}

mode_t umask(mode_t mask)
{
    mode_t old;

    old = u_umask;
    u_umask = mask;
    return old;
}

int setreuid(uid_t ruid, uid_t euid)
{
    if ((ruid != (uid_t)-1 && ruid != (uid_t)u_getuid())
        || (euid != (uid_t)-1 && euid != (uid_t)u_getuid()))
    {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int setregid(gid_t rgid, gid_t egid)
{
    if ((rgid != (gid_t)-1 && rgid != (gid_t)u_getgid())
        || (egid != (gid_t)-1 && egid != (gid_t)u_getgid()))
    {
        errno = EPERM;
        return -1;
    }
    return 0;
}

pid_t getpgrp(void)
{
    return 1;
}

pid_t getpgid(pid_t pid)
{
    (void)pid;
    return 1;
}

int setpgid(pid_t pid, pid_t pgid)
{
    (void)pid;
    (void)pgid;
    return 0;
}

pid_t setsid(void)
{
    return 1;
}

pid_t getsid(pid_t pid)
{
    (void)pid;
    return 1;
}

int setpgrp(void)
{
    return setpgid(0, 0);
}

int getpriority(int which, id_t who)
{
    (void)which;
    (void)who;
    return 0;
}

int setpriority(int which, id_t who, int prio)
{
    (void)which;
    (void)who;
    (void)prio;
    return 0;
}

int getresuid(uid_t *ruid, uid_t *euid, uid_t *suid)
{
    if (ruid)
    {
        *ruid = (uid_t)u_getuid();
    }
    if (euid)
    {
        *euid = (uid_t)u_geteuid();
    }
    if (suid)
    {
        *suid = (uid_t)u_getuid();
    }
    return 0;
}

int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid)
{
    if (rgid)
    {
        *rgid = (gid_t)u_getgid();
    }
    if (egid)
    {
        *egid = (gid_t)u_getegid();
    }
    if (sgid)
    {
        *sgid = (gid_t)u_getgid();
    }
    return 0;
}

int setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
    (void)suid;
    return setreuid(ruid, euid);
}

int setresgid(gid_t rgid, gid_t egid, gid_t sgid)
{
    (void)sgid;
    return setregid(rgid, egid);
}

int nice(int inc)
{
    (void)inc;
    return 0;
}

int getgroups(int size, gid_t list[])
{
    if (size < 0)
    {
        errno = EINVAL;
        return -1;
    }

    /*
     * 先按“只有一个默认组”来模拟：
     * - size == 0 时返回组数
     * - 否则把当前 gid 当作唯一组返回
     */
    if (size == 0)
    {
        return 1;
    }
    if (!list || size < 1)
    {
        errno = ERANGE;
        return -1;
    }

    list[0] = (gid_t)u_getgid();
    return 1;
}

int setgroups(size_t size, const gid_t *list)
{
    (void)list;

    /*
     * 当前不维护真实的 supplementary groups。
     * 这里先允许调用通过，保证上层初始化流程能继续走。
     */
    if (size > 0 && size > 1)
    {
        /* 暂时只接受单组模型。 */
        return 0;
    }
    return 0;
}

int initgroups(const char *user, gid_t group)
{
    (void)user;
    (void)group;
    return 0;
}

int getrusage(int who, struct rusage *usage)
{
    (void)who;

    if (!usage)
    {
        errno = EINVAL;
        return -1;
    }

    /*
     * 目前没有完善的进程级 CPU / 内存统计，因此先返回全 0。
     * 这足以支撑 Python 的时间模块和大部分只读探测逻辑。
     */
    memset(usage, 0, sizeof(*usage));
    return 0;
}

clock_t times(struct tms *buffer)
{
    if (buffer)
    {
        memset(buffer, 0, sizeof(*buffer));
    }

    /*
     * 这里返回一个单调递增的“tick”值，先满足 os.times()/timemodule 的需要。
     * 后续接入真实进程调度统计后，再换成内核态的 CPU 计数。
     */
    return clock();
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    return u_sysret_int((int64_t)u_gettimeofday((struct stupidos_timeval *)tv));
}

int isatty(int fd)
{
    return u_isatty(fd);
}

pid_t tcgetpgrp(int fd)
{
    (void)fd;
    return (pid_t)1;
}

int tcsetpgrp(int fd, pid_t pgrp)
{
    (void)fd;
    (void)pgrp;
    return 0;
}

char *ttyname(int fd)
{
    static char u_ttyname_buf[] = "/dev/tty";

    if (!u_isatty(fd))
    {
        errno = ENOTTY;
        return 0;
    }

    return u_ttyname_buf;
}

int ttyname_r(int fd, char *buf, size_t buflen)
{
    const char *name = ttyname(fd);
    size_t len;

    if (!name || !buf || !buflen)
    {
        return ENOTTY;
    }

    len = strlen(name) + 1U;
    if (len > buflen)
    {
        return ERANGE;
    }

    memcpy(buf, name, len);
    return 0;
}

int access(const char *path, int mode)
{
    return u_sysret_int((int64_t)u_access((const int8_t *)path, mode));
}

int faccessat(int dirfd, const char *path, int mode, int flags)
{
    if (flags != 0)
    {
        errno = ENOTSUP;
        return -1;
    }

    if (dirfd != AT_FDCWD)
    {
        errno = ENOTSUP;
        return -1;
    }

    return access(path, mode);
}

int chmod(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    return u_errno_rofs();
}

int fchmod(int fd, mode_t mode)
{
    (void)fd;
    (void)mode;
    return u_errno_rofs();
}

int chown(const char *path, uid_t owner, gid_t group)
{
    (void)path;
    (void)owner;
    (void)group;
    return u_errno_rofs();
}

int fchown(int fd, uid_t owner, gid_t group)
{
    (void)fd;
    (void)owner;
    (void)group;
    return u_errno_rofs();
}

int link(const char *oldpath, const char *newpath)
{
    return u_sysret_int((int64_t)u_link((const int8_t *)oldpath, (const int8_t *)newpath));
}

int symlink(const char *oldpath, const char *newpath)
{
    return u_sysret_int((int64_t)u_symlink((const int8_t *)oldpath, (const int8_t *)newpath));
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    return u_sysret_ssize((int64_t)u_readlink((const int8_t *)path, (int8_t *)buf, bufsiz));
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz)
{
    char resolved[STUPIDOS_PATH_MAX];

    if (u_resolve_at_path(dirfd, path, resolved, sizeof(resolved)) != 0)
    {
        return -1;
    }

    return readlink(resolved, buf, bufsiz);
}

int mkdir(const char *path, mode_t mode)
{
    return u_sysret_int((int64_t)u_mkdir((const int8_t *)path, (uint32_t)mode));
}

int rmdir(const char *path)
{
    return u_sysret_int((int64_t)u_rmdir((const int8_t *)path));
}

int unlink(const char *path)
{
    return u_sysret_int((int64_t)u_unlink((const int8_t *)path));
}

int rename(const char *oldpath, const char *newpath)
{
    return u_sysret_int((int64_t)u_rename((const int8_t *)oldpath, (const int8_t *)newpath));
}

int mkdirat(int dirfd, const char *path, mode_t mode)
{
    char resolved[STUPIDOS_PATH_MAX];

    if (u_resolve_at_path(dirfd, path, resolved, sizeof(resolved)) != 0)
    {
        return -1;
    }

    return mkdir(resolved, mode);
}

int unlinkat(int dirfd, const char *path, int flags)
{
    char resolved[STUPIDOS_PATH_MAX];

    if (u_resolve_at_path(dirfd, path, resolved, sizeof(resolved)) != 0)
    {
        return -1;
    }

#ifdef AT_REMOVEDIR
    if (flags & AT_REMOVEDIR)
    {
        return rmdir(resolved);
    }
#endif

    return unlink(resolved);
}

int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
{
    char old_resolved[STUPIDOS_PATH_MAX];
    char new_resolved[STUPIDOS_PATH_MAX];

    if (u_resolve_at_path(olddirfd, oldpath, old_resolved, sizeof(old_resolved)) != 0)
    {
        return -1;
    }

    if (u_resolve_at_path(newdirfd, newpath, new_resolved, sizeof(new_resolved)) != 0)
    {
        return -1;
    }

    return rename(old_resolved, new_resolved);
}

int renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned int flags)
{
    if (flags != 0U)
    {
        errno = ENOTSUP;
        return -1;
    }

    return renameat(olddirfd, oldpath, newdirfd, newpath);
}

int chflags(const char *path, unsigned long flags)
{
    (void)path;
    (void)flags;
    errno = ENOTSUP;
    return -1;
}

int lchflags(const char *path, unsigned long flags)
{
    return chflags(path, flags);
}

char *ctermid(char *s)
{
    static char u_ctermid_default[] = "/dev/tty";

    if (!s)
    {
        return u_ctermid_default;
    }
    strcpy(s, u_ctermid_default);
    return s;
}

int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags)
{
    (void)flags;
    char old_resolved[STUPIDOS_PATH_MAX];
    char new_resolved[STUPIDOS_PATH_MAX];

    if (u_resolve_at_path(olddirfd, oldpath, old_resolved, sizeof(old_resolved)) != 0)
    {
        return -1;
    }

    if (u_resolve_at_path(newdirfd, newpath, new_resolved, sizeof(new_resolved)) != 0)
    {
        return -1;
    }

    return link(old_resolved, new_resolved);
}

int symlinkat(const char *target, int newdirfd, const char *linkpath)
{
    char resolved[STUPIDOS_PATH_MAX];

    if (u_resolve_at_path(newdirfd, linkpath, resolved, sizeof(resolved)) != 0)
    {
        return -1;
    }
    return symlink(target, resolved);
}

int lchown(const char *path, uid_t owner, gid_t group)
{
    return chown(path, owner, group);
}

int mkfifo(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    return u_errno_rofs();
}

int mkfifoat(int dirfd, const char *path, mode_t mode)
{
    char resolved[STUPIDOS_PATH_MAX];

    if (u_resolve_at_path(dirfd, path, resolved, sizeof(resolved)) != 0)
    {
        return -1;
    }
    return mkfifo(resolved, mode);
}

int futimens(int fd, const struct timespec times[2])
{
    (void)fd;
    (void)times;
    return 0;
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    va_list ap;

    if (flags & O_CREAT)
    {
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return openat(dirfd, path, flags, mode);
    }
    return openat(dirfd, path, flags);
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
    char resolved[STUPIDOS_PATH_MAX];

    if (flags != 0)
    {
        errno = ENOTSUP;
        return -1;
    }

    if (u_resolve_at_path(dirfd, path, resolved, sizeof(resolved)) != 0)
    {
        return -1;
    }

    return chmod(resolved, mode);
}

int fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flags)
{
    char resolved[STUPIDOS_PATH_MAX];

    if (flags != 0)
    {
        errno = ENOTSUP;
        return -1;
    }

    if (u_resolve_at_path(dirfd, path, resolved, sizeof(resolved)) != 0)
    {
        return -1;
    }

    return chown(resolved, owner, group);
}

int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
    return u_sysret_int((int64_t)u_utimensat(dirfd, (const int8_t *)path,
                                              (const struct stupidos_timespec *)times, flags));
}

int truncate(const char *path, off_t length)
{
    if (length < 0)
    {
        errno = EINVAL;
        return -1;
    }

    return u_sysret_int((int64_t)u_truncate((const int8_t *)path, (uint64_t)length));
}

int truncate64(const char *path, off_t length)
{
    return truncate(path, length);
}

int ftruncate(int fd, off_t length)
{
    if (length < 0)
    {
        errno = EINVAL;
        return -1;
    }

    return u_sysret_int((int64_t)u_ftruncate(fd, (uint64_t)length));
}

int ftruncate64(int fd, off_t length)
{
    return ftruncate(fd, length);
}

int utime(const char *filename, const struct utimbuf *times)
{
    struct timespec ts[2];

    if (!times)
    {
        return utimensat(AT_FDCWD, filename, 0, 0);
    }

    ts[0].tv_sec = times->actime;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = times->modtime;
    ts[1].tv_nsec = 0;
    return utimensat(AT_FDCWD, filename, ts, 0);
}

ssize_t getxattr(const char *path, const char *name, void *value, size_t size)
{
    (void)path;
    (void)name;
    (void)value;
    (void)size;
    return u_errno_notsup_ssize();
}

ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size)
{
    return getxattr(path, name, value, size);
}

ssize_t fgetxattr(int fd, const char *name, void *value, size_t size)
{
    (void)fd;
    return getxattr(0, name, value, size);
}

ssize_t listxattr(const char *path, char *list, size_t size)
{
    (void)path;
    (void)list;
    (void)size;
    return u_errno_notsup_ssize();
}

ssize_t llistxattr(const char *path, char *list, size_t size)
{
    return listxattr(path, list, size);
}

ssize_t flistxattr(int fd, char *list, size_t size)
{
    (void)fd;
    return listxattr(0, list, size);
}

int setxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
    (void)path;
    (void)name;
    (void)value;
    (void)size;
    (void)flags;
    return u_errno_notsup();
}

int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
    return setxattr(path, name, value, size, flags);
}

int fsetxattr(int fd, const char *name, const void *value, size_t size, int flags)
{
    (void)fd;
    return setxattr(0, name, value, size, flags);
}

int memfd_create(const char *name, unsigned int flags)
{
    (void)name;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int removexattr(const char *path, const char *name)
{
    (void)path;
    (void)name;
    return u_errno_notsup();
}

int lremovexattr(const char *path, const char *name)
{
    return removexattr(path, name);
}

int fremovexattr(int fd, const char *name)
{
    (void)fd;
    return removexattr(0, name);
}

static int u_translate_open_flags(int flags)
{
    int out;

    /* 用户态使用宿主/POSIX 语义，内核侧仍沿用 stupidos 自己的标志位。 */
    switch (flags & O_ACCMODE)
    {
    case O_WRONLY:
        out = STUPIDOS_O_WRONLY;
        break;
    case O_RDWR:
        out = STUPIDOS_O_RDWR;
        break;
    case O_RDONLY:
    default:
        out = STUPIDOS_O_RDONLY;
        break;
    }

    if (flags & O_CREAT)
    {
        out |= STUPIDOS_O_CREAT;
    }
    if (flags & O_TRUNC)
    {
        out |= STUPIDOS_O_TRUNC;
    }
    if (flags & O_APPEND)
    {
        out |= STUPIDOS_O_APPEND;
    }
    return out;
}

int open(const char *path, int flags, ...)
{
    va_list ap;
    int mode;

    if (flags & O_CREAT)
    {
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        (void)mode;
    }

    return u_sysret_int((int64_t)u_open((const int8_t *)path, u_translate_open_flags(flags)));
}

int open64(const char *path, int flags, ...)
{
    va_list ap;
    int mode;

    if (flags & O_CREAT)
    {
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        (void)mode;
    }

    return u_sysret_int((int64_t)u_open((const int8_t *)path, u_translate_open_flags(flags)));
}

int openat(int dirfd, const char *path, int flags, ...)
{
    va_list ap;
    int mode;

    if (flags & O_CREAT)
    {
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        (void)mode;
    }

    return u_sysret_int((int64_t)u_openat(dirfd, (const int8_t *)path, u_translate_open_flags(flags)));
}

int close(int fd)
{
    bool is_write_end;

    if (u_pipe_slot_from_fd(fd, &is_write_end))
    {
        return u_pipe_close(fd);
    }
    return u_sysret_int((int64_t)u_close(fd));
}

ssize_t read(int fd, void *buf, size_t len)
{
    bool is_write_end;

    if (u_pipe_slot_from_fd(fd, &is_write_end))
    {
        return u_pipe_read(fd, buf, len);
    }
    return u_sysret_ssize((int64_t)u_read(fd, buf, len));
}

ssize_t write(int fd, const void *buf, size_t len)
{
    bool is_write_end;

    if (u_pipe_slot_from_fd(fd, &is_write_end))
    {
        return u_pipe_write(fd, buf, len);
    }
    return u_sysret_ssize((int64_t)u_write(fd, buf, len));
}

off_t lseek(int fd, off_t offset, int whence)
{
    return u_sysret_off((int64_t)u_lseek(fd, offset, whence));
}

off_t lseek64(int fd, off_t offset, int whence)
{
    return u_sysret_off((int64_t)u_lseek(fd, offset, whence));
}

static void u_fill_stat(struct stat *out, const struct stupidos_stat *kst)
{
    if (!out || !kst)
    {
        return;
    }

    memset((int8_t *)out, 0, sizeof(*out));
    out->st_dev = 0;
    out->st_ino = (ino_t)kst->ino;
    out->st_mode = (mode_t)kst->mode;
    out->st_nlink = (nlink_t)kst->nlink;
    out->st_uid = (uid_t)kst->uid;
    out->st_gid = (gid_t)kst->gid;
    out->st_rdev = 0;
    out->st_size = (off_t)kst->size;
    out->st_blksize = (blksize_t)(kst->blksize ? kst->blksize : 4096U);
    out->st_blocks = (blkcnt_t)kst->blocks;
    out->st_atime = 0;
    out->st_mtime = 0;
    out->st_ctime = 0;
}

int stat(const char *path, struct stat *out)
{
    struct stupidos_stat kst;
    int64_t ret;

    if (!out)
    {
        errno = EINVAL;
        return -1;
    }

    ret = (int64_t)u_stat((const int8_t *)path, &kst);
    if (u_sysret_is_error(ret))
    {
        errno = (int)(-ret);
        return -1;
    }

    u_fill_stat(out, &kst);
    return 0;
}

int fstat(int fd, struct stat *out)
{
    struct stupidos_stat kst;
    int64_t ret;
    bool is_write_end;

    if (!out)
    {
        errno = EINVAL;
        return -1;
    }

    if (u_pipe_slot_from_fd(fd, &is_write_end))
    {
        memset((int8_t *)out, 0, sizeof(*out));
        out->st_mode = (mode_t)(S_IFIFO | 0600);
        out->st_nlink = 1;
        out->st_blksize = 4096;
        return 0;
    }

    ret = (int64_t)u_fstat(fd, &kst);
    if (u_sysret_is_error(ret))
    {
        errno = (int)(-ret);
        return -1;
    }

    u_fill_stat(out, &kst);
    return 0;
}

int stat64(const char *path, struct stat64 *out)
{
    struct stupidos_stat kst;
    int64_t ret;

    if (!out)
    {
        errno = EINVAL;
        return -1;
    }

    ret = (int64_t)u_stat((const int8_t *)path, &kst);
    if (u_sysret_is_error(ret))
    {
        errno = (int)(-ret);
        return -1;
    }

    /*
     * 某些头文件组合下 struct stat64 只有前置声明。
     * AArch64 上 stat/stat64 布局一致，这里按 struct stat 填充即可。
     */
    u_fill_stat((struct stat *)out, &kst);
    return 0;
}

int fstat64(int fd, struct stat64 *out)
{
    struct stupidos_stat kst;
    int64_t ret;
    bool is_write_end;

    if (!out)
    {
        errno = EINVAL;
        return -1;
    }

    if (u_pipe_slot_from_fd(fd, &is_write_end))
    {
        memset((int8_t *)out, 0, sizeof(struct stat));
        ((struct stat *)out)->st_mode = (mode_t)(S_IFIFO | 0600);
        ((struct stat *)out)->st_nlink = 1;
        ((struct stat *)out)->st_blksize = 4096;
        return 0;
    }

    ret = (int64_t)u_fstat(fd, &kst);
    if (u_sysret_is_error(ret))
    {
        errno = (int)(-ret);
        return -1;
    }

    u_fill_stat((struct stat *)out, &kst);
    return 0;
}

int fstatat(int dirfd, const char *path, struct stat *out, int flags)
{
    struct stupidos_stat kst;
    int64_t ret;

    if (!out)
    {
        errno = EINVAL;
        return -1;
    }

    ret = (int64_t)u_fstatat(dirfd, (const int8_t *)path, &kst, flags);
    if (u_sysret_is_error(ret))
    {
        errno = (int)(-ret);
        return -1;
    }

    u_fill_stat(out, &kst);
    return 0;
}

int fstatat64(int dirfd, const char *path, struct stat64 *out, int flags)
{
    struct stupidos_stat kst;
    int64_t ret;

    if (!out)
    {
        errno = EINVAL;
        return -1;
    }

    ret = (int64_t)u_fstatat(dirfd, (const int8_t *)path, &kst, flags);
    if (u_sysret_is_error(ret))
    {
        errno = (int)(-ret);
        return -1;
    }

    u_fill_stat((struct stat *)out, &kst);
    return 0;
}

int newfstatat(int dirfd, const char *path, struct stat *out, int flags)
{
    return fstatat(dirfd, path, out, flags);
}

int lstat(const char *path, struct stat *out)
{
    return fstatat(AT_FDCWD, path, out, AT_SYMLINK_NOFOLLOW);
}

ssize_t pread64(int fd, void *buf, size_t len, off_t off)
{
    return u_sysret_ssize((int64_t)u_pread64(fd, buf, len, (uint64_t)off));
}

ssize_t pwrite64(int fd, const void *buf, size_t len, off_t off)
{
    return u_sysret_ssize((int64_t)u_pwrite64(fd, buf, len, (uint64_t)off));
}

int lstat64(const char *path, struct stat64 *out)
{
    return stat64(path, out);
}

int __xstat64(int ver, const char *path, struct stat64 *out)
{
    (void)ver;
    return stat64(path, out);
}

int __fxstat64(int ver, int fd, struct stat64 *out)
{
    (void)ver;
    return fstat64(fd, out);
}

int __lxstat64(int ver, const char *path, struct stat64 *out)
{
    (void)ver;
    return lstat64(path, out);
}

int __fxstatat64(int ver, int dirfd, const char *path, struct stat64 *out, int flags)
{
    (void)ver;
    return fstatat64(dirfd, path, out, flags);
}

int __xmknod(int ver, const char *path, mode_t mode, dev_t *dev)
{
    (void)ver;
    (void)path;
    (void)mode;
    (void)dev;
    return u_errno_rofs();
}

int __xmknodat(int ver, int dirfd, const char *path, mode_t mode, dev_t *dev)
{
    (void)ver;
    (void)dirfd;
    (void)path;
    (void)mode;
    (void)dev;
    return u_errno_rofs();
}

int statvfs64(const char *path, struct statvfs *buf)
{
    (void)path;
    if (!buf)
    {
        errno = EINVAL;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    return 0;
}

int fstatvfs64(int fd, struct statvfs *buf)
{
    (void)fd;
    return statvfs64("/", buf);
}

int chdir(const char *path)
{
    return u_sysret_int((int64_t)u_chdir((const int8_t *)path));
}

int fchdir(int fd)
{
    (void)fd;
    errno = ENOSYS;
    return -1;
}

int chroot(const char *path)
{
    /*
     * Python 的 posixmodule 会探测 chroot 符号，即使运行时未必真的调用。
     * 这里先返回 ENOSYS，保持链接可通过，后续如果需要容器/隔离能力，
     * 再把它接到真正的内核路径切换语义上。
     */
    (void)path;
    errno = ENOSYS;
    return -1;
}

int uname(struct utsname *buf)
{
    struct stupidos_utsname uts;
    int ret;

    if (!buf)
    {
        errno = EINVAL;
        return -1;
    }

    ret = u_uname(&uts);
    if (ret < 0)
    {
        errno = -ret;
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    strncpy(buf->sysname, (const char *)uts.sysname, sizeof(buf->sysname) - 1U);
    strncpy(buf->nodename, (const char *)uts.nodename, sizeof(buf->nodename) - 1U);
    strncpy(buf->release, (const char *)uts.release, sizeof(buf->release) - 1U);
    strncpy(buf->version, (const char *)uts.version, sizeof(buf->version) - 1U);
    strncpy(buf->machine, (const char *)uts.machine, sizeof(buf->machine) - 1U);
    return 0;
}

char *getcwd(char *buf, size_t len)
{
    bool alloced;
    size_t use_len;

    alloced = false;
    use_len = len;
    if (!buf)
    {
        /*
         * 对齐 POSIX 常见用法：getcwd(NULL, 0) 由 libc 分配缓冲。
         * 构建工具链/解释器常使用该模式探测当前路径。
         */
        if (use_len == 0)
        {
            use_len = STUPIDOS_PATH_MAX;
        }

        buf = (char *)malloc(use_len);
        if (!buf)
        {
            errno = ENOMEM;
            return 0;
        }
        alloced = true;
    }
    else if (use_len == 0)
    {
        errno = EINVAL;
        return 0;
    }

    if (u_sysret_int((int64_t)u_getcwd((int8_t *)buf, use_len)) < 0)
    {
        if (alloced)
        {
            free(buf);
        }
        return 0;
    }

    return buf;
}

int clock_gettime(clockid_t clockid, struct timespec *out)
{
    return u_sysret_int((int64_t)u_clock_gettime((int)clockid, (struct stupidos_timespec *)out));
}

int clock_settime(clockid_t clockid, const struct timespec *tp)
{
    (void)clockid;
    (void)tp;
    errno = ENOSYS;
    return -1;
}

int clock_getres(clockid_t clockid, struct timespec *out)
{
    (void)clockid;

    if (!out)
    {
        errno = EINVAL;
        return -1;
    }

    /*
     * 先把时钟分辨率固定成纳秒级，满足 CPython 的时间探测逻辑。
     * 后续如果内核定时器精度变化，这里再同步调整。
     */
    out->tv_sec = 0;
    out->tv_nsec = 1;
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    return u_sysret_int((int64_t)u_nanosleep((const struct stupidos_timespec *)req, (struct stupidos_timespec *)rem));
}

static int64_t u_days_from_civil(int64_t year, unsigned month, unsigned day)
{
    int64_t era;
    int64_t yoe;
    int64_t doy;
    int64_t doe;

    /*
     * 把公历日期转换成从 1970-01-01 起算的天数。
     * 这个公式只依赖整数运算，适合在没有完整 libc 的用户态里使用。
     */
    year -= (month <= 2U);
    era = (year >= 0) ? year / 400 : (year - 399) / 400;
    yoe = year - era * 400;
    doy = (153 * (month + (month > 2U ? -3 : 9)) + 2) / 5 + (int64_t)day - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

clock_t clock(void)
{
    struct timespec ts;
    uint64_t ticks;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
        return (clock_t)-1;
    }

    ticks = (uint64_t)ts.tv_sec * (uint64_t)CLOCKS_PER_SEC;
    ticks += ((uint64_t)ts.tv_nsec * (uint64_t)CLOCKS_PER_SEC) / 1000000000ULL;
    return (clock_t)ticks;
}

time_t timegm(struct tm *tm)
{
    int64_t days;
    int64_t secs;

    if (!tm)
    {
        errno = EINVAL;
        return (time_t)-1;
    }

    days = u_days_from_civil((int64_t)tm->tm_year + 1900LL,
                             (unsigned)(tm->tm_mon + 1),
                             (unsigned)tm->tm_mday);
    secs = days * 86400LL
         + (int64_t)tm->tm_hour * 3600LL
         + (int64_t)tm->tm_min * 60LL
         + (int64_t)tm->tm_sec;
    return (time_t)secs;
}

time_t mktime(struct tm *tm)
{
    return timegm(tm);
}

static void u_civil_from_days(int64_t z, int64_t *year, unsigned *month, unsigned *day)
{
    int64_t era;
    int64_t doe;
    int64_t yoe;
    int64_t y;
    int64_t doy;
    int64_t mp;

    z += 719468;
    era = (z >= 0) ? z / 146097 : (z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *day = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    *month = (unsigned)(mp + (mp < 10 ? 3 : -9));
    *year = y + (*month <= 2);
}

struct tm *gmtime_r(const time_t *timep, struct tm *result)
{
    int64_t secs;
    int64_t days;
    int64_t year;
    unsigned month;
    unsigned day;

    if (!timep || !result)
    {
        errno = EINVAL;
        return 0;
    }

    secs = (int64_t)*timep;
    days = secs / 86400LL;
    secs %= 86400LL;
    if (secs < 0)
    {
        secs += 86400LL;
        days -= 1LL;
    }

    u_civil_from_days(days, &year, &month, &day);
    memset(result, 0, sizeof(*result));
    result->tm_year = (int)year - 1900;
    result->tm_mon = (int)month - 1;
    result->tm_mday = (int)day;
    result->tm_hour = (int)(secs / 3600LL);
    result->tm_min = (int)((secs % 3600LL) / 60LL);
    result->tm_sec = (int)(secs % 60LL);
    result->tm_wday = (int)((days + 4LL) % 7LL);
    if (result->tm_wday < 0)
    {
        result->tm_wday += 7;
    }
    result->tm_yday = (int)(days - u_days_from_civil(year, month, day));
    result->tm_isdst = 0;
    return result;
}

struct tm *localtime_r(const time_t *timep, struct tm *result)
{
    /*
     * stupidos 目前先把本地时间视为 UTC，避免引入时区数据库依赖。
     * 之后再接入真实时区和 RTC 同步。
     */
    return gmtime_r(timep, result);
}

unsigned int sleep(unsigned int seconds)
{
    struct stupidos_timespec ts;

    ts.tv_sec = seconds;
    ts.tv_nsec = 0;
    (void)nanosleep((const struct timespec *)&ts, 0);
    return 0;
}

time_t time(time_t *tloc)
{
    time_t now;

    now = (time_t)u_time();
    if (tloc)
    {
        *tloc = now;
    }
    return now;
}

ssize_t getrandom(void *buf, size_t len, unsigned int flags)
{
    int64_t ret;

    ret = (int64_t)u_getrandom(buf, len, flags);
    if (u_sysret_is_error(ret))
    {
        errno = (int)(-ret);
        return -1;
    }

    return (ssize_t)ret;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    int64_t ret;

    ret = (int64_t)u_mmap(addr, len, prot, flags, fd, off);
    if (u_sysret_is_error(ret))
    {
        errno = (int)(-ret);
        return (void *)-1;
    }

    return (void *)(uint64_t)ret;
}

void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off64_t off)
{
    return mmap(addr, len, prot, flags, fd, (off_t)off);
}

void *memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p;
    unsigned char ch;

    if (!s || n == 0)
    {
        return 0;
    }

    p = (const unsigned char *)s + n;
    ch = (unsigned char)c;
    while (n--)
    {
        p--;
        if (*p == ch)
        {
            return (void *)p;
        }
    }
    return 0;
}

int sem_init(sem_t *sem, int pshared, unsigned int value)
{
    struct u_sem_slot *slot;

    (void)pshared;
    if (!sem)
    {
        errno = EINVAL;
        return -1;
    }

    slot = u_sem_slot_find(sem);
    if (!slot)
    {
        slot = u_sem_slot_alloc(sem);
    }
    if (!slot)
    {
        errno = ENOMEM;
        return -1;
    }

    slot->value = (int)value;
    return 0;
}

int sem_destroy(sem_t *sem)
{
    struct u_sem_slot *slot;

    if (!sem)
    {
        errno = EINVAL;
        return -1;
    }

    slot = u_sem_slot_find(sem);
    if (!slot)
    {
        errno = EINVAL;
        return -1;
    }
    slot->used = false;
    slot->sem = 0;
    slot->value = 0;
    return 0;
}

int sem_post(sem_t *sem)
{
    struct u_sem_slot *slot;

    slot = u_sem_slot_find(sem);
    if (!slot)
    {
        errno = EINVAL;
        return -1;
    }
    slot->value++;
    return 0;
}

int sem_trywait(sem_t *sem)
{
    struct u_sem_slot *slot;

    slot = u_sem_slot_find(sem);
    if (!slot)
    {
        errno = EINVAL;
        return -1;
    }
    if (slot->value <= 0)
    {
        errno = EAGAIN;
        return -1;
    }
    slot->value--;
    return 0;
}

int sem_wait(sem_t *sem)
{
    struct u_sem_slot *slot;

    slot = u_sem_slot_find(sem);
    if (!slot)
    {
        errno = EINVAL;
        return -1;
    }
    while (slot->value <= 0)
    {
        (void)u_yield();
    }
    slot->value--;
    return 0;
}

int sem_clockwait(sem_t *sem, clockid_t clockid, const struct timespec *abstime)
{
    (void)clockid;
    (void)abstime;
    return sem_wait(sem);
}

int munmap(void *addr, size_t len)
{
    return u_sysret_int((int64_t)u_munmap(addr, len));
}

int mprotect(void *addr, size_t len, int prot)
{
    return u_sysret_int((int64_t)u_mprotect(addr, len, prot));
}

int fsync(int fd)
{
    /*
     * stupidos 当前的文件系统/块设备层还没有对“刷盘”做真实区分，
     * 这里先返回成功，避免 Python 的 os.fsync() 以及相关库直接失败。
     * 以后接入真正的持久化存储后，再把这里改成真实同步。
     */
    (void)fd;
    return 0;
}

void sync(void)
{
}

int fdatasync(int fd)
{
    (void)fd;
    return 0;
}

static void u_timespec_from_ms(struct stupidos_timespec *ts, uint32_t ms)
{
    if (!ts)
    {
        return;
    }

    ts->tv_sec = (int64_t)(ms / 1000U);
    ts->tv_nsec = (int64_t)(ms % 1000U) * 1000000LL;
}

static int u_select_sleep_slice(void)
{
    struct stupidos_timespec ts;

    /*
     * 只睡一个很小的时间片，避免把 select/poll 变成“长时间盯死”的轮询。
     * 这样 stdin、socket、pty 这类输入路径都能更快被重新检查到。
     */
    u_timespec_from_ms(&ts, 1U);
    return (int)u_nanosleep(&ts, 0);
}

static int u_fd_read_ready(int fd)
{
    int pending;
    struct u_pipe_slot *pipe_slot;
    bool is_write_end;

    if (fd < 0)
    {
        return 0;
    }

    pipe_slot = u_pipe_slot_from_fd(fd, &is_write_end);
    if (pipe_slot)
    {
        if (is_write_end)
        {
            return 0;
        }
        return (pipe_slot->used_bytes > 0) || !pipe_slot->write_open;
    }

    /*
     * 读取就绪统一走 FIONREAD 探测：
     * - tty / stdin：pending > 0 时立刻返回
     * - socket：后面 socket ioctl 会返回接收队列长度
     * - 普通文件：ioctl 失败后按“总是可读”处理
     */
    pending = 0;
    if (ioctl(fd, FIONREAD, &pending) == 0)
    {
        return pending > 0;
    }

    return 1;
}

static int u_fd_write_ready(int fd)
{
    struct u_pipe_slot *pipe_slot;
    bool is_write_end;
    int so_error;
    unsigned int so_len;

    if (fd < 0)
    {
        return 0;
    }

    pipe_slot = u_pipe_slot_from_fd(fd, &is_write_end);
    if (pipe_slot)
    {
        if (!is_write_end)
        {
            return 0;
        }
        return pipe_slot->read_open && pipe_slot->used_bytes < U_PIPE_BUF_SIZE;
    }

    /*
     * 对非 stdin 的真实 fd 先按“通常可写”处理。
     * 这里对 socket 额外询问 SO_ERROR：
     * - EINPROGRESS 说明 connect 还没完成，先别把它当成可写
     * - 其他值（包括 0）都交还给上层，让 dropbear 走自己的处理逻辑
     *
     * 这样 SSH 的 nonblocking connect 就不会在“连接未完成”时被
     * select/poll 误判为可写，从而出现静默等待或者假连接。
     */
    if (fd == 0)
    {
        return 0;
    }

    so_error = 0;
    so_len = sizeof(so_error);
    if (u_getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) == 0)
    {
        return so_error != EINPROGRESS;
    }

    return 1;
}

static int u_fd_except_ready(int fd)
{
    (void)fd;
    return 0;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
    int ready;
    int fd;
    struct timeval deadline;
    struct timeval now;
    struct timeval rem;
    int have_timeout;
    fd_set read_orig;
    fd_set write_orig;
    fd_set except_orig;
    fd_set read_work;
    fd_set write_work;
    fd_set except_work;

    if (nfds < 0)
    {
        errno = EINVAL;
        return -1;
    }

    have_timeout = timeout != 0;
    if (have_timeout)
    {
        if (timeout->tv_sec < 0 || timeout->tv_usec < 0)
        {
            errno = EINVAL;
            return -1;
        }

        if (u_gettimeofday((struct stupidos_timeval *)&now) < 0)
        {
            now.tv_sec = 0;
            now.tv_usec = 0;
        }
        deadline.tv_sec = now.tv_sec + timeout->tv_sec;
        deadline.tv_usec = now.tv_usec + timeout->tv_usec;
        if (deadline.tv_usec >= 1000000)
        {
            deadline.tv_sec += deadline.tv_usec / 1000000;
            deadline.tv_usec %= 1000000;
        }
    }

    if (readfds)
    {
        read_orig = *readfds;
    }
    if (writefds)
    {
        write_orig = *writefds;
    }
    if (exceptfds)
    {
        except_orig = *exceptfds;
    }

    for (;;)
    {
        ready = 0;
        if (readfds)
        {
            read_work = read_orig;
            for (fd = 0; fd < nfds; ++fd)
            {
                if (!FD_ISSET(fd, &read_work))
                {
                    continue;
                }
                if (u_fd_read_ready(fd))
                {
                    ready++;
                }
                else
                {
                    FD_CLR(fd, &read_work);
                }
            }
        }

        if (writefds)
        {
            write_work = write_orig;
            for (fd = 0; fd < nfds; ++fd)
            {
                if (!FD_ISSET(fd, &write_work))
                {
                    continue;
                }
                if (u_fd_write_ready(fd))
                {
                    ready++;
                }
                else
                {
                    FD_CLR(fd, &write_work);
                }
            }
        }

        if (exceptfds)
        {
            except_work = except_orig;
            for (fd = 0; fd < nfds; ++fd)
            {
                if (!FD_ISSET(fd, &except_work))
                {
                    continue;
                }
                if (u_fd_except_ready(fd))
                {
                    ready++;
                }
                else
                {
                    FD_CLR(fd, &except_work);
                }
            }
        }

        if (ready > 0)
        {
            if (readfds)
            {
                *readfds = read_work;
            }
            if (writefds)
            {
                *writefds = write_work;
            }
            if (exceptfds)
            {
                *exceptfds = except_work;
            }
            return ready;
        }

        if (!have_timeout)
        {
            if (u_select_sleep_slice() < 0)
            {
                errno = EINTR;
                return -1;
            }
            continue;
        }

        if (u_gettimeofday((struct stupidos_timeval *)&now) < 0)
        {
            errno = EIO;
            return -1;
        }

        if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_usec >= deadline.tv_usec))
        {
            if (readfds)
            {
                FD_ZERO(readfds);
            }
            if (writefds)
            {
                FD_ZERO(writefds);
            }
            if (exceptfds)
            {
                FD_ZERO(exceptfds);
            }
            return 0;
        }

        rem.tv_sec = deadline.tv_sec - now.tv_sec;
        rem.tv_usec = deadline.tv_usec - now.tv_usec;
        if (rem.tv_usec < 0)
        {
            rem.tv_sec--;
            rem.tv_usec += 1000000;
        }
        if (rem.tv_sec > 0 || rem.tv_usec > 1000)
        {
            rem.tv_sec = 0;
            rem.tv_usec = 1000;
        }

        if (u_nanosleep((const struct stupidos_timespec *)&(struct stupidos_timespec){ .tv_sec = rem.tv_sec, .tv_nsec = rem.tv_usec * 1000LL }, 0) < 0)
        {
            errno = EINTR;
            return -1;
        }
    }
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout, const sigset_t *sigmask)
{
    struct timeval tv;
    struct timeval *tvp;

    (void)sigmask;

    if (timeout)
    {
        tv.tv_sec = (time_t)timeout->tv_sec;
        tv.tv_usec = (suseconds_t)(timeout->tv_nsec / 1000LL);
        tvp = &tv;
    }
    else
    {
        tvp = 0;
    }

    return select(nfds, readfds, writefds, exceptfds, tvp);
}

static void u_termios_init_once(void)
{
    if (u_termios_state_ready)
    {
        return;
    }

    memset((char *)&u_termios_state, 0, sizeof(u_termios_state));
    u_termios_state.c_iflag = ICRNL | IXON;
    u_termios_state.c_oflag = OPOST | ONLCR;
    u_termios_state.c_cflag = CS8 | CREAD;
    u_termios_state.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
    u_termios_state.c_cc[VMIN] = 1;
    u_termios_state.c_cc[VTIME] = 0;
    u_termios_state_ready = true;
}

int tcgetattr(int fd, struct termios *termios_p)
{
    if (!termios_p)
    {
        errno = EINVAL;
        return -1;
    }
    if (!isatty(fd))
    {
        errno = ENOTTY;
        return -1;
    }

    /*
     * 优先向内核同步真实 tty 状态（ECHO/ICANON），
     * 失败时再回退到用户态缓存，保证老路径兼容。
     */
    if (ioctl(fd, TCGETS, termios_p) == 0)
    {
        u_termios_state = *termios_p;
        u_termios_state_ready = true;
        return 0;
    }

    u_termios_init_once();
    *termios_p = u_termios_state;
    return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p)
{
    unsigned long req;

    if (!termios_p)
    {
        errno = EINVAL;
        return -1;
    }
    if (!isatty(fd))
    {
        errno = ENOTTY;
        return -1;
    }

    req = TCSETS;
    if (optional_actions == TCSADRAIN)
    {
        req = TCSETSW;
    }
    else if (optional_actions == TCSAFLUSH)
    {
        req = TCSETSF;
    }

    (void)ioctl(fd, req, (void *)termios_p);
    u_termios_state = *termios_p;
    u_termios_state_ready = true;
    return 0;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    fd_set rfds;
    fd_set wfds;
    fd_set efds;
    struct timeval tv;
    struct timeval *tvp;
    int maxfd;
    int ret;
    nfds_t i;
    int ready;

    if (!fds && nfds)
    {
        errno = EINVAL;
        return -1;
    }

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    maxfd = -1;
    for (i = 0; i < nfds; i++)
    {
        fds[i].revents = 0;
        if (fds[i].fd < 0)
        {
            continue;
        }
        if (fds[i].events & (POLLIN | POLLPRI))
        {
            FD_SET(fds[i].fd, &rfds);
        }
        if (fds[i].events & POLLOUT)
        {
            FD_SET(fds[i].fd, &wfds);
        }
        if (fds[i].events & POLLERR)
        {
            FD_SET(fds[i].fd, &efds);
        }
        if (fds[i].fd > maxfd)
        {
            maxfd = fds[i].fd;
        }
    }

    if (timeout < 0)
    {
        tvp = 0;
    }
    else
    {
        tv.tv_sec = (time_t)(timeout / 1000);
        tv.tv_usec = (suseconds_t)((timeout % 1000) * 1000);
        tvp = &tv;
    }

    ret = select(maxfd + 1, &rfds, &wfds, &efds, tvp);
    if (ret <= 0)
    {
        return ret;
    }

    ready = 0;
    for (i = 0; i < nfds; i++)
    {
        short revents = 0;
        if (fds[i].fd < 0)
        {
            continue;
        }
        if (FD_ISSET(fds[i].fd, &rfds))
        {
            revents |= POLLIN;
        }
        if (FD_ISSET(fds[i].fd, &wfds))
        {
            revents |= POLLOUT;
        }
        if (FD_ISSET(fds[i].fd, &efds))
        {
            revents |= POLLERR;
        }
        fds[i].revents = revents;
        if (revents)
        {
            ready++;
        }
    }

    return ready;
}

int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout_ts, const sigset_t *sigmask)
{
    int timeout;

    (void)sigmask;
    if (!timeout_ts)
    {
        timeout = -1;
    }
    else
    {
        if (timeout_ts->tv_sec < 0 || timeout_ts->tv_nsec < 0)
        {
            errno = EINVAL;
            return -1;
        }
        timeout = (int)(timeout_ts->tv_sec * 1000 + timeout_ts->tv_nsec / 1000000);
    }
    return poll(fds, nfds, timeout);
}

/*
 * Python 的线程后端会直接碰到一组 pthread 符号。
 * stupidos 目前还没有真正的用户态并发线程，因此这里先提供
 * “单线程兼容版”实现：
 * - 锁/条件变量：直接成功
 * - TLS：用一个很小的 key-value 表模拟
 * - pthread_create：明确返回 ENOSYS，避免伪造并发
 *
 * 这样 CPython 至少可以把解释器和单线程标准库跑起来。
 */
static void *u_pthread_tls_values[64];
static unsigned char u_pthread_tls_used[64];
struct u_pthread_mutex_slot
{
    pthread_mutex_t *key;
    int used;
    int locked;
    unsigned long owner;
    uint32_t recursion;
};
static struct u_pthread_mutex_slot u_pthread_mutex_slots[64];

static struct u_pthread_mutex_slot *u_pthread_mutex_slot_get(pthread_mutex_t *mutex, int create)
{
    unsigned int i;
    struct u_pthread_mutex_slot *free_slot = 0;

    if (!mutex)
    {
        return 0;
    }

    for (i = 0; i < (unsigned int)(sizeof(u_pthread_mutex_slots) / sizeof(u_pthread_mutex_slots[0])); ++i)
    {
        struct u_pthread_mutex_slot *slot = &u_pthread_mutex_slots[i];

        if (slot->used && slot->key == mutex)
        {
            return slot;
        }
        if (!slot->used && !free_slot)
        {
            free_slot = slot;
        }
    }

    if (!create || !free_slot)
    {
        return 0;
    }

    free_slot->used = 1;
    free_slot->key = mutex;
    free_slot->locked = 0;
    free_slot->owner = 0;
    free_slot->recursion = 0;
    return free_slot;
}

static int u_timespec_cmp(const struct timespec *lhs, const struct timespec *rhs)
{
    if (lhs->tv_sec < rhs->tv_sec)
    {
        return -1;
    }
    if (lhs->tv_sec > rhs->tv_sec)
    {
        return 1;
    }
    if (lhs->tv_nsec < rhs->tv_nsec)
    {
        return -1;
    }
    if (lhs->tv_nsec > rhs->tv_nsec)
    {
        return 1;
    }
    return 0;
}

static int u_clock_realtime_now(struct timespec *ts)
{
    struct timeval tv;

    if (!ts)
    {
        return -1;
    }
    if (clock_gettime(CLOCK_REALTIME, ts) == 0)
    {
        return 0;
    }
    if (gettimeofday(&tv, 0) < 0)
    {
        return -1;
    }
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = (long)tv.tv_usec * 1000L;
    return 0;
}

int pthread_attr_init(pthread_attr_t *attr)
{
    if (attr)
    {
        memset(attr, 0, sizeof(*attr));
    }
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    (void)attr;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
    (void)attr;
    (void)stacksize;
    return 0;
}

int pthread_attr_setscope(pthread_attr_t *attr, int scope)
{
    (void)attr;
    (void)scope;
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *attr)
{
    if (attr)
    {
        memset(attr, 0, sizeof(*attr));
    }
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id)
{
    (void)attr;
    (void)clock_id;
    return 0;
}

int pthread_getcpuclockid(pthread_t thread_id, clockid_t *clock_id)
{
    (void)thread_id;
    if (!clock_id)
    {
        return EINVAL;
    }
    *clock_id = CLOCK_MONOTONIC;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    struct u_pthread_mutex_slot *slot;

    (void)attr;
    if (!mutex)
    {
        return EINVAL;
    }
    memset(mutex, 0, sizeof(*mutex));
    slot = u_pthread_mutex_slot_get(mutex, 1);
    if (!slot)
    {
        return ENOSPC;
    }
    slot->locked = 0;
    slot->owner = 0;
    slot->recursion = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    struct u_pthread_mutex_slot *slot;

    if (!mutex)
    {
        return EINVAL;
    }
    slot = u_pthread_mutex_slot_get(mutex, 0);
    if (slot)
    {
        slot->used = 0;
        slot->key = 0;
        slot->locked = 0;
        slot->owner = 0;
        slot->recursion = 0;
    }
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    struct u_pthread_mutex_slot *slot;
    unsigned long me;

    if (!mutex)
    {
        return EINVAL;
    }
    slot = u_pthread_mutex_slot_get(mutex, 1);
    if (!slot)
    {
        return ENOSPC;
    }
    me = (unsigned long)pthread_self();

    if (slot->locked && slot->owner == me)
    {
        /*
         * 关键兼容（中文）：
         * 在 stupidos 当前单线程模型里，默认 mutex 若遇到同线程重复加锁，
         * 按 POSIX 严格语义会死锁，实际会把 CPython 启动链直接卡死。
         * 这里先做“可重入计数”语义，优先保证解释器可运行。
         */
        slot->recursion++;
        return 0;
    }

    /*
     * stupidos 单线程兼容语义：
     * - 如果已经上锁，这里短暂让出 CPU，等待解锁。
     * - 在当前阶段它主要服务 CPython 内部锁，不追求完整抢占公平。
     */
    while (slot->locked)
    {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 1000000L;
        (void)nanosleep(&ts, 0);
    }
    slot->locked = 1;
    slot->owner = me;
    slot->recursion = 1;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    struct u_pthread_mutex_slot *slot;
    unsigned long me;

    if (!mutex)
    {
        return EINVAL;
    }
    slot = u_pthread_mutex_slot_get(mutex, 1);
    if (!slot)
    {
        return ENOSPC;
    }
    me = (unsigned long)pthread_self();

    if (slot->locked && slot->owner == me)
    {
        slot->recursion++;
        return 0;
    }

    if (slot->locked)
    {
        return EBUSY;
    }
    slot->locked = 1;
    slot->owner = me;
    slot->recursion = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    struct u_pthread_mutex_slot *slot;
    unsigned long me;

    if (!mutex)
    {
        return EINVAL;
    }
    slot = u_pthread_mutex_slot_get(mutex, 0);
    if (!slot || !slot->locked)
    {
        return EPERM;
    }
    me = (unsigned long)pthread_self();
    if (slot->owner != 0 && slot->owner != me)
    {
        return EPERM;
    }
    if (slot->recursion > 1)
    {
        slot->recursion--;
        return 0;
    }

    slot->locked = 0;
    slot->owner = 0;
    slot->recursion = 0;
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    (void)attr;
    if (cond)
    {
        memset(cond, 0, sizeof(*cond));
    }
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    struct timespec ts;
    int ret;

    (void)cond;
    if (!mutex)
    {
        return EINVAL;
    }
    ret = pthread_mutex_unlock(mutex);
    if (ret != 0)
    {
        return ret;
    }
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000L;
    (void)nanosleep(&ts, 0);
    ret = pthread_mutex_lock(mutex);
    if (ret != 0)
    {
        return ret;
    }
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abs_timeout)
{
    int ret;

    (void)cond;
    if (!mutex || !abs_timeout)
    {
        return EINVAL;
    }

    ret = pthread_mutex_unlock(mutex);
    if (ret != 0)
    {
        return ret;
    }

    /*
     * 关键修复：
     * CPython 的 pthread 锁实现依赖 timedwait 超时返回 ETIMEDOUT。
     * 之前固定返回 0 会让等待循环误以为被唤醒，最终在 import/bootstrap
     * 阶段进入无穷循环卡死。
     */
    for (;;)
    {
        struct timespec now;
        struct timespec slice;
        long long remain_ns;

        if (u_clock_realtime_now(&now) < 0)
        {
            ret = EINVAL;
            break;
        }
        if (u_timespec_cmp(&now, abs_timeout) >= 0)
        {
            ret = ETIMEDOUT;
            break;
        }

        remain_ns = (long long)(abs_timeout->tv_sec - now.tv_sec) * 1000000000LL
                  + (long long)(abs_timeout->tv_nsec - now.tv_nsec);
        if (remain_ns <= 0)
        {
            ret = ETIMEDOUT;
            break;
        }

        if (remain_ns > 1000000LL)
        {
            remain_ns = 1000000LL;
        }
        slice.tv_sec = 0;
        slice.tv_nsec = (long)remain_ns;
        (void)nanosleep(&slice, 0);
    }

    {
        int lock_ret = pthread_mutex_lock(mutex);
        if (lock_ret != 0)
        {
            return lock_ret;
        }
    }
    return ret;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    static unsigned int next_key = 1;

    (void)destructor;
    if (!key)
    {
        return EINVAL;
    }

    if (next_key >= (unsigned int)(sizeof(u_pthread_tls_values) / sizeof(u_pthread_tls_values[0])))
    {
        return ENOSPC;
    }

    *key = (pthread_key_t)next_key;
    u_pthread_tls_used[next_key] = 1;
    u_pthread_tls_values[next_key] = 0;
    next_key++;
    return 0;
}

int pthread_key_delete(pthread_key_t key)
{
    unsigned long index;

    index = (unsigned long)key;
    if (index >= (unsigned long)(sizeof(u_pthread_tls_values) / sizeof(u_pthread_tls_values[0])))
    {
        return EINVAL;
    }

    u_pthread_tls_used[index] = 0;
    u_pthread_tls_values[index] = 0;
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    unsigned long index;

    index = (unsigned long)key;
    if (index >= (unsigned long)(sizeof(u_pthread_tls_values) / sizeof(u_pthread_tls_values[0]))
        || !u_pthread_tls_used[index])
    {
        return EINVAL;
    }

    u_pthread_tls_values[index] = (void *)value;
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    unsigned long index;

    index = (unsigned long)key;
    if (index >= (unsigned long)(sizeof(u_pthread_tls_values) / sizeof(u_pthread_tls_values[0]))
        || !u_pthread_tls_used[index])
    {
        return 0;
    }

    return u_pthread_tls_values[index];
}

pthread_t pthread_self(void)
{
    return (pthread_t)(uintptr_t)1;
}

int pthread_detach(pthread_t thread)
{
    (void)thread;
    return 0;
}

void pthread_exit(void *retval)
{
    (void)retval;
    u_exit(0);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
    (void)attr;
    (void)start_routine;
    (void)arg;

    /*
     * 这里明确告诉上层：当前版本没有真正的内核线程支持。
     * 比起伪造“成功创建但其实没有并发”更安全，也更利于定位后续问题。
     */
    if (thread)
    {
        *thread = (pthread_t)(uintptr_t)1;
    }
    errno = ENOSYS;
    return ENOSYS;
}

int dup(int oldfd)
{
    return u_sysret_int((int64_t)u_dup(oldfd));
}

#ifndef STUPIDOS_MINIMAL_OS
int dup2(int oldfd, int newfd)
{
    return u_sysret_int((int64_t)u_dup2(oldfd, newfd));
}
#endif

int dup3(int oldfd, int newfd, int flags)
{
    if (oldfd == newfd)
    {
        errno = EINVAL;
        return -1;
    }

    if (flags & ~O_CLOEXEC)
    {
        errno = EINVAL;
        return -1;
    }

    return dup2(oldfd, newfd);
}

ssize_t pread(int fd, void *buf, size_t len, off_t off)
{
    return u_sysret_ssize((int64_t)u_pread64(fd, buf, len, (uint64_t)off));
}

ssize_t pwrite(int fd, const void *buf, size_t len, off_t off)
{
    return u_sysret_ssize((int64_t)u_pwrite64(fd, buf, len, (uint64_t)off));
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    struct stupidos_iovec tmp[16];
    int i;

    if (!iov || iovcnt < 0 || iovcnt > 16)
    {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < iovcnt; i++)
    {
        tmp[i].iov_base = iov[i].iov_base;
        tmp[i].iov_len = (uint64_t)iov[i].iov_len;
    }
    return u_sysret_ssize((int64_t)u_readv(fd, tmp, iovcnt));
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    struct stupidos_iovec tmp[16];
    int i;

    if (!iov || iovcnt < 0 || iovcnt > 16)
    {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < iovcnt; i++)
    {
        tmp[i].iov_base = (void *)iov[i].iov_base;
        tmp[i].iov_len = (uint64_t)iov[i].iov_len;
    }
    return u_sysret_ssize((int64_t)u_writev(fd, tmp, iovcnt));
}

ssize_t preadv64v2(int fd, const struct iovec *iov, int iovcnt, off64_t off, int flags)
{
    ssize_t total;
    int i;

    (void)flags;
    if (!iov || iovcnt < 0)
    {
        errno = EINVAL;
        return -1;
    }

    total = 0;
    for (i = 0; i < iovcnt; i++)
    {
        ssize_t n = pread(fd, iov[i].iov_base, iov[i].iov_len, (off_t)off);
        if (n < 0)
        {
            return (total > 0) ? total : -1;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len)
        {
            break;
        }
        off += (off64_t)n;
    }
    return total;
}

ssize_t pwritev64v2(int fd, const struct iovec *iov, int iovcnt, off64_t off, int flags)
{
    ssize_t total;
    int i;

    (void)flags;
    if (!iov || iovcnt < 0)
    {
        errno = EINVAL;
        return -1;
    }

    total = 0;
    for (i = 0; i < iovcnt; i++)
    {
        ssize_t n = pwrite(fd, iov[i].iov_base, iov[i].iov_len, (off_t)off);
        if (n < 0)
        {
            return (total > 0) ? total : -1;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len)
        {
            break;
        }
        off += (off64_t)n;
    }
    return total;
}

ssize_t sendfile64(int out_fd, int in_fd, off64_t *offset, size_t count)
{
    uint8_t buf[4096];
    size_t done;
    ssize_t rd;
    ssize_t wr;
    off_t off;

    done = 0;
    off = offset ? (off_t)(*offset) : lseek(in_fd, 0, SEEK_CUR);
    while (done < count)
    {
        size_t chunk = count - done;
        if (chunk > sizeof(buf))
        {
            chunk = sizeof(buf);
        }

        rd = pread(in_fd, buf, chunk, off);
        if (rd <= 0)
        {
            break;
        }

        wr = write(out_fd, buf, (size_t)rd);
        if (wr <= 0)
        {
            break;
        }

        done += (size_t)wr;
        off += (off_t)wr;
    }

    if (offset)
    {
        *offset = (off64_t)off;
    }
    return (ssize_t)done;
}

ssize_t splice(int fd_in, off64_t *off_in, int fd_out, off64_t *off_out, size_t len, unsigned int flags)
{
    (void)fd_out;
    (void)off_out;
    (void)flags;
    return sendfile64(fd_out, fd_in, off_in, len);
}

ssize_t copy_file_range(int fd_in,
                        off64_t *off_in,
                        int fd_out,
                        off64_t *off_out,
                        size_t len,
                        unsigned int flags)
{
    (void)flags;
    (void)off_out;
    return sendfile64(fd_out, fd_in, off_in, len);
}

int fcntl(int fd, int cmd, ...)
{
    unsigned long arg = 0;
    va_list ap;
    bool is_write_end;

    switch (cmd)
    {
    case F_GETFD:
    case F_GETFL:
        break;
    default:
        va_start(ap, cmd);
        arg = va_arg(ap, unsigned long);
        va_end(ap);
        break;
    }

    if (u_pipe_slot_from_fd(fd, &is_write_end))
    {
        return u_pipe_fcntl(fd, cmd, arg);
    }

    return u_sysret_int((int64_t)u_fcntl(fd, cmd, arg));
}

int fcntl64(int fd, int cmd, ...)
{
    unsigned long arg = 0;
    va_list ap;
    bool is_write_end;

    switch (cmd)
    {
    case F_GETFD:
    case F_GETFL:
        break;
    default:
        va_start(ap, cmd);
        arg = va_arg(ap, unsigned long);
        va_end(ap);
        break;
    }

    if (u_pipe_slot_from_fd(fd, &is_write_end))
    {
        return u_pipe_fcntl(fd, cmd, arg);
    }

    return u_sysret_int((int64_t)u_fcntl(fd, cmd, arg));
}

int lockf64(int fd, int cmd, off64_t len)
{
    (void)fd;
    (void)cmd;
    (void)len;
    return 0;
}

int posix_fadvise64(int fd, off64_t offset, off64_t len, int advice)
{
    (void)fd;
    (void)offset;
    (void)len;
    (void)advice;
    return 0;
}

int posix_fallocate64(int fd, off64_t offset, off64_t len)
{
    (void)fd;
    (void)offset;
    (void)len;
    return 0;
}

int ioctl(int fd, unsigned long request, ...)
{
    void *argp;
    va_list ap;
    bool is_write_end;

    va_start(ap, request);
    argp = va_arg(ap, void *);
    va_end(ap);

    if (u_pipe_slot_from_fd(fd, &is_write_end))
    {
        return u_pipe_ioctl(fd, request, argp);
    }
    return u_sysret_int((int64_t)u_ioctl(fd, request, argp));
}

int pipe(int fds[2])
{
    return pipe2(fds, 0);
}

int pipe2(int fds[2], int flags)
{
    int64_t kret;
    struct u_pipe_slot *slot;
    int read_fd;
    int write_fd;
    size_t i;

    if (!fds)
    {
        errno = EINVAL;
        return -1;
    }

    if (flags & ~(O_CLOEXEC | O_NONBLOCK))
    {
        errno = EINVAL;
        return -1;
    }

    /*
     * 优先尝试内核 pipe2：
     * - 这样 Dropbear / BusyBox / 后续 shell pipeline 都能走真实 fd
     * - 如果内核路径暂时不稳定，也不要直接把 ssh/dbclient 卡死
     *   这里会继续回退到 libc 自管管道，保证交互工具“先能连上”
     *
     * 这层回退对 dbclient 特别重要，因为它启动时会先创建一个
     * signal self-pipe；只要这个 pipe 能工作，SSH 后续的 select
     * / 信号唤醒 / 输入输出协作就能继续跑下去。
     */
    kret = (int64_t)u_pipe2(fds, flags);
    if (!u_sysret_is_error(kret))
    {
        return 0;
    }

    for (i = 0; i < U_PIPE_SLOT_MAX; i++)
    {
        if (!u_pipe_slots[i].used)
        {
            slot = &u_pipe_slots[i];
            memset((int8_t *)slot, 0, sizeof(*slot));
            slot->used = true;
            slot->read_fd = -1;
            slot->write_fd = -1;
            read_fd = u_pipe_alloc_fd();
            if (read_fd < 0)
            {
                memset((int8_t *)slot, 0, sizeof(*slot));
                errno = EMFILE;
                return -1;
            }
            slot->read_fd = read_fd;
            write_fd = u_pipe_alloc_fd();
            if (write_fd < 0)
            {
                memset((int8_t *)slot, 0, sizeof(*slot));
                errno = EMFILE;
                return -1;
            }
            slot->write_fd = write_fd;
            slot->read_open = true;
            slot->write_open = true;
            slot->read_nonblock = (flags & O_NONBLOCK) ? true : false;
            slot->write_nonblock = (flags & O_NONBLOCK) ? true : false;
            slot->head = 0;
            slot->tail = 0;
            slot->used_bytes = 0;
            fds[0] = read_fd;
            fds[1] = write_fd;
            return 0;
        }
    }

    /*
     * 如果内核 pipe2 失败，而 libc 兜底管道也没有槽位了，
     * 就按“资源耗尽”返回。这里优先保证 ssh/dbclient 可继续启动，
     * 因为它对 signal self-pipe 的要求本质上只是“有一个可用 pipe”。
     */
    errno = EMFILE;
    return -1;
}

int openpty(int *amaster, int *aslave, char *name, void *termp, void *winp)
{
    (void)amaster;
    (void)aslave;
    (void)name;
    (void)termp;
    (void)winp;
    errno = ENOSYS;
    return -1;
}

int getdents64(unsigned int fd, struct dirent64 *dirp, unsigned int count)
{
    return u_sysret_int((int64_t)u_getdents64((int)fd, (struct stupidos_linux_dirent64 *)dirp, count));
}

int getdents(unsigned int fd, struct dirent *dirp, unsigned int count)
{
    return getdents64(fd, (struct dirent64 *)dirp, count);
}

int pause(void)
{
    /*
     * 当前还没有完整的异步信号挂起语义。
     * 返回 EINTR 让上层回到错误处理路径，比死等更安全。
     */
    errno = EINTR;
    return -1;
}

int getrlimit(int resource, struct rlimit *rlim)
{
    return u_sysret_int((int64_t)u_prlimit64(0, resource, 0, (struct stupidos_rlimit *)rlim));
}

int setrlimit(int resource, const struct rlimit *rlim)
{
    return u_sysret_int((int64_t)u_prlimit64(0, resource, (const struct stupidos_rlimit *)rlim, 0));
}

int getrlimit64(int resource, struct rlimit64 *rlim)
{
    return u_sysret_int((int64_t)u_prlimit64(0, resource, 0, (struct stupidos_rlimit *)rlim));
}

int setrlimit64(int resource, const struct rlimit64 *rlim)
{
    return u_sysret_int((int64_t)u_prlimit64(0, resource, (const struct stupidos_rlimit *)rlim, 0));
}

long sysconf(int name)
{
    /*
     * 这里返回一组对 Python 比较关键的系统参数：
     * - 页面大小 / IOV 最大值 / 终端名称长度 / CPU 数
     * - 其余未实现项统一返回 -1 并设置 EINVAL
     */
    switch (name)
    {
#if defined(_SC_PAGESIZE) && defined(_SC_PAGE_SIZE) && (_SC_PAGESIZE == _SC_PAGE_SIZE)
    case _SC_PAGESIZE:
#elif defined(_SC_PAGESIZE)
    case _SC_PAGESIZE:
#elif defined(_SC_PAGE_SIZE)
    case _SC_PAGE_SIZE:
#endif
        return 4096L;
#ifdef _SC_CLK_TCK
    case _SC_CLK_TCK:
        return 100L;
#endif
#ifdef _SC_IOV_MAX
    case _SC_IOV_MAX:
        return 1024L;
#endif
#ifdef _SC_TTY_NAME_MAX
    case _SC_TTY_NAME_MAX:
        return 32L;
#endif
#ifdef _SC_OPEN_MAX
    case _SC_OPEN_MAX:
        return 256L;
#endif
#ifdef _SC_NPROCESSORS_ONLN
    case _SC_NPROCESSORS_ONLN:
#endif
#ifdef _SC_NPROCESSORS_CONF
    case _SC_NPROCESSORS_CONF:
#endif
        return 4L;
    default:
        errno = EINVAL;
        return -1L;
    }
}

long pathconf(const char *path, int name)
{
    (void)path;
    return sysconf(name);
}

long fpathconf(int fd, int name)
{
    (void)fd;
    return sysconf(name);
}

size_t confstr(int name, char *buf, size_t len)
{
    const char *value;
    size_t need;

    value = "";
    switch (name)
    {
#ifdef _CS_PATH
    case _CS_PATH:
        value = "/bin:/usr/bin";
        break;
#endif
#ifdef _CS_GNU_LIBC_VERSION
    case _CS_GNU_LIBC_VERSION:
        value = "stupidos 0.1";
        break;
#endif
#ifdef _CS_GNU_LIBPTHREAD_VERSION
    case _CS_GNU_LIBPTHREAD_VERSION:
        value = "stupidos-pthread 0.1";
        break;
#endif
    default:
        value = "";
        break;
    }

    need = strlen(value) + 1U;
    if (buf && len > 0U)
    {
        if (need > len)
        {
            memcpy(buf, value, len - 1U);
            buf[len - 1U] = '\0';
        }
        else
        {
            memcpy(buf, value, need);
        }
    }
    return need;
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    return u_sysret_int((int64_t)u_rt_sigaction(signum, act, oldact, sizeof(sigset_t)));
}

sighandler_t signal(int signum, sighandler_t handler)
{
    struct sigaction act;
    struct sigaction oldact;

    memset((char *)&act, 0, sizeof(act));
    act.sa_handler = handler;
    act.sa_flags = SA_RESTART;
    if (sigemptyset(&act.sa_mask) < 0)
    {
        return SIG_ERR;
    }
    if (sigaction(signum, &act, &oldact) < 0)
    {
        return SIG_ERR;
    }
    return oldact.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    return u_sysret_int((int64_t)u_rt_sigprocmask(how, set, oldset, sizeof(sigset_t)));
}

int sigaltstack(const stack_t *ss, stack_t *old_ss)
{
    return u_sysret_int((int64_t)u_sigaltstack(ss, old_ss));
}

int sigemptyset(sigset_t *set)
{
    if (!set)
    {
        return EINVAL;
    }

    memset(set, 0, sizeof(*set));
    return 0;
}

int sigfillset(sigset_t *set)
{
    if (!set)
    {
        return EINVAL;
    }

    memset(set, 0xFF, sizeof(*set));
    return 0;
}

static int u_sigset_bit_index(int signum, size_t *word_index, unsigned long *bit_mask)
{
    size_t idx;
    size_t bits_per_word;

    if (signum <= 0 || signum >= NSIG)
    {
        return -EINVAL;
    }

    bits_per_word = sizeof(unsigned long) * 8U;
    idx = (size_t)(signum - 1) / bits_per_word;
    *word_index = idx;
    *bit_mask = 1UL << ((unsigned long)(signum - 1) % bits_per_word);
    return 0;
}

int sigaddset(sigset_t *set, int signum)
{
    size_t word_index;
    unsigned long bit_mask;
    int ret;

    if (!set)
    {
        return EINVAL;
    }

    ret = u_sigset_bit_index(signum, &word_index, &bit_mask);
    if (ret < 0)
    {
        return EINVAL;
    }

    set->__val[word_index] |= bit_mask;
    return 0;
}

int sigdelset(sigset_t *set, int signum)
{
    size_t word_index;
    unsigned long bit_mask;
    int ret;

    if (!set)
    {
        return EINVAL;
    }

    ret = u_sigset_bit_index(signum, &word_index, &bit_mask);
    if (ret < 0)
    {
        return EINVAL;
    }

    set->__val[word_index] &= ~bit_mask;
    return 0;
}

int sigismember(const sigset_t *set, int signum)
{
    size_t word_index;
    unsigned long bit_mask;
    int ret;

    if (!set)
    {
        return EINVAL;
    }

    ret = u_sigset_bit_index(signum, &word_index, &bit_mask);
    if (ret < 0)
    {
        return EINVAL;
    }

    return (set->__val[word_index] & bit_mask) ? 1 : 0;
}

int kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;

    errno = ENOSYS;
    return -1;
}

int killpg(pid_t pgrp, int sig)
{
    (void)pgrp;
    (void)sig;
    errno = ENOSYS;
    return -1;
}

unsigned int alarm(unsigned int seconds)
{
    (void)seconds;
    return 0;
}

int getitimer(int which, struct itimerval *curr_value)
{
    (void)which;
    if (curr_value)
    {
        memset(curr_value, 0, sizeof(*curr_value));
    }
    return 0;
}

int setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value)
{
    (void)which;
    (void)new_value;
    if (old_value)
    {
        memset(old_value, 0, sizeof(*old_value));
    }
    return 0;
}

int sigpending(sigset_t *set)
{
    if (!set)
    {
        errno = EINVAL;
        return -1;
    }
    return sigemptyset(set);
}

int sigwaitinfo(const sigset_t *set, siginfo_t *info)
{
    (void)set;
    (void)info;
    errno = ENOSYS;
    return -1;
}

int sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *timeout)
{
    (void)set;
    (void)info;
    (void)timeout;
    errno = ENOSYS;
    return -1;
}

int raise(int sig)
{
    return kill(getpid(), sig);
}

char *strsignal(int sig)
{
    static char buf[32];

    (void)snprintf(buf, sizeof(buf), "signal %d", sig);
    return buf;
}

pid_t fork(void)
{
    errno = ENOSYS;
    return -1;
}

pid_t vfork(void)
{
    return fork();
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    int64_t ret;
    int32_t kstatus;

    (void)options;
    kstatus = 0;
    ret = u_waitpid_status((int32_t)pid, &kstatus);
    if (ret == -ENOSYS)
    {
        ret = u_waitpid((int32_t)pid);
        kstatus = 0;
    }
    if (ret < 0)
    {
        errno = (int)(-ret);
        return -1;
    }

    if (status)
    {
        *status = (int)kstatus;
    }

    return (pid_t)ret;
}

pid_t wait(int *status)
{
    return waitpid(-1, status, 0);
}

pid_t wait4(pid_t pid, int *status, int options, struct rusage *rusage)
{
    (void)rusage;
    return waitpid(pid, status, options);
}

pid_t wait3(int *status, int options, struct rusage *rusage)
{
    return wait4(-1, status, options, rusage);
}

int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options)
{
    (void)idtype;
    (void)id;
    (void)infop;
    (void)options;
    errno = ENOSYS;
    return -1;
}

void exit(int code)
{
    while (u_atexit_count > 0)
    {
        void (*handler)(void);

        handler = u_atexit_handlers[--u_atexit_count];
        if (handler)
        {
            handler();
        }
    }
    u_exit(code);
}

void _exit(int code)
{
    u_exit(code);
}

int atexit(void (*func)(void))
{
    if (!func)
    {
        errno = EINVAL;
        return -1;
    }

    if (u_atexit_count >= (sizeof(u_atexit_handlers) / sizeof(u_atexit_handlers[0])))
    {
        errno = ENOMEM;
        return -1;
    }

    u_atexit_handlers[u_atexit_count++] = func;
    return 0;
}

void abort(void)
{
    u_exit(134);
}

/*
 * exec 兼容层（中文）：
 * 当前内核提供的是“spawn 新任务 + waitpid”的组合，而不是完整的
 * 进程地址空间替换语义。这里把 exec* 做成“成功后等待子任务结束并退出自己”，
 * 尽量接近“成功不返回”的行为，先支撑工具链迁移。
 */
#define U_EXEC_MAX_ARGS 16

static int u_exec_build_argv(const char *path, char *const argv[], const int8_t **out_argv, int *out_argc)
{
    int argc;

    if (!path || !out_argv || !out_argc)
    {
        errno = EINVAL;
        return -1;
    }

    argc = 0;
    if (argv && argv[0])
    {
        while (argv[argc])
        {
            if (argc >= U_EXEC_MAX_ARGS)
            {
                errno = E2BIG;
                return -1;
            }

            out_argv[argc] = (const int8_t *)argv[argc];
            argc++;
        }
    }
    else
    {
        out_argv[0] = (const int8_t *)path;
        argc = 1;
    }

    if (argc <= 0)
    {
        errno = EINVAL;
        return -1;
    }

    *out_argc = argc;
    return 0;
}

static int u_exec_spawn_only(const char *path, char *const argv[])
{
    const int8_t *kargv[U_EXEC_MAX_ARGS];
    int argc;
    int64_t ret;

    if (u_exec_build_argv(path, argv, kargv, &argc) < 0)
    {
        return -1;
    }

    ret = (int64_t)u_exec((const int8_t *)path, argc, kargv);
    if (u_sysret_is_error(ret))
    {
        errno = (int)(-ret);
        return -1;
    }

    return (int)ret;
}

static int u_exec_replace(const char *path, char *const argv[])
{
    int pid;
    int status;

    pid = u_exec_spawn_only(path, argv);
    if (pid < 0)
    {
        return -1;
    }

    if (waitpid((pid_t)pid, &status, 0) < 0)
    {
        return -1;
    }

    u_exit(0);
    __builtin_unreachable();
}

static int u_exec_search_path_replace(const char *file, char *const argv[])
{
    const char *path_env;
    char candidate[STUPIDOS_PATH_MAX];
    const char *p;

    if (!file || file[0] == '\0')
    {
        errno = ENOENT;
        return -1;
    }

    if (strchr(file, '/'))
    {
        return u_exec_replace(file, argv);
    }

    path_env = getenv("PATH");
    if (!path_env || path_env[0] == '\0')
    {
        path_env = "/bin:/usr/bin";
    }

    p = path_env;
    while (*p)
    {
        const char *seg = p;
        size_t seg_len;
        size_t file_len;
        size_t total;
        int err;

        while (*p && *p != ':')
        {
            p++;
        }
        seg_len = (size_t)(p - seg);
        file_len = strlen(file);

        if (seg_len == 0)
        {
            seg = ".";
            seg_len = 1;
        }

        total = seg_len + 1 + file_len;
        if (total + 1 <= sizeof(candidate))
        {
            memcpy(candidate, seg, seg_len);
            candidate[seg_len] = '/';
            memcpy(candidate + seg_len + 1, file, file_len);
            candidate[total] = '\0';

            if (u_exec_replace(candidate, argv) == 0)
            {
                return 0;
            }

            err = errno;
            if (err != ENOENT && err != ENOTDIR)
            {
                return -1;
            }
        }

        if (*p == ':')
        {
            p++;
        }
    }

    errno = ENOENT;
    return -1;
}

int system(const char *command)
{
    char *const argv_sh[] = { (char *)"sh", (char *)"-c", (char *)command, 0 };
    char *const argv_direct[] = { (char *)command, 0 };
    int pid;

    if (!command)
    {
        return 1;
    }

    if (command[0] == '\0')
    {
        return 0;
    }

    /*
     * 兼容优先：先尝试 /bin/sh -c，
     * 若当前 shell 还不支持 -c，再回退成“直接执行单个程序路径”。
     */
    pid = u_exec_spawn_only("/bin/sh", argv_sh);
    if (pid < 0)
    {
        pid = u_exec_spawn_only(command, argv_direct);
        if (pid < 0)
        {
            return -1;
        }
    }

    if (waitpid((pid_t)pid, 0, 0) < 0)
    {
        return -1;
    }

    return 0;
}

int execv(const char *path, char *const argv[])
{
    (void)environ;
    if (!path)
    {
        errno = EINVAL;
        return -1;
    }
    return u_exec_replace(path, argv);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    if (!path)
    {
        errno = EINVAL;
        return -1;
    }

    (void)envp;
    return u_exec_replace(path, argv);
}

int execvp(const char *file, char *const argv[])
{
    return u_exec_search_path_replace(file, argv);
}

int execveat(int dirfd, const char *path, char *const argv[], char *const envp[], int flags)
{
    if (flags != 0)
    {
        errno = ENOTSUP;
        return -1;
    }

    if (dirfd != AT_FDCWD)
    {
        errno = ENOTSUP;
        return -1;
    }

    return execve(path, argv, envp);
}

static int u_exec_build_varargs(const char *arg0, va_list ap, char *argv_buf[], size_t argv_cap, char *const **out_argv, char *const **out_envp, int has_envp)
{
    size_t argc;
    char *arg;

    if (!argv_buf || argv_cap < 2 || !out_argv)
    {
        errno = EINVAL;
        return -1;
    }

    argc = 0;
    arg = (char *)arg0;
    while (arg)
    {
        if (argc + 1 >= argv_cap)
        {
            errno = E2BIG;
            return -1;
        }

        argv_buf[argc++] = arg;
        arg = va_arg(ap, char *);
    }
    argv_buf[argc] = 0;
    *out_argv = argv_buf;

    if (has_envp && out_envp)
    {
        *out_envp = va_arg(ap, char *const *);
    }

    return 0;
}

int execl(const char *path, const char *arg, ...)
{
    va_list ap;
    char *argv_buf[U_EXEC_MAX_ARGS + 1];
    char *const *argv_list;

    va_start(ap, arg);
    if (u_exec_build_varargs(arg, ap, argv_buf, U_EXEC_MAX_ARGS + 1U, &argv_list, 0, 0) < 0)
    {
        va_end(ap);
        return -1;
    }
    va_end(ap);
    return execv(path, (char *const *)argv_list);
}

int execlp(const char *file, const char *arg, ...)
{
    va_list ap;
    char *argv_buf[U_EXEC_MAX_ARGS + 1];
    char *const *argv_list;

    va_start(ap, arg);
    if (u_exec_build_varargs(arg, ap, argv_buf, U_EXEC_MAX_ARGS + 1U, &argv_list, 0, 0) < 0)
    {
        va_end(ap);
        return -1;
    }
    va_end(ap);
    return execvp(file, (char *const *)argv_list);
}

int execle(const char *path, const char *arg, ...)
{
    va_list ap;
    char *argv_buf[U_EXEC_MAX_ARGS + 1];
    char *const *argv_list;
    char *const *envp;

    envp = 0;
    va_start(ap, arg);
    if (u_exec_build_varargs(arg, ap, argv_buf, U_EXEC_MAX_ARGS + 1U, &argv_list, &envp, 1) < 0)
    {
        va_end(ap);
        return -1;
    }
    va_end(ap);
    return execve(path, (char *const *)argv_list, (char *const *)envp);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    (void)envp;
    return execvp(file, argv);
}

int fexecve(int fd, char *const argv[], char *const envp[])
{
    (void)fd;
    (void)argv;
    (void)envp;
    errno = ENOSYS;
    return -1;
}

DIR *fdopendir(int fd)
{
    (void)fd;
    errno = ENOSYS;
    return 0;
}

int forkpty(int *amaster, char *name, const struct termios *termp, const struct winsize *winp)
{
    (void)amaster;
    (void)name;
    (void)termp;
    (void)winp;
    errno = ENOSYS;
    return -1;
}

int posix_spawn(pid_t *pid,
                const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[],
                char *const envp[])
{
    int child;

    (void)file_actions;
    (void)attrp;
    (void)envp;

    child = u_exec_spawn_only(path, argv);
    if (child < 0)
    {
        return errno ? errno : ENOSYS;
    }
    if (pid)
    {
        *pid = (pid_t)child;
    }
    return 0;
}

int posix_spawnp(pid_t *pid,
                 const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[],
                 char *const envp[])
{
    (void)file_actions;
    (void)attrp;
    (void)envp;

    if (pid)
    {
        *pid = -1;
    }
    if (!file)
    {
        return EINVAL;
    }
    return ENOSYS;
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions)
{
    if (!actions)
    {
        return EINVAL;
    }
    memset(actions, 0, sizeof(*actions));
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions)
{
    if (!actions)
    {
        return EINVAL;
    }
    return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions, int fd, const char *path, int oflag, mode_t mode)
{
    (void)actions;
    (void)fd;
    (void)path;
    (void)oflag;
    (void)mode;
    return ENOSYS;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions, int fd)
{
    (void)actions;
    (void)fd;
    return ENOSYS;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd, int newfd)
{
    (void)actions;
    (void)fd;
    (void)newfd;
    return ENOSYS;
}

int posix_spawnattr_init(posix_spawnattr_t *attr)
{
    if (!attr)
    {
        return EINVAL;
    }
    memset(attr, 0, sizeof(*attr));
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *attr)
{
    if (!attr)
    {
        return EINVAL;
    }
    return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags)
{
    if (!attr)
    {
        return EINVAL;
    }
    attr->_flags = flags;
    return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup)
{
    if (!attr)
    {
        return EINVAL;
    }
    attr->_pgrp = pgroup;
    return 0;
}

int posix_spawnattr_setschedpolicy(posix_spawnattr_t *attr, int policy)
{
    if (!attr)
    {
        return EINVAL;
    }
    attr->_policy = policy;
    return 0;
}

int posix_spawnattr_setschedparam(posix_spawnattr_t *attr, const struct sched_param *param)
{
    if (!attr || !param)
    {
        return EINVAL;
    }
    attr->_sp = *param;
    return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *sigdefault)
{
    if (!attr || !sigdefault)
    {
        return EINVAL;
    }
    attr->_sd = *sigdefault;
    return 0;
}

int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *sigmask)
{
    if (!attr || !sigmask)
    {
        return EINVAL;
    }
    attr->_ss = *sigmask;
    return 0;
}

long syscall(long number, ...)
{
    va_list ap;
    long ret;

    va_start(ap, number);
    ret = -1;

    /*
     * 这里只实现 CPython 运行时最常碰到的几个 Linux syscall 编号：
     * - gettid：线程标识
     * - getrandom：启动随机数 / hash seed
     * - getdents64：子进程辅助代码可能会碰到
     *
     * 其它号统一返回 ENOSYS，避免误导上层以为功能可用。
     */
    switch (number)
    {
#ifdef SYS_read
    case SYS_read:
    {
        int fd = va_arg(ap, int);
        void *buf = va_arg(ap, void *);
        size_t len = va_arg(ap, size_t);
        ret = (long)u_read(fd, buf, len);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_write
    case SYS_write:
    {
        int fd = va_arg(ap, int);
        const void *buf = va_arg(ap, const void *);
        size_t len = va_arg(ap, size_t);
        ret = (long)u_write(fd, buf, len);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_openat
    case SYS_openat:
    {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        int flags = va_arg(ap, int);
        int mode = 0;
        if (flags & O_CREAT)
        {
            mode = va_arg(ap, int);
        }
        (void)mode;
        ret = (long)u_openat(dirfd, (const int8_t *)path, u_translate_open_flags(flags));
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_close
    case SYS_close:
    {
        int fd = va_arg(ap, int);
        ret = (long)u_close(fd);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_lseek
    case SYS_lseek:
    {
        int fd = va_arg(ap, int);
        off_t off = va_arg(ap, off_t);
        int whence = va_arg(ap, int);
        ret = (long)u_lseek(fd, off, whence);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_fcntl
    case SYS_fcntl:
    {
        int fd = va_arg(ap, int);
        int cmd = va_arg(ap, int);
        unsigned long arg = va_arg(ap, unsigned long);
        ret = (long)u_fcntl(fd, cmd, arg);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_ioctl
    case SYS_ioctl:
    {
        int fd = va_arg(ap, int);
        unsigned long req = va_arg(ap, unsigned long);
        void *argp = va_arg(ap, void *);
        ret = (long)u_ioctl(fd, req, argp);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_mmap
    case SYS_mmap:
    {
        void *addr = va_arg(ap, void *);
        size_t len = va_arg(ap, size_t);
        int prot = va_arg(ap, int);
        int flags = va_arg(ap, int);
        int fd = va_arg(ap, int);
        off_t off = va_arg(ap, off_t);
        ret = (long)u_mmap(addr, len, prot, flags, fd, off);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_munmap
    case SYS_munmap:
    {
        void *addr = va_arg(ap, void *);
        size_t len = va_arg(ap, size_t);
        ret = (long)u_munmap(addr, len);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_mprotect
    case SYS_mprotect:
    {
        void *addr = va_arg(ap, void *);
        size_t len = va_arg(ap, size_t);
        int prot = va_arg(ap, int);
        ret = (long)u_mprotect(addr, len, prot);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_brk
    case SYS_brk:
    {
        void *addr = va_arg(ap, void *);
        ret = (long)(uintptr_t)u_brk(addr);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_clock_gettime
    case SYS_clock_gettime:
    {
        int clockid = va_arg(ap, int);
        struct timespec *ts = va_arg(ap, struct timespec *);
        ret = (long)u_clock_gettime(clockid, (struct stupidos_timespec *)ts);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_nanosleep
    case SYS_nanosleep:
    {
        const struct timespec *req = va_arg(ap, const struct timespec *);
        struct timespec *rem = va_arg(ap, struct timespec *);
        ret = (long)u_nanosleep((const struct stupidos_timespec *)req, (struct stupidos_timespec *)rem);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_newfstatat
    case SYS_newfstatat:
    {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        struct stat *st = va_arg(ap, struct stat *);
        int flags = va_arg(ap, int);
        ret = (long)u_fstatat(dirfd, (const int8_t *)path, (struct stupidos_stat *)st, flags);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_readlinkat
    case SYS_readlinkat:
    {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        char *buf = va_arg(ap, char *);
        size_t len = va_arg(ap, size_t);
        char resolved[STUPIDOS_PATH_MAX];

        if (u_resolve_at_path(dirfd, path, resolved, sizeof(resolved)) != 0)
        {
            ret = -1;
            break;
        }

        ret = (long)u_readlink((const int8_t *)resolved, (int8_t *)buf, len);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_mkdirat
    case SYS_mkdirat:
    {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        mode_t mode = (mode_t)va_arg(ap, int);
        char resolved[STUPIDOS_PATH_MAX];

        if (u_resolve_at_path(dirfd, path, resolved, sizeof(resolved)) != 0)
        {
            ret = -1;
            break;
        }
        ret = (long)mkdir(resolved, mode);
        break;
    }
#endif
#ifdef SYS_unlinkat
    case SYS_unlinkat:
    {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        int flags = va_arg(ap, int);
        char resolved[STUPIDOS_PATH_MAX];

        if (u_resolve_at_path(dirfd, path, resolved, sizeof(resolved)) != 0)
        {
            ret = -1;
            break;
        }
#ifdef AT_REMOVEDIR
        if (flags & AT_REMOVEDIR)
        {
            ret = (long)rmdir(resolved);
        }
        else
#endif
        {
            ret = (long)unlink(resolved);
        }
        break;
    }
#endif
#ifdef SYS_renameat
    case SYS_renameat:
    {
        int olddirfd = va_arg(ap, int);
        const char *oldpath = va_arg(ap, const char *);
        int newdirfd = va_arg(ap, int);
        const char *newpath = va_arg(ap, const char *);
        char old_resolved[STUPIDOS_PATH_MAX];
        char new_resolved[STUPIDOS_PATH_MAX];

        if (u_resolve_at_path(olddirfd, oldpath, old_resolved, sizeof(old_resolved)) != 0)
        {
            ret = -1;
            break;
        }
        if (u_resolve_at_path(newdirfd, newpath, new_resolved, sizeof(new_resolved)) != 0)
        {
            ret = -1;
            break;
        }
        ret = (long)rename(old_resolved, new_resolved);
        break;
    }
#endif
#ifdef SYS_renameat2
    case SYS_renameat2:
    {
        int olddirfd = va_arg(ap, int);
        const char *oldpath = va_arg(ap, const char *);
        int newdirfd = va_arg(ap, int);
        const char *newpath = va_arg(ap, const char *);
        unsigned int flags = va_arg(ap, unsigned int);
        char old_resolved[STUPIDOS_PATH_MAX];
        char new_resolved[STUPIDOS_PATH_MAX];
        if (flags != 0U)
        {
            errno = ENOTSUP;
            ret = -1;
            break;
        }
        if (u_resolve_at_path(olddirfd, oldpath, old_resolved, sizeof(old_resolved)) != 0)
        {
            ret = -1;
            break;
        }
        if (u_resolve_at_path(newdirfd, newpath, new_resolved, sizeof(new_resolved)) != 0)
        {
            ret = -1;
            break;
        }
        ret = (long)rename(old_resolved, new_resolved);
        break;
    }
#endif
#ifdef SYS_fchmodat
    case SYS_fchmodat:
    {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        mode_t mode = (mode_t)va_arg(ap, int);
        int flags = va_arg(ap, int);
        char resolved[STUPIDOS_PATH_MAX];

        if (flags != 0)
        {
            errno = ENOTSUP;
            ret = -1;
            break;
        }
        if (u_resolve_at_path(dirfd, path, resolved, sizeof(resolved)) != 0)
        {
            ret = -1;
            break;
        }
        ret = (long)chmod(resolved, mode);
        break;
    }
#endif
#ifdef SYS_fchownat
    case SYS_fchownat:
    {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        uid_t owner = (uid_t)va_arg(ap, unsigned int);
        gid_t group = (gid_t)va_arg(ap, unsigned int);
        int flags = va_arg(ap, int);
        char resolved[STUPIDOS_PATH_MAX];

        if (flags != 0)
        {
            errno = ENOTSUP;
            ret = -1;
            break;
        }
        if (u_resolve_at_path(dirfd, path, resolved, sizeof(resolved)) != 0)
        {
            ret = -1;
            break;
        }
        ret = (long)chown(resolved, owner, group);
        break;
    }
#endif
#ifdef SYS_truncate
    case SYS_truncate:
    {
        const char *path = va_arg(ap, const char *);
        off_t len = va_arg(ap, off_t);
        ret = (long)truncate(path, len);
        break;
    }
#endif
#ifdef SYS_ftruncate
    case SYS_ftruncate:
    {
        int fd = va_arg(ap, int);
        off_t len = va_arg(ap, off_t);
        ret = (long)ftruncate(fd, len);
        break;
    }
#endif
#ifdef SYS_utimensat
    case SYS_utimensat:
    {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        const struct timespec *times = va_arg(ap, const struct timespec *);
        int flags = va_arg(ap, int);
        ret = (long)u_utimensat(dirfd, (const int8_t *)path,
                                (const struct stupidos_timespec *)times, flags);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_sched_getaffinity
    case SYS_sched_getaffinity:
    {
        int pid = va_arg(ap, int);
        size_t cpusetsize = va_arg(ap, size_t);
        void *mask = va_arg(ap, void *);
        ret = (long)u_sched_getaffinity(pid, cpusetsize, mask);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_prlimit64
    case SYS_prlimit64:
    {
        int pid = va_arg(ap, int);
        int resource = va_arg(ap, int);
        const struct rlimit *new_limit = va_arg(ap, const struct rlimit *);
        struct rlimit *old_limit = va_arg(ap, struct rlimit *);
        ret = (long)u_prlimit64(pid, resource, (const struct stupidos_rlimit *)new_limit,
                                (struct stupidos_rlimit *)old_limit);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_gettid
    case SYS_gettid:
        ret = (long)u_gettid();
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
#endif
#ifdef SYS_getrandom
    case SYS_getrandom:
    {
        void *buf = va_arg(ap, void *);
        size_t len = va_arg(ap, size_t);
        unsigned int flags = va_arg(ap, unsigned int);
        ret = (long)u_getrandom(buf, len, flags);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_getdents64
    case SYS_getdents64:
    {
        unsigned int fd = va_arg(ap, unsigned int);
        struct dirent64 *dirp = va_arg(ap, struct dirent64 *);
        unsigned int count = va_arg(ap, unsigned int);
        ret = (long)u_getdents64((int)fd, (struct stupidos_linux_dirent64 *)dirp, count);
        if (ret < 0)
        {
            errno = (int)(-ret);
            ret = -1;
        }
        break;
    }
#endif
#ifdef SYS_faccessat
    case SYS_faccessat:
    {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        int mode = va_arg(ap, int);
        int flags = va_arg(ap, int);
        ret = (long)faccessat(dirfd, path, mode, flags);
        break;
    }
#endif
#ifdef SYS_faccessat2
    case SYS_faccessat2:
    {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        int mode = va_arg(ap, int);
        int flags = va_arg(ap, int);
        ret = (long)faccessat(dirfd, path, mode, flags);
        break;
    }
#endif
#ifdef SYS_dup3
    case SYS_dup3:
    {
        int oldfd = va_arg(ap, int);
        int newfd = va_arg(ap, int);
        int flags = va_arg(ap, int);
        ret = (long)dup3(oldfd, newfd, flags);
        break;
    }
#endif
#ifdef SYS_pipe2
    case SYS_pipe2:
    {
        int *fds = va_arg(ap, int *);
        int flags = va_arg(ap, int);
        ret = (long)pipe2(fds, flags);
        break;
    }
#endif
#ifdef SYS_wait4
    case SYS_wait4:
    {
        pid_t pid = (pid_t)va_arg(ap, int);
        int *status = va_arg(ap, int *);
        int options = va_arg(ap, int);
        void *rusage = va_arg(ap, void *);
        (void)rusage;
        ret = (long)waitpid(pid, status, options);
        break;
    }
#endif
#ifdef SYS_execve
    case SYS_execve:
    {
        const char *path = va_arg(ap, const char *);
        char *const *argv = va_arg(ap, char *const *);
        char *const *envp = va_arg(ap, char *const *);
        ret = (long)execve(path, (char *const *)argv, (char *const *)envp);
        break;
    }
#endif
#ifdef SYS_execveat
    case SYS_execveat:
    {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        char *const *argv = va_arg(ap, char *const *);
        char *const *envp = va_arg(ap, char *const *);
        int flags = va_arg(ap, int);
        ret = (long)execveat(dirfd, path, (char *const *)argv, (char *const *)envp, flags);
        break;
    }
#endif
    default:
        errno = ENOSYS;
        ret = -1;
        break;
    }

    va_end(ap);
    return ret;
}

static void u_memswap_bytes(uint8_t *a, uint8_t *b, size_t size)
{
    size_t i;
    uint8_t tmp;

    for (i = 0; i < size; ++i)
    {
        tmp = a[i];
        a[i] = b[i];
        b[i] = tmp;
    }
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *))
{
    size_t i;
    size_t j;
    size_t min_index;
    uint8_t *data;

    if (!base || !compar || size == 0 || nmemb < 2)
    {
        return;
    }

    data = (uint8_t *)base;
    for (i = 0; i < nmemb; ++i)
    {
        min_index = i;
        for (j = i + 1; j < nmemb; ++j)
        {
            if (compar(data + j * size, data + min_index * size) < 0)
            {
                min_index = j;
            }
        }
        if (min_index != i)
        {
            u_memswap_bytes(data + i * size, data + min_index * size, size);
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *))
{
    const uint8_t *data;
    size_t left;
    size_t right;
    size_t mid;
    int cmp;

    if (!key || !base || !compar || size == 0)
    {
        return 0;
    }

    data = (const uint8_t *)base;
    left = 0;
    right = nmemb;
    while (left < right)
    {
        mid = left + (right - left) / 2U;
        cmp = compar(key, data + mid * size);
        if (cmp < 0)
        {
            right = mid;
        }
        else if (cmp > 0)
        {
            left = mid + 1U;
        }
        else
        {
            return (void *)(data + mid * size);
        }
    }

    return 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    int ret;

    ret = sigprocmask(how, set, oldset);
    if (ret < 0)
    {
        return errno;
    }
    return 0;
}

int pthread_kill(pthread_t thread, int sig)
{
    (void)thread;
    (void)sig;
    return ENOSYS;
}

int sigwait(const sigset_t *set, int *sig)
{
    (void)set;
    (void)sig;
    return ENOSYS;
}

int futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3)
{
    return u_sysret_int((int64_t)u_futex(uaddr, op, val, timeout, uaddr2, val3));
}

char *strcat(char *dest, const char *src)
{
    size_t dlen;
    size_t i;

    if (!dest || !src)
    {
        return dest;
    }

    dlen = strlen(dest);
    for (i = 0; src[i] != '\0'; i++)
    {
        dest[dlen + i] = src[i];
    }
    dest[dlen + i] = '\0';
    return dest;
}

char *strpbrk(const char *s, const char *accept)
{
    size_t i;
    size_t j;

    if (!s || !accept)
    {
        return 0;
    }

    for (i = 0; s[i] != '\0'; i++)
    {
        for (j = 0; accept[j] != '\0'; j++)
        {
            if (s[i] == accept[j])
            {
                return (char *)(s + i);
            }
        }
    }

    return 0;
}

int remove(const char *pathname)
{
    if (!pathname)
    {
        errno = EINVAL;
        return -1;
    }

    return unlink(pathname);
}

/*
 * 简化 realpath：先检查路径可访问，再返回原路径副本。
 * 对当前系统移植阶段（无符号链接、路径层级较浅）足够稳定，
 * 后续若引入 symlink/.. 规范化，可在这里升级为完整实现。
 */
char *realpath(const char *path, char *resolved_path)
{
    size_t n;
    char *out;

    if (!path)
    {
        errno = EINVAL;
        return 0;
    }

    if (access(path, F_OK) != 0)
    {
        return 0;
    }

    n = strnlen(path, STUPIDOS_PATH_MAX * 4U);
    if (!resolved_path)
    {
        out = (char *)malloc(n + 1U);
        if (!out)
        {
            errno = ENOMEM;
            return 0;
        }
    }
    else
    {
        out = resolved_path;
    }

    memcpy(out, path, n);
    out[n] = '\0';
    return out;
}

static int u_is_space_char(int ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

/*
 * 最小浮点解析器：
 * 支持 [+/-]digits[.digits][e[+/-]digits]，足够覆盖 tinycc 自己的词法需求。
 */
static double u_parse_fp(const char *nptr, char **endptr)
{
    const char *p;
    int sign;
    double val;
    int has_digit;
    int exp_sign;
    int exp10;

    p = nptr;
    while (*p && u_is_space_char((unsigned char)*p))
    {
        p++;
    }

    sign = 1;
    if (*p == '+')
    {
        p++;
    }
    else if (*p == '-')
    {
        sign = -1;
        p++;
    }

    val = 0.0;
    has_digit = 0;
    while (*p >= '0' && *p <= '9')
    {
        val = val * 10.0 + (double)(*p - '0');
        p++;
        has_digit = 1;
    }

    if (*p == '.')
    {
        double place = 0.1;
        p++;
        while (*p >= '0' && *p <= '9')
        {
            val += (double)(*p - '0') * place;
            place *= 0.1;
            p++;
            has_digit = 1;
        }
    }

    if (!has_digit)
    {
        if (endptr)
        {
            *endptr = (char *)nptr;
        }
        return 0.0;
    }

    if (*p == 'e' || *p == 'E')
    {
        const char *ep = p + 1;
        exp_sign = 1;
        exp10 = 0;

        if (*ep == '+')
        {
            ep++;
        }
        else if (*ep == '-')
        {
            exp_sign = -1;
            ep++;
        }

        if (*ep >= '0' && *ep <= '9')
        {
            while (*ep >= '0' && *ep <= '9')
            {
                exp10 = exp10 * 10 + (*ep - '0');
                ep++;
            }
            p = ep;
        }

        if (exp10 > 0)
        {
            while (exp10--)
            {
                if (exp_sign > 0)
                {
                    val *= 10.0;
                }
                else
                {
                    val *= 0.1;
                }
            }
        }
    }

    if (endptr)
    {
        *endptr = (char *)p;
    }

    if (sign < 0)
    {
        val = -val;
    }
    return val;
}

double strtod(const char *nptr, char **endptr) { return u_parse_fp(nptr, endptr); }
float strtof(const char *nptr, char **endptr) { return (float)u_parse_fp(nptr, endptr); }
long double strtold(const char *nptr, char **endptr) { return (long double)u_parse_fp(nptr, endptr); }

struct tm *localtime(const time_t *timep)
{
    static struct tm tm_buf;
    return localtime_r(timep, &tm_buf);
}

FILE *freopen(const char *pathname, const char *mode, FILE *stream)
{
    (void)pathname;
    (void)mode;
    (void)stream;
    errno = ENOSYS;
    return 0;
}

void *__clear_cache(void *begin, void *end)
{
    (void)begin;
    (void)end;
    return 0;
}

static char u_dlerror_buf[64];

void *dlopen(const char *file, int mode)
{
    (void)file;
    (void)mode;
    strcpy(u_dlerror_buf, "dlopen: not supported");
    errno = ENOSYS;
    return 0;
}

void *dlsym(void *handle, const char *symbol)
{
    (void)handle;
    (void)symbol;
    strcpy(u_dlerror_buf, "dlsym: not supported");
    errno = ENOSYS;
    return 0;
}

int dlclose(void *handle)
{
    (void)handle;
    errno = ENOSYS;
    return -1;
}

char *dlerror(void)
{
    if (u_dlerror_buf[0] == '\0')
    {
        return 0;
    }
    return u_dlerror_buf;
}

static uint16_t u_bswap16(uint16_t v)
{
    return __builtin_bswap16(v);
}

static uint32_t u_bswap32(uint32_t v)
{
    return __builtin_bswap32(v);
}

uint16_t htons(uint16_t hostshort)
{
    return u_bswap16(hostshort);
}

uint16_t ntohs(uint16_t netshort)
{
    return u_bswap16(netshort);
}

uint32_t htonl(uint32_t hostlong)
{
    return u_bswap32(hostlong);
}

uint32_t ntohl(uint32_t netlong)
{
    return u_bswap32(netlong);
}

int inet_pton(int af, const char *src, void *dst)
{
    uint32_t octets[4];
    uint32_t cur;
    size_t i;
    size_t idx;
    uint8_t *out;

    if (!src || !dst)
    {
        errno = EINVAL;
        return -1;
    }

    if (af != AF_INET)
    {
        errno = EAFNOSUPPORT;
        return -1;
    }

    idx = 0;
    cur = 0;
    for (i = 0;; i++)
    {
        char ch = src[i];
        if (ch >= '0' && ch <= '9')
        {
            cur = cur * 10U + (uint32_t)(ch - '0');
            if (cur > 255U)
            {
                errno = EINVAL;
                return -1;
            }
            continue;
        }
        if (ch == '.' || ch == '\0')
        {
            if (idx >= 4)
            {
                errno = EINVAL;
                return -1;
            }
            octets[idx++] = cur;
            cur = 0;
            if (ch == '\0')
            {
                break;
            }
            continue;
        }
        errno = EINVAL;
        return -1;
    }

    if (idx != 4)
    {
        errno = EINVAL;
        return -1;
    }

    out = (uint8_t *)dst;
    out[0] = (uint8_t)octets[0];
    out[1] = (uint8_t)octets[1];
    out[2] = (uint8_t)octets[2];
    out[3] = (uint8_t)octets[3];
    return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    const uint8_t *p;
    int n;

    if (!src || !dst)
    {
        errno = EINVAL;
        return 0;
    }
    if (af != AF_INET)
    {
        errno = EAFNOSUPPORT;
        return 0;
    }
    if (size < 16U)
    {
        errno = ENOSPC;
        return 0;
    }

    p = (const uint8_t *)src;
    n = snprintf(dst, size, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    if (n < 0 || (socklen_t)n >= size)
    {
        errno = ENOSPC;
        return 0;
    }
    return dst;
}

static int u_parse_service_port(const char *service, uint16_t *port_out)
{
    uint32_t port;
    size_t i;

    if (!service || !port_out || !service[0])
    {
        return -1;
    }

    if (strcmp(service, "ssh") == 0)
    {
        *port_out = 22;
        return 0;
    }
    if (strcmp(service, "http") == 0)
    {
        *port_out = 80;
        return 0;
    }
    if (strcmp(service, "https") == 0)
    {
        *port_out = 443;
        return 0;
    }

    port = 0;
    for (i = 0; service[i] != '\0'; i++)
    {
        if (service[i] < '0' || service[i] > '9')
        {
            return -1;
        }
        port = port * 10U + (uint32_t)(service[i] - '0');
        if (port > 65535U)
        {
            return -1;
        }
    }

    if (port == 0)
    {
        return -1;
    }

    *port_out = (uint16_t)port;
    return 0;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    struct addrinfo *ai;
    struct sockaddr_in *sin;
    uint32_t ipv4;
    uint16_t port;
    size_t node_len;
    char hostbuf[128];

    if (!res)
    {
        return EAI_SYSTEM;
    }

    *res = 0;

    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
    {
        return EAI_FAMILY;
    }
    if (hints && hints->ai_socktype && hints->ai_socktype != SOCK_STREAM && hints->ai_socktype != SOCK_DGRAM)
    {
        return EAI_SOCKTYPE;
    }

    if (service)
    {
        if (u_parse_service_port(service, &port) < 0)
        {
            return EAI_SERVICE;
        }
    }
    else
    {
        port = 0;
    }

    ipv4 = INADDR_LOOPBACK;
    if (!node || !node[0])
    {
        if (hints && (hints->ai_flags & AI_PASSIVE))
        {
            ipv4 = INADDR_ANY;
        }
    }
    else if (inet_pton(AF_INET, node, &ipv4) == 1)
    {
        uint8_t *raw = (uint8_t *)&ipv4;
        ipv4 = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | (uint32_t)raw[3];
    }
    else if (strcmp(node, "localhost") == 0)
    {
        ipv4 = INADDR_LOOPBACK;
    }
    else
    {
        if (hints && (hints->ai_flags & AI_NUMERICHOST))
        {
            return EAI_NONAME;
        }

        node_len = strnlen(node, sizeof(hostbuf) - 1U);
        if (node_len == 0 || node_len >= sizeof(hostbuf))
        {
            return EAI_NONAME;
        }
        memcpy(hostbuf, node, node_len);
        hostbuf[node_len] = '\0';
        if (u_dns_lookup((const int8_t *)hostbuf, &ipv4, 3000U) < 0)
        {
            return EAI_AGAIN;
        }
    }

    ai = (struct addrinfo *)calloc(1, sizeof(*ai));
    if (!ai)
    {
        return EAI_MEMORY;
    }

    sin = (struct sockaddr_in *)calloc(1, sizeof(*sin));
    if (!sin)
    {
        free(ai);
        return EAI_MEMORY;
    }

    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    {
        uint8_t *dst = (uint8_t *)&sin->sin_addr.s_addr;
        dst[0] = (uint8_t)((ipv4 >> 24) & 0xffU);
        dst[1] = (uint8_t)((ipv4 >> 16) & 0xffU);
        dst[2] = (uint8_t)((ipv4 >> 8) & 0xffU);
        dst[3] = (uint8_t)(ipv4 & 0xffU);
    }

    ai->ai_family = AF_INET;
    ai->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints ? hints->ai_protocol : IPPROTO_TCP;
    ai->ai_addrlen = sizeof(*sin);
    ai->ai_addr = (struct sockaddr *)sin;
    if (node && (hints && (hints->ai_flags & AI_CANONNAME)))
    {
        size_t canon_len = strlen(node) + 1U;

        ai->ai_canonname = (char *)malloc(canon_len);
        if (!ai->ai_canonname)
        {
            free(sin);
            free(ai);
            return EAI_MEMORY;
        }
        memcpy(ai->ai_canonname, node, canon_len);
    }

    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res)
    {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res->ai_canonname);
        free(res);
        res = next;
    }
}

const char *gai_strerror(int errcode)
{
    switch (errcode)
    {
    case 0: return "success";
    case EAI_BADFLAGS: return "bad flags";
    case EAI_NONAME: return "name not found";
    case EAI_AGAIN: return "temporary failure";
    case EAI_FAIL: return "non-recoverable failure";
    case EAI_FAMILY: return "address family not supported";
    case EAI_SOCKTYPE: return "socket type not supported";
    case EAI_SERVICE: return "service not supported";
    case EAI_MEMORY: return "out of memory";
    default: return "unknown error";
    }
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags)
{
    const struct sockaddr_in *sin;
    const uint8_t *ip;
    uint16_t port;

    (void)flags;
    if (!sa || salen < sizeof(*sin))
    {
        return EAI_FAMILY;
    }

    sin = (const struct sockaddr_in *)sa;
    if (sin->sin_family != AF_INET)
    {
        return EAI_FAMILY;
    }

    if (host && hostlen > 0)
    {
        ip = (const uint8_t *)&sin->sin_addr.s_addr;
        if (!inet_ntop(AF_INET, ip, host, hostlen))
        {
            return EAI_MEMORY;
        }
    }

    if (serv && servlen > 0)
    {
        port = ntohs(sin->sin_port);
        if (snprintf(serv, servlen, "%u", port) < 0)
        {
            return EAI_MEMORY;
        }
    }

    return 0;
}

int socket(int domain, int type, int protocol)
{
    if (domain != AF_INET || type != SOCK_STREAM || (protocol != 0 && protocol != IPPROTO_TCP))
    {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return u_sysret_int((int64_t)u_socket(domain, type, protocol));
}

int connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    return u_sysret_int((int64_t)u_connect(fd, addr, addrlen));
}

int shutdown(int fd, int how)
{
    return u_sysret_int((int64_t)u_shutdown(fd, how));
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    (void)flags;
    return u_sysret_ssize((int64_t)u_write(fd, buf, len));
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    (void)flags;
    return u_sysret_ssize((int64_t)u_read(fd, buf, len));
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t addrlen)
{
    (void)addr;
    (void)addrlen;
    return send(fd, buf, len, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)addr;
    (void)addrlen;
    return recv(fd, buf, len, flags);
}

int bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    (void)fd;
    (void)addr;
    (void)addrlen;
    errno = ENOTSUP;
    return -1;
}

int listen(int fd, int backlog)
{
    (void)fd;
    (void)backlog;
    errno = ENOTSUP;
    return -1;
}

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)fd;
    (void)addr;
    (void)addrlen;
    errno = ENOTSUP;
    return -1;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)fd;
    (void)addr;
    (void)addrlen;
    errno = ENOTSUP;
    return -1;
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)fd;
    (void)addr;
    (void)addrlen;
    errno = ENOTSUP;
    return -1;
}

int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
    (void)fd;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    return 0;
}

int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen)
{
    if (!optval || !optlen)
    {
        errno = EINVAL;
        return -1;
    }

    return u_sysret_int((int64_t)u_getsockopt(fd, level, optname, optval, (unsigned int *)optlen));
}

int _setjmp(jmp_buf env)
{
    return __builtin_setjmp(env);
}

void longjmp(jmp_buf env, int val)
{
    (void)val;
    __builtin_longjmp(env, 1);
}
