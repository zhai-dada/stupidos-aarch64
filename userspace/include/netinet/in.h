#ifndef __STUPIDOS_NETINET_IN_H__
#define __STUPIDOS_NETINET_IN_H__

#include <stdint.h>
#include "sys/socket.h"

struct in_addr
{
    uint32_t s_addr;
};

struct in6_addr
{
    uint8_t s6_addr[16];
};

struct sockaddr_in
{
    sa_family_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

struct sockaddr_in6
{
    sa_family_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

#ifndef INADDR_ANY
#define INADDR_ANY       0x00000000U
#endif
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK  0x7f000001U
#endif

uint16_t htons(uint16_t hostshort);
uint16_t ntohs(uint16_t netshort);
uint32_t htonl(uint32_t hostlong);
uint32_t ntohl(uint32_t netlong);

#ifndef IPPROTO_TCP
#define IPPROTO_TCP      6
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP      17
#endif
#ifndef IPPROTO_ICMPV6
#define IPPROTO_ICMPV6   58
#endif

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif

#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY      26
#endif
#ifndef IPV6_TCLASS
#define IPV6_TCLASS      67
#endif

#endif
