#include "driver/ramfb.h"
#include "driver/fwcfg.h"
#include "printk.h"
#include "lib/libmem.h"

#define FB_WIDTH 1280
#define FB_HEIGHT 800
#define FB_BPP 4 // RGB888
uint8_t framebuffer[FB_WIDTH * FB_HEIGHT * FB_BPP];

void setup_ramfb(uint8_t *fb_addr, uint32_t width, uint32_t height)
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
            printk("[ramfb init] : %s select -> %x\n", ramfb.name, to_be16(ramfb.select));
            break;
        }
    }

    ramfb_config_t config;
    config.addr = to_be64((uint64_t)fb_addr);
    config.fourcc = to_be32(0x34325258); // "XR24" = XRGB8888
    config.flags = to_be32(0);
    config.width = to_be32(width);
    config.height = to_be32(height);
    config.stride = to_be32(1280 * 4);

    // Write config
    fw_cfg_dma_transfer(
        (to_be16(ramfb.select) << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_WRITE,
        sizeof(ramfb_config_t),
        (uint64_t)&config);

    return;
}
