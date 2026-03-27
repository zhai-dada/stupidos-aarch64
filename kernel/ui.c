#include "ui.h"

#include "driver/ramfb.h"
#include "mm/mm.h"
#include "mm/page_alloc.h"
#include "net/net.h"
#include "printk.h"
#include "sched.h"
#include "smp.h"
#include "syscall.h"
#include "tty.h"

void ui_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    uint32_t row;
    uint32_t col;
    uint32_t *base;

    base = ramfb_info.ramfb_base;
    if (!base || w == 0U || h == 0U)
    {
        return;
    }

    for (row = 0; row < h; row++)
    {
        uint32_t *p = &base[(uint64_t)(y + row) * ramfb_info.width + x];
        for (col = 0; col < w; col++)
        {
            p[col] = color;
        }
    }
}

void ui_draw_text(uint32_t x, uint32_t y, uint32_t fg, uint32_t bg, const uint8_t *s)
{
    ramfb_info.x_position = x;
    ramfb_info.y_position = y;
    ramfb_putstring(fg, bg, s);
}

void ui_clear(uint32_t color)
{
    ui_fill_rect(0, 0, ramfb_info.width, ramfb_info.height, color);
}

void ui_fb_info(struct stupidos_fbinfo *out)
{
    if (!out)
    {
        return;
    }

    out->width = ramfb_info.width;
    out->height = ramfb_info.height;
    out->stride = ramfb_info.width * FB_BPP;
    out->bpp = FB_BPP;
    out->x_charsize = ramfb_info.x_charsize;
    out->y_charsize = ramfb_info.y_charsize;
    out->total_bytes = ramfb_info.ramfb_length;
}

static void ui_draw_frame_border(uint32_t color)
{
    /*
     * 画一个很明显的外框，方便在 GTK 窗口里一眼判断 framebuffer 是否真的亮了。
     * 这里故意使用高对比度颜色，避免“内容画了但肉眼看不出来”。
     */
    ui_fill_rect(0, 0, ramfb_info.width, 8, color);
    ui_fill_rect(0, ramfb_info.height - 8U, ramfb_info.width, 8, color);
    ui_fill_rect(0, 0, 8, ramfb_info.height, color);
    ui_fill_rect(ramfb_info.width - 8U, 0, 8, ramfb_info.height, color);
}

void ui_early_banner(void)
{
    /*
     * 早期横幅不依赖任何子系统状态，只负责快速证明：
     * 1. RAMFB 地址是通的
     * 2. 像素写入是通的
     * 3. GTK / 显示窗口里确实会出现明显变化
     */
    ui_clear(0x00000000);
    ui_fill_rect(0, 0, ramfb_info.width, ramfb_info.height, 0x00001820);
    ui_fill_rect(0, 0, ramfb_info.width, 84, 0x00FFB000);
    ui_fill_rect(0, 84, ramfb_info.width, 12, 0x00FFFFFF);
    ui_fill_rect(24, 132, ramfb_info.width - 48, 120, 0x0018222D);
    ui_fill_rect(24, 280, ramfb_info.width - 48, 64, 0x00FF4040);
    ui_fill_rect(24, 360, ramfb_info.width - 48, 64, 0x0000D0FF);
    ui_draw_frame_border(0x00FFFFFF);

    ui_draw_text(40, 24, COLOR_BLACK, 0x00FFB000, (const uint8_t *)"STUPIDOS-AARCH64");
    ui_draw_text(40, 160, COLOR_WHITE, 0x0018222D, (const uint8_t *)"framebuffer online");
    ui_draw_text(40, 190, COLOR_WHITE, 0x0018222D, (const uint8_t *)"if you can see this, RAMFB is alive");
    ui_draw_text(40, 296, COLOR_BLACK, 0x00FF4040, (const uint8_t *)"early splash");
    ui_draw_text(40, 376, COLOR_BLACK, 0x0000D0FF, (const uint8_t *)"waiting for full system init");
}

void ui_boot_screen(void)
{
    char mem_text[64];
    char cpu_text[64];
    char net_text[80];
    char syscall_text[64];
    char shell_text[64];
    char mouse_text[80];
    struct net_device *dev;
    struct tty_mouse_state mouse;

    ui_clear(0x00101820);
    ui_fill_rect(0, 0, ramfb_info.width, 56, 0x00FFB000);
    ui_fill_rect(0, 56, ramfb_info.width, 2, 0x00FFFFFF);
    ui_fill_rect(16, 88, ramfb_info.width - 32, 252, 0x0018222D);
    ui_fill_rect(16, 352, ramfb_info.width - 32, 204, 0x0012161C);
    ui_draw_frame_border(0x00FF4040);

    /*
     * 左上角增加一个亮色块，哪怕字体没显示，用户也能立刻看见显存变化。
     */
    ui_fill_rect(24, 104, 136, 96, 0x00FF4040);
    ui_fill_rect(176, 104, 136, 96, 0x0000D0FF);
    ui_fill_rect(328, 104, 136, 96, 0x0000FF88);
    ui_fill_rect(480, 104, 136, 96, 0x00FFFFFF);

    ui_draw_text(40, 18, COLOR_BLACK, 0x00FFBF00, (const uint8_t *)"STUPIDOS-AARCH64");
    ui_draw_text(40, 110, COLOR_WHITE, 0x0018222D, (const uint8_t *)"boot dashboard");
    ui_draw_text(40, 136, COLOR_CYAN, 0x0018222D, (const uint8_t *)"status: kernel online, scheduler online, shell online");
    ui_draw_text(40, 170, COLOR_WHITE, 0x0018222D, (const uint8_t *)"subsystems:");
    ui_draw_text(64, 196, COLOR_GREEN, 0x0018222D, (const uint8_t *)"vfs      ext4 + fat32 mounted");
    ui_draw_text(64, 220, COLOR_GREEN, 0x0018222D, (const uint8_t *)"memory   buddy allocator online");
    ui_draw_text(64, 244, COLOR_GREEN, 0x0018222D, (const uint8_t *)"sched    multicore cfs-lite ready");
    ui_draw_text(64, 268, COLOR_GREEN, 0x0018222D, (const uint8_t *)"syscall  svc64 dispatch ready");
    ui_draw_text(40, 374, COLOR_AMBER, 0x0012161C, (const uint8_t *)"shell: type help, ls, cat, write, info");
    ui_draw_text(40, 404, COLOR_WHITE, 0x0012161C, (const uint8_t *)"next: user mode, network stack, pcie net, python runtime");

    sprintf((int8_t *)mem_text, "memory: %lu MB free_pages: %u/%u",
            (uint64_t)(TOTAL_MEMORY / 0x100000),
            page_alloc_free_pages(),
            page_alloc_total_pages());
    sprintf((int8_t *)cpu_text, "cpus: %u online: %u", smp_cpu_count(), smp_online_count());
    dev = net_default_device();
    if (dev)
    {
        sprintf((int8_t *)net_text, "net: %s online", dev->name);
    }
    else
    {
        sprintf((int8_t *)net_text, "net: offline");
    }
    sprintf((int8_t *)syscall_text, "syscall: abi ready");
    sprintf((int8_t *)shell_text, "shell: pid %d", task_current() ? task_current()->pid : -1);
    tty_mouse_get_state(&mouse);
    sprintf((int8_t *)mouse_text, "input: mouse x=%d y=%d buttons=%#x",
            mouse.x, mouse.y, mouse.buttons);

    ui_draw_text(40, 308, COLOR_WHITE, 0x0018222D, (const uint8_t *)mem_text);
    ui_draw_text(40, 332, COLOR_WHITE, 0x0018222D, (const uint8_t *)cpu_text);
    ui_draw_text(40, 356, COLOR_WHITE, 0x0018222D, (const uint8_t *)net_text);
    ui_draw_text(40, 380, COLOR_WHITE, 0x0012161C, (const uint8_t *)mouse_text);
    ui_draw_text(40, 450, COLOR_WHITE, 0x0012161C, (const uint8_t *)syscall_text);
    ui_draw_text(40, 474, COLOR_WHITE, 0x0012161C, (const uint8_t *)shell_text);

    /*
     * 通过几条高对比度横线，进一步确认 ramfb 的刷新方向和地址都对。
     */
    ui_fill_rect(40, 520, 360, 6, 0x00FFBF00);
    ui_fill_rect(40, 536, 420, 6, 0x0000FFFF);
    ui_fill_rect(40, 552, 480, 6, 0x00FFFFFF);

    printk("[ui\tinit]: boot dashboard rendered\n");
}
