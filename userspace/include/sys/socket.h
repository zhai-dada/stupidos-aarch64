#ifndef __STUPIDOS_SYS_SOCKET_H__
#define __STUPIDOS_SYS_SOCKET_H__

#include <stddef.h>
#include <stdint.h>
#include "stupidos_user.h"

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

#ifndef AF_UNSPEC
#define AF_UNSPEC   0
#endif
#ifndef AF_UNIX
#define AF_UNIX     1
#endif
#ifndef AF_INET
#define AF_INET     2
#endif
#ifndef AF_INET6
#define AF_INET6    10
#endif
#ifndef PF_UNSPEC
#define PF_UNSPEC   AF_UNSPEC
#endif
#ifndef PF_UNIX
#define PF_UNIX     AF_UNIX
#endif
#ifndef PF_INET
#define PF_INET     AF_INET
#endif
#ifndef PF_INET6
#define PF_INET6    AF_INET6
#endif

#ifndef SOCK_STREAM
#define SOCK_STREAM  1
#endif
#ifndef SOCK_DGRAM
#define SOCK_DGRAM   2
#endif
#ifndef SOCK_RAW
#define SOCK_RAW     3
#endif

#ifndef SOL_SOCKET
#define SOL_SOCKET   1
#endif
#ifndef SOL_TCP
#define SOL_TCP      6
#endif
#ifndef SO_ERROR
#define SO_ERROR     4
#endif
#ifndef SO_REUSEADDR
#define SO_REUSEADDR 2
#endif
#ifndef SO_KEEPALIVE
#define SO_KEEPALIVE 9
#endif
#ifndef SO_RCVBUF
#define SO_RCVBUF    8
#endif
#ifndef SO_SNDBUF
#define SO_SNDBUF    7
#endif
#ifndef SO_PRIORITY
#define SO_PRIORITY  12
#endif
#ifndef SO_BINDTODEVICE
#define SO_BINDTODEVICE 25
#endif
#ifndef TCP_NODELAY
#define TCP_NODELAY  1
#endif
#ifndef TCP_FASTOPEN
#define TCP_FASTOPEN  23
#endif

#ifndef IPPROTO_IP
#define IPPROTO_IP    0
#endif
#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6   41
#endif

#ifndef SHUT_RD
#define SHUT_RD      0
#endif
#ifndef SHUT_WR
#define SHUT_WR      1
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR    2
#endif

struct sockaddr
{
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage
{
    sa_family_t ss_family;
    uint8_t __ss_pad[126];
};

struct linger
{
    int l_onoff;
    int l_linger;
};

int socket(int domain, int type, int protocol);
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int shutdown(int fd, int how);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t addrlen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addrlen);
int bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);
int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);

#endif
