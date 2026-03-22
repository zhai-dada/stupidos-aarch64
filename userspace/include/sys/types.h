#ifndef __STUPIDOS_SYS_TYPES_H__
#define __STUPIDOS_SYS_TYPES_H__

#include_next <sys/types.h>

/*
 * 直接沿用工具链的 POSIX 类型定义，避免和 CPython 的 pyconfig.h
 * 里那些探测结果发生 typedef 冲突。
 */

#endif
