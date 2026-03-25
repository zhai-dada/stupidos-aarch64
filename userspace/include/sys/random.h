#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H

#include <stddef.h>
#include <sys/types.h>

/*
 * Linux 语义兼容：
 * - GRND_NONBLOCK：不阻塞等待熵池
 * - GRND_RANDOM：从随机设备池读取（当前内核暂时忽略该语义）
 */
#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 0x0001
#endif

#ifndef GRND_RANDOM
#define GRND_RANDOM 0x0002
#endif

ssize_t getrandom(void *buf, size_t len, unsigned int flags);

#endif
