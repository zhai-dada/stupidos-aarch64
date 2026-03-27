#ifndef __STUPIDOS_STDINT_H__
#define __STUPIDOS_STDINT_H__

#include <stddef.h>

/*
 * 这个头文件同时服务两类编译环境：
 * 1) 主工程的交叉 GCC / glibc 头文件。
 * 2) guest 侧 TinyCC / 精简 libc 场景。
 *
 * 对于 GCC 交叉编译，优先采用工具链自带的 <stdint.h>，
 * 这样不会和 glibc / system headers 里的 typedef 发生冲突。
 * 对于 TinyCC，才回退到本地最小定义。
 */
#if defined(__TINYC__)
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;

typedef long intptr_t;
typedef unsigned long uintptr_t;

typedef long long intmax_t;
typedef unsigned long long uintmax_t;

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define UINT8_MAX  255U

#define INT16_MIN  (-32768)
#define INT16_MAX  32767
#define UINT16_MAX 65535U

#define INT32_MIN  (-2147483647 - 1)
#define INT32_MAX  2147483647
#define UINT32_MAX 4294967295U

#define INT64_MIN  (-9223372036854775807LL - 1LL)
#define INT64_MAX  9223372036854775807LL
#define UINT64_MAX 18446744073709551615ULL

#define INTPTR_MIN INT64_MIN
#define INTPTR_MAX INT64_MAX
#define UINTPTR_MAX UINT64_MAX

#define INTMAX_MIN INT64_MIN
#define INTMAX_MAX INT64_MAX
#define UINTMAX_MAX UINT64_MAX

#ifndef SIZE_MAX
#define SIZE_MAX UINTPTR_MAX
#endif
#else
#include_next <stdint.h>
#endif

#endif
