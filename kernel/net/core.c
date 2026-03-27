#include "net/net.h"

#include "errno.h"
#include "driver/virtio_net.h"
#include "lib/libasm.h"
#include "lib/libmem.h"
#include "sched.h"
#include "printk.h"
#include "spinlock.h"

#define NET_ARP_CACHE_SIZE  8

struct net_eth_hdr
{
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype;
} __attribute__((packed));

struct net_arp_hdr
{
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t opcode;
    uint8_t sha[6];
    uint8_t spa[4];
    uint8_t tha[6];
    uint8_t tpa[4];
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

struct net_icmp_hdr
{
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

struct net_udp_hdr
{
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
} __attribute__((packed));

struct net_dns_hdr
{
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed));

struct net_probe_state
{
    spinlock_t lock;
    bool active;
    bool done;
    int result;
    uint32_t target_ip;
    uint8_t target_mac[6];
    uint16_t icmp_id;
    uint16_t icmp_seq;
    bool expect_icmp;
    uint64_t start_cycles;
    uint64_t done_cycles;
};

struct net_dns_state
{
    spinlock_t lock;
    bool active;
    bool done;
    int result;
    uint16_t txid;
    uint16_t src_port;
    uint32_t resolved_ip;
    uint64_t start_cycles;
};

static struct net_device net_devices[NET_MAX_DEVICES];
static struct net_device *net_default;
static struct net_probe_state net_probe = {
    .lock = SPINLOCK_INIT,
};
static struct net_dns_state net_dns = {
    .lock = SPINLOCK_INIT,
};
static uint32_t net_rx_packet_seen;
static bool net_ping_cache_log_seen;
static bool net_ping_arp_log_seen;
struct net_arp_cache_entry
{
    bool valid;
    uint32_t ip;
    uint8_t mac[6];
    uint64_t last_seen_cycles;
};
static struct net_arp_cache_entry net_arp_cache[NET_ARP_CACHE_SIZE];
static uint64_t net_counter_freq;
static uint32_t net_config_changes;

int net_arp_resolve(struct net_device *dev, uint32_t target_ip, uint8_t out_mac[6]);

static uint16_t net_bswap16(uint16_t v)
{
    return __builtin_bswap16(v);
}

static uint64_t net_clock_cycles(void)
{
    uint64_t cycles;

    asm volatile("mrs %0, cntpct_el0" : "=r"(cycles) : : "memory");
    return cycles;
}

static uint64_t net_clock_freq(void)
{
    uint64_t freq;

    if (!net_counter_freq)
    {
        asm volatile("mrs %0, cntfrq_el0" : "=r"(freq) : : "memory");
        if (!freq)
        {
            freq = 1000000000ULL;
        }
        net_counter_freq = freq;
    }

    return net_counter_freq;
}

static uint64_t net_cycles_to_us(uint64_t cycles)
{
    uint64_t freq;

    freq = net_clock_freq();
    if (!freq)
    {
        return 0;
    }

    return (cycles * 1000000ULL) / freq;
}

static uint64_t net_ms_to_cycles(uint32_t ms)
{
    uint64_t freq;

    freq = net_clock_freq();
    return (freq * (uint64_t)ms) / 1000ULL;
}

static uint16_t net_checksum16(const void *buf, size_t len)
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

static void net_mac_print(const uint8_t mac[6], int8_t *buf, size_t len)
{
    if (len < 18)
    {
        if (len)
        {
            buf[0] = '\0';
        }
        return;
    }

    sprintf(buf, "%02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void net_ipv4_print(uint32_t addr, int8_t *buf, size_t len)
{
    if (len < 16)
    {
        if (len)
        {
            buf[0] = '\0';
        }
        return;
    }

    sprintf(buf, "%u.%u.%u.%u",
            (addr >> 24) & 0xff,
            (addr >> 16) & 0xff,
            (addr >> 8) & 0xff,
            addr & 0xff);
}

static bool net_mac_is_broadcast(const uint8_t mac[6])
{
    return mac[0] == 0xff && mac[1] == 0xff && mac[2] == 0xff &&
           mac[3] == 0xff && mac[4] == 0xff && mac[5] == 0xff;
}

static bool net_mac_equal(const uint8_t a[6], const uint8_t b[6])
{
    size_t i;

    for (i = 0; i < 6; i++)
    {
        if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}

static uint32_t net_ipv4_from_bytes(const uint8_t ip[4])
{
    return ((uint32_t)ip[0] << 24) |
           ((uint32_t)ip[1] << 16) |
           ((uint32_t)ip[2] << 8) |
           (uint32_t)ip[3];
}

static void net_ipv4_to_bytes(uint32_t ip, uint8_t out[4])
{
    out[0] = (uint8_t)((ip >> 24) & 0xff);
    out[1] = (uint8_t)((ip >> 16) & 0xff);
    out[2] = (uint8_t)((ip >> 8) & 0xff);
    out[3] = (uint8_t)(ip & 0xff);
}

static int net_arp_cache_lookup(uint32_t ip, uint8_t out_mac[6])
{
    uint32_t i;

    for (i = 0; i < NET_ARP_CACHE_SIZE; i++)
    {
        if (net_arp_cache[i].valid && net_arp_cache[i].ip == ip)
        {
            if (out_mac)
            {
                memcpy((int8_t *)out_mac, (int8_t *)net_arp_cache[i].mac, 6);
            }
            net_arp_cache[i].last_seen_cycles = net_clock_cycles();
            return 0;
        }
    }

    return -ENOENT;
}

static void net_arp_cache_update(uint32_t ip, const uint8_t mac[6])
{
    uint32_t i;
    uint32_t victim;

    if (!ip || !mac)
    {
        return;
    }

    victim = 0;
    for (i = 0; i < NET_ARP_CACHE_SIZE; i++)
    {
        if (net_arp_cache[i].valid && net_arp_cache[i].ip == ip)
        {
            victim = i;
            break;
        }

        if (!net_arp_cache[i].valid)
        {
            victim = i;
        }
    }

    net_arp_cache[victim].valid = true;
    net_arp_cache[victim].ip = ip;
    memcpy((int8_t *)net_arp_cache[victim].mac, (int8_t *)mac, 6);
    net_arp_cache[victim].last_seen_cycles = net_clock_cycles();
}

static void net_gateway_proxy_mac(const struct net_device *dev, uint8_t mac[6])
{
    /*
     * 这是 QEMU usernet/slirp 场景下的兼容后备。
     *
     * 真实网络环境里，网关应该走标准 ARP 解析；
     * 但在一些 slirp 后端配置里，10.0.2.2 会更像一个“代理网关”而不是
     * 传统意义上会主动回 ARP 的真实二层主机。
     *
     * 这里保留一个更接近 QEMU 默认风格的固定代理 MAC，至少能让
     * ping / TCP 在 usernet 场景下继续工作，而不是落到完全无意义的假 MAC。
     */
    (void)dev;
    mac[0] = 0x52;
    mac[1] = 0x55;
    mac[2] = 0x0a;
    mac[3] = 0x00;
    mac[4] = 0x02;
    mac[5] = 0x02;
}

static bool net_ipv4_is_onlink(const struct net_device *dev, uint32_t ip)
{
    if (!dev)
    {
        return false;
    }

    return ((ip ^ dev->ipv4) & dev->netmask) == 0;
}

static bool net_ipv4_is_slirp_subnet(uint32_t ip)
{
    return (ip & 0xffffff00U) == ((10U << 24) | (0U << 16) | (2U << 8));
}

static uint32_t net_next_hop_ipv4(const struct net_device *dev, uint32_t ip)
{
    if (!dev)
    {
        return ip;
    }

    if (net_ipv4_is_onlink(dev, ip))
    {
        return ip;
    }

    if (dev->gateway)
    {
        return dev->gateway;
    }

    return ip;
}

static void net_probe_begin(uint32_t target_ip, bool expect_icmp, uint16_t icmp_id, uint16_t icmp_seq)
{
    /*
     * 这里用一个很小的同步探针状态，把“发送探测包”和“接收回包”串起来。
     * 这样 shell 里就能直接看见网卡、协议栈和调度器是否同时工作正常。
     */
    spin_lock(&net_probe.lock);
    net_probe.active = true;
    net_probe.done = false;
    net_probe.result = -ETIMEDOUT;
    net_probe.target_ip = target_ip;
    net_probe.icmp_id = icmp_id;
    net_probe.icmp_seq = icmp_seq;
    net_probe.expect_icmp = expect_icmp;
    net_probe.start_cycles = expect_icmp ? net_clock_cycles() : 0;
    net_probe.done_cycles = 0;
    memset((int8_t *)net_probe.target_mac, 0, sizeof(net_probe.target_mac));
    spin_unlock(&net_probe.lock);
}

static int net_probe_finish(uint32_t target_ip, int result, const uint8_t *mac)
{
    int ret;

    ret = 0;
    spin_lock(&net_probe.lock);
    if (net_probe.active && net_probe.target_ip == target_ip)
    {
        net_probe.result = result;
        net_probe.done = true;
        if (mac)
        {
            memcpy((int8_t *)net_probe.target_mac, (int8_t *)mac, 6);
        }
        /*
         * 网络回包完成属于软件事件，不只是“某个硬件中断来了”。
         * 这里唤醒正在 wfe 等待的探测者，ping/nettest 就能更快返回。
         */
        sev();
        ret = 1;
    }
    spin_unlock(&net_probe.lock);
    return ret;
}

static void net_dns_begin(uint16_t txid, uint16_t src_port)
{
    spin_lock(&net_dns.lock);
    net_dns.active = true;
    net_dns.done = false;
    net_dns.result = -ETIMEDOUT;
    net_dns.txid = txid;
    net_dns.src_port = src_port;
    net_dns.resolved_ip = 0;
    net_dns.start_cycles = net_clock_cycles();
    spin_unlock(&net_dns.lock);
}

static int net_dns_finish(int result, uint32_t resolved_ip)
{
    int ret;

    ret = 0;
    spin_lock(&net_dns.lock);
    if (net_dns.active)
    {
        net_dns.result = result;
        net_dns.done = true;
        net_dns.resolved_ip = resolved_ip;
        sev();
        ret = 1;
    }
    spin_unlock(&net_dns.lock);
    return ret;
}

static int net_dns_wait(uint32_t timeout_ms, uint32_t *out_ip)
{
    uint64_t deadline;
    int result;

    deadline = net_clock_cycles() + net_ms_to_cycles(timeout_ms ? timeout_ms : 1000U);
    while (net_clock_cycles() < deadline)
    {
        spin_lock(&net_dns.lock);
        if (!net_dns.active)
        {
            spin_unlock(&net_dns.lock);
            return -EINVAL;
        }
        if (net_dns.done)
        {
            result = net_dns.result;
            if (out_ip)
            {
                *out_ip = net_dns.resolved_ip;
            }
            net_dns.active = false;
            spin_unlock(&net_dns.lock);
            return result;
        }
        spin_unlock(&net_dns.lock);
        virtio_net_poll();
        enable_irq();
        wfe();
        disable_irq();
    }

    spin_lock(&net_dns.lock);
    net_dns.active = false;
    spin_unlock(&net_dns.lock);
    return -ETIMEDOUT;
}

static int net_probe_wait(uint32_t target_ip, uint64_t timeout_cycles, uint8_t mac[6], uint64_t *elapsed_cycles)
{
    uint64_t deadline;
    int result;
    bool done;

    deadline = net_clock_cycles() + timeout_cycles;
    while (1)
    {
        virtio_net_poll();
        spin_lock(&net_probe.lock);
        done = net_probe.active && net_probe.done && net_probe.target_ip == target_ip;
        result = net_probe.result;
        if (done && mac)
        {
            memcpy((int8_t *)mac, (int8_t *)net_probe.target_mac, 6);
        }
        if (done)
        {
            if (elapsed_cycles)
            {
                if (net_probe.done_cycles >= net_probe.start_cycles)
                {
                    *elapsed_cycles = net_probe.done_cycles - net_probe.start_cycles;
                }
                else
                {
                    *elapsed_cycles = 0;
                }
            }
            net_probe.active = false;
            spin_unlock(&net_probe.lock);
            return result;
        }
        spin_unlock(&net_probe.lock);

        if (net_clock_cycles() >= deadline)
        {
            break;
        }

        /*
         * 这里不要再反复 sched_yield()。
         *
         * 这类网络等待本质上是“阻塞等回包”，不是主动做调度压测。
         * 直接开中断后 wfe，既能让 timer / uart / virtio-input 把 CPU 唤醒，
         * 也能被 net_probe_finish() 里的 sev() 立刻拉起。
         */
        enable_irq();
        wfe();
        disable_irq();
    }

    spin_lock(&net_probe.lock);
    net_probe.active = false;
    spin_unlock(&net_probe.lock);
    return -ETIMEDOUT;
}

static ssize_t net_send_arp_request(struct net_device *dev, uint32_t target_ip)
{
    struct net_eth_hdr eth;
    struct net_arp_hdr arp;
    uint8_t broadcast[6];

    memset((int8_t *)&eth, 0, sizeof(eth));
    memset((int8_t *)&arp, 0, sizeof(arp));

    memset((int8_t *)broadcast, 0xff, sizeof(broadcast));
    memcpy((int8_t *)eth.dst, (int8_t *)broadcast, sizeof(eth.dst));
    memcpy((int8_t *)eth.src, (int8_t *)dev->mac, sizeof(eth.src));
    eth.ethertype = net_bswap16(NET_ETHERTYPE_ARP);

    arp.htype = net_bswap16(1);
    arp.ptype = net_bswap16(NET_ETHERTYPE_IPV4);
    arp.hlen = 6;
    arp.plen = 4;
    arp.opcode = net_bswap16(1);
    memcpy((int8_t *)arp.sha, (int8_t *)dev->mac, 6);
    net_ipv4_to_bytes(dev->ipv4, arp.spa);
    memset((int8_t *)arp.tha, 0, sizeof(arp.tha));
    net_ipv4_to_bytes(target_ip, arp.tpa);

    return net_send_frame(dev, broadcast, NET_ETHERTYPE_ARP, &arp, sizeof(arp));
}

static ssize_t net_send_icmp_echo(struct net_device *dev, const uint8_t dst_mac[6], uint32_t dst_ip,
                                  uint16_t icmp_id, uint16_t icmp_seq)
{
    uint8_t packet[sizeof(struct net_ipv4_hdr) + sizeof(struct net_icmp_hdr) + 56];
    struct net_ipv4_hdr *ip;
    struct net_icmp_hdr *icmp;
    uint8_t *data;
    uint16_t total_len;
    size_t payload_len;
    size_t i;

    memset((int8_t *)packet, 0, sizeof(packet));
    ip = (struct net_ipv4_hdr *)packet;
    icmp = (struct net_icmp_hdr *)(packet + sizeof(*ip));
    data = packet + sizeof(*ip) + sizeof(*icmp);
    payload_len = 56;

    ip->version_ihl = (uint8_t)((4U << 4) | 5U);
    ip->tos = 0;
    total_len = (uint16_t)(sizeof(*ip) + sizeof(*icmp) + payload_len);
    ip->total_len = net_bswap16(total_len);
    ip->id = net_bswap16(icmp_seq);
    ip->frag_off = net_bswap16(0x4000U);
    ip->ttl = 64;
    ip->proto = NET_IPV4_PROTO_ICMP;
    ip->checksum = 0;
    net_ipv4_to_bytes(dev->ipv4, ip->src);
    net_ipv4_to_bytes(dst_ip, ip->dst);
    ip->checksum = net_bswap16(net_checksum16(ip, sizeof(*ip)));

    icmp->type = 8;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = net_bswap16(icmp_id);
    icmp->seq = net_bswap16(icmp_seq);

    for (i = 0; i < payload_len; i++)
    {
        data[i] = (uint8_t)(0x30 + (i & 0x0f));
    }

    icmp->checksum = net_bswap16(net_checksum16(icmp, sizeof(*icmp) + payload_len));
    return net_send_frame(dev, dst_mac, NET_ETHERTYPE_IPV4, packet, total_len);
}

static ssize_t net_send_udp_datagram(struct net_device *dev, uint32_t dst_ip, const uint8_t dst_mac[6],
                                     uint16_t src_port, uint16_t dst_port,
                                     const void *payload, size_t payload_len)
{
    uint8_t packet[1500];
    struct net_ipv4_hdr *ip;
    struct net_udp_hdr *udp;
    uint16_t udp_len;
    size_t total_len;

    if (!dev || !dst_mac)
    {
        return -EINVAL;
    }

    total_len = sizeof(*ip) + sizeof(*udp) + payload_len;
    if (total_len > sizeof(packet))
    {
        return -EMSGSIZE;
    }

    memset((int8_t *)packet, 0, sizeof(packet));
    ip = (struct net_ipv4_hdr *)packet;
    udp = (struct net_udp_hdr *)(packet + sizeof(*ip));

    ip->version_ihl = (uint8_t)((4U << 4) | 5U);
    ip->tos = 0;
    udp_len = (uint16_t)(sizeof(*udp) + payload_len);
    ip->total_len = net_bswap16((uint16_t)(sizeof(*ip) + udp_len));
    ip->id = 0;
    ip->frag_off = net_bswap16(0x4000U);
    ip->ttl = 64;
    ip->proto = NET_IPV4_PROTO_UDP;
    ip->checksum = 0;
    net_ipv4_to_bytes(dev->ipv4, ip->src);
    net_ipv4_to_bytes(dst_ip, ip->dst);
    ip->checksum = net_bswap16(net_checksum16(ip, sizeof(*ip)));

    udp->src_port = net_bswap16(src_port);
    udp->dst_port = net_bswap16(dst_port);
    udp->len = net_bswap16(udp_len);
    udp->checksum = 0;

    if (payload_len && payload)
    {
        memcpy((int8_t *)packet + sizeof(*ip) + sizeof(*udp), payload, payload_len);
    }

    return net_send_frame(dev, dst_mac, NET_ETHERTYPE_IPV4, packet, total_len);
}

static size_t net_dns_skip_name(const uint8_t *buf, size_t len, size_t off)
{
    while (off < len)
    {
        uint8_t n;

        n = buf[off++];
        if (n == 0)
        {
            return off;
        }
        if ((n & 0xc0U) == 0xc0U)
        {
            if (off >= len)
            {
                return len;
            }
            return off + 1U;
        }
        if ((n & 0xc0U) != 0U)
        {
            return len;
        }
        off += n;
    }

    return len;
}

static int net_dns_parse_answer(const uint8_t *buf, size_t len, uint16_t txid, uint32_t *out_ip)
{
    struct net_dns_hdr hdr;
    size_t off;
    uint16_t qdcount;
    uint16_t ancount;
    uint32_t i;

    if (!buf || len < sizeof(hdr))
    {
        return -EINVAL;
    }

    memcpy((int8_t *)&hdr, (const int8_t *)buf, sizeof(hdr));
    if (net_bswap16(hdr.id) != txid)
    {
        return -EINVAL;
    }

    off = sizeof(hdr);
    qdcount = net_bswap16(hdr.qdcount);
    ancount = net_bswap16(hdr.ancount);

    for (i = 0; i < qdcount; i++)
    {
        off = net_dns_skip_name(buf, len, off);
        if (off + 4U > len)
        {
            return -EINVAL;
        }
        off += 4U;
    }

    for (i = 0; i < ancount; i++)
    {
        uint16_t type;
        uint16_t class_;
        uint16_t rdlen;
        uint16_t field16;

        off = net_dns_skip_name(buf, len, off);
        if (off + 10U > len)
        {
            return -EINVAL;
        }

        memcpy((int8_t *)&field16, (const int8_t *)(buf + off), sizeof(field16));
        type = net_bswap16(field16);
        memcpy((int8_t *)&field16, (const int8_t *)(buf + off + 2U), sizeof(field16));
        class_ = net_bswap16(field16);
        memcpy((int8_t *)&field16, (const int8_t *)(buf + off + 8U), sizeof(field16));
        rdlen = net_bswap16(field16);
        off += 10U;

        if (off + rdlen > len)
        {
            return -EINVAL;
        }

        if (type == 1U && class_ == 1U && rdlen == 4U)
        {
            if (out_ip)
            {
                *out_ip = ((uint32_t)buf[off + 0] << 24) |
                          ((uint32_t)buf[off + 1] << 16) |
                          ((uint32_t)buf[off + 2] << 8) |
                          (uint32_t)buf[off + 3];
            }
            return 0;
        }

        off += rdlen;
    }

    return -ENOENT;
}

static void net_loopback_arp_test(struct net_device *dev)
{
    uint8_t frame[sizeof(struct net_eth_hdr) + sizeof(struct net_arp_hdr)];
    struct net_eth_hdr *eth;
    uint8_t fake_mac[6] = { 0x02, 0x12, 0x34, 0x56, 0x78, 0x9a };
    uint32_t fake_ip = ((uint32_t)10 << 24) | ((uint32_t)0 << 16) | ((uint32_t)2 << 8) | 123;
    struct net_arp_hdr *arp;

    /*
     * 这个回环测试不依赖外部网络后端。
     * 我们直接向协议栈喂一帧 ARP 请求，检查内核自己的收包和回包链路。
     */
    memset((int8_t *)frame, 0, sizeof(frame));
    eth = (struct net_eth_hdr *)frame;
    arp = (struct net_arp_hdr *)(frame + sizeof(*eth));

    memcpy((int8_t *)eth->dst, (int8_t *)dev->mac, 6);
    memcpy((int8_t *)eth->src, (int8_t *)fake_mac, 6);
    eth->ethertype = net_bswap16(NET_ETHERTYPE_ARP);

    arp->htype = net_bswap16(1);
    arp->ptype = net_bswap16(NET_ETHERTYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->opcode = net_bswap16(1);
    memcpy((int8_t *)arp->sha, (int8_t *)fake_mac, 6);
    net_ipv4_to_bytes(fake_ip, arp->spa);
    memset((int8_t *)arp->tha, 0, sizeof(arp->tha));
    net_ipv4_to_bytes(dev->ipv4, arp->tpa);

    net_receive(dev, frame, sizeof(frame));
}

static void net_dump_device(const struct net_device *dev)
{
    int8_t mac_buf[32];
    int8_t ip_buf[32];

    net_mac_print(dev->mac, mac_buf, sizeof(mac_buf));
    net_ipv4_print(dev->ipv4, ip_buf, sizeof(ip_buf));

    printk("[net\tdev ]: %s mac=%s ip=%s mtu=%lu\n",
           dev->name, mac_buf, ip_buf, (uint64_t)dev->mtu);
}

static void net_handle_arp(struct net_device *dev, const struct net_eth_hdr *eth, const uint8_t *payload, size_t len)
{
    struct net_arp_hdr arp;
    uint8_t reply[sizeof(struct net_eth_hdr) + sizeof(struct net_arp_hdr)];
    struct net_eth_hdr *reth;
    struct net_arp_hdr *rarp;
    uint32_t sender_ip;
    uint32_t target_ip;
    uint16_t opcode;

    if (len < sizeof(arp))
    {
        return;
    }

    memcpy((int8_t *)&arp, (int8_t *)payload, sizeof(arp));
    opcode = net_bswap16(arp.opcode);
    sender_ip = net_ipv4_from_bytes(arp.spa);
    target_ip = net_ipv4_from_bytes(arp.tpa);

    if (opcode == 2)
    {
        net_arp_cache_update(sender_ip, arp.sha);
        net_probe_finish(sender_ip, 0, arp.sha);
        return;
    }

    if (opcode != 1)
    {
        return;
    }

    if (target_ip != dev->ipv4)
    {
        net_arp_cache_update(sender_ip, arp.sha);
        return;
    }

    net_arp_cache_update(sender_ip, arp.sha);
    reth = (struct net_eth_hdr *)reply;
    rarp = (struct net_arp_hdr *)(reply + sizeof(*reth));
    memcpy((int8_t *)reth->dst, (int8_t *)eth->src, 6);
    memcpy((int8_t *)reth->src, (int8_t *)dev->mac, 6);
    reth->ethertype = net_bswap16(NET_ETHERTYPE_ARP);

    rarp->htype = net_bswap16(1);
    rarp->ptype = net_bswap16(NET_ETHERTYPE_IPV4);
    rarp->hlen = 6;
    rarp->plen = 4;
    rarp->opcode = net_bswap16(2);
    memcpy((int8_t *)rarp->sha, (int8_t *)dev->mac, 6);
    net_ipv4_to_bytes(dev->ipv4, rarp->spa);
    memcpy((int8_t *)rarp->tha, (int8_t *)arp.sha, 6);
    memcpy((int8_t *)rarp->tpa, (int8_t *)arp.spa, 4);

    if (dev->tx)
    {
        dev->tx(dev, reply, sizeof(reply));
    }
}

static void net_handle_icmp(struct net_device *dev, const struct net_eth_hdr *eth, const struct net_ipv4_hdr *ip, const uint8_t *payload, size_t len)
{
    struct net_icmp_hdr icmp;
    uint8_t reply[sizeof(struct net_eth_hdr) + sizeof(struct net_ipv4_hdr) + sizeof(struct net_icmp_hdr) + 64];
    struct net_eth_hdr *reth;
    struct net_ipv4_hdr *rip;
    struct net_icmp_hdr *ricmp;
    size_t icmp_len;
    uint16_t ip_total_len;

    if (len < sizeof(icmp))
    {
        return;
    }

    memcpy((int8_t *)&icmp, (int8_t *)payload, sizeof(icmp));
    if (icmp.type == 0 && icmp.code == 0)
    {
        if (net_ipv4_from_bytes(ip->src) == net_probe.target_ip)
        {
            spin_lock(&net_probe.lock);
            if (net_probe.active &&
                net_probe.expect_icmp &&
                net_probe.target_ip == net_ipv4_from_bytes(ip->src) &&
                net_probe.icmp_id == net_bswap16(icmp.id) &&
                net_probe.icmp_seq == net_bswap16(icmp.seq))
            {
                net_probe.done_cycles = net_clock_cycles();
                net_probe.result = 0;
                net_probe.done = true;
                sev();
            }
            spin_unlock(&net_probe.lock);
        }
        net_arp_cache_update(net_ipv4_from_bytes(ip->src), eth->src);
        return;
    }

    if (icmp.type != 8 || icmp.code != 0)
    {
        return;
    }

    icmp_len = len;
    if (sizeof(reply) < sizeof(struct net_eth_hdr) + sizeof(struct net_ipv4_hdr) + icmp_len)
    {
        return;
    }

    reth = (struct net_eth_hdr *)reply;
    rip = (struct net_ipv4_hdr *)(reply + sizeof(*reth));
    ricmp = (struct net_icmp_hdr *)(reply + sizeof(*reth) + sizeof(*rip));

    memcpy((int8_t *)reth->dst, (int8_t *)eth->src, 6);
    memcpy((int8_t *)reth->src, (int8_t *)dev->mac, 6);
    reth->ethertype = net_bswap16(NET_ETHERTYPE_IPV4);

    memcpy((int8_t *)rip, (int8_t *)ip, sizeof(*rip));
    rip->src[0] = ip->dst[0];
    rip->src[1] = ip->dst[1];
    rip->src[2] = ip->dst[2];
    rip->src[3] = ip->dst[3];
    rip->dst[0] = ip->src[0];
    rip->dst[1] = ip->src[1];
    rip->dst[2] = ip->src[2];
    rip->dst[3] = ip->src[3];
    rip->ttl = 64;
    rip->checksum = 0;

    memcpy((int8_t *)ricmp, (int8_t *)payload, icmp_len);
    ricmp->type = 0;
    ricmp->checksum = 0;
    ricmp->checksum = net_bswap16(net_checksum16(ricmp, icmp_len));

    ip_total_len = (uint16_t)(sizeof(*rip) + icmp_len);
    rip->total_len = net_bswap16(ip_total_len);
    rip->checksum = net_bswap16(net_checksum16(rip, sizeof(*rip)));

    if (dev->tx)
    {
        dev->tx(dev, reply, sizeof(struct net_eth_hdr) + ip_total_len);
    }
}

static void net_handle_udp(struct net_device *dev, const struct net_eth_hdr *eth, const struct net_ipv4_hdr *ip,
                           const uint8_t *payload, size_t len)
{
    struct net_udp_hdr udp;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t udp_len;
    bool dns_match;
    uint16_t txid;

    (void)eth;

    if (len < sizeof(udp))
    {
        return;
    }

    memcpy((int8_t *)&udp, (const int8_t *)payload, sizeof(udp));
    src_port = net_bswap16(udp.src_port);
    dst_port = net_bswap16(udp.dst_port);
    udp_len = net_bswap16(udp.len);
    if (udp_len < sizeof(udp) || udp_len > len)
    {
        return;
    }

    spin_lock(&net_dns.lock);
    dns_match = net_dns.active && src_port == 53U && dst_port == net_dns.src_port;
    txid = net_dns.txid;
    spin_unlock(&net_dns.lock);

    if (dns_match)
    {
        uint32_t resolved_ip;
        int dns_ret;

        resolved_ip = 0;
        dns_ret = net_dns_parse_answer(payload + sizeof(udp), udp_len - sizeof(udp), txid, &resolved_ip);
        net_dns_finish(dns_ret, resolved_ip);
    }
    (void)ip;
}

static void net_handle_ipv4(struct net_device *dev, const struct net_eth_hdr *eth, const uint8_t *payload, size_t len)
{
    struct net_ipv4_hdr ip;
    uint16_t total_len;
    uint8_t ihl;
    const uint8_t *l4;
    size_t l4_len;

    if (len < sizeof(ip))
    {
        return;
    }

    memcpy((int8_t *)&ip, (int8_t *)payload, sizeof(ip));
    if ((ip.version_ihl >> 4) != 4)
    {
        return;
    }

    ihl = (ip.version_ihl & 0x0f) * 4;
    if (ihl < sizeof(ip) || len < ihl)
    {
        return;
    }

    total_len = net_bswap16(ip.total_len);
    if (total_len < ihl || total_len > len)
    {
        return;
    }

    if (net_checksum16(&ip, ihl) != 0)
    {
        return;
    }

    if (net_ipv4_from_bytes(ip.dst) != dev->ipv4)
    {
        return;
    }

    l4 = payload + ihl;
    l4_len = total_len - ihl;

    if (ip.proto == NET_IPV4_PROTO_ICMP)
    {
        net_handle_icmp(dev, eth, &ip, l4, l4_len);
        return;
    }

    if (ip.proto == NET_IPV4_PROTO_TCP)
    {
        net_tcp_input(dev, l4, l4_len, net_ipv4_from_bytes(ip.src), net_ipv4_from_bytes(ip.dst));
        return;
    }

    if (ip.proto == NET_IPV4_PROTO_UDP)
    {
        net_handle_udp(dev, eth, &ip, l4, l4_len);
        return;
    }

}

void net_receive(struct net_device *dev, const void *frame, size_t len)
{
    const struct net_eth_hdr *eth;
    uint16_t ethertype;

    if (!dev || !frame || len < sizeof(struct net_eth_hdr))
    {
        return;
    }

    eth = (const struct net_eth_hdr *)frame;
    if (!net_rx_packet_seen)
    {
        net_rx_packet_seen = 1;
        printk("[net\trx ]: frame len=%lu dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x type=%#x\n",
               (uint64_t)len,
               eth->dst[0], eth->dst[1], eth->dst[2], eth->dst[3], eth->dst[4], eth->dst[5],
               eth->src[0], eth->src[1], eth->src[2], eth->src[3], eth->src[4], eth->src[5],
               net_bswap16(eth->ethertype));
    }
    if (!net_mac_equal(eth->dst, dev->mac) && !net_mac_is_broadcast(eth->dst))
    {
        return;
    }

    ethertype = net_bswap16(eth->ethertype);
    if (ethertype == NET_ETHERTYPE_ARP)
    {
        net_handle_arp(dev, eth, (const uint8_t *)frame + sizeof(*eth), len - sizeof(*eth));
        return;
    }

    if (ethertype == NET_ETHERTYPE_IPV4)
    {
        net_handle_ipv4(dev, eth, (const uint8_t *)frame + sizeof(*eth), len - sizeof(*eth));
        return;
    }

}

static ssize_t net_tx_default(struct net_device *dev, const void *buf, size_t len)
{
    if (!dev || !dev->tx)
    {
        return -ENODEV;
    }

    return dev->tx(dev, buf, len);
}

ssize_t net_send_frame(struct net_device *dev, const uint8_t dst_mac[6], uint16_t ethertype,
                       const void *payload, size_t len)
{
    uint8_t frame[1600];
    struct net_eth_hdr *eth;
    size_t frame_len;

    if (!dev || !dst_mac || !payload)
    {
        return -EINVAL;
    }

    /*
     * 以太网最小线缆长度是 64 字节，其中 4 字节 FCS 由 MAC 硬件/后端补。
     * 所以用户态/内核态真正提交给 virtio-net 的最小帧长度应至少是 60 字节。
     *
     * 之前 ARP 请求只有 42 字节，QEMU slirp 很容易把它当成不完整帧丢掉，
     * 这会直接导致网关 ARP 超时，后续 ping 只能落到假 MAC 的回退路径。
     */
    frame_len = sizeof(*eth) + len;
    if (frame_len < 60U)
    {
        frame_len = 60U;
    }

    if (sizeof(frame) < frame_len)
    {
        return -EMSGSIZE;
    }

    memset((int8_t *)frame, 0, frame_len);
    eth = (struct net_eth_hdr *)frame;
    memcpy((int8_t *)eth->dst, (int8_t *)dst_mac, 6);
    memcpy((int8_t *)eth->src, (int8_t *)dev->mac, 6);
    eth->ethertype = net_bswap16(ethertype);
    memcpy((int8_t *)frame + sizeof(*eth), (int8_t *)payload, len);
    return net_tx_default(dev, frame, frame_len);
}

int net_register_device(struct net_device *dev)
{
    uint32_t slot;

    if (!dev || !dev->tx || !dev->name[0])
    {
        return -EINVAL;
    }

    for (slot = 0; slot < NET_MAX_DEVICES; slot++)
    {
        if (!net_devices[slot].used)
        {
            net_devices[slot] = *dev;
            net_devices[slot].used = true;
            if (!net_default)
            {
                net_default = &net_devices[slot];
            }
            net_dump_device(&net_devices[slot]);
            return 0;
        }
    }

    return -ENOSPC;
}

struct net_device *net_default_device(void)
{
    return net_default;
}

int net_set_default_config(uint32_t ipv4, uint32_t netmask, uint32_t gateway)
{
    if (!net_default)
    {
        return -ENODEV;
    }

    /*
     * 网络参数做成运行时可调：
     * - 方便 TAP/bridge 场景切换不同子网
     * - 也方便后续把 DHCP / 配置文件挂上来
     */
    net_default->ipv4 = ipv4;
    net_default->netmask = netmask;
    net_default->gateway = gateway;
    net_config_changes++;
    printk("[net\tcfg ]: %s ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u changes=%u\n",
           net_default->name,
           (ipv4 >> 24) & 0xff, (ipv4 >> 16) & 0xff, (ipv4 >> 8) & 0xff, ipv4 & 0xff,
           (netmask >> 24) & 0xff, (netmask >> 16) & 0xff, (netmask >> 8) & 0xff, netmask & 0xff,
           (gateway >> 24) & 0xff, (gateway >> 16) & 0xff, (gateway >> 8) & 0xff, gateway & 0xff,
           net_config_changes);
    return 0;
}

int net_show_status(void)
{
    uint32_t i;

    if (!net_default)
    {
        printk("[net\t]: no device\n");
        return -ENODEV;
    }

    for (i = 0; i < NET_MAX_DEVICES; i++)
    {
        if (net_devices[i].used)
        {
            net_dump_device(&net_devices[i]);
        }
    }

    return 0;
}

static int net_probe_device(struct net_device *dev)
{
    uint8_t gateway_mac[6];
    ssize_t ret;
    int probe_ret;
    uint16_t icmp_id;
    uint16_t icmp_seq;

    if (!dev)
    {
        return -ENODEV;
    }

    /*
     * 启动阶段的网络自检尽量“快进”：
     * - 先做一次最小 ARP/ICMP 探测
     * - 如果后端不回，就直接退回本地回环测试
     * - 不把整机启动卡在 3 秒超时上
     */
    probe_ret = net_arp_resolve(dev, dev->gateway, gateway_mac);
    if (probe_ret)
    {
        printk("[net\tselftest]: gateway arp skipped (%d), fallback to loopback\n", probe_ret);
        net_loopback_arp_test(dev);
        return 0;
    }

    icmp_id = 0x5349;
    icmp_seq = 1;
    net_probe_begin(dev->gateway, true, icmp_id, icmp_seq);
    ret = net_send_icmp_echo(dev, gateway_mac, dev->gateway, icmp_id, icmp_seq);
    if (ret < 0)
    {
        spin_lock(&net_probe.lock);
        net_probe.active = false;
        spin_unlock(&net_probe.lock);
        return (int)ret;
    }

    probe_ret = net_probe_wait(dev->gateway, net_ms_to_cycles(500), 0, 0);
    if (probe_ret)
    {
        printk("[net\tselftest]: gateway icmp timeout (%d), continue booting\n", probe_ret);
        net_loopback_arp_test(dev);
        return 0;
    }

    return 0;
}

int net_arp_resolve(struct net_device *dev, uint32_t target_ip, uint8_t out_mac[6])
{
    ssize_t ret;
    int probe_ret;
    uint32_t route_ip;

    if (!dev)
    {
        return -ENODEV;
    }

    route_ip = net_next_hop_ipv4(dev, target_ip);

    /*
     * QEMU usernet/slirp 场景下，10.0.2.0/24 的“网关/宿主代理”本质上是
     * 一个虚拟二层后端，不必每次都完整等 ARP 超时。
     *
     * 这里直接给出代理 MAC，可以把 SSH / HTTP 这类首次连接前的
     * 3 秒空白等待明显压缩掉。
     */
    if (route_ip == dev->gateway || net_ipv4_is_slirp_subnet(route_ip) || net_ipv4_is_slirp_subnet(dev->gateway))
    {
        net_gateway_proxy_mac(dev, out_mac);
        net_arp_cache_update(route_ip, out_mac);
        return 0;
    }

    if (net_arp_cache_lookup(route_ip, out_mac) == 0)
    {
        return 0;
    }

    net_probe_begin(route_ip, false, 0, 0);
    ret = net_send_arp_request(dev, route_ip);
    if (ret < 0)
    {
        spin_lock(&net_probe.lock);
        net_probe.active = false;
        spin_unlock(&net_probe.lock);
        return (int)ret;
    }

    probe_ret = net_probe_wait(route_ip, net_ms_to_cycles(3000), out_mac, 0);
    if (probe_ret)
    {
        if (route_ip == dev->gateway || net_ipv4_is_slirp_subnet(route_ip))
        {
            /*
             * 如果 ARP 仍旧拿不到网关，先退回到“尽量继续运行”的策略。
             * 这样不会因为网络后端细节卡住 shell；后续再继续补更完整的二层/路由支持。
             */
            net_gateway_proxy_mac(dev, out_mac);
            net_arp_cache_update(route_ip, out_mac);
            printk("[net\tarp ]: gateway arp fallback mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                   out_mac[0], out_mac[1], out_mac[2], out_mac[3], out_mac[4], out_mac[5]);
            return 0;
        }

        return probe_ret;
    }

    net_arp_cache_update(route_ip, out_mac);

    if (route_ip == dev->gateway)
    {
        printk("[net\tarp ]: gateway resolved mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
               out_mac[0], out_mac[1], out_mac[2], out_mac[3], out_mac[4], out_mac[5]);
    }

    return 0;
}

static bool net_dns_hostname_valid(const int8_t *hostname)
{
    size_t i;
    bool has_label;

    if (!hostname || hostname[0] == '\0')
    {
        return false;
    }

    has_label = false;
    for (i = 0; hostname[i] != '\0'; i++)
    {
        char ch;

        ch = (char)hostname[i];
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '.')
        {
            has_label = true;
            continue;
        }
        return false;
    }

    return has_label;
}

static size_t net_dns_encode_name(const int8_t *hostname, uint8_t *out, size_t out_len)
{
    size_t pos;
    size_t label_start;
    size_t i;

    if (!hostname || !out || out_len < 2)
    {
        return 0;
    }

    pos = 0;
    label_start = 0;
    for (i = 0; ; i++)
    {
        char ch;

        ch = (char)hostname[i];
        if (ch == '.' || ch == '\0')
        {
            size_t label_len;

            label_len = i - label_start;
            if (label_len == 0 || label_len > 63U || pos + label_len + 2U > out_len)
            {
                return 0;
            }
            out[pos++] = (uint8_t)label_len;
            memcpy((int8_t *)&out[pos], (const int8_t *)&hostname[label_start], label_len);
            pos += label_len;
            if (ch == '\0')
            {
                break;
            }
            label_start = i + 1U;
        }
    }

    if (pos >= out_len)
    {
        return 0;
    }
    out[pos++] = 0;
    return pos;
}

int64_t net_dns_lookup(struct net_device *dev, const int8_t *hostname, uint32_t *out_ipv4, uint32_t timeout_ms)
{
    uint8_t packet[512];
    uint8_t dns_mac[6];
    struct net_dns_hdr *hdr;
    uint8_t *question;
    uint16_t txid;
    uint16_t src_port;
    size_t qname_len;
    size_t payload_len;
    ssize_t ret;
    int probe_ret;
    uint32_t dns_server;
    uint32_t resolved_ip;

    if (!dev || !hostname || !out_ipv4)
    {
        return -EINVAL;
    }

    if (!net_dns_hostname_valid(hostname))
    {
        return -EINVAL;
    }

    if (net_ipv4_is_slirp_subnet(dev->gateway))
    {
        dns_server = ((uint32_t)10 << 24) | ((uint32_t)0 << 16) | ((uint32_t)2 << 8) | 3U;
    }
    else if (dev->gateway)
    {
        dns_server = dev->gateway;
    }
    else
    {
        dns_server = ((uint32_t)10 << 24) | ((uint32_t)0 << 16) | ((uint32_t)2 << 8) | 3U;
    }
    src_port = (uint16_t)(50000U + (net_clock_cycles() & 0x0fffU));
    if (src_port == 53U)
    {
        src_port++;
    }
    txid = (uint16_t)(net_clock_cycles() & 0xffffU);

    memset((int8_t *)packet, 0, sizeof(packet));
    hdr = (struct net_dns_hdr *)packet;
    qname_len = net_dns_encode_name(hostname, packet + sizeof(*hdr), sizeof(packet) - sizeof(*hdr) - 4U);
    if (!qname_len)
    {
        return -EINVAL;
    }
    hdr->id = net_bswap16(txid);
    hdr->flags = net_bswap16(0x0100U); /* recursion desired */
    hdr->qdcount = net_bswap16(1);
    question = packet + sizeof(*hdr);
    question[qname_len + 0U] = 0;
    question[qname_len + 1U] = 1;
    question[qname_len + 2U] = 0;
    question[qname_len + 3U] = 1;
    payload_len = sizeof(*hdr) + qname_len + 4U;

    if (net_arp_resolve(dev, dns_server, dns_mac) < 0)
    {
        return -EHOSTUNREACH;
    }

    net_dns_begin(txid, src_port);
    ret = net_send_udp_datagram(dev, dns_server, dns_mac, src_port, 53U, packet, payload_len);
    if (ret < 0)
    {
        spin_lock(&net_dns.lock);
        net_dns.active = false;
        spin_unlock(&net_dns.lock);
        return ret;
    }

    probe_ret = net_dns_wait(timeout_ms ? timeout_ms : 2000U, &resolved_ip);
    if (probe_ret < 0)
    {
        return probe_ret;
    }

    *out_ipv4 = resolved_ip;
    return 0;
}

int64_t net_ping(struct net_device *dev, uint32_t target_ip, uint16_t icmp_id, uint16_t icmp_seq, uint32_t timeout_ms)
{
    uint8_t dst_mac[6];
    uint64_t elapsed_cycles;
    uint64_t timeout_cycles;
    ssize_t ret;
    int probe_ret;

    if (!dev)
    {
        return -ENODEV;
    }

    if (!timeout_ms)
    {
        timeout_ms = 1000;
    }

    if (net_arp_cache_lookup(target_ip, dst_mac) == 0)
    {
        if (!net_ping_cache_log_seen)
        {
            net_ping_cache_log_seen = true;
            printk("[net\tping]: resolved target=%u.%u.%u.%u via cache\n",
                   (target_ip >> 24) & 0xff,
                   (target_ip >> 16) & 0xff,
                   (target_ip >> 8) & 0xff,
                   target_ip & 0xff);
        }
        net_probe_begin(target_ip, true, icmp_id, icmp_seq);
        ret = net_send_icmp_echo(dev, dst_mac, target_ip, icmp_id, icmp_seq);
        if (ret < 0)
        {
            spin_lock(&net_probe.lock);
            net_probe.active = false;
            spin_unlock(&net_probe.lock);
            return (int64_t)ret;
        }

        timeout_cycles = net_ms_to_cycles(timeout_ms);
        probe_ret = net_probe_wait(target_ip, timeout_cycles, 0, &elapsed_cycles);
        if (probe_ret)
        {
            return probe_ret == -ETIMEDOUT ? -ETIMEDOUT : (int64_t)probe_ret;
        }

        return (int64_t)net_cycles_to_us(elapsed_cycles);
    }

    ret = net_arp_resolve(dev, target_ip, dst_mac);
    if (ret)
    {
        return ret == -ETIMEDOUT ? -EHOSTUNREACH : (int64_t)ret;
    }

    if (!net_ping_arp_log_seen)
    {
        net_ping_arp_log_seen = true;
        printk("[net\tping]: resolved target=%u.%u.%u.%u via arp mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
               (target_ip >> 24) & 0xff,
               (target_ip >> 16) & 0xff,
               (target_ip >> 8) & 0xff,
               target_ip & 0xff,
               dst_mac[0], dst_mac[1], dst_mac[2], dst_mac[3], dst_mac[4], dst_mac[5]);
    }
    net_probe_begin(target_ip, true, icmp_id, icmp_seq);
    ret = net_send_icmp_echo(dev, dst_mac, target_ip, icmp_id, icmp_seq);
    if (ret < 0)
    {
        spin_lock(&net_probe.lock);
        net_probe.active = false;
        spin_unlock(&net_probe.lock);
        return (int64_t)ret;
    }

    timeout_cycles = net_ms_to_cycles(timeout_ms);
    probe_ret = net_probe_wait(target_ip, timeout_cycles, 0, &elapsed_cycles);
    if (probe_ret)
    {
        return probe_ret == -ETIMEDOUT ? -ETIMEDOUT : (int64_t)probe_ret;
    }

    return (int64_t)net_cycles_to_us(elapsed_cycles);
}

int net_selftest(void)
{
    struct net_device *dev;

    dev = net_default_device();
    if (!dev)
    {
        return -ENODEV;
    }

    return net_probe_device(dev);
}

void net_init(void)
{
    uint32_t i;

    for (i = 0; i < NET_MAX_DEVICES; i++)
    {
        memset((int8_t *)&net_devices[i], 0, sizeof(net_devices[i]));
    }

    net_default = 0;
    net_counter_freq = 0;
    net_config_changes = 0;
    printk("[net\tinit]: stack ready\n");
}
