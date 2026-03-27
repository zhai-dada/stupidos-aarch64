#ifndef __STUPIDOS_WCHAR_H__
#define __STUPIDOS_WCHAR_H__

#include <stddef.h>

/*
 * 关键说明（中文）：
 * CPython 头文件会先后包含 <stdio.h> 与 <wchar.h>。
 * 如果 wchar.h 落到工具链 libc，会重新定义 FILE，和我们自带 stdio.h 冲突。
 * 这里提供最小 wchar 接口声明，统一走 stupidos 的用户态 libc 头文件。
 */

typedef unsigned int wint_t;

typedef struct
{
    unsigned int __opaque;
} mbstate_t;

struct tm;

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

#ifndef WCHAR_MIN
#define WCHAR_MIN (-2147483647 - 1)
#endif

#ifndef WCHAR_MAX
#define WCHAR_MAX 2147483647
#endif

size_t wcslen(const wchar_t *s);
wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcscat(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **saveptr);
long wcstol(const wchar_t *nptr, wchar_t **endptr, int base);
size_t mbstowcs(wchar_t *dst, const char *src, size_t len);
size_t wcstombs(char *dst, const wchar_t *src, size_t len);
int wmemcmp(const wchar_t *lhs, const wchar_t *rhs, size_t n);
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n);
int wcscoll(const wchar_t *lhs, const wchar_t *rhs);
size_t wcsftime(wchar_t *s, size_t max, const wchar_t *fmt, const struct tm *tm);

#endif
