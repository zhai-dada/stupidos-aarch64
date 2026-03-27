#include "driver/ramfb.h"
#include "driver/fwcfg.h"
#include "lib/libmem.h"
#include "font.h"
#include "mmu.h"
#include "printk.h"

uint8_t framebuffer[FB_WIDTH * FB_HEIGHT * FB_BPP];

ramfb_info_t ramfb_info;

void ramfb_rebind_runtime_base(void)
{
    /*
     * 启用 MMU 之后，内核代码运行在 KIMAGE_VADDR 别名下。
     * framebuffer 也属于内核镜像 BSS，给它绑定一个高地址别名，
     * 后续 UI / browser / shell 的绘制就始终走统一虚拟地址。
     */
    if (ramfb_info.ramfb_phys_base)
    {
        ramfb_info.ramfb_kimage_base = (uint32_t *)kimage_phys_to_virt(ramfb_info.ramfb_phys_base);
        ramfb_info.ramfb_base = ramfb_info.ramfb_kimage_base;
    }
}

void ramfb_init(uint8_t *fb_addr, uint32_t width, uint32_t height)
{
    uint32_t num_entries = 0;

    // 读取 entry 数量
    fw_cfg_dma_transfer(
        (FW_CFG_FILE_DIR << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_READ,
        sizeof(uint32_t),
        (uint64_t)&num_entries);

    num_entries = to_be32(num_entries);

    fw_cfg_file_t ramfb;
    memset((void *)&ramfb, 0, sizeof(ramfb));

    // 遍历 fw_cfg 文件目录
    for (uint32_t i = 0; i < num_entries; i++)
    {
        fw_cfg_dma_transfer(
            FW_CFG_DMA_CTL_READ,
            sizeof(fw_cfg_file_t),
            (uint64_t)&ramfb);

        if (strcmp((int8_t*)ramfb.name, "etc/ramfb") == 0)
        {
            printk("[ramfb\tinit]: %s select -> %x\n", ramfb.name, to_be16(ramfb.select));
            break;
        }
    }

    ramfb_config_t config;
    config.addr = to_be64((uint64_t)fb_addr);
    config.fourcc = to_be32(0x34325258); // "XR24" = XRGB8888
    config.flags = to_be32(0);
    config.width = to_be32(width);
    config.height = to_be32(height);
    config.stride = to_be32(width * 4);

    // Write config
    fw_cfg_dma_transfer(
        (to_be16(ramfb.select) << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_WRITE,
        sizeof(ramfb_config_t),
        (uint64_t)&config);

    ramfb_info.ramfb_base = (uint32_t *)fb_addr;
    ramfb_info.ramfb_phys_base = (uint64_t)fb_addr;
    ramfb_info.ramfb_kimage_base = NULL;
    ramfb_info.ramfb_length = FB_HEIGHT * FB_WIDTH * FB_BPP;
    ramfb_info.width = FB_WIDTH;
    ramfb_info.height = FB_HEIGHT;
    ramfb_info.x_charsize = FONT_WIDTH;
    ramfb_info.y_charsize = FONT_HEIGHT;
    ramfb_info.x_position = 0;
    ramfb_info.y_position = 0;
    ramfb_info.pps = 4;

    // 填充成为纯白色背景
    __memset_4bytes((void *)ramfb_info.ramfb_base, COLOR_WHITE, ramfb_info.width * ramfb_info.height);
    printk("[ramfb\tinit]: init ok\n");

    return;
}

static void ramfb_putchar(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
    uint32_t *base;
    const uint8_t *glyph = font_ascii[(uint8_t)c];
    base = ramfb_info.ramfb_base;
    if (!base)
    {
        return;
    }

    uint32_t *row_ptr = &base[y * ramfb_info.width + x];

    for (uint8_t row = 0; row < ramfb_info.y_charsize; row++)
    {
        uint8_t bits = glyph[row];
        // Unroll the 8-pixel row for speed
        row_ptr[0] = (bits & 0x80) ? fg : bg;
        row_ptr[1] = (bits & 0x40) ? fg : bg;
        row_ptr[2] = (bits & 0x20) ? fg : bg;
        row_ptr[3] = (bits & 0x10) ? fg : bg;
        row_ptr[4] = (bits & 0x08) ? fg : bg;
        row_ptr[5] = (bits & 0x04) ? fg : bg;
        row_ptr[6] = (bits & 0x02) ? fg : bg;
        row_ptr[7] = (bits & 0x01) ? fg : bg;
        row_ptr += ramfb_info.width;
    }
}

void ramfb_putstring(uint32_t fg, uint32_t bg, const uint8_t *s)
{
    while(*s != '\0')
    {
        if(*s == '\n')
        {
            ramfb_info.y_position += ramfb_info.y_charsize;
            ramfb_info.x_position = 0;
            s++;
            continue;
        }
        else if(*s == '\b')
        {
            if(ramfb_info.x_position > ramfb_info.x_charsize)
            {
                ramfb_info.x_position -= ramfb_info.x_charsize;
            }
            ramfb_putchar(ramfb_info.x_position, ramfb_info.y_position, ' ', fg, bg);
            s++;
            continue;
        }
        ramfb_putchar(ramfb_info.x_position, ramfb_info.y_position, *s, fg, bg);
        ramfb_info.x_position += ramfb_info.x_charsize;
        s++;
    }
}
