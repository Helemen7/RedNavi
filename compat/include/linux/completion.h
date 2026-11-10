// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_COMPLETION_H
#define _LINUX_COMPLETION_H

#include <linux/jiffies.h>
#include <rn/hooks.h>

struct completion {
    volatile int done;
};

#define DECLARE_COMPLETION(x) struct completion x = { 0 }

static inline void init_completion(struct completion *c) { c->done = 0; }
static inline void reinit_completion(struct completion *c) { c->done = 0; }
static inline void complete(struct completion *c) { __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE); }
static inline int completion_done(struct completion *c) { return __atomic_load_n(&c->done, __ATOMIC_ACQUIRE); }
static inline void wait_for_completion(struct completion *c) { while (!completion_done(c)) rn_yield(); }
// Returns the jiffies left, or 0 if it timed out.
static inline unsigned long wait_for_completion_timeout(struct completion *c, unsigned long timeout) {
    uint64_t end = rn_now_ns() + (uint64_t)timeout * 1000000ull;
    while (!completion_done(c)) {
        uint64_t now = rn_now_ns();
        if (now >= end) return 0;
        rn_yield();
    }
    uint64_t now = rn_now_ns();
    return now >= end ? 1 : (unsigned long)((end - now) / 1000000ull) + 1;
}

#endif
