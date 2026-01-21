#ifndef __LIBMEM_H__
#define __LIBMEM_H__

#include "asm/types.h"

void *__memset_16bytes(void *s, uint64_t val, size_t count);
void *__memset_1bytes(void *s, uint32_t c, size_t count);
void* memset(int8_t *s, int32_t c, size_t count);

#endif
