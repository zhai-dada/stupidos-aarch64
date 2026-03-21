#include "driver/uart.h"
#include "lib/librw.h"
#include "lib/libasm.h"
#include "lib/libirq.h"
#include "spinlock.h"
#include "tty.h"
#include "debug.h"

static spinlock_t uart_log_lock = SPINLOCK_INIT;

static void uart_send_string_raw(int8_t *str)
{
    for (int32_t i = 0; str[i] != '\0'; i++)
    {
        uart_putc((int8_t)str[i]);
    }
}

void uart_send_string(int8_t *str)
{
    uint64_t daif;

    if (!str)
    {
        return;
    }

    daif = read_daif();
    disable_irq();
    spin_lock(&uart_log_lock);
    uart_send_string_raw(str);
    spin_unlock(&uart_log_lock);
    write_daif(daif);
}

void uart_putc(uint8_t ch)
{
    /* Wait until there is space in the FIFO or device is disabled */
    while (get32(PL011_UART0_BASE + UART_FR) & UART_FR_TXFF)
    {
        nop();
    }
    /* Send the character */
    put32((PL011_UART0_BASE + UART_DR), (uint32_t)ch);
}

int32_t uart_getc(void)
{
    return tty_getc();
}

void early_uart_init(void)
{
    // Init the uart

    /* Clear all errors */
    *(uint32_t *)(PL011_UART0_BASE + UART_RSR_ECR) = 0;

    /* Disable everything */
    *(uint32_t *)(PL011_UART0_BASE + UART_CR) = 0;

    /* Configure TX to 8 bits, 1 stop bit, no parity, fifo disabled. */
    *(uint32_t *)(PL011_UART0_BASE + UART_LCR_H) = UART_LCRH_WLEN_8;

    /* 真正的使能位在 CR 里，不是在 LCR_H 里。 */
    *(uint32_t *)(PL011_UART0_BASE + UART_CR) = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
    
    printk("[early_uart\tinit]: init ok\n");
    return;
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

    /* 清掉残留中断，避免启动后立刻吃到旧状态。 */
    *(uint32_t *)(PL011_UART0_BASE + UART_ICR) = 0x7ff;

    /* Enable Interrupts*/
    *(uint32_t *)(PL011_UART0_BASE + UART_IMSC) = UART_IMSC_RTIM | UART_IMSC_RXIM;

    /* 真正的使能位在 CR 里，不是在 LCR_H 里。 */
    *(uint32_t *)(PL011_UART0_BASE + UART_CR) = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
    
    irq_handlers[UART_IRQ] = uart_irq_handle;
    gic_enable_irq(UART_IRQ);
    
    printk("[uart\tinit]: init ok\n");
    return;
}

int32_t uart_printf(int8_t* front, int8_t* back, const int8_t* fmt, ...)
{
    int8_t buffer[2048];
    int32_t i = 0;
    uint64_t daif;

	va_list args;
	va_start(args, fmt);
	i = vsprintf(buffer, fmt, args);
	va_end(args);

    /*
     * 多核环境下串口是全局共享资源。
     * 这里同时关本地中断并持有全局日志锁，避免：
     * 1. 不同 CPU 的 printk 互相打断
     * 2. 同一 CPU 在持锁期间被中断后再次 printk 造成自锁
     */
    daif = read_daif();
    disable_irq();
    spin_lock(&uart_log_lock);

    uart_send_string_raw((int8_t*)front);
    uart_send_string_raw((int8_t*)back);
    uart_send_string_raw((int8_t*)buffer);
    uart_send_string_raw((int8_t*)UART_ATTR_RESET);

    spin_unlock(&uart_log_lock);
    write_daif(daif);

    return i;
}

void uart_irq_handle(void)
{
    uint32_t fr;
    uint8_t ch;
printk("====\n");
    while (1)
    {
        fr = get32(PL011_UART0_BASE + UART_FR);
        if (fr & UART_FR_RXFE)
        {
            break;
        }

        ch = (uint8_t)get32(PL011_UART0_BASE + UART_DR);
        tty_feed_char(ch);
    }

    /* 清掉本次收包相关中断，避免 UART 继续挂着未处理状态。 */
    *(uint32_t *)(PL011_UART0_BASE + UART_ICR) = UART_IMSC_RTIM | UART_IMSC_RXIM;
}
