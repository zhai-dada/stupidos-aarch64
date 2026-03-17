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

int kernel_main(void)
{
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