#ifndef __PAGE_ALLOC_H__
#define __PAGE_ALLOC_H__

#include "asm/types.h"

#define PAGE_ALLOC_MAX_ORDER 17

void page_alloc_init(void);
void *alloc_pages(uint32_t order);
uint64_t alloc_pages_phys(uint32_t order);
void free_pages(void *addr, uint32_t order);
void free_pages_phys(uint64_t phys, uint32_t order);
uint32_t page_alloc_free_pages(void);
uint32_t page_alloc_total_pages(void);

#endif
