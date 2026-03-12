#ifndef __PAGETABLE_H__
#define __PAGETABLE_H__

#include "asm/pagetable_types.h"
#include "asm/barrier.h"

#define pgd_none(pgd) (!pgd_val(pgd))
#define pud_none(pud) (!pud_val(pud))
#define pmd_none(pmd) (!pmd_val(pmd))
#define pte_none(ptd) (!pte_val(ptd))

#define MAIR_DEVICE_nGnRnE_ATTR     0x00
#define MAIR_DEVICE_nGnRE_ATTR      0x04
#define MAIR_DEVICE_nGRE_ATTR       0x08
#define MAIR_DEVICE_GRE_ATTR        0x0c
// undefined
#define MAIR_UNDEFINED              0x03
// 关闭高速缓存
#define MAIR_NORMAL_NC_ATTR         0x44
// 写直通策略
#define MAIR_NORMAL_WT_ATTR         0xbb
// 回写策略
#define MAIR_NORMAL_ATTR            0xff

#endif
