#include "driver/virtio_input.h"

#include "asm/barrier.h"
#include "driver/virtio_blk.h"
#include "fdt.h"
#include "errno.h"
#include "gicv2.h"
#include "irq.h"
#include "lib/libasm.h"
#include "lib/librw.h"
#include "lib/libmem.h"
#include "lib/libstr.h"
#include "mmu.h"
#include "printk.h"
#include "sched.h"
#include "softirq.h"
#include "spinlock.h"
#include "tty.h"

#define VIRTIO_MMIO_MAGIC_VALUE         0x000
#define VIRTIO_MMIO_VERSION             0x004
#define VIRTIO_MMIO_DEVICE_ID           0x008
#define VIRTIO_MMIO_VENDOR_ID           0x00c
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
#define VIRTIO_DEVICE_INPUT             18
#define VIRTIO_VENDOR_QEMU              0x554d4551

#define VIRTIO_STATUS_ACKNOWLEDGE       1
#define VIRTIO_STATUS_DRIVER            2
#define VIRTIO_STATUS_DRIVER_OK         4
#define VIRTIO_STATUS_FEATURES_OK       8

#define VIRTQ_DESC_F_NEXT               1
#define VIRTQ_DESC_F_WRITE              2

#define VIRTIO_INPUT_QUEUE_EVENT        0
#define VIRTIO_INPUT_QUEUE_STATUS       1
#define VIRTIO_INPUT_QUEUE_SIZE         8

/* Linux input-event-codes.h 里的常用按键码。 */
#define EV_SYN                          0x00
#define EV_KEY                          0x01
#define EV_REL                          0x02
#define EV_ABS                          0x03

#define SYN_REPORT                      0

#define REL_X                           0x00
#define REL_Y                           0x01

#define ABS_X                           0x00
#define ABS_Y                           0x01

#define BTN_LEFT                        0x110
#define BTN_RIGHT                       0x111
#define BTN_MIDDLE                      0x112
#define BTN_TOUCH                       0x14a

#define KEY_ESC                         1
#define KEY_1                           2
#define KEY_2                           3
#define KEY_3                           4
#define KEY_4                           5
#define KEY_5                           6
#define KEY_6                           7
#define KEY_7                           8
#define KEY_8                           9
#define KEY_9                           10
#define KEY_0                           11
#define KEY_MINUS                       12
#define KEY_EQUAL                       13
#define KEY_BACKSPACE                   14
#define KEY_TAB                         15
#define KEY_Q                           16
#define KEY_W                           17
#define KEY_E                           18
#define KEY_R                           19
#define KEY_T                           20
#define KEY_Y                           21
#define KEY_U                           22
#define KEY_I                           23
#define KEY_O                           24
#define KEY_P                           25
#define KEY_LEFTBRACE                   26
#define KEY_RIGHTBRACE                  27
#define KEY_ENTER                       28
#define KEY_LEFTCTRL                    29
#define KEY_A                           30
#define KEY_S                           31
#define KEY_D                           32
#define KEY_F                           33
#define KEY_G                           34
#define KEY_H                           35
#define KEY_J                           36
#define KEY_K                           37
#define KEY_L                           38
#define KEY_SEMICOLON                   39
#define KEY_APOSTROPHE                  40
#define KEY_GRAVE                       41
#define KEY_LEFTSHIFT                   42
#define KEY_BACKSLASH                   43
#define KEY_Z                           44
#define KEY_X                           45
#define KEY_C                           46
#define KEY_V                           47
#define KEY_B                           48
#define KEY_N                           49
#define KEY_M                           50
#define KEY_COMMA                       51
#define KEY_DOT                         52
#define KEY_SLASH                       53
#define KEY_RIGHTSHIFT                  54
#define KEY_SPACE                       57

enum virtio_input_kind
{
    VIRTIO_INPUT_KIND_KEYBOARD = 0,
    VIRTIO_INPUT_KIND_POINTER = 1,
};

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
    uint16_t ring[VIRTIO_INPUT_QUEUE_SIZE];
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
    struct virtq_used_elem ring[VIRTIO_INPUT_QUEUE_SIZE];
} __attribute__((packed));

struct virtio_input_event
{
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed));

struct virtio_input_config
{
    uint8_t select;
    uint8_t subsel;
    uint8_t size;
    uint8_t reserved[5];
    union
    {
        uint8_t string[128];
        uint8_t bitmap[128];
        uint8_t abs[16];
    } u;
} __attribute__((packed));

struct virtio_input_frame
{
    struct virtio_input_event event;
} __attribute__((packed));

struct virtio_input_state
{
    uint64_t mmio_base;
    bool ready;
    bool irq_ready;
    enum virtio_input_kind kind;
    uint32_t irq;
    uint32_t irq_count;
    int8_t name[64];
    struct tasklet_struct event_tasklet;
    struct virtq_desc event_desc[VIRTIO_INPUT_QUEUE_SIZE] __attribute__((aligned(4096)));
    struct virtq_avail event_avail __attribute__((aligned(4096)));
    struct virtq_used event_used __attribute__((aligned(4096)));
    struct virtio_input_frame event_buf[VIRTIO_INPUT_QUEUE_SIZE] __attribute__((aligned(4096)));
    uint16_t event_last_used;
    struct virtq_desc status_desc[1] __attribute__((aligned(4096)));
    struct virtq_avail status_avail __attribute__((aligned(4096)));
    struct virtq_used status_used __attribute__((aligned(4096)));
    struct virtio_input_frame status_buf[1] __attribute__((aligned(4096)));
    uint16_t status_last_used;
    bool shift;
    bool caps;
};

static struct virtio_input_state virtio_input_state[4];
static bool virtio_input_irq_stable;

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

static inline void virtio_mmio_write32(uint64_t base, uint64_t reg, uint32_t value)
{
    put32(base + reg, value);
}

static inline void virtio_mmio_write64(uint64_t base, uint64_t reg, uint64_t value)
{
    virtio_mmio_write32(base, reg, (uint32_t)value);
    virtio_mmio_write32(base, reg + 4, (uint32_t)(value >> 32));
}

static void virtio_input_irq_handle_slot(uint32_t slot)
{
    struct virtio_input_state *st;
    uint32_t status;

    if (slot >= (sizeof(virtio_input_state) / sizeof(virtio_input_state[0])))
    {
        return;
    }

    st = &virtio_input_state[slot];
    if (!st->ready || !st->irq_ready)
    {
        return;
    }

    status = virtio_mmio_read32(st->mmio_base, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (!status)
    {
        return;
    }

    st->irq_count++;
    virtio_mmio_write32(st->mmio_base, VIRTIO_MMIO_INTERRUPT_ACK, status);
    tasklet_schedule(&st->event_tasklet);
}

static void virtio_input_irq_handle0(void) { virtio_input_irq_handle_slot(0); }
static void virtio_input_irq_handle1(void) { virtio_input_irq_handle_slot(1); }
static void virtio_input_irq_handle2(void) { virtio_input_irq_handle_slot(2); }
static void virtio_input_irq_handle3(void) { virtio_input_irq_handle_slot(3); }

static void (* const virtio_input_irq_handlers[])(void) =
{
    virtio_input_irq_handle0,
    virtio_input_irq_handle1,
    virtio_input_irq_handle2,
    virtio_input_irq_handle3,
};

static int virtio_input_detect(uint64_t base)
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

    if (virtio_mmio_read32(base, VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_INPUT)
    {
        return -1;
    }

    return 0;
}

static bool virtio_input_name_contains(const int8_t *name, const int8_t *needle)
{
    size_t name_len;
    size_t needle_len;
    size_t i;

    if (!name || !needle)
    {
        return false;
    }

    name_len = strlen((int8_t *)name);
    needle_len = strlen((int8_t *)needle);
    if (!needle_len || needle_len > name_len)
    {
        return false;
    }

    for (i = 0; i + needle_len <= name_len; i++)
    {
        if (strncmp(name + i, needle, needle_len) == 0)
        {
            return true;
        }
    }

    return false;
}

static void virtio_input_read_name(uint64_t base, int8_t *name, size_t name_len)
{
    volatile struct virtio_input_config *cfg;
    size_t copy_len;

    if (!name || !name_len)
    {
        return;
    }

    cfg = (volatile struct virtio_input_config *)(base + VIRTIO_MMIO_CONFIG);
    cfg->select = 0x01; /* VIRTIO_INPUT_CFG_ID_NAME */
    cfg->subsel = 0;
    dsb(sy);
    copy_len = cfg->size;
    if (copy_len >= name_len)
    {
        copy_len = name_len - 1;
    }
    memset(name, 0, name_len);
    memcpy(name, (int8_t *)cfg->u.string, copy_len);
    name[copy_len] = '\0';
}

static void virtio_input_queue_event(struct virtio_input_state *st, uint32_t slot)
{
    st->event_desc[slot].addr = kernel_virt_to_phys((uint64_t)&st->event_buf[slot]);
    st->event_desc[slot].len = sizeof(st->event_buf[slot]);
    st->event_desc[slot].flags = VIRTQ_DESC_F_WRITE;
    st->event_desc[slot].next = 0;

    dma_sync_clean_range((uint64_t)&st->event_desc[slot], sizeof(st->event_desc[slot]));
    dma_sync_clean_invalidate_range((uint64_t)&st->event_buf[slot], sizeof(st->event_buf[slot]));
    st->event_avail.ring[st->event_avail.idx % VIRTIO_INPUT_QUEUE_SIZE] = slot;
    dma_sync_clean_range((uint64_t)&st->event_avail.ring[st->event_avail.idx % VIRTIO_INPUT_QUEUE_SIZE],
                         sizeof(st->event_avail.ring[0]));
    st->event_avail.idx++;
    dma_sync_clean_range((uint64_t)&st->event_avail, sizeof(st->event_avail));
}

static uint8_t virtio_input_key_to_ascii(struct virtio_input_state *st, uint16_t code)
{
    bool shift;

    shift = st->shift;
    switch (code)
    {
    case KEY_SPACE: return ' ';
    case KEY_TAB: return '\t';
    case KEY_ENTER: return '\r';
    case KEY_BACKSPACE: return '\b';
    case KEY_1: return shift ? '!' : '1';
    case KEY_2: return shift ? '@' : '2';
    case KEY_3: return shift ? '#' : '3';
    case KEY_4: return shift ? '$' : '4';
    case KEY_5: return shift ? '%' : '5';
    case KEY_6: return shift ? '^' : '6';
    case KEY_7: return shift ? '&' : '7';
    case KEY_8: return shift ? '*' : '8';
    case KEY_9: return shift ? '(' : '9';
    case KEY_0: return shift ? ')' : '0';
    case KEY_MINUS: return shift ? '_' : '-';
    case KEY_EQUAL: return shift ? '+' : '=';
    case KEY_LEFTBRACE: return shift ? '{' : '[';
    case KEY_RIGHTBRACE: return shift ? '}' : ']';
    case KEY_SEMICOLON: return shift ? ':' : ';';
    case KEY_APOSTROPHE: return shift ? '"' : '\'';
    case KEY_GRAVE: return shift ? '~' : '`';
    case KEY_BACKSLASH: return shift ? '|' : '\\';
    case KEY_COMMA: return shift ? '<' : ',';
    case KEY_DOT: return shift ? '>' : '.';
    case KEY_SLASH: return shift ? '?' : '/';
    default:
        break;
    }

    if (code >= KEY_A && code <= KEY_Z)
    {
        uint8_t ch;

        ch = (uint8_t)('a' + (code - KEY_A));
        if (shift)
        {
            ch = (uint8_t)(ch - 'a' + 'A');
        }
        return ch;
    }

    return 0;
}

static void virtio_input_handle_keyboard(struct virtio_input_state *st, const struct virtio_input_event *event)
{
    uint8_t ch;

    if (event->code == KEY_LEFTSHIFT || event->code == KEY_RIGHTSHIFT)
    {
        st->shift = event->value != 0;
        return;
    }

    if (event->value == 0)
    {
        return;
    }

    ch = virtio_input_key_to_ascii(st, event->code);
    if (ch)
    {
        tty_feed_char(ch);
    }
}

static void virtio_input_handle_pointer(struct virtio_input_state *st, const struct virtio_input_event *event)
{
    static int32_t abs_x;
    static int32_t abs_y;
    static uint32_t buttons;

    switch (event->type)
    {
    case EV_REL:
        if (event->code == REL_X)
        {
            tty_report_mouse_delta((int32_t)event->value, 0, buttons);
        }
        else if (event->code == REL_Y)
        {
            tty_report_mouse_delta(0, (int32_t)event->value, buttons);
        }
        break;
    case EV_ABS:
        if (event->code == ABS_X)
        {
            abs_x = (int32_t)event->value;
        }
        else if (event->code == ABS_Y)
        {
            abs_y = (int32_t)event->value;
        }
        tty_report_mouse_abs(abs_x, abs_y, buttons);
        break;
    case EV_KEY:
        if (event->code == BTN_LEFT)
        {
            if (event->value)
            {
                buttons |= 0x1U;
            }
            else
            {
                buttons &= ~0x1U;
            }
        }
        else if (event->code == BTN_RIGHT)
        {
            if (event->value)
            {
                buttons |= 0x2U;
            }
            else
            {
                buttons &= ~0x2U;
            }
        }
        else if (event->code == BTN_MIDDLE || event->code == BTN_TOUCH)
        {
            if (event->value)
            {
                buttons |= 0x4U;
            }
            else
            {
                buttons &= ~0x4U;
            }
        }
        tty_report_mouse_abs(abs_x, abs_y, buttons);
        break;
    default:
        break;
    }
    (void)st;
}

static void virtio_input_handle_event(struct virtio_input_state *st, const struct virtio_input_event *event)
{
    if (!event)
    {
        return;
    }

    if (st->kind == VIRTIO_INPUT_KIND_KEYBOARD)
    {
        if (event->type == EV_KEY)
        {
            virtio_input_handle_keyboard(st, event);
        }
        return;
    }

    virtio_input_handle_pointer(st, event);
}

static void virtio_input_poll_state(struct virtio_input_state *st)
{
    while (1)
    {
        uint16_t idx;
        uint32_t slot;

        dma_sync_invalidate_range((uint64_t)&st->event_used, sizeof(st->event_used));
        if (st->event_used.idx == st->event_last_used)
        {
            break;
        }

        idx = st->event_last_used % VIRTIO_INPUT_QUEUE_SIZE;
        slot = st->event_used.ring[idx].id;
        dma_sync_invalidate_range((uint64_t)&st->event_buf[slot], sizeof(st->event_buf[slot]));
        virtio_input_handle_event(st, &st->event_buf[slot].event);

        st->event_last_used++;
        virtio_input_queue_event(st, slot);
        virtio_mmio_write32(st->mmio_base, VIRTIO_MMIO_INTERRUPT_ACK, 0x1);
    }
}

static void virtio_input_event_tasklet(unsigned long data)
{
    struct virtio_input_state *st;

    st = (struct virtio_input_state *)data;
    if (!st || !st->ready)
    {
        return;
    }

    virtio_input_poll_state(st);
}

static void virtio_input_worker(void *arg)
{
    struct virtio_input_state *st;

    st = (struct virtio_input_state *)arg;
    while (1)
    {
        if (st->ready)
        {
            if (!st->irq_ready)
            {
                virtio_input_poll_state(st);
            }
            sched_maybe_resched();
        }
        /*
         * 和网卡 fallback 一样，只有在 IRQ 真不可用时才会跑到这里。
         * 先给调度器机会切走，再用 wfi 降低空转成本。
         */
        asm volatile("wfi" : : : "memory");
    }
}

static int virtio_input_configure(uint64_t base, struct virtio_input_state *st)
{
    const struct fdt_device_desc *dev_desc;
    uint32_t queue_max;
    uint32_t status;
    uint32_t slot;
    int ret;

    virtio_mmio_write32(base, VIRTIO_MMIO_STATUS, 0);
    status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    virtio_mmio_write32(base, VIRTIO_MMIO_STATUS, status);

    /* 这里先不谈复杂 feature，保证设备能起来、能收事件即可。 */
    status |= VIRTIO_STATUS_FEATURES_OK;
    virtio_mmio_write32(base, VIRTIO_MMIO_STATUS, status);

    virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_SEL, VIRTIO_INPUT_QUEUE_EVENT);
    queue_max = virtio_mmio_read32(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max < VIRTIO_INPUT_QUEUE_SIZE)
    {
        return -ENOSPC;
    }
    virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_INPUT_QUEUE_SIZE);
    virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_DESC_LOW, kernel_virt_to_phys((uint64_t)st->event_desc));
    virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_AVAIL_LOW, kernel_virt_to_phys((uint64_t)&st->event_avail));
    virtio_mmio_write64(base, VIRTIO_MMIO_QUEUE_USED_LOW, kernel_virt_to_phys((uint64_t)&st->event_used));
    virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_READY, 1);

    virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_SEL, VIRTIO_INPUT_QUEUE_STATUS);
    queue_max = virtio_mmio_read32(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max > 0)
    {
        /*
         * virtio-input 的 status 队列是可选的，主要用于少数状态回传场景。
         * 这里先不要主动塞一个全 0 的“伪事件”，否则 QEMU 会把它当成
         * 输入事件处理，打印出 unknown type 0 之类的噪声。
         *
         * 后续如果确实需要回传 LED / 设备状态，再按真实协议补这条路径。
         */
        virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_NUM, 1);
        virtio_mmio_write32(base, VIRTIO_MMIO_QUEUE_READY, 0);
    }

    virtio_input_read_name(base, st->name, sizeof(st->name));
    st->kind = virtio_input_name_contains(st->name, (const int8_t *)"Tablet") ||
               virtio_input_name_contains(st->name, (const int8_t *)"Pointer")
               ? VIRTIO_INPUT_KIND_POINTER
               : VIRTIO_INPUT_KIND_KEYBOARD;
    st->irq = 0;
    st->irq_count = 0;
    st->irq_ready = false;
    tasklet_init(&st->event_tasklet, virtio_input_event_tasklet, (unsigned long)st);

    for (slot = 0; slot < VIRTIO_INPUT_QUEUE_SIZE; slot++)
    {
        virtio_input_queue_event(st, slot);
    }

    status |= VIRTIO_STATUS_DRIVER_OK;
    virtio_mmio_write32(base, VIRTIO_MMIO_STATUS, status);

    dev_desc = fdt_find_device_by_reg(FDT_DEVICE_VIRTIO_MMIO, kernel_virt_to_phys(base));
    if (dev_desc && dev_desc->has_irq)
    {
        uint32_t idx = (uint32_t)(st - virtio_input_state);

        st->irq = dev_desc->irq;
        if (virtio_input_irq_stable &&
            idx < (sizeof(virtio_input_irq_handlers) / sizeof(virtio_input_irq_handlers[0])))
        {
            irq_handlers[st->irq] = virtio_input_irq_handlers[idx];
            gic_enable_irq(st->irq);
            st->irq_ready = true;
        }
    }

    st->ready = true;
    if (!st->irq_ready)
    {
        ret = kthread_create((const int8_t *)(st->kind == VIRTIO_INPUT_KIND_KEYBOARD ? "kbd-in" : "mouse-in"),
                             virtio_input_worker, st);
        if (ret < 0)
        {
            st->ready = false;
            return ret;
        }
    }

    printk("[input\tinit]: %s kind=%s base=%#lx\n",
           st->name,
           st->kind == VIRTIO_INPUT_KIND_KEYBOARD ? "keyboard" : "pointer",
           base);
    if (st->irq_ready)
    {
        printk("[input\tinit]: %s irq=%u gic=%u tasklet=on\n",
               st->name, st->irq, gic_irq_is_enabled(st->irq));
    }
    else if (st->irq)
    {
        /*
         * 只有在 IRQ 还没真正接管时才需要 worker 轮询。
         * IRQ 稳定后，这里不会额外保留一个后台轮询线程。
         */
        printk("[input\tinit]: %s irq=%u discovered, polling fallback enabled\n",
               st->name, st->irq);
    }
    else
    {
        printk("[input\tinit]: %s irq unavailable, fallback worker polling\n", st->name);
    }
    return 0;
}

int virtio_input_init(void)
{
    uint32_t slot;
    uint32_t used;
    int ret;

    memset((int8_t *)virtio_input_state, 0, sizeof(virtio_input_state));
    /*
     * 在 QEMU virt 平台上，这组 virtio-input IRQ 是稳定可用的。
     * 之前这里一直保持 false，会把键盘/鼠标强制压到轮询 worker，
     * 直接拖慢“按键 -> 中断处理 -> tty 回显”的整条链路。
     */
    virtio_input_irq_stable = true;

    used = 0;
    for (slot = 0; slot < VIRTIO_MMIO_COUNT; slot++)
    {
        uint64_t probe = VIRTIO_MMIO_HIGH_BASE + ((uint64_t)slot * VIRTIO_MMIO_STRIDE);

        if (virtio_input_detect(probe) != 0)
        {
            continue;
        }

        if (used >= (sizeof(virtio_input_state) / sizeof(virtio_input_state[0])))
        {
            break;
        }

        virtio_input_state[used].mmio_base = probe;
        ret = virtio_input_configure(probe, &virtio_input_state[used]);
        if (ret)
        {
            printk("[input\tinit]: setup failed %d\n", ret);
            virtio_input_state[used].ready = false;
            continue;
        }

        used++;
    }

    if (!used)
    {
        printk("[input\tinit]: no virtio-input device found\n");
        return -ENODEV;
    }

    printk("[input\tinit]: %u device(s) online\n", used);
    return 0;
}

void virtio_input_poll(void)
{
    uint32_t i;

    for (i = 0; i < sizeof(virtio_input_state) / sizeof(virtio_input_state[0]); i++)
    {
        if (virtio_input_state[i].ready)
        {
            virtio_input_poll_state(&virtio_input_state[i]);
        }
    }
}

uint32_t virtio_input_irq_count(void)
{
    uint32_t i;
    uint32_t total;

    total = 0;
    for (i = 0; i < sizeof(virtio_input_state) / sizeof(virtio_input_state[0]); i++)
    {
        total += virtio_input_state[i].irq_count;
    }

    return total;
}
