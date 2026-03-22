#ifndef __STUPIDOS_ASSERT_H__
#define __STUPIDOS_ASSERT_H__

/*
 * 用户态采用标准 C 语义的 assert。
 * Python 的构建默认定义了 NDEBUG，所以这里直接让断言退化成空操作，
 * 避免把内核调试头再拉进来。
 */

#ifdef NDEBUG
#define assert(exp) ((void)0)
#else
#define assert(exp) ((exp) ? (void)0 : __builtin_trap())
#endif

#endif
