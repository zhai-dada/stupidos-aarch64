#include "driver/uart.h"
#include "lib/librw.h"
#include "lib/libasm.h"
#include "lib/libirq.h"
#include "fdt.h"
#include "gicv2.h"
#include "spinlock.h"
#include "tty.h"
#include "debug.h"

static spinlock_t uart_log_lock = SPINLOCK_INIT;
static uint32_t uart_irq_line = UART_IRQ;
static bool uart_quiet_mode;

static int8_t uart_ascii_lower(int8_t ch)
{
    if (ch >= 'A' && ch <= 'Z')
    {
        return (int8_t)(ch - 'A' + 'a');
    }

    return ch;
}

static bool uart_contains_ci(const int8_t *haystack, const int8_t *needle)
{
    size_t i;
    size_t j;

    if (!haystack || !needle || !needle[0])
    {
        return false;
    }

    for (i = 0; haystack[i] != '\0'; i++)
    {
        for (j = 0; needle[j] != '\0'; j++)
        {
            if (haystack[i + j] == '\0')
            {
                return false;
            }

            if (uart_ascii_lower(haystack[i + j]) != uart_ascii_lower(needle[j]))
            {
                break;
            }
        }

        if (needle[j] == '\0')
        {
            return true;
        }
    }

    return false;
}

static bool uart_log_is_important(const int8_t *fmt)
{
    /*
     * 用户态 shell 已经接管之后，串口日志默认应尽量安静。
     * 这里保留真正有意义的错误信息，避免把终端重新刷满。
     */
    return uart_contains_ci(fmt, (const int8_t *)"error") ||
           uart_contains_ci(fmt, (const int8_t *)"failed") ||
           uart_contains_ci(fmt, (const int8_t *)"fail") ||
           uart_contains_ci(fmt, (const int8_t *)"fault") ||
           uart_contains_ci(fmt, (const int8_t *)"panic") ||
           uart_contains_ci(fmt, (const int8_t *)"abort") ||
           uart_contains_ci(fmt, (const int8_t *)"timeout") ||
           uart_contains_ci(fmt, (const int8_t *)"Unhandled");
}

static uint32_t uart_detect_irq_line(void)
{
    const struct fdt_device_desc *uart_dev;

    uart_dev = fdt_find_device_by_kind(FDT_DEVICE_UART);
    if (!uart_dev)
    {
        return UART_IRQ;
    }

    if (uart_dev->has_irq)
    {
        return uart_dev->irq;
    }

    return UART_IRQ;
}

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

void uart_set_quiet(bool quiet)
{
    uart_quiet_mode = quiet;
}

bool uart_is_quiet(void)
{
    return uart_quiet_mode;
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

int32_t uart_try_getc(void)
{
    if (get32(PL011_UART0_BASE + UART_FR) & UART_FR_RXFE)
    {
        return -1;
    }

    return (int32_t)(get32(PL011_UART0_BASE + UART_DR) & 0xffU);
}

void early_uart_init(void)
{
    // Init the uart

    /* Clear all errors */
    *(uint32_t *)(PL011_UART0_BASE + UART_RSR_ECR) = 0;

    /* Disable everything */
    *(uint32_t *)(PL011_UART0_BASE + UART_CR) = 0;

    /*
     * 开启 FIFO（中文）：
     * 之前 FEN=0 时，RX 基本只有 1 字节深度，输入稍快就容易丢字符；
     * shell 看起来就像“按键没反应/只进了前几个字母”。
     */
    *(uint32_t *)(PL011_UART0_BASE + UART_LCR_H) = UART_LCRH_WLEN_8 | UART_LCRH_FEN;

    /* 真正的使能位在 CR 里，不是在 LCR_H 里。 */
    *(uint32_t *)(PL011_UART0_BASE + UART_CR) = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
    
    printk("[early_uart\tinit]: init ok\n");
    return;
}

void uart_init(void)
{
    uint32_t imsc;

    // Init the uart

    /* Clear all errors */
    *(uint32_t *)(PL011_UART0_BASE + UART_RSR_ECR) = 0;

    /* Disable everything */
    *(uint32_t *)(PL011_UART0_BASE + UART_CR) = 0;

    /*
     * 同 early_uart_init，正式驱动阶段也保持 FIFO 打开，
     * 把突发输入的抗抖能力做起来，减少串口字符丢失。
     */
    *(uint32_t *)(PL011_UART0_BASE + UART_LCR_H) = UART_LCRH_WLEN_8 | UART_LCRH_FEN;

    /* 清掉残留中断，避免启动后立刻吃到旧状态。 */
    *(uint32_t *)(PL011_UART0_BASE + UART_ICR) = 0x7ff;

    /* Enable Interrupts*/
    *(uint32_t *)(PL011_UART0_BASE + UART_IMSC) = UART_IMSC_RTIM | UART_IMSC_RXIM;

    /* 真正的使能位在 CR 里，不是在 LCR_H 里。 */
    *(uint32_t *)(PL011_UART0_BASE + UART_CR) = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;

    uart_irq_line = uart_detect_irq_line();
    irq_handlers[uart_irq_line] = uart_irq_handle;
    gic_enable_irq(uart_irq_line);

    imsc = get32(PL011_UART0_BASE + UART_IMSC);
    printk("[uart\tinit]: irq=%u gic=%u imsc=%#x cr=%#x init ok\n",
           uart_irq_line,
           gic_irq_is_enabled(uart_irq_line),
           imsc,
           get32(PL011_UART0_BASE + UART_CR));
    return;
}

int32_t uart_printf(int8_t* front, int8_t* back, const int8_t* fmt, ...)
{
    /*
     * 串口日志是全局串行的，所以这里完全可以用共享静态缓冲，
     * 避免每次 printk 都在当前任务栈上再压一个 2KB 大对象。
     * 这能显著降低 shell / IRQ / 调度混跑时的栈污染风险。
     */
    static int8_t buffer[4096];
    int32_t i = 0;
    uint64_t daif;

	va_list args;
	va_start(args, fmt);
    i = vsprintf(buffer, fmt, args);
    va_end(args);

    if (uart_quiet_mode && !uart_log_is_important(buffer) && !uart_log_is_important(fmt))
    {
        return i;
    }

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
    uint8_t rx_batch[64];
    size_t rx_count;

    rx_count = 0;
    while (1)
    {
        fr = get32(PL011_UART0_BASE + UART_FR);
        if (fr & UART_FR_RXFE)
        {
            break;
        }

        rx_batch[rx_count++] = (uint8_t)get32(PL011_UART0_BASE + UART_DR);
        if (rx_count == sizeof(rx_batch))
        {
            /*
             * 批量喂给 tty（中文）：
             * 旧路径每个字节都要走一次关中断/加锁/回显，
             * 在串口突发输入时会明显拉高中断处理时延。
             */
            tty_feed_bytes(rx_batch, rx_count);
            rx_count = 0;
        }
    }

    if (rx_count)
    {
        tty_feed_bytes(rx_batch, rx_count);
    }

    /* 清掉本次收包相关中断，避免 UART 继续挂着未处理状态。 */
    *(uint32_t *)(PL011_UART0_BASE + UART_ICR) = 0x7ff;
}
