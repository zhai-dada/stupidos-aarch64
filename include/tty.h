#ifndef __TTY_H__
#define __TTY_H__

#include "asm/types.h"

/*
 * 统一终端层。
 * 未来键盘、串口、SSH、伪终端都可以往这里喂输入；
 * shell 只依赖 tty，不需要关心底层到底是 UART 还是 virtio-input。
 */
struct tty_mouse_state
{
    int32_t x;
    int32_t y;
    uint32_t buttons;
};

void tty_init(void);
void tty_flush_input(void);
void tty_putc(uint8_t ch);
void tty_write(const int8_t *str);
void tty_write_bytes(const void *buf, size_t len);
int32_t tty_try_getc(void);
int32_t tty_getc(void);
void tty_feed_bytes(const uint8_t *buf, size_t len);
void tty_feed_char(uint8_t ch);
void tty_report_mouse_delta(int32_t dx, int32_t dy, uint32_t buttons);
void tty_report_mouse_abs(int32_t x, int32_t y, uint32_t buttons);
void tty_mouse_get_state(struct tty_mouse_state *out);

#endif
