#include "lib/libirq.h"
#include "lib/libasm.h"

void disable_irq(void)
{
    uint64_t val = read_daif();
    val |= (DAIF_I);
    write_daif(val);
    return;
}

void enable_irq(void)
{
    uint64_t val = read_daif();
    val &= (~(DAIF_I));
    write_daif(val);
    return;
}