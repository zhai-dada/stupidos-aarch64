#ifndef __STUPIDOS_SCHED_H__
#define __STUPIDOS_SCHED_H__

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

#endif
