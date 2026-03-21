#ifndef __SMP_H__
#define __SMP_H__

#include "asm/types.h"

#define CONFIG_MAX_CPUS 4

/*
 * 这是当前内核的最小 SMP 管理层。
 * 这一轮先完成三件事：
 * 1. 记录每个 CPU 的 MPIDR 和 online 状态
 * 2. 通过 PSCI CPU_ON 拉起次级核
 * 3. 给次级核建立独立栈并让它进入内核高地址执行
 */
struct cpu_info
{
    uint32_t logical_id;
    uint64_t mpidr;
    volatile bool online;
};

void smp_init(void);
void smp_secondary_boot(uint64_t cpu_id);
void smp_secondary_online(uint32_t cpu_id);
uint32_t smp_cpu_id(void);
uint32_t smp_cpu_count(void);
uint32_t smp_online_count(void);

#endif
