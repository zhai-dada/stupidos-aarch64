#define SYS_EXIT 8

.section .text.start
.global _start

_start:
    mov x29, xzr
    bl main
    bl u_exit
