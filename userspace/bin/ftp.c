#include "stupidos_user.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * stupidos 用户态 FTP 客户端。
 *
 * 设计目标：
 * - 直接复用系统已经有的 socket / DNS / stdio 兼容层；
 * - 支持最常见的 FTP 下载和上传；
 * - 既能作为 `ftp` 使用，也能以 `ftpget` / `ftpput` 的名字运行。
 *
 * 说明：
 * - 当前实现走被动模式（PASV），这样更适合 QEMU/NAT/主机互通场景；
 * - 控制连接和数据连接都通过现有用户态 syscall 封装完成；
 * - 这不是一个完整的交互式 ftp shell，而是一个可直接用的文件传输工具。
 */

enum ftp_mode
{
    FTP_MODE_GET = 0,
    FTP_MODE_PUT = 1,
};

struct ftp_target
{
    char host[128];
    char peer_host[64];
    char user[64];
    char pass[64];
    char remote_path[256];
    char local_path[256];
    uint16_t port;
    enum ftp_mode mode;
};

static void ftp_usage(void)
{
    puts("usage:\n");
    puts("  ftp get  [options] HOST REMOTE_FILE [LOCAL_FILE]\n");
    puts("  ftp put  [options] HOST REMOTE_FILE LOCAL_FILE\n");
    puts("  ftp get  [options] ftp://[user[:pass]@]HOST[:PORT]/REMOTE [LOCAL_FILE]\n");
    puts("  ftp put  [options] ftp://[user[:pass]@]HOST[:PORT]/REMOTE LOCAL_FILE\n");
    puts("\noptions:\n");
    puts("  -u USER   username (default: anonymous)\n");
    puts("  -p PASS   password (default: busybox)\n");
    puts("  -P PORT   ftp control port (default: 21)\n");
}

static int ftp_starts_with(const char *s, const char *prefix)
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

static size_t ftp_strlcpy(char *dst, const char *src, size_t size)
{
    size_t i;
    size_t n;

    if (!dst || !size)
    {
        return 0;
    }

    if (!src)
    {
        dst[0] = '\0';
        return 0;
    }

    n = strlen(src);
    i = (n >= size) ? size - 1U : n;
    if (i > 0)
    {
        memcpy(dst, src, i);
    }
    dst[i] = '\0';
    return n;
}

static int ftp_copy_basename(const char *path, char *out, size_t out_len)
{
    const char *base;

    if (!out || out_len == 0)
    {
        return -1;
    }

    if (!path || !*path)
    {
        ftp_strlcpy(out, "download.bin", out_len);
        return 0;
    }

    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!*base)
    {
        ftp_strlcpy(out, "download.bin", out_len);
        return 0;
    }

    if (strlen(base) >= out_len)
    {
        return -1;
    }
    ftp_strlcpy(out, base, out_len);
    return 0;
}

static int ftp_parse_u16(const char *s, uint16_t *out)
{
    unsigned long v;
    char *end;

    if (!s || !*s || !out)
    {
        return -1;
    }

    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || !end || *end != '\0' || v == 0 || v > 65535UL)
    {
        return -1;
    }

    *out = (uint16_t)v;
    return 0;
}

static int ftp_parse_url(const char *url, struct ftp_target *tgt, const char *local_fallback)
{
    const char *p;
    const char *host_start;
    const char *host_end;
    const char *path_start;
    const char *userinfo_end;
    char hostbuf[128];
    char pathbuf[256];
    char userbuf[64];
    char passbuf[64];
    size_t len;

    if (!url || !tgt)
    {
        return -1;
    }

    if (!ftp_starts_with(url, "ftp://"))
    {
        return -1;
    }

    p = url + 6;
    userbuf[0] = '\0';
    passbuf[0] = '\0';

    userinfo_end = strchr(p, '@');
    host_start = p;
    if (userinfo_end)
    {
        const char *sep = memchr(p, ':', (size_t)(userinfo_end - p));
        if (sep)
        {
            len = (size_t)(sep - p);
            if (len >= sizeof(userbuf))
            {
                return -1;
            }
            memcpy(userbuf, p, len);
            userbuf[len] = '\0';

            len = (size_t)(userinfo_end - sep - 1);
            if (len >= sizeof(passbuf))
            {
                return -1;
            }
            memcpy(passbuf, sep + 1, len);
            passbuf[len] = '\0';
        }
        else
        {
            len = (size_t)(userinfo_end - p);
            if (len >= sizeof(userbuf))
            {
                return -1;
            }
            memcpy(userbuf, p, len);
            userbuf[len] = '\0';
        }
        host_start = userinfo_end + 1;
    }

    host_end = host_start;
    while (*host_end && *host_end != ':' && *host_end != '/')
    {
        host_end++;
    }
    if (host_end == host_start)
    {
        return -1;
    }

    len = (size_t)(host_end - host_start);
    if (len >= sizeof(hostbuf))
    {
        return -1;
    }
    memcpy(hostbuf, host_start, len);
    hostbuf[len] = '\0';

    if (*host_end == ':')
    {
        const char *port_start = host_end + 1;
        const char *port_end = port_start;

        while (*port_end && *port_end != '/')
        {
            port_end++;
        }
        len = (size_t)(port_end - port_start);
        if (len == 0 || len >= 16)
        {
            return -1;
        }
        {
            char portbuf[16];
            memcpy(portbuf, port_start, len);
            portbuf[len] = '\0';
            if (ftp_parse_u16(portbuf, &tgt->port) < 0)
            {
                return -1;
            }
        }
        path_start = port_end;
    }
    else
    {
        path_start = host_end;
    }

    if (*path_start == '/')
    {
        ftp_strlcpy(pathbuf, path_start, sizeof(pathbuf));
    }
    else
    {
        ftp_strlcpy(pathbuf, "/", sizeof(pathbuf));
    }

    ftp_strlcpy(tgt->host, hostbuf, sizeof(tgt->host));
    if (userbuf[0])
    {
        ftp_strlcpy(tgt->user, userbuf, sizeof(tgt->user));
    }
    if (passbuf[0])
    {
        ftp_strlcpy(tgt->pass, passbuf, sizeof(tgt->pass));
    }
    ftp_strlcpy(tgt->remote_path, pathbuf, sizeof(tgt->remote_path));
    if (!tgt->local_path[0] && local_fallback)
    {
        ftp_strlcpy(tgt->local_path, local_fallback, sizeof(tgt->local_path));
    }

    return 0;
}

static int ftp_peer_ip_string(int fd, char *out, size_t out_len)
{
    struct sockaddr_storage ss;
    socklen_t len;

    if (!out || out_len == 0)
    {
        return -1;
    }

    len = (socklen_t)sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &len) < 0)
    {
        return -1;
    }

    if (ss.ss_family != AF_INET)
    {
        return -1;
    }

    {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)&ss;
        const uint8_t *ip = (const uint8_t *)&sin->sin_addr.s_addr;
        if (!inet_ntop(AF_INET, ip, out, (socklen_t)out_len))
        {
            return -1;
        }
    }
    return 0;
}

static int ftp_connect_host(const char *host, uint16_t port)
{
    struct addrinfo hints;
    struct addrinfo *ai;
    char portbuf[16];
    int fd;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)port);
    if (getaddrinfo(host, portbuf, &hints, &ai) != 0 || !ai)
    {
        return -1;
    }

    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
    {
        freeaddrinfo(ai);
        return fd;
    }

    if (fd >= 0)
    {
        close(fd);
    }
    freeaddrinfo(ai);
    return -1;
}

static int ftp_read_reply(FILE *ctl, char *buf, size_t buf_len)
{
    int code;

    code = -1;
    for (;;)
    {
        if (!fgets(buf, (int)buf_len, ctl))
        {
            return -1;
        }
        if (isdigit((unsigned char)buf[0]) && isdigit((unsigned char)buf[1]) && isdigit((unsigned char)buf[2]))
        {
            code = (buf[0] - '0') * 100 + (buf[1] - '0') * 10 + (buf[2] - '0');
            if (buf[3] == ' ')
            {
                return code;
            }
            if (buf[3] == '-')
            {
                continue;
            }
        }
    }
}

static int ftp_cmd(FILE *ctl, char *reply, size_t reply_len, const char *cmd, const char *arg)
{
    int code;

    if (cmd)
    {
        if (arg)
        {
            fprintf(ctl, "%s %s\r\n", cmd, arg);
        }
        else
        {
            fprintf(ctl, "%s\r\n", cmd);
        }
        fflush(ctl);
    }

    code = ftp_read_reply(ctl, reply, reply_len);
    return code;
}

static int ftp_parse_pasv_port(const char *reply)
{
    int vals[6];
    int count;
    const char *p;

    if (!reply)
    {
        return -1;
    }

    p = strchr(reply, '(');
    if (!p)
    {
        return -1;
    }
    p++;

    count = 0;
    while (*p && count < 6)
    {
        while (*p && !isdigit((unsigned char)*p))
        {
            if (*p == ')')
            {
                break;
            }
            p++;
        }
        if (!isdigit((unsigned char)*p))
        {
            break;
        }
        vals[count++] = (int)strtoul(p, (char **)&p, 10);
        if (*p == ',')
        {
            p++;
        }
    }

    if (count != 6)
    {
        return -1;
    }

    return vals[4] * 256 + vals[5];
}

static int ftp_open_data(struct ftp_target *tgt, FILE *ctl, char *reply, size_t reply_len)
{
    int code;
    int port;

    code = ftp_cmd(ctl, reply, reply_len, "PASV", 0);
    if (code != 227)
    {
        return -1;
    }

    port = ftp_parse_pasv_port(reply);
    if (port < 0)
    {
        return -1;
    }

    return ftp_connect_host(tgt->peer_host, (uint16_t)port);
}

static int ftp_copy_stream(int in_fd, int out_fd)
{
    char buf[4096];

    for (;;)
    {
        ssize_t rd;
        ssize_t wr;
        size_t off;

        rd = read(in_fd, buf, sizeof(buf));
        if (rd == 0)
        {
            return 0;
        }
        if (rd < 0)
        {
            return -1;
        }

        off = 0;
        while (off < (size_t)rd)
        {
            wr = write(out_fd, buf + off, (size_t)rd - off);
            if (wr < 0)
            {
                return -1;
            }
            off += (size_t)wr;
        }
    }
}

static int ftp_login(FILE *ctl, struct ftp_target *tgt)
{
    char reply[512];
    int code;

    code = ftp_read_reply(ctl, reply, sizeof(reply));
    if (code != 220)
    {
        return -1;
    }

    code = ftp_cmd(ctl, reply, sizeof(reply), "USER", tgt->user[0] ? tgt->user : "anonymous");
    if (code == 331)
    {
        code = ftp_cmd(ctl, reply, sizeof(reply), "PASS", tgt->pass[0] ? tgt->pass : "busybox");
        if (code != 230)
        {
            return -1;
        }
    }
    else if (code != 230)
    {
        return -1;
    }

    code = ftp_cmd(ctl, reply, sizeof(reply), "TYPE", "I");
    return (code == 200) ? 0 : -1;
}

static int ftp_do_get(struct ftp_target *tgt)
{
    int ctl_fd;
    FILE *ctl;
    int data_fd;
    int local_fd;
    char reply[512];
    int code;
    char local_buf[256];

    ctl_fd = ftp_connect_host(tgt->host, tgt->port);
    if (ctl_fd < 0)
    {
        perror("ftp: connect");
        return 1;
    }

    if (ftp_peer_ip_string(ctl_fd, tgt->peer_host, sizeof(tgt->peer_host)) < 0)
    {
        close(ctl_fd);
        return 1;
    }

    ctl = fdopen(ctl_fd, "r+");
    if (!ctl)
    {
        close(ctl_fd);
        return 1;
    }

    if (ftp_login(ctl, tgt) < 0)
    {
        fclose(ctl);
        fprintf(stderr, "ftp: login failed\n");
        return 1;
    }

    if (!tgt->local_path[0])
    {
        if (ftp_copy_basename(tgt->remote_path, local_buf, sizeof(local_buf)) < 0)
        {
            fclose(ctl);
            fprintf(stderr, "ftp: invalid local output path\n");
            return 1;
        }
        ftp_strlcpy(tgt->local_path, local_buf, sizeof(tgt->local_path));
    }

    data_fd = ftp_open_data(tgt, ctl, reply, sizeof(reply));
    if (data_fd < 0)
    {
        fclose(ctl);
        fprintf(stderr, "ftp: PASV failed\n");
        return 1;
    }

    code = ftp_cmd(ctl, reply, sizeof(reply), "RETR", tgt->remote_path);
    if (code != 125 && code != 150)
    {
        close(data_fd);
        fclose(ctl);
        fprintf(stderr, "ftp: RETR failed\n");
        return 1;
    }

    if (strcmp(tgt->local_path, "-") == 0)
    {
        if (ftp_copy_stream(data_fd, STDOUT_FILENO) < 0)
        {
            close(data_fd);
            fclose(ctl);
            fprintf(stderr, "ftp: write stdout failed\n");
            return 1;
        }
    }
    else
    {
        local_fd = open(tgt->local_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (local_fd < 0)
        {
            close(data_fd);
            fclose(ctl);
            perror("ftp: open");
            return 1;
        }
        if (ftp_copy_stream(data_fd, local_fd) < 0)
        {
            close(local_fd);
            close(data_fd);
            fclose(ctl);
            perror("ftp: copy");
            return 1;
        }
        close(local_fd);
    }

    close(data_fd);
    code = ftp_read_reply(ctl, reply, sizeof(reply));
    ftp_cmd(ctl, reply, sizeof(reply), "QUIT", 0);
    fclose(ctl);
    return (code == 226) ? 0 : 1;
}

static int ftp_do_put(struct ftp_target *tgt)
{
    int ctl_fd;
    FILE *ctl;
    int data_fd;
    int local_fd;
    char reply[512];
    int code;

    ctl_fd = ftp_connect_host(tgt->host, tgt->port);
    if (ctl_fd < 0)
    {
        perror("ftp: connect");
        return 1;
    }

    if (ftp_peer_ip_string(ctl_fd, tgt->peer_host, sizeof(tgt->peer_host)) < 0)
    {
        close(ctl_fd);
        return 1;
    }

    ctl = fdopen(ctl_fd, "r+");
    if (!ctl)
    {
        close(ctl_fd);
        return 1;
    }

    if (ftp_login(ctl, tgt) < 0)
    {
        fclose(ctl);
        fprintf(stderr, "ftp: login failed\n");
        return 1;
    }

    if (!tgt->local_path[0])
    {
        fclose(ctl);
        fprintf(stderr, "ftp: missing local file\n");
        return 1;
    }

    local_fd = open(tgt->local_path, O_RDONLY);
    if (local_fd < 0)
    {
        fclose(ctl);
        perror("ftp: open");
        return 1;
    }

    data_fd = ftp_open_data(tgt, ctl, reply, sizeof(reply));
    if (data_fd < 0)
    {
        close(local_fd);
        fclose(ctl);
        fprintf(stderr, "ftp: PASV failed\n");
        return 1;
    }

    code = ftp_cmd(ctl, reply, sizeof(reply), "STOR", tgt->remote_path);
    if (code != 125 && code != 150)
    {
        close(local_fd);
        close(data_fd);
        fclose(ctl);
        fprintf(stderr, "ftp: STOR failed\n");
        return 1;
    }

    if (ftp_copy_stream(local_fd, data_fd) < 0)
    {
        close(local_fd);
        close(data_fd);
        fclose(ctl);
        perror("ftp: copy");
        return 1;
    }

    shutdown(data_fd, SHUT_WR);
    close(local_fd);
    close(data_fd);
    code = ftp_read_reply(ctl, reply, sizeof(reply));
    ftp_cmd(ctl, reply, sizeof(reply), "QUIT", 0);
    fclose(ctl);
    return (code == 226) ? 0 : 1;
}

static int ftp_detect_mode(const char *argv0, int argc, char **argv, int *argi, struct ftp_target *tgt)
{
    if (!argv0 || !tgt || !argi)
    {
        return -1;
    }

    tgt->mode = FTP_MODE_GET;
    if (strstr(argv0, "ftpput"))
    {
        tgt->mode = FTP_MODE_PUT;
    }

    *argi = 1;
    if (strcmp(argv0, "ftp") == 0)
    {
        if (*argi >= argc)
        {
            return -1;
        }
        if (strcmp(argv[*argi], "get") == 0)
        {
            tgt->mode = FTP_MODE_GET;
            (*argi)++;
        }
        else if (strcmp(argv[*argi], "put") == 0)
        {
            tgt->mode = FTP_MODE_PUT;
            (*argi)++;
        }
        else
        {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct ftp_target tgt;
    int argi;
    const char *argv0;

    memset(&tgt, 0, sizeof(tgt));
    ftp_strlcpy(tgt.user, "anonymous", sizeof(tgt.user));
    ftp_strlcpy(tgt.pass, "busybox", sizeof(tgt.pass));
    tgt.port = 21;

    argv0 = argv && argv[0] ? argv[0] : "ftp";
    if (ftp_detect_mode(argv0, argc, argv, &argi, &tgt) < 0)
    {
        ftp_usage();
        return 1;
    }

    while (argi < argc && argv[argi] && argv[argi][0] == '-')
    {
        if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0)
        {
            ftp_usage();
            return 0;
        }
        if (strcmp(argv[argi], "-u") == 0 && argi + 1 < argc)
        {
            ftp_strlcpy(tgt.user, argv[argi + 1], sizeof(tgt.user));
            argi += 2;
            continue;
        }
        if (strcmp(argv[argi], "-p") == 0 && argi + 1 < argc)
        {
            ftp_strlcpy(tgt.pass, argv[argi + 1], sizeof(tgt.pass));
            argi += 2;
            continue;
        }
        if (strcmp(argv[argi], "-P") == 0 && argi + 1 < argc)
        {
            if (ftp_parse_u16(argv[argi + 1], &tgt.port) < 0)
            {
                ftp_usage();
                return 1;
            }
            argi += 2;
            continue;
        }
        ftp_usage();
        return 1;
    }

    if (argi >= argc)
    {
        ftp_usage();
        return 1;
    }

    if (ftp_starts_with(argv[argi], "ftp://"))
    {
        if (ftp_parse_url(argv[argi], &tgt, 0) < 0)
        {
            fprintf(stderr, "ftp: invalid url: %s\n", argv[argi]);
            return 1;
        }
        argi++;
        if (tgt.mode == FTP_MODE_GET)
        {
            if (argi < argc)
            {
                ftp_strlcpy(tgt.local_path, argv[argi], sizeof(tgt.local_path));
            }
            if (!tgt.local_path[0] && ftp_copy_basename(tgt.remote_path, tgt.local_path, sizeof(tgt.local_path)) < 0)
            {
                fprintf(stderr, "ftp: invalid local file\n");
                return 1;
            }
        }
        else
        {
            if (argi < argc)
            {
                ftp_strlcpy(tgt.local_path, argv[argi], sizeof(tgt.local_path));
            }
        }
    }
    else
    {
        ftp_strlcpy(tgt.host, argv[argi], sizeof(tgt.host));
        argi++;
        if (argi >= argc)
        {
            ftp_usage();
            return 1;
        }
        ftp_strlcpy(tgt.remote_path, argv[argi], sizeof(tgt.remote_path));
        argi++;
        if (tgt.mode == FTP_MODE_GET)
        {
            if (argi < argc)
            {
                ftp_strlcpy(tgt.local_path, argv[argi], sizeof(tgt.local_path));
            }
            if (!tgt.local_path[0] && ftp_copy_basename(tgt.remote_path, tgt.local_path, sizeof(tgt.local_path)) < 0)
            {
                fprintf(stderr, "ftp: invalid local file\n");
                return 1;
            }
        }
        else
        {
            if (argi >= argc)
            {
                ftp_usage();
                return 1;
            }
            ftp_strlcpy(tgt.local_path, argv[argi], sizeof(tgt.local_path));
        }
    }

    if (tgt.mode == FTP_MODE_GET)
    {
        return ftp_do_get(&tgt);
    }
    return ftp_do_put(&tgt);
}
