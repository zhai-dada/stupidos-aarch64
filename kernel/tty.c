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
#define TTY_ACTIVE_POLL_SPINS 4096U

static spinlock_t tty_lock = SPINLOCK_INIT;
static uint8_t tty_rx_buf[TTY_RX_BUF_SIZE];
static uint32_t tty_rx_head;
static uint32_t tty_rx_tail;
static uint32_t tty_line_len;
static uint8_t tty_escape_state;
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

static size_t tty_ingest_char_locked(uint8_t ch, uint8_t *echo_buf)
{
    uint8_t stored_ch;
    size_t echo_len;

    /*
     * 所有输入源统一在这里做规范化和回显：
     * - 回车统一成换行
     * - 字符进入 TTY 队列
     * - 生成对应的本地回显序列
     *
     * 这样 UART IRQ、virtio keyboard IRQ，以及轮询补位路径的表现一致。
    */
    stored_ch = (ch == '\r') ? '\n' : ch;
    tty_rx_push(stored_ch);

    /*
     * 方向键会以 ANSI escape 序列发进来，例如 ESC [ A。
     * 这些字节要继续留给上层 shell 解析，但不要直接回显出来，
     * 否则终端上会看到 "[A" 这类碎片字符。
     */
    if (tty_escape_state == 1)
    {
        if (stored_ch == '[')
        {
            tty_escape_state = 2;
            return 0;
        }

        /*
         * 不是标准 CSI 前缀时，不要吞掉下一个普通字符回显。
         * 之前这里直接 return 0，会造成“字符进了命令缓冲但终端没显示”。
         */
        tty_escape_state = 0;
    }
    if (tty_escape_state == 2)
    {
        /*
         * CSI 序列以 0x40..0x7e 结尾，期间所有字节都不回显。
         * 这样方向键/功能键不会在终端上留下 "[A"/"~" 等碎片。
         */
        if (stored_ch >= 0x40 && stored_ch <= 0x7e)
        {
            tty_escape_state = 0;
        }
        return 0;
    }
    if (stored_ch == 0x1b)
    {
        tty_escape_state = 1;
        return 0;
    }

    echo_len = 0;
    if (stored_ch == '\n')
    {
        tty_line_len = 0;
        echo_buf[echo_len++] = '\r';
        echo_buf[echo_len++] = '\n';
    }
    else if (stored_ch == '\b' || stored_ch == 0x7f)
    {
        if (tty_line_len > 0)
        {
            tty_line_len--;
            echo_buf[echo_len++] = '\b';
            echo_buf[echo_len++] = ' ';
            echo_buf[echo_len++] = '\b';
        }
    }
    else if (stored_ch >= 0x20 && stored_ch <= 0x7e)
    {
        tty_line_len++;
        echo_buf[echo_len++] = stored_ch;
    }

    return echo_len;
}

void tty_init(void)
{
    spin_lock_init(&tty_lock);
    tty_rx_head = 0;
    tty_rx_tail = 0;
    tty_line_len = 0;
    tty_escape_state = 0;
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
    tty_line_len = 0;
    tty_escape_state = 0;
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

void tty_write_bytes(const void *buf, size_t len)
{
    const uint8_t *bytes;
    size_t i;

    if (!buf || !len)
    {
        return;
    }

    /*
     * 给 stdout / shell 回显提供一个批量入口。
     * 底层还是走同一条串口发送路径，但可以减少上层反复拆成
     * 单字节调用带来的函数开销和调度抖动。
     */
    bytes = (const uint8_t *)buf;
    for (i = 0; i < len; i++)
    {
        tty_putc(bytes[i]);
    }
}

int32_t tty_try_getc(void)
{
    uint8_t echo_buf[3];
    uint8_t echo_accum[TTY_RX_BUF_SIZE];
    uint64_t daif;
    int32_t ch;
    int32_t raw;
    size_t echo_len;
    size_t echo_accum_len;
    bool have_polled_input;

    daif = read_daif();
    disable_irq();
    spin_lock(&tty_lock);
    ch = tty_rx_pop();
    if (ch >= 0)
    {
        spin_unlock(&tty_lock);
        write_daif(daif);
        return ch;
    }

    /*
     * 终端缓冲区里没有字符时，再顺手查一把 PL011 FIFO。
     * 这样串口输入和 virtio-input 输入都能走同一条 tty 入口。
     */
    echo_accum_len = 0;
    have_polled_input = false;
    while (1)
    {
        raw = uart_try_getc();
        if (raw < 0)
        {
            break;
        }

        have_polled_input = true;
        echo_len = tty_ingest_char_locked((uint8_t)raw, echo_buf);
        if (echo_len && echo_accum_len + echo_len <= sizeof(echo_accum))
        {
            for (size_t i = 0; i < echo_len; i++)
            {
                echo_accum[echo_accum_len++] = echo_buf[i];
            }
        }
    }

    if (have_polled_input)
    {
        ch = tty_rx_pop();
    }
    spin_unlock(&tty_lock);
    write_daif(daif);

    if (echo_accum_len)
    {
        tty_write_bytes(echo_accum, echo_accum_len);
    }
    if (have_polled_input)
    {
        sev();
    }

    return ch;
}

void tty_feed_bytes(const uint8_t *buf, size_t len)
{
    uint64_t daif;
    uint8_t echo_buf[3];
    uint8_t echo_accum[TTY_RX_BUF_SIZE];
    size_t echo_len;
    size_t echo_accum_len;
    size_t i;

    if (!buf || !len)
    {
        return;
    }

    daif = read_daif();
    disable_irq();
    spin_lock(&tty_lock);
    echo_accum_len = 0;
    for (i = 0; i < len; i++)
    {
        echo_len = tty_ingest_char_locked(buf[i], echo_buf);
        if (!echo_len)
        {
            continue;
        }

        if (echo_accum_len + echo_len > sizeof(echo_accum))
        {
            break;
        }

        echo_accum[echo_accum_len++] = echo_buf[0];
        if (echo_len > 1)
        {
            echo_accum[echo_accum_len++] = echo_buf[1];
            if (echo_len > 2)
            {
                echo_accum[echo_accum_len++] = echo_buf[2];
            }
        }
    }
    spin_unlock(&tty_lock);
    write_daif(daif);
    if (echo_accum_len)
    {
        tty_write_bytes(echo_accum, echo_accum_len);
    }

    /*
     * 唤醒等待输入的 CPU。
     * 这样 sys_read()/tty_getc() 里用 wfe 时，不会卡到下一次 timer tick。
     */
    sev();
}

void tty_feed_char(uint8_t ch)
{
    tty_feed_bytes(&ch, 1);
}

int32_t tty_getc(void)
{
    int32_t ch;
    uint32_t spins;
    uint64_t wait_daif;

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
         * 混合等待策略：
         * - 先做一小段主动轮询，覆盖“UART IRQ 临时异常但 FIFO 有字节”的情况；
         * - 再退回 wfe，避免彻底忙等。
         * 这样交互体验会更稳定，不会出现按键偶发卡顿到下一个时钟节拍才醒。
         */
        for (spins = 0; spins < TTY_ACTIVE_POLL_SPINS; spins++)
        {
            ch = tty_try_getc();
            if (ch >= 0)
            {
                return ch;
            }
            nop();
        }

        /*
         * 备用 tty 读取路径也切到事件等待。
         * 输入 IRQ 在入队时会发 sev，这里用 wfe 可以更快被唤醒。
         */
        wait_daif = read_daif();
        enable_irq();
        wfe();
        write_daif(wait_daif);
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
