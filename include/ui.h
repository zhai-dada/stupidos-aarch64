#ifndef __UI_H__
#define __UI_H__

#include "asm/types.h"

/*
 * 用户态 framebuffer UI 的最小信息结构。
 * 这个结构会通过 syscall 复制给用户态，让浏览器/图形程序知道：
 * - 屏幕多大
 * - 每个像素多少字节
 * - 当前字体单元大小
 *
 * 这样用户态不需要直接摸内核 framebuffer 指针，也能画出界面。
 */
struct stupidos_fbinfo
{
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t bpp;
    uint32_t x_charsize;
    uint32_t y_charsize;
    uint64_t total_bytes;
};

void ui_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void ui_draw_text(uint32_t x, uint32_t y, uint32_t fg, uint32_t bg, const uint8_t *s);
void ui_fb_info(struct stupidos_fbinfo *out);
void ui_clear(uint32_t color);
void ui_early_banner(void);
void ui_boot_screen(void);

#endif
