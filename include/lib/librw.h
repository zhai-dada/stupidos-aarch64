#ifndef __LIBRW_H__
#define __LIBRW_H__

#include "asm/types.h"

extern uint32_t get32(uint64_t addr);
extern uint64_t get64(uint64_t addr);

extern void put32(uint64_t addr, uint32_t val);
extern void put64(uint64_t addr, uint64_t val);

#endif
