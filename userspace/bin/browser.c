#include "stupidos_user.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
#define BROWSER_RECV_TIMEOUT_MS  800
#define BROWSER_CONNECT_TIMEOUT_MS 2500
#define BROWSER_PAGE_TITLE_MAX   128
#define BROWSER_LINK_MAX         48

struct browser_line_item
{
    char *text;
    char href[BROWSER_URL_MAX];
    bool has_link;
};

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
    struct browser_line_item *items;
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
        free(lines->items[i].text);
    }
    free(lines->items);
    lines->items = NULL;
    lines->count = 0;
    lines->cap = 0;
}

static int browser_lines_push_copy(struct browser_lines *lines, const char *line, size_t len, const char *href)
{
    char *copy;
    struct browser_line_item *items;
    size_t cap;
    struct browser_line_item *item;

    if (!lines || !line)
    {
        return -1;
    }

    if (lines->count + 1U > lines->cap)
    {
        cap = lines->cap ? lines->cap * 2U : 128U;
        items = (struct browser_line_item *)realloc(lines->items, cap * sizeof(lines->items[0]));
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
    item = &lines->items[lines->count++];
    item->text = copy;
    item->has_link = false;
    item->href[0] = '\0';
    if (href && href[0] != '\0')
    {
        strncpy(item->href, href, sizeof(item->href) - 1U);
        item->href[sizeof(item->href) - 1U] = '\0';
        item->has_link = true;
    }
    return 0;
}

static int browser_lines_append_text(struct browser_lines *lines, const char *text, size_t len, size_t width, const char *href)
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
                if (browser_lines_push_copy(lines, line, pos, href) < 0)
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
                if (browser_lines_push_copy(lines, line, pos, href) < 0)
                {
                    return -1;
                }
                pos = 0;
            }
        }
        pending_space = false;

        if (pos + 1U >= sizeof(line))
        {
            if (browser_lines_push_copy(lines, line, pos, href) < 0)
            {
                return -1;
            }
            pos = 0;
        }

        line[pos++] = ch;
        if (pos >= width)
        {
            if (browser_lines_push_copy(lines, line, pos, href) < 0)
            {
                return -1;
            }
            pos = 0;
        }
    }

    if (pos > 0U)
    {
        if (browser_lines_push_copy(lines, line, pos, href) < 0)
        {
            return -1;
        }
    }

    if (lines->count == 0U)
    {
        (void)browser_lines_push_copy(lines, "(empty page)", strlen("(empty page)"), 0);
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

static void browser_parse_tag_href(const char *tag, char *href_out, size_t href_out_len)
{
    const char *p;

    if (!tag || !href_out || href_out_len == 0U)
    {
        return;
    }

    href_out[0] = '\0';
    p = tag;
    while (*p && isspace((unsigned char)*p))
    {
        p++;
    }
    if (*p == '/')
    {
        return;
    }

    while (*p)
    {
        if ((p[0] == 'h' || p[0] == 'H') &&
            (p[1] == 'r' || p[1] == 'R') &&
            (p[2] == 'e' || p[2] == 'E') &&
            (p[3] == 'f' || p[3] == 'F'))
        {
            const char *q = p + 4;

            while (*q && isspace((unsigned char)*q))
            {
                q++;
            }
            if (*q != '=')
            {
                p++;
                continue;
            }
            q++;
            while (*q && isspace((unsigned char)*q))
            {
                q++;
            }

            if (*q == '"' || *q == '\'')
            {
                char quote = *q++;
                size_t i = 0;

                while (*q && *q != quote && i + 1U < href_out_len)
                {
                    href_out[i++] = *q++;
                }
                href_out[i] = '\0';
                return;
            }

            {
                size_t i = 0;
                while (*q && !isspace((unsigned char)*q) && *q != '>' && i + 1U < href_out_len)
                {
                    href_out[i++] = *q++;
                }
                href_out[i] = '\0';
                return;
            }
        }
        p++;
    }
}

static void browser_handle_tag_with_link(const char *tag,
                                         struct browser_buffer *out,
                                         bool *in_script,
                                         bool *in_style,
                                         char *current_link,
                                         size_t current_link_len)
{
    char buf[64];
    size_t i;
    size_t j;
    bool closing;
    char href[BROWSER_URL_MAX];

    if (!tag || !in_script || !in_style || !current_link || current_link_len == 0U)
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

    if (strcmp(buf, "a") == 0)
    {
        if (closing)
        {
            current_link[0] = '\0';
        }
        else
        {
            browser_parse_tag_href(tag, href, sizeof(href));
            if (href[0] != '\0')
            {
                strncpy(current_link, href, current_link_len - 1U);
                current_link[current_link_len - 1U] = '\0';
            }
            else
            {
                current_link[0] = '\0';
            }
        }
        return;
    }

    if (strcmp(buf, "script") == 0)
    {
        if (closing)
        {
            *in_script = false;
            if (out)
            {
                browser_emit_block_break(out, 1U);
            }
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
        if (out)
        {
            browser_emit_block_break(out, 1U);
        }
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
        if (out)
        {
            browser_emit_block_break(out, 2U);
        }
        return;
    }

    if (strcmp(buf, "title") == 0)
    {
        if (out)
        {
            browser_emit_block_break(out, 1U);
        }
        return;
    }
}

static unsigned browser_html_handle_tag(const char *tag,
                                        bool *in_script,
                                        bool *in_style,
                                        char *current_link,
                                        size_t current_link_len)
{
    char buf[64];
    size_t i;
    size_t j;
    bool closing;
    unsigned breaks;
    char href[BROWSER_URL_MAX];

    if (!tag || !in_script || !in_style || !current_link || current_link_len == 0U)
    {
        return 0U;
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
    breaks = 0;

    if (strcmp(buf, "a") == 0)
    {
        if (closing)
        {
            current_link[0] = '\0';
        }
        else
        {
            browser_parse_tag_href(tag, href, sizeof(href));
            if (href[0] != '\0')
            {
                strncpy(current_link, href, current_link_len - 1U);
                current_link[current_link_len - 1U] = '\0';
            }
            else
            {
                current_link[0] = '\0';
            }
        }
        return 0U;
    }

    if (strcmp(buf, "script") == 0)
    {
        if (closing)
        {
            *in_script = false;
            breaks = 1U;
        }
        else
        {
            *in_script = true;
        }
        return breaks;
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
        return 0U;
    }

    if (strcmp(buf, "br") == 0 || strcmp(buf, "hr") == 0)
    {
        return 1U;
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
        return 2U;
    }

    if (strcmp(buf, "title") == 0)
    {
        return 1U;
    }

    return 0U;
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

static int browser_html_to_lines(const char *html, size_t html_len, struct browser_lines *lines, size_t width)
{
    char tag[128];
    char entity[32];
    char decoded[8];
    char line[BROWSER_LINE_MAX];
    char current_link[BROWSER_URL_MAX];
    size_t tag_len;
    size_t entity_len;
    size_t i;
    size_t pos;
    bool in_tag;
    bool in_entity;
    bool in_script;
    bool in_style;
    bool pending_space;

    if (!html || !lines || width == 0U)
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
    pos = 0;
    current_link[0] = '\0';

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
                {
                    unsigned breaks;

                    breaks = browser_html_handle_tag(tag, &in_script, &in_style, current_link, sizeof(current_link));
                    if (breaks > 0U)
                    {
                        if (pos > 0U)
                        {
                            if (browser_lines_push_copy(lines, line, pos, current_link[0] ? current_link : NULL) < 0)
                            {
                                return -1;
                            }
                            pos = 0;
                        }
                        pending_space = false;
                        while (breaks > 0U)
                        {
                            (void)browser_lines_push_copy(lines, "", 0U, NULL);
                            breaks--;
                        }
                    }
                }
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
                    size_t dlen = strlen(decoded);
                    size_t di;

                    for (di = 0; di < dlen; di++)
                    {
                        if (decoded[di] == '\r')
                        {
                            continue;
                        }
                        if (decoded[di] == '\n')
                        {
                            if (pos > 0U)
                            {
                                if (browser_lines_push_copy(lines, line, pos, current_link[0] ? current_link : NULL) < 0)
                                {
                                    return -1;
                                }
                                pos = 0;
                            }
                            pending_space = false;
                            continue;
                        }

                        if (isspace((unsigned char)decoded[di]))
                        {
                            pending_space = true;
                            continue;
                        }

                        if (pending_space && pos > 0U && pos + 1U < sizeof(line))
                        {
                            line[pos++] = ' ';
                            if (pos >= width)
                            {
                                if (browser_lines_push_copy(lines, line, pos, current_link[0] ? current_link : NULL) < 0)
                                {
                                    return -1;
                                }
                                pos = 0;
                            }
                        }
                        pending_space = false;

                        if (pos + 1U >= sizeof(line))
                        {
                            if (browser_lines_push_copy(lines, line, pos, current_link[0] ? current_link : NULL) < 0)
                            {
                                return -1;
                            }
                            pos = 0;
                        }
                        line[pos++] = decoded[di];
                        if (pos >= width)
                        {
                            if (browser_lines_push_copy(lines, line, pos, current_link[0] ? current_link : NULL) < 0)
                            {
                                return -1;
                            }
                            pos = 0;
                        }
                    }
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

        if (ch == '\r')
        {
            continue;
        }
        if (ch == '\n')
        {
            if (pos > 0U)
            {
                if (browser_lines_push_copy(lines, line, pos, current_link[0] ? current_link : NULL) < 0)
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
                if (browser_lines_push_copy(lines, line, pos, current_link[0] ? current_link : NULL) < 0)
                {
                    return -1;
                }
                pos = 0;
            }
        }
        pending_space = false;

        if (pos + 1U >= sizeof(line))
        {
            if (browser_lines_push_copy(lines, line, pos, current_link[0] ? current_link : NULL) < 0)
            {
                return -1;
            }
            pos = 0;
        }

        line[pos++] = ch;
        if (pos >= width)
        {
            if (browser_lines_push_copy(lines, line, pos, current_link[0] ? current_link : NULL) < 0)
            {
                return -1;
            }
            pos = 0;
        }
    }

    if (pos > 0U)
    {
        if (browser_lines_push_copy(lines, line, pos, current_link[0] ? current_link : NULL) < 0)
        {
            return -1;
        }
    }

    if (lines->count == 0U)
    {
        (void)browser_lines_push_copy(lines, "(empty page)", strlen("(empty page)"), 0);
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

static int browser_socket_wait_connected(int fd, int timeout_ms)
{
    struct pollfd pfd;
    int so_error;
    socklen_t so_len;
    int ret;

    pfd.fd = fd;
    pfd.events = POLLOUT | POLLERR | POLLHUP;
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

    so_error = 0;
    so_len = sizeof(so_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) < 0)
    {
        return -1;
    }

    if (so_error == 0)
    {
        return 2;
    }

    errno = so_error;
    return -1;
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
    bool any_connect_ok;
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
        return -11;
    }

    ret = -13;
    any_connect_ok = false;
    for (ai = res; ai; ai = ai->ai_next)
    {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
        {
            continue;
        }

        ret = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (ret < 0 && errno == EINPROGRESS)
        {
            ready = browser_socket_wait_connected(fd, BROWSER_CONNECT_TIMEOUT_MS);
            if (ready <= 0)
            {
                close(fd);
                ret = -13;
                continue;
            }
            ret = 0;
        }

        if (ret < 0)
        {
            close(fd);
            ret = -13;
            continue;
        }
        any_connect_ok = true;

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
            ret = -12;
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
    if (!any_connect_ok && ret == -13)
    {
        return -13;
    }
    return ret;
}

static int browser_fetch_once_via_syscall(const struct browser_url *url,
                                          uint32_t ipv4,
                                          struct browser_buffer *response)
{
    char temp_path[128];
    int fd;
    int64_t ret;
    ssize_t n;
    char buf[BROWSER_RECV_CHUNK];

    if (!url || !response)
    {
        return -1;
    }

    /*
     * 这里用临时文件承接 `u_http_get` 的输出：
     * - 避免 pipe 缓冲满了以后写端阻塞
     * - 不要求浏览器一次性持有整页 body
     * - 成功后再把文件内容读回到内存缓冲
     */
    snprintf(temp_path, sizeof(temp_path), "/tmp/.browser-http-%d.tmp", getpid());
    fd = open(temp_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
    {
        return -1;
    }

    ret = u_http_get(ipv4, url->port, (const int8_t *)(url->path[0] ? url->path : "/"), fd, 6000U);
    if (ret < 0)
    {
        close(fd);
        (void)unlink(temp_path);
        return (int)ret;
    }

    if (lseek(fd, 0, SEEK_SET) < 0)
    {
        close(fd);
        (void)unlink(temp_path);
        return -1;
    }

    while (1)
    {
        n = read(fd, buf, sizeof(buf));
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            close(fd);
            (void)unlink(temp_path);
            return -1;
        }
        if (n == 0)
        {
            break;
        }

        if (browser_buffer_append(response, buf, (size_t)n) < 0)
        {
            close(fd);
            (void)unlink(temp_path);
            return -1;
        }
    }

    close(fd);
    (void)unlink(temp_path);
    return 0;
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
        uint32_t fallback_ipv4;

        browser_buffer_free(&raw);
        browser_buffer_free(&decoded);

        rc = browser_fetch_once(&cur, &raw);
        if (rc < 0)
        {
            if (rc == -13)
            {
                struct addrinfo hints;
                struct addrinfo *res;
                struct addrinfo *ai;
                char portbuf[16];

                /*
                 * socket/connect 路径失败时，回退到内核 httpget syscall。
                 * 这条路径不依赖 userspace TCP socket 的细节，能显著提高
                 * 实际可用性。
                 */
                memset(&hints, 0, sizeof(hints));
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                hints.ai_protocol = IPPROTO_TCP;
                snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)cur.port);
                if (getaddrinfo(cur.host, portbuf, &hints, &res) == 0)
                {
                    fallback_ipv4 = 0;
                    for (ai = res; ai; ai = ai->ai_next)
                    {
                        const struct sockaddr_in *sin;

                        if (!ai->ai_addr || ai->ai_addrlen < sizeof(struct sockaddr_in))
                        {
                            continue;
                        }
                        sin = (const struct sockaddr_in *)ai->ai_addr;
                        if (sin->sin_family != AF_INET)
                        {
                            continue;
                        }
                        fallback_ipv4 = ntohl(sin->sin_addr.s_addr);
                        if (fallback_ipv4 != 0U)
                        {
                            break;
                        }
                    }
                    freeaddrinfo(res);
                    if (fallback_ipv4 != 0U)
                    {
                        browser_buffer_free(&raw);
                        browser_buffer_free(&decoded);
                        if (browser_fetch_once_via_syscall(&cur, fallback_ipv4, &decoded) == 0)
                        {
                            *status_code = 200;
                            content_type[0] = '\0';
                            *final_url = cur;
                            *body = decoded;
                            return 0;
                        }
                    }
                }
            }
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
    printf("\x1b[1;30;46m%-78.78s\x1b[0m\r\n", " arrows / space / PgUp / PgDn also work, blue lines are clickable links ");

    for (row = 0; row < visible_rows; row++)
    {
        i = scroll + row;
        if (i < total && lines->items[i].text)
        {
            if (lines->items[i].has_link)
            {
                printf("\x1b[1;34m%-78.78s\x1b[0m\r\n", lines->items[i].text);
            }
            else
            {
                printf("%-78.78s\r\n", lines->items[i].text);
            }
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
    snprintf(hint, sizeof(hint), " q=quit  r=reload  j/k=scroll  click blue links to navigate ");

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

        if (idx < total && lines->items[idx].text)
        {
            size_t line_len;

            line_len = strnlen(lines->items[idx].text, sizeof(linebuf) - 1U);
            if (line_len >= cols)
            {
                line_len = cols - 1U;
            }
            memcpy(linebuf, lines->items[idx].text, line_len);
            linebuf[line_len] = '\0';
            if (lines->items[idx].has_link)
            {
                (void)u_fbtext(fb->x_charsize * 2U, y, 0x0000A8FF, body_bg, (const int8_t *)linebuf);
            }
            else
            {
                (void)u_fbtext(fb->x_charsize * 2U, y, body_fg, body_bg, (const int8_t *)linebuf);
            }
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
                   (const int8_t *)" q=quit  r=reload  j/k=scroll  blue lines can be clicked ");
    (void)u_fbtext(fb->x_charsize * 2U, fb->y_charsize * 6U, 0x00A8FFB0, 0x0018222D,
                   (const int8_t *)"waiting for http response...");
}

static bool browser_line_href_at(const struct browser_lines *lines, size_t index, char *href_out, size_t href_out_len)
{
    if (!lines || !href_out || href_out_len == 0U)
    {
        return false;
    }

    href_out[0] = '\0';
    if (index >= lines->count)
    {
        return false;
    }

    if (!lines->items[index].has_link || !lines->items[index].text)
    {
        return false;
    }

    strncpy(href_out, lines->items[index].href, href_out_len - 1U);
    href_out[href_out_len - 1U] = '\0';
    return href_out[0] != '\0';
}

static int browser_open_clicked_href(const struct browser_url *base,
                                     const struct browser_lines *lines,
                                     const struct stupidos_fbinfo *fb,
                                     const struct stupidos_mouseinfo *mouse,
                                     size_t scroll,
                                     struct browser_url *out)
{
    size_t body_x;
    size_t body_y;
    size_t row_h;
    size_t row;
    size_t idx;
    char href[BROWSER_URL_MAX];

    if (!base || !lines || !fb || !mouse || !out)
    {
        return 0;
    }
    if (fb->x_charsize == 0U || fb->y_charsize == 0U)
    {
        return 0;
    }

    body_x = fb->x_charsize * 2U;
    body_y = fb->y_charsize * 5U;
    row_h = fb->y_charsize;

    if (mouse->x < 0 || mouse->y < 0)
    {
        return 0;
    }
    if ((size_t)mouse->x < body_x || (size_t)mouse->y < body_y)
    {
        return 0;
    }

    row = ((size_t)mouse->y - body_y) / row_h;
    idx = scroll + row;
    if (!browser_line_href_at(lines, idx, href, sizeof(href)))
    {
        return 0;
    }

    if (browser_resolve_url(base, href, out) < 0)
    {
        return -1;
    }
    return 1;
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
        browser_buffer_free(&plain);
        browser_buffer_free(&text);
        return browser_html_to_lines(body, body_len, lines, wrap_width);
    }

    rc = browser_lines_append_text(lines, body, body_len, wrap_width, NULL);
    browser_buffer_free(&plain);
    browser_buffer_free(&text);
    return rc;
}

static int browser_load_page(const struct browser_url *start_url,
                             struct browser_url *final_url,
                             struct browser_buffer *body,
                             struct browser_lines *lines,
                             char *content_type,
                             size_t content_type_len,
                             int *status_code,
                             size_t wrap_width)
{
    int rc;

    if (!start_url || !final_url || !body || !lines || !content_type || !status_code)
    {
        return -1;
    }

    browser_buffer_free(body);
    browser_lines_free(lines);
    memset(body, 0, sizeof(*body));
    memset(lines, 0, sizeof(*lines));

    rc = browser_fetch_url(start_url, final_url, body, content_type, content_type_len, status_code);
    if (rc < 0)
    {
        return rc;
    }

    rc = browser_build_text_view(body->data ? body->data : "", body->len, content_type, lines, wrap_width);
    browser_buffer_free(body);
    if (rc < 0)
    {
        return -1;
    }

    return 0;
}

static void browser_usage(void)
{
    puts("usage: browser http://host[:port]/path\n");
    puts("note : https is not supported yet, try plain http or a local web server.\n");
}

int main(int argc, char **argv)
{
    struct stupidos_mouseinfo mouse;
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
    bool mouse_valid;
    bool dirty;
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
    mouse_valid = false;
    memset(&mouse, 0, sizeof(mouse));

    if (!have_fb)
    {
        printf("\x1b[2J\x1b[H");
        puts("browser: loading...");
    }
    else
    {
        browser_render_loading_framebuffer(&start_url, &fb, "drawing initial frame");
    }

    rc = browser_load_page(&start_url, &final_url, &body, &lines, content_type, sizeof(content_type), &status_code, wrap_width);
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
            else if (rc == -11)
            {
                browser_render_loading_framebuffer(&start_url, &fb, "dns lookup failed");
            }
            else if (rc == -13)
            {
                browser_render_loading_framebuffer(&start_url, &fb, "tcp connect failed");
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
        else if (rc == -11)
        {
            fprintf(stderr, "browser: dns lookup failed\n");
        }
        else if (rc == -13)
        {
            fprintf(stderr, "browser: tcp connect failed\n");
        }
        else
        {
            fprintf(stderr, "browser: fetch failed (%d)\n", rc);
        }
        return 1;
    }

    interactive = isatty(STDIN_FILENO) ? true : false;
    scroll = 0;
    max_scroll = (lines.count > view_rows) ? (lines.count - view_rows) : 0U;
    dirty = false;
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
            puts(lines.items[i].text ? lines.items[i].text : "");
        }
        browser_lines_free(&lines);
        return 0;
    }

    while (1)
    {
        struct pollfd pfd;
        int pret;
        char ch;
        ssize_t n;
        struct stupidos_mouseinfo cur_mouse;
        bool mouse_changed;

        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pret = poll(&pfd, 1, have_fb ? 16 : -1);
        if (pret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        if (pret > 0 && (pfd.revents & POLLIN))
        {
            n = read(STDIN_FILENO, &ch, 1);
            if (n > 0)
            {
                if (ch == 'q' || ch == 'Q' || ch == 0x03)
                {
                    break;
                }
                else if (ch == 'j' || ch == ' ' || ch == '\x06')
                {
                    if (scroll < max_scroll)
                    {
                        scroll++;
                        dirty = true;
                    }
                }
                else if (ch == 'k' || ch == '\x02')
                {
                    if (scroll > 0U)
                    {
                        scroll--;
                        dirty = true;
                    }
                }
                else if (ch == 'g' || ch == '\x01')
                {
                    scroll = 0;
                    dirty = true;
                }
                else if (ch == 'G' || ch == '\x05')
                {
                    scroll = max_scroll;
                    dirty = true;
                }
                else if (ch == 'r' || ch == 'R')
                {
                    if (!have_fb)
                    {
                        puts("\x1b[2J\x1b[Hbrowser: reloading...");
                    }
                    rc = browser_load_page(&start_url, &final_url, &body, &lines, content_type, sizeof(content_type), &status_code, wrap_width);
                    if (rc < 0)
                    {
                        fprintf(stderr, "browser: reload failed (%d)\n", rc);
                        break;
                    }
                    max_scroll = (lines.count > view_rows) ? (lines.count - view_rows) : 0U;
                    scroll = 0;
                    dirty = true;
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
                                dirty = true;
                            }
                            break;
                        case 'B':
                            if (scroll < max_scroll)
                            {
                                scroll++;
                                dirty = true;
                            }
                            break;
                        case '5':
                        {
                            char dummy;
                            (void)read(STDIN_FILENO, &dummy, 1);
                            if (scroll > 0U)
                            {
                                scroll = (scroll > 5U) ? (scroll - 5U) : 0U;
                                dirty = true;
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
                                dirty = true;
                            }
                            break;
                        }
                        default:
                            break;
                        }
                    }
                }
            }
        }

        if (have_fb)
        {
            u_memset(&cur_mouse, 0, sizeof(cur_mouse));
            if (u_mouseinfo(&cur_mouse) == 0)
            {
                bool left_now = (cur_mouse.buttons & 0x1U) != 0U;
                bool left_prev = (mouse_valid && (mouse.buttons & 0x1U) != 0U);
                mouse_changed = !mouse_valid ||
                                cur_mouse.x != mouse.x ||
                                cur_mouse.y != mouse.y ||
                                cur_mouse.buttons != mouse.buttons;

                mouse = cur_mouse;
                mouse_valid = true;
                if (mouse_changed)
                {
                    dirty = true;
                }

                if (left_now && !left_prev)
                {
                    struct browser_url clicked;
                    int link_rc;

                    link_rc = browser_open_clicked_href(&final_url, &lines, &fb, &mouse, scroll, &clicked);
                    if (link_rc < 0)
                    {
                        fprintf(stderr, "browser: link resolve failed\n");
                        break;
                    }
                    if (link_rc > 0)
                    {
                        start_url = clicked;
                        rc = browser_load_page(&start_url, &final_url, &body, &lines, content_type, sizeof(content_type), &status_code, wrap_width);
                        if (rc < 0)
                        {
                            fprintf(stderr, "browser: link navigation failed (%d)\n", rc);
                            break;
                        }
                        max_scroll = (lines.count > view_rows) ? (lines.count - view_rows) : 0U;
                        scroll = 0;
                        dirty = true;
                    }
                }
            }
            else
            {
                mouse_valid = false;
            }
        }

        if (!dirty)
        {
            continue;
        }

        if (have_fb)
        {
            browser_render_framebuffer(&final_url, status_code, content_type, &lines, scroll, &fb);
        }
        else
        {
            browser_render_terminal(&final_url, status_code, content_type, &lines, scroll);
        }
        dirty = false;
    }

    browser_lines_free(&lines);
    return 0;
}
