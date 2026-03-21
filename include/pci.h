#ifndef __PCI_H__
#define __PCI_H__

#include "asm/types.h"

/*
 * QEMU virt 板在设备树里给出的 PCIe ECAM 信息如下：
 * - compatible = "pci-host-ecam-generic"
 * - reg = <0x00000040 0x10000000 0x00000000 0x10000000>
 *   也就是 ECAM 物理基址 0x4010000000，大小 256MB
 *
 * 这里先实现一个最小可用的 PCI 子系统：
 * 1. 通过 ECAM 访问 PCI 配置空间
 * 2. 枚举 bus/device/function
 * 3. 打印 vendor/device/class/BAR，给后续 virtio-pci、网卡和块设备驱动打底
 */
#define PCIE_ECAM_BASE          0x4010000000UL
#define PCIE_ECAM_SIZE          0x10000000UL
#define PCIE_ECAM_HIGH_BASE     0xFFFF910000000000UL

#define PCI_MAX_BUSES           256
#define PCI_MAX_DEVICES         32
#define PCI_MAX_FUNCTIONS       8

#define PCI_VENDOR_ID           0x00
#define PCI_DEVICE_ID           0x02
#define PCI_COMMAND             0x04
#define PCI_STATUS              0x06
#define PCI_REVISION_ID         0x08
#define PCI_PROG_IF             0x09
#define PCI_SUBCLASS            0x0A
#define PCI_CLASS_CODE          0x0B
#define PCI_HEADER_TYPE         0x0E
#define PCI_BAR0                0x10

#define PCI_HEADER_TYPE_MASK    0x7f
#define PCI_HEADER_TYPE_MULTI   0x80

#define PCI_CLASS_NETWORK       0x02

struct pci_bdf
{
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

void pci_init(void);
uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg);
void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg, uint32_t value);

#endif
