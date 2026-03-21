#include "asm/asm.h"

.section ".text.boot"

FUNC_GLOBAL(_start)
	adrp	x2, boot_dtb_phys
	str	x0, [x2, #:lo12:boot_dtb_phys]
	mrs	x1, mpidr_el1
	and	x1, x1, #0xFF		// 检查处理器核心ID
	cbz	x1, master		    // 除了CPU0，其他CPU先在这里等待，后续由 PSCI 单独拉起
	b	proc_hang

proc_hang: 
	b 	proc_hang

master:
	adrp x1, vectors
	add x1, x1, #:lo12:vectors
	msr	vbar_el1, x1

	adrp x1, init_stack_end
	add x1, x1, #:lo12:init_stack_end
	mov	sp, x1

	bl	kernel_main
	b 	proc_hang
FUNC_END(_start)
