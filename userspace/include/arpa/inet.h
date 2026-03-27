#ifndef __STUPIDOS_ARPA_INET_H__
#define __STUPIDOS_ARPA_INET_H__

#include <stddef.h>
#include <stdint.h>
#include "netinet/in.h"

#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

int inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);

#endif
