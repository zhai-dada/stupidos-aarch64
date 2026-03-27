#include "libbb.h"

/*
 * BusyBox vi 专用的最小兼容实现。
 *
 * 目标只有一个：让 upstream 的 editors/vi.c 可以在 stupidos 用户态里直接跑起来，
 * 而不是重新写一个“像 vi 的东西”。
 */

const char bb_msg_memory_exhausted[] = "out of memory";
const char bb_msg_write_error[] = "write error";
const char bb_msg_requires_arg[] = "%s requires an argument";
const char bb_msg_invalid_arg_to[] = "invalid argument '%s' to '%s'";
const char bb_msg_invalid_date[] = "invalid date '%s'";
const char bb_msg_standard_input[] = "standard input";
const char bb_msg_standard_output[] = "standard output";
smallint bb_got_signal;
int optind = 1;
int opterr;
int optopt;
char *optarg;

void bb_simple_error_msg(const char *s)
{
    if (s && *s)
    {
        fprintf(stderr, "%s\n", s);
    }
    fflush(stderr);
}

void bb_simple_error_msg_and_die(const char *s)
{
    bb_simple_error_msg(s);
    u_exit(1);
}

void bb_simple_perror_msg(const char *s)
{
    if (s && *s)
    {
        perror(s);
    }
    else
    {
        perror("busybox");
    }
}

void bb_simple_perror_msg_and_die(const char *s)
{
    bb_simple_perror_msg(s);
    u_exit(1);
}

static void bb_vmsg_impl(const char *fmt, va_list ap, int with_errno, int die)
{
    if (fmt && *fmt)
    {
        vfprintf(stderr, fmt, ap);
        if (with_errno)
        {
            fprintf(stderr, ": %s", strerror(errno));
        }
        fputc('\n', stderr);
    }
    else if (with_errno)
    {
        fprintf(stderr, "%s\n", strerror(errno));
    }
    fflush(stderr);
    if (die)
    {
        u_exit(1);
    }
}

void bb_error_msg(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    bb_vmsg_impl(fmt, ap, 0, 0);
    va_end(ap);
}

void bb_error_msg_and_die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    bb_vmsg_impl(fmt, ap, 0, 1);
    va_end(ap);
}

void bb_perror_msg(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    bb_vmsg_impl(fmt, ap, 1, 0);
    va_end(ap);
}

void bb_perror_msg_and_die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    bb_vmsg_impl(fmt, ap, 1, 1);
    va_end(ap);
}

void bb_verror_msg(const char *fmt, va_list ap)
{
    bb_vmsg_impl(fmt, ap, 0, 0);
}

void bb_show_usage(void)
{
    /*
     * 只给 vi 使用的最小 usage 文本。
     * BusyBox 原版会根据 applet 元数据生成，这里直接硬编码即可。
     */
    fprintf(stderr, "usage: vi [-c CMD] [-R] [-H] [FILE]...\n");
    fflush(stderr);
    u_exit(1);
}

int bb_putchar(int ch)
{
    unsigned char c = (unsigned char)ch;
    return (int)write(STDOUT_FILENO, &c, 1);
}

int bb_putchar_stderr(char ch)
{
    unsigned char c = (unsigned char)ch;
    return (int)write(STDERR_FILENO, &c, 1);
}

int fflush_all(void)
{
    return fflush(NULL);
}

int fputs_stdout(const char *s)
{
    return fputs(s ? s : "", stdout);
}

void *xmalloc(size_t size)
{
    void *ptr = malloc(size ? size : 1);
    if (!ptr)
    {
        bb_simple_error_msg_and_die(bb_msg_memory_exhausted);
    }
    return ptr;
}

void *xrealloc(void *ptr, size_t size)
{
    void *nptr = realloc(ptr, size ? size : 1);
    if (!nptr)
    {
        bb_simple_error_msg_and_die(bb_msg_memory_exhausted);
    }
    return nptr;
}

void *xzalloc(size_t size)
{
    void *ptr = xmalloc(size);
    memset(ptr, 0, size);
    return ptr;
}

char *xstrdup(const char *s)
{
    char *dup;

    if (!s)
    {
        return NULL;
    }
    dup = strdup(s);
    if (!dup)
    {
        bb_simple_error_msg_and_die(bb_msg_memory_exhausted);
    }
    return dup;
}

char *xstrndup(const char *s, size_t n)
{
    char *dup;

    if (!s)
    {
        return NULL;
    }
    dup = strndup(s, n);
    if (!dup)
    {
        bb_simple_error_msg_and_die(bb_msg_memory_exhausted);
    }
    return dup;
}

char *xasprintf(const char *format, ...)
{
    va_list ap;
    va_list ap2;
    int need;
    char *buf;

    va_start(ap, format);
    va_copy(ap2, ap);
    need = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    if (need < 0)
    {
        va_end(ap2);
        bb_simple_error_msg_and_die("vsnprintf failed");
    }
    buf = xmalloc((size_t)need + 1U);
    if (vsnprintf(buf, (size_t)need + 1U, format, ap2) < 0)
    {
        free(buf);
        va_end(ap2);
        bb_simple_error_msg_and_die("vsnprintf failed");
    }
    va_end(ap2);
    return buf;
}

unsigned bb_strtou(const char *arg, char **endp, int base)
{
    unsigned long v;
    char *end_local = NULL;

    errno = 0;
    v = strtoul(arg ? arg : "", &end_local, base);
    if (endp)
    {
        *endp = end_local;
    }
    if (!arg || end_local == arg || errno != 0)
    {
        return 0;
    }
    return (unsigned)v;
}

unsigned getopt32(char **argv, const char *applet_opts, ...)
{
    struct opt_desc
    {
        char opt;
        bool has_arg;
        void **sink;
    } opts[32];
    unsigned opt_count;
    unsigned flags;
    va_list ap;
    const char *p;
    int argc;
    int i;

    opt_count = 0;
    flags = 0;

    va_start(ap, applet_opts);
    for (p = applet_opts; p && *p != '\0'; p++)
    {
        if (*p == ':' || *p == '*' || *p == '^' || *p == '+')
        {
            continue;
        }
        if (!isprint((unsigned char)*p))
        {
            continue;
        }
        if (opt_count >= ARRAY_SIZE(opts))
        {
            break;
        }
        opts[opt_count].opt = *p;
        opts[opt_count].has_arg = (p[1] == ':' || p[1] == '*');
        opts[opt_count].sink = opts[opt_count].has_arg ? va_arg(ap, void **) : NULL;
        opt_count++;
        if (opts[opt_count - 1].has_arg)
        {
            p++;
        }
    }
    va_end(ap);

    argc = 1;
    while (argv[argc])
    {
        char *arg = argv[argc];
        char *optarg_local;
        bool found;

        if (arg[0] != '-' || arg[1] == '\0')
        {
            break;
        }
        if (strcmp(arg, "--") == 0)
        {
            argc++;
            break;
        }

        for (i = 1; arg[i] != '\0'; i++)
        {
            unsigned j;

            found = false;
            for (j = 0; j < opt_count; j++)
            {
                if (opts[j].opt == arg[i])
                {
                    found = true;
                    flags |= (1U << j);
                    if (opts[j].has_arg)
                    {
                        if (arg[i + 1] != '\0')
                        {
                            optarg_local = &arg[i + 1];
                            i = (int)strlen(arg) - 1;
                        }
                        else
                        {
                            argc++;
                            optarg_local = argv[argc];
                            if (!optarg_local)
                            {
                                bb_show_usage();
                            }
                        }

                        if (opts[j].sink)
                        {
                            llist_add_to_end((llist_t **)opts[j].sink, xstrdup(optarg_local));
                        }
                        break;
                    }
                    break;
                }
            }
            if (!found)
            {
                bb_show_usage();
            }
        }
        argc++;
    }

    optind = argc;
    return flags;
}

ssize_t safe_read(int fd, void *buf, size_t len)
{
    ssize_t ret;

    do
    {
        ret = read(fd, buf, len);
    }
    while (ret < 0 && errno == EINTR);

    return ret;
}

int safe_poll(struct pollfd *ufds, nfds_t nfds, int timeout)
{
    int ret;

    do
    {
        ret = poll(ufds, nfds, timeout);
    }
    while (ret < 0 && (errno == EINTR || errno == ENOMEM));

    return ret;
}

int get_terminal_width(int fd)
{
    unsigned width = 80;
    unsigned height = 24;

    (void)get_terminal_width_height(fd, &width, &height);
    return (int)width;
}

void *xrealloc_vector_helper(void *vector, unsigned sizeof_and_shift, int idx)
{
    int mask;
    size_t elem_size;

    mask = 1 << (uint8_t)sizeof_and_shift;
    if (!(idx & (mask - 1)))
    {
        elem_size = sizeof_and_shift >> 8;
        vector = xrealloc(vector, elem_size * (size_t)(idx + mask + 1));
        memset((char *)vector + (elem_size * (size_t)idx), 0, elem_size * (size_t)(mask + 1));
    }
    return vector;
}

int64_t read_key(int fd, char *buffer, int timeout)
{
    return safe_read_key(fd, buffer, timeout);
}

ssize_t nonblock_immune_read(int fd, void *buf, size_t count)
{
    struct pollfd pfd[1];
    ssize_t n;

    while (1)
    {
        n = safe_read(fd, buf, count);
        if (n >= 0 || errno != EAGAIN)
        {
            return n;
        }

        pfd[0].fd = fd;
        pfd[0].events = POLLIN;
        (void)safe_poll(pfd, 1, -1);
    }
}

int set_termios_to_raw(int fd, struct termios *oldterm, int flags)
{
    struct termios raw;

    if (!oldterm)
    {
        errno = EINVAL;
        return -1;
    }

    if (tcgetattr(fd, oldterm) < 0)
    {
        return -1;
    }

    raw = *oldterm;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON | IXOFF);
    if (flags & TERMIOS_RAW_CRNL)
    {
        raw.c_iflag |= ICRNL;
    }
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= (tcflag_t)(CS8 | CREAD);
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    return tcsetattr(fd, TCSANOW, &raw);
}

int tcsetattr_stdin_TCSANOW(const struct termios *tp)
{
    return tcsetattr(STDIN_FILENO, TCSANOW, tp);
}

int get_terminal_width_height(int fd, unsigned *width, unsigned *height)
{
    struct winsize ws;

    if (ioctl(fd, TIOCGWINSZ, &ws) == 0)
    {
        if (width)
        {
            *width = ws.ws_col ? ws.ws_col : 80U;
        }
        if (height)
        {
            *height = ws.ws_row ? ws.ws_row : 24U;
        }
        return 0;
    }

    if (width)
    {
        *width = 80U;
    }
    if (height)
    {
        *height = 24U;
    }
    return -1;
}

char *concat_path_file(const char *path, const char *filename)
{
    size_t path_len;
    size_t file_len;
    size_t need;
    char *out;
    size_t pos;

    if (!path || !*path)
    {
        return xstrdup(filename ? filename : "");
    }
    if (!filename || !*filename)
    {
        return xstrdup(path);
    }
    if (filename[0] == '/')
    {
        return xstrdup(filename);
    }

    path_len = strlen(path);
    file_len = strlen(filename);
    need = path_len + file_len + 2U;
    out = xmalloc(need);
    memcpy(out, path, path_len);
    pos = path_len;
    if (pos > 0 && out[pos - 1] != '/')
    {
        out[pos++] = '/';
    }
    memcpy(out + pos, filename, file_len);
    pos += file_len;
    out[pos] = '\0';
    return out;
}

char *skip_whitespace(const char *s)
{
    while (s && (*s == ' ' || (unsigned char)(*s - 9) <= (13 - 9)))
    {
        s++;
    }
    return (char *)s;
}

char *skip_non_whitespace(const char *s)
{
    while (s && *s != '\0' && *s != ' ' && (unsigned char)(*s - 9) > (13 - 9))
    {
        s++;
    }
    return (char *)s;
}

char *strchrnul(const char *s, int c)
{
    char *p = strchr(s, c);
    return p ? p : (char *)(s + strlen(s));
}

int index_in_strings(const char *strings, const char *key)
{
    const char *p;
    int index = 0;

    for (p = strings; p && *p; p += strlen(p) + 1, index++)
    {
        if (strcmp(p, key) == 0)
        {
            return index;
        }
    }
    return -1;
}

ssize_t full_read(int fd, void *buf, size_t len)
{
    size_t done = 0;

    while (done < len)
    {
        ssize_t rd = safe_read(fd, (char *)buf + done, len - done);
        if (rd <= 0)
        {
            if (done == 0)
            {
                return rd;
            }
            break;
        }
        done += (size_t)rd;
    }
    return (ssize_t)done;
}

ssize_t full_write(int fd, const void *buf, size_t len)
{
    size_t done = 0;

    while (done < len)
    {
        ssize_t wr = write(fd, (const char *)buf + done, len - done);
        if (wr <= 0)
        {
            if (done == 0)
            {
                return wr;
            }
            break;
        }
        done += (size_t)wr;
    }
    return (ssize_t)done;
}

char *xmalloc_open_read_close(const char *filename, size_t *maxsz_p)
{
    int fd;
    char *buf = NULL;
    size_t size = 0;
    size_t cap = 256;

    fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        return NULL;
    }

    buf = xmalloc(cap + 1U);
    while (1)
    {
        ssize_t rd = read(fd, buf + size, cap - size);
        if (rd < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            free(buf);
            close(fd);
            return NULL;
        }
        if (rd == 0)
        {
            break;
        }
        size += (size_t)rd;
        if (size == cap)
        {
            cap *= 2U;
            buf = xrealloc(buf, cap + 1U);
        }
    }
    close(fd);
    buf[size] = '\0';
    if (maxsz_p)
    {
        *maxsz_p = size;
    }
    return buf;
}

llist_t *llist_add_to_end(llist_t **list_head, void *data)
{
    llist_t *node;
    llist_t *tail;

    node = xmalloc(sizeof(*node));
    node->data = data;
    node->link = NULL;

    if (!list_head)
    {
        return node;
    }

    if (!*list_head)
    {
        *list_head = node;
        return node;
    }

    tail = *list_head;
    while (tail->link)
    {
        tail = tail->link;
    }
    tail->link = node;
    return node;
}

void *llist_pop(llist_t **head)
{
    llist_t *node;
    void *data;

    if (!head || !*head)
    {
        return NULL;
    }

    node = *head;
    *head = node->link;
    data = node->data;
    free(node);
    return data;
}

static int64_t bb_read_key_store(char *buffer, const char *seq, size_t len)
{
    size_t room;

    room = KEYCODE_BUFFER_SIZE - 1U - (unsigned char)buffer[0];
    if (len > room)
    {
        len = room;
    }
    memcpy(buffer + 1 + (unsigned char)buffer[0], seq, len);
    buffer[0] = (char)((unsigned char)buffer[0] + len);
    return 0;
}

void read_key_ungets(char *buffer, const char *str, unsigned len)
{
    (void)bb_read_key_store(buffer, str, len);
}

int64_t safe_read_key(int fd, char *buffer, int timeout)
{
    struct pollfd pfd;
    ssize_t n;
    unsigned char c;
    char escbuf[KEYCODE_BUFFER_SIZE];
    size_t esc_len = 0;

    if (!buffer)
    {
        errno = EINVAL;
        return -1;
    }

    pfd.fd = fd;
    pfd.events = POLLIN;

    if ((unsigned char)buffer[0] > 0)
    {
        c = (unsigned char)buffer[1];
        memmove(buffer + 1, buffer + 2, (unsigned char)buffer[0] - 1U);
        buffer[0]--;
        return c;
    }

    errno = 0;
    if (timeout >= 0)
    {
        int pret = safe_poll(&pfd, 1, timeout);
        if (pret <= 0)
        {
            if (pret == 0)
            {
                errno = EAGAIN;
            }
            return -1;
        }
    }

    n = safe_read(fd, &c, 1);
    if (n <= 0)
    {
        return -1;
    }

    if (c != 27)
    {
        return c;
    }

    while (esc_len < sizeof(escbuf))
    {
        /*
         * ESC 后的“是否还有续字节”确认不要等太久。
         * 这里压到 2ms，可以明显减少 BusyBox vi / lineedit 的按键滞后感，
         * 同时仍能容纳常见的方向键 ANSI 序列。
         */
        int pret = safe_poll(&pfd, 1, 2);
        if (pret <= 0)
        {
            break;
        }
        n = safe_read(fd, &escbuf[esc_len], 1);
        if (n <= 0)
        {
            break;
        }
        esc_len++;

        if (esc_len == 2 && escbuf[0] == '[')
        {
            switch (escbuf[1])
            {
            case 'A': return KEYCODE_UP;
            case 'B': return KEYCODE_DOWN;
            case 'C': return KEYCODE_RIGHT;
            case 'D': return KEYCODE_LEFT;
            case 'H': return KEYCODE_HOME;
            case 'F': return KEYCODE_END;
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
                break;
            default:
                break;
            }
        }
        if (esc_len == 3 && escbuf[0] == '[')
        {
            if (escbuf[1] == '1' && escbuf[2] == '~')
            {
                return KEYCODE_HOME;
            }
            if (escbuf[1] == '2' && escbuf[2] == '~')
            {
                return KEYCODE_INSERT;
            }
            if (escbuf[1] == '3' && escbuf[2] == '~')
            {
                return KEYCODE_DELETE;
            }
            if (escbuf[1] == '4' && escbuf[2] == '~')
            {
                return KEYCODE_END;
            }
            if (escbuf[1] == '5' && escbuf[2] == '~')
            {
                return KEYCODE_PAGEUP;
            }
            if (escbuf[1] == '6' && escbuf[2] == '~')
            {
                return KEYCODE_PAGEDOWN;
            }
            if (escbuf[1] == '7' && escbuf[2] == '~')
            {
                return KEYCODE_HOME;
            }
            if (escbuf[1] == '8' && escbuf[2] == '~')
            {
                return KEYCODE_END;
            }
        }
        if (esc_len == 2 && escbuf[0] == 'O')
        {
            switch (escbuf[1])
            {
            case 'A': return KEYCODE_UP;
            case 'B': return KEYCODE_DOWN;
            case 'C': return KEYCODE_RIGHT;
            case 'D': return KEYCODE_LEFT;
            case 'H': return KEYCODE_HOME;
            case 'F': return KEYCODE_END;
            default:
                break;
            }
        }
    }

    if (esc_len > 0)
    {
        if ((unsigned char)buffer[0] + esc_len > KEYCODE_BUFFER_SIZE - 1U)
        {
            esc_len = KEYCODE_BUFFER_SIZE - 1U - (unsigned char)buffer[0];
        }
        if (esc_len > 0)
        {
            memcpy(buffer + 1 + (unsigned char)buffer[0], escbuf, esc_len);
            buffer[0] = (char)((unsigned char)buffer[0] + esc_len);
        }
    }

    return 27;
}
