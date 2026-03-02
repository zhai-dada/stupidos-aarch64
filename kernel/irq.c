#include "debug.h"
#include "irq.h"
#include "lib/librw.h"
#include "lib/libirq.h"
#include "gicv2.h"

// Maximum number of IRQs
#define MAX_IRQS    128

// IRQ handlers
static void (*irq_handlers[MAX_IRQS])(void);

static const char * const bad_mode_handler[] =
{
	"Sync Abort",
	"IRQ",
	"FIQ",
	"SError"
};

static void handle_irq(void)
{
	gicv2_handle_irq();
	enable_irq();
}

void bad_mode(void *stack, uint32_t reason, uint32_t esr)
{
    disable_irq();
    // printk("Bad mode: reason=0x%x, esr=0x%x\n", reason, esr);
    // show_ptregs(stack);
    printk("%s\n", bad_mode_handler[reason]);
    handle_irq();
}

