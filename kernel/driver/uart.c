#include "driver/uart.h"
#include "lib/librw.h"
#include "lib/libasm.h"
#include "lib/libirq.h"
#include "spinlock.h"
#include "debug.h"

static spinlock_t uart_log_lock = SPINLOCK_INIT;
static spinlock_t uart_rx_lock = SPINLOCK_INIT;
#define UART_RX_BUF_SIZE 256
static uint8_t uart_rx_buf[UART_RX_BUF_SIZE];
static uint32_t uart_rx_head;
static uint32_t uart_rx_tail;

static void uart_send_string_raw(int8_t *str)
{
    for (int32_t i = 0; str[i] != '\0'; i++)
    {
        uart_putc((int8_t)str[i]);
    }
}

static void uart_rx_push(uint8_t ch)
{
    uint32_t next;

    next = (uart_rx_head + 1U) % UART_RX_BUF_SIZE;
    if (next == uart_rx_tail)
    {
        /*
         * 缓冲区满时宁可丢弃新字节，也不要阻塞中断处理。
         * shell 的输入速率远低于这个深度，正常交互不会触发这里。
         */
        return;
    }

    uart_rx_buf[uart_rx_head] = ch;
    uart_rx_head = next;
}

static int32_t uart_rx_pop(void)
{
    int32_t ch;

    if (uart_rx_tail == uart_rx_head)
    {
        return -1;
    }

    ch = uart_rx_buf[uart_rx_tail];
    uart_rx_tail = (uart_rx_tail + 1U) % UART_RX_BUF_SIZE;
    return ch;
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
    uint64_t daif;
    int32_t ch;

    while (1)
    {
        daif = read_daif();
        disable_irq();
        spin_lock(&uart_rx_lock);
        ch = uart_rx_pop();
        spin_unlock(&uart_rx_lock);
        write_daif(daif);

        if (ch >= 0)
        {
            return ch;
        }

        /*
         * 没有输入时进入低功耗等待。
         * 后续 UART 中断一到，会把字节塞入缓冲区并通过 event 唤醒这里。
         */
        asm volatile("wfe" : : : "memory");
    }
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

    /* Enable UART and RX/TX */
    *(uint32_t *)(PL011_UART0_BASE + UART_LCR_H) = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
    
    printk("[early_uart\tinit]: init ok\n");
    return;
}

void uart_init(void)
{
    // Init the uart
    spin_lock_init(&uart_rx_lock);
    uart_rx_head = 0;
    uart_rx_tail = 0;

    /* Clear all errors */
    *(uint32_t *)(PL011_UART0_BASE + UART_RSR_ECR) = 0;

    /* Disable everything */
    *(uint32_t *)(PL011_UART0_BASE + UART_CR) = 0;

    /* Configure TX to 8 bits, 1 stop bit, no parity, fifo disabled. */
    *(uint32_t *)(PL011_UART0_BASE + UART_LCR_H) = UART_LCRH_WLEN_8;

    /* Enable Interrupts*/
    *(uint32_t *)(PL011_UART0_BASE + UART_IMSC) = UART_IMSC_RTIM | UART_IMSC_RXIM;

    /* Enable UART and RX/TX */
    *(uint32_t *)(PL011_UART0_BASE + UART_LCR_H) = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
    
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

    while (1)
    {
        fr = get32(PL011_UART0_BASE + UART_FR);
        if (fr & UART_FR_RXFE)
        {
            break;
        }

        ch = (uint8_t)get32(PL011_UART0_BASE + UART_DR);
        spin_lock(&uart_rx_lock);
        uart_rx_push(ch);
        spin_unlock(&uart_rx_lock);
    }
}
