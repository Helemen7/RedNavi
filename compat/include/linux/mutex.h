// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_MUTEX_H
#define _LINUX_MUTEX_H

#include <linux/types.h>
#include <rn/hooks.h>

// Not recursive, no owner tracking. A contended lock yields the CPU, so it is for code that may sleep.
struct mutex {
    volatile int locked;
};

#define DEFINE_MUTEX(x) struct mutex x = { 0 }

static inline void mutex_init(struct mutex *m) { m->locked = 0; }
static inline int mutex_trylock(struct mutex *m) { return !__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE); }
static inline void mutex_lock(struct mutex *m) { while (!mutex_trylock(m)) rn_yield(); }
static inline int mutex_lock_interruptible(struct mutex *m) { mutex_lock(m); return 0; }
static inline void mutex_unlock(struct mutex *m) { __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE); }
static inline int mutex_is_locked(struct mutex *m) { return __atomic_load_n(&m->locked, __ATOMIC_RELAXED); }
static inline void mutex_destroy(struct mutex *m) { (void)m; }

#endif
