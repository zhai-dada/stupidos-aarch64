#ifndef __NET_H__
#define __NET_H__

#include "asm/types.h"

#define NET_MAX_DEVICES     4
#define NET_IFNAME_LEN      16

#define NET_ETHERTYPE_ARP   0x0806
#define NET_ETHERTYPE_IPV4  0x0800

#define NET_IPV4_PROTO_ICMP 1
#define NET_IPV4_PROTO_UDP  17
#define NET_IPV4_PROTO_TCP  6

struct net_device;
struct net_socket
{
    void *priv;
};

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
int net_set_default_config(uint32_t ipv4, uint32_t netmask, uint32_t gateway);
int net_show_status(void);
int net_selftest(void);
int net_arp_resolve(struct net_device *dev, uint32_t target_ip, uint8_t out_mac[6]);
int64_t net_ping(struct net_device *dev, uint32_t target_ip, uint16_t icmp_id, uint16_t icmp_seq, uint32_t timeout_ms);
int64_t net_dns_lookup(struct net_device *dev, const int8_t *hostname, uint32_t *out_ipv4, uint32_t timeout_ms);
void net_receive(struct net_device *dev, const void *frame, size_t len);
ssize_t net_send_frame(struct net_device *dev, const uint8_t dst_mac[6], uint16_t ethertype,
                       const void *payload, size_t len);
void net_tcp_input(struct net_device *dev, const void *eth_payload, size_t len, uint32_t src_ip, uint32_t dst_ip);
int64_t net_http_get(struct net_device *dev, uint32_t target_ip, uint16_t port, const int8_t *path,
                     int out_fd, uint32_t timeout_ms);
int net_socket_init(struct net_socket *sock);
int net_socket_connect(struct net_socket *sock, struct net_device *dev, uint32_t target_ip, uint16_t port, uint32_t timeout_ms);
int net_socket_connect_begin(struct net_socket *sock, struct net_device *dev, uint32_t target_ip, uint16_t port);
ssize_t net_socket_read(struct net_socket *sock, void *buf, size_t len, uint32_t timeout_ms);
ssize_t net_socket_write(struct net_socket *sock, const void *buf, size_t len, uint32_t timeout_ms);
int net_socket_shutdown(struct net_socket *sock, int how);
int net_socket_pending(struct net_socket *sock);
int net_socket_so_error(struct net_socket *sock);
void net_socket_close(struct net_socket *sock);

#endif
