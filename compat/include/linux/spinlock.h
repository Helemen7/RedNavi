// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_SPINLOCK_H
#define _LINUX_SPINLOCK_H

#include <linux/types.h>
#include <rn/hooks.h>

// Plain test-and-set lock. Nothing in the kext runs in interrupt context yet, so the irq variants
// do not mask interrupts; revisit this when an interrupt handler is added.
typedef struct { volatile int locked; } spinlock_t;

#define __SPIN_LOCK_UNLOCKED(x) { 0 }
#define DEFINE_SPINLOCK(x) spinlock_t x = __SPIN_LOCK_UNLOCKED(x)

static inline void spin_lock_init(spinlock_t *l) { l->locked = 0; }
static inline int spin_trylock(spinlock_t *l) { return !__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE); }
static inline void spin_lock(spinlock_t *l) {
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) rn_cpu_relax();
    }
}
static inline void spin_unlock(spinlock_t *l) { __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE); }

#define spin_lock_irqsave(l, flags) do { (flags) = 0; spin_lock(l); } while (0)
#define spin_unlock_irqrestore(l, flags) do { (void)(flags); spin_unlock(l); } while (0)
#define spin_lock_irq(l) spin_lock(l)
#define spin_unlock_irq(l) spin_unlock(l)

#endif
