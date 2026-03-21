#include "asm/asm.h"

#define SECONDARY_BOOTDATA_STACK   0
#define SECONDARY_BOOTDATA_CPUID   8

.section ".text.boot"

/*
 * 次级核从 PSCI CPU_ON 跳进来时，MMU 还没开，运行在低地址物理映射上。
 * x0 由 PSCI 传入，指向 secondary_boot_data：
 * - [0x0]  栈顶物理地址
 * - [0x8]  逻辑 CPU 编号
 */
FUNC_GLOBAL(secondary_entry)
	adrp x2, vectors
	add x2, x2, #:lo12:vectors
	msr vbar_el1, x2

	ldr x1, [x0, #SECONDARY_BOOTDATA_CPUID]
	ldr x2, [x0, #SECONDARY_BOOTDATA_STACK]
	mov sp, x2
	mov x0, x1
	bl smp_secondary_boot
1:
	wfi
	b 1b
FUNC_END(secondary_entry)
