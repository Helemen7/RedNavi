// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_KTIME_H
#define _LINUX_KTIME_H

#include <linux/types.h>
#include <rn/hooks.h>

static inline ktime_t ktime_get(void) { return (ktime_t)rn_now_ns(); }
static inline u64 ktime_get_ns(void) { return rn_now_ns(); }
static inline ktime_t ktime_sub(ktime_t a, ktime_t b) { return a - b; }
static inline ktime_t ktime_add_us(ktime_t a, u64 us) { return a + (ktime_t)us * 1000; }
static inline s64 ktime_to_ns(ktime_t k) { return k; }
static inline s64 ktime_to_us(ktime_t k) { return k / 1000; }
static inline s64 ktime_to_ms(ktime_t k) { return k / 1000000; }
static inline s64 ktime_us_delta(ktime_t a, ktime_t b) { return (a - b) / 1000; }
static inline int ktime_after(ktime_t a, ktime_t b) { return a > b; }

#endif
