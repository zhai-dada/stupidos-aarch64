#ifndef __STUPIDOS_SCHED_H__
#define __STUPIDOS_SCHED_H__

#include <sys/types.h>
#include <time.h>

/*
 * 用户态只需要一个最小的 cpu_set_t 兼容定义。
 * 不再转发到内核 sched.h，避免把内核私有 asm/types.h 带进来。
 */

#ifndef CPU_SETSIZE
#define CPU_SETSIZE 1024
#endif

#ifndef __CPU_SET_T_DEFINED
typedef struct
{
    unsigned long __bits[CPU_SETSIZE / (8 * sizeof(unsigned long))];
} cpu_set_t;
#define __CPU_SET_T_DEFINED
#endif

struct sched_param
{
    int sched_priority;
};

#ifndef SCHED_OTHER
#define SCHED_OTHER 0
#endif
#ifndef SCHED_FIFO
#define SCHED_FIFO 1
#endif
#ifndef SCHED_RR
#define SCHED_RR 2
#endif

int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
int sched_getscheduler(pid_t pid);
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
int sched_getparam(pid_t pid, struct sched_param *param);
int sched_setparam(pid_t pid, const struct sched_param *param);
int sched_rr_get_interval(pid_t pid, struct timespec *tp);
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask);
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);
int sched_yield(void);

#endif
