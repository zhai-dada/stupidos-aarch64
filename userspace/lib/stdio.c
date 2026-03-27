#include "stdio.h"
#include "errno.h"
#include "stdlib.h"
#include "string.h"
#include "fcntl.h"
#include "unistd.h"
#include <stdarg.h>

/*
 * 最小 stdio 实现：
 * 目标不是做完整 glibc，而是给 stupidos 用户态和 CPython 提供
 * 一组足够稳定的 FILE / printf / fgets / fread / fwrite 入口。
 */

struct u_stdio_file
{
    int fd;
    unsigned int flags;
    int eof;
    int err;
    int has_ungot;
    int ungot;
};

#define UFP(fp) ((struct u_stdio_file *)(fp))

static struct u_stdio_file u_stdin_file = { .fd = STUPIDOS_STDIN_FILENO };
static struct u_stdio_file u_stdout_file = { .fd = STUPIDOS_STDOUT_FILENO };
static struct u_stdio_file u_stderr_file = { .fd = STUPIDOS_STDERR_FILENO };

FILE *stdin = (FILE *)&u_stdin_file;
FILE *stdout = (FILE *)&u_stdout_file;
FILE *stderr = (FILE *)&u_stderr_file;

static FILE *u_file_alloc(int fd)
{
    FILE *fp;

    fp = (FILE *)malloc(sizeof(struct u_stdio_file));
    if (!fp)
    {
        errno = ENOMEM;
        return 0;
    }

    memset(fp, 0, sizeof(struct u_stdio_file));
    UFP(fp)->fd = fd;
    return fp;
}

static int u_stdio_mode_to_flags(const char *mode)
{
    int flags;
    int plus;
    int write;
    int append;

    flags = 0;
    plus = 0;
    write = 0;
    append = 0;
    if (!mode)
    {
        return -1;
    }

    for (; *mode; mode++)
    {
        if (*mode == 'r')
        {
            flags |= O_RDONLY;
        }
        else if (*mode == 'w')
        {
            flags |= O_WRONLY | O_CREAT | O_TRUNC;
            write = 1;
        }
        else if (*mode == 'a')
        {
            flags |= O_WRONLY | O_CREAT | O_APPEND;
            write = 1;
            append = 1;
        }
        else if (*mode == '+')
        {
            plus = 1;
        }
        else if (*mode == 'b' || *mode == 't')
        {
            continue;
        }
        else
        {
            return -1;
        }
    }

    if (plus)
    {
        flags &= ~(O_RDONLY | O_WRONLY);
        flags |= O_RDWR;
    }
    if (append)
    {
        flags |= O_APPEND;
    }
    if (!write && !plus)
    {
        flags |= O_RDONLY;
    }
    return flags;
}

static void u_append_char(char *buf, size_t size, size_t *pos, char ch)
{
    if (buf && size > 0 && *pos + 1 < size)
    {
        buf[*pos] = ch;
    }
    (*pos)++;
}

static void u_append_string(char *buf, size_t size, size_t *pos, const char *s)
{
    if (!s)
    {
        s = "(null)";
    }

    while (*s)
    {
        u_append_char(buf, size, pos, *s++);
    }
}

static void u_append_unsigned(char *buf, size_t size, size_t *pos, unsigned long long value, unsigned base, int upper)
{
    char tmp[32];
    size_t len;
    const char *digits;

    digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    len = 0;
    if (value == 0)
    {
        tmp[len++] = '0';
    }
    else
    {
        while (value && len < sizeof(tmp))
        {
            tmp[len++] = digits[value % base];
            value /= base;
        }
    }

    while (len > 0)
    {
        u_append_char(buf, size, pos, tmp[--len]);
    }
}

static void u_append_signed(char *buf, size_t size, size_t *pos, long long value)
{
    unsigned long long mag;

    if (value < 0)
    {
        u_append_char(buf, size, pos, '-');
        mag = (unsigned long long)(-(value + 1LL)) + 1ULL;
    }
    else
    {
        mag = (unsigned long long)value;
    }

    u_append_unsigned(buf, size, pos, mag, 10, 0);
}

static int u_vsnprintf_impl(char *buf, size_t size, const char *fmt, va_list ap)
{
    size_t pos;
    va_list aq;

    pos = 0;
    if (!fmt)
    {
        return -1;
    }

    va_copy(aq, ap);
    while (*fmt)
    {
        if (*fmt != '%')
        {
            u_append_char(buf, size, &pos, *fmt++);
            continue;
        }

        fmt++;
        if (*fmt == '%')
        {
            u_append_char(buf, size, &pos, '%');
            fmt++;
            continue;
        }

        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' || *fmt == '0')
        {
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
        {
            fmt++;
        }
        if (*fmt == '.')
        {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9')
            {
                fmt++;
            }
        }

        int long_count = 0;
        if (*fmt == 'l')
        {
            fmt++;
            long_count = 1;
            if (*fmt == 'l')
            {
                fmt++;
                long_count = 2;
            }
        }
        else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j')
        {
            fmt++;
            long_count = 2;
        }

        switch (*fmt)
        {
            case 'c':
                u_append_char(buf, size, &pos, (char)va_arg(aq, int));
                break;
            case 's':
                u_append_string(buf, size, &pos, va_arg(aq, const char *));
                break;
            case 'd':
            case 'i':
                if (long_count >= 2)
                {
                    u_append_signed(buf, size, &pos, va_arg(aq, long long));
                }
                else if (long_count == 1)
                {
                    u_append_signed(buf, size, &pos, va_arg(aq, long));
                }
                else
                {
                    u_append_signed(buf, size, &pos, va_arg(aq, int));
                }
                break;
            case 'u':
                if (long_count >= 2)
                {
                    u_append_unsigned(buf, size, &pos, va_arg(aq, unsigned long long), 10, 0);
                }
                else if (long_count == 1)
                {
                    u_append_unsigned(buf, size, &pos, va_arg(aq, unsigned long), 10, 0);
                }
                else
                {
                    u_append_unsigned(buf, size, &pos, va_arg(aq, unsigned int), 10, 0);
                }
                break;
            case 'x':
            case 'X':
                if (long_count >= 2)
                {
                    u_append_unsigned(buf, size, &pos, va_arg(aq, unsigned long long), 16, *fmt == 'X');
                }
                else if (long_count == 1)
                {
                    u_append_unsigned(buf, size, &pos, va_arg(aq, unsigned long), 16, *fmt == 'X');
                }
                else
                {
                    u_append_unsigned(buf, size, &pos, va_arg(aq, unsigned int), 16, *fmt == 'X');
                }
                break;
            case 'p':
                u_append_string(buf, size, &pos, "0x");
                u_append_unsigned(buf, size, &pos, (unsigned long long)(uintptr_t)va_arg(aq, void *), 16, 0);
                break;
            case 'o':
                if (long_count >= 2)
                {
                    u_append_unsigned(buf, size, &pos, va_arg(aq, unsigned long long), 8, 0);
                }
                else if (long_count == 1)
                {
                    u_append_unsigned(buf, size, &pos, va_arg(aq, unsigned long), 8, 0);
                }
                else
                {
                    u_append_unsigned(buf, size, &pos, va_arg(aq, unsigned int), 8, 0);
                }
                break;
            default:
                u_append_char(buf, size, &pos, '%');
                u_append_char(buf, size, &pos, *fmt);
                break;
        }
        if (*fmt)
        {
            fmt++;
        }
    }

    if (buf && size > 0)
    {
        if (pos < size)
        {
            buf[pos] = '\0';
        }
        else
        {
            buf[size - 1] = '\0';
        }
    }
    va_end(aq);
    return (int)pos;
}

FILE *fopen(const char *path, const char *mode)
{
    FILE *fp;
    int flags;
    int fd;

    flags = u_stdio_mode_to_flags(mode);
    if (flags < 0)
    {
        errno = EINVAL;
        return 0;
    }

    fd = open(path, flags, 0644);
    if (fd < 0)
    {
        return 0;
    }

    fp = u_file_alloc(fd);
    if (!fp)
    {
        close(fd);
        return 0;
    }

    return fp;
}

FILE *fopen64(const char *path, const char *mode)
{
    return fopen(path, mode);
}

FILE *fdopen(int fd, const char *mode)
{
    FILE *fp;

    if (fd < 0 || !mode)
    {
        errno = EINVAL;
        return 0;
    }

    fp = u_file_alloc(fd);
    if (!fp)
    {
        return 0;
    }

    return fp;
}

int fclose(FILE *fp)
{
    if (!fp)
    {
        errno = EINVAL;
        return -1;
    }

    if (fp == stdin || fp == stdout || fp == stderr)
    {
        return 0;
    }

    if (UFP(fp)->fd >= 0)
    {
        close(UFP(fp)->fd);
    }
    free(fp);
    return 0;
}

int fflush(FILE *fp)
{
    (void)fp;
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    size_t want;
    size_t done;
    ssize_t ret;

    if (!fp || !ptr || !size || !nmemb)
    {
        return 0;
    }

    want = size * nmemb;
    done = 0;
    while (done < want)
    {
        ret = read(UFP(fp)->fd, (uint8_t *)ptr + done, want - done);
        if (ret <= 0)
        {
            if (ret == 0)
            {
                UFP(fp)->eof = 1;
            }
            else
            {
                UFP(fp)->err = 1;
            }
            break;
        }
        done += (size_t)ret;
    }

    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    size_t want;
    size_t done;
    ssize_t ret;

    if (!fp || !ptr || !size || !nmemb)
    {
        return 0;
    }

    want = size * nmemb;
    done = 0;
    while (done < want)
    {
        ret = write(UFP(fp)->fd, (const uint8_t *)ptr + done, want - done);
        if (ret <= 0)
        {
            UFP(fp)->err = 1;
            break;
        }
        done += (size_t)ret;
    }

    return done / size;
}

int fputs(const char *s, FILE *fp)
{
    size_t len;
    size_t written;

    if (!s || !fp)
    {
        errno = EINVAL;
        return EOF;
    }

    len = strlen(s);
    written = fwrite(s, 1, len, fp);
    if (written != len)
    {
        return EOF;
    }
    return 0;
}

int puts(const char *s)
{
    int ret;

    if (!s)
    {
        errno = EINVAL;
        return EOF;
    }

    ret = fputs(s, stdout);
    if (ret == EOF)
    {
        return EOF;
    }
    if (fputc('\n', stdout) == EOF)
    {
        return EOF;
    }
    return 0;
}

int fseek(FILE *fp, long offset, int whence)
{
    if (!fp)
    {
        errno = EINVAL;
        return -1;
    }

    if (lseek(UFP(fp)->fd, (off_t)offset, whence) < 0)
    {
        UFP(fp)->err = 1;
        return -1;
    }

    UFP(fp)->eof = 0;
    UFP(fp)->has_ungot = 0;
    return 0;
}

long ftell(FILE *fp)
{
    off_t ret;

    if (!fp)
    {
        errno = EINVAL;
        return -1;
    }

    ret = lseek(UFP(fp)->fd, 0, SEEK_CUR);
    if (ret < 0)
    {
        UFP(fp)->err = 1;
        return -1;
    }

    return (long)ret;
}

void rewind(FILE *fp)
{
    /*
     * 标准库里的 rewind 是一个无返回值的便利封装：
     * 它相当于把文件位置重置到开头，并清掉 EOF / 错误状态。
     * CPython 的启动路径会依赖这个行为。
     */
    if (!fp)
    {
        return;
    }

    (void)fseek(fp, 0L, SEEK_SET);
    clearerr(fp);
}

int fgetc(FILE *fp)
{
    unsigned char ch;
    ssize_t ret;

    if (!fp)
    {
        errno = EINVAL;
        return EOF;
    }

    if (UFP(fp)->has_ungot)
    {
        UFP(fp)->has_ungot = 0;
        return UFP(fp)->ungot;
    }

    ret = read(UFP(fp)->fd, &ch, 1);
    if (ret <= 0)
    {
        if (ret == 0)
        {
            UFP(fp)->eof = 1;
        }
        else
        {
            UFP(fp)->err = 1;
        }
        return EOF;
    }

    return (int)ch;
}

int fputc(int ch, FILE *fp)
{
    unsigned char c;
    ssize_t ret;

    if (!fp)
    {
        errno = EINVAL;
        return EOF;
    }

    c = (unsigned char)ch;
    ret = write(UFP(fp)->fd, &c, 1);
    if (ret != 1)
    {
        UFP(fp)->err = 1;
        return EOF;
    }

    return (int)c;
}

int ungetc(int ch, FILE *fp)
{
    if (!fp)
    {
        errno = EINVAL;
        return EOF;
    }

    UFP(fp)->ungot = ch & 0xFF;
    UFP(fp)->has_ungot = 1;
    UFP(fp)->eof = 0;
    return ch;
}

int feof(FILE *fp)
{
    return fp ? UFP(fp)->eof : 0;
}

int ferror(FILE *fp)
{
    return fp ? UFP(fp)->err : 0;
}

void clearerr(FILE *fp)
{
    if (!fp)
    {
        return;
    }

    UFP(fp)->eof = 0;
    UFP(fp)->err = 0;
}

int fileno(FILE *fp)
{
    if (!fp)
    {
        errno = EINVAL;
        return -1;
    }
    return UFP(fp)->fd;
}

int setvbuf(FILE *fp, char *buf, int mode, size_t size)
{
    (void)fp;
    (void)buf;
    (void)mode;
    (void)size;
    return 0;
}

void setbuf(FILE *fp, char *buf)
{
    (void)fp;
    (void)buf;
}

void flockfile(FILE *fp)
{
    (void)fp;
}

void funlockfile(FILE *fp)
{
    (void)fp;
}

char *fgets(char *s, int size, FILE *fp)
{
    int ch;
    int i;

    if (!s || size <= 0 || !fp)
    {
        errno = EINVAL;
        return 0;
    }

    for (i = 0; i + 1 < size; i++)
    {
        ch = fgetc(fp);
        if (ch == EOF)
        {
            break;
        }
        s[i] = (char)ch;
        if (ch == '\n')
        {
            i++;
            break;
        }
    }

    if (i == 0)
    {
        return 0;
    }

    s[i] = '\0';
    return s;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    va_list aq;
    int ret;

    va_copy(aq, ap);
    ret = u_vsnprintf_impl(buf, size, fmt, aq);
    va_end(aq);
    return ret;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    int ret;
    va_list ap;

    va_start(ap, fmt);
    ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *buf, const char *fmt, ...)
{
    int ret;
    va_list ap;

    va_start(ap, fmt);
    ret = vsprintf(buf, fmt, ap);
    va_end(ap);
    return ret;
}

int vfprintf(FILE *fp, const char *fmt, va_list ap)
{
    int need;
    char *buf;
    int ret;
    va_list aq;

    if (!fp)
    {
        errno = EINVAL;
        return -1;
    }

    va_copy(aq, ap);
    need = vsnprintf(0, 0, fmt, aq);
    va_end(aq);
    if (need < 0)
    {
        return need;
    }

    buf = (char *)malloc((size_t)need + 1U);
    if (!buf)
    {
        errno = ENOMEM;
        return -1;
    }

    va_copy(aq, ap);
    ret = vsnprintf(buf, (size_t)need + 1U, fmt, aq);
    va_end(aq);
    if (ret >= 0)
    {
        if (fwrite(buf, 1, (size_t)ret, fp) != (size_t)ret)
        {
            ret = -1;
        }
    }

    free(buf);
    return ret;
}

int fprintf(FILE *fp, const char *fmt, ...)
{
    int ret;
    va_list ap;

    va_start(ap, fmt);
    ret = vfprintf(fp, fmt, ap);
    va_end(ap);
    return ret;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...)
{
    int ret;
    va_list ap;

    va_start(ap, fmt);
    ret = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return ret;
}

void perror(const char *s)
{
    if (s && *s)
    {
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    }
    else
    {
        fprintf(stderr, "%s\n", strerror(errno));
    }
}
