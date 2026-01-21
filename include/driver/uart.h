#ifndef __UART_H__
#define __UART_H__

#include "asm/types.h"
#include "stdarg.h"
#include "printk.h"

#define CONSOLE_BAUDRATE       115200
#define CONSOLE_UART_CLK_IN_HZ 24000000
#define UARTDR                 0x000
#define UARTFR                 0x018
#define UARTFR_TXFF            (1 << 5)
#define PL011_UART0_BASE       0x9000000
#define PL011_REG_SIZE         0x1000

#define UART_DR      0x00 /* data register */
#define UART_RSR_ECR 0x04 /* receive status or error clear */
#define UART_DMAWM   0x08 /* DMA watermark configure */
#define UART_TIMEOUT 0x0C /* Timeout period */
/* reserved space */
#define UART_FR    0x18 /* flag register */
#define UART_ILPR  0x20 /* IrDA low-poer */
#define UART_IBRD  0x24 /* integer baud register */
#define UART_FBRD  0x28 /* fractional baud register */
#define UART_LCR_H 0x2C /* line control register */
#define UART_CR    0x30 /* control register */
#define UART_IFLS  0x34 /* interrupt FIFO level select */
#define UART_IMSC  0x38 /* interrupt mask set/clear */
#define UART_RIS   0x3C /* raw interrupt register */
#define UART_MIS   0x40 /* masked interrupt register */
#define UART_ICR   0x44 /* interrupt clear register */
#define UART_DMACR 0x48 /* DMA control register */

/* flag register bits */
#define UART_FR_RTXDIS (1 << 13)
#define UART_FR_TERI   (1 << 12)
#define UART_FR_DDCD   (1 << 11)
#define UART_FR_DDSR   (1 << 10)
#define UART_FR_DCTS   (1 << 9)
#define UART_FR_RI     (1 << 8)
#define UART_FR_TXFE   (1 << 7)
#define UART_FR_RXFF   (1 << 6)
#define UART_FR_TXFF   (1 << 5)
#define UART_FR_RXFE   (1 << 4)
#define UART_FR_BUSY   (1 << 3)
#define UART_FR_DCD    (1 << 2)
#define UART_FR_DSR    (1 << 1)
#define UART_FR_CTS    (1 << 0)

/* transmit/receive line register bits */
#define UART_LCRH_SPS    (1 << 7)
#define UART_LCRH_WLEN_8 (3 << 5)
#define UART_LCRH_WLEN_7 (2 << 5)
#define UART_LCRH_WLEN_6 (1 << 5)
#define UART_LCRH_WLEN_5 (0 << 5)
#define UART_LCRH_FEN    (1 << 4)
#define UART_LCRH_STP2   (1 << 3)
#define UART_LCRH_EPS    (1 << 2)
#define UART_LCRH_PEN    (1 << 1)
#define UART_LCRH_BRK    (1 << 0)

/* control register bits */
#define UART_CR_CTSEN   (1 << 15)
#define UART_CR_RTSEN   (1 << 14)
#define UART_CR_OUT2    (1 << 13)
#define UART_CR_OUT1    (1 << 12)
#define UART_CR_RTS     (1 << 11)
#define UART_CR_DTR     (1 << 10)
#define UART_CR_RXE     (1 << 9)
#define UART_CR_TXE     (1 << 8)
#define UART_CR_LPE     (1 << 7)
#define UART_CR_OVSFACT (1 << 3)
#define UART_CR_UARTEN  (1 << 0)

#define UART_IMSC_RTIM (1 << 6)
#define UART_IMSC_RXIM (1 << 4)

#define UART_ATTR_RESET           "\x1b[0m"
#define UART_ATTR_FRONT_BLACK     "\x1b[30m"
#define UART_ATTR_FRONT_RED       "\x1b[31m"
#define UART_ATTR_FRONT_GREEN     "\x1b[32m"
#define UART_ATTR_FRONT_YELLOW    "\x1b[33m"
#define UART_ATTR_FRONT_BLUE      "\x1b[34m"
#define UART_ATTR_FRONT_MAGENTA   "\x1b[35m"
#define UART_ATTR_FRONT_CYAN      "\x1b[36m"
#define UART_ATTR_FRONT_WHITE     "\x1b[37m"

#define UART_ATTR_BACK_BLACK      "\x1b[40m"
#define UART_ATTR_BACK_RED        "\x1b[41m"
#define UART_ATTR_BACK_GREEN      "\x1b[42m"
#define UART_ATTR_BACK_YELLOW     "\x1b[43m"
#define UART_ATTR_BACK_BLUE       "\x1b[44m"
#define UART_ATTR_BACK_MAGENTA    "\x1b[45m"
#define UART_ATTR_BACK_CYAN       "\x1b[46m"
#define UART_ATTR_BACK_WHITE      "\x1b[47m"

void uart_init(void);
void uart_putc(uint8_t ch);
void uart_send_string(int8_t *str);
int32_t uart_printf(int8_t* front, int8_t* back, const int8_t* fmt, ...);

#endif
