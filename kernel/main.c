#include "driver/uart.h"
#include "lib/libmem.h"
#include "lib/libasm.h"
#include "lib/libirq.h"
#include "debug.h"
#include "timer.h"
#include "gicv2.h"
#include "pt_regs.h"
#include "mmu.h"
#include "assert.h"

int8_t stack[40960];

extern uint64_t __bss_start, __bss_end;

int kernel_main(void)
{
    memset((int8_t*)&__bss_start, 0, (&__bss_end - &__bss_start));
    
    early_uart_init();
    
    disable_irq();

    mmu_init();
    
    gic_init();

    uart_init();
    
    timer_init();
    
    enable_irq();

    debug("Hello debug\n");
    assert(5 > 6);
    
    while(1)
    {
        // uart_putc(uart_getc());
    }
}