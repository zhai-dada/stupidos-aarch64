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

void tty_flush_input(void)
{
    uint64_t daif;

    /*
     * shell 启动前把终端队列清空，避免设备初始化阶段残留的脏输入
     * 直接被解释成第一条命令。
     */
    daif = read_daif();
    disable_irq();
    spin_lock(&tty_lock);
    tty_rx_head = 0;
    tty_rx_tail = 0;
    spin_unlock(&tty_lock);
    write_daif(daif);
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

int32_t tty_try_getc(void)
{
    uint64_t daif;
    int32_t ch;

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
     * 终端缓冲区里没有字符时，再顺手查一把 PL011 FIFO。
     * 这样串口输入和 virtio-input 输入都能走同一条 tty 入口。
     */
    ch = uart_try_getc();
    write_daif(daif);
    return ch;
}

void tty_feed_char(uint8_t ch)
{
    uint64_t daif;

    daif = read_daif();
    disable_irq();
    spin_lock(&tty_lock);
    tty_rx_push(ch);
    spin_unlock(&tty_lock);

    /*
     * 唤醒等待输入的 CPU。
     * 这样 sys_read()/tty_getc() 里用 wfe 时，不会卡到下一次 timer tick。
     */
    sev();
    write_daif(daif);
}

int32_t tty_getc(void)
{
    int32_t ch;

    while (1)
    {
        /*
         * 只做一次快速探测，避免把输入路径拖进多次调度切换。
         * 这样字符一到，能立刻从 FIFO/TTY 队列里取走。
         */
        ch = tty_try_getc();
        if (ch >= 0)
        {
            return ch;
        }

        /*
         * 备用 tty 读取路径也切到事件等待。
         * 输入 IRQ 在入队时会发 sev，这里用 wfe 可以更快被唤醒。
         */
        enable_irq();
        wfe();
        disable_irq();
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
