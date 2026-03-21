#include "debug.h"
#include "lib/libasm.h"
#include "sched.h"
#include "timer.h"
#include "lib/librw.h"
#include "gicv2.h"

#define TICK_RATE_HZ 10

uint32_t cnt_tval = 0x00;
uint32_t cnt_ctl = 0x00;

static void plat_timer_init()
{
    uint64_t cur_cnt;
    uint64_t cur_freq;

	/* Get the frequency and init count */
	asm volatile
    (
        "mrs %0, cntvct_el0"
        :"=r"(cur_cnt)
        :
        :"memory"
    );
	asm volatile
    (
        "mrs %0, cntfrq_el0"
        :"=r"(cur_freq)
        :
        :"memory"
    );
    
	/* Calculate the tv */
	cnt_tval = (cur_freq / TICK_RATE_HZ);
    
	/* set the timervalue here */
	asm volatile
    (
        "msr cntv_tval_el0, %0"
        :
        :"r"(cnt_tval)
        :"memory"
    );

	/* Set the control register */
	cnt_ctl = 0x1;

	asm volatile
    (
        "msr cntv_ctl_el0, %0"
        :
        :"r"(cnt_ctl)
        :"memory"
    );
}

static void plat_handle_timer_irq()
{
	asm volatile
    (
        "msr cntv_tval_el0, %0"
        :
        :"r"(cnt_tval)
        :"memory"
    );

	asm volatile
    (
        "msr cntv_ctl_el0, %0"
        :
        :"r"(cnt_ctl)
        :"memory"
    );
}

volatile uint64_t jiffies;

void handle_timer_irq(void)
{
	++jiffies;
    scheduler_tick();
	plat_handle_timer_irq();
}

void timer_init(void)
{
	jiffies = 0;
	plat_timer_init();
    
    irq_handlers[GIC_INTID_VIRT_TIMER] = handle_timer_irq;
    
    gic_enable_irq(GIC_INTID_VIRT_TIMER);

    printk("[timer\tinit]: init ok\n");
    return;
}

void timer_init_secondary(void)
{
    plat_timer_init();
    gic_enable_irq(GIC_INTID_VIRT_TIMER);
    printk("[timer\tinit]: cpu %d local timer ready\n", arch_curr_cpu_id());
}
