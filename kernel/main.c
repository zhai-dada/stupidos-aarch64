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
#include "driver/ramfb.h"
#include "driver/fwcfg.h"

extern uint8_t framebuffer[FB_WIDTH * FB_HEIGHT * FB_BPP];

extern uint64_t __bss_start, __bss_end;
extern uint64_t __kernel_start, __kernel_end;

int32_t kernel_main(void)
{
    memset((int8_t *)&__bss_start, 0, (&__bss_end - &__bss_start));

    early_uart_init();

    disable_irq();

    mmu_init();
    
    gic_init();

    uart_init();

    timer_init();

    enable_irq();

    ramfb_init((uint8_t *)framebuffer, FB_WIDTH, FB_HEIGHT);

    ramfb_putstring(COLOR_BLACK, COLOR_WHITE, (uint8_t*)"Hello World\n\btest");

    assert(6 > 5);

    while (1)
    {
        // uart_putc(uart_getc());
    }
}