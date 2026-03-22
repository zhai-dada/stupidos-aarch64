#ifndef __GICV2_H__
#define __GICV2_H__

#include "asm/types.h"
#include "irq.h"
/*
    [VIRT_GIC_DIST] =           { 0x08000000, 0x00010000 },
    [VIRT_GIC_CPU] =            { 0x08010000, 0x00010000 },
    [VIRT_GIC_V2M] =            { 0x08020000, 0x00001000 },
    [VIRT_GIC_HYP] =            { 0x08030000, 0x00010000 },
    [VIRT_GIC_VCPU] =           { 0x08040000, 0x00010000 },
    The space in between here is reserved for GICv3 CPU/vCPU/HYP
    [VIRT_GIC_ITS] =            { 0x08080000, 0x00020000 },
    This redistributor space allows up to 2*64kB*123 CPUs
    [VIRT_GIC_REDIST] =         { 0x080A0000, 0x00F60000 },
 */
#define COUNTER_FREQ_IN_HZ  0x3b9aca0

#define GIC_BASE            (0x08000000)

#define GICD_BASE           (GIC_BASE + 0x00000)
#define GICC_BASE           (GIC_BASE + 0x10000)

/* GICD Registers */
#define GICD_CTLR           (GICD_BASE + 0x000)
#define GICD_TYPER          (GICD_BASE + 0x004)
#define GICD_IIDR           (GICD_BASE + 0x008)
#define GICD_IGROUPR        (GICD_BASE + 0x080)
#define GICD_ISENABLER      (GICD_BASE + 0x100)
#define GICD_ICENABLER      (GICD_BASE + 0x180)
#define GICD_ISPENDR        (GICD_BASE + 0x200)
#define GICD_ICPENDR        (GICD_BASE + 0x280)
#define GICD_ISACTIVER      (GICD_BASE + 0x300)
#define GICD_ICACTIVER      (GICD_BASE + 0x380)
#define GICD_IPRIORITYR     (GICD_BASE + 0x400)
#define GICD_ITARGETSR      (GICD_BASE + 0x800)
#define GICD_ICFGR          (GICD_BASE + 0xC00)
#define GICD_PPISR          (GICD_BASE + 0xD00)
#define GICD_SGIR           (GICD_BASE + 0xF00)
#define GICD_SGIR_CLRPEND   (GICD_BASE + 0xF10)
#define GICD_SGIR_SETPEND   (GICD_BASE + 0xF20)

/* GICC Registers */
#define GICC_CTLR           (GICC_BASE + 0x0000)
#define GICC_PMR            (GICC_BASE + 0x0004)
#define GICC_BPR            (GICC_BASE + 0x0008)
#define GICC_IAR            (GICC_BASE + 0x000C)
#define GICC_EOIR           (GICC_BASE + 0x0010)
#define GICC_RPR            (GICC_BASE + 0x0014)
#define GICC_HPPIR          (GICC_BASE + 0x0018)
#define GICC_ABPR           (GICC_BASE + 0x001C)
#define GICC_AIAR           (GICC_BASE + 0x0020)
#define GICC_AEOIR          (GICC_BASE + 0x0024)
#define GICC_AHPPIR         (GICC_BASE + 0x0028)
#define GICC_APR            (GICC_BASE + 0x00D0)
#define GICC_IIDR           (GICC_BASE + 0x00FC)
#define GICC_DIR            (GICC_BASE + 0x1000)

#define GICD_ENABLE_GRP0        0x01
#define GICD_ENABLE_GRP1        0x02
#define GICD_ENABLE             (GICD_ENABLE_GRP0 | GICD_ENABLE_GRP1)
#define GICD_DISABLE            0x00
#define GICD_INT_EN_SET_SGI     0x0000ffff
#define GICD_INT_EN_CLR_X32     0xffffffff
#define GICD_INT_DEF_PRI        0xa0
#define GICD_INT_DEF_PRI_X4     ((GICD_INT_DEF_PRI << 24) | (GICD_INT_DEF_PRI << 16) | (GICD_INT_DEF_PRI << 8) | GICD_INT_DEF_PRI)
#define GICD_INT_EN_CLR_PPI     0xffff0000

#define GICD_INT_ACTLOW_LVLTRIG		0x0

#define GICC_DISABLE                0x00
#define GICC_ENABLE_GRP0            0x01
#define GICC_ENABLE_GRP1            0x02
#define GICC_ENABLE                 (GICC_ENABLE_GRP0 | GICC_ENABLE_GRP1)
#define GICC_INT_PRI_THRESHOLD      0xff
#define GICC_CTRL_ACKCTL            (1 << 2)
#define GICC_CTRL_EOImodeNS_SHIFT   9
#define GICC_CTRL_EOImodeNS         (1 << GICC_CTRL_EOImodeNS_SHIFT)
#define GICC_DIS_BYPASS_MASK        0x1e0

typedef struct
{
    uint32_t irq_num;
    uint32_t cpu_num;
} _gic_info;

extern _gic_info global_gic_info;

void gic_init(void);

uint32_t gic_enable_irq(uint32_t irq);
uint32_t gic_irq_is_enabled(uint32_t irq);

#endif
