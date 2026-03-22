#ifndef __SOFTIRQ_H__
#define __SOFTIRQ_H__

#include "asm/types.h"

enum softirq_nr
{
    TASKLET_SOFTIRQ = 0,
    NR_SOFTIRQS,
};

typedef void (*tasklet_func_t)(unsigned long data);

struct tasklet_struct
{
    struct tasklet_struct *next;
    tasklet_func_t func;
    unsigned long data;
    bool scheduled;
};

void softirq_init(void);
void softirq_init_secondary(uint32_t cpu_id);
void raise_softirq(uint32_t nr);
void softirq_irq_exit(void);
uint32_t softirq_pending_mask(uint32_t cpu_id);

void tasklet_init(struct tasklet_struct *tasklet, tasklet_func_t func, unsigned long data);
void tasklet_schedule(struct tasklet_struct *tasklet);

#endif
