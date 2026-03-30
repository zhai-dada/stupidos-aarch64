#ifndef __STUPIDOS_UNISTD_H__
#define __STUPIDOS_UNISTD_H__

#include "stupidos_user.h"
#include <sys/types.h>
#include <sys/time.h>

#if defined(__TINYC__)
/*
 * TinyCC guest 环境下缺少完整系统头时，提供最小 unistd 原型集，
 * 让 read/write/close 这类最基础接口可以无告警编译。
 */
ssize_t read(int fd, void *buf, size_t len);
ssize_t write(int fd, const void *buf, size_t len);
int close(int fd);
int access(const char *path, int mode);
int unlink(const char *path);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int isatty(int fd);
int64_t lseek(int fd, int64_t offset, int whence);
unsigned int sleep(unsigned int seconds);
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
#else
#include_next <unistd.h>
#endif

/*
 * 用户态程序直接看到标准 POSIX 接口；额外的 stupidos 系统调用包装
 * 由 compat.c 中的同名实现提供，不再在这里手工重定义基础类型。
 */

int chroot(const char *path);
int link(const char *oldpath, const char *newpath);
int rename(const char *oldpath, const char *newpath);
int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
int unlinkat(int dirfd, const char *path, int flags);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);
char *ctermid(char *s);
int chflags(const char *path, unsigned long flags);
int lchflags(const char *path, unsigned long flags);
int killpg(pid_t pgrp, int sig);
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups);
int initgroups(const char *user, gid_t group);

#ifndef L_ctermid
#define L_ctermid 16
#endif

unsigned int alarm(unsigned int seconds);

#endif
