#include "stupidos_user.h"
#include "errno.h"
#include "stdlib.h"
#include "string.h"
#include "ctype.h"
#include "fcntl.h"
#include "pthread.h"
#include "time.h"
#include <wchar.h>
#include <locale.h>
#include <pwd.h>
#include <dirent.h>
#include <langinfo.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/syscall.h>
#include <sys/select.h>
#include <sys/time.h>
#include <stdarg.h>

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
static char u_locale_name[16] = "C";
static struct lconv u_locale_conv;
static char u_langinfo_codeset[] = "UTF-8";
static struct passwd u_passwd_root;
static struct passwd u_passwd_user;
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

    if (!src)
    {
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

    if (!src)
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
    return u_gettimeofday((struct stupidos_timeval *)tv);
}

int isatty(int fd)
{
    return u_isatty(fd);
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
    return u_access((const int8_t *)path, mode);
}

int chmod(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

int fchmod(int fd, mode_t mode)
{
    (void)fd;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

int chown(const char *path, uid_t owner, gid_t group)
{
    (void)path;
    (void)owner;
    (void)group;
    errno = ENOSYS;
    return -1;
}

int fchown(int fd, uid_t owner, gid_t group)
{
    (void)fd;
    (void)owner;
    (void)group;
    errno = ENOSYS;
    return -1;
}

int link(const char *oldpath, const char *newpath)
{
    (void)oldpath;
    (void)newpath;
    errno = ENOSYS;
    return -1;
}

int symlink(const char *oldpath, const char *newpath)
{
    (void)oldpath;
    (void)newpath;
    errno = ENOSYS;
    return -1;
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    (void)path;
    (void)buf;
    (void)bufsiz;
    errno = ENOSYS;
    return -1;
}

int mkdir(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

int rmdir(const char *path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}

int unlink(const char *path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}

int rename(const char *oldpath, const char *newpath)
{
    (void)oldpath;
    (void)newpath;
    errno = ENOSYS;
    return -1;
}

int truncate(const char *path, off_t length)
{
    (void)path;
    (void)length;
    errno = ENOSYS;
    return -1;
}

int ftruncate(int fd, off_t length)
{
    (void)fd;
    (void)length;
    errno = ENOSYS;
    return -1;
}

int utime(const char *filename, const struct utimbuf *times)
{
    (void)filename;
    (void)times;
    errno = ENOSYS;
    return -1;
}

ssize_t getxattr(const char *path, const char *name, void *value, size_t size)
{
    (void)path;
    (void)name;
    (void)value;
    (void)size;
    errno = ENOSYS;
    return -1;
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
    errno = ENOSYS;
    return -1;
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
    errno = ENOSYS;
    return -1;
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
    errno = ENOSYS;
    return -1;
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

int stat64(const char *path, struct stat64 *out)
{
    return u_stat((const int8_t *)path, (struct stupidos_stat *)out);
}

int fstat64(int fd, struct stat64 *out)
{
    return u_fstat(fd, (struct stupidos_stat *)out);
}

int lstat64(const char *path, struct stat64 *out)
{
    return stat64(path, out);
}

int chdir(const char *path)
{
    return (int)u_chdir((const int8_t *)path);
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
    return u_nanosleep((const struct stupidos_timespec *)req, (struct stupidos_timespec *)rem);
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

int fdatasync(int fd)
{
    (void)fd;
    return 0;
}

static int u_select_sleep(const struct timeval *timeout)
{
    struct stupidos_timespec ts;

    if (!timeout)
    {
        return 0;
    }

    if (timeout->tv_sec < 0 || timeout->tv_usec < 0)
    {
        return EINVAL;
    }

    ts.tv_sec = (int64_t)timeout->tv_sec;
    ts.tv_nsec = (int64_t)timeout->tv_usec * 1000LL;
    if (ts.tv_nsec >= 1000000000LL)
    {
        ts.tv_sec += ts.tv_nsec / 1000000000LL;
        ts.tv_nsec %= 1000000000LL;
    }

    /*
     * 先把 select 退化成“超时等待”：
     * - timemodule / selectors / subprocess 这类路径先能继续跑
     * - 当前 stupidos 还没有完整的 fd 就绪唤醒链路
     * - 后面接入真正的事件驱动后，再替换成精确等待
     */
    return (int)u_nanosleep((const struct stupidos_timespec *)&ts, 0);
}

static int u_select_count_ready(int nfds, fd_set *set)
{
    int fd;
    int ready;

    if (!set || nfds <= 0)
    {
        return 0;
    }

    ready = 0;
    for (fd = 0; fd < nfds; ++fd)
    {
        if (FD_ISSET(fd, set))
        {
            ++ready;
        }
    }
    return ready;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
    int ready;
    int ret;

    ret = u_select_sleep(timeout);
    if (ret < 0)
    {
        errno = -ret;
        return -1;
    }

    /*
     * 当前没有完整的 fd 就绪通知链路。
     * 为了让 CPython 以及大量基于 select 轮询的库继续推进，
     * 我们先把传入的 fd 视为“可用”。
     *
     * 这样会让上层逻辑偏向“立即返回”，但比卡死更有利于迁移。
     */
    ready = u_select_count_ready(nfds, readfds)
          + u_select_count_ready(nfds, writefds)
          + u_select_count_ready(nfds, exceptfds);

    if (readfds || writefds || exceptfds)
    {
        return ready;
    }

    return 0;
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

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    (void)attr;
    if (mutex)
    {
        memset(mutex, 0, sizeof(*mutex));
    }
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    (void)mutex;
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
    (void)cond;
    (void)mutex;
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abs_timeout)
{
    (void)cond;
    (void)mutex;
    (void)abs_timeout;
    return 0;
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
    return u_dup(oldfd);
}

#ifndef STUPIDOS_MINIMAL_OS
int dup2(int oldfd, int newfd)
{
    return u_dup2(oldfd, newfd);
}
#endif

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

int fcntl64(int fd, int cmd, ...)
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

int getrlimit64(int resource, struct rlimit64 *rlim)
{
    return u_prlimit64(0, resource, 0, (struct stupidos_rlimit *)rlim);
}

int setrlimit64(int resource, const struct rlimit64 *rlim)
{
    return u_prlimit64(0, resource, (const struct stupidos_rlimit *)rlim, 0);
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
#ifdef _SC_PAGESIZE
    case _SC_PAGESIZE:
#endif
#ifdef _SC_PAGE_SIZE
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

int raise(int sig)
{
    return kill(getpid(), sig);
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

void exit(int code)
{
    u_exit(code);
}

void _exit(int code)
{
    u_exit(code);
}

void abort(void)
{
    u_exit(134);
}

int system(const char *command)
{
    (void)command;
    errno = ENOSYS;
    return -1;
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
#ifdef SYS_gettid
    case SYS_gettid:
        ret = (long)u_gettid();
        break;
#endif
#ifdef SYS_getrandom
    case SYS_getrandom:
    {
        void *buf = va_arg(ap, void *);
        size_t len = va_arg(ap, size_t);
        unsigned int flags = va_arg(ap, unsigned int);
        ret = (long)u_getrandom(buf, len, flags);
        break;
    }
#endif
#ifdef SYS_getdents64
    case SYS_getdents64:
        errno = ENOSYS;
        ret = -1;
        break;
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
        return -ret;
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
    return (int)u_futex(uaddr, op, val, timeout, uaddr2, val3);
}
