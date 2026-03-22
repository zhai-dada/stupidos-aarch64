#ifndef __SYSCALL_H__
#define __SYSCALL_H__

#include "asm/types.h"
#include "pt_regs.h"

#define ESR_EC_SHIFT            26
#define ESR_EC_MASK             0x3f
#define ESR_EC_SVC32            0x11
#define ESR_EC_SVC64            0x15

enum syscall_no
{
    SYS_READ = 0,
    SYS_WRITE,
    SYS_OPEN,
    SYS_CLOSE,
    SYS_LSEEK,
    SYS_YIELD,
    SYS_GETPID,
    SYS_TIME,
    SYS_EXIT,
    SYS_READDIR,
    SYS_NETTEST,
    SYS_EXEC,
    SYS_NETPING,
    SYS_WAITPID,
    SYS_SLEEP,
    SYS_NETCFG,
    SYS_MAX,
};

void syscall_init(void);
int64_t syscall_dispatch(pt_regs_t *regs);

#endif
