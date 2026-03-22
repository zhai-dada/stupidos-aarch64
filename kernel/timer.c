#include "debug.h"
#include "lib/libasm.h"
#include "sched.h"
#include "softirq.h"
#include "timer.h"
#include "lib/librw.h"
#include "gicv2.h"

#define TICK_RATE_HZ 10
#define KERNEL_TIMER_IRQ GIC_INTID_EL1_PHYS_TIMER

uint32_t cnt_tval = 0x00;
uint32_t cnt_ctl = 0x00;

static void plat_timer_init()
{
    uint64_t cur_cnt;
    uint64_t cur_freq;

	/* Get the frequency and init count */
	asm volatile
    (
        "mrs %0, cntpct_el0"
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
        "msr cntp_tval_el0, %0"
        :
        :"r"(cnt_tval)
        :"memory"
    );

	/* Set the control register */
	cnt_ctl = 0x1;

	asm volatile
    (
        "msr cntp_ctl_el0, %0"
        :
        :"r"(cnt_ctl)
        :"memory"
    );
    asm volatile("isb" : : : "memory");
}

static void plat_handle_timer_irq()
{
	asm volatile
    (
        "msr cntp_tval_el0, %0"
        :
        :"r"(cnt_tval)
        :"memory"
    );

	asm volatile
    (
        "msr cntp_ctl_el0, %0"
        :
        :"r"(cnt_ctl)
        :"memory"
    );
    asm volatile("isb" : : : "memory");
}

volatile uint64_t jiffies;
static bool timer_irq_seen;
static bool timer_tasklet_selftest_done;
static struct tasklet_struct timer_tasklet_selftest;

static void timer_tasklet_selftest_fn(unsigned long data)
{
    (void)data;
    printk("[softirq\tinit]: tasklet selftest ok cpu=%u jiffies=%lu\n",
           arch_curr_cpu_id(), jiffies);
}

void handle_timer_irq(void)
{
	++jiffies;
    if (!timer_irq_seen)
    {
        timer_irq_seen = true;
        printk("[timer\tirq ]: first tick jiffies=%lu\n", jiffies);
    }

    /*
     * 这里挂一个一次性的 tasklet 自检：
     * - 证明“硬中断 -> softirq -> tasklet”整条链路已经可用
     * - 之后网络收包、TTY 延后处理、块设备完成回调都可以复用这套框架
     */
    if (!timer_tasklet_selftest_done)
    {
        timer_tasklet_selftest_done = true;
        tasklet_schedule(&timer_tasklet_selftest);
    }

    scheduler_tick();
	plat_handle_timer_irq();
}

void timer_init(void)
{
	jiffies = 0;
    timer_irq_seen = false;
    timer_tasklet_selftest_done = false;
    tasklet_init(&timer_tasklet_selftest, timer_tasklet_selftest_fn, 0);
	plat_timer_init();
    
    irq_handlers[KERNEL_TIMER_IRQ] = handle_timer_irq;
    
    gic_enable_irq(KERNEL_TIMER_IRQ);

    printk("[timer\tinit]: irq=%u gic=%u init ok\n",
           KERNEL_TIMER_IRQ, gic_irq_is_enabled(KERNEL_TIMER_IRQ));
    return;
}

void timer_init_secondary(void)
{
    plat_timer_init();
    gic_enable_irq(KERNEL_TIMER_IRQ);
    printk("[timer\tinit]: cpu %d local timer irq=%u ready\n",
           arch_curr_cpu_id(), KERNEL_TIMER_IRQ);
}
