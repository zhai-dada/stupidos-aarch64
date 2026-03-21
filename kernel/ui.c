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

static void ui_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    uint32_t row;
    uint32_t col;

    for (row = 0; row < h; row++)
    {
        uint32_t *p = (uint32_t *)((uint8_t *)ramfb_info.ramfb_base +
                                   ((uint64_t)(y + row) * ramfb_info.width + x) * FB_BPP);
        for (col = 0; col < w; col++)
        {
            p[col] = color;
        }
    }
}

static void ui_write_line(uint32_t x, uint32_t y, uint32_t fg, uint32_t bg, const uint8_t *s)
{
    ramfb_info.x_position = x;
    ramfb_info.y_position = y;
    ramfb_putstring(fg, bg, s);
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

    ui_fill_rect(0, 0, ramfb_info.width, ramfb_info.height, 0x00101820);
    ui_fill_rect(0, 0, ramfb_info.width, 56, 0x00FFBF00);
    ui_fill_rect(0, 56, ramfb_info.width, 2, 0x00FFFFFF);
    ui_fill_rect(24, 96, ramfb_info.width - 48, 232, 0x0018222D);
    ui_fill_rect(24, 350, ramfb_info.width - 48, 188, 0x0012161C);

    ui_write_line(40, 18, COLOR_BLACK, 0x00FFBF00, (const uint8_t *)"STUPIDOS-AARCH64");
    ui_write_line(40, 110, COLOR_WHITE, 0x0018222D, (const uint8_t *)"boot dashboard");
    ui_write_line(40, 136, COLOR_CYAN, 0x0018222D, (const uint8_t *)"status: kernel online, scheduler online, shell online");
    ui_write_line(40, 170, COLOR_WHITE, 0x0018222D, (const uint8_t *)"subsystems:");
    ui_write_line(64, 196, COLOR_GREEN, 0x0018222D, (const uint8_t *)"vfs      ext4 + fat32 mounted");
    ui_write_line(64, 220, COLOR_GREEN, 0x0018222D, (const uint8_t *)"memory   buddy allocator online");
    ui_write_line(64, 244, COLOR_GREEN, 0x0018222D, (const uint8_t *)"sched    multicore cfs-lite ready");
    ui_write_line(64, 268, COLOR_GREEN, 0x0018222D, (const uint8_t *)"syscall  svc64 dispatch ready");
    ui_write_line(40, 374, COLOR_AMBER, 0x0012161C, (const uint8_t *)"shell: type help, ls, cat, write, info");
    ui_write_line(40, 404, COLOR_WHITE, 0x0012161C, (const uint8_t *)"next: user mode, network stack, pcie net, python runtime");

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

    ui_write_line(40, 308, COLOR_WHITE, 0x0018222D, (const uint8_t *)mem_text);
    ui_write_line(40, 332, COLOR_WHITE, 0x0018222D, (const uint8_t *)cpu_text);
    ui_write_line(40, 356, COLOR_WHITE, 0x0018222D, (const uint8_t *)net_text);
    ui_write_line(40, 380, COLOR_WHITE, 0x0012161C, (const uint8_t *)mouse_text);
    ui_write_line(40, 450, COLOR_WHITE, 0x0012161C, (const uint8_t *)syscall_text);
    ui_write_line(40, 474, COLOR_WHITE, 0x0012161C, (const uint8_t *)shell_text);

    printk("[ui\tinit]: boot dashboard rendered\n");
}
