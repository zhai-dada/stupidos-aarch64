#ifndef __DEBUG_H__
#define __DEBUG_H__

#define CONFIG_DEBUG_ENABLE

#include "driver/uart.h"

#ifdef CONFIG_DEBUG_ENABLE
#define debug(fmt, arg...)     \
    uart_printf(UART_ATTR_BACK_BLACK, UART_ATTR_FRONT_GREEN, fmt, ##arg)
#else
#define debug(fmt, arg...)
#endif

#endif
