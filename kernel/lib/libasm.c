#include "lib/libasm.h"

uint64_t read_daif(void)
{
	return read_sysreg(daif);
}

void write_daif(uint64_t val)
{
	write_sysreg(val, daif);
    return;
}

uint64_t read_mpidr(void)
{
    return read_sysreg(mpidr_el1);
}

uint32_t arch_curr_cpu_id(void)
{
    return (uint32_t)(read_mpidr() & 0xffUL);
}
