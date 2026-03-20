#include "mmu.h"

void mmu_init(void)
{
    uint32_t sctlr_el1;

	uint64_t mair = 0;
	uint64_t tcr = 0;
	uint64_t tmp;
	uint64_t parang;

	// 
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

	// TG0 TG1 设置页面颗粒度
	// TxSZ 设置输入地址最大值
	tcr = TCR_TxSZ(VA_BITS) | TCR_TG_FLAGS;

	tmp = read_sysreg(ID_AA64MMFR0_EL1);

	// 系统支持最大的物理地址范围
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

	// PGD 页表基地址写入 ttbr0_el1
	write_sysreg(init_pgd, ttbr0_el1);
	isb();

	// SCTLR M位,开启MMU
	write_sysreg(SCTLR_ELx_M, sctlr_el1);
	isb();

	// 清空所有 CPU 的指令缓存（当前 EL 可见范围）保证后续执行的指令一定从内存重新取
	asm("ic iallu");
	dsb(nsh);

	// 重新取指令
	isb();

	return 0;
}

void page_map_init(void)
{

	return;
}
