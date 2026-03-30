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
#define SYS_CHDIR       16
#define SYS_GETCWD      17
#define SYS_STAT        18
#define SYS_FSTAT       19
#define SYS_UNAME       20
#define SYS_GETTIMEOFDAY 21
#define SYS_ISATTY      22
#define SYS_DUP2        23
#define SYS_BRK         24
#define SYS_MMAP        25
#define SYS_MUNMAP      26
#define SYS_MPROTECT    27
#define SYS_CLOCK_GETTIME 28
#define SYS_NANOSLEEP   29
#define SYS_GETUID      30
#define SYS_GETGID      31
#define SYS_GETEUID     32
#define SYS_GETEGID     33
#define SYS_ACCESS      34
#define SYS_OPENAT      35
#define SYS_FSTATAT     36
#define SYS_READLINK    37
#define SYS_IOCTL       38
#define SYS_DUP         39
#define SYS_READV       40
#define SYS_WRITEV      41
#define SYS_GETTID      42
#define SYS_GETPPID     43
#define SYS_EXIT_GROUP  44
#define SYS_GETRANDOM   45
#define SYS_SET_TID_ADDRESS 46
#define SYS_RT_SIGACTION 47
#define SYS_RT_SIGPROCMASK 48
#define SYS_SIGALTSTACK 49
#define SYS_FUTEX      50
#define SYS_PREAD64    51
#define SYS_PWRITE64   52
#define SYS_FCNTL      53
#define SYS_SCHED_GETAFFINITY 54
#define SYS_SYSINFO    55
#define SYS_PRLIMIT64   56
#define SYS_GETDENTS64 57
#define SYS_MKDIR      58
#define SYS_RMDIR      59
#define SYS_UNLINK     60
#define SYS_RENAME     61
#define SYS_TRUNCATE   62
#define SYS_FTRUNCATE  63
#define SYS_UTIMENSAT  64
#define SYS_HTTPGET    65
#define SYS_DNSLOOKUP  66
#define SYS_SOCKET     67
#define SYS_CONNECT    68
#define SYS_SHUTDOWN   69
#define SYS_GETSOCKOPT 70
#define SYS_FBINFO     71
#define SYS_FBFILL     72
#define SYS_FBTEXT     73
#define SYS_MOUSEINFO  74
#define SYS_PIPE2      75
#define SYS_WAITPID_STATUS 76
#define SYS_LINK      77
#define SYS_SYMLINK   78

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

FUNC_GLOBAL(u_waitpid_status)
	mov x8, #SYS_WAITPID_STATUS
	svc #0
	ret
FUNC_END(u_waitpid_status)

FUNC_GLOBAL(u_link)
	mov x8, #SYS_LINK
	svc #0
	ret
FUNC_END(u_link)

FUNC_GLOBAL(u_symlink)
	mov x8, #SYS_SYMLINK
	svc #0
	ret
FUNC_END(u_symlink)

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

FUNC_GLOBAL(u_chdir)
	mov x8, #SYS_CHDIR
	svc #0
	ret
FUNC_END(u_chdir)

FUNC_GLOBAL(u_getcwd)
	mov x8, #SYS_GETCWD
	svc #0
	ret
FUNC_END(u_getcwd)

FUNC_GLOBAL(u_stat)
	mov x8, #SYS_STAT
	svc #0
	ret
FUNC_END(u_stat)

FUNC_GLOBAL(u_fstat)
	mov x8, #SYS_FSTAT
	svc #0
	ret
FUNC_END(u_fstat)

FUNC_GLOBAL(u_uname)
	mov x8, #SYS_UNAME
	svc #0
	ret
FUNC_END(u_uname)

FUNC_GLOBAL(u_gettimeofday)
	mov x8, #SYS_GETTIMEOFDAY
	svc #0
	ret
FUNC_END(u_gettimeofday)

FUNC_GLOBAL(u_isatty)
	mov x8, #SYS_ISATTY
	svc #0
	ret
FUNC_END(u_isatty)

FUNC_GLOBAL(u_dup2)
	mov x8, #SYS_DUP2
	svc #0
	ret
FUNC_END(u_dup2)

FUNC_GLOBAL(u_brk)
	mov x8, #SYS_BRK
	svc #0
	ret
FUNC_END(u_brk)

FUNC_GLOBAL(u_mmap)
	mov x8, #SYS_MMAP
	svc #0
	ret
FUNC_END(u_mmap)

FUNC_GLOBAL(u_munmap)
	mov x8, #SYS_MUNMAP
	svc #0
	ret
FUNC_END(u_munmap)

FUNC_GLOBAL(u_mprotect)
	mov x8, #SYS_MPROTECT
	svc #0
	ret
FUNC_END(u_mprotect)

FUNC_GLOBAL(u_clock_gettime)
	mov x8, #SYS_CLOCK_GETTIME
	svc #0
	ret
FUNC_END(u_clock_gettime)

FUNC_GLOBAL(u_nanosleep)
	mov x8, #SYS_NANOSLEEP
	svc #0
	ret
FUNC_END(u_nanosleep)

FUNC_GLOBAL(u_getuid)
	mov x8, #SYS_GETUID
	svc #0
	ret
FUNC_END(u_getuid)

FUNC_GLOBAL(u_getgid)
	mov x8, #SYS_GETGID
	svc #0
	ret
FUNC_END(u_getgid)

FUNC_GLOBAL(u_geteuid)
	mov x8, #SYS_GETEUID
	svc #0
	ret
FUNC_END(u_geteuid)

FUNC_GLOBAL(u_getegid)
	mov x8, #SYS_GETEGID
	svc #0
	ret
FUNC_END(u_getegid)

FUNC_GLOBAL(u_access)
	mov x8, #SYS_ACCESS
	svc #0
	ret
FUNC_END(u_access)

FUNC_GLOBAL(u_openat)
	mov x8, #SYS_OPENAT
	svc #0
	ret
FUNC_END(u_openat)

FUNC_GLOBAL(u_fstatat)
	mov x8, #SYS_FSTATAT
	svc #0
	ret
FUNC_END(u_fstatat)

FUNC_GLOBAL(u_readlink)
	mov x8, #SYS_READLINK
	svc #0
	ret
FUNC_END(u_readlink)

FUNC_GLOBAL(u_ioctl)
	mov x8, #SYS_IOCTL
	svc #0
	ret
FUNC_END(u_ioctl)

FUNC_GLOBAL(u_dup)
	mov x8, #SYS_DUP
	svc #0
	ret
FUNC_END(u_dup)

FUNC_GLOBAL(u_readv)
	mov x8, #SYS_READV
	svc #0
	ret
FUNC_END(u_readv)

FUNC_GLOBAL(u_writev)
	mov x8, #SYS_WRITEV
	svc #0
	ret
FUNC_END(u_writev)

FUNC_GLOBAL(u_gettid)
	mov x8, #SYS_GETTID
	svc #0
	ret
FUNC_END(u_gettid)

FUNC_GLOBAL(u_getppid)
	mov x8, #SYS_GETPPID
	svc #0
	ret
FUNC_END(u_getppid)

FUNC_GLOBAL(u_exit_group)
	mov x8, #SYS_EXIT_GROUP
	svc #0
1:
	b 1b
FUNC_END(u_exit_group)

FUNC_GLOBAL(u_getrandom)
	mov x8, #SYS_GETRANDOM
	svc #0
	ret
FUNC_END(u_getrandom)

FUNC_GLOBAL(u_set_tid_address)
	mov x8, #SYS_SET_TID_ADDRESS
	svc #0
	ret
FUNC_END(u_set_tid_address)

FUNC_GLOBAL(u_rt_sigaction)
	mov x8, #SYS_RT_SIGACTION
	svc #0
	ret
FUNC_END(u_rt_sigaction)

FUNC_GLOBAL(u_rt_sigprocmask)
	mov x8, #SYS_RT_SIGPROCMASK
	svc #0
	ret
FUNC_END(u_rt_sigprocmask)

FUNC_GLOBAL(u_sigaltstack)
	mov x8, #SYS_SIGALTSTACK
	svc #0
	ret
FUNC_END(u_sigaltstack)

FUNC_GLOBAL(u_futex)
	mov x8, #SYS_FUTEX
	svc #0
	ret
FUNC_END(u_futex)

FUNC_GLOBAL(u_pread64)
	mov x8, #SYS_PREAD64
	svc #0
	ret
FUNC_END(u_pread64)

FUNC_GLOBAL(u_pwrite64)
	mov x8, #SYS_PWRITE64
	svc #0
	ret
FUNC_END(u_pwrite64)

FUNC_GLOBAL(u_fcntl)
	mov x8, #SYS_FCNTL
	svc #0
	ret
FUNC_END(u_fcntl)

FUNC_GLOBAL(u_sched_getaffinity)
	mov x8, #SYS_SCHED_GETAFFINITY
	svc #0
	ret
FUNC_END(u_sched_getaffinity)

FUNC_GLOBAL(u_sysinfo)
	mov x8, #SYS_SYSINFO
	svc #0
	ret
FUNC_END(u_sysinfo)

FUNC_GLOBAL(u_prlimit64)
	mov x8, #SYS_PRLIMIT64
	svc #0
	ret
FUNC_END(u_prlimit64)

FUNC_GLOBAL(u_getdents64)
	mov x8, #SYS_GETDENTS64
	svc #0
	ret
FUNC_END(u_getdents64)

FUNC_GLOBAL(u_mkdir)
	mov x8, #SYS_MKDIR
	svc #0
	ret
FUNC_END(u_mkdir)

FUNC_GLOBAL(u_rmdir)
	mov x8, #SYS_RMDIR
	svc #0
	ret
FUNC_END(u_rmdir)

FUNC_GLOBAL(u_unlink)
	mov x8, #SYS_UNLINK
	svc #0
	ret
FUNC_END(u_unlink)

FUNC_GLOBAL(u_rename)
	mov x8, #SYS_RENAME
	svc #0
	ret
FUNC_END(u_rename)

FUNC_GLOBAL(u_truncate)
	mov x8, #SYS_TRUNCATE
	svc #0
	ret
FUNC_END(u_truncate)

FUNC_GLOBAL(u_ftruncate)
	mov x8, #SYS_FTRUNCATE
	svc #0
	ret
FUNC_END(u_ftruncate)

FUNC_GLOBAL(u_utimensat)
	mov x8, #SYS_UTIMENSAT
	svc #0
	ret
FUNC_END(u_utimensat)

FUNC_GLOBAL(u_http_get)
	mov x8, #SYS_HTTPGET
	svc #0
	ret
FUNC_END(u_http_get)

FUNC_GLOBAL(u_dns_lookup)
	mov x8, #SYS_DNSLOOKUP
	svc #0
	ret
FUNC_END(u_dns_lookup)

FUNC_GLOBAL(u_socket)
	mov x8, #SYS_SOCKET
	svc #0
	ret
FUNC_END(u_socket)

FUNC_GLOBAL(u_connect)
	mov x8, #SYS_CONNECT
	svc #0
	ret
FUNC_END(u_connect)

FUNC_GLOBAL(u_shutdown)
	mov x8, #SYS_SHUTDOWN
	svc #0
	ret
FUNC_END(u_shutdown)

FUNC_GLOBAL(u_getsockopt)
	mov x8, #SYS_GETSOCKOPT
	svc #0
	ret
FUNC_END(u_getsockopt)

FUNC_GLOBAL(u_pipe2)
	mov x8, #SYS_PIPE2
	svc #0
	ret
FUNC_END(u_pipe2)

FUNC_GLOBAL(u_fbinfo)
	mov x8, #SYS_FBINFO
	svc #0
	ret
FUNC_END(u_fbinfo)

FUNC_GLOBAL(u_fbfill)
	mov x8, #SYS_FBFILL
	svc #0
	ret
FUNC_END(u_fbfill)

FUNC_GLOBAL(u_fbtext)
	mov x8, #SYS_FBTEXT
	svc #0
	ret
FUNC_END(u_fbtext)

FUNC_GLOBAL(u_mouseinfo)
	mov x8, #SYS_MOUSEINFO
	svc #0
	ret
FUNC_END(u_mouseinfo)

FUNC_GLOBAL(u_exit)
	mov x8, #SYS_EXIT
	svc #0
1:
	b 1b
FUNC_END(u_exit)
