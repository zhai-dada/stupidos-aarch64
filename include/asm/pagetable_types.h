#ifndef __PAGETABLE_TYPES_H__
#define __PAGETABLE_TYPES_H__

#include "asm/types.h"

typedef uint64_t pteval_t;
typedef uint64_t pmdval_t;
typedef uint64_t pudval_t;
typedef uint64_t pgdval_t;

typedef struct
{
	pteval_t pte;
} pte_t;

#define pte_val(x) ((x).pte)

typedef struct
{
	pmdval_t pmd;
} pmd_t;

#define pmd_val(x) ((x).pmd)

typedef struct
{
	pudval_t pud;
} pud_t;

#define pud_val(x) ((x).pud)

typedef struct
{
	pgdval_t pgd;
} pgd_t;

#define pgd_val(x) ((x).pgd)

#endif
