#ifndef __STUPIDOS_USER_ASM_TYPES_H__
#define __STUPIDOS_USER_ASM_TYPES_H__

#include_next <asm/types.h>

/*
 * 用户态优先使用交叉工具链自带的 Linux ABI 类型。
 * 这样 glibc / CPython 看到的 __u8/__u16/__u32/__u64 不会被我们
 * 内核私有的 include/asm/types.h 污染。
 */

#endif
