#include "asm/asm.h"

/*
 * 受控进入 ELF 程序：
 * 1. 把 SP 重置到当前 task 栈顶，丢掉 exec_task_main 的旧调用帧
 * 2. 按 AArch64 ABI 传入 argc/argv
 * 3. 把 x30 预置成内核侧兜底返回处理函数
 *
 * 这样即便用户程序错误地 return，也不会把控制流落回一段已经失效的 C 栈帧。
 */
FUNC_GLOBAL(exec_enter)
	mov x9, x0
	mov x10, x2
	mov x11, x4
	and x12, x3, #~0xf
	mov sp, x12
	mov x0, x1
	mov x1, x10
	mov x29, xzr
	mov x30, x11
	br x9
FUNC_END(exec_enter)
