#include "driver/virtio_blk.h"
#include "asm/barrier.h"
#include "errno.h"
#include "lib/libasm.h"
#include "lib/librw.h"
#include "lib/libmem.h"
#include "mmu.h"
#include "printk.h"

/*
 * 当前块设备层只实现一个最小可用的 virtio-mmio block 驱动：
 * 1. 单队列、轮询完成
 * 2. 通过 bounce buffer 处理多扇区读写
 * 3. 在打开 D-cache 后显式做 DMA cache 同步
 */
#define VIRTIO_MMIO_MAGIC_VALUE         0x000
#define VIRTIO_MMIO_VERSION             0x004
#define VIRTIO_MMIO_DEVICE_ID           0x008
#define VIRTIO_MMIO_VENDOR_ID           0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_READY         0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064
#define VIRTIO_MMIO_STATUS              0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW     0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH    0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW      0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH     0x0a4
#define VIRTIO_MMIO_CONFIG              0x100

#define VIRTIO_MAGIC                    0x74726976
#define VIRTIO_VERSION_2                2
#define VIRTIO_DEVICE_BLOCK             2
#define VIRTIO_VENDOR_QEMU              0x554d4551

#define VIRTIO_STATUS_ACKNOWLEDGE       1
#define VIRTIO_STATUS_DRIVER            2
#define VIRTIO_STATUS_DRIVER_OK         4
#define VIRTIO_STATUS_FEATURES_OK       8
#define VIRTIO_STATUS_FAILED            128

#define VIRTIO_F_VERSION_1              32

#define VIRTQ_DESC_F_NEXT               1
#define VIRTQ_DESC_F_WRITE              2

#define VIRTIO_BLK_T_IN                 0
#define VIRTIO_BLK_T_OUT                1
#define VIRTIO_BLK_S_OK                 0

#define VIRTIO_BLK_QUEUE_SIZE           8
#define VIRTIO_BLK_MAX_XFER_SECTORS     8

struct virtq_desc
{
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail
{
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_BLK_QUEUE_SIZE];
} __attribute__((packed));

struct virtq_used_elem
{
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used
{
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VIRTIO_BLK_QUEUE_SIZE];
} __attribute__((packed));

struct virtio_blk_req
{
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

static struct virtio_blk_state
{
    uint64_t mmio_base;
    uint64_t capacity_sectors;
    bool ready;
} virtio_blk_state;

static struct virtq_desc virtio_blk_desc[VIRTIO_BLK_QUEUE_SIZE] __attribute__((aligned(4096)));
static struct virtq_avail virtio_blk_avail __attribute__((aligned(4096)));
static struct virtq_used virtio_blk_used __attribute__((aligned(4096)));
static struct virtio_blk_req virtio_blk_req_hdr __attribute__((aligned(64)));
static uint8_t virtio_blk_req_status __attribute__((aligned(64)));
static uint8_t virtio_blk_bounce[VIRTIO_BLK_MAX_XFER_SECTORS * VIRTIO_BLK_SECTOR_SIZE] __attribute__((aligned(4096)));
static uint16_t virtio_blk_last_used_idx;

static uint32_t virtio_dcache_line_size(void)
{
    uint64_t ctr;
    uint32_t dminline;

    ctr = read_sysreg(ctr_el0);
    dminline = (ctr >> 16) & 0xf;
    return 4U << dminline;
}

/*
 * virtio-blk 当前通过 DMA 直接访问普通内存。
 * 打开 D-cache 后，提交描述符前必须 clean，设备写回后必须 invalidate，
 * 否则 CPU 和设备会看到不同的内存内容。
 */
static void dma_sync_clean_range(uint64_t addr, size_t len)
{
    uint64_t start;
    uint64_t end;
    uint32_t line_size;

    if (!len)
    {
        return;
    }

    line_size = virtio_dcache_line_size();
    start = addr & ~((uint64_t)line_size - 1);
    end = (addr + len + line_size - 1) & ~((uint64_t)line_size - 1);

    for (; start < end; start += line_size)
    {
        asm volatile("dc cvac, %0" : : "r"(start) : "memory");
    }

    dsb(ish);
}

static void dma_sync_invalidate_range(uint64_t addr, size_t len)
{
    uint64_t start;
    uint64_t end;
    uint32_t line_size;

    if (!len)
    {
        return;
    }

    line_size = virtio_dcache_line_size();
    start = addr & ~((uint64_t)line_size - 1);
    end = (addr + len + line_size - 1) & ~((uint64_t)line_size - 1);

    for (; start < end; start += line_size)
    {
        asm volatile("dc ivac, %0" : : "r"(start) : "memory");
    }

    dsb(ish);
}

static void dma_sync_clean_invalidate_range(uint64_t addr, size_t len)
{
    uint64_t start;
    uint64_t end;
    uint32_t line_size;

    if (!len)
    {
        return;
    }

    line_size = virtio_dcache_line_size();
    start = addr & ~((uint64_t)line_size - 1);
    end = (addr + len + line_size - 1) & ~((uint64_t)line_size - 1);

    for (; start < end; start += line_size)
    {
        asm volatile("dc civac, %0" : : "r"(start) : "memory");
    }

    dsb(ish);
}

static inline uint32_t virtio_mmio_read32(uint64_t base, uint64_t reg)
{
    return get32(base + reg);
}

static inline uint64_t virtio_mmio_read64(uint64_t base, uint64_t reg)
{
    return ((uint64_t)virtio_mmio_read32(base, reg + 4) << 32) | virtio_mmio_read32(base, reg);
}

static inline void virtio_mmio_write32(uint64_t base, uint64_t reg, uint32_t value)
{
    put32(base + reg, value);
}

static inline void virtio_mmio_write64(uint64_t base, uint64_t reg, uint64_t value)
{
    virtio_mmio_write32(base, reg, (uint32_t)value);
    virtio_mmio_write32(base, reg + 4, (uint32_t)(value >> 32));
}

static int virtio_blk_detect(uint64_t base)
{
    if (virtio_mmio_read32(base, VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MAGIC)
    {
        return -1;
    }

    if (virtio_mmio_read32(base, VIRTIO_MMIO_VERSION) != VIRTIO_VERSION_2)
    {
        return -1;
    }

    if (virtio_mmio_read32(base, VIRTIO_MMIO_VENDOR_ID) != VIRTIO_VENDOR_QEMU)
    {
        return -1;
    }

    if (virtio_mmio_read32(base, VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_BLOCK)
    {
        return -1;
    }

    return 0;
}

static int virtio_blk_submit(uint32_t type, uint64_t sector, void *buf, uint32_t sector_count)
{
    uint16_t head;
    uint16_t used_idx;
    uint32_t data_len;
    uint32_t spins;

    if (!virtio_blk_state.ready)
    {
        return -ENODEV;
    }

    if (!sector_count || sector_count > VIRTIO_BLK_MAX_XFER_SECTORS)
    {
        return -EINVAL;
    }

    data_len = sector_count * VIRTIO_BLK_SECTOR_SIZE;

    virtio_blk_req_hdr.type = type;
    virtio_blk_req_hdr.reserved = 0;
    virtio_blk_req_hdr.sector = sector;
    virtio_blk_req_status = 0xff;

    virtio_blk_desc[0].addr = kernel_virt_to_phys((uint64_t)&virtio_blk_req_hdr);
    virtio_blk_desc[0].len = sizeof(virtio_blk_req_hdr);
    virtio_blk_desc[0].flags = VIRTQ_DESC_F_NEXT;
    virtio_blk_desc[0].next = 1;

    virtio_blk_desc[1].addr = kernel_virt_to_phys((uint64_t)buf);
    virtio_blk_desc[1].len = data_len;
    virtio_blk_desc[1].flags = VIRTQ_DESC_F_NEXT | ((type == VIRTIO_BLK_T_IN) ? VIRTQ_DESC_F_WRITE : 0);
    virtio_blk_desc[1].next = 2;

    virtio_blk_desc[2].addr = kernel_virt_to_phys((uint64_t)&virtio_blk_req_status);
    virtio_blk_desc[2].len = 1;
    virtio_blk_desc[2].flags = VIRTQ_DESC_F_WRITE;
    virtio_blk_desc[2].next = 0;

    head = 0;
    used_idx = virtio_blk_last_used_idx;

    if (type == VIRTIO_BLK_T_IN)
    {
        dma_sync_clean_invalidate_range((uint64_t)buf, data_len);
    }
    else
    {
        dma_sync_clean_range((uint64_t)buf, data_len);
    }

    dma_sync_clean_range((uint64_t)&virtio_blk_req_hdr, sizeof(virtio_blk_req_hdr));
    dma_sync_clean_invalidate_range((uint64_t)&virtio_blk_req_status, sizeof(virtio_blk_req_status));
    dma_sync_clean_range((uint64_t)virtio_blk_desc, sizeof(virtio_blk_desc));
    virtio_blk_avail.ring[virtio_blk_avail.idx % VIRTIO_BLK_QUEUE_SIZE] = head;
    dma_sync_clean_range((uint64_t)&virtio_blk_avail.ring[virtio_blk_avail.idx % VIRTIO_BLK_QUEUE_SIZE],
                         sizeof(virtio_blk_avail.ring[0]));
    virtio_blk_avail.idx++;
    dma_sync_clean_range((uint64_t)&virtio_blk_avail, sizeof(virtio_blk_avail));

    dsb(sy);
    virtio_mmio_write32(virtio_blk_state.mmio_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    spins = 0;
    while (virtio_blk_used.idx == used_idx)
    {
        dma_sync_invalidate_range((uint64_t)&virtio_blk_used, sizeof(virtio_blk_used));
        dsb(sy);
        spins++;
        if (spins == 100000000)
        {
            printk("[virtio-blk\tio]: timeout type=%x sector=%x count=%x\n", type, sector, sector_count);
            return -ETIMEDOUT;
        }
    }

    dma_sync_invalidate_range((uint64_t)&virtio_blk_req_status, sizeof(virtio_blk_req_status));
    if (type == VIRTIO_BLK_T_IN)
    {
        dma_sync_invalidate_range((uint64_t)buf, data_len);
    }

    virtio_blk_last_used_idx = virtio_blk_used.idx;
    virtio_mmio_write32(virtio_blk_state.mmio_base, VIRTIO_MMIO_INTERRUPT_ACK, 0x3);

    if (virtio_blk_req_status != VIRTIO_BLK_S_OK)
    {
        return -EIO;
    }

    return 0;
}

uint64_t virtio_blk_capacity_sectors(void)
{
    return virtio_blk_state.capacity_sectors;
}

int virtio_blk_read(uint64_t sector, void *buf, uint32_t sector_count)
{
    uint8_t *dst;
    uint32_t remaining;
    uint32_t chunk;
    int ret;

    dst = (uint8_t *)buf;
    remaining = sector_count;

    while (remaining)
    {
        chunk = remaining;
        if (chunk > VIRTIO_BLK_MAX_XFER_SECTORS)
        {
            chunk = VIRTIO_BLK_MAX_XFER_SECTORS;
        }

        ret = virtio_blk_submit(VIRTIO_BLK_T_IN, sector, virtio_blk_bounce, chunk);
        if (ret)
        {
            return ret;
        }

        memcpy((int8_t *)dst, (int8_t *)virtio_blk_bounce, chunk * VIRTIO_BLK_SECTOR_SIZE);
        sector += chunk;
        dst += chunk * VIRTIO_BLK_SECTOR_SIZE;
        remaining -= chunk;
    }

    return 0;
}

int virtio_blk_write(uint64_t sector, const void *buf, uint32_t sector_count)
{
    const uint8_t *src;
    uint32_t remaining;
    uint32_t chunk;
    int ret;

    src = (const uint8_t *)buf;
    remaining = sector_count;

    while (remaining)
    {
        chunk = remaining;
        if (chunk > VIRTIO_BLK_MAX_XFER_SECTORS)
        {
            chunk = VIRTIO_BLK_MAX_XFER_SECTORS;
        }

        memcpy((int8_t *)virtio_blk_bounce, (int8_t *)src, chunk * VIRTIO_BLK_SECTOR_SIZE);
        ret = virtio_blk_submit(VIRTIO_BLK_T_OUT, sector, virtio_blk_bounce, chunk);
        if (ret)
        {
            return ret;
        }

        sector += chunk;
        src += chunk * VIRTIO_BLK_SECTOR_SIZE;
        remaining -= chunk;
    }

    return 0;
}

int virtio_blk_init(void)
{
    uint64_t base;
    uint32_t features;
    uint32_t queue_max;
    uint32_t status;
    uint32_t slot;

    base = 0;
    for (slot = 0; slot < VIRTIO_MMIO_COUNT; slot++)
    {
        uint64_t probe = VIRTIO_MMIO_HIGH_BASE + ((uint64_t)slot * VIRTIO_MMIO_STRIDE);
        if (virtio_blk_detect(probe) == 0)
        {
            base = probe;
            break;
        }
    }

    if (!base)
    {
        printk("[virtio-blk\tinit]: no virtio-mmio block device found\n");
        return -ENODEV;
    }

    virtio_blk_state.mmio_base = base;

    virtio_mmio_write32(base, VIRTIO_MMIO_STATUS, 0);
    status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    virtio_mmio_write32(base, VIRTIO_MMIO_STATUS, status);

    virtio_mmio_write32(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    features = virtio_mmio_read32(base, VIRTIO_MMIO_DEVICE_FEATURES);
    if ((features & (1U << (VIRTIO_F_VERSION_1 - 32))) == 0)
    {
        printk("[virtio-blk\tinit]: device does not support virtio 1.0\n");
        return -ENOTSUP;
    }

    virtio_mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    virtio_mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    virtio_mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    virtio_mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES, (1U << (VIRTIO_F_VERSION_1 - 32)));

    status |= VIRTIO_STATUS_FEATURES_OK;
    virtio_mmio_write32(base, VIRTIO_MMIO_STATUS, status);
    if ((virtio_mmio_read32(base, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0)
    {
        printk("[virtio-blk\tinit]: feature negotiation failed\n");
        return -EIO;
    }

    virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_SEL, 0);
    queue_max = virtio_mmio_read32(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max < VIRTIO_BLK_QUEUE_SIZE)
    {
        printk("[virtio-blk\tinit]: queue too small (%u)\n", queue_max);
        return -ENOSPC;
    }

    memset((int8_t *)virtio_blk_desc, 0, sizeof(virtio_blk_desc));
    memset((int8_t *)&virtio_blk_avail, 0, sizeof(virtio_blk_avail));
    memset((int8_t *)&virtio_blk_used, 0, sizeof(virtio_blk_used));

    virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_BLK_QUEUE_SIZE);
    virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_DESC_LOW, kernel_virt_to_phys((uint64_t)virtio_blk_desc));
    virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_AVAIL_LOW, kernel_virt_to_phys((uint64_t)&virtio_blk_avail));
    virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_USED_LOW, kernel_virt_to_phys((uint64_t)&virtio_blk_used));
    virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_READY, 1);

    virtio_blk_state.capacity_sectors = virtio_mmio_read64(base, VIRTIO_MMIO_CONFIG);
    virtio_blk_state.ready = true;
    virtio_blk_last_used_idx = 0;

    status |= VIRTIO_STATUS_DRIVER_OK;
    virtio_mmio_write32(base, VIRTIO_MMIO_STATUS, status);

    printk("[virtio-blk\tinit]: base=%#lx capacity=%llu sector(s)\n", base, virtio_blk_state.capacity_sectors);
    return 0;
}
