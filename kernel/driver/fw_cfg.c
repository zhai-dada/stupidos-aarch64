#include "driver/fwcfg.h"
#include "lib/libmem.h"
#include "printk.h"
#include "driver/ramfb.h"
#include "asm/barrier.h"

uint16_t to_be16(uint16_t v)
{
    return __builtin_bswap16(v);
}

uint32_t to_be32(uint32_t v)
{
    return __builtin_bswap32(v);
}

uint64_t to_be64(uint64_t v)
{
    return __builtin_bswap64(v);
}

void fw_cfg_dma_transfer(uint32_t control, uint32_t len, uint64_t addr)
{
    fw_cfg_dma_t dma;

    dma.control = to_be32(control);
    dma.len = to_be32(len);
    dma.addr = to_be64(addr);

    // 写 DMA 描述符地址（BE）
    *(volatile uint64_t*)FW_CFG_DMA_ADDR = to_be64((uint64_t)&dma);

    // Wait for completion
    while (to_be32(dma.control) & ~FW_CFG_DMA_CTL_ERROR)
    {
        dsb(sy);
    }
}
