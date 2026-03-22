#include "driver/virtio_net.h"

#include "asm/barrier.h"
#include "driver/virtio_blk.h"
#include "fdt.h"
#include "errno.h"
#include "gicv2.h"
#include "irq.h"
#include "lib/libasm.h"
#include "lib/librw.h"
#include "lib/libmem.h"
#include "mmu.h"
#include "net/net.h"
#include "printk.h"
#include "sched.h"
#include "softirq.h"
#include "spinlock.h"

#define VIRTIO_MMIO_MAGIC_VALUE         0x000
#define VIRTIO_MMIO_VERSION             0x004
#define VIRTIO_MMIO_DEVICE_ID           0x008
#define VIRTIO_MMIO_VENDOR_ID           0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL  0x014
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL  0x024
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_READY         0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060
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
#define VIRTIO_DEVICE_NET               1
#define VIRTIO_VENDOR_QEMU              0x554d4551

#define VIRTIO_STATUS_ACKNOWLEDGE       1
#define VIRTIO_STATUS_DRIVER            2
#define VIRTIO_STATUS_DRIVER_OK         4
#define VIRTIO_STATUS_FEATURES_OK       8
#define VIRTIO_STATUS_FAILED            128

#define VIRTIO_F_VERSION_1              32

#define VIRTQ_DESC_F_NEXT               1
#define VIRTQ_DESC_F_WRITE              2

#define VIRTIO_NET_HDR_F_NEEDS_CSUM     1
#define VIRTIO_NET_HDR_GSO_NONE         0

#define VIRTIO_NET_QUEUE_RX             0
#define VIRTIO_NET_QUEUE_TX             1
#define VIRTIO_NET_QUEUE_SIZE           8
#define VIRTIO_NET_RX_BUF_SIZE          2048
#define VIRTIO_NET_TX_BUF_SIZE          2048
#define VIRTIO_NET_HDR_LEN              10

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
    uint16_t ring[VIRTIO_NET_QUEUE_SIZE];
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
    struct virtq_used_elem ring[VIRTIO_NET_QUEUE_SIZE];
} __attribute__((packed));

struct virtio_net_hdr
{
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

struct virtio_net_frame
{
    struct virtio_net_hdr hdr;
    uint8_t payload[VIRTIO_NET_RX_BUF_SIZE];
} __attribute__((packed));

struct virtio_net_state
{
    uint64_t mmio_base;
    bool ready;
    bool irq_ready;
    uint32_t irq;
    uint32_t irq_count;
    struct net_device dev;
    struct tasklet_struct rx_tasklet;
    struct virtq_desc rx_desc[VIRTIO_NET_QUEUE_SIZE] __attribute__((aligned(4096)));
    struct virtq_avail rx_avail __attribute__((aligned(4096)));
    struct virtq_used rx_used __attribute__((aligned(4096)));
    struct virtio_net_frame rx_buf[VIRTIO_NET_QUEUE_SIZE] __attribute__((aligned(4096)));
    uint16_t rx_last_used;
    struct virtq_desc tx_desc __attribute__((aligned(4096)));
    struct virtq_avail tx_avail __attribute__((aligned(4096)));
    struct virtq_used tx_used __attribute__((aligned(4096)));
    struct virtio_net_frame tx_buf __attribute__((aligned(4096)));
    uint16_t tx_last_used;
    spinlock_t tx_lock;
};

static struct virtio_net_state virtio_net_state;
static bool virtio_net_irq_stable;

static void virtio_net_irq_handle(void);

static uint32_t virtio_dcache_line_size(void)
{
    uint64_t ctr;
    uint32_t dminline;

    ctr = read_sysreg(ctr_el0);
    dminline = (ctr >> 16) & 0xf;
    return 4U << dminline;
}

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
    dma_sync_clean_range(addr, len);
    dma_sync_invalidate_range(addr, len);
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

static int virtio_net_detect(uint64_t base)
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

    if (virtio_mmio_read32(base, VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_NET)
    {
        return -1;
    }

    return 0;
}

static void virtio_net_read_mac(uint64_t base, uint8_t mac[6])
{
    uint64_t raw;

    raw = virtio_mmio_read64(base, VIRTIO_MMIO_CONFIG);
    memcpy((int8_t *)mac, (int8_t *)&raw, 6);
}

static void virtio_net_queue_rx(struct virtio_net_state *st, uint32_t slot)
{
    st->rx_desc[slot].addr = kernel_virt_to_phys((uint64_t)&st->rx_buf[slot]);
    st->rx_desc[slot].len = sizeof(st->rx_buf[slot]);
    st->rx_desc[slot].flags = VIRTQ_DESC_F_WRITE;
    st->rx_desc[slot].next = 0;

    dma_sync_clean_invalidate_range((uint64_t)&st->rx_buf[slot], sizeof(st->rx_buf[slot]));
    dma_sync_clean_range((uint64_t)&st->rx_desc[slot], sizeof(st->rx_desc[slot]));
    st->rx_avail.ring[st->rx_avail.idx % VIRTIO_NET_QUEUE_SIZE] = slot;
    dma_sync_clean_range((uint64_t)&st->rx_avail.ring[st->rx_avail.idx % VIRTIO_NET_QUEUE_SIZE],
                         sizeof(st->rx_avail.ring[0]));
    st->rx_avail.idx++;
    dma_sync_clean_range((uint64_t)&st->rx_avail, sizeof(st->rx_avail));
}

static ssize_t virtio_net_tx(struct net_device *dev, const void *buf, size_t len)
{
    struct virtio_net_state *st;
    uint16_t used_idx;
    uint32_t spins;

    if (!dev || !buf || !len)
    {
        return -EINVAL;
    }

    st = (struct virtio_net_state *)dev->priv;
    if (!st || !st->ready)
    {
        return -ENODEV;
    }

    if (len > VIRTIO_NET_TX_BUF_SIZE)
    {
        return -EMSGSIZE;
    }

    spin_lock(&st->tx_lock);
    printk("[virtio-net\ttx ]: submit len=%lu\n", (uint64_t)len);

    memset((int8_t *)&st->tx_buf, 0, sizeof(st->tx_buf));
    st->tx_buf.hdr.flags = 0;
    st->tx_buf.hdr.gso_type = VIRTIO_NET_HDR_GSO_NONE;
    st->tx_buf.hdr.hdr_len = 0;
    st->tx_buf.hdr.gso_size = 0;
    st->tx_buf.hdr.csum_start = 0;
    st->tx_buf.hdr.csum_offset = 0;
    memcpy((int8_t *)st->tx_buf.payload, (int8_t *)buf, len);

    used_idx = st->tx_last_used;
    st->tx_desc.addr = kernel_virt_to_phys((uint64_t)&st->tx_buf);
    st->tx_desc.len = sizeof(st->tx_buf.hdr) + len;
    st->tx_desc.flags = 0;
    st->tx_desc.next = 0;

    dma_sync_clean_range((uint64_t)&st->tx_buf, sizeof(st->tx_buf.hdr) + len);
    dma_sync_clean_range((uint64_t)&st->tx_desc, sizeof(st->tx_desc));
    st->tx_avail.ring[0] = 0;
    dma_sync_clean_range((uint64_t)&st->tx_avail.ring[0], sizeof(st->tx_avail.ring[0]));
    st->tx_avail.idx++;
    dma_sync_clean_range((uint64_t)&st->tx_avail, sizeof(st->tx_avail));

    dsb(sy);
    virtio_mmio_write32(st->mmio_base, VIRTIO_MMIO_QUEUE_NOTIFY, VIRTIO_NET_QUEUE_TX);

    spins = 0;
    while (st->tx_used.idx == used_idx)
    {
        dma_sync_invalidate_range((uint64_t)&st->tx_used, sizeof(st->tx_used));
        dsb(sy);
        if (++spins == 1000000)
        {
            printk("[virtio-net\ttx ]: timeout used=%u last=%u\n",
                   st->tx_used.idx, used_idx);
            spin_unlock(&st->tx_lock);
            return -ETIMEDOUT;
        }
    }

    st->tx_last_used = st->tx_used.idx;
    printk("[virtio-net\ttx ]: complete used=%u\n", st->tx_used.idx);
    virtio_mmio_write32(st->mmio_base, VIRTIO_MMIO_INTERRUPT_ACK, 0x3);
    spin_unlock(&st->tx_lock);

    return (ssize_t)len;
}

static void virtio_net_poll_rx(struct virtio_net_state *st)
{
    while (1)
    {
        uint16_t idx;
        uint32_t slot;
        uint32_t packet_len;

        dma_sync_invalidate_range((uint64_t)&st->rx_used, sizeof(st->rx_used));
        if (st->rx_used.idx == st->rx_last_used)
        {
            break;
        }

        idx = st->rx_last_used % VIRTIO_NET_QUEUE_SIZE;
        slot = st->rx_used.ring[idx].id;
        packet_len = st->rx_used.ring[idx].len;
        dma_sync_invalidate_range((uint64_t)&st->rx_buf[slot], sizeof(st->rx_buf[slot]));

        if (packet_len > sizeof(st->rx_buf[slot].hdr))
        {
            printk("[virtio-net\trx ]: slot=%u len=%u\n", slot, packet_len);
            net_receive(&st->dev, st->rx_buf[slot].payload, packet_len - sizeof(st->rx_buf[slot].hdr));
        }

        st->rx_last_used++;
        virtio_net_queue_rx(st, slot);
        virtio_mmio_write32(st->mmio_base, VIRTIO_MMIO_INTERRUPT_ACK, 0x1);
    }
}

static void virtio_net_rx_tasklet(unsigned long data)
{
    struct virtio_net_state *st;

    st = (struct virtio_net_state *)data;
    if (!st || !st->ready)
    {
        return;
    }

    virtio_net_poll_rx(st);
}

static void virtio_net_irq_handle(void)
{
    uint32_t status;

    status = virtio_mmio_read32(virtio_net_state.mmio_base, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (!status)
    {
        return;
    }

    /*
     * virtio-mmio 的硬中断里只做最小动作：
     * 1. 记账，方便 shell/日志观察 IRQ 是否真的进来了
     * 2. 立刻 ACK，避免设备线一直保持拉高
     * 3. 把真正的 used ring 处理下沉到 tasklet
     */
    virtio_net_state.irq_count++;
    virtio_mmio_write32(virtio_net_state.mmio_base, VIRTIO_MMIO_INTERRUPT_ACK, status);
    tasklet_schedule(&virtio_net_state.rx_tasklet);
}

static void virtio_net_worker(void *arg)
{
    struct virtio_net_state *st;

    st = (struct virtio_net_state *)arg;
    while (1)
    {
        if (st->ready)
        {
            if (!st->irq_ready)
            {
                virtio_net_poll_rx(st);
            }
        }
        sched_yield();
    }
}

void virtio_net_poll(void)
{
    if (virtio_net_state.ready)
    {
        virtio_net_poll_rx(&virtio_net_state);
    }
}

int virtio_net_init(void)
{
    const struct fdt_device_desc *dev_desc;
    uint64_t base;
    uint32_t queue_max;
    uint32_t status;
    uint32_t slot;
    uint8_t mac[6];
    int ret;

    memset((int8_t *)&virtio_net_state, 0, sizeof(virtio_net_state));
    virtio_net_irq_stable = false;
    spin_lock_init(&virtio_net_state.tx_lock);

    base = 0;
    for (slot = 0; slot < VIRTIO_MMIO_COUNT; slot++)
    {
        uint64_t probe = VIRTIO_MMIO_HIGH_BASE + ((uint64_t)slot * VIRTIO_MMIO_STRIDE);
        if (virtio_net_detect(probe) == 0)
        {
            base = probe;
            break;
        }
    }

    if (!base)
    {
        printk("[virtio-net\tinit]: no virtio-net device found\n");
        return -ENODEV;
    }

    virtio_net_state.mmio_base = base;
    virtio_net_state.irq = 0;
    virtio_net_state.irq_count = 0;
    virtio_net_state.irq_ready = false;
    tasklet_init(&virtio_net_state.rx_tasklet, virtio_net_rx_tasklet, (unsigned long)&virtio_net_state);
    virtio_mmio_write32(base, VIRTIO_MMIO_STATUS, 0);
    status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    virtio_mmio_write32(base, VIRTIO_MMIO_STATUS, status);

    virtio_mmio_write32(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    (void)virtio_mmio_read32(base, VIRTIO_MMIO_DEVICE_FEATURES);
    virtio_mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    virtio_mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    virtio_mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    virtio_mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES, (1U << (VIRTIO_F_VERSION_1 - 32)));

    status |= VIRTIO_STATUS_FEATURES_OK;
    virtio_mmio_write32(base, VIRTIO_MMIO_STATUS, status);
    if ((virtio_mmio_read32(base, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0)
    {
        printk("[virtio-net\tinit]: feature negotiation failed\n");
        return -EIO;
    }

    virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_SEL, VIRTIO_NET_QUEUE_RX);
    queue_max = virtio_mmio_read32(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max < VIRTIO_NET_QUEUE_SIZE)
    {
        printk("[virtio-net\tinit]: rx queue too small (%u)\n", queue_max);
        return -ENOSPC;
    }

    virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_NET_QUEUE_SIZE);
    virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_DESC_LOW, kernel_virt_to_phys((uint64_t)virtio_net_state.rx_desc));
    virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_AVAIL_LOW, kernel_virt_to_phys((uint64_t)&virtio_net_state.rx_avail));
    virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_USED_LOW, kernel_virt_to_phys((uint64_t)&virtio_net_state.rx_used));
    virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_READY, 1);

    virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_SEL, VIRTIO_NET_QUEUE_TX);
    queue_max = virtio_mmio_read32(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max < 1)
    {
        printk("[virtio-net\tinit]: tx queue too small (%u)\n", queue_max);
        return -ENOSPC;
    }

    virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_NUM, 1);
    virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_DESC_LOW, kernel_virt_to_phys((uint64_t)&virtio_net_state.tx_desc));
    virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_AVAIL_LOW, kernel_virt_to_phys((uint64_t)&virtio_net_state.tx_avail));
    virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_USED_LOW, kernel_virt_to_phys((uint64_t)&virtio_net_state.tx_used));
    virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_READY, 1);

    virtio_net_read_mac(base, mac);

    memset((int8_t *)virtio_net_state.dev.name, 0, sizeof(virtio_net_state.dev.name));
    memcpy((int8_t *)virtio_net_state.dev.name, (int8_t *)"eth0", 5);
    memcpy((int8_t *)virtio_net_state.dev.mac, (int8_t *)mac, 6);
    virtio_net_state.dev.ipv4 = ((uint32_t)10 << 24) | ((uint32_t)0 << 16) | ((uint32_t)2 << 8) | 15;
    virtio_net_state.dev.netmask = ((uint32_t)255 << 24) | ((uint32_t)255 << 16) | ((uint32_t)255 << 8) | 0;
    virtio_net_state.dev.gateway = ((uint32_t)10 << 24) | ((uint32_t)0 << 16) | ((uint32_t)2 << 8) | 2;
    virtio_net_state.dev.mtu = VIRTIO_NET_RX_BUF_SIZE;
    virtio_net_state.dev.priv = &virtio_net_state;
    virtio_net_state.dev.tx = virtio_net_tx;

    virtio_net_state.ready = true;
    virtio_net_state.rx_last_used = 0;
    virtio_net_state.tx_last_used = 0;

    for (slot = 0; slot < VIRTIO_NET_QUEUE_SIZE; slot++)
    {
        virtio_net_queue_rx(&virtio_net_state, slot);
    }

    status |= VIRTIO_STATUS_DRIVER_OK;
    virtio_mmio_write32(base, VIRTIO_MMIO_STATUS, status);

    ret = net_register_device(&virtio_net_state.dev);
    if (ret)
    {
        printk("[virtio-net\tinit]: net register failed %d\n", ret);
        return ret;
    }

    ret = kthread_create((const int8_t *)"vnet", virtio_net_worker, &virtio_net_state);
    if (ret < 0)
    {
        printk("[virtio-net\tinit]: worker create failed %d\n", ret);
        return ret;
    }

    dev_desc = fdt_find_device_by_reg(FDT_DEVICE_VIRTIO_MMIO, kernel_virt_to_phys(base));
    if (dev_desc && dev_desc->has_irq)
    {
        virtio_net_state.irq = dev_desc->irq;
        if (virtio_net_irq_stable)
        {
            irq_handlers[virtio_net_state.irq] = virtio_net_irq_handle;
            gic_enable_irq(virtio_net_state.irq);
            virtio_net_state.irq_ready = true;
        }
    }

    printk("[virtio-net\tinit]: base=%#lx mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           base, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (virtio_net_state.irq_ready)
    {
        printk("[virtio-net\tinit]: irq=%u gic=%u tasklet=on\n",
               virtio_net_state.irq, gic_irq_is_enabled(virtio_net_state.irq));
    }
    else if (virtio_net_state.irq)
    {
        /*
         * 先和 virtio-input 保持同样策略：IRQ 能探测到，但在早期内核
         * 尚未把整条中断/软中断路径验证稳定前，默认继续走 worker 轮询。
         * 这样能保证 make run 先以“可持续启动、可交互”为优先目标。
         */
        printk("[virtio-net\tinit]: irq=%u discovered, polling fallback enabled\n",
               virtio_net_state.irq);
    }
    else
    {
        printk("[virtio-net\tinit]: irq unavailable, fallback worker polling\n");
    }
    return 0;
}

uint32_t virtio_net_irq_count(void)
{
    return virtio_net_state.irq_count;
}
