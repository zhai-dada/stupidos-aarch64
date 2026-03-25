#ifndef __STUPIDOS_SYS_SYSINFO_H__
#define __STUPIDOS_SYS_SYSINFO_H__

#include <sys/types.h>

/*
 * 与 Linux 常见结构保持字段顺序一致，便于现有软件最小改动迁移。
 * 当前内核实现主要填充 uptime/totalram/freeram/procs/mem_unit。
 */
struct sysinfo
{
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
    char _f[64];
};

int sysinfo(struct sysinfo *info);

#endif
