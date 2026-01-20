#include "driver/uart.h"

char stack[4096];

int kernel_main(void)
{
    int a = 0;
    uart_init();
    uart_send_string((uint8_t*)"Hello\n");
    while(1)
    {
        a++;
    }
}