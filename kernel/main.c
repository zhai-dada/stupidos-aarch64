#include "driver/uart.h"
#include "lib/libmem.h"
#include "lib/libasm.h"
#include "lib/libirq.h"
#include "debug.h"
#include "timer.h"
#include "gicv2.h"
#include "pt_regs.h"

int8_t stack[40960];

int kernel_main(void)
{
    uart_init();
    
    disable_irq();

    gic_init();

    timer_init();
    
    enable_irq();

    while(1)
    {
        uart_putc(uart_getc());
    }
}