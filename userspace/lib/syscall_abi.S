#include "asm/asm.h"

#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_LSEEK       4
#define SYS_YIELD       5
#define SYS_GETPID      6
#define SYS_TIME        7
#define SYS_EXIT        8
#define SYS_READDIR     9
#define SYS_NETTEST     10
#define SYS_EXEC        11
#define SYS_NETPING     12
#define SYS_WAITPID     13
#define SYS_SLEEP       14
#define SYS_NETCFG      15

.section .text

/*
 * 用户态 syscall 桩全部改成纯汇编，避免 C 包装器在 O0 下引入额外栈帧，
 * 这样可以把 “svc 返回后谁把 SP/LR 搞坏了” 这个问题先从用户库层剥离掉。
 */

FUNC_GLOBAL(u_read)
	mov x8, #SYS_READ
	svc #0
	ret
FUNC_END(u_read)

FUNC_GLOBAL(u_write)
	mov x8, #SYS_WRITE
	svc #0
	ret
FUNC_END(u_write)

FUNC_GLOBAL(u_open)
	mov x8, #SYS_OPEN
	svc #0
	ret
FUNC_END(u_open)

FUNC_GLOBAL(u_close)
	mov x8, #SYS_CLOSE
	svc #0
	ret
FUNC_END(u_close)

FUNC_GLOBAL(u_lseek)
	mov x8, #SYS_LSEEK
	svc #0
	ret
FUNC_END(u_lseek)

FUNC_GLOBAL(u_yield)
	mov x8, #SYS_YIELD
	svc #0
	ret
FUNC_END(u_yield)

FUNC_GLOBAL(u_getpid)
	mov x8, #SYS_GETPID
	svc #0
	ret
FUNC_END(u_getpid)

FUNC_GLOBAL(u_time)
	mov x8, #SYS_TIME
	svc #0
	ret
FUNC_END(u_time)

FUNC_GLOBAL(u_readdir)
	mov x8, #SYS_READDIR
	svc #0
	ret
FUNC_END(u_readdir)

FUNC_GLOBAL(u_nettest)
	mov x8, #SYS_NETTEST
	svc #0
	ret
FUNC_END(u_nettest)

FUNC_GLOBAL(u_exec)
	mov x8, #SYS_EXEC
	svc #0
	ret
FUNC_END(u_exec)

FUNC_GLOBAL(u_netping)
	mov x8, #SYS_NETPING
	svc #0
	ret
FUNC_END(u_netping)

FUNC_GLOBAL(u_waitpid)
	mov x8, #SYS_WAITPID
	svc #0
	ret
FUNC_END(u_waitpid)

FUNC_GLOBAL(u_sleep_ms)
	mov x8, #SYS_SLEEP
	svc #0
	ret
FUNC_END(u_sleep_ms)

FUNC_GLOBAL(u_netcfg)
	mov x8, #SYS_NETCFG
	svc #0
	ret
FUNC_END(u_netcfg)

FUNC_GLOBAL(u_exit)
	mov x8, #SYS_EXIT
	svc #0
1:
	b 1b
FUNC_END(u_exit)
