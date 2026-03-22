#include "gicv2.h"
#include "lib/librw.h"
#include "lib/libasm.h"
#include "debug.h"
#include "irq.h"
#include "timer.h"

_gic_info global_gic_info;

static uint8_t gicv2_get_cpumask(void)
{
    uint8_t i = 0, mask = 0;
    for (i = mask = 0; i < 32; i += 4)
    {
        mask = get32(GICD_ITARGETSR);
        mask |= mask >> 16;
        mask |= mask >> 8;
        if (mask)
        {
            break;
        }
    }

    return mask;
}

static void gicv2_dist_init()
{
    uint32_t cpumask;
    uint32_t type;
    uint32_t nr_lines;
    int32_t i;

    /* Disable the distributor */
    put32(GICD_CTLR, GICD_DISABLE);

    type = get32(GICD_TYPER);
    nr_lines = get32(GICD_TYPER) & 0x1f;
    nr_lines = (nr_lines + 1) * 32;

    global_gic_info.irq_num = nr_lines;
    global_gic_info.cpu_num = ((type & 0x0e0) >> 5) + 1;

    printk("[GICv2\tinit]: %d irq(s), %d cpu(s)\n", global_gic_info.irq_num, global_gic_info.cpu_num);

    /* Set all global interrupts to this CPU only */
    cpumask = gicv2_get_cpumask();
    cpumask |= cpumask << 8;
    cpumask |= cpumask << 16;

    for (i = 32; i < nr_lines; i += 4)
    {
        put32(GICD_ITARGETSR + i * 4 / 4, cpumask);
    }

    /*
     * 把外设中断都归到 Group1 IRQ。
     * 当前内核跑在普通 EL1 路径，串口、virtio、PCIe 这些设备中断
     * 都应该按普通 IRQ 送到 CPU，而不是留在 Group0/FIQ 语义里。
     */
    for (i = 32; i < nr_lines; i += 32)
    {
        put32(GICD_IGROUPR + i / 8, 0xffffffffU);
    }

    /* Set all global interrupts to be level triggered, active low */
    for (i = 32; i < nr_lines; i += 16)
    {
        put32(GICD_ICFGR + i / 4, GICD_INT_ACTLOW_LVLTRIG);
    }

    /* Set priority on all global interrupts */
    for (i = 32; i < nr_lines; i += 4)
    {
        put32(GICD_IPRIORITYR + i, GICD_INT_DEF_PRI_X4);
    }

    /* Turn on the distributor */
    put32(GICD_CTLR, GICD_ENABLE);
    asm volatile("dsb sy\nisb" : : : "memory");
    printk("[GICv2\tinit]: distributor init ok\n");
    return;
}

static void gicv2_cpu_init()
{
    uint32_t bypass;
    uint32_t ctlr;
    uint32_t i;

	/*
	 * Deal with the banked PPI and SGI interrupts - disable all
	 * private interrupts. Make sure everything is deactivated.
	 */
	for (i = 0; i < 32; i += 32)
    {
		put32(GICD_ICACTIVER + i / 8, GICD_INT_EN_CLR_X32);
		put32(GICD_ICENABLER + i / 8, GICD_INT_EN_CLR_X32);
        put32(GICD_ICPENDR + i / 8, GICD_INT_EN_CLR_X32);
        put32(GICD_IGROUPR + i / 8, 0xffffffffU);
	}

    /*
     * SGI 的 pending 位是独立 banked 的。
     * 若不显式清掉，某些 QEMU/固件组合上会在刚开 IRQ 时先收到一发
     * 残留的私有中断，导致我们误判成 timer/uart 的问题。
     */
    for (i = 0; i < 4; i++)
    {
        put32(GICD_SGIR_CLRPEND + i * 4, 0xffffffffU);
    }

	/* Set priority on PPI and SGI interrupts */
	for (i = 0; i < 32; i += 4)
    {
		put32(GICD_IPRIORITYR + i * 4 / 4, GICD_INT_DEF_PRI_X4);
	}

	// /* Ensure all SGI interrupts are now enabled */
	// put32(GICD_ISENABLER, GICD_INT_EN_SET_SGI);

    /* Don't mask by priority */
    put32(GICC_PMR, GICC_INT_PRI_THRESHOLD);

    /*
     * Group0/Group1 都设成最细粒度的抢占分组。
     * 这样即便当前 QEMU/固件把 CPU interface 暴露成 secure 视图，
     * Group1 IRQ 也不会因为别名寄存器或二级分组配置不一致而卡住。
     */
    put32(GICC_BPR, 0);
    put32(GICC_ABPR, 0);

    for (i = 0; i < 4; i++)
    {
        put32(GICC_APR + i * 4, 0);
    }

    /*
     * QEMU virt 这里既可能表现成 non-secure alias 视图，
     * 也可能给我们 secure banked 视图。
     * 统一把 Group0/Group1 都打开，避免 Group1 只挂起不送达 CPU。
     */
    bypass = get32(GICC_CTLR);
    bypass &= GICC_DIS_BYPASS_MASK;
    ctlr = bypass | GICC_CTRL_ACKCTL | GICC_CTRL_EOImodeNS | GICC_ENABLE;
    put32(GICC_CTLR, ctlr);
    asm volatile("dsb sy\nisb" : : : "memory");
    printk("[GICv2\tinit]: cpu interface ctlr=%#x pmr=%#x init ok\n",
           get32(GICC_CTLR), get32(GICC_PMR));
    return;
}

void gic_init(void)
{
	uint32_t cpuid = arch_curr_cpu_id();

	if (cpuid == 0)
	{
        gicv2_dist_init();
    }

	/* init the cpu interface (GICC) */
	gicv2_cpu_init();

	/* enable the ppi's irq */
	// put32(GICD_ISENABLER, GICD_INT_EN_CLR_PPI);
    printk("[GICv2\tinit]: init ok\n");
    return;
}

uint32_t gic_enable_irq(uint32_t irq)
{
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;

    if(irq >= global_gic_info.irq_num)
    {
        printk("[gic_enable_irq\terror]: irq > irq_num\n");
        return 0;
    }

    /*
     * GICD_ISENABLER 是 write-1-to-set 语义。
     * 这里直接写单 bit，避免把一次普通“使能某个 IRQ”变成不必要的读改写。
     */
    put32(GICD_ISENABLER + (reg * 4), (1UL << bit));
    asm volatile("dsb sy\nisb" : : : "memory");
    return gic_irq_is_enabled(irq);
}

uint32_t gic_irq_is_enabled(uint32_t irq)
{
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;

    if (irq >= global_gic_info.irq_num)
    {
        return 0;
    }

    return (get32(GICD_ISENABLER + (reg * 4)) >> bit) & 0x1U;
}
