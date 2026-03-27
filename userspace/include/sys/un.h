#ifndef __STUPIDOS_SYS_UN_H__
#define __STUPIDOS_SYS_UN_H__

#include <stddef.h>
#include "sys/socket.h"

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX 108
#endif

struct sockaddr_un
{
    sa_family_t sun_family;
    char sun_path[UNIX_PATH_MAX];
};

#ifndef SUN_LEN
#define SUN_LEN(sun) ((unsigned int)(offsetof(struct sockaddr_un, sun_path) + strlen((sun)->sun_path)))
#endif

#endif
