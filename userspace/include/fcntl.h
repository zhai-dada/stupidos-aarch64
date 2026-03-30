#ifndef __STUPIDOS_FCNTL_H__
#define __STUPIDOS_FCNTL_H__

#include "stupidos_user.h"

#if defined(__TINYC__)
/*
 * TinyCC 运行在 guest 内时通常没有“下一层系统头文件”可 include_next。
 * 这里提供最小 fcntl 兼容集，满足 open/openat + O_* 的常见移植需求。
 *
 * 注意：
 * 这些 O_* 常量必须保持“POSIX 语义值”，由 compat 层再翻译到 STUPIDOS_O_*。
 * 不能直接填 STUPIDOS_O_*，否则 O_CREAT 等位会被 compat 误判，导致创建失败。
 */
#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#ifndef O_WRONLY
#define O_WRONLY 1
#endif
#ifndef O_RDWR
#define O_RDWR 2
#endif
#ifndef O_ACCMODE
#define O_ACCMODE 3
#endif
#ifndef O_CREAT
#define O_CREAT 0x40
#endif
#ifndef O_TRUNC
#define O_TRUNC 0x200
#endif
#ifndef O_APPEND
#define O_APPEND 0x400
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x800
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0x80000
#endif
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif

int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int fcntl(int fd, int cmd, ...);
#else
#include_next <fcntl.h>
#endif

#endif
