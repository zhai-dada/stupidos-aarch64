#ifndef __STUPIDOS_UNISTD_H__
#define __STUPIDOS_UNISTD_H__

#include_next <unistd.h>

/*
 * 用户态程序直接看到标准 POSIX 接口；额外的 stupidos 系统调用包装
 * 由 compat.c 中的同名实现提供，不再在这里手工重定义基础类型。
 */

int chroot(const char *path);

#endif
