#define SYS_EXIT 8

.section .text.start
.global _start

_start:
    mov x29, xzr
    /*
     * 内核 exec_enter 已把 argc/argv 放在 x0/x1。
     * u_lib_early_init() 可能改写这些 caller-saved 寄存器，
     * 先暂存到 callee-saved 寄存器，再恢复给 main。
     */
    mov x19, x0
    mov x20, x1
    bl u_lib_early_init
    mov x0, x19
    mov x1, x20
    bl main
    bl u_exit
