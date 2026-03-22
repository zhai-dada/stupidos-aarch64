#ifndef __STUPIDOS_PTHREAD_H__
#define __STUPIDOS_PTHREAD_H__

#include <sched.h>
#include_next <pthread.h>

/*
 * 先沿用工具链自带的 pthread 类型，让 CPython 的配置探测顺利通过。
 * 后续如果需要把线程完全接到 stupidos 的 task/futex 上，再在 compat.c
 * 里补对应实现即可。
 */

#ifndef _POSIX_THREADS
#define _POSIX_THREADS 1
#endif

#endif
