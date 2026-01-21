#include "asm/asm.h"

.section ".text.boot"

FUNC_GLOBAL(_start)
	mrs	x0, mpidr_el1
	and	x0, x0, #0xFF		// 检查处理器核心ID
	cbz	x0, master		    // 除了CPU0，其他CPU都会在这里死循环等待
	b	proc_hang

proc_hang: 
	b 	proc_hang

master:
	adr	x0, __bss_start
	adr	x1, __bss_end
	sub x2, x1, x0
	mov x1, #0
	
	bl memset

	ldr	x0, =stack
	mov	sp, x0

	bl	kernel_main
	b 	proc_hang
FUNC_END(_start)
