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
    printk("[GICv2\tinit]: distributor init ok\n");
    return;
}

static void gicv2_cpu_init()
{
    uint32_t bypass;
    uint32_t i;

	/*
	 * Deal with the banked PPI and SGI interrupts - disable all
	 * private interrupts. Make sure everything is deactivated.
	 */
	for (i = 0; i < 32; i += 32)
    {
		put32(GICD_ICACTIVER + i / 8, GICD_INT_EN_CLR_X32);
		put32(GICD_ICENABLER + i / 8, GICD_INT_EN_CLR_X32);
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

    /* Finest granularity of priority */
    put32(GICC_BPR, 0);

    for (i = 0; i < 4; i++)
    {
        put32(GICC_APR + i * 4, 0);
    }

    /* Turn on delivery */
    bypass = get32(GICC_CTLR);
    bypass &= GICC_DIS_BYPASS_MASK;
    put32(GICC_CTLR, bypass | GICC_CTRL_EOImodeNS | GICC_ENABLE);
    printk("[GICv2\tinit]: cpu interface init ok\n");
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
    uint32_t reg_val = get32(GICD_ISENABLER + (reg * 4));
    reg_val |= (1UL << bit);
    put32(GICD_ISENABLER + (reg * 4), reg_val);
    return (get32(GICD_ISENABLER + (reg * 4)) | (1UL << bit));
}
