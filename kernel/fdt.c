#include "fdt.h"

#include "lib/libmem.h"
#include "lib/libstr.h"
#include "printk.h"

#define FDT_MAGIC               0xd00dfeedU
#define FDT_BEGIN_NODE          0x1U
#define FDT_END_NODE            0x2U
#define FDT_PROP                0x3U
#define FDT_NOP                 0x4U
#define FDT_END                 0x9U

#define FDT_MAX_STACK_DEPTH     32
#define FDT_MAX_DEVICES         64

struct fdt_header
{
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
} __attribute__((packed));

struct fdt_scan_node
{
    struct fdt_device_desc desc;
    uint32_t path_len;
};

struct fdt_state
{
    bool valid;
    const uint8_t *blob;
    uint32_t total_size;
    uint32_t struct_off;
    uint32_t strings_off;
    uint32_t strings_size;
    uint64_t memory_base;
    uint64_t memory_size;
    int8_t model[64];
    struct fdt_device_desc devices[FDT_MAX_DEVICES];
    uint32_t device_count;
};

static struct fdt_state fdt_state;

static uint32_t fdt_read_be32(const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;

    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint64_t fdt_read_be64(const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;

    return ((uint64_t)fdt_read_be32(p) << 32) | (uint64_t)fdt_read_be32(p + 4);
}

static uint32_t fdt_align4(uint32_t value)
{
    return (value + 3U) & ~3U;
}

static uint32_t fdt_gic_irq_to_intid(uint32_t irq_type, uint32_t irq_num)
{
    /*
     * 设备树里的 GIC interrupt specifier 不是直接的 intid：
     * - type=0: SPI，实际 intid 从 32 开始
     * - type=1: PPI，实际 intid 从 16 开始
     * 这里把它转换成真正的 GIC intid，后续日志和驱动注册都更直观。
     */
    switch (irq_type)
    {
    case 0:
        return 32U + irq_num;
    case 1:
        return 16U + irq_num;
    default:
        return irq_num;
    }
}

static void fdt_copy_string(int8_t *dst, size_t dst_len, const int8_t *src, size_t src_len)
{
    size_t copy_len;

    if (!dst || !dst_len)
    {
        return;
    }

    copy_len = src_len;
    if (copy_len >= dst_len)
    {
        copy_len = dst_len - 1;
    }

    if (copy_len)
    {
        memcpy(dst, (int8_t *)src, copy_len);
    }
    dst[copy_len] = '\0';
}

static bool fdt_string_list_contains(const void *data, uint32_t len, const int8_t *needle)
{
    const int8_t *p;
    uint32_t off;

    if (!data || !needle)
    {
        return false;
    }

    p = (const int8_t *)data;
    off = 0;
    while (off < len)
    {
        size_t slen;

        slen = strlen((int8_t *)(p + off));
        if (!slen)
        {
            break;
        }

        if (strcmp(p + off, needle) == 0)
        {
            return true;
        }

        off += (uint32_t)slen + 1U;
    }

    return false;
}

static void fdt_record_memory(uint64_t base, uint64_t size)
{
    if (!size)
    {
        return;
    }

    /*
     * QEMU virt 通常只有一段连续 RAM。
     * 我们只记录第一段主内存，后续页分配器和线性映射都按这段来展开。
     */
    if (!fdt_state.memory_size)
    {
        fdt_state.memory_base = base;
        fdt_state.memory_size = size;
    }
}

static enum fdt_device_kind fdt_classify_device(const struct fdt_device_desc *desc)
{
    if (!desc)
    {
        return FDT_DEVICE_UNKNOWN;
    }

    if (desc->kind == FDT_DEVICE_MEMORY)
    {
        return FDT_DEVICE_MEMORY;
    }

    if (desc->compatible[0] &&
        fdt_string_list_contains(desc->compatible, (uint32_t)strlen((int8_t *)desc->compatible), (const int8_t *)"virtio,mmio"))
    {
        return FDT_DEVICE_VIRTIO_MMIO;
    }

    if (desc->compatible[0] &&
        fdt_string_list_contains(desc->compatible, (uint32_t)strlen((int8_t *)desc->compatible), (const int8_t *)"qemu,fw-cfg-mmio"))
    {
        return FDT_DEVICE_FWCFG;
    }

    if (desc->compatible[0] &&
        (fdt_string_list_contains(desc->compatible, (uint32_t)strlen((int8_t *)desc->compatible), (const int8_t *)"pci-host-ecam-generic") ||
         fdt_string_list_contains(desc->compatible, (uint32_t)strlen((int8_t *)desc->compatible), (const int8_t *)"pci-host-ecam")))
    {
        return FDT_DEVICE_PCIE_HOST;
    }

    if (desc->compatible[0] &&
        (strcmp(desc->compatible, (const int8_t *)"arm,pl011") == 0 ||
         strcmp(desc->compatible, (const int8_t *)"arm,sbsa-uart") == 0))
    {
        return FDT_DEVICE_UART;
    }

    if (desc->compatible[0] &&
        (fdt_string_list_contains(desc->compatible, (uint32_t)strlen((int8_t *)desc->compatible), (const int8_t *)"simple-framebuffer") ||
         fdt_string_list_contains(desc->compatible, (uint32_t)strlen((int8_t *)desc->compatible), (const int8_t *)"ramfb")))
    {
        return FDT_DEVICE_FRAMEBUFFER;
    }

    return FDT_DEVICE_UNKNOWN;
}

static void fdt_commit_device(const struct fdt_scan_node *node)
{
    struct fdt_device_desc *dst;

    if (!node || !node->desc.path[0])
    {
        return;
    }

    if (node->desc.kind == FDT_DEVICE_UNKNOWN && !node->desc.has_reg && !node->desc.has_irq)
    {
        return;
    }

    if (fdt_state.device_count >= FDT_MAX_DEVICES)
    {
        return;
    }

    dst = &fdt_state.devices[fdt_state.device_count++];
    *dst = node->desc;
    dst->kind = fdt_classify_device(dst);

    if (dst->kind == FDT_DEVICE_MEMORY)
    {
        fdt_record_memory(dst->reg_base, dst->reg_size);
    }
}

static void fdt_handle_property(struct fdt_scan_node *node, const int8_t *name, const void *data, uint32_t len)
{
    if (!node || !name || !data)
    {
        return;
    }

    if (strcmp(name, (const int8_t *)"compatible") == 0)
    {
        const int8_t *compat = (const int8_t *)data;
        size_t compat_len = strlen((int8_t *)compat);

        fdt_copy_string(node->desc.compatible, sizeof(node->desc.compatible), compat, compat_len);
        return;
    }

    if (strcmp(name, (const int8_t *)"device_type") == 0)
    {
        const int8_t *type = (const int8_t *)data;

        if (strcmp(type, (const int8_t *)"memory") == 0)
        {
            node->desc.kind = FDT_DEVICE_MEMORY;
        }
        return;
    }

    if (strcmp(name, (const int8_t *)"model") == 0)
    {
        fdt_copy_string(fdt_state.model, sizeof(fdt_state.model), (const int8_t *)data, strlen((int8_t *)data));
        return;
    }

    if (strcmp(name, (const int8_t *)"reg") == 0)
    {
        if (len >= 16)
        {
            node->desc.reg_base = fdt_read_be64(data);
            node->desc.reg_size = fdt_read_be64((const uint8_t *)data + 8);
            node->desc.has_reg = true;
        }
        else if (len >= 8)
        {
            node->desc.reg_base = (uint64_t)fdt_read_be32(data);
            node->desc.reg_size = (uint64_t)fdt_read_be32((const uint8_t *)data + 4);
            node->desc.has_reg = true;
        }
        return;
    }

    if (strcmp(name, (const int8_t *)"interrupts") == 0 && len >= 12)
    {
        uint32_t irq_type;
        uint32_t irq_num;

        irq_type = fdt_read_be32(data);
        irq_num = fdt_read_be32((const uint8_t *)data + 4);
        node->desc.irq = fdt_gic_irq_to_intid(irq_type, irq_num);
        node->desc.has_irq = true;
        return;
    }
}

static int fdt_parse_blob(void)
{
    const uint8_t *struct_blk;
    const uint8_t *strings;
    const uint8_t *p;
    const uint8_t *end;
    struct fdt_scan_node stack[FDT_MAX_STACK_DEPTH];
    uint32_t depth = 0;
    int8_t path[256];

    struct_blk = fdt_state.blob + fdt_state.struct_off;
    strings = fdt_state.blob + fdt_state.strings_off;
    p = struct_blk;
    end = fdt_state.blob + fdt_state.total_size;

    memset((int8_t *)stack, 0, sizeof(stack));
    path[0] = '/';
    path[1] = '\0';

    while (p + 4 <= end)
    {
        uint32_t token;

        token = fdt_read_be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE)
        {
            const int8_t *node_name;
            size_t node_name_len;

            node_name = (const int8_t *)p;
            node_name_len = strlen((int8_t *)node_name);
            p += node_name_len + 1;
            p = fdt_state.blob + fdt_align4((uint32_t)(p - fdt_state.blob));

            if (depth < FDT_MAX_STACK_DEPTH)
            {
                stack[depth].path_len = strlen(path);
                memset((int8_t *)&stack[depth].desc, 0, sizeof(stack[depth].desc));
                stack[depth].desc.kind = FDT_DEVICE_UNKNOWN;

                if (node_name_len)
                {
                    size_t cur_len;

                    cur_len = strlen(path);
                    if (cur_len > 1 && path[cur_len - 1] != '/')
                    {
                        path[cur_len++] = '/';
                        path[cur_len] = '\0';
                    }
                    fdt_copy_string(path + cur_len, sizeof(path) - cur_len, node_name, node_name_len);
                }

                fdt_copy_string(stack[depth].desc.path, sizeof(stack[depth].desc.path), path, strlen(path));
                fdt_copy_string(stack[depth].desc.name, sizeof(stack[depth].desc.name), node_name, node_name_len);
                depth++;
            }
            continue;
        }

        if (token == FDT_END_NODE)
        {
            if (depth > 0)
            {
                depth--;
                fdt_commit_device(&stack[depth]);
                path[stack[depth].path_len] = '\0';
            }
            continue;
        }

        if (token == FDT_PROP)
        {
            uint32_t len;
            uint32_t nameoff;
            const int8_t *prop_name;
            const void *prop_data;

            if (p + 8 > end)
            {
                return -1;
            }

            len = fdt_read_be32(p);
            nameoff = fdt_read_be32(p + 4);
            p += 8;
            if (p + len > end || nameoff >= fdt_state.strings_size)
            {
                return -1;
            }

            prop_name = (const int8_t *)(strings + nameoff);
            prop_data = p;
            p += fdt_align4(len);

            if (depth > 0)
            {
                fdt_handle_property(&stack[depth - 1], prop_name, prop_data, len);
            }
            continue;
        }

        if (token == FDT_NOP)
        {
            continue;
        }

        if (token == FDT_END)
        {
            break;
        }

        return -1;
    }

    return 0;
}

void fdt_boot_init(const void *dtb)
{
    const struct fdt_header *hdr;

    memset((int8_t *)&fdt_state, 0, sizeof(fdt_state));
    fdt_state.blob = (const uint8_t *)dtb;
    if (!dtb)
    {
        return;
    }

    hdr = (const struct fdt_header *)dtb;
    if (fdt_read_be32(&hdr->magic) != FDT_MAGIC)
    {
        printk("[fdt\tinit]: invalid magic\n");
        return;
    }

    fdt_state.valid = true;
    fdt_state.total_size = fdt_read_be32(&hdr->totalsize);
    fdt_state.struct_off = fdt_read_be32(&hdr->off_dt_struct);
    fdt_state.strings_off = fdt_read_be32(&hdr->off_dt_strings);
    fdt_state.strings_size = fdt_read_be32(&hdr->size_dt_strings);

    if (!fdt_state.total_size || !fdt_state.struct_off || !fdt_state.strings_off)
    {
        fdt_state.valid = false;
        printk("[fdt\tinit]: broken header\n");
        return;
    }

    if (fdt_parse_blob())
    {
        fdt_state.valid = false;
        printk("[fdt\tinit]: parse failed\n");
        return;
    }
}

void fdt_log_summary(void)
{
    uint32_t i;

    if (!fdt_state.valid)
    {
        printk("[fdt\tinit]: no valid dtb\n");
        return;
    }

    printk("[fdt\tinit]: model=%s memory=%#lx-%#lx devices=%u\n",
           fdt_state.model[0] ? fdt_state.model : "unknown",
           fdt_state.memory_base,
           fdt_state.memory_base + fdt_state.memory_size,
           fdt_state.device_count);

    for (i = 0; i < fdt_state.device_count; i++)
    {
        const struct fdt_device_desc *dev;
        const char *kind;

        dev = &fdt_state.devices[i];
        switch (dev->kind)
        {
        case FDT_DEVICE_MEMORY: kind = "memory"; break;
        case FDT_DEVICE_UART: kind = "uart"; break;
        case FDT_DEVICE_FWCFG: kind = "fwcfg"; break;
        case FDT_DEVICE_VIRTIO_MMIO: kind = "virtio-mmio"; break;
        case FDT_DEVICE_PCIE_HOST: kind = "pcie-host"; break;
        case FDT_DEVICE_FRAMEBUFFER: kind = "framebuffer"; break;
        default: kind = "other"; break;
        }

        printk("[fdt\tdev ]: kind=%s path=%s compat=%s reg=%#lx/%#lx irq=%u\n",
               kind,
               dev->path,
               dev->compatible[0] ? dev->compatible : "unknown",
               dev->has_reg ? dev->reg_base : 0,
               dev->has_reg ? dev->reg_size : 0,
               dev->has_irq ? dev->irq : 0);
    }
}

uint64_t fdt_memory_base(void)
{
    return fdt_state.memory_base;
}

uint64_t fdt_memory_size(void)
{
    return fdt_state.memory_size;
}

uint32_t fdt_device_count(void)
{
    return fdt_state.device_count;
}

const struct fdt_device_desc *fdt_device(uint32_t index)
{
    if (index >= fdt_state.device_count)
    {
        return NULL;
    }

    return &fdt_state.devices[index];
}

const struct fdt_device_desc *fdt_find_device_by_kind(enum fdt_device_kind kind)
{
    uint32_t index;

    for (index = 0; index < fdt_state.device_count; index++)
    {
        if (fdt_state.devices[index].kind == kind)
        {
            return &fdt_state.devices[index];
        }
    }

    return NULL;
}

const struct fdt_device_desc *fdt_find_device_by_reg(enum fdt_device_kind kind, uint64_t reg_base)
{
    uint32_t index;

    for (index = 0; index < fdt_state.device_count; index++)
    {
        const struct fdt_device_desc *dev = &fdt_state.devices[index];

        if (dev->kind != kind)
        {
            continue;
        }

        if (!dev->has_reg || dev->reg_base != reg_base)
        {
            continue;
        }

        return dev;
    }

    return NULL;
}
