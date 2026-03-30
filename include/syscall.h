#ifndef __SYSCALL_H__
#define __SYSCALL_H__

#include "asm/types.h"
#include "pt_regs.h"

#define ESR_EC_SHIFT            26
#define ESR_EC_MASK             0x3f
#define ESR_EC_SVC32            0x11
#define ESR_EC_SVC64            0x15

enum syscall_no
{
    SYS_READ = 0,
    SYS_WRITE,
    SYS_OPEN,
    SYS_CLOSE,
    SYS_LSEEK,
    SYS_YIELD,
    SYS_GETPID,
    SYS_TIME,
    SYS_EXIT,
    SYS_READDIR,
    SYS_NETTEST,
    SYS_EXEC,
    SYS_NETPING,
    SYS_WAITPID,
    SYS_SLEEP,
    SYS_NETCFG,
    SYS_CHDIR,
    SYS_GETCWD,
    SYS_STAT,
    SYS_FSTAT,
    SYS_UNAME,
    SYS_GETTIMEOFDAY,
    SYS_ISATTY,
    SYS_DUP2,
    SYS_BRK,
    SYS_MMAP,
    SYS_MUNMAP,
    SYS_MPROTECT,
    SYS_CLOCK_GETTIME,
    SYS_NANOSLEEP,
    SYS_GETUID,
    SYS_GETGID,
    SYS_GETEUID,
    SYS_GETEGID,
    SYS_ACCESS,
    SYS_OPENAT,
    SYS_FSTATAT,
    SYS_READLINK,
    SYS_IOCTL,
    SYS_DUP,
    SYS_READV,
    SYS_WRITEV,
    SYS_GETTID,
    SYS_GETPPID,
    SYS_EXIT_GROUP,
    SYS_GETRANDOM,
    SYS_SET_TID_ADDRESS,
    SYS_RT_SIGACTION,
    SYS_RT_SIGPROCMASK,
    SYS_SIGALTSTACK,
    SYS_FUTEX,
    SYS_PREAD64,
    SYS_PWRITE64,
    SYS_FCNTL,
    SYS_SCHED_GETAFFINITY,
    SYS_SYSINFO,
    SYS_PRLIMIT64,
    SYS_GETDENTS64,
    SYS_MKDIR,
    SYS_RMDIR,
    SYS_UNLINK,
    SYS_RENAME,
    SYS_TRUNCATE,
    SYS_FTRUNCATE,
    SYS_UTIMENSAT,
    SYS_HTTPGET,
    SYS_DNSLOOKUP,
    SYS_SOCKET,
    SYS_CONNECT,
    SYS_SHUTDOWN,
    SYS_GETSOCKOPT,
    SYS_FBINFO,
    SYS_FBFILL,
    SYS_FBTEXT,
    SYS_MOUSEINFO,
    SYS_PIPE2,
    SYS_WAITPID_STATUS,
    SYS_LINK,
    SYS_SYMLINK,
    SYS_MAX,
};

#define STUPIDOS_FUTEX_WAIT         0
#define STUPIDOS_FUTEX_WAKE         1
#define STUPIDOS_FUTEX_PRIVATE_FLAG 128

struct stupidos_rlimit
{
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

struct stupidos_sysinfo
{
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    uint8_t _pad[64];
};

struct stupidos_stat
{
    uint32_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t blocks;
    uint32_t blksize;
    uint32_t reserved;
};

struct stupidos_timeval
{
    int64_t tv_sec;
    int64_t tv_usec;
};

struct stupidos_mouseinfo
{
    int32_t x;
    int32_t y;
    uint32_t buttons;
};

struct stupidos_timespec
{
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct stupidos_iovec
{
    void *iov_base;
    uint64_t iov_len;
};

struct stupidos_utsname
{
    int8_t sysname[32];
    int8_t nodename[32];
    int8_t release[32];
    int8_t version[64];
    int8_t machine[32];
    int8_t domainname[32];
};

void syscall_init(void);
int64_t syscall_dispatch(pt_regs_t *regs);

#endif
