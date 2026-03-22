#ifndef __STUPIDOS_SIGNAL_H__
#define __STUPIDOS_SIGNAL_H__

#include <stddef.h>
#include <sys/types.h>
#include_next <signal.h>

/*
 * 用户态 signal 兼容层。
 * 这里优先使用交叉工具链自带的 POSIX 定义，只补足 CPython
 * 交叉编译时偶尔缺失的宏常量，避免我们自己重新定义一整套信号类型。
 */

#ifndef __sig_atomic_t_defined
typedef int sig_atomic_t;
#define __sig_atomic_t_defined 1
#endif

#ifndef __sigset_t_defined
typedef struct
{
    unsigned long __val[16];
} sigset_t;
#define __sigset_t_defined 1
#endif

#ifndef SA_ONSTACK
#define SA_ONSTACK 0x08000000
#endif

#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#endif
#ifndef SIG_UNBLOCK
#define SIG_UNBLOCK 1
#endif
#ifndef SIG_SETMASK
#define SIG_SETMASK 2
#endif

#ifndef NSIG
#define NSIG 64
#endif
#ifndef _NSIG
#define _NSIG NSIG
#endif

#endif
