#ifndef __PAGETABLE_H__
#define __PAGETABLE_H__

#include "asm/pagetable_types.h"
#include "asm/barrier.h"

#define pgd_none(pgd) (!pgd_val(pgd))
#define pud_none(pud) (!pud_val(pud))
#define pmd_none(pmd) (!pmd_val(pmd))
#define pte_none(ptd) (!pte_val(ptd))

#endif
