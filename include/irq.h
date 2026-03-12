#ifndef __IRQ_H__
#define __IRQ_H__

#include "debug.h"
#include "pt_regs.h"
#include "lib/librw.h"
#include "lib/libirq.h"
#include "lib/libasm.h"
#include "gicv2.h"

#define GIC_INTID_EL2_PHYS_TIMER 26
#define GIC_INTID_VIRT_TIMER     27
#define GIC_INTID_EL3_PHYS_TIMER 29
#define GIC_INTID_EL1_PHYS_TIMER 30

#define UART_IRQ                 33

#define IRQ_FRAME_SIZE  272

#define BAD_SYNC        0
#define BAD_IRQ         1
#define BAD_FIQ         2
#define BAD_ERROR       3

#define MAX_IRQS    1024

extern void (*irq_handlers[MAX_IRQS])(void);


void do_irq(void *stack);
void do_sync(void *stack, uint32_t esr);
void bad_mode(void *stack, uint32_t reason, uint32_t esr);

void handle_irq(void);

#endif
