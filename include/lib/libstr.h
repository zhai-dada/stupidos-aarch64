#ifndef __LIBSTR_H__
#define __LIBSTR_H__

#include "asm/types.h"

size_t strlen(int8_t *str);
size_t strcmp(const int8_t *str1, const int8_t *str2);
size_t strncmp(const int8_t *str1, const int8_t *str2, size_t n);
int32_t memcmp(const void *s1, const void *s2, size_t n);

#endif
