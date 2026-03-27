#include "tty.h"

#include "driver/uart.h"
#include "lib/libasm.h"
#include "lib/libirq.h"
#include "lib/libstr.h"
#include "sched.h"
#include "spinlock.h"

/*
 * tty 的输入缓冲。
 * UART IRQ 和 virtio-input IRQ 都会往这里塞字节，shell 只负责取字节。
 */
#define TTY_RX_BUF_SIZE 512
#define TTY_ACTIVE_POLL_SPINS 4096U
/* 最小 termios 标志位（与 Linux 常量保持一致）。 */
#define TTY_TERM_LFLAG_ICANON 0x00000002U
#define TTY_TERM_LFLAG_ECHO   0x00000008U
#define TTY_TERM_LFLAG_ISIG   0x00000001U
#define TTY_TERM_LFLAG_ECHOE  0x00000010U
#define TTY_TERM_LFLAG_ECHOK  0x00000020U
#define TTY_TERM_IFLAG_ICRNL  0x00000100U
#define TTY_TERM_IFLAG_IXON   0x00000400U
#define TTY_TERM_OFLAG_OPOST  0x00000001U
#define TTY_TERM_OFLAG_ONLCR  0x00000004U
#define TTY_TERM_CFLAG_CREAD  0x00000080U
#define TTY_TERM_CFLAG_CS8    0x00000030U
#define TTY_TERM_CC_VTIME     5U
#define TTY_TERM_CC_VMIN      6U

static spinlock_t tty_lock = SPINLOCK_INIT;
static uint8_t tty_rx_buf[TTY_RX_BUF_SIZE];
static uint32_t tty_rx_head;
static uint32_t tty_rx_tail;
static uint32_t tty_line_len;
static uint8_t tty_escape_state;
static struct tty_mouse_state tty_mouse;
static struct tty_termios_state tty_termios;

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
    bool canonical;
    bool echo_enabled;

    canonical = (tty_termios.lflag & TTY_TERM_LFLAG_ICANON) != 0U;
    echo_enabled = (tty_termios.lflag & TTY_TERM_LFLAG_ECHO) != 0U;

    /*
     * 所有输入源统一在这里做规范化和回显：
     * - 回车统一成换行
     * - 字符进入 TTY 队列
     * - 生成对应的本地回显序列
     *
     * 这样 UART IRQ、virtio keyboard IRQ，以及轮询补位路径的表现一致。
    */
    /*
     * 统一把 CR 归一化成 LF，避免不同输入设备（串口/virtio 键盘）
     * 在 Enter 键上行为不一致，影响 vi 命令模式确认。
     */
    stored_ch = (ch == '\r') ? '\n' : ch;
    tty_rx_push(stored_ch);

    /*
     * 方向键会以 ANSI escape 序列发进来，例如 ESC [ A。
     * 这些字节要继续留给上层 shell 解析，但不要直接回显出来，
     * 否则终端上会看到 "[A" 这类碎片字符。
     */
    if (!canonical || !echo_enabled)
    {
        tty_escape_state = 0;
    }
    else if (tty_escape_state == 1)
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
    if (canonical && echo_enabled && tty_escape_state == 2)
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
    if (canonical && echo_enabled && stored_ch == 0x1b)
    {
        tty_escape_state = 1;
        return 0;
    }

    echo_len = 0;
    if (!echo_enabled)
    {
        return 0;
    }

    if (!canonical)
    {
        if (stored_ch == '\n' || stored_ch == '\r')
        {
            echo_buf[echo_len++] = '\r';
            echo_buf[echo_len++] = '\n';
        }
        else if (stored_ch >= 0x20 && stored_ch <= 0x7e)
        {
            echo_buf[echo_len++] = stored_ch;
        }
        return echo_len;
    }

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

    /*
     * 缺省使用“规范模式 + 回显”，保证 shell 行编辑开箱即用；
     * vi/vim 再通过 tcsetattr 切换到 raw/noecho。
     */
    tty_termios.iflag = TTY_TERM_IFLAG_ICRNL | TTY_TERM_IFLAG_IXON;
    tty_termios.oflag = TTY_TERM_OFLAG_OPOST | TTY_TERM_OFLAG_ONLCR;
    tty_termios.cflag = TTY_TERM_CFLAG_CREAD | TTY_TERM_CFLAG_CS8;
    tty_termios.lflag = TTY_TERM_LFLAG_ISIG | TTY_TERM_LFLAG_ICANON | TTY_TERM_LFLAG_ECHO |
                        TTY_TERM_LFLAG_ECHOE | TTY_TERM_LFLAG_ECHOK;
    tty_termios.line = 0;
    for (uint32_t i = 0; i < 19U; i++)
    {
        tty_termios.cc[i] = 0;
    }
    tty_termios.cc[TTY_TERM_CC_VMIN] = 1;
    tty_termios.cc[TTY_TERM_CC_VTIME] = 0;
    tty_termios.ispeed = 0;
    tty_termios.ospeed = 0;
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

    uart_write_bytes((const uint8_t *)str, strlen((int8_t *)str));
}

void tty_write_bytes(const void *buf, size_t len)
{
    const uint8_t *bytes;

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
    uart_write_bytes(bytes, len);
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

int32_t tty_pending_count(void)
{
    uint64_t daif;
    uint32_t count;
    uint32_t fr;

    daif = read_daif();
    disable_irq();
    spin_lock(&tty_lock);
    if (tty_rx_head >= tty_rx_tail)
    {
        count = tty_rx_head - tty_rx_tail;
    }
    else
    {
        count = (TTY_RX_BUF_SIZE - tty_rx_tail) + tty_rx_head;
    }
    /*
     * 如果环形队列里暂时还没有字节，但 UART 硬件 FIFO 已经收到输入，
     * 这里也把它算作“有待处理输入”。
     *
     * 这样 select/poll 能更快判断 stdin 可读，不会因为 IRQ 与上层轮询
     * 的极小时间差把编辑器交互拖慢。
     */
    if (count == 0)
    {
        fr = get32(PL011_UART0_BASE + UART_FR);
        if ((fr & UART_FR_RXFE) == 0U)
        {
            count = 1;
        }
    }
    spin_unlock(&tty_lock);
    write_daif(daif);
    return (int32_t)count;
}

int32_t tty_is_canonical_mode(void)
{
    uint64_t daif;
    int32_t canonical;

    daif = read_daif();
    disable_irq();
    spin_lock(&tty_lock);
    canonical = (tty_termios.lflag & TTY_TERM_LFLAG_ICANON) ? 1 : 0;
    spin_unlock(&tty_lock);
    write_daif(daif);
    return canonical;
}

void tty_get_termios(struct tty_termios_state *out)
{
    uint64_t daif;

    if (!out)
    {
        return;
    }

    daif = read_daif();
    disable_irq();
    spin_lock(&tty_lock);
    *out = tty_termios;
    spin_unlock(&tty_lock);
    write_daif(daif);
}

int32_t tty_set_termios(const struct tty_termios_state *in, int32_t flush_input)
{
    uint64_t daif;

    if (!in)
    {
        return -1;
    }

    daif = read_daif();
    disable_irq();
    spin_lock(&tty_lock);
    tty_termios = *in;
    if (flush_input)
    {
        tty_rx_head = 0;
        tty_rx_tail = 0;
        tty_line_len = 0;
        tty_escape_state = 0;
    }
    spin_unlock(&tty_lock);
    write_daif(daif);
    return 0;
}
