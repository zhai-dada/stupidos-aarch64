#ifndef __STUPIDOS_USER_H__
#define __STUPIDOS_USER_H__

#include "asm/types.h"

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
#define SYS_FUTEX       50
#define SYS_PREAD64     51
#define SYS_PWRITE64    52
#define SYS_FCNTL       53
#define SYS_SCHED_GETAFFINITY 54
#define SYS_SYSINFO     55
#define SYS_PRLIMIT64   56

#define STUPIDOS_PATH_MAX       256

#define STUPIDOS_STDIN_FILENO   0
#define STUPIDOS_STDOUT_FILENO  1
#define STUPIDOS_STDERR_FILENO  2

#define STUPIDOS_O_RDONLY       0x1
#define STUPIDOS_O_WRONLY       0x2
#define STUPIDOS_O_RDWR         (STUPIDOS_O_RDONLY | STUPIDOS_O_WRONLY)

#define STUPIDOS_SEEK_SET       0
#define STUPIDOS_SEEK_CUR       1
#define STUPIDOS_SEEK_END       2

#define STUPIDOS_VFS_S_IFMT     0xF000
#define STUPIDOS_VFS_S_IFREG    0x8000
#define STUPIDOS_VFS_S_IFDIR    0x4000
#define STUPIDOS_VFS_S_IFCHR    0x2000

#define STUPIDOS_VFS_S_IRUSR    0x0100
#define STUPIDOS_VFS_S_IWUSR    0x0080
#define STUPIDOS_VFS_S_IXUSR    0x0040
#define STUPIDOS_VFS_S_IRGRP    0x0020
#define STUPIDOS_VFS_S_IWGRP    0x0010
#define STUPIDOS_VFS_S_IXGRP    0x0008
#define STUPIDOS_VFS_S_IROTH    0x0004
#define STUPIDOS_VFS_S_IWOTH    0x0002
#define STUPIDOS_VFS_S_IXOTH    0x0001

#define STUPIDOS_ENOENT         45
#define STUPIDOS_EHOSTUNREACH   23
#define STUPIDOS_ETIMEDOUT      78

#define STUPIDOS_FUTEX_WAIT         0
#define STUPIDOS_FUTEX_WAKE         1
#define STUPIDOS_FUTEX_PRIVATE_FLAG 128

struct stupidos_dirent
{
    uint32_t ino;
    uint16_t mode;
    uint64_t size;
    int8_t name[256];
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

struct stupidos_utsname
{
    int8_t sysname[32];
    int8_t nodename[32];
    int8_t release[32];
    int8_t version[64];
    int8_t machine[32];
    int8_t domainname[32];
};

ssize_t u_read(int fd, void *buf, size_t len);
ssize_t u_write(int fd, const void *buf, size_t len);
int u_open(const int8_t *path, int flags);
int u_close(int fd);
int64_t u_lseek(int fd, int64_t offset, int whence);
int u_yield(void);
int u_getpid(void);
int64_t u_time(void);
int u_readdir(const int8_t *path, uint32_t index, struct stupidos_dirent *out);
int u_nettest(void);
int u_exec(const int8_t *path, int argc, const int8_t *argv[]);
int64_t u_netping(uint32_t target_ip, uint16_t icmp_seq, uint32_t timeout_ms);
int64_t u_waitpid(int32_t pid);
int64_t u_sleep_ms(uint32_t ms);
int64_t u_netcfg(uint32_t ipv4, uint32_t netmask, uint32_t gateway);
int64_t u_chdir(const int8_t *path);
int64_t u_getcwd(int8_t *buf, size_t len);
int u_stat(const int8_t *path, struct stupidos_stat *out);
int u_fstat(int fd, struct stupidos_stat *out);
int u_uname(struct stupidos_utsname *out);
int64_t u_gettimeofday(struct stupidos_timeval *out);
int u_isatty(int fd);
int u_dup2(int oldfd, int newfd);
void *u_brk(void *addr);
void *u_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off);
int u_munmap(void *addr, size_t len);
int u_mprotect(void *addr, size_t len, int prot);
int u_clock_gettime(int clockid, struct stupidos_timespec *out);
int u_nanosleep(const struct stupidos_timespec *req, struct stupidos_timespec *rem);
int u_getuid(void);
int u_getgid(void);
int u_geteuid(void);
int u_getegid(void);
int u_access(const int8_t *path, int mode);
int u_openat(int dirfd, const int8_t *path, int flags);
int u_fstatat(int dirfd, const int8_t *path, struct stupidos_stat *out);
ssize_t u_readlink(const int8_t *path, int8_t *buf, size_t len);
int u_ioctl(int fd, uint64_t request, void *argp);
int u_dup(int oldfd);
ssize_t u_readv(int fd, const struct stupidos_iovec *iov, int iovcnt);
ssize_t u_writev(int fd, const struct stupidos_iovec *iov, int iovcnt);
int u_gettid(void);
int u_getppid(void);
void u_exit_group(int code) __attribute__((noreturn));
ssize_t u_getrandom(void *buf, size_t len, uint32_t flags);
int u_set_tid_address(int *tidptr);
int u_rt_sigaction(int signum, const void *act, void *oldact, size_t sigsetsize);
int u_rt_sigprocmask(int how, const void *set, void *oldset, size_t sigsetsize);
int u_sigaltstack(const void *ss, void *old_ss);
int64_t u_futex(uint32_t *uaddr, int op, uint32_t val, const void *timeout, uint32_t *uaddr2, uint32_t val3);
ssize_t u_pread64(int fd, void *buf, size_t len, uint64_t off);
ssize_t u_pwrite64(int fd, const void *buf, size_t len, uint64_t off);
int u_fcntl(int fd, int cmd, uint64_t arg);
int u_sched_getaffinity(int pid, size_t cpusetsize, void *mask);
int u_sysinfo(struct stupidos_sysinfo *info);
int u_prlimit64(int pid, int resource, const struct stupidos_rlimit *new_limit, struct stupidos_rlimit *old_limit);
void u_exit(int code) __attribute__((noreturn));

size_t u_strlen(const int8_t *str);
size_t u_strnlen(const int8_t *str, size_t max_len);
int u_strcmp(const int8_t *a, const int8_t *b);
void *u_memcpy(void *dst, const void *src, size_t len);
void *u_memset(void *dst, int value, size_t len);

void u_puts(const int8_t *str);
void u_putsn(const int8_t *str, size_t len);
void u_putc(int8_t ch);

#endif
