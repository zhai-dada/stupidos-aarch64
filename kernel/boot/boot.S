#include "asm/asm.h"

.section ".text.boot"

FUNC_GLOBAL(_start)
	mrs	x0, mpidr_el1
	and	x0, x0, #0xFF		// 检查处理器核心ID
	cbz	x0, master		    // 除了CPU0，其他CPU先在这里等待，后续由 PSCI 单独拉起
	b	proc_hang

proc_hang: 
	b 	proc_hang

master:
	adrp x0, vectors
	add x0, x0, #:lo12:vectors
	msr	vbar_el1, x0

	adrp x0, init_stack_end
	add x0, x0, #:lo12:init_stack_end
	mov	sp, x0

	bl	kernel_main
	b 	proc_hang
FUNC_END(_start)
