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

int8_t stack[40960];

#define FB_WIDTH 1280
#define FB_HEIGHT 800
#define FB_BPP 4 // RGB888
extern uint8_t framebuffer[FB_WIDTH * FB_HEIGHT * FB_BPP];

extern uint64_t __bss_start, __bss_end;

int32_t kernel_main(void)
{
    memset((int8_t*)&__bss_start, 0, (&__bss_end - &__bss_start));

    early_uart_init();
    
    disable_irq();

    mmu_init();
    
    gic_init();

    uart_init();
    
    timer_init();
    
    enable_irq();

    setup_ramfb((uint8_t*)framebuffer, 1280, 800);

    __memset_4bytes((void*)framebuffer, COLOR_GREEN, FB_WIDTH * FB_HEIGHT);

    assert(5 > 6);
    
    while(1)
    {
        // uart_putc(uart_getc());
    }
}