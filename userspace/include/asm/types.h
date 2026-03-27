#ifndef __STUPIDOS_USER_ASM_TYPES_H__
#define __STUPIDOS_USER_ASM_TYPES_H__

/*
 * 用户态专用的 Linux ABI 类型层。
 *
 * 这个头文件不能再转去包含内核自己的 include/asm/types.h，
 * 否则 third_party 代码在交叉编译时会和 glibc 的内核头发生 typedef 冲突。
 *
 * 这里仅提供 Linux 内核/工具链常见的 __u* / __s* / __be* / __le*
 * 命名空间类型，避免污染标准 C 的 uint*_t / int*_t。
 */

typedef unsigned char       __u8;
typedef unsigned short      __u16;
typedef unsigned int        __u32;
typedef unsigned long long  __u64;

typedef signed char         __s8;
typedef short               __s16;
typedef int                 __s32;
typedef long long           __s64;

typedef __u16               __be16;
typedef __u32               __be32;
typedef __u64               __be64;
typedef __u16               __le16;
typedef __u32               __le32;
typedef __u64               __le64;

#endif
