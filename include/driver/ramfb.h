#ifndef __RAMFB_H__
#define __RAMFB_H__

#include "driver/fwcfg.h"

typedef struct
{
    uint32_t *ramfb_base;     // Pointer to pixel memory
    uint64_t ramfb_length;

    uint32_t width;     // Width in pixels
    uint32_t height;    // Height in pixels

    uint32_t x_position;
    uint32_t y_position;

    uint32_t x_charsize;
    uint32_t y_charsize;

    uint32_t pps;
} ramfb_info_t;

typedef struct
{
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
}__attribute__((packed)) ramfb_config_t;

// Colors (32-bit XRGB)
#define COLOR_BLACK   0x00000000
#define COLOR_WHITE   0x00FFFFFF
#define COLOR_GREEN   0x0000FF00
#define COLOR_AMBER   0x00FFBF00
#define COLOR_RED     0x00FF0000
#define COLOR_BLUE    0x000000FF
#define COLOR_CYAN    0x0000FFFF

#define FB_WIDTH    1280
#define FB_HEIGHT   800
#define FB_BPP      4 // RGB888

extern ramfb_info_t ramfb_info;

void ramfb_init(uint8_t *fb_addr, uint32_t width, uint32_t height);
void ramfb_putstring(uint32_t fg, uint32_t bg, const uint8_t *s);

#endif
