#include "irq.h"
#include "sched.h"
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

static void dump_backtrace(pt_regs_t *regs)
{
    uint64_t *fp;
    uint32_t depth;

    if (!regs)
    {
        return;
    }

    fp = (uint64_t *)regs->s_fp;
    for (depth = 0; depth < 6; depth++)
    {
        uint64_t prev_fp;
        uint64_t ret;

        if (!fp)
        {
            break;
        }

        /*
         * 只做最小的链路检查，避免在异常里又因为回溯本身踩到更坏的地址。
         */
        if ((uint64_t)fp < regs->s_sp || (uint64_t)fp >= regs->s_sp + 0x4000UL)
        {
            break;
        }

        prev_fp = fp[0];
        ret = fp[1];
        printk("[irq\ttrace]: bt%u fp=%#lx lr=%#lx\n", depth, (uint64_t)fp, ret);
        if (prev_fp <= (uint64_t)fp)
        {
            break;
        }

        fp = (uint64_t *)prev_fp;
    }
}

void do_irq(void *stack)
{
	// pt_regs_t* regs = (pt_regs_t*)stack;

    disable_irq();
	// show_ptregs(regs);
	
	handle_irq();
    softirq_irq_exit();
    /*
     * 这里补上一次真正的抢占检查。
     *
     * 之前 timer IRQ 虽然已经把 rq->need_resched 置位，但中断返回后仍然
     * 直接 eret 回当前任务，导致新建 ELF 任务必须等 shell 退出后才有机会跑。
     * 在 IRQ 退出前主动进一次调度，才能让 /bin/hello、/bin/ls 这类前台任务
     * 像 Linux 那样及时获得 CPU。
     */
    sched_maybe_resched();
	return;
}

void do_sync(void *stack, uint32_t esr)
{
	pt_regs_t* regs = (pt_regs_t*)stack;
    struct task_struct *curr;
	uint32_t ec;
	
	disable_irq();

	ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;
	if (ec == ESR_EC_SVC64)
	{
		regs->s_reg[0] = (uint64_t)syscall_dispatch(regs);
		/*
		 * AArch64 的 SVC 进入异常时，ELR_EL1 已经指向 SVC 之后的下一条指令。
		 * 这里如果再手动 +4，就会跳过 userspace syscall stub 里的 `ret`，
		 * 直接落到后续函数/数据区，表现为 shell 一执行 syscall 就跑飞。
		 */
        sched_maybe_resched();
		return;
	}

    curr = task_current();
    if (curr && curr->has_exec_image)
    {
        /*
         * 用户态 ELF 任务异常时，优先回收当前任务。
         * 由上层 supervisor 负责重启 shell，避免整个系统卡死在异常循环里。
         */
        printk("[irq\ttrace]: exec task fault pid=%d comm=%s ec=0x%x far_el1=%#lx\n",
               curr->pid, curr->comm, ec, read_sysreg(far_el1));
        show_ptregs(regs);
        dump_backtrace(regs);
        printk("[irq\ttrace]: fault pc=%#lx inst=%08x\n",
               regs->s_pc,
               *(uint32_t *)regs->s_pc);
        task_exit();
    }

	show_ptregs(regs);
    dump_backtrace(regs);
    printk("[irq\ttrace]: fault pc=%#lx inst=%08x\n",
	       regs->s_pc,
	       *(uint32_t *)regs->s_pc);
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
