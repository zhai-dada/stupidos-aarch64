#ifndef __STUPIDOS_SIGNAL_H__
#define __STUPIDOS_SIGNAL_H__

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

/*
 * 交叉 GCC / glibc 编译时直接沿用工具链的 signal.h。
 * guest TinyCC 或显式最小模式时，走本地兼容定义。
 */
#if !defined(__TINYC__) && !defined(STUPIDOS_SIGNAL_MINIMAL)
#include_next <signal.h>
#endif

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

#ifndef __sigval_defined
union sigval
{
    int sival_int;
    void *sival_ptr;
};
#define __sigval_defined 1
#endif

#ifndef __siginfo_t_defined
typedef struct
{
    int si_signo;
    int si_errno;
    int si_code;
    pid_t si_pid;
    uid_t si_uid;
    int si_status;
    long si_band;
    union sigval si_value;
} siginfo_t;
#define __siginfo_t_defined 1
#endif

#ifndef __stack_t_defined
typedef struct
{
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;
#define __stack_t_defined 1
#endif

#ifndef __sigaction_defined
struct sigaction
{
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};
typedef struct sigaction sigaction_t;
#define __sigaction_defined 1
#endif

typedef void (*sighandler_t)(int);

#ifndef SA_ONSTACK
#define SA_ONSTACK 0x08000000
#endif
#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#endif
#ifndef SA_NODEFER
#define SA_NODEFER 0x40000000
#endif
#ifndef SA_RESETHAND
#define SA_RESETHAND 0x80000000
#endif
#ifndef SA_SIGINFO
#define SA_SIGINFO 0x00000004
#endif

#ifndef SIGSTKSZ
#define SIGSTKSZ 8192
#endif

#ifndef SIG_ERR
#define SIG_ERR ((void (*)(int))-1)
#endif
#ifndef SIG_DFL
#define SIG_DFL ((void (*)(int))0)
#endif
#ifndef SIG_IGN
#define SIG_IGN ((void (*)(int))1)
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

#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGINT
#define SIGINT 2
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGILL
#define SIGILL 4
#endif
#ifndef SIGABRT
#define SIGABRT 6
#endif
#ifndef SIGFPE
#define SIGFPE 8
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGSEGV
#define SIGSEGV 11
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGTERM
#define SIGTERM 15
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGSTOP
/* neatvi 在挂起终端时会发送 SIGSTOP，最小 signal 头需要补齐该常量。 */
#define SIGSTOP 19
#endif
#ifndef SIGWINCH
#define SIGWINCH 28
#endif

int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigdelset(sigset_t *set, int signum);
int sigismember(const sigset_t *set, int signum);
sighandler_t signal(int signum, sighandler_t handler);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigaltstack(const stack_t *ss, stack_t *old_ss);
int raise(int sig);
int sigwait(const sigset_t *set, int *sig);
int kill(pid_t pid, int sig);
int sigpending(sigset_t *set);
int sigwaitinfo(const sigset_t *set, siginfo_t *info);
int sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *timeout);

#endif
