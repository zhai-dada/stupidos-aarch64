#ifndef __STUPIDOS_SYS_TYPES_H__
#define __STUPIDOS_SYS_TYPES_H__

#include_next <sys/types.h>

/*
 * 直接沿用工具链的 POSIX 类型定义，避免和 CPython 的 pyconfig.h
 * 里那些探测结果发生 typedef 冲突。
 */

#if defined(__TINYC__)
/*
 * tinycc 在 guest 内编译自身目标程序时，系统头探测并不总是完整可用。
 * 这里补一组最小但稳定的类型定义，供 unistd.h / pwd.h / grp.h / stat.h
 * 这类头文件使用。
 */
#ifndef __stubidos_posix_types_defined
#define __stubidos_posix_types_defined 1
#ifndef __uid_t_defined
typedef unsigned int uid_t;
#define __uid_t_defined 1
#endif
#ifndef __gid_t_defined
typedef unsigned int gid_t;
#define __gid_t_defined 1
#endif
#ifndef __mode_t_defined
typedef unsigned int mode_t;
#define __mode_t_defined 1
#endif
#ifndef __ino_t_defined
typedef unsigned long ino_t;
#define __ino_t_defined 1
#endif
#ifndef __nlink_t_defined
typedef unsigned long nlink_t;
#define __nlink_t_defined 1
#endif
#ifndef __off_t_defined
typedef long off_t;
#define __off_t_defined 1
#endif
#ifndef __blksize_t_defined
typedef long blksize_t;
#define __blksize_t_defined 1
#endif
#ifndef __blkcnt_t_defined
typedef long blkcnt_t;
#define __blkcnt_t_defined 1
#endif
#ifndef __ssize_t_defined
typedef long ssize_t;
#define __ssize_t_defined 1
#endif
#endif
#endif

#endif
