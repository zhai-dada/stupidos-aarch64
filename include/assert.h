#ifndef __ASSERT_H__
#define __ASSERT_H__

#define CONFIG_ASSERT

/*
 * 内核断言接口不需要依赖用户态的固定宽度类型。
 * 这里改成标准 C 字符串指针，避免用户态编译时把内核私有
 * asm/types.h 重新卷进来。
 */
void assert_failure(const char *exp, const char *file, const char *base,
                    const char *func, int line);

#ifndef CONFIG_ASSERT
#define assert(exp) ((void)0)
#else
#define assert(exp)                                                             \
    ((exp) ? (void)0 : assert_failure(#exp, __FILE__, __BASE_FILE__, __func__,  \
                                      __LINE__))


#endif

#endif
