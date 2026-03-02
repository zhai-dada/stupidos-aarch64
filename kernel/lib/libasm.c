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
