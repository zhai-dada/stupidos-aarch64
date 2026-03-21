#ifndef __NET_H__
#define __NET_H__

#include "asm/types.h"

#define NET_MAX_DEVICES     4
#define NET_IFNAME_LEN      16

#define NET_ETHERTYPE_ARP   0x0806
#define NET_ETHERTYPE_IPV4  0x0800

#define NET_IPV4_PROTO_ICMP 1

struct net_device;

typedef ssize_t (*net_tx_fn_t)(struct net_device *dev, const void *buf, size_t len);

struct net_device
{
    bool used;
    int8_t name[NET_IFNAME_LEN];
    uint8_t mac[6];
    uint32_t ipv4;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t mtu;
    void *priv;
    net_tx_fn_t tx;
};

void net_init(void);
int net_register_device(struct net_device *dev);
struct net_device *net_default_device(void);
int net_show_status(void);
int net_selftest(void);
int net_arp_resolve(struct net_device *dev, uint32_t target_ip, uint8_t out_mac[6]);
void net_receive(struct net_device *dev, const void *frame, size_t len);
ssize_t net_send_frame(struct net_device *dev, const uint8_t dst_mac[6], uint16_t ethertype,
                       const void *payload, size_t len);

#endif
