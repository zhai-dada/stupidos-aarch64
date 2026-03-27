#include "stupidos_user.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * /bin/wget
 *
 * 说明：
 * - 这是一个面向 stupidos 的最小 wget 兼容 ELF，接口风格向 BusyBox 对齐；
 * - 当前先支持 HTTP 明文下载，URL 形式为：
 *   http://10.0.2.2[:port]/path
 *   http://localhost[:port]/path
 *   http://example.com[:port]/path
 * - 后端通过内核 httpget syscall 完成连接、收包和写文件。
 * - 同一份实现会同时被 /bin/wget 和 /bin/busybox wget 复用。
 */

static int wget_starts_with(const int8_t *s, const char *prefix)
{
    size_t i;

    if (!s || !prefix)
    {
        return 0;
    }

    for (i = 0; prefix[i] != '\0'; i++)
    {
        if (s[i] != prefix[i])
        {
            return 0;
        }
    }
    return 1;
}

static int wget_parse_u32(const int8_t *str, uint32_t *out)
{
    uint64_t value;
    size_t i;

    if (!str || !out || !str[0])
    {
        return -1;
    }

    value = 0;
    for (i = 0; str[i] != '\0'; i++)
    {
        if (str[i] < '0' || str[i] > '9')
        {
            return -1;
        }
        value = value * 10ULL + (uint64_t)(str[i] - '0');
        if (value > 0xffffffffULL)
        {
            return -1;
        }
    }

    *out = (uint32_t)value;
    return 0;
}

static int wget_parse_ipv4(const int8_t *str, uint32_t *out)
{
    uint32_t octets[4];
    uint32_t cur;
    size_t octet_idx;
    size_t i;

    if (!str || !out)
    {
        return -1;
    }

    octet_idx = 0;
    cur = 0;
    for (i = 0; ; i++)
    {
        char ch = (char)str[i];

        if (ch >= '0' && ch <= '9')
        {
            cur = cur * 10U + (uint32_t)(ch - '0');
            if (cur > 255U)
            {
                return -1;
            }
            continue;
        }

        if (ch == '.' || ch == '\0')
        {
            if (octet_idx >= 4)
            {
                return -1;
            }
            octets[octet_idx++] = cur;
            cur = 0;
            if (ch == '\0')
            {
                break;
            }
            continue;
        }

        return -1;
    }

    if (octet_idx != 4)
    {
        return -1;
    }

    *out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return 0;
}

static int wget_default_outname(const int8_t *path, int8_t *out, size_t out_len)
{
    size_t len;
    size_t i;
    size_t start;

    if (!out || out_len == 0)
    {
        return -1;
    }

    if (!path || path[0] == '\0' || u_strcmp(path, (const int8_t *)"/") == 0)
    {
        if (out_len < 11)
        {
            return -1;
        }
        out[0] = 'i';
        out[1] = 'n';
        out[2] = 'd';
        out[3] = 'e';
        out[4] = 'x';
        out[5] = '.';
        out[6] = 'h';
        out[7] = 't';
        out[8] = 'm';
        out[9] = 'l';
        out[10] = '\0';
        return 0;
    }

    len = u_strnlen(path, 4096);
    start = 0;
    for (i = 0; i < len; i++)
    {
        if (path[i] == '/')
        {
            start = i + 1U;
        }
    }
    if (start >= len)
    {
        return wget_default_outname((const int8_t *)"/", out, out_len);
    }

    if (len - start + 1U > out_len)
    {
        return -1;
    }

    for (i = 0; i < len - start; i++)
    {
        out[i] = path[start + i];
    }
    out[len - start] = '\0';
    return 0;
}

static int wget_parse_host(const int8_t *host, uint32_t *ipv4)
{
    int8_t host_buf[128];
    uint32_t resolved;
    size_t len;

    if (!host || !ipv4)
    {
        return -1;
    }

    len = u_strnlen(host, sizeof(host_buf) - 1U);
    if (len == 0 || len >= sizeof(host_buf))
    {
        return -1;
    }

    u_memcpy(host_buf, host, len);
    host_buf[len] = '\0';

    if (wget_parse_ipv4(host_buf, &resolved) == 0)
    {
        *ipv4 = resolved;
        return 0;
    }

    if (u_strcmp(host_buf, (const int8_t *)"localhost") == 0)
    {
        *ipv4 = (10U << 24) | (0U << 16) | (2U << 8) | 2U;
        return 0;
    }

    if (u_dns_lookup(host_buf, &resolved, 3000U) == 0)
    {
        *ipv4 = resolved;
        return 0;
    }

    return -1;
}

static int wget_parse_url(const int8_t *url, uint32_t *ipv4, uint16_t *port, int8_t *path, size_t path_len)
{
    const int8_t *p;
    const int8_t *host;
    size_t host_len;
    size_t i;

    if (!url || !ipv4 || !port || !path || path_len == 0)
    {
        return -1;
    }

    if (!wget_starts_with(url, "http://"))
    {
        return -1;
    }

    p = url + 7;
    host = p;
    host_len = 0;
    while (p[host_len] && p[host_len] != ':' && p[host_len] != '/')
    {
        host_len++;
    }

    if (host_len == 0)
    {
        return -1;
    }

    {
        int8_t host_buf[128];

        if (host_len >= sizeof(host_buf))
        {
            return -1;
        }

        for (i = 0; i < host_len; i++)
        {
            host_buf[i] = host[i];
        }
        host_buf[host_len] = '\0';
        if (wget_parse_host(host_buf, ipv4))
        {
            return -1;
        }
    }

    *port = 80;
    p += host_len;
    if (*p == ':')
    {
        uint32_t tmp;
        const int8_t *port_start;
        size_t port_len;
        int8_t port_buf[16];

        p++;
        port_start = p;
        port_len = 0;
        while (port_start[port_len] && port_start[port_len] != '/')
        {
            port_len++;
        }
        if (port_len == 0 || port_len >= sizeof(port_buf))
        {
            return -1;
        }
        for (i = 0; i < port_len; i++)
        {
            port_buf[i] = port_start[i];
        }
        port_buf[port_len] = '\0';
        if (wget_parse_u32(port_buf, &tmp) || tmp > 65535U || tmp == 0U)
        {
            return -1;
        }
        *port = (uint16_t)tmp;
        p += port_len;
    }

    if (*p == '\0')
    {
        if (path_len < 2)
        {
            return -1;
        }
        path[0] = '/';
        path[1] = '\0';
        return 0;
    }

    if (*p != '/')
    {
        return -1;
    }

    if (u_strnlen(p, path_len) + 1U > path_len)
    {
        return -1;
    }

    for (i = 0; p[i] != '\0' && i + 1U < path_len; i++)
    {
        path[i] = p[i];
    }
    path[i] = '\0';
    return 0;
}

static void wget_usage(void)
{
    u_puts((const int8_t *)"usage: wget [-O file|-] http://host[:port]/path\n");
    u_puts((const int8_t *)"       host may be an IPv4 address or localhost\n");
}

int busybox_wget_main(int argc, char **argv)
{
    uint32_t ipv4;
    uint16_t port;
    int8_t path[STUPIDOS_PATH_MAX];
    int8_t outname[STUPIDOS_PATH_MAX];
    int out_fd;
    int i;
    int ret;
    bool quiet;
    const int8_t *url;

    quiet = false;
    out_fd = -1;
    url = 0;

    for (i = 1; i < argc; i++)
    {
        if (!argv[i])
        {
            continue;
        }

        if (u_strcmp((const int8_t *)argv[i], (const int8_t *)"-q") == 0)
        {
            quiet = true;
            continue;
        }

        if (u_strcmp((const int8_t *)argv[i], (const int8_t *)"-O") == 0)
        {
            if (i + 1 >= argc || !argv[i + 1])
            {
                wget_usage();
                return 1;
            }
            outname[0] = '\0';
            if (u_strcmp((const int8_t *)argv[i + 1], (const int8_t *)"-") == 0)
            {
                out_fd = STUPIDOS_STDOUT_FILENO;
            }
            else
            {
                out_fd = open((const char *)argv[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (out_fd < 0)
                {
                    u_puts((const int8_t *)"wget: failed to open output file\n");
                    return 1;
                }
            }
            i++;
            continue;
        }

        if (argv[i][0] == '-')
        {
            wget_usage();
            return 1;
        }

        url = (const int8_t *)argv[i];
    }

    if (!url)
    {
        wget_usage();
        return 1;
    }

    ret = wget_parse_url(url, &ipv4, &port, path, sizeof(path));
    if (ret)
    {
        u_puts((const int8_t *)"wget: only HTTP URLs with numeric IPv4, localhost, or DNS hostnames are supported yet\n");
        return 1;
    }

    if (out_fd < 0)
    {
        if (wget_default_outname(path, outname, sizeof(outname)) < 0)
        {
            u_puts((const int8_t *)"wget: cannot derive output filename\n");
            return 1;
        }
        out_fd = open((const char *)outname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0)
        {
            u_puts((const int8_t *)"wget: failed to open output file\n");
            return 1;
        }
    }

    ret = (int)u_http_get(ipv4, port, path, out_fd, 15000U);
    if (ret < 0)
    {
        if (!quiet)
        {
            u_puts((const int8_t *)"wget: download failed\n");
        }
        if (out_fd > STUPIDOS_STDERR_FILENO)
        {
            (void)close(out_fd);
        }
        return 1;
    }

    if (out_fd > STUPIDOS_STDERR_FILENO)
    {
        (void)close(out_fd);
    }

    if (!quiet)
    {
        u_puts((const int8_t *)"wget: saved\n");
    }
    return 0;
}

#ifndef BUSYBOX_WGET_APPLET_ONLY
int main(int argc, char **argv)
{
    return busybox_wget_main(argc, argv);
}
#endif
