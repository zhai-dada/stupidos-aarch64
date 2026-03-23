#ifndef __STUPIDOS_PTHREAD_H__
#define __STUPIDOS_PTHREAD_H__

#include "signal.h"
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

/*
 * 某些交叉头文件组合下，pthread 信号相关原型不会自动暴露出来。
 * 这里显式补一层，给 CPython 的 signalmodule / posixmodule 使用。
 */
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
int sigwait(const sigset_t *set, int *sig);
int pthread_kill(pthread_t thread, int sig);

#endif
