#define S_FRAME_SIZE 272    /* sizeof(struct pt_regs)*/
#define S_X0 0              /* offsetof(struct pt_regs, regs[0])*/
#define S_X1 8              /* offsetof(struct pt_regs, regs[1])*/
#define S_X2 16             /* offsetof(struct pt_regs, regs[2])*/
#define S_X3 24             /* offsetof(struct pt_regs, regs[3])*/
#define S_X4 32             /* offsetof(struct pt_regs, regs[4])*/
#define S_X5 40             /* offsetof(struct pt_regs, regs[5])*/
#define S_X6 48             /* offsetof(struct pt_regs, regs[6])*/
#define S_X7 56             /* offsetof(struct pt_regs, regs[7])*/
#define S_X8 64             /* offsetof(struct pt_regs, regs[8])*/
#define S_X10 80            /* offsetof(struct pt_regs, regs[10])*/
#define S_X12 96            /* offsetof(struct pt_regs, regs[12])*/
#define S_X14 112           /* offsetof(struct pt_regs, regs[14])*/
#define S_X16 128           /* offsetof(struct pt_regs, regs[16])*/
#define S_X18 144           /* offsetof(struct pt_regs, regs[18])*/
#define S_X20 160           /* offsetof(struct pt_regs, regs[20])*/
#define S_X22 176           /* offsetof(struct pt_regs, regs[22])*/
#define S_X24 192           /* offsetof(struct pt_regs, regs[24])*/
#define S_X26 208           /* offsetof(struct pt_regs, regs[26])*/
#define S_X28 224           /* offsetof(struct pt_regs, regs[28])*/
#define S_FP 232            /* offsetof(struct pt_regs, regs[29])*/
#define S_LR 240            /* offsetof(struct pt_regs, regs[30])*/
#define S_SP 248            /* offsetof(struct pt_regs, sp)*/
#define S_PC 256            /* offsetof(struct pt_regs, pc)*/
#define S_PSTATE 264        /* offsetof(struct pt_regs, pstate)*/

#define BAD_SYNC        0
#define BAD_IRQ         1
#define BAD_FIQ         2
#define BAD_ERROR       3


.macro kernel_entry
	sub sp, sp, #S_FRAME_SIZE

	stp x0, x1, [sp, #16 * 0]
	stp x2, x3, [sp, #16 * 1]
	stp x4, x5, [sp, #16 * 2]
	stp x6, x7, [sp, #16 * 3]
	stp x8, x9, [sp, #16 * 4]
	stp x10, x11, [sp, #16 * 5]
	stp x12, x13, [sp, #16 * 6]
	stp x14, x15, [sp, #16 * 7]
	stp x16, x17, [sp, #16 * 8]
	stp x18, x19, [sp, #16 * 9]
	stp x20, x21, [sp, #16 * 10]
	stp x22, x23, [sp, #16 * 11]
	stp x24, x25, [sp, #16 * 12]
	stp x26, x27, [sp, #16 * 13]
	stp x28, x29, [sp, #16 * 14]

	add x21, sp, #S_FRAME_SIZE

	mrs x22, elr_el1
	mrs x23, spsr_el1

	stp lr, x21, [sp, #S_LR]
	stp x22, x23, [sp, #S_PC]
.endm

.macro kernel_exit
	ldp x21, x22, [sp, #S_PC]           // load ELR, SPSR

	msr elr_el1, x21                    // set up the return data
	msr spsr_el1, x22
	ldp x0, x1, [sp, #16 * 0]
	ldp x2, x3, [sp, #16 * 1]
	ldp x4, x5, [sp, #16 * 2]
	ldp x6, x7, [sp, #16 * 3]
	ldp x8, x9, [sp, #16 * 4]
	ldp x10, x11, [sp, #16 * 5]
	ldp x12, x13, [sp, #16 * 6]
	ldp x14, x15, [sp, #16 * 7]
	ldp x16, x17, [sp, #16 * 8]
	ldp x18, x19, [sp, #16 * 9]
	ldp x20, x21, [sp, #16 * 10]
	ldp x22, x23, [sp, #16 * 11]
	ldp x24, x25, [sp, #16 * 12]
	ldp x26, x27, [sp, #16 * 13]
	ldp x28, x29, [sp, #16 * 14]

	ldr lr, [sp, #S_LR]
	add sp, sp, #S_FRAME_SIZE           // restore sp
	eret
.endm

/*
   处理无效的异常向量
*/
.macro inv_entry el, reason
	kernel_entry
	mov x0, sp
	mov x1, #\reason
	mrs x2, esr_el1
	bl bad_mode
	kernel_exit
.endm

/*
   每个表项是128字节
   align 7表示128字节对齐
*/
.macro kernel_ventry label
	.align 7
	b \label
.endm

/*
 * Vector Table
 * ARM64的异常向量表一共占用2048个字节
 * 分成4组，每组4个表项，每个表项占128字节
 * align 11表示2048字节对齐
 */
.align 11
.global vectors
vectors:
	/* Current EL with SP0
	   当前系统运行在EL1时使用EL0的栈指针SP
	*/
	kernel_ventry el1_sync_invalid
	kernel_ventry el1_irq_invalid
	kernel_ventry el1_fiq_invalid
	kernel_ventry el1_error_invalid

	/* Current EL with SPx
	   当前系统运行在EL1时使用EL1的栈指针SP
	*/
	kernel_ventry el1_sync_invalid
	kernel_ventry el1_irq_invalid
	kernel_ventry el1_fiq_invalid
	kernel_ventry el1_error_invalid

	/*
	   在用户态的aarch64的程序发生了异常
	*/
	kernel_ventry el0_sync_invalid
	kernel_ventry el0_irq_invalid
	kernel_ventry el0_fiq_invalid
	kernel_ventry el0_error_invalid

	/*
	   在用户态的aarch32的程序发生了异常
	*/
	kernel_ventry el0_sync_invalid
	kernel_ventry el0_irq_invalid
	kernel_ventry el0_fiq_invalid
	kernel_ventry el0_error_invalid

el1_sync_invalid:
	inv_entry 1, BAD_SYNC
el1_irq_invalid:
	inv_entry 1, BAD_IRQ
el1_fiq_invalid:
	inv_entry 1, BAD_FIQ
el1_error_invalid:
	inv_entry 1, BAD_ERROR

el0_sync_invalid:
	inv_entry 0, BAD_SYNC
el0_irq_invalid:
	inv_entry 0, BAD_IRQ
el0_fiq_invalid:
	inv_entry 0, BAD_FIQ
el0_error_invalid:
	inv_entry 0, BAD_ERROR