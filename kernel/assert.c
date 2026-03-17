#include "assert.h"
#include "driver/uart.h"
#include "asm/asm.h"

void assert_failure(int8_t* exp, int8_t* file, int8_t* base, const int8_t* func, int32_t line)
{
    uart_printf(UART_ATTR_FRONT_RED, UART_ATTR_BACK_BLACK, "ASSERT : ");
    uart_printf(UART_ATTR_FRONT_RED, UART_ATTR_BACK_BLACK, "%s----->file:%s\tbase_file:%s\tfunc:%s\tline:%d\n", exp, file, base, func, line);
    while(1)
    {
        nop();
    }
}