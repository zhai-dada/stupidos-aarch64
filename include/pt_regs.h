#ifndef __PT_REGS_H__
#define __PT_REGS_H__

#include "asm/types.h"

typedef struct __pt_regs
{
    uint64_t s_reg[29];     // x0 - x28
    uint64_t s_fp;          // x29
    uint64_t s_lr;          // x30
    uint64_t s_sp;          // sp
    uint64_t s_pc;          // pc
    uint64_t s_pstate;      // pstate
}__attribute__((packed)) pt_regs_t;

void show_ptregs(pt_regs_t* regs);

#endif
