#ifndef __FDT_H__
#define __FDT_H__

#include "asm/types.h"

/*
 * 设备树扫描结果里，只保留我们当前最需要的几类节点。
 * 后续驱动模型可以直接基于这些信息做匹配和绑定。
 */
enum fdt_device_kind
{
    FDT_DEVICE_UNKNOWN = 0,
    FDT_DEVICE_MEMORY,
    FDT_DEVICE_UART,
    FDT_DEVICE_FWCFG,
    FDT_DEVICE_VIRTIO_MMIO,
    FDT_DEVICE_PCIE_HOST,
    FDT_DEVICE_FRAMEBUFFER,
};

struct fdt_device_desc
{
    enum fdt_device_kind kind;
    int8_t path[128];
    int8_t name[64];
    int8_t compatible[128];
    uint64_t reg_base;
    uint64_t reg_size;
    uint32_t irq;
    bool has_reg;
    bool has_irq;
};

void fdt_boot_init(const void *dtb);
void fdt_log_summary(void);

uint64_t fdt_memory_base(void);
uint64_t fdt_memory_size(void);

uint32_t fdt_device_count(void);
const struct fdt_device_desc *fdt_device(uint32_t index);
const struct fdt_device_desc *fdt_find_device_by_kind(enum fdt_device_kind kind);
const struct fdt_device_desc *fdt_find_device_by_reg(enum fdt_device_kind kind, uint64_t reg_base);

#endif
