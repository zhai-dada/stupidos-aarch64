#include "irq.h"
#include "syscall.h"

void (*irq_handlers[MAX_IRQS])(void);

static const char * const bad_mode_handler[] =
{
	"Sync Abort",
	"IRQ",
	"FIQ",
	"SError"
};

void do_irq(void *stack)
{
	// pt_regs_t* regs = (pt_regs_t*)stack;

	disable_irq();
	// show_ptregs(regs);
	
	handle_irq();
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
	printk("Unhandled sync exception: esr = 0x%x ec=0x%x\n", esr, ec);
	
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
	uint32_t irqnr = 0;
	uint32_t irqstat = 0;

	irqstat = get32(GICC_IAR);
	irqnr = irqstat & 0x3ff;

	if(irq_handlers[irqnr])
	{
		irq_handlers[irqnr]();
	}

	put32(GICC_EOIR, irqstat);
	put32(GICC_DIR, irqstat);
}
