#include "irq.h"
#include "softirq.h"
#include "syscall.h"

void (*irq_handlers[MAX_IRQS])(void);
static volatile uint32_t irq_debug_stage;
static volatile uint32_t irq_debug_iar;
static volatile uint32_t irq_debug_aiar;
static volatile uint32_t irq_debug_irqnr;
static volatile uint32_t irq_debug_eoir_reg;

static const char * const bad_mode_handler[] =
{
	"Sync Abort",
	"IRQ",
	"FIQ",
	"SError"
};

static inline bool gic_irq_is_special_id(uint32_t irqnr)
{
    return irqnr >= 1020U;
}

void do_irq(void *stack)
{
	// pt_regs_t* regs = (pt_regs_t*)stack;

	disable_irq();
	// show_ptregs(regs);
	
	handle_irq();
    softirq_irq_exit();
	enable_irq();
	return;
}

void do_sync(void *stack, uint32_t esr)
{
	pt_regs_t* regs = (pt_regs_t*)stack;
	uint32_t ec;
	
	disable_irq();

	ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;
	if (ec == ESR_EC_SVC64)
	{
		regs->s_reg[0] = (uint64_t)syscall_dispatch(regs);
		regs->s_pc += 4;
		return;
	}

	show_ptregs(regs);
    printk("[irq\ttrace]: stage=%u iar=%#x aiar=%#x irq=%u eoir=%#x hppir=%u ahppir=%u ctlr=%#x\n",
           irq_debug_stage,
           irq_debug_iar,
           irq_debug_aiar,
           irq_debug_irqnr,
           irq_debug_eoir_reg,
           get32(GICC_HPPIR) & 0x3ffU,
           get32(GICC_AHPPIR) & 0x3ffU,
           get32(GICC_CTLR));
	printk("Unhandled sync exception: esr = 0x%x ec=0x%x far_el1=%#lx\n",
           esr, ec, read_sysreg(far_el1));
	
	while (1)
	{
		;
	}
}

void bad_mode(void *stack, uint32_t reason, uint32_t esr)
{
	pt_regs_t* regs = (pt_regs_t*)stack;

    disable_irq();

	show_ptregs(regs);
    printk("[%s]: reason=0x%x, esr=0x%x, far_el1=0x%lx\n", bad_mode_handler[reason], reason, esr, read_sysreg(far_el1));
	
	while(1)
	{
		;
	}
}

void handle_irq(void)
{
	uint32_t irqnr;
	uint32_t irqstat;
    uint64_t eoir_reg = GICC_EOIR;

    irq_debug_stage = 1;
	irqstat = get32(GICC_IAR);
    irq_debug_iar = irqstat;
	irqnr = irqstat & 0x3ff;

    /*
     * 在 secure CPU interface 视图里，如果最高优先级中断属于 Group1，
     * 直接读 IAR/HPPIR 可能只看到特殊值 1022，而真实的 intid 需要从
     * AIAR/AHPPIR 这组 alias 寄存器里取。
     */
    if (irqnr == 1022U || irqnr == 1023U)
    {
        uint32_t alias_irqstat = get32(GICC_AIAR);
        uint32_t alias_irqnr = alias_irqstat & 0x3ffU;

        irq_debug_aiar = alias_irqstat;
        if (!gic_irq_is_special_id(alias_irqnr))
        {
            irqstat = alias_irqstat;
            irqnr = alias_irqnr;
            eoir_reg = GICC_AEOIR;
        }
    }

    irq_debug_irqnr = irqnr;
    irq_debug_eoir_reg = (uint32_t)eoir_reg;
    irq_debug_stage = 2;
    if (gic_irq_is_special_id(irqnr))
    {
        return;
    }

    irq_debug_stage = 3;
	if (irqnr < MAX_IRQS && irq_handlers[irqnr])
	{
		irq_handlers[irqnr]();
	}

    irq_debug_stage = 4;
	put32(eoir_reg, irqstat);
	put32(GICC_DIR, irqstat);
    irq_debug_stage = 5;
}
