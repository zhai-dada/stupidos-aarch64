#include "net/net.h"

#include "errno.h"
#include "driver/virtio_net.h"
#include "lib/libmem.h"
#include "sched.h"
#include "printk.h"
#include "spinlock.h"

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
};

static struct net_device net_devices[NET_MAX_DEVICES];
static struct net_device *net_default;
static struct net_probe_state net_probe = {
    .lock = SPINLOCK_INIT,
};

int net_arp_resolve(struct net_device *dev, uint32_t target_ip, uint8_t out_mac[6]);

static uint16_t net_bswap16(uint16_t v)
{
    return __builtin_bswap16(v);
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
        ret = 1;
    }
    spin_unlock(&net_probe.lock);
    return ret;
}

static int net_probe_wait(uint32_t target_ip, uint32_t tries, uint8_t mac[6])
{
    uint32_t left;
    int result;
    bool done;

    left = tries;
    while (left--)
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
            net_probe.active = false;
            spin_unlock(&net_probe.lock);
            return result;
        }
        spin_unlock(&net_probe.lock);
        /*
         * 自检阶段采用主动轮询，避免等待路径依赖调度器时延。
         * 这样即使 RX 侧没有立刻收到回包，也能快速给出超时结果。
         */
        asm volatile("nop");
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
    uint8_t packet[sizeof(struct net_ipv4_hdr) + sizeof(struct net_icmp_hdr) + 32];
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
    payload_len = 32;

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
    ip->checksum = net_checksum16(ip, sizeof(*ip));

    icmp->type = 8;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = net_bswap16(icmp_id);
    icmp->seq = net_bswap16(icmp_seq);

    for (i = 0; i < payload_len; i++)
    {
        data[i] = (uint8_t)(0x30 + (i & 0x0f));
    }

    icmp->checksum = net_checksum16(icmp, sizeof(*icmp) + payload_len);
    return net_send_frame(dev, dst_mac, NET_ETHERTYPE_IPV4, packet, total_len);
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
    printk("[net\tselftest]: local loopback test start\n");

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
    printk("[net\tselftest]: local loopback test done\n");
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
        printk("[net\trx ]: arp reply from %u.%u.%u.%u\n",
               (sender_ip >> 24) & 0xff,
               (sender_ip >> 16) & 0xff,
               (sender_ip >> 8) & 0xff,
               sender_ip & 0xff);
        net_probe_finish(sender_ip, 0, arp.sha);
        return;
    }

    if (opcode != 1)
    {
        return;
    }

    if (target_ip != dev->ipv4)
    {
        return;
    }

    printk("[net\trx ]: arp request for me from %u.%u.%u.%u\n",
           (sender_ip >> 24) & 0xff,
           (sender_ip >> 16) & 0xff,
           (sender_ip >> 8) & 0xff,
           sender_ip & 0xff);

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
        printk("[net\trx ]: arp reply sent\n");
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
                net_probe.result = 0;
                net_probe.done = true;
            }
            spin_unlock(&net_probe.lock);
        }
        return;
    }

    if (icmp.type != 8 || icmp.code != 0)
    {
        return;
    }

    printk("[net\trx ]: icmp echo request from %u.%u.%u.%u\n",
           (ip->src[0]),
           (ip->src[1]),
           (ip->src[2]),
           (ip->src[3]));

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
    ricmp->checksum = net_checksum16(ricmp, icmp_len);

    ip_total_len = (uint16_t)(sizeof(*rip) + icmp_len);
    rip->total_len = net_bswap16(ip_total_len);
    rip->checksum = net_checksum16(rip, sizeof(*rip));

    if (dev->tx)
    {
        dev->tx(dev, reply, sizeof(struct net_eth_hdr) + ip_total_len);
        printk("[net\trx ]: icmp echo reply sent\n");
    }
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

    printk("[net\trx ]: ipv4 proto=%u len=%u\n", ip.proto, total_len);
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

    printk("[net\trx ]: ethertype=%#x len=%lu\n", ethertype, (uint64_t)len);
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

    if (!dev || !dst_mac || !payload)
    {
        return -EINVAL;
    }

    if (sizeof(frame) < sizeof(*eth) + len)
    {
        return -EMSGSIZE;
    }

    eth = (struct net_eth_hdr *)frame;
    memcpy((int8_t *)eth->dst, (int8_t *)dst_mac, 6);
    memcpy((int8_t *)eth->src, (int8_t *)dev->mac, 6);
    eth->ethertype = net_bswap16(ethertype);
    memcpy((int8_t *)frame + sizeof(*eth), (int8_t *)payload, len);
    return net_tx_default(dev, frame, sizeof(*eth) + len);
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

    printk("[net\tselftest]: probing %s gateway %u.%u.%u.%u\n",
           dev->name,
           (dev->gateway >> 24) & 0xff,
           (dev->gateway >> 16) & 0xff,
           (dev->gateway >> 8) & 0xff,
           dev->gateway & 0xff);

    probe_ret = net_arp_resolve(dev, dev->gateway, gateway_mac);
    if (probe_ret)
    {
        printk("[net\tselftest]: arp failed %d\n", probe_ret);
        net_loopback_arp_test(dev);
        return 0;
    }

    printk("[net\tselftest]: arp ok mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           gateway_mac[0], gateway_mac[1], gateway_mac[2],
           gateway_mac[3], gateway_mac[4], gateway_mac[5]);

    icmp_id = 0x5349;
    icmp_seq = 1;
    net_probe_begin(dev->gateway, true, icmp_id, icmp_seq);
    ret = net_send_icmp_echo(dev, gateway_mac, dev->gateway, icmp_id, icmp_seq);
    if (ret < 0)
    {
        spin_lock(&net_probe.lock);
        net_probe.active = false;
        spin_unlock(&net_probe.lock);
        printk("[net\tselftest]: icmp send failed %ld\n", ret);
        return (int)ret;
    }

    probe_ret = net_probe_wait(dev->gateway, 200, 0);
    if (probe_ret)
    {
        printk("[net\tselftest]: icmp timeout %d\n", probe_ret);
        return probe_ret;
    }

    printk("[net\tselftest]: icmp echo reply ok\n");
    return 0;
}

int net_arp_resolve(struct net_device *dev, uint32_t target_ip, uint8_t out_mac[6])
{
    ssize_t ret;
    int probe_ret;

    if (!dev)
    {
        return -ENODEV;
    }

    net_probe_begin(target_ip, false, 0, 0);
    ret = net_send_arp_request(dev, target_ip);
    if (ret < 0)
    {
        spin_lock(&net_probe.lock);
        net_probe.active = false;
        spin_unlock(&net_probe.lock);
        return (int)ret;
    }

    probe_ret = net_probe_wait(target_ip, 200, out_mac);
    if (probe_ret)
    {
        return probe_ret;
    }

    return 0;
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
    printk("[net\tinit]: stack ready\n");
}
