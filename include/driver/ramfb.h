#ifndef __RAMFB_H__
#define __RAMFB_H__

#include "driver/fwcfg.h"

typedef struct
{
    uint32_t *base;     // Pointer to pixel memory
    uint32_t width;     // Width in pixels
    uint32_t height;    // Height in pixels
    uint32_t pitch;     // Bytes per row (may include padding)
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

void setup_ramfb(uint8_t *fb_addr, uint32_t width, uint32_t height);

#endif
