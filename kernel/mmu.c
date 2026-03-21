#include "mmu.h"
#include "fdt.h"
#include "driver/fwcfg.h"
#include "driver/uart.h"
#include "driver/virtio_blk.h"
#include "gicv2.h"
#include "printk.h"

#define GIC_MMIO_SIZE       0x20000UL
#define FW_CFG_MMIO_SIZE    PAGE_SIZE

struct early_page_pool
{
    uint64_t pud_base;
    uint64_t pmd_base;
    uint64_t pte_base;
    uint32_t pud_next;
    uint32_t pmd_next;
    uint32_t pte_next;
    uint32_t pud_max;
    uint32_t pmd_max;
    uint32_t pte_max;
};

static struct early_page_pool idmap_pool;
static struct early_page_pool swapper_pool;

static inline uint64_t page_start(uint64_t addr)
{
    return PAGE_ALIGN_DOWN(addr);
}

static inline uint64_t page_end(uint64_t addr)
{
    return PAGE_ALIGN_UP(addr);
}

static inline uint64_t pmd_start(uint64_t addr)
{
    return addr & PMD_MASK;
}

static inline uint64_t pmd_end(uint64_t addr)
{
    return (addr + PMD_SIZE - 1) & PMD_MASK;
}

static inline pte_t pfn_pte(uint64_t pa, uint64_t prot)
{
    return (pte_t){
        .pte = (pa & PTE_MASK) | prot
    };
}

static inline pmd_t pfn_pmd_block(uint64_t pa, uint64_t prot)
{
    return (pmd_t){
        .pmd = (pa & PMD_MASK) | prot
    };
}

static inline int pmd_table(pmd_t pmd)
{
    return (pmd_val(pmd) & PMD_TYPE_MASK) == PMD_TYPE_TABLE;
}

static uint64_t mmu_boot_memory_size(void)
{
    uint64_t size;

    size = fdt_memory_size();
    if (!size || size > TOTAL_MEMORY)
    {
        size = TOTAL_MEMORY;
    }

    return size;
}

static void dump_mapping(const char *name, pgd_t *root, uint64_t va)
{
    pgd_t *pgdp;
    pud_t *pudp;
    pmd_t *pmdp;
    pte_t *ptep;

    pgdp = pgd_offset_raw(root, va);
    printk("[mmu\tmap]: %s va=%#lx pgd=%#lx\n", name, va, pgd_val(*pgdp));
    if (pgd_none(*pgdp))
    {
        return;
    }

    pudp = pud_offset_phys(pgdp, va);
    printk("[mmu\tmap]: %s pud=%#lx\n", name, pud_val(*pudp));
    if (pud_none(*pudp))
    {
        return;
    }

    pmdp = pmd_offset_phys(pudp, va);
    printk("[mmu\tmap]: %s pmd=%#lx\n", name, pmd_val(*pmdp));
    if (pmd_none(*pmdp) || pmd_sect(*pmdp))
    {
        return;
    }

    ptep = pte_offset_phys(pmdp, va);
    printk("[mmu\tmap]: %s pte=%#lx\n", name, pte_val(*ptep));
}

static void early_page_pool_init(struct early_page_pool *pool,
                                 uint64_t pud_base, uint32_t pud_max,
                                 uint64_t pmd_base, uint32_t pmd_max,
                                 uint64_t pte_base, uint32_t pte_max)
{
    pool->pud_base = pud_base;
    pool->pmd_base = pmd_base;
    pool->pte_base = pte_base;
    pool->pud_next = 0;
    pool->pmd_next = 0;
    pool->pte_next = 0;
    pool->pud_max = pud_max;
    pool->pmd_max = pmd_max;
    pool->pte_max = pte_max;
}

/*
 * 从早期页表池里按页分配新表页。
 * 这一步发生在最早期，还没有通用页分配器，因此使用最简单的线性分配。
 */
static uint64_t alloc_table_page(uint64_t base, uint32_t *next, uint32_t max, const char *name)
{
    uint64_t table;

    if (*next >= max)
    {
        printk("[mmu\tinit]: early %s table pool exhausted\n", name);
        return 0;
    }

    table = base + ((uint64_t)(*next) * PAGE_SIZE);
    (*next)++;

    memset((void *)table, 0, PAGE_SIZE);
    return table;
}

static pud_t *pud_alloc(struct early_page_pool *pool, pgd_t *pgdp, uint64_t va)
{
    uint64_t table;

    if (pgd_none(*pgdp))
    {
        table = alloc_table_page(pool->pud_base, &pool->pud_next, pool->pud_max, "PUD");
        if (!table)
        {
            return 0;
        }

        set_pgd(pgdp, (pgd_t){ .pgd = (table & PAGE_MASK) | PUD_TYPE_TABLE });
    }

    return pud_offset_phys(pgdp, va);
}

static pmd_t *pmd_alloc(struct early_page_pool *pool, pud_t *pudp, uint64_t va)
{
    uint64_t table;

    if (pud_none(*pudp))
    {
        table = alloc_table_page(pool->pmd_base, &pool->pmd_next, pool->pmd_max, "PMD");
        if (!table)
        {
            return 0;
        }

        set_pud(pudp, (pud_t){ .pud = (table & PAGE_MASK) | PMD_TYPE_TABLE });
    }

    return pmd_offset_phys(pudp, va);
}

static pte_t *pte_alloc(struct early_page_pool *pool, pmd_t *pmdp, uint64_t va)
{
    uint64_t table;

    if (pmd_none(*pmdp))
    {
        table = alloc_table_page(pool->pte_base, &pool->pte_next, pool->pte_max, "PTE");
        if (!table)
        {
            return 0;
        }

        set_pmd(pmdp, (pmd_t){ .pmd = (table & PAGE_MASK) | PMD_TYPE_TABLE });
    }

    if (pmd_sect(*pmdp))
    {
        printk("[mmu\tinit]: PMD block/table conflict at va=%#lx\n", va);
        return 0;
    }

    return pte_offset_phys(pmdp, va);
}

/*
 * 页粒度映射，主要给内核镜像用。
 * 因为 .text/.rodata/.data/.bss 的权限不同，所以这里必须精细到页。
 */
static int map_range_pte(pgd_t *root, struct early_page_pool *pool,
                         uint64_t va_start, uint64_t pa_start,
                         uint64_t size, uint64_t prot)
{
    pgd_t *pgdp;
    pud_t *pudp;
    pmd_t *pmdp;
    pte_t *ptep;
    uint64_t va;
    uint64_t pa;
    uint64_t end;

    if (!size)
    {
        return 0;
    }

    if (((va_start ^ pa_start) & (PAGE_SIZE - 1)) != 0)
    {
        printk("[mmu\tinit]: unaligned PTE map va=%#lx pa=%#lx\n", va_start, pa_start);
        return -1;
    }

    va = page_start(va_start);
    pa = page_start(pa_start);
    end = page_end(va_start + size);

    for (; va < end; va += PAGE_SIZE, pa += PAGE_SIZE)
    {
        pgdp = pgd_offset_raw(root, va);
        pudp = pud_alloc(pool, pgdp, va);
        if (!pudp)
        {
            return -1;
        }

        pmdp = pmd_alloc(pool, pudp, va);
        if (!pmdp)
        {
            return -1;
        }

        ptep = pte_alloc(pool, pmdp, va);
        if (!ptep)
        {
            return -1;
        }

        set_pte(ptep, pfn_pte(pa, prot));
    }

    return 0;
}

/*
 * PMD block 映射，主要给 linear map 用。
 * 整段 RAM 在 linear map 里权限统一为 RW + XN，因此用 2MB block 更接近 Linux。
 */
static int map_range_pmd_block(pgd_t *root, struct early_page_pool *pool,
                               uint64_t va_start, uint64_t pa_start,
                               uint64_t size, uint64_t prot)
{
    pgd_t *pgdp;
    pud_t *pudp;
    pmd_t *pmdp;
    uint64_t va;
    uint64_t pa;
    uint64_t end;

    if (!size)
    {
        return 0;
    }

    if (((va_start ^ pa_start) & (PMD_SIZE - 1)) != 0)
    {
        printk("[mmu\tinit]: unaligned PMD map va=%#lx pa=%#lx\n", va_start, pa_start);
        return -1;
    }

    if ((size & (PMD_SIZE - 1)) != 0)
    {
        printk("[mmu\tinit]: PMD map size must be 2MB aligned\n");
        return -1;
    }

    va = pmd_start(va_start);
    pa = pmd_start(pa_start);
    end = va_start + size;

    for (; va < end; va += PMD_SIZE, pa += PMD_SIZE)
    {
        pgdp = pgd_offset_raw(root, va);
        pudp = pud_alloc(pool, pgdp, va);
        if (!pudp)
        {
            return -1;
        }

        pmdp = pmd_alloc(pool, pudp, va);
        if (!pmdp)
        {
            return -1;
        }

        if (!pmd_none(*pmdp))
        {
            printk("[mmu\tinit]: PMD entry already in use at va=%#lx\n", va);
            return -1;
        }

        set_pmd(pmdp, pfn_pmd_block(pa, prot));
    }

    return 0;
}

static int map_kernel_image_segment(pgd_t *root, struct early_page_pool *pool,
                                    uint64_t phys_start, uint64_t phys_end,
                                    uint64_t prot)
{
    return map_range_pte(root, pool,
                         kimage_phys_to_virt(phys_start), phys_start,
                         phys_end - phys_start, prot);
}

/*
 * TTBR0 使用的低地址 idmap：
 * 1. 内核镜像的低地址 identity map，用于打开 MMU 的瞬间继续执行
 * 2. 仍然使用低地址常量访问的 MMIO
 */
static int build_idmap(void)
{
    memset((void *)&idmap_pgd, 0, PAGE_SIZE);
    memset((void *)&idmap_pud, 0, IDMAP_PUD_TABLES * PAGE_SIZE);
    memset((void *)&idmap_pmd, 0, IDMAP_PMD_TABLES * PAGE_SIZE);
    memset((void *)&idmap_pt, 0, IDMAP_PTE_TABLES * PAGE_SIZE);

    early_page_pool_init(&idmap_pool,
                         (uint64_t)&idmap_pud, IDMAP_PUD_TABLES,
                         (uint64_t)&idmap_pmd, IDMAP_PMD_TABLES,
                         (uint64_t)&idmap_pt, IDMAP_PTE_TABLES);

    if (map_range_pte((pgd_t *)&idmap_pgd, &idmap_pool,
                      (uint64_t)&__text_start, (uint64_t)&__text_start,
                      (uint64_t)&__text_end - (uint64_t)&__text_start, PAGE_KERNEL_ROX))
    {
        return -1;
    }

    if (map_range_pte((pgd_t *)&idmap_pgd, &idmap_pool,
                      (uint64_t)&__rodata_start, (uint64_t)&__rodata_start,
                      (uint64_t)&__rodata_end - (uint64_t)&__rodata_start, PAGE_KERNEL_RO))
    {
        return -1;
    }

    if (map_range_pte((pgd_t *)&idmap_pgd, &idmap_pool,
                      (uint64_t)&__data_start, (uint64_t)&__data_start,
                      (uint64_t)&__data_end - (uint64_t)&__data_start, PAGE_KERNEL))
    {
        return -1;
    }

    if (map_range_pte((pgd_t *)&idmap_pgd, &idmap_pool,
                      (uint64_t)&__bss_start, (uint64_t)&__bss_start,
                      (uint64_t)&__bss_end - (uint64_t)&__bss_start, PAGE_KERNEL))
    {
        return -1;
    }

    if (map_range_pte((pgd_t *)&idmap_pgd, &idmap_pool,
                      PL011_UART0_BASE, PL011_UART0_BASE, PL011_REG_SIZE, PAGE_KERNEL_DEVICE))
    {
        return -1;
    }

    if (map_range_pte((pgd_t *)&idmap_pgd, &idmap_pool,
                      GIC_BASE, GIC_BASE, GIC_MMIO_SIZE, PAGE_KERNEL_DEVICE))
    {
        return -1;
    }

    if (map_range_pte((pgd_t *)&idmap_pgd, &idmap_pool,
                      FW_CFG_BASE, FW_CFG_BASE, FW_CFG_MMIO_SIZE, PAGE_KERNEL_DEVICE))
    {
        return -1;
    }

    if (map_range_pte((pgd_t *)&idmap_pgd, &idmap_pool,
                      VIRTIO_MMIO0_BASE, VIRTIO_MMIO0_BASE, VIRTIO_MMIO_SIZE, PAGE_KERNEL_DEVICE))
    {
        return -1;
    }

    return 0;
}

/*
 * TTBR1 使用的高地址内核页表：
 * 1. linear map：整段 RAM 连续映射到 PAGE_OFFSET
 * 2. kernel image：内核镜像单独映射到 KIMAGE_VADDR
 */
static int build_swapper_map(void)
{
    memset((void *)&swapper_pgd, 0, PAGE_SIZE);
    memset((void *)&swapper_pud, 0, SWAPPER_PUD_TABLES * PAGE_SIZE);
    memset((void *)&swapper_pmd, 0, SWAPPER_PMD_TABLES * PAGE_SIZE);
    memset((void *)&swapper_pt, 0, SWAPPER_PTE_TABLES * PAGE_SIZE);

    early_page_pool_init(&swapper_pool,
                         (uint64_t)&swapper_pud, SWAPPER_PUD_TABLES,
                         (uint64_t)&swapper_pmd, SWAPPER_PMD_TABLES,
                         (uint64_t)&swapper_pt, SWAPPER_PTE_TABLES);

    if (map_range_pmd_block((pgd_t *)&swapper_pgd, &swapper_pool,
                            PAGE_OFFSET, PHYS_OFFSET, mmu_boot_memory_size(), PMD_KERNEL))
    {
        return -1;
    }

    if (map_kernel_image_segment((pgd_t *)&swapper_pgd, &swapper_pool,
                                 (uint64_t)&__text_start, (uint64_t)&__text_end, PAGE_KERNEL_ROX))
    {
        return -1;
    }

    if (map_kernel_image_segment((pgd_t *)&swapper_pgd, &swapper_pool,
                                 (uint64_t)&__rodata_start, (uint64_t)&__rodata_end, PAGE_KERNEL_RO))
    {
        return -1;
    }

    if (map_kernel_image_segment((pgd_t *)&swapper_pgd, &swapper_pool,
                                 (uint64_t)&__data_start, (uint64_t)&__data_end, PAGE_KERNEL))
    {
        return -1;
    }

    if (map_kernel_image_segment((pgd_t *)&swapper_pgd, &swapper_pool,
                                 (uint64_t)&__bss_start, (uint64_t)&__bss_end, PAGE_KERNEL))
    {
        return -1;
    }

    if (map_range_pte((pgd_t *)&swapper_pgd, &swapper_pool,
                      VIRTIO_MMIO_HIGH_BASE, VIRTIO_MMIO0_BASE,
                      VIRTIO_MMIO_SIZE, PAGE_KERNEL_DEVICE))
    {
        return -1;
    }

    /*
     * PCIe ECAM 配置空间位于 4GB 以上，不能继续依赖低地址常量访问。
     * 这里给它建立一个独立的高地址设备映射窗口，后续 PCI 子系统统一通过
     * PCIE_ECAM_HIGH_BASE 访问配置空间。
     */
    if (map_range_pmd_block((pgd_t *)&swapper_pgd, &swapper_pool,
                            PCIE_ECAM_HIGH_BASE, PCIE_ECAM_BASE,
                            PCIE_ECAM_SIZE, PMD_KERNEL_DEVICE))
    {
        return -1;
    }

    return 0;
}

/*
 * 配置 MAIR/TCR 等控制寄存器。
 * 这一步只是准备好地址翻译环境，真正打开 MMU 在 enable_mmu() 里完成。
 */
void mmu_init(void)
{
    uint64_t mair;
    uint64_t tcr;
    uint64_t mmfr0;
    uint64_t parange;

    asm volatile(
        "tlbi vmalle1"
        :
        :
        : "memory"
    );
    dsb(nsh);

    write_sysreg(3UL << 20, cpacr_el1);
    write_sysreg(1UL << 12, mdscr_el1);

    mair = MAIR(MAIR_DEVICE_nGnRnE_ATTR, MT_DEVICE_nGnRnE_IDX) |
           MAIR(MAIR_DEVICE_nGnRE_ATTR, MT_DEVICE_nGnRE_IDX) |
           MAIR(MAIR_DEVICE_GRE_ATTR, MT_DEVICE_GRE_IDX) |
           MAIR(MAIR_NORMAL_NC_ATTR, MT_NORMAL_NC_IDX) |
           MAIR(MAIR_NORMAL_ATTR, MT_NORMAL_IDX) |
           MAIR(MAIR_NORMAL_WT_ATTR, MT_NORMAL_WT_IDX);
    write_sysreg(mair, mair_el1);

    tcr = TCR_TxSZ(VA_BITS) | TCR_TG_FLAGS | TCR_CACHE_FLAGS | TCR_SMP_FLAGS;

    mmfr0 = read_sysreg(ID_AA64MMFR0_EL1);
    parange = mmfr0 & 0xf;
    if (parange > ID_AA64MMFR0_PARANGE_48)
    {
        parange = ID_AA64MMFR0_PARANGE_48;
    }

    tcr |= parange << TCR_IPS_SHIFT;
    write_sysreg(tcr, tcr_el1);

    printk("[mmu\tinit]: mair/tcr configured\n");
}

/*
 * 构建启动阶段需要的所有映射。
 *
 * 最终页表分工如下：
 * - TTBR0 / idmap_pgd:
 *   低地址内核 identity map + 低地址 MMIO
 * - TTBR1 / swapper_pgd:
 *   高地址 linear map + 独立的 KIMAGE_VADDR 内核镜像区
 */
int page_map_init(void)
{
    uint64_t kernel_start;
    uint64_t kernel_end;

    kernel_start = page_start((uint64_t)&__kernel_start);
    kernel_end = page_end((uint64_t)&__kernel_end);

    if (build_idmap())
    {
        return -1;
    }

    if (build_swapper_map())
    {
        return -1;
    }

    printk("[mmu\tinit]: idmap ready [%#lx, %#lx)\n", kernel_start, kernel_end);
    printk("[mmu\tinit]: linear map ready [%#018lx, %#018lx)\n",
           PAGE_OFFSET, PAGE_OFFSET + mmu_boot_memory_size());
    printk("[mmu\tinit]: kimage map ready at KIMAGE_VADDR\n");
    dump_mapping("kimage", (pgd_t *)&swapper_pgd, KIMAGE_VADDR);
    dump_mapping("virtio-hi", (pgd_t *)&swapper_pgd, VIRTIO_MMIO_HIGH_BASE);
    dump_mapping("pcie-ecam", (pgd_t *)&swapper_pgd, PCIE_ECAM_HIGH_BASE);
    dump_mapping("virtio-lo", (pgd_t *)&idmap_pgd, VIRTIO_MMIO0_BASE);

    return 0;
}

/*
 * 真正打开 MMU。
 *
 * 这里比“单一高地址别名”更接近 Linux arm64：
 * 1. TTBR0 指向 idmap，负责低地址过渡和低地址 MMIO
 * 2. TTBR1 指向 swapper，负责高地址 linear map 和 KIMAGE
 */
int enable_mmu(void)
{
    uint64_t mmfr0;
    uint64_t tgran4;
    uint64_t sctlr;

    mmfr0 = read_sysreg(ID_AA64MMFR0_EL1);
    tgran4 = (mmfr0 >> ID_AA64MMFR0_TGRAN4_SHIFT) & 0xf;
    if (tgran4 != ID_AA64MMFR0_TGRAN4_SUPPORTED)
    {
        printk("[mmu\tinit]: 4KB granule not supported\n");
        return -1;
    }

    dsb(ishst);
    write_sysreg((uint64_t)&idmap_pgd, ttbr0_el1);
    write_sysreg((uint64_t)&swapper_pgd, ttbr1_el1);
    isb();

    sctlr = read_sysreg(sctlr_el1);
    sctlr |= SCTLR_ELx_M | SCTLR_ELx_C | SCTLR_ELx_I;
    write_sysreg(sctlr, sctlr_el1);
    isb();

    asm volatile("ic iallu");
    dsb(nsh);
    isb();

    printk("[mmu\tinit]: mmu enabled\n");
    return 0;
}

/*
 * 次级核不需要再次构建页表，只要把 CPU 本地的 MAIR/TCR/TTBR/SCTLR
 * 编程成和启动核一致即可。
 */
int mmu_secondary_init(void)
{
    return enable_mmu();
}
