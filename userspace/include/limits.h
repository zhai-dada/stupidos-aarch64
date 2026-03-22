#ifndef __STUPIDOS_LIMITS_H__
#define __STUPIDOS_LIMITS_H__

#include_next <limits.h>

/*
 * 让标准 C 库的极限宏优先生效，再补充 stupidos 额外需要的值。
 * 这样 CPython 的 UCHAR_MAX / PATH_MAX 探测不会被我们截断。
 */

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

#endif
