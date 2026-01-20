#include "driver/uart.h"
#include "lib/librw.h"

void uart_send_string(uint8_t *str)
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
    volatile uint32_t *base_rsr_ecr = (uint32_t *)(PL011_UART0_BASE + UART_RSR_ECR);
    volatile uint32_t *base_cr = (uint32_t *)(PL011_UART0_BASE + UART_CR);
    volatile uint32_t *base_lcr_h = (uint32_t *)(PL011_UART0_BASE + UART_LCR_H);

    // Init the uart
    /* Clear all errors */
    *base_rsr_ecr = 0;
    /* Disable everything */
    *base_cr = 0;
    /* Configure TX to 8 bits, 1 stop bit, no parity, fifo disabled. */
    *base_lcr_h = UART_LCRH_WLEN_8;
    /* Enable UART and RX/TX */
    *base_cr = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
}
