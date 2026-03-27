#include "net/net.h"

#include "driver/virtio_net.h"
#include "errno.h"
#include "fs/vfs.h"
#include "lib/libasm.h"
#include "lib/libmem.h"
#include "printk.h"
#include "sched.h"
#include "spinlock.h"
#include "tty.h"

#define NET_TCP_MAX_CONNS      4
#define NET_TCP_RX_BUF_SIZE    32768
#define NET_TCP_PORT_MIN       49152
#define NET_TCP_CONNECT_TIMEOUT_MS  8000U

#define TCP_FLAG_FIN           0x001
#define TCP_FLAG_SYN           0x002
#define TCP_FLAG_RST           0x004
#define TCP_FLAG_PSH           0x008
#define TCP_FLAG_ACK           0x010

struct net_tcp_hdr
{
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint16_t doff_flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

struct net_ipv4_hdr
{
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t proto;
    uint16_t checksum;
    uint8_t src[4];
    uint8_t dst[4];
} __attribute__((packed));

struct net_tcp_pseudo_hdr
{
    uint8_t src[4];
    uint8_t dst[4];
    uint8_t zero;
    uint8_t proto;
    uint16_t len;
} __attribute__((packed));

enum net_tcp_state
{
    NET_TCP_FREE = 0,
    NET_TCP_SYN_SENT,
    NET_TCP_ESTABLISHED,
    NET_TCP_CLOSED,
};

struct net_tcp_conn
{
    bool used;
    enum net_tcp_state state;
    spinlock_t lock;
    struct net_device *dev;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
    uint8_t remote_mac[6];
    uint32_t iss;
    uint32_t snd_nxt;
    uint32_t snd_una;
    uint32_t rcv_nxt;
    uint64_t connect_start_cycles;
    int so_error;
    uint8_t rx_buf[NET_TCP_RX_BUF_SIZE];
    size_t rx_head;
    size_t rx_tail;
    bool eof;
    bool error;
};

static struct net_tcp_conn net_tcp_conns[NET_TCP_MAX_CONNS];
static spinlock_t net_tcp_port_lock = SPINLOCK_INIT;
static uint16_t net_tcp_next_port = NET_TCP_PORT_MIN;

static uint16_t net_tcp_bswap16(uint16_t v)
{
    return __builtin_bswap16(v);
}

static uint32_t net_tcp_bswap32(uint32_t v)
{
    return __builtin_bswap32(v);
}

static uint64_t net_tcp_cycles(void)
{
    uint64_t cycles;

    asm volatile("mrs %0, cntpct_el0" : "=r"(cycles) : : "memory");
    return cycles;
}

static uint64_t net_tcp_freq(void)
{
    uint64_t freq;

    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq) : : "memory");
    return freq ? freq : 1000000000ULL;
}

static uint64_t net_tcp_ms_to_cycles(uint32_t ms)
{
    return (net_tcp_freq() * (uint64_t)ms) / 1000ULL;
}

static uint16_t net_tcp_checksum16(const void *buf, size_t len)
{
    const uint8_t *p;
    uint32_t sum;

    p = (const uint8_t *)buf;
    sum = 0;
    while (len > 1)
    {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }

    if (len)
    {
        sum += ((uint16_t)p[0] << 8);
    }

    while (sum >> 16)
    {
        sum = (sum & 0xffffU) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

static uint32_t net_tcp_checksum_sum(const void *buf, size_t len, uint32_t sum)
{
    const uint8_t *p;

    p = (const uint8_t *)buf;
    while (len > 1)
    {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }

    if (len)
    {
        sum += ((uint16_t)p[0] << 8);
    }

    return sum;
}

static uint16_t net_tcp_checksum(struct net_device *dev, uint32_t dst_ip, const struct net_tcp_hdr *tcp,
                                 const void *payload, size_t payload_len)
{
    struct net_tcp_pseudo_hdr pseudo;
    uint32_t sum;
    uint16_t tcp_len;

    if (!dev || !tcp)
    {
        return 0;
    }

    tcp_len = (uint16_t)(sizeof(*tcp) + payload_len);
    pseudo.src[0] = (uint8_t)((dev->ipv4 >> 24) & 0xff);
    pseudo.src[1] = (uint8_t)((dev->ipv4 >> 16) & 0xff);
    pseudo.src[2] = (uint8_t)((dev->ipv4 >> 8) & 0xff);
    pseudo.src[3] = (uint8_t)(dev->ipv4 & 0xff);
    pseudo.dst[0] = (uint8_t)((dst_ip >> 24) & 0xff);
    pseudo.dst[1] = (uint8_t)((dst_ip >> 16) & 0xff);
    pseudo.dst[2] = (uint8_t)((dst_ip >> 8) & 0xff);
    pseudo.dst[3] = (uint8_t)(dst_ip & 0xff);
    pseudo.zero = 0;
    pseudo.proto = NET_IPV4_PROTO_TCP;
    pseudo.len = net_tcp_bswap16(tcp_len);
    sum = 0;
    sum = net_tcp_checksum_sum(&pseudo, sizeof(pseudo), sum);
    sum = net_tcp_checksum_sum(tcp, sizeof(*tcp), sum);
    if (payload_len && payload)
    {
        sum = net_tcp_checksum_sum(payload, payload_len, sum);
    }

    while (sum >> 16)
    {
        sum = (sum & 0xffffU) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static void net_tcp_wake(struct net_tcp_conn *conn)
{
    (void)conn;
    sev();
}

static struct net_tcp_conn *net_tcp_find(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port)
{
    uint32_t i;

    for (i = 0; i < NET_TCP_MAX_CONNS; i++)
    {
        if (!net_tcp_conns[i].used)
        {
            continue;
        }
        if (net_tcp_conns[i].remote_ip == remote_ip &&
            net_tcp_conns[i].remote_port == remote_port &&
            net_tcp_conns[i].local_port == local_port)
        {
            return &net_tcp_conns[i];
        }
    }

    return 0;
}

static struct net_tcp_conn *net_tcp_alloc(void)
{
    uint32_t i;

    for (i = 0; i < NET_TCP_MAX_CONNS; i++)
    {
        if (!net_tcp_conns[i].used)
        {
            memset((int8_t *)&net_tcp_conns[i], 0, sizeof(net_tcp_conns[i]));
            net_tcp_conns[i].used = true;
            net_tcp_conns[i].state = NET_TCP_FREE;
            spin_lock_init(&net_tcp_conns[i].lock);
            return &net_tcp_conns[i];
        }
    }

    return 0;
}

static void net_tcp_free(struct net_tcp_conn *conn)
{
    if (!conn)
    {
        return;
    }
    memset((int8_t *)conn, 0, sizeof(*conn));
}

static uint16_t net_tcp_alloc_port(void)
{
    uint16_t port;

    spin_lock(&net_tcp_port_lock);
    port = net_tcp_next_port++;
    if (net_tcp_next_port < NET_TCP_PORT_MIN)
    {
        net_tcp_next_port = NET_TCP_PORT_MIN;
    }
    spin_unlock(&net_tcp_port_lock);
    return port;
}

static size_t net_tcp_rx_avail(const struct net_tcp_conn *conn)
{
    if (conn->rx_tail >= conn->rx_head)
    {
        return conn->rx_tail - conn->rx_head;
    }
    return NET_TCP_RX_BUF_SIZE - conn->rx_head + conn->rx_tail;
}

static size_t net_tcp_rx_space(const struct net_tcp_conn *conn)
{
    return NET_TCP_RX_BUF_SIZE - 1U - net_tcp_rx_avail(conn);
}

static size_t net_tcp_rx_copy(struct net_tcp_conn *conn, void *buf, size_t len)
{
    size_t avail;
    size_t first;
    size_t copied;

    avail = net_tcp_rx_avail(conn);
    if (!avail || !len)
    {
        return 0;
    }

    copied = (len < avail) ? len : avail;
    first = copied;
    if (conn->rx_head + first > NET_TCP_RX_BUF_SIZE)
    {
        first = NET_TCP_RX_BUF_SIZE - conn->rx_head;
    }
    memcpy((int8_t *)buf, (int8_t *)&conn->rx_buf[conn->rx_head], first);
    if (copied > first)
    {
        memcpy((int8_t *)buf + first, (int8_t *)&conn->rx_buf[0], copied - first);
    }
    conn->rx_head = (conn->rx_head + copied) % NET_TCP_RX_BUF_SIZE;
    return copied;
}

static size_t net_tcp_rx_push(struct net_tcp_conn *conn, const void *buf, size_t len)
{
    size_t space;
    size_t first;
    size_t pushed;

    space = net_tcp_rx_space(conn);
    if (!space || !len)
    {
        return 0;
    }

    pushed = (len < space) ? len : space;
    first = pushed;
    if (conn->rx_tail + first > NET_TCP_RX_BUF_SIZE)
    {
        first = NET_TCP_RX_BUF_SIZE - conn->rx_tail;
    }
    memcpy((int8_t *)&conn->rx_buf[conn->rx_tail], (int8_t *)buf, first);
    if (pushed > first)
    {
        memcpy((int8_t *)&conn->rx_buf[0], (int8_t *)buf + first, pushed - first);
    }
    conn->rx_tail = (conn->rx_tail + pushed) % NET_TCP_RX_BUF_SIZE;
    return pushed;
}

static void net_tcp_send_segment(struct net_tcp_conn *conn, uint16_t flags, const void *payload, size_t payload_len)
{
    uint8_t packet[1500];
    struct net_ipv4_hdr *ip;
    struct net_tcp_hdr *tcp;
    size_t total_len;
    uint16_t tcp_len;
    uint8_t dst_mac[6];

    if (!conn || !conn->dev)
    {
        return;
    }

    memcpy((int8_t *)dst_mac, (int8_t *)conn->remote_mac, 6);
    memset((int8_t *)packet, 0, sizeof(packet));
    ip = (struct net_ipv4_hdr *)packet;
    tcp = (struct net_tcp_hdr *)(packet + sizeof(*ip));

    total_len = sizeof(*ip) + sizeof(*tcp) + payload_len;
    if (sizeof(packet) < total_len)
    {
        return;
    }

    ip->version_ihl = (uint8_t)((4U << 4) | 5U);
    ip->tos = 0;
    tcp_len = (uint16_t)(sizeof(*tcp) + payload_len);
    ip->total_len = net_tcp_bswap16((uint16_t)(sizeof(*ip) + tcp_len));
    ip->id = 0;
    ip->frag_off = net_tcp_bswap16(0x4000U);
    ip->ttl = 64;
    ip->proto = NET_IPV4_PROTO_TCP;
    ip->checksum = 0;
    ip->src[0] = (uint8_t)((conn->dev->ipv4 >> 24) & 0xff);
    ip->src[1] = (uint8_t)((conn->dev->ipv4 >> 16) & 0xff);
    ip->src[2] = (uint8_t)((conn->dev->ipv4 >> 8) & 0xff);
    ip->src[3] = (uint8_t)(conn->dev->ipv4 & 0xff);
    ip->dst[0] = (uint8_t)((conn->remote_ip >> 24) & 0xff);
    ip->dst[1] = (uint8_t)((conn->remote_ip >> 16) & 0xff);
    ip->dst[2] = (uint8_t)((conn->remote_ip >> 8) & 0xff);
    ip->dst[3] = (uint8_t)(conn->remote_ip & 0xff);
    ip->checksum = net_tcp_bswap16(net_tcp_checksum16(ip, sizeof(*ip)));

    tcp->src_port = net_tcp_bswap16(conn->local_port);
    tcp->dst_port = net_tcp_bswap16(conn->remote_port);
    tcp->seq = net_tcp_bswap32(conn->snd_nxt);
    tcp->ack = net_tcp_bswap32(conn->rcv_nxt);
    tcp->doff_flags = net_tcp_bswap16((uint16_t)((5U << 12) | (flags & 0x1ffU)));
    tcp->window = net_tcp_bswap16(32768U);
    tcp->checksum = 0;
    tcp->urgent = 0;

    if (payload_len && payload)
    {
        memcpy((int8_t *)packet + sizeof(*ip) + sizeof(*tcp), (int8_t *)payload, payload_len);
    }

    tcp->checksum = net_tcp_bswap16(net_tcp_checksum(conn->dev, conn->remote_ip, tcp, payload, payload_len));
    (void)net_send_frame(conn->dev, dst_mac, NET_ETHERTYPE_IPV4, packet, total_len);

    if (flags & TCP_FLAG_SYN)
    {
        conn->snd_nxt++;
    }
    if (flags & TCP_FLAG_FIN)
    {
        conn->snd_nxt++;
    }
    conn->snd_nxt += (uint32_t)payload_len;
}

static void net_tcp_connect_fail_locked(struct net_tcp_conn *conn, int err)
{
    if (!conn)
    {
        return;
    }

    /*
     * 连接层的错误码需要统一落到 so_error 上，
     * 这样 getsockopt(SO_ERROR)、select/poll、read/write 的语义才一致。
     */
    conn->so_error = err;
    conn->error = true;
    conn->eof = true;
    conn->state = NET_TCP_CLOSED;
}

static bool net_tcp_connect_timeout_locked(struct net_tcp_conn *conn)
{
    uint64_t now;
    uint64_t timeout_cycles;

    if (!conn || conn->state != NET_TCP_SYN_SENT || conn->so_error != EINPROGRESS)
    {
        return false;
    }

    timeout_cycles = net_tcp_ms_to_cycles(NET_TCP_CONNECT_TIMEOUT_MS);
    now = net_tcp_cycles();
    if (now - conn->connect_start_cycles < timeout_cycles)
    {
        return false;
    }

    net_tcp_connect_fail_locked(conn, ETIMEDOUT);
    net_tcp_wake(conn);
    return true;
}

static int net_tcp_wait_ack(struct net_tcp_conn *conn, uint32_t target_una, uint32_t timeout_ms)
{
    uint64_t deadline;
    bool infinite;

    infinite = timeout_ms == 0;
    deadline = infinite ? 0 : (net_tcp_cycles() + net_tcp_ms_to_cycles(timeout_ms));
    while (infinite || net_tcp_cycles() < deadline)
    {
        spin_lock(&conn->lock);
        if (conn->error)
        {
            spin_unlock(&conn->lock);
            return -ECONNRESET;
        }
        if (conn->snd_una >= target_una)
        {
            spin_unlock(&conn->lock);
            return 0;
        }
        spin_unlock(&conn->lock);
        virtio_net_poll();
        enable_irq();
        wfe();
        disable_irq();
    }
    return -ETIMEDOUT;
}

static ssize_t net_tcp_recv_wait(struct net_tcp_conn *conn, void *buf, size_t len, uint32_t timeout_ms)
{
    uint64_t deadline;
    size_t copied;
    bool infinite;

    if (!len)
    {
        return 0;
    }

    infinite = timeout_ms == 0;
    deadline = infinite ? 0 : (net_tcp_cycles() + net_tcp_ms_to_cycles(timeout_ms));
    while (infinite || net_tcp_cycles() < deadline)
    {
        spin_lock(&conn->lock);
        copied = net_tcp_rx_copy(conn, buf, len);
        if (copied)
        {
            spin_unlock(&conn->lock);
            return (ssize_t)copied;
        }
        if (conn->eof)
        {
            spin_unlock(&conn->lock);
            return 0;
        }
        if (conn->error)
        {
            spin_unlock(&conn->lock);
            return -ECONNRESET;
        }
        spin_unlock(&conn->lock);
        virtio_net_poll();
        enable_irq();
        wfe();
        disable_irq();
    }
    return -ETIMEDOUT;
}

static int net_tcp_connect(struct net_device *dev, uint32_t target_ip, uint16_t port,
                           struct net_tcp_conn **out_conn, uint32_t timeout_ms);
static void net_tcp_close(struct net_tcp_conn *conn);

static int net_tcp_connect_begin(struct net_device *dev, uint32_t target_ip, uint16_t port,
                                 struct net_tcp_conn **out_conn)
{
    struct net_tcp_conn *conn;
    uint8_t remote_mac[6];

    if (!dev || !out_conn)
    {
        return -EINVAL;
    }

    conn = net_tcp_alloc();
    if (!conn)
    {
        return -ENOSPC;
    }

    conn->dev = dev;
    conn->remote_ip = target_ip;
    conn->remote_port = port ? port : 80U;
    conn->local_port = net_tcp_alloc_port();
    conn->iss = (uint32_t)(net_tcp_cycles() ^ ((uint64_t)(task_current() ? task_current()->pid : 0) << 16) ^ (uint64_t)conn->local_port);
    conn->snd_nxt = conn->iss;
    conn->snd_una = conn->iss;
    conn->rcv_nxt = 0;
    conn->connect_start_cycles = net_tcp_cycles();
    conn->so_error = EINPROGRESS;

    if (net_arp_resolve(dev, target_ip, remote_mac) < 0)
    {
        net_tcp_free(conn);
        return -EHOSTUNREACH;
    }
    memcpy((int8_t *)conn->remote_mac, (int8_t *)remote_mac, 6);

    conn->state = NET_TCP_SYN_SENT;
    net_tcp_send_segment(conn, TCP_FLAG_SYN, 0, 0);
    *out_conn = conn;
    return -EINPROGRESS;
}

static int net_tcp_connect(struct net_device *dev, uint32_t target_ip, uint16_t port,
                           struct net_tcp_conn **out_conn, uint32_t timeout_ms)
{
    struct net_tcp_conn *conn;
    uint64_t deadline;
    bool infinite;
    int ret;

    if (!dev || !out_conn)
    {
        return -EINVAL;
    }

    ret = net_tcp_connect_begin(dev, target_ip, port, &conn);
    if (ret < 0 && ret != -EINPROGRESS)
    {
        return ret;
    }

    infinite = timeout_ms == 0;
    deadline = infinite ? 0 : (net_tcp_cycles() + net_tcp_ms_to_cycles(timeout_ms));
    while (infinite || net_tcp_cycles() < deadline)
    {
        spin_lock(&conn->lock);
        if (conn->state == NET_TCP_ESTABLISHED)
        {
            spin_unlock(&conn->lock);
            *out_conn = conn;
            return 0;
        }
        if (net_tcp_connect_timeout_locked(conn))
        {
            int err;

            err = conn->so_error ? conn->so_error : ETIMEDOUT;
            spin_unlock(&conn->lock);
            net_tcp_free(conn);
            return -err;
        }
        if (conn->error)
        {
            int err;

            err = conn->so_error ? conn->so_error : ECONNRESET;
            spin_unlock(&conn->lock);
            net_tcp_free(conn);
            return -err;
        }
        spin_unlock(&conn->lock);
        virtio_net_poll();
        enable_irq();
        wfe();
        disable_irq();
    }

    net_tcp_free(conn);
    return -ETIMEDOUT;
}

int net_socket_connect_begin(struct net_socket *sock, struct net_device *dev, uint32_t target_ip, uint16_t port)
{
    struct net_tcp_conn *conn;
    int ret;

    if (!sock || !dev)
    {
        return -EINVAL;
    }

    if (sock->priv)
    {
        net_tcp_close((struct net_tcp_conn *)sock->priv);
        sock->priv = 0;
    }

    ret = net_tcp_connect_begin(dev, target_ip, port, &conn);
    if (ret < 0 && ret != -EINPROGRESS)
    {
        return ret;
    }

    sock->priv = conn;
    return ret;
}

static void net_tcp_close(struct net_tcp_conn *conn)
{
    if (!conn)
    {
        return;
    }
    spin_lock(&conn->lock);
    conn->state = NET_TCP_CLOSED;
    conn->used = false;
    spin_unlock(&conn->lock);
    memset((int8_t *)conn, 0, sizeof(*conn));
}

static int64_t net_http_write_body(int out_fd, const void *buf, size_t len)
{
    ssize_t ret;

    if (!len)
    {
        return 0;
    }

    if (out_fd >= 0 && out_fd < 3)
    {
        tty_write_bytes(buf, len);
        return (int64_t)len;
    }

    ret = vfs_write(out_fd - 3, buf, len);
    return (int64_t)ret;
}

static const int8_t *net_http_find_ci(const int8_t *haystack, size_t hay_len, const char *needle)
{
    size_t nlen;
    size_t i;
    size_t j;

    if (!haystack || !needle)
    {
        return 0;
    }

    nlen = strlen((int8_t *)needle);
    if (!nlen || hay_len < nlen)
    {
        return 0;
    }

    for (i = 0; i + nlen <= hay_len; i++)
    {
        for (j = 0; j < nlen; j++)
        {
            char a = (char)haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z')
            {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z')
            {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b)
            {
                break;
            }
        }
        if (j == nlen)
        {
            return &haystack[i];
        }
    }

    return 0;
}

static int64_t net_http_parse_content_length(const int8_t *hdr, size_t hdr_len)
{
    const int8_t *p;
    int64_t value;

    p = net_http_find_ci(hdr, hdr_len, "Content-Length:");
    if (!p)
    {
        return -1;
    }
    p += strlen("Content-Length:");
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    value = 0;
    while (*p >= '0' && *p <= '9')
    {
        value = value * 10 + (int64_t)(*p - '0');
        p++;
    }
    return value;
}

static ssize_t net_http_consume_stream(struct net_tcp_conn *conn, int out_fd, uint32_t timeout_ms)
{
    int8_t hdr_buf[2048];
    size_t hdr_len;
    bool headers_done;
    int64_t content_len;
    int64_t body_written;
    ssize_t ret;
    size_t copy_len;
    size_t i;

    hdr_len = 0;
    headers_done = false;
    content_len = -1;
    body_written = 0;

    while (1)
    {
        if (!headers_done)
        {
            const int8_t *mark;
            size_t body_start;

            mark = 0;
            if (hdr_len >= 4)
            {
                for (i = 3; i < hdr_len; i++)
                {
                    if (hdr_buf[i - 3] == '\r' && hdr_buf[i - 2] == '\n' &&
                        hdr_buf[i - 1] == '\r' && hdr_buf[i] == '\n')
                    {
                        mark = &hdr_buf[i - 3];
                        break;
                    }
                }
            }

            if (!mark)
            {
                int8_t chunk[512];
                ret = net_tcp_recv_wait(conn, chunk, sizeof(chunk), timeout_ms);
                if (ret < 0)
                {
                    return ret;
                }
                if (ret == 0)
                {
                    return body_written;
                }
                if (hdr_len + (size_t)ret > sizeof(hdr_buf))
                {
                    return -EMSGSIZE;
                }
                memcpy(hdr_buf + hdr_len, chunk, (size_t)ret);
                hdr_len += (size_t)ret;
                continue;
            }

            headers_done = true;
            body_start = (size_t)((mark - hdr_buf) + 4);
            content_len = net_http_parse_content_length(hdr_buf, body_start);
            if (body_start < hdr_len)
            {
                copy_len = hdr_len - body_start;
                ret = net_http_write_body(out_fd, hdr_buf + body_start, copy_len);
                if (ret < 0)
                {
                    return ret;
                }
                body_written += ret;
            }
            if (content_len >= 0 && body_written >= content_len)
            {
                return body_written;
            }
            continue;
        }

        {
            int8_t chunk[1024];
            ret = net_tcp_recv_wait(conn, chunk, sizeof(chunk), timeout_ms);
            if (ret < 0)
            {
                return ret;
            }
            if (ret == 0)
            {
                return body_written;
            }
            if (content_len >= 0 && body_written + ret > content_len)
            {
                ret = (ssize_t)(content_len - body_written);
            }
            if (ret > 0)
            {
                ssize_t written;
                written = net_http_write_body(out_fd, chunk, (size_t)ret);
                if (written < 0)
                {
                    return written;
                }
                body_written += written;
            }
            if (content_len >= 0 && body_written >= content_len)
            {
                return body_written;
            }
        }
    }
}

void net_tcp_input(struct net_device *dev, const void *eth_payload, size_t len, uint32_t src_ip, uint32_t dst_ip)
{
    const struct net_tcp_hdr *tcp;
    struct net_tcp_conn *conn;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t flags;
    size_t hdr_len;
    size_t payload_len;
    const uint8_t *payload;
    uint32_t seq;
    uint32_t ack;
    size_t pushed;

    (void)dst_ip;

    if (!dev || !eth_payload || len < sizeof(*tcp))
    {
        return;
    }

    tcp = (const struct net_tcp_hdr *)eth_payload;
    hdr_len = (size_t)((net_tcp_bswap16(tcp->doff_flags) >> 12) * 4U);
    if (hdr_len < sizeof(*tcp) || len < hdr_len)
    {
        return;
    }

    payload = (const uint8_t *)eth_payload + hdr_len;
    payload_len = len - hdr_len;
    src_port = net_tcp_bswap16(tcp->src_port);
    dst_port = net_tcp_bswap16(tcp->dst_port);
    flags = net_tcp_bswap16(tcp->doff_flags) & 0x01ffU;
    seq = net_tcp_bswap32(tcp->seq);
    ack = net_tcp_bswap32(tcp->ack);

    conn = net_tcp_find(src_ip, src_port, dst_port);
    if (!conn)
    {
        return;
    }

    spin_lock(&conn->lock);
    if (conn->state == NET_TCP_SYN_SENT)
    {
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            ack == conn->snd_nxt)
        {
            conn->rcv_nxt = seq + 1U;
            conn->snd_una = ack;
            conn->so_error = 0;
            conn->state = NET_TCP_ESTABLISHED;
            net_tcp_send_segment(conn, TCP_FLAG_ACK, 0, 0);
            net_tcp_wake(conn);
            spin_unlock(&conn->lock);
            return;
        }
    }
    else if (conn->state == NET_TCP_ESTABLISHED)
    {
        if ((flags & TCP_FLAG_RST) != 0)
        {
            conn->so_error = ECONNRESET;
            conn->error = true;
            conn->eof = true;
            net_tcp_wake(conn);
            spin_unlock(&conn->lock);
            return;
        }

        if (ack > conn->snd_una)
        {
            conn->snd_una = ack;
        }

        if (payload_len)
        {
            if (seq == conn->rcv_nxt)
            {
                pushed = net_tcp_rx_push(conn, payload, payload_len);
                conn->rcv_nxt += (uint32_t)pushed;
                net_tcp_send_segment(conn, TCP_FLAG_ACK, 0, 0);
                net_tcp_wake(conn);
                spin_unlock(&conn->lock);
                return;
            }
            else if (seq > conn->rcv_nxt)
            {
                net_tcp_send_segment(conn, TCP_FLAG_ACK, 0, 0);
                spin_unlock(&conn->lock);
                return;
            }
        }

        if ((flags & TCP_FLAG_FIN) != 0)
        {
            conn->rcv_nxt = seq + 1U;
            conn->eof = true;
            net_tcp_send_segment(conn, TCP_FLAG_ACK, 0, 0);
            net_tcp_wake(conn);
            spin_unlock(&conn->lock);
            return;
        }
    }

    spin_unlock(&conn->lock);
}

int64_t net_http_get(struct net_device *dev, uint32_t target_ip, uint16_t port, const int8_t *path,
                     int out_fd, uint32_t timeout_ms)
{
    struct net_tcp_conn *conn;
    int8_t req[512];
    int64_t req_len;
    int64_t total_written;
    int64_t ret;
    size_t path_len;

    if (!dev || !path || !path[0])
    {
        return -EINVAL;
    }

    if (path[0] == '/')
    {
        path_len = strlen((int8_t *)path);
    }
    else
    {
        path_len = strlen((int8_t *)path) + 1U;
    }

    if (path_len + 128U >= sizeof(req))
    {
        return -ENAMETOOLONG;
    }

    ret = net_tcp_connect(dev, target_ip, port, &conn, timeout_ms);
    if (ret < 0)
    {
        return ret;
    }

    req_len = sprintf(req,
                      "GET %s HTTP/1.0\r\n"
                      "Host: %u.%u.%u.%u\r\n"
                      "Connection: close\r\n"
                      "User-Agent: stupidos-wget/0.1\r\n"
                      "Accept: */*\r\n\r\n",
                      (const int8_t *)path,
                      (target_ip >> 24) & 0xff, (target_ip >> 16) & 0xff,
                      (target_ip >> 8) & 0xff, target_ip & 0xff);
    if (req_len <= 0 || req_len >= (int64_t)sizeof(req))
    {
        net_tcp_close(conn);
        return -EINVAL;
    }

    net_tcp_send_segment(conn, TCP_FLAG_ACK | TCP_FLAG_PSH, req, (size_t)req_len);
    if (net_tcp_wait_ack(conn, conn->snd_nxt, timeout_ms) < 0)
    {
        net_tcp_close(conn);
        return -ETIMEDOUT;
    }

    total_written = net_http_consume_stream(conn, out_fd, timeout_ms);
    net_tcp_close(conn);
    return total_written;
}

int net_socket_init(struct net_socket *sock)
{
    if (!sock)
    {
        return -EINVAL;
    }

    sock->priv = 0;
    return 0;
}

int net_socket_connect(struct net_socket *sock, struct net_device *dev, uint32_t target_ip, uint16_t port, uint32_t timeout_ms)
{
    struct net_tcp_conn *conn;
    uint64_t deadline;
    bool infinite;
    int ret;

    if (!sock || !dev)
    {
        return -EINVAL;
    }

    ret = net_socket_connect_begin(sock, dev, target_ip, port);
    if (ret < 0 && ret != -EINPROGRESS)
    {
        return ret;
    }

    conn = (struct net_tcp_conn *)sock->priv;
    if (!conn)
    {
        return -ENOTCONN;
    }

    if (conn->state == NET_TCP_ESTABLISHED)
    {
        return 0;
    }

    infinite = timeout_ms == 0;
    deadline = infinite ? 0 : (net_tcp_cycles() + net_tcp_ms_to_cycles(timeout_ms));
    while (infinite || net_tcp_cycles() < deadline)
    {
        spin_lock(&conn->lock);
        if (conn->state == NET_TCP_ESTABLISHED)
        {
            spin_unlock(&conn->lock);
            return 0;
        }
        if (net_tcp_connect_timeout_locked(conn))
        {
            int err;

            err = conn->so_error ? conn->so_error : ETIMEDOUT;
            spin_unlock(&conn->lock);
            net_tcp_close(conn);
            sock->priv = 0;
            return -err;
        }
        if (conn->error)
        {
            int err;

            err = conn->so_error ? conn->so_error : ECONNRESET;
            spin_unlock(&conn->lock);
            net_tcp_close(conn);
            sock->priv = 0;
            return -err;
        }
        spin_unlock(&conn->lock);
        virtio_net_poll();
        enable_irq();
        wfe();
        disable_irq();
    }

    net_tcp_close(conn);
    sock->priv = 0;
    return -ETIMEDOUT;
}

ssize_t net_socket_read(struct net_socket *sock, void *buf, size_t len, uint32_t timeout_ms)
{
    struct net_tcp_conn *conn;

    if (!sock || !sock->priv)
    {
        return -ENOTCONN;
    }

    conn = (struct net_tcp_conn *)sock->priv;
    if (conn->state != NET_TCP_ESTABLISHED)
    {
        return conn->so_error ? -conn->so_error : -EAGAIN;
    }
    return net_tcp_recv_wait(conn, buf, len, timeout_ms);
}

ssize_t net_socket_write(struct net_socket *sock, const void *buf, size_t len, uint32_t timeout_ms)
{
    struct net_tcp_conn *conn;
    size_t done;
    size_t chunk;
    uint32_t target_una;

    if (!sock || !sock->priv)
    {
        return -ENOTCONN;
    }
    if (!buf && len)
    {
        return -EINVAL;
    }

    conn = (struct net_tcp_conn *)sock->priv;
    if (conn->state != NET_TCP_ESTABLISHED)
    {
        return conn->so_error ? -conn->so_error : -EINPROGRESS;
    }
    done = 0;
    while (done < len)
    {
        chunk = len - done;
        if (chunk > 1024U)
        {
            chunk = 1024U;
        }
        target_una = conn->snd_nxt + (uint32_t)chunk;
        net_tcp_send_segment(conn, TCP_FLAG_ACK | TCP_FLAG_PSH, (const uint8_t *)buf + done, chunk);
        if (net_tcp_wait_ack(conn, target_una, timeout_ms) < 0)
        {
            return -ETIMEDOUT;
        }
        done += chunk;
    }

    return (ssize_t)done;
}

int net_socket_shutdown(struct net_socket *sock, int how)
{
    struct net_tcp_conn *conn;
    uint32_t target_una;

    if (!sock || !sock->priv)
    {
        return -ENOTCONN;
    }

    conn = (struct net_tcp_conn *)sock->priv;
    if (conn->state == NET_TCP_SYN_SENT)
    {
        return -EINPROGRESS;
    }
    if (how == 0)
    {
        return 0;
    }

    if (how != 1 && how != 2)
    {
        return -EINVAL;
    }

    target_una = conn->snd_nxt + 1U;
    net_tcp_send_segment(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
    (void)net_tcp_wait_ack(conn, target_una, 3000U);
    conn->eof = true;
    if (how == 2)
    {
        net_tcp_close(conn);
        sock->priv = 0;
    }
    return 0;
}

int net_socket_pending(struct net_socket *sock)
{
    struct net_tcp_conn *conn;

    if (!sock || !sock->priv)
    {
        return 0;
    }

    conn = (struct net_tcp_conn *)sock->priv;
    return (int)net_tcp_rx_avail(conn);
}

int net_socket_so_error(struct net_socket *sock)
{
    struct net_tcp_conn *conn;

    if (!sock || !sock->priv)
    {
        return ENOTCONN;
    }

    conn = (struct net_tcp_conn *)sock->priv;
    spin_lock(&conn->lock);
    if (net_tcp_connect_timeout_locked(conn))
    {
        int err;

        err = conn->so_error ? conn->so_error : ETIMEDOUT;
        spin_unlock(&conn->lock);
        return err;
    }
    if (conn->error)
    {
        int err;

        err = conn->so_error ? conn->so_error : ECONNRESET;
        spin_unlock(&conn->lock);
        return err;
    }
    if (conn->state == NET_TCP_SYN_SENT)
    {
        spin_unlock(&conn->lock);
        return EINPROGRESS;
    }
    if (conn->state != NET_TCP_ESTABLISHED)
    {
        spin_unlock(&conn->lock);
        return ENOTCONN;
    }
    spin_unlock(&conn->lock);
    return 0;
}

void net_socket_close(struct net_socket *sock)
{
    if (!sock || !sock->priv)
    {
        return;
    }

    net_tcp_close((struct net_tcp_conn *)sock->priv);
    sock->priv = 0;
}
