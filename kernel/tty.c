#include "tty.h"

#include "driver/uart.h"
#include "lib/libasm.h"
#include "lib/libirq.h"
#include "sched.h"
#include "spinlock.h"

/*
 * tty 的输入缓冲。
 * UART IRQ 和 virtio-input IRQ 都会往这里塞字节，shell 只负责取字节。
 */
#define TTY_RX_BUF_SIZE 512

static spinlock_t tty_lock = SPINLOCK_INIT;
static uint8_t tty_rx_buf[TTY_RX_BUF_SIZE];
static uint32_t tty_rx_head;
static uint32_t tty_rx_tail;
static struct tty_mouse_state tty_mouse;

static void tty_rx_push(uint8_t ch)
{
    uint32_t next;

    next = (tty_rx_head + 1U) % TTY_RX_BUF_SIZE;
    if (next == tty_rx_tail)
    {
        /*
         * 缓冲区满时丢弃最新字符。
         * 输入速度远低于这个深度，正常交互不会触发。
         */
        return;
    }

    tty_rx_buf[tty_rx_head] = ch;
    tty_rx_head = next;
}

static int32_t tty_rx_pop(void)
{
    int32_t ch;

    if (tty_rx_tail == tty_rx_head)
    {
        return -1;
    }

    ch = tty_rx_buf[tty_rx_tail];
    tty_rx_tail = (tty_rx_tail + 1U) % TTY_RX_BUF_SIZE;
    return ch;
}

void tty_init(void)
{
    spin_lock_init(&tty_lock);
    tty_rx_head = 0;
    tty_rx_tail = 0;
    tty_mouse.x = 0;
    tty_mouse.y = 0;
    tty_mouse.buttons = 0;
}

void tty_putc(uint8_t ch)
{
    uart_putc(ch);
}

void tty_write(const int8_t *str)
{
    if (!str)
    {
        return;
    }

    while (*str)
    {
        tty_putc((uint8_t)*str++);
    }
}

void tty_feed_char(uint8_t ch)
{
    uint64_t daif;

    daif = read_daif();
    disable_irq();
    spin_lock(&tty_lock);
    tty_rx_push(ch);
    spin_unlock(&tty_lock);
    write_daif(daif);
}

int32_t tty_getc(void)
{
    uint64_t daif;
    int32_t ch;

    while (1)
    {
        daif = read_daif();
        disable_irq();
        spin_lock(&tty_lock);
        ch = tty_rx_pop();
        spin_unlock(&tty_lock);
        if (ch >= 0)
        {
            write_daif(daif);
            return ch;
        }

        /*
         * 串口输入优先走中断队列，但如果中断暂时没到，这里再直接轮询
         * 一次 PL011 RX FIFO，避免用户在 QEMU 里按键后 shell 一直无响应。
         * 这样既保留中断路径，也给早期调试留一条可靠兜底通道。
         */
        ch = uart_try_getc();
        if (ch >= 0)
        {
            write_daif(daif);
            return ch;
        }

        write_daif(daif);

        /*
         * 当前还没有做完整的等待队列和阻塞唤醒。
         * shell 线程在没有输入时主动让出 CPU，既不会空转卡死，
         * 也能让 boot 线程继续推进后面的驱动初始化。
         */
        sched_yield();
    }
}

void tty_report_mouse_delta(int32_t dx, int32_t dy, uint32_t buttons)
{
    uint64_t daif;

    daif = read_daif();
    disable_irq();
    spin_lock(&tty_lock);
    tty_mouse.x += dx;
    tty_mouse.y += dy;
    tty_mouse.buttons = buttons;
    spin_unlock(&tty_lock);
    write_daif(daif);
}

void tty_report_mouse_abs(int32_t x, int32_t y, uint32_t buttons)
{
    uint64_t daif;

    daif = read_daif();
    disable_irq();
    spin_lock(&tty_lock);
    tty_mouse.x = x;
    tty_mouse.y = y;
    tty_mouse.buttons = buttons;
    spin_unlock(&tty_lock);
    write_daif(daif);
}

void tty_mouse_get_state(struct tty_mouse_state *out)
{
    uint64_t daif;

    if (!out)
    {
        return;
    }

    daif = read_daif();
    disable_irq();
    spin_lock(&tty_lock);
    *out = tty_mouse;
    spin_unlock(&tty_lock);
    write_daif(daif);
}
