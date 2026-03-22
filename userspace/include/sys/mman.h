#ifndef __STUPIDOS_SYS_MMAN_H__
#define __STUPIDOS_SYS_MMAN_H__

#include_next <sys/mman.h>
#include "stupidos_user.h"

#undef PROT_NONE
#undef PROT_READ
#undef PROT_WRITE
#undef PROT_EXEC
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#undef MAP_SHARED
#undef MAP_PRIVATE
#undef MAP_ANONYMOUS
#define MAP_SHARED   0x01
#define MAP_PRIVATE  0x02
#define MAP_ANONYMOUS 0x20

#endif
