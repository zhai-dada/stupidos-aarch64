#include "pci.h"
#include "lib/librw.h"
#include "printk.h"

struct pci_state
{
    bool ready;
    uint32_t nr_devices;
};

static struct pci_state pci_state;

/*
 * 当前 QEMU virt 平台基本都落在 bus 0 上。
 * 先把扫描范围收紧到少量 bus，避免 256 个 bus 全扫在 TCG 下拖慢启动。
 * 以后补 bridge / 热插拔时再把这里升级成递归拓扑枚举。
 */
#define PCI_SCAN_BUS_LIMIT 8

static inline uint64_t pci_cfg_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg)
{
    return PCIE_ECAM_HIGH_BASE +
           ((uint64_t)bus << 20) +
           ((uint64_t)dev << 15) +
           ((uint64_t)fn << 12) +
           (reg & 0xfff);
}

static uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg)
{
    uint32_t value;

    value = pci_cfg_read32(bus, dev, fn, reg & ~0x3U);
    return (uint16_t)(value >> ((reg & 0x2U) * 8));
}

static uint8_t pci_cfg_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg)
{
    uint32_t value;

    value = pci_cfg_read32(bus, dev, fn, reg & ~0x3U);
    return (uint8_t)(value >> ((reg & 0x3U) * 8));
}

static void pci_dump_bars(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t header_type)
{
    uint32_t bar;
    uint32_t i;
    uint32_t bar_count;

    bar_count = (header_type == 0x01) ? 2 : 6;
    for (i = 0; i < bar_count; i++)
    {
        bar = pci_cfg_read32(bus, dev, fn, PCI_BAR0 + (uint16_t)(i * 4));
        if (bar == 0)
        {
            continue;
        }

        if (bar & 0x1)
        {
            /*
             * I/O BAR 在 arm64 QEMU virt 上通常不会被实际用到，
             * 但这里仍然打印出来，便于后续扩展到更完整的 PCI 资源管理。
             */
            printk("[pci\tbar ]: %02x:%02x.%x bar%u io=%#x\n",
                   bus, dev, fn, i, bar & ~0x3U);
            continue;
        }

        if ((bar & 0x6) == 0x4 && i + 1 < bar_count)
        {
            uint64_t bar_hi = pci_cfg_read32(bus, dev, fn, PCI_BAR0 + (uint16_t)((i + 1) * 4));
            uint64_t addr = ((bar_hi << 32) | (bar & ~0xfU));
            printk("[pci\tbar ]: %02x:%02x.%x bar%u mem64=%#lx\n",
                   bus, dev, fn, i, addr);
            i++;
            continue;
        }

        printk("[pci\tbar ]: %02x:%02x.%x bar%u mem32=%#x\n",
               bus, dev, fn, i, bar & ~0xfU);
    }
}

static void pci_probe_function(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint16_t vendor;
    uint16_t device;
    uint16_t command;
    uint16_t status;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t header_type;

    vendor = pci_cfg_read16(bus, dev, fn, PCI_VENDOR_ID);
    if (vendor == 0xffffU)
    {
        return;
    }

    device = pci_cfg_read16(bus, dev, fn, PCI_DEVICE_ID);
    command = pci_cfg_read16(bus, dev, fn, PCI_COMMAND);
    status = pci_cfg_read16(bus, dev, fn, PCI_STATUS);
    revision = pci_cfg_read8(bus, dev, fn, PCI_REVISION_ID);
    prog_if = pci_cfg_read8(bus, dev, fn, PCI_PROG_IF);
    subclass = pci_cfg_read8(bus, dev, fn, PCI_SUBCLASS);
    class_code = pci_cfg_read8(bus, dev, fn, PCI_CLASS_CODE);
    header_type = pci_cfg_read8(bus, dev, fn, PCI_HEADER_TYPE) & PCI_HEADER_TYPE_MASK;

    pci_state.nr_devices++;

    printk("[pci\tinit]: %02x:%02x.%x vendor=%#x device=%#x class=%#x:%#x prog-if=%#x rev=%#x cmd=%#x sts=%#x\n",
           bus, dev, fn, vendor, device, class_code, subclass, prog_if, revision, command, status);

    pci_dump_bars(bus, dev, fn, header_type);
}

static void pci_probe_device(uint8_t bus, uint8_t dev)
{
    uint8_t header_type;
    uint8_t max_fn;
    uint8_t fn;

    if (pci_cfg_read16(bus, dev, 0, PCI_VENDOR_ID) == 0xffffU)
    {
        return;
    }

    header_type = pci_cfg_read8(bus, dev, 0, PCI_HEADER_TYPE);
    max_fn = (header_type & PCI_HEADER_TYPE_MULTI) ? PCI_MAX_FUNCTIONS : 1;

    for (fn = 0; fn < max_fn; fn++)
    {
        pci_probe_function(bus, dev, fn);
    }
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg)
{
    if (!pci_state.ready || dev >= PCI_MAX_DEVICES || fn >= PCI_MAX_FUNCTIONS)
    {
        return 0xffffffffU;
    }

    return get32(pci_cfg_addr(bus, dev, fn, reg));
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg, uint32_t value)
{
    if (!pci_state.ready || dev >= PCI_MAX_DEVICES || fn >= PCI_MAX_FUNCTIONS)
    {
        return;
    }

    put32(pci_cfg_addr(bus, dev, fn, reg), value);
}

void pci_init(void)
{
    uint16_t bus;
    uint8_t dev;

    pci_state.ready = true;
    pci_state.nr_devices = 0;

    printk("[pci\tinit]: ecam=%#lx..%#lx\n",
           PCIE_ECAM_BASE, PCIE_ECAM_BASE + PCIE_ECAM_SIZE);

    /*
     * 这里先做一版最直接的全总线扫描。
     * ECAM 地址计算是纯规则化的，所以即使还没有 bridge 拓扑管理，
     * 也能先把当前系统里的设备完整扫出来。
     */
    for (bus = 0; bus < PCI_SCAN_BUS_LIMIT; bus++)
    {
        for (dev = 0; dev < PCI_MAX_DEVICES; dev++)
        {
            pci_probe_device((uint8_t)bus, dev);
        }
    }

    printk("[pci\tinit]: %u device function(s) discovered\n", pci_state.nr_devices);
}
