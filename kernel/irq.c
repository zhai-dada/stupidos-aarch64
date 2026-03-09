#include "irq.h"

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
	
	gicv2_handle_irq();
	enable_irq();
	return;
}

void do_sync(void *stack, uint32_t esr)
{
	pt_regs_t* regs = (pt_regs_t*)stack;
	
	disable_irq();
	
	show_ptregs(regs);
	printk("Unhandled sync exception: esr = 0x%x\n", esr);
	
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
    printk("[%s]: reason=0x%x, esr=0x%x, far_el1=0x%x\n", bad_mode_handler[reason], reason, esr, read_sysreg(far_el1));
	
	while(1)
	{
		;
	}
}

