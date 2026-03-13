#include "mmu.h"
#include "asm/types.h"
#include "debug.h"

void mmu_init(void)
{
    uint32_t sctlr_el1;

    asm volatile
    (
        "mrs %0, sctlr_el1"
        : "=r"(sctlr_el1)
        :
        : "memory"
    );
    
    printk("sctlr_el1 : %#x\n", sctlr_el1);
    return;
}