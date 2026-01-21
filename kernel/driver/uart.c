#include "driver/uart.h"
#include "lib/librw.h"

void uart_send_string(int8_t *str)
{
    for (int i = 0; str[i] != '\0'; i++)
    {
        uart_putc((char)str[i]);
    }
}

void uart_putc(uint8_t ch)
{
    /* Wait until there is space in the FIFO or device is disabled */
    while (get32(PL011_UART0_BASE + UART_FR) & UART_FR_TXFF)
    {
        ;
    }
    /* Send the character */
    put32((PL011_UART0_BASE + UART_DR), (uint32_t)ch);
}

void uart_init(void)
{
    // Init the uart
    /* Clear all errors */
    *(uint32_t *)(PL011_UART0_BASE + UART_RSR_ECR) = 0;
    /* Disable everything */
    *(uint32_t *)(PL011_UART0_BASE + UART_CR) = 0;
    /* Configure TX to 8 bits, 1 stop bit, no parity, fifo disabled. */
    *(uint32_t *)(PL011_UART0_BASE + UART_LCR_H) = UART_LCRH_WLEN_8;
    /* Enable UART and RX/TX */
    *(uint32_t *)(PL011_UART0_BASE + UART_LCR_H) = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
}

int32_t uart_printf(int8_t* front, int8_t* back, const int8_t* fmt, ...)
{
    int8_t buffer[512];
    int32_t i = 0;

	va_list args;
	va_start(args, fmt);
	i = vsprintf(buffer, fmt, args);
	va_end(args);

    uart_send_string((int8_t*)front);
    uart_send_string((int8_t*)back);
    uart_send_string((int8_t*)buffer);
    uart_send_string((int8_t*)UART_ATTR_RESET);

    return i;
}

