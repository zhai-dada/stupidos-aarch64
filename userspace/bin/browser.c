#include "stupidos_user.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

/*
 * 一个尽量小但可用的网页浏览器：
 * - 先做“能打开网页并显示正文”的版本；
 * - 走 userspace socket + DNS + libc 兼容层；
 * - 用终端 ANSI 画一个简单 UI，而不是依赖额外图形子系统；
 * - 以后如果要接更完整的 GUI，只需要把渲染后端替换掉。
 */

#define BROWSER_URL_MAX          1024
#define BROWSER_HOST_MAX         256
#define BROWSER_PATH_MAX         1024
#define BROWSER_LINE_MAX         160
#define BROWSER_RECV_CHUNK       4096
#define BROWSER_WRAP_WIDTH       78
#define BROWSER_VIEW_ROWS        18
#define BROWSER_MAX_REDIRECTS    5
#define BROWSER_RECV_TIMEOUT_MS  2500

struct browser_url
{
    char scheme[8];
    char host[BROWSER_HOST_MAX];
    char path[BROWSER_PATH_MAX];
    uint16_t port;
    bool https;
};

struct browser_buffer
{
    char *data;
    size_t len;
    size_t cap;
};

struct browser_lines
{
    char **items;
    size_t count;
    size_t cap;
};

static struct termios browser_saved_termios;
static bool browser_saved_termios_valid;
static int browser_parse_url(const char *url, struct browser_url *out);

static void browser_puts(const char *s)
{
    if (!s)
    {
        return;
    }
    (void)write(STDOUT_FILENO, s, strlen(s));
}

static void browser_restore_terminal(void)
{
    if (!browser_saved_termios_valid)
    {
        return;
    }
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &browser_saved_termios);
    browser_saved_termios_valid = false;
    browser_puts("\x1b[?25h");
}

static void browser_enable_raw_terminal(void)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO))
    {
        browser_saved_termios_valid = false;
        return;
    }

    if (tcgetattr(STDIN_FILENO, &browser_saved_termios) < 0)
    {
        browser_saved_termios_valid = false;
        return;
    }

    raw = browser_saved_termios;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON | IXOFF);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= (tcflag_t)(CS8 | CREAD);
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
    {
        browser_saved_termios_valid = true;
        return;
    }

    browser_saved_termios_valid = false;
}

static void browser_buffer_free(struct browser_buffer *buf)
{
    if (!buf)
    {
        return;
    }
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static int browser_buffer_reserve(struct browser_buffer *buf, size_t need)
{
    size_t cap;
    char *ptr;

    if (!buf)
    {
        return -1;
    }

    if (need <= buf->cap)
    {
        return 0;
    }

    cap = buf->cap ? buf->cap : 4096U;
    while (cap < need)
    {
        if (cap > (size_t)-1 / 2U)
        {
            cap = need;
            break;
        }
        cap *= 2U;
    }

    ptr = (char *)realloc(buf->data, cap);
    if (!ptr)
    {
        return -1;
    }

    buf->data = ptr;
    buf->cap = cap;
    return 0;
}

static int browser_buffer_append(struct browser_buffer *buf, const void *data, size_t len)
{
    if (!buf || (!data && len))
    {
        return -1;
    }

    if (browser_buffer_reserve(buf, buf->len + len + 1U) < 0)
    {
        return -1;
    }

    if (len)
    {
        memcpy(buf->data + buf->len, data, len);
        buf->len += len;
    }
    buf->data[buf->len] = '\0';
    return 0;
}

static int browser_buffer_append_c(struct browser_buffer *buf, char ch)
{
    return browser_buffer_append(buf, &ch, 1U);
}

static void browser_lines_free(struct browser_lines *lines)
{
    size_t i;

    if (!lines)
    {
        return;
    }

    for (i = 0; i < lines->count; i++)
    {
        free(lines->items[i]);
    }
    free(lines->items);
    lines->items = NULL;
    lines->count = 0;
    lines->cap = 0;
}

static int browser_lines_push_copy(struct browser_lines *lines, const char *line, size_t len)
{
    char *copy;
    char **items;
    size_t cap;

    if (!lines || !line)
    {
        return -1;
    }

    if (lines->count + 1U > lines->cap)
    {
        cap = lines->cap ? lines->cap * 2U : 128U;
        items = (char **)realloc(lines->items, cap * sizeof(lines->items[0]));
        if (!items)
        {
            return -1;
        }
        lines->items = items;
        lines->cap = cap;
    }

    copy = (char *)malloc(len + 1U);
    if (!copy)
    {
        return -1;
    }
    memcpy(copy, line, len);
    copy[len] = '\0';
    lines->items[lines->count++] = copy;
    return 0;
}

static int browser_lines_append_text(struct browser_lines *lines, const char *text, size_t len, size_t width)
{
    char line[BROWSER_LINE_MAX];
    size_t pos;
    bool pending_space;
    size_t i;

    if (!lines || !text || width == 0U)
    {
        return -1;
    }

    pos = 0;
    pending_space = false;
    for (i = 0; i < len; i++)
    {
        char ch = text[i];

        if (ch == '\r')
        {
            continue;
        }
        if (ch == '\n')
        {
            if (pos > 0U)
            {
                if (browser_lines_push_copy(lines, line, pos) < 0)
                {
                    return -1;
                }
                pos = 0;
            }
            pending_space = false;
            continue;
        }

        if (isspace((unsigned char)ch))
        {
            pending_space = true;
            continue;
        }

        if (pending_space && pos > 0U && pos + 1U < sizeof(line))
        {
            line[pos++] = ' ';
            if (pos >= width)
            {
                if (browser_lines_push_copy(lines, line, pos) < 0)
                {
                    return -1;
                }
                pos = 0;
            }
        }
        pending_space = false;

        if (pos + 1U >= sizeof(line))
        {
            if (browser_lines_push_copy(lines, line, pos) < 0)
            {
                return -1;
            }
            pos = 0;
        }

        line[pos++] = ch;
        if (pos >= width)
        {
            if (browser_lines_push_copy(lines, line, pos) < 0)
            {
                return -1;
            }
            pos = 0;
        }
    }

    if (pos > 0U)
    {
        if (browser_lines_push_copy(lines, line, pos) < 0)
        {
            return -1;
        }
    }

    if (lines->count == 0U)
    {
        (void)browser_lines_push_copy(lines, "(empty page)", strlen("(empty page)"));
    }

    return 0;
}

static const char *browser_ascii_casestr(const char *haystack, const char *needle)
{
    size_t needle_len;
    size_t i;

    if (!haystack || !needle || !*needle)
    {
        return haystack;
    }

    needle_len = strlen(needle);
    for (i = 0; haystack[i] != '\0'; i++)
    {
        if (strncasecmp(&haystack[i], needle, needle_len) == 0)
        {
            return &haystack[i];
        }
    }
    return NULL;
}

static void browser_trim_inplace(char *s)
{
    char *start;
    char *end;

    if (!s)
    {
        return;
    }

    start = s;
    while (*start && isspace((unsigned char)*start))
    {
        start++;
    }

    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1]))
    {
        end--;
    }

    if (start != s)
    {
        memmove(s, start, (size_t)(end - start));
    }
    s[end - start] = '\0';
}

static int browser_decode_entity(const char *entity, char *out, size_t out_len)
{
    unsigned long value;

    if (!entity || !out || out_len == 0U)
    {
        return -1;
    }

    if (strcmp(entity, "amp") == 0)
    {
        out[0] = '&';
        out[1] = '\0';
        return 0;
    }
    if (strcmp(entity, "lt") == 0)
    {
        out[0] = '<';
        out[1] = '\0';
        return 0;
    }
    if (strcmp(entity, "gt") == 0)
    {
        out[0] = '>';
        out[1] = '\0';
        return 0;
    }
    if (strcmp(entity, "quot") == 0)
    {
        out[0] = '"';
        out[1] = '\0';
        return 0;
    }
    if (strcmp(entity, "apos") == 0)
    {
        out[0] = '\'';
        out[1] = '\0';
        return 0;
    }
    if (strcmp(entity, "nbsp") == 0)
    {
        out[0] = ' ';
        out[1] = '\0';
        return 0;
    }

    if (entity[0] == '#')
    {
        if (entity[1] == 'x' || entity[1] == 'X')
        {
            char *endp = NULL;

            errno = 0;
            value = strtoul(entity + 2, &endp, 16);
            if (errno == 0 && endp && *endp == '\0' && value <= 0x10ffffUL && out_len > 1U)
            {
                out[0] = (char)(value & 0xffUL);
                out[1] = '\0';
                return 0;
            }
        }
        else
        {
            char *endp = NULL;

            errno = 0;
            value = strtoul(entity + 1, &endp, 10);
            if (errno == 0 && endp && *endp == '\0' && value <= 0x10ffffUL && out_len > 1U)
            {
                out[0] = (char)(value & 0xffUL);
                out[1] = '\0';
                return 0;
            }
        }
    }

    return -1;
}

static void browser_emit_block_break(struct browser_buffer *out, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++)
    {
        (void)browser_buffer_append_c(out, '\n');
    }
}

static void browser_handle_tag(const char *tag, struct browser_buffer *out, bool *in_script, bool *in_style)
{
    char buf[64];
    size_t i;
    size_t j;
    bool closing;

    if (!tag || !out || !in_script || !in_style)
    {
        return;
    }

    i = 0;
    while (tag[i] && isspace((unsigned char)tag[i]))
    {
        i++;
    }

    closing = false;
    if (tag[i] == '/')
    {
        closing = true;
        i++;
    }

    j = 0;
    while (tag[i] && !isspace((unsigned char)tag[i]) && tag[i] != '>' && tag[i] != '/' && j + 1U < sizeof(buf))
    {
        buf[j++] = (char)tolower((unsigned char)tag[i]);
        i++;
    }
    buf[j] = '\0';

    if (strcmp(buf, "script") == 0)
    {
        if (closing)
        {
            *in_script = false;
            browser_emit_block_break(out, 1U);
        }
        else
        {
            *in_script = true;
        }
        return;
    }

    if (strcmp(buf, "style") == 0)
    {
        if (closing)
        {
            *in_style = false;
        }
        else
        {
            *in_style = true;
        }
        return;
    }

    if (strcmp(buf, "br") == 0 || strcmp(buf, "hr") == 0)
    {
        browser_emit_block_break(out, 1U);
        return;
    }

    if (strcmp(buf, "p") == 0 || strcmp(buf, "/p") == 0 ||
        strcmp(buf, "div") == 0 || strcmp(buf, "/div") == 0 ||
        strcmp(buf, "section") == 0 || strcmp(buf, "/section") == 0 ||
        strcmp(buf, "article") == 0 || strcmp(buf, "/article") == 0 ||
        strcmp(buf, "header") == 0 || strcmp(buf, "/header") == 0 ||
        strcmp(buf, "footer") == 0 || strcmp(buf, "/footer") == 0 ||
        strcmp(buf, "li") == 0 || strcmp(buf, "/li") == 0 ||
        strcmp(buf, "tr") == 0 || strcmp(buf, "/tr") == 0 ||
        strcmp(buf, "table") == 0 || strcmp(buf, "/table") == 0 ||
        strcmp(buf, "h1") == 0 || strcmp(buf, "/h1") == 0 ||
        strcmp(buf, "h2") == 0 || strcmp(buf, "/h2") == 0 ||
        strcmp(buf, "h3") == 0 || strcmp(buf, "/h3") == 0 ||
        strcmp(buf, "h4") == 0 || strcmp(buf, "/h4") == 0 ||
        strcmp(buf, "h5") == 0 || strcmp(buf, "/h5") == 0 ||
        strcmp(buf, "h6") == 0 || strcmp(buf, "/h6") == 0)
    {
        browser_emit_block_break(out, 2U);
        return;
    }

    if (strcmp(buf, "title") == 0)
    {
        browser_emit_block_break(out, 1U);
        return;
    }
}

static int browser_strip_html(const char *html, size_t html_len, struct browser_buffer *out)
{
    char tag[128];
    char entity[32];
    char decoded[8];
    size_t tag_len;
    size_t entity_len;
    size_t i;
    bool in_tag;
    bool in_entity;
    bool in_script;
    bool in_style;
    bool pending_space;

    if (!html || !out)
    {
        return -1;
    }

    in_tag = false;
    in_entity = false;
    in_script = false;
    in_style = false;
    pending_space = false;
    tag_len = 0;
    entity_len = 0;
    for (i = 0; i < html_len; i++)
    {
        char ch = html[i];

        if (in_script || in_style)
        {
            if (!in_tag && ch == '<')
            {
                in_tag = true;
                tag_len = 0;
                continue;
            }
            if (in_tag)
            {
                if (ch == '>')
                {
                    tag[tag_len] = '\0';
                    if ((in_script && strncmp(tag, "/script", 7) == 0) ||
                        (in_style && strncmp(tag, "/style", 6) == 0))
                    {
                        in_script = false;
                        in_style = false;
                    }
                    in_tag = false;
                    tag_len = 0;
                }
                else if (tag_len + 1U < sizeof(tag))
                {
                    tag[tag_len++] = ch;
                }
            }
            continue;
        }

        if (in_tag)
        {
            if (ch == '>')
            {
                tag[tag_len] = '\0';
                browser_handle_tag(tag, out, &in_script, &in_style);
                in_tag = false;
                tag_len = 0;
            }
            else if (tag_len + 1U < sizeof(tag))
            {
                tag[tag_len++] = ch;
            }
            continue;
        }

        if (in_entity)
        {
            if (ch == ';' || entity_len + 1U >= sizeof(entity))
            {
                entity[entity_len] = '\0';
                if (browser_decode_entity(entity, decoded, sizeof(decoded)) == 0)
                {
                    if (pending_space && out->len > 0U && out->data[out->len - 1U] != '\n' && out->data[out->len - 1U] != ' ')
                    {
                        (void)browser_buffer_append_c(out, ' ');
                    }
                    pending_space = false;
                    (void)browser_buffer_append(out, decoded, strlen(decoded));
                }
                in_entity = false;
                entity_len = 0;
            }
            else
            {
                entity[entity_len++] = ch;
            }
            continue;
        }

        if (ch == '<')
        {
            in_tag = true;
            tag_len = 0;
            continue;
        }

        if (ch == '&')
        {
            in_entity = true;
            entity_len = 0;
            continue;
        }

        if (isspace((unsigned char)ch))
        {
            pending_space = true;
            continue;
        }

        if (pending_space && out->len > 0U && out->data[out->len - 1U] != '\n' && out->data[out->len - 1U] != ' ')
        {
            (void)browser_buffer_append_c(out, ' ');
        }
        pending_space = false;
        (void)browser_buffer_append_c(out, ch);
    }

    if (out->len == 0U)
    {
        (void)browser_buffer_append(out, "(no text content)", strlen("(no text content)"));
    }

    return 0;
}

static int browser_decode_chunked(const char *body, size_t body_len, struct browser_buffer *out)
{
    size_t off;
    char *endp;
    unsigned long chunk_len;

    if (!body || !out)
    {
        return -1;
    }

    off = 0;
    while (off < body_len)
    {
        size_t line_start;
        size_t line_end;

        line_start = off;
        while (off < body_len && body[off] != '\n' && body[off] != '\r')
        {
            off++;
        }
        line_end = off;
        while (off < body_len && (body[off] == '\r' || body[off] == '\n'))
        {
            off++;
        }

        if (line_end <= line_start)
        {
            continue;
        }

        {
            char size_buf[32];
            size_t size_len = 0;
            size_t i;

            for (i = line_start; i < line_end && size_len + 1U < sizeof(size_buf); i++)
            {
                if (body[i] == ';')
                {
                    break;
                }
                size_buf[size_len++] = body[i];
            }
            size_buf[size_len] = '\0';
            chunk_len = strtoul(size_buf, &endp, 16);
            if (endp == size_buf || chunk_len == 0UL)
            {
                break;
            }
        }

        if (off + chunk_len > body_len)
        {
            chunk_len = body_len - off;
        }
        if (browser_buffer_append(out, body + off, (size_t)chunk_len) < 0)
        {
            return -1;
        }
        off += (size_t)chunk_len;

        while (off < body_len && (body[off] == '\r' || body[off] == '\n'))
        {
            off++;
        }
    }

    return 0;
}

static const char *browser_header_value(const char *headers, const char *name)
{
    size_t name_len;
    const char *p;

    if (!headers || !name)
    {
        return NULL;
    }

    name_len = strlen(name);
    p = headers;
    while (*p)
    {
        const char *line_end;
        const char *colon;

        line_end = strstr(p, "\r\n");
        if (!line_end)
        {
            line_end = p + strlen(p);
        }
        colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon && (size_t)(colon - p) == name_len && strncasecmp(p, name, name_len) == 0)
        {
            const char *value = colon + 1;

            while (value < line_end && isspace((unsigned char)*value))
            {
                value++;
            }
            return value;
        }

        if (*line_end == '\0')
        {
            break;
        }
        p = line_end + 2U;
    }

    return NULL;
}

static int browser_parse_status_code(const char *headers)
{
    const char *line_end;
    const char *p;

    if (!headers)
    {
        return -1;
    }

    line_end = strstr(headers, "\r\n");
    if (!line_end)
    {
        line_end = headers + strlen(headers);
    }

    p = headers;
    while (p < line_end && !isspace((unsigned char)*p))
    {
        p++;
    }
    while (p < line_end && isspace((unsigned char)*p))
    {
        p++;
    }
    if (p + 2 >= line_end)
    {
        return -1;
    }

    return (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
}

static int browser_resolve_url(const struct browser_url *base, const char *location, struct browser_url *out)
{
    char tmp[BROWSER_URL_MAX];

    if (!base || !location || !out)
    {
        return -1;
    }

    if (strncmp(location, "http://", 7) == 0 || strncmp(location, "https://", 8) == 0)
    {
        return browser_parse_url(location, out);
    }

    memset(out, 0, sizeof(*out));
    memcpy(out, base, sizeof(*out));

    if (location[0] == '/')
    {
        size_t len = strlen(location);
        if (len >= sizeof(out->path))
        {
            return -1;
        }
        memcpy(out->path, location, len + 1U);
        return 0;
    }

    {
        const char *slash = strrchr(base->path, '/');
        size_t dir_len = slash ? (size_t)(slash - base->path + 1U) : 1U;

        if (dir_len + strlen(location) >= sizeof(tmp))
        {
            return -1;
        }
        memcpy(tmp, base->path, dir_len);
        tmp[dir_len] = '\0';
        strcat(tmp, location);
        if (strlen(tmp) >= sizeof(out->path))
        {
            return -1;
        }
        strcpy(out->path, tmp);
    }

    return 0;
}

static int browser_parse_url(const char *url, struct browser_url *out)
{
    const char *p;
    const char *host_start;
    const char *host_end;
    const char *path_start;
    size_t host_len;
    size_t path_len;
    unsigned long port;
    char *endp;

    if (!url || !out)
    {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    if (strncmp(url, "http://", 7) == 0)
    {
        strcpy(out->scheme, "http");
        out->port = 80U;
        out->https = false;
        p = url + 7;
    }
    else if (strncmp(url, "https://", 8) == 0)
    {
        /*
         * 先明确告诉用户：当前是最小浏览器，不内置 TLS。
         * 后续如果接入 mbedtls / rustls，可以把这里直接扩展成 HTTPS。
         */
        strcpy(out->scheme, "https");
        out->port = 443U;
        out->https = true;
        p = url + 8;
    }
    else
    {
        return -1;
    }

    host_start = p;
    host_end = host_start;
    while (*host_end && *host_end != ':' && *host_end != '/' && *host_end != '?')
    {
        host_end++;
    }
    host_len = (size_t)(host_end - host_start);
    if (host_len == 0U || host_len >= sizeof(out->host))
    {
        return -1;
    }
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    if (*host_end == ':')
    {
        const char *port_start = host_end + 1;
        const char *port_end = port_start;

        while (*port_end && *port_end != '/' && *port_end != '?')
        {
            port_end++;
        }
        if (port_end == port_start)
        {
            return -1;
        }
        {
            char port_buf[16];
            size_t i;
            size_t len = (size_t)(port_end - port_start);

            if (len >= sizeof(port_buf))
            {
                return -1;
            }
            for (i = 0; i < len; i++)
            {
                port_buf[i] = port_start[i];
            }
            port_buf[len] = '\0';
            errno = 0;
            port = strtoul(port_buf, &endp, 10);
            if (errno != 0 || endp == port_buf || *endp != '\0' || port == 0UL || port > 65535UL)
            {
                return -1;
            }
            out->port = (uint16_t)port;
        }
        host_end = port_end;
    }

    path_start = (*host_end == '/' || *host_end == '?') ? host_end : "/";
    path_len = strlen(path_start);
    if (path_len == 0U || path_len >= sizeof(out->path))
    {
        strcpy(out->path, "/");
    }
    else
    {
        memcpy(out->path, path_start, path_len + 1U);
    }

    return 0;
}

static int browser_socket_send_all(int fd, const char *buf, size_t len)
{
    size_t off;

    off = 0;
    while (off < len)
    {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (n == 0)
        {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int browser_socket_wait_readable(int fd, int timeout_ms)
{
    struct pollfd pfd;
    int ret;

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0)
    {
        if (errno == EINTR)
        {
            return 1;
        }
        return -1;
    }

    if (ret == 0)
    {
        return 0;
    }

    if (pfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
    {
        return 2;
    }

    return 0;
}

static int browser_fetch_once(const struct browser_url *url, struct browser_buffer *response)
{
    struct addrinfo hints;
    struct addrinfo *res;
    struct addrinfo *ai;
    int fd;
    int ret;
    int ready;
    bool received_any;
    char portbuf[16];
    char req[2048];
    char recvbuf[BROWSER_RECV_CHUNK];

    if (!url || !response)
    {
        return -1;
    }

    if (url->https)
    {
        return -2;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)url->port);
    if (getaddrinfo(url->host, portbuf, &hints, &res) != 0)
    {
        return -1;
    }

    ret = -1;
    for (ai = res; ai; ai = ai->ai_next)
    {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
        {
            continue;
        }

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0)
        {
            close(fd);
            continue;
        }

        snprintf(req, sizeof(req),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "User-Agent: stupidos-browser/0.1\r\n"
                 "Accept: text/html, text/plain;q=0.9, */*;q=0.1\r\n"
                 "Accept-Encoding: identity\r\n"
                 "Connection: close\r\n\r\n",
                 url->path[0] ? url->path : "/",
                 url->host);

        if (browser_socket_send_all(fd, req, strlen(req)) < 0)
        {
            close(fd);
            continue;
        }

        received_any = false;
        while (1)
        {
            ready = browser_socket_wait_readable(fd, BROWSER_RECV_TIMEOUT_MS);
            if (ready == 0)
            {
                /*
                 * 对于一些服务器，主体已经收完但连接不会立刻关闭。
                 * 这里不再无限阻塞，先把现有数据交给上层解析。
                 */
                if (received_any)
                {
                    ret = 0;
                }
                else
                {
                    ret = -ETIMEDOUT;
                }
                break;
            }
            if (ready < 0)
            {
                ret = -1;
                break;
            }

            ssize_t n = recv(fd, recvbuf, sizeof(recvbuf), 0);
            if (n < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                ret = -1;
                break;
            }
            if (n == 0)
            {
                ret = 0;
                break;
            }
            received_any = true;
            if (browser_buffer_append(response, recvbuf, (size_t)n) < 0)
            {
                ret = -1;
                break;
            }
        }

        close(fd);
        if (ret == 0)
        {
            break;
        }
    }

    freeaddrinfo(res);
    return ret;
}

static int browser_extract_response(struct browser_buffer *raw,
                                    struct browser_buffer *body,
                                    int *status_code,
                                    char *content_type,
                                    size_t content_type_len,
                                    char *location,
                                    size_t location_len,
                                    bool *chunked)
{
    char *headers;
    char *body_ptr;
    char *sep;
    const char *val;

    if (!raw || !raw->data || raw->len == 0U || !body || !status_code || !content_type || !location || !chunked)
    {
        return -1;
    }

    sep = strstr(raw->data, "\r\n\r\n");
    if (!sep)
    {
        sep = strstr(raw->data, "\n\n");
        if (sep)
        {
            body_ptr = sep + 2;
        }
    }
    else
    {
        body_ptr = sep + 4;
    }

    if (!sep)
    {
        return -1;
    }

    *status_code = browser_parse_status_code(raw->data);
    if (*status_code < 0)
    {
        return -1;
    }

    headers = raw->data;
    *sep = '\0';

    content_type[0] = '\0';
    location[0] = '\0';
    *chunked = false;

    val = browser_header_value(headers, "Content-Type");
    if (val)
    {
        strncpy(content_type, val, content_type_len - 1U);
        content_type[content_type_len - 1U] = '\0';
        browser_trim_inplace(content_type);
    }

    val = browser_header_value(headers, "Location");
    if (val)
    {
        strncpy(location, val, location_len - 1U);
        location[location_len - 1U] = '\0';
        browser_trim_inplace(location);
    }

    val = browser_header_value(headers, "Transfer-Encoding");
    if (val && browser_ascii_casestr(val, "chunked"))
    {
        *chunked = true;
    }

    if (*chunked)
    {
        if (browser_decode_chunked(body_ptr, raw->len - (size_t)(body_ptr - raw->data), body) < 0)
        {
            return -1;
        }
    }
    else
    {
        if (browser_buffer_append(body, body_ptr, raw->len - (size_t)(body_ptr - raw->data)) < 0)
        {
            return -1;
        }
    }

    return 0;
}

static int browser_fetch_url(const struct browser_url *start_url,
                             struct browser_url *final_url,
                             struct browser_buffer *body,
                             char *content_type,
                             size_t content_type_len,
                             int *status_code)
{
    struct browser_url cur;
    struct browser_buffer raw;
    struct browser_buffer decoded;
    char location[BROWSER_URL_MAX];
    bool chunked;
    int rc;
    int redirects;

    if (!start_url || !final_url || !body || !content_type || !status_code)
    {
        return -1;
    }

    cur = *start_url;
    redirects = 0;
    memset(&raw, 0, sizeof(raw));
    memset(&decoded, 0, sizeof(decoded));
    while (redirects <= BROWSER_MAX_REDIRECTS)
    {
        browser_buffer_free(&raw);
        browser_buffer_free(&decoded);

        rc = browser_fetch_once(&cur, &raw);
        if (rc < 0)
        {
            browser_buffer_free(&raw);
            browser_buffer_free(&decoded);
            return rc;
        }

        location[0] = '\0';
        rc = browser_extract_response(&raw, &decoded, status_code, content_type, content_type_len, location, sizeof(location), &chunked);
        browser_buffer_free(&raw);
        if (rc < 0)
        {
            browser_buffer_free(&decoded);
            return -1;
        }

        if ((*status_code >= 300 && *status_code < 400) && location[0] != '\0')
        {
            struct browser_url next;

            if (browser_resolve_url(&cur, location, &next) < 0)
            {
                browser_buffer_free(&decoded);
                return -1;
            }
            if (next.https)
            {
                /*
                 * 百度这类站点经常把 HTTP 入口重定向到 HTTPS。
                 * 当前浏览器尚未接入 TLS，所以这里要明确告诉上层“不是卡住了，
                 * 而是跳到了暂不支持的 HTTPS”。
                 */
                browser_buffer_free(&decoded);
                return -2;
            }
            cur = next;
            redirects++;
            browser_buffer_free(&decoded);
            continue;
        }

        *final_url = cur;
        *body = decoded;
        return 0;
    }

    return -1;
}

static void browser_clear_screen(void)
{
    browser_puts("\x1b[2J\x1b[H\x1b[?25l");
}

static size_t browser_fb_view_rows(const struct stupidos_fbinfo *fb)
{
    size_t usable_h;
    size_t rows;

    if (!fb || fb->y_charsize == 0U)
    {
        return BROWSER_VIEW_ROWS;
    }

    usable_h = (fb->height > (fb->y_charsize * 6U)) ? (fb->height - (fb->y_charsize * 6U)) : fb->height;
    rows = usable_h / fb->y_charsize;
    if (rows == 0U)
    {
        rows = 1U;
    }
    if (rows > 40U)
    {
        rows = 40U;
    }
    return rows;
}

static size_t browser_fb_wrap_cols(const struct stupidos_fbinfo *fb)
{
    size_t cols;

    if (!fb || fb->x_charsize == 0U)
    {
        return BROWSER_WRAP_WIDTH;
    }

    cols = (fb->width > 80U) ? ((fb->width - 80U) / fb->x_charsize) : BROWSER_WRAP_WIDTH;
    if (cols < 40U)
    {
        cols = 40U;
    }
    if (cols > 120U)
    {
        cols = 120U;
    }
    return cols;
}

static void browser_render_terminal(const struct browser_url *url,
                                    int status_code,
                                    const char *content_type,
                                    const struct browser_lines *lines,
                                    size_t scroll)
{
    size_t i;
    size_t row;
    char header[BROWSER_LINE_MAX];
    char status[BROWSER_LINE_MAX];
    size_t visible_rows;
    size_t total;

    total = lines ? lines->count : 0U;
    visible_rows = BROWSER_VIEW_ROWS;

    browser_clear_screen();

    snprintf(header, sizeof(header), " stupidos browser  %s://%s:%u%s ",
             url->https ? "https" : "http",
             url->host,
             (unsigned)url->port,
             url->path[0] ? url->path : "/");
    snprintf(status, sizeof(status), " status=%d  type=%s  lines=%u  scroll=%u  q=quit j/k=scroll r=reload ",
             status_code,
             content_type && content_type[0] ? content_type : "unknown",
             (unsigned)total,
             (unsigned)scroll);

    printf("\x1b[1;44m%-78.78s\x1b[0m\r\n", header);
    printf("\x1b[1;30;47m%-78.78s\x1b[0m\r\n", status);
    printf("\x1b[1;30;46m%-78.78s\x1b[0m\r\n", " arrows / space / PgUp / PgDn also work, browser is text-only for now ");

    for (row = 0; row < visible_rows; row++)
    {
        i = scroll + row;
        if (i < total && lines->items[i])
        {
            printf("%-78.78s\r\n", lines->items[i]);
        }
        else
        {
            browser_puts("\r\n");
        }
    }
}

static void browser_render_framebuffer(const struct browser_url *url,
                                       int status_code,
                                       const char *content_type,
                                       const struct browser_lines *lines,
                                       size_t scroll,
                                       const struct stupidos_fbinfo *fb)
{
    size_t row;
    size_t total;
    size_t view_rows;
    size_t cols;
    uint32_t title_bg = 0x00FFB000;
    uint32_t title_fg = 0x00000000;
    uint32_t panel_bg = 0x0018222D;
    uint32_t panel_fg = 0x00FFFFFF;
    uint32_t body_bg = 0x00111820;
    uint32_t body_fg = 0x00E8EEF5;
    uint32_t status_bg = 0x00133648;
    char title[BROWSER_LINE_MAX];
    char status[BROWSER_LINE_MAX];
    char hint[BROWSER_LINE_MAX];
    char linebuf[BROWSER_LINE_MAX];

    if (!fb || fb->width == 0U || fb->height == 0U)
    {
        return;
    }

    total = lines ? lines->count : 0U;
    view_rows = browser_fb_view_rows(fb);
    cols = browser_fb_wrap_cols(fb);

    (void)u_fbfill(0, 0, fb->width, fb->height, body_bg);
    (void)u_fbfill(0, 0, fb->width, fb->y_charsize * 3U, title_bg);
    (void)u_fbfill(0, fb->y_charsize * 3U, fb->width, fb->y_charsize * 2U, panel_bg);
    (void)u_fbfill(0, fb->height - (fb->y_charsize * 2U), fb->width, fb->y_charsize * 2U, status_bg);

    snprintf(title, sizeof(title), " STUPIDOS BROWSER  %s://%s:%u%s ",
             url->https ? "https" : "http",
             url->host,
             (unsigned)url->port,
             url->path[0] ? url->path : "/");
    snprintf(status, sizeof(status), " status=%d  type=%s  lines=%u  scroll=%u/%u ",
             status_code,
             content_type && content_type[0] ? content_type : "unknown",
             (unsigned)total,
             (unsigned)scroll,
             (unsigned)((total > view_rows) ? (total - view_rows) : 0U));
    snprintf(hint, sizeof(hint), " q=quit  r=reload  j/k=scroll  arrows/space/PgUp/PgDn supported ");

    (void)u_fbtext(fb->x_charsize * 2U, fb->y_charsize / 2U, title_fg, title_bg, (const int8_t *)title);
    (void)u_fbtext(fb->x_charsize * 2U, fb->y_charsize * 2U, panel_fg, panel_bg, (const int8_t *)status);
    (void)u_fbtext(fb->x_charsize * 2U, fb->y_charsize * 3U + (fb->y_charsize / 2U),
                   panel_fg, panel_bg, (const int8_t *)hint);

    for (row = 0; row < view_rows; row++)
    {
        size_t idx = scroll + row;
        uint32_t y = fb->y_charsize * 5U + (uint32_t)row * fb->y_charsize;

        if (y + fb->y_charsize >= fb->height - (fb->y_charsize * 2U))
        {
            break;
        }

        if (idx < total && lines->items[idx])
        {
            size_t line_len;

            line_len = strnlen(lines->items[idx], sizeof(linebuf) - 1U);
            if (line_len >= cols)
            {
                line_len = cols - 1U;
            }
            memcpy(linebuf, lines->items[idx], line_len);
            linebuf[line_len] = '\0';
            (void)u_fbtext(fb->x_charsize * 2U, y, body_fg, body_bg, (const int8_t *)linebuf);
        }
    }
}

static void browser_render_loading_framebuffer(const struct browser_url *url,
                                               const struct stupidos_fbinfo *fb,
                                               const char *message)
{
    char title[BROWSER_LINE_MAX];
    char body[BROWSER_LINE_MAX];

    if (!fb || fb->width == 0U || fb->height == 0U)
    {
        return;
    }

    (void)u_fbfill(0, 0, fb->width, fb->height, 0x00101820);
    (void)u_fbfill(0, 0, fb->width, fb->y_charsize * 4U, 0x00FFB000);
    (void)u_fbfill(0, fb->y_charsize * 4U, fb->width, fb->height - (fb->y_charsize * 4U), 0x0018222D);

    snprintf(title, sizeof(title), " STUPIDOS BROWSER  %s://%s:%u%s ",
             url->https ? "https" : "http",
             url->host,
             (unsigned)url->port,
             url->path[0] ? url->path : "/");
    snprintf(body, sizeof(body),
             " loading page...  %s ",
             message ? message : "please wait");

    (void)u_fbtext(fb->x_charsize * 2U, fb->y_charsize / 2U, 0x00000000, 0x00FFB000, (const int8_t *)title);
    (void)u_fbtext(fb->x_charsize * 2U, fb->y_charsize * 2U, 0x00FFFFFF, 0x0018222D, (const int8_t *)body);
    (void)u_fbtext(fb->x_charsize * 2U, fb->y_charsize * 4U, 0x00FFFFFF, 0x0018222D,
                   (const int8_t *)" q=quit  r=reload  j/k=scroll  arrows and PgUp/PgDn supported ");
    (void)u_fbtext(fb->x_charsize * 2U, fb->y_charsize * 6U, 0x00A8FFB0, 0x0018222D,
                   (const int8_t *)"waiting for http response...");
}

static int browser_build_text_view(const char *body,
                                   size_t body_len,
                                   const char *content_type,
                                   struct browser_lines *lines,
                                   size_t wrap_width)
{
    struct browser_buffer plain;
    struct browser_buffer text;
    int rc;
    bool html_like;

    if (!body || !lines)
    {
        return -1;
    }

    memset(&plain, 0, sizeof(plain));
    memset(&text, 0, sizeof(text));

    html_like = false;
    if (content_type && content_type[0] != '\0')
    {
        if (browser_ascii_casestr(content_type, "html") || browser_ascii_casestr(content_type, "xml"))
        {
            html_like = true;
        }
    }
    else if (browser_ascii_casestr(body, "<html") || browser_ascii_casestr(body, "<!doctype html"))
    {
        html_like = true;
    }

    if (html_like)
    {
        rc = browser_strip_html(body, body_len, &plain);
        if (rc < 0)
        {
            browser_buffer_free(&plain);
            browser_buffer_free(&text);
            return -1;
        }
        rc = browser_lines_append_text(lines, plain.data ? plain.data : "", plain.len, wrap_width);
        browser_buffer_free(&plain);
        browser_buffer_free(&text);
        return rc;
    }

    rc = browser_lines_append_text(lines, body, body_len, wrap_width);
    browser_buffer_free(&plain);
    browser_buffer_free(&text);
    return rc;
}

static void browser_usage(void)
{
    puts("usage: browser http://host[:port]/path\n");
    puts("note : https is not supported yet, try plain http or a local web server.\n");
}

int main(int argc, char **argv)
{
    struct browser_url start_url;
    struct browser_url final_url;
    struct browser_buffer body;
    struct browser_lines lines;
    struct stupidos_fbinfo fb;
    char content_type[128];
    int status_code;
    size_t scroll;
    size_t max_scroll;
    size_t view_rows;
    size_t wrap_width;
    int rc;
    bool have_fb;
    bool interactive;

    if (argc < 2 || !argv[1] || argv[1][0] == '\0')
    {
        browser_usage();
        return 1;
    }

    browser_enable_raw_terminal();
    atexit(browser_restore_terminal);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (browser_parse_url(argv[1], &start_url) < 0)
    {
        fprintf(stderr, "browser: invalid url: %s\n", argv[1]);
        return 1;
    }

    if (start_url.https)
    {
        fprintf(stderr, "browser: https/TLS is not supported yet\n");
        return 1;
    }

    memset(&body, 0, sizeof(body));
    memset(&lines, 0, sizeof(lines));
    memset(&fb, 0, sizeof(fb));
    content_type[0] = '\0';
    status_code = 0;
    have_fb = (u_fbinfo(&fb) == 0 && fb.width > 0U && fb.height > 0U);
    view_rows = have_fb ? browser_fb_view_rows(&fb) : BROWSER_VIEW_ROWS;
    wrap_width = have_fb ? browser_fb_wrap_cols(&fb) : BROWSER_WRAP_WIDTH;

    if (!have_fb)
    {
        printf("\x1b[2J\x1b[H");
        puts("browser: loading...");
    }
    else
    {
        browser_render_loading_framebuffer(&start_url, &fb, "drawing initial frame");
    }

    rc = browser_fetch_url(&start_url, &final_url, &body, content_type, sizeof(content_type), &status_code);
    if (rc < 0)
    {
        if (have_fb)
        {
            if (rc == -2)
            {
                browser_render_loading_framebuffer(&start_url, &fb, "redirected to https, tls not supported");
            }
            else if (rc == -ETIMEDOUT)
            {
                browser_render_loading_framebuffer(&start_url, &fb, "http recv timeout");
            }
            else
            {
                browser_render_loading_framebuffer(&start_url, &fb, "fetch failed");
            }
        }
        if (rc == -2)
        {
            fprintf(stderr, "browser: redirected to https, TLS is not supported yet\n");
        }
        else if (rc == -ETIMEDOUT)
        {
            fprintf(stderr, "browser: http receive timeout\n");
        }
        else
        {
            fprintf(stderr, "browser: fetch failed (%d)\n", rc);
        }
        browser_buffer_free(&body);
        return 1;
    }

    rc = browser_build_text_view(body.data ? body.data : "", body.len, content_type, &lines, wrap_width);
    browser_buffer_free(&body);
    if (rc < 0)
    {
        if (have_fb)
        {
            browser_render_loading_framebuffer(&final_url, &fb, "failed to build text view");
        }
        browser_lines_free(&lines);
        fprintf(stderr, "browser: failed to build text view\n");
        return 1;
    }

    interactive = isatty(STDIN_FILENO) ? true : false;
    scroll = 0;
    max_scroll = (lines.count > view_rows) ? (lines.count - view_rows) : 0U;
    if (have_fb)
    {
        browser_render_framebuffer(&final_url, status_code, content_type, &lines, scroll, &fb);
    }
    else
    {
        browser_render_terminal(&final_url, status_code, content_type, &lines, scroll);
    }

    if (!interactive)
    {
        size_t i;

        for (i = 0; i < lines.count; i++)
        {
            puts(lines.items[i]);
        }
        browser_lines_free(&lines);
        return 0;
    }

    while (1)
    {
        char ch;
        ssize_t n;

        n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0)
        {
            continue;
        }

        if (ch == 'q' || ch == 'Q' || ch == 0x03)
        {
            break;
        }
        else if (ch == 'j' || ch == ' ' || ch == '\x06')
        {
            if (scroll < max_scroll)
            {
                scroll++;
            }
        }
        else if (ch == 'k' || ch == '\x02')
        {
            if (scroll > 0U)
            {
                scroll--;
            }
        }
        else if (ch == 'g' || ch == '\x01')
        {
            scroll = 0;
        }
        else if (ch == 'G' || ch == '\x05')
        {
            scroll = max_scroll;
        }
        else if (ch == 'r' || ch == 'R')
        {
            browser_lines_free(&lines);
            memset(&lines, 0, sizeof(lines));
            memset(&body, 0, sizeof(body));
            if (!have_fb)
            {
                puts("\x1b[2J\x1b[Hbrowser: reloading...");
            }
            rc = browser_fetch_url(&start_url, &final_url, &body, content_type, sizeof(content_type), &status_code);
            if (rc < 0)
            {
                fprintf(stderr, "browser: reload failed (%d)\n", rc);
                break;
            }
            rc = browser_build_text_view(body.data ? body.data : "", body.len, content_type, &lines, wrap_width);
            browser_buffer_free(&body);
            if (rc < 0)
            {
                fprintf(stderr, "browser: reload text view failed\n");
                break;
            }
            max_scroll = (lines.count > view_rows) ? (lines.count - view_rows) : 0U;
            scroll = 0;
        }
        else if (ch == '\x1b')
        {
            char seq[2];
            ssize_t got;

            got = read(STDIN_FILENO, seq, sizeof(seq));
            if (got == 2 && seq[0] == '[')
            {
                switch (seq[1])
                {
                case 'A':
                    if (scroll > 0U)
                    {
                        scroll--;
                    }
                    break;
                case 'B':
                    if (scroll < max_scroll)
                    {
                        scroll++;
                    }
                    break;
                case '5':
                {
                    char dummy;
                    (void)read(STDIN_FILENO, &dummy, 1);
                    if (scroll > 0U)
                    {
                        scroll = (scroll > 5U) ? (scroll - 5U) : 0U;
                    }
                    break;
                }
                case '6':
                {
                    char dummy;
                    (void)read(STDIN_FILENO, &dummy, 1);
                    if (scroll < max_scroll)
                    {
                        scroll += 5U;
                        if (scroll > max_scroll)
                        {
                            scroll = max_scroll;
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }

        if (have_fb)
        {
            browser_render_framebuffer(&final_url, status_code, content_type, &lines, scroll, &fb);
        }
        else
        {
            browser_render_terminal(&final_url, status_code, content_type, &lines, scroll);
        }
    }

    browser_lines_free(&lines);
    return 0;
}
