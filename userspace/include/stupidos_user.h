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

#define STUPIDOS_STDIN_FILENO   0
#define STUPIDOS_STDOUT_FILENO  1
#define STUPIDOS_STDERR_FILENO  2

#define STUPIDOS_O_RDONLY       0x1
#define STUPIDOS_O_WRONLY       0x2
#define STUPIDOS_O_RDWR         (STUPIDOS_O_RDONLY | STUPIDOS_O_WRONLY)

#define STUPIDOS_SEEK_SET       0
#define STUPIDOS_SEEK_CUR       1
#define STUPIDOS_SEEK_END       2

#define STUPIDOS_VFS_S_IFDIR    0x4000

#define STUPIDOS_ENOENT         45
#define STUPIDOS_EHOSTUNREACH   23
#define STUPIDOS_ETIMEDOUT      78

struct stupidos_dirent
{
    uint32_t ino;
    uint16_t mode;
    uint64_t size;
    int8_t name[256];
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
