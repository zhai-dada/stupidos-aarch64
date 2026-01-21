#include "driver/uart.h"

int8_t stack[40960];

int kernel_main(void)
{
    int a = 0;
    uart_init();
    uart_printf(UART_ATTR_FRONT_GREEN, UART_ATTR_BACK_BLACK, "%s %#018lx %d\n", "Hello", 1024, 4096);
    
    while(1)
    {
        a++;
    }
}