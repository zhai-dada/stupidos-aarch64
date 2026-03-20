#ifndef __MMU_H__
#define __MMU_H__

#include "asm/pagetable_hwdef.h"
#include "asm/barrier.h"
#include "asm/types.h"
#include "debug.h"
#include "asm/sysreg.h"

extern uint64_t init_pgd;
extern uint64_t init_pud;
extern uint64_t init_pmd;
extern uint64_t init_pt;

void mmu_init(void);
int enable_mmu(void);

#endif