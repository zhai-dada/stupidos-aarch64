#ifndef __IRQ_H__
#define __IRQ_H__

#define GIC_INTID_EL2_PHYS_TIMER 26
#define GIC_INTID_VIRT_TIMER     27
#define GIC_INTID_EL3_PHYS_TIMER 29
#define GIC_INTID_EL1_PHYS_TIMER 30

#define IRQ_FRAME_SIZE  272

#define BAD_SYNC        0
#define BAD_IRQ         1
#define BAD_FIQ         2
#define BAD_ERROR       3

#endif
