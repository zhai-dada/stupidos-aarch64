#include "stupidos_user.h"
#include "errno.h"
#include "stdlib.h"
#include "string.h"
#include "ctype.h"
#include "fcntl.h"
#include <stdarg.h>

#include "lib/libasm.h"

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
char **environ = u_environ_items;
int errno;

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
    uint64_t size;

    size = u_heap_round_size(need);
    arena = (struct u_heap_arena *)u_mmap(0, size, 0, 0, -1, 0);
    if (!arena)
    {
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

int getuid(void)
{
    return u_getuid();
}

int getgid(void)
{
    return u_getgid();
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    return u_gettimeofday((struct stupidos_timeval *)tv);
}

int isatty(int fd)
{
    return u_isatty(fd);
}

int access(const char *path, int mode)
{
    return u_access((const int8_t *)path, mode);
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

    return u_open((const int8_t *)path, u_translate_open_flags(flags));
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

    return u_openat(dirfd, (const int8_t *)path, u_translate_open_flags(flags));
}

int close(int fd)
{
    return u_close(fd);
}

ssize_t read(int fd, void *buf, size_t len)
{
    return u_read(fd, buf, len);
}

ssize_t write(int fd, const void *buf, size_t len)
{
    return u_write(fd, buf, len);
}

off_t lseek(int fd, off_t offset, int whence)
{
    return (off_t)u_lseek(fd, offset, whence);
}

int stat(const char *path, struct stat *out)
{
    return u_stat((const int8_t *)path, (struct stupidos_stat *)out);
}

int fstat(int fd, struct stat *out)
{
    return u_fstat(fd, (struct stupidos_stat *)out);
}

int chdir(const char *path)
{
    return (int)u_chdir((const int8_t *)path);
}

char *getcwd(char *buf, size_t len)
{
    if (u_getcwd((int8_t *)buf, len) < 0)
    {
        return 0;
    }
    return buf;
}

int clock_gettime(clockid_t clockid, struct timespec *out)
{
    return u_clock_gettime((int)clockid, (struct stupidos_timespec *)out);
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    return u_nanosleep((const struct stupidos_timespec *)req, (struct stupidos_timespec *)rem);
}

unsigned int sleep(unsigned int seconds)
{
    struct stupidos_timespec ts;

    ts.tv_sec = seconds;
    ts.tv_nsec = 0;
    (void)nanosleep((const struct timespec *)&ts, 0);
    return 0;
}

ssize_t getrandom(void *buf, size_t len, unsigned int flags)
{
    return u_getrandom(buf, len, flags);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    return u_mmap(addr, len, prot, flags, fd, off);
}

int munmap(void *addr, size_t len)
{
    return u_munmap(addr, len);
}

int mprotect(void *addr, size_t len, int prot)
{
    return u_mprotect(addr, len, prot);
}

int dup(int oldfd)
{
    return u_dup(oldfd);
}

int dup2(int oldfd, int newfd)
{
    return u_dup2(oldfd, newfd);
}

ssize_t pread(int fd, void *buf, size_t len, off_t off)
{
    return u_pread64(fd, buf, len, (uint64_t)off);
}

ssize_t pwrite(int fd, const void *buf, size_t len, off_t off)
{
    return u_pwrite64(fd, buf, len, (uint64_t)off);
}

int fcntl(int fd, int cmd, ...)
{
    unsigned long arg = 0;
    va_list ap;

    va_start(ap, cmd);
    arg = va_arg(ap, unsigned long);
    va_end(ap);
    return u_fcntl(fd, cmd, arg);
}

int ioctl(int fd, unsigned long request, void *argp)
{
    return u_ioctl(fd, request, argp);
}

int pipe(int fds[2])
{
    (void)fds;
    return -ENOSYS;
}

int pipe2(int fds[2], int flags)
{
    (void)fds;
    (void)flags;
    return -ENOSYS;
}

int getrlimit(int resource, struct rlimit *rlim)
{
    return u_prlimit64(0, resource, 0, (struct stupidos_rlimit *)rlim);
}

int setrlimit(int resource, const struct rlimit *rlim)
{
    return u_prlimit64(0, resource, (const struct stupidos_rlimit *)rlim, 0);
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    return u_rt_sigaction(signum, act, oldact, sizeof(sigset_t));
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    return u_rt_sigprocmask(how, set, oldset, sizeof(sigset_t));
}

int sigaltstack(const stack_t *ss, stack_t *old_ss)
{
    return u_sigaltstack(ss, old_ss);
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

pid_t fork(void)
{
    errno = ENOSYS;
    return -1;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    int64_t ret;

    (void)status;
    (void)options;
    ret = u_waitpid((int32_t)pid);
    if (ret < 0)
    {
        errno = (int)(-ret);
        return -1;
    }

    if (status)
    {
        *status = 0;
    }

    return (pid_t)pid;
}

pid_t wait(int *status)
{
    (void)status;
    errno = ENOSYS;
    return -1;
}

int futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3)
{
    return (int)u_futex(uaddr, op, val, timeout, uaddr2, val3);
}
