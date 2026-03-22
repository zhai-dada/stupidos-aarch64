#ifndef __LIBASM_H__
#define __LIBASM_H__

#include "asm/types.h"

#define read_sysreg(reg)                            \
({                                                  \
		uint64_t _val;                              \
		asm volatile                                \
        (                                           \
            "mrs %0," #reg                          \
		    : "=rZ"(_val)                           \
        );                                          \
		_val;                                       \
})

#define write_sysreg(val, reg)                      \
({                                                  \
		uint64_t _val = (uint64_t)val;              \
		asm volatile                                \
        (                                           \
            "msr " #reg ", %x0"                     \
		    :                                       \
            :"rZ"(_val)                             \
        );                                          \
})

#define nop()       asm volatile ("nop":::"memory")
#define wfe()       asm volatile ("wfe":::"memory")
#define sev()       asm volatile ("sev":::"memory")

uint64_t read_daif(void);
void write_daif(uint64_t val);
uint64_t read_mpidr(void);
uint32_t arch_curr_cpu_id(void);

#endif
