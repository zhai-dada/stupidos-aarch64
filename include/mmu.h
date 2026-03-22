#ifndef __MMU_H__
#define __MMU_H__

#include "lib/libmem.h"
#include "asm/pagetable.h"
#include "asm/pagetable_hwdef.h"
#include "asm/barrier.h"
#include "asm/types.h"
#include "driver/virtio_blk.h"
#include "debug.h"
#include "asm/sysreg.h"
#include "pci.h"

extern uint64_t idmap_pgd;
extern uint64_t idmap_pud;
extern uint64_t idmap_pmd;
extern uint64_t idmap_pt;

extern uint64_t swapper_pgd;
extern uint64_t swapper_pud;
extern uint64_t swapper_pmd;
extern uint64_t swapper_pt;
extern uint64_t init_stack_end;

extern uint64_t __kernel_start, __kernel_end;
extern uint64_t __text_start, __text_end;
extern uint64_t __rodata_start, __rodata_end;
extern uint64_t __data_start, __data_end;
extern uint64_t __bss_start, __bss_end;
extern uint64_t vectors;

/*
 * 早期页表仍然放在内核镜像内部，因此这里预留一小块静态页表池。
 * 当前只映射：
 * 1. 内核镜像本身
 * 2. 开机阶段会访问到的少量 MMIO
 * 所以固定数量的表页已经足够。
 */
#define IDMAP_PUD_TABLES        2
#define IDMAP_PMD_TABLES        4
#define IDMAP_PTE_TABLES        16

#define SWAPPER_PUD_TABLES      4
#define SWAPPER_PMD_TABLES      8
#define SWAPPER_PTE_TABLES      32

/*
 * 这里采用更接近 Linux arm64 的双区域布局：
 * 1. linear map：把整段 RAM 连续映射到高地址 PAGE_OFFSET
 * 2. kernel image：把内核镜像单独映射到另一个高地址窗口 KIMAGE_VADDR
 *
 * 同时保留一个低地址 idmap，仅用于打开 MMU 这一小段过渡过程。
 */
#define PHYS_OFFSET         0x40000000UL
#define PAGE_OFFSET         0xFFFF000000000000UL
#define KIMAGE_VADDR        0xFFFF800010000000UL

static inline uint64_t linear_phys_to_virt(uint64_t phys)
{
    return PAGE_OFFSET + (phys - PHYS_OFFSET);
}

static inline uint64_t linear_virt_to_phys(uint64_t virt)
{
    return PHYS_OFFSET + (virt - PAGE_OFFSET);
}

static inline uint64_t kimage_phys_to_virt(uint64_t phys)
{
    return KIMAGE_VADDR + (phys - PHYS_OFFSET);
}

static inline uint64_t kimage_virt_to_phys(uint64_t virt)
{
    return PHYS_OFFSET + (virt - KIMAGE_VADDR);
}

static inline uint64_t virtio_mmio_virt_to_phys(uint64_t virt)
{
    return VIRTIO_MMIO0_BASE + (virt - VIRTIO_MMIO_HIGH_BASE);
}

static inline uint64_t pcie_ecam_virt_to_phys(uint64_t virt)
{
    return PCIE_ECAM_BASE + (virt - PCIE_ECAM_HIGH_BASE);
}

static inline uint64_t kernel_virt_to_phys(uint64_t virt)
{
    /*
     * 先匹配高地址 MMIO 窗口，再判断内核镜像别名。
     * 否则像 0xffff9000... 这样的 virtio 高地址别名会被误判成
     * KIMAGE_VADDR 区域，反推出完全错误的“物理地址”。
     */
    if (virt >= VIRTIO_MMIO_HIGH_BASE &&
        virt < (VIRTIO_MMIO_HIGH_BASE + VIRTIO_MMIO_SIZE))
    {
        return virtio_mmio_virt_to_phys(virt);
    }

    if (virt >= PCIE_ECAM_HIGH_BASE &&
        virt < (PCIE_ECAM_HIGH_BASE + PCIE_ECAM_SIZE))
    {
        return pcie_ecam_virt_to_phys(virt);
    }

    if (virt >= KIMAGE_VADDR)
    {
        return kimage_virt_to_phys(virt);
    }

    if (virt >= PAGE_OFFSET)
    {
        return linear_virt_to_phys(virt);
    }

    return virt;
}

void mmu_init(void);
int enable_mmu(void);
int page_map_init(void);
int mmu_secondary_init(void);

#endif
