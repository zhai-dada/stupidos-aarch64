#include "driver/uart.h"
#include "lib/libmem.h"

int8_t stack[40960];
int8_t buffer[40960];

int kernel_main(void)
{
    int a = 0;
    uart_init();
    uart_printf(UART_ATTR_FRONT_GREEN, UART_ATTR_BACK_BLACK, "%s %#018lx %d\n", "Hello", 1024, 4096);
    memset(buffer, 0xff, 511);
    for(uint32_t i = 0; i < 512; i++)
    {
        uart_printf(UART_ATTR_FRONT_GREEN, UART_ATTR_BACK_BLACK, "%04x\n", buffer[i]);
    }    
    while(1)
    {
        a++;
    }
}