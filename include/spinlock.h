#ifndef __SPINLOCK_H__
#define __SPINLOCK_H__

#include "atomic.h"

/*
 * 最小自旋锁实现。
 * 当前内核还是单核主路径，但后续做 SMP、runqueue、页分配器和设备驱动时，
 * 锁接口必须先稳定下来。
 */
typedef struct
{
    atomic_t val;
} spinlock_t;

#define SPINLOCK_INIT { ATOMIC_INIT(0) }

static inline void spin_lock_init(spinlock_t *lock)
{
    atomic_set(&lock->val, 0);
}

static inline void spin_lock(spinlock_t *lock)
{
    while (atomic_cmpxchg(&lock->val, 0, 1) != 0)
    {
        while (atomic_read(&lock->val))
        {
            asm volatile("wfe" : : : "memory");
        }
    }
}

static inline void spin_unlock(spinlock_t *lock)
{
    asm volatile(
        "stlr %w1, [%0]\n"
        :
        : "r"(&lock->val.counter), "r"(0)
        : "memory"
    );
    asm volatile("sev" : : : "memory");
}

#endif
