#include "mmu.h"

void mmu_init(void)
{
    uint32_t sctlr_el1;

	uint64_t mair = 0;
	uint64_t tcr = 0;
	uint64_t tmp;
	uint64_t parang;

	asm volatile
    (
        "tlbi vmalle1"
        : 
        : 
        : "memory"
    );
	dsb(nsh);

	write_sysreg(3UL << 20, cpacr_el1);
	write_sysreg(1UL << 12, mdscr_el1);

	mair = MAIR(MAIR_DEVICE_nGnRnE_ATTR, MT_DEVICE_nGnRnE_IDX) |
	       MAIR(MAIR_DEVICE_nGnRE_ATTR, MT_DEVICE_nGnRE_IDX) |
	       MAIR(MAIR_DEVICE_GRE_ATTR, MT_DEVICE_GRE_IDX) |
	       MAIR(MAIR_NORMAL_NC_ATTR, MT_NORMAL_NC_IDX) |
	       MAIR(MAIR_NORMAL_ATTR, MT_NORMAL_IDX) |
	       MAIR(MAIR_NORMAL_WT_ATTR, MT_NORMAL_WT_IDX);
           
	write_sysreg(mair, mair_el1);

	tcr = TCR_TxSZ(VA_BITS) | TCR_TG_FLAGS;

	tmp = read_sysreg(ID_AA64MMFR0_EL1);
	parang = tmp & 0xf;
	if (parang > ID_AA64MMFR0_PARANGE_48)
	{
        parang = ID_AA64MMFR0_PARANGE_48;
    }

	tcr |= parang << TCR_IPS_SHIFT;

	write_sysreg(tcr, tcr_el1);

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

int enable_mmu(void)
{
	unsigned long tmp;
	int tgran4;

	tmp = read_sysreg(ID_AA64MMFR0_EL1);
	tgran4 = (tmp >> ID_AA64MMFR0_TGRAN4_SHIFT) & 0xf;
	if (tgran4 != ID_AA64MMFR0_TGRAN4_SUPPORTED)
	{
		return -1;
	}

	write_sysreg(init_pgdir, ttbr0_el1);
	isb();

	write_sysreg(SCTLR_ELx_M, sctlr_el1);
	isb();
	
	asm("ic iallu");
	
	dsb(nsh);
	isb();

	return 0;
}
