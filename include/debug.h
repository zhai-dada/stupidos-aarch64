#ifndef __DEBUG_H__
#define __DEBUG_H__

#define DEBUG_ENABLE 1

#include "driver/uart.h"

#if DEBUG_ENABLE
#define printk(fmt, arg...)     \
    uart_printf(UART_ATTR_BACK_BLACK, UART_ATTR_FRONT_GREEN, fmt, ##arg)
#else
#define printk(fmt, arg...)
#endif

#endif
