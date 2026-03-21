#ifndef __ATOMIC_H__
#define __ATOMIC_H__

#include "asm/types.h"

/*
 * AArch64 下的最小原子操作封装。
 * 这里优先提供后续内核基础设施最常用的几个操作：
 * - read / set
 * - add / sub / inc / dec
 * - cmpxchg
 *
 * 后续做 SMP、锁、页分配器和调度器时都会直接依赖这些原语。
 */
typedef struct
{
    volatile int32_t counter;
} atomic_t;

#define ATOMIC_INIT(v) { (v) }

static inline int32_t atomic_read(const atomic_t *v)
{
    return v->counter;
}

static inline void atomic_set(atomic_t *v, int32_t i)
{
    v->counter = i;
}

static inline int32_t atomic_add_return(atomic_t *v, int32_t i)
{
    int32_t old;
    int32_t tmp;

    asm volatile(
        "1: ldaxr %w0, [%2]\n"
        "add %w0, %w0, %w3\n"
        "stlxr %w1, %w0, [%2]\n"
        "cbnz %w1, 1b\n"
        : "=&r"(old), "=&r"(tmp)
        : "r"(&v->counter), "r"(i)
        : "cc", "memory"
    );

    return old;
}

static inline int32_t atomic_sub_return(atomic_t *v, int32_t i)
{
    return atomic_add_return(v, -i);
}

static inline void atomic_inc(atomic_t *v)
{
    (void)atomic_add_return(v, 1);
}

static inline void atomic_dec(atomic_t *v)
{
    (void)atomic_sub_return(v, 1);
}

static inline int32_t atomic_cmpxchg(atomic_t *v, int32_t old, int32_t new)
{
    int32_t prev;
    int32_t tmp;

    asm volatile(
        "1: ldaxr %w0, [%2]\n"
        "cmp %w0, %w3\n"
        "b.ne 2f\n"
        "stlxr %w1, %w4, [%2]\n"
        "cbnz %w1, 1b\n"
        "2:\n"
        : "=&r"(prev), "=&r"(tmp)
        : "r"(&v->counter), "r"(old), "r"(new)
        : "cc", "memory"
    );

    return prev;
}

#endif
