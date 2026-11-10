// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_BITS_H
#define _LINUX_BITS_H

#include <linux/types.h>

#define BITS_PER_LONG 64
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (64 - 1 - (h))))

#define BITS_TO_LONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)

static inline void set_bit(unsigned int nr, volatile unsigned long *addr) { __atomic_fetch_or(&addr[nr / BITS_PER_LONG], 1UL << (nr % BITS_PER_LONG), __ATOMIC_SEQ_CST); }
static inline void clear_bit(unsigned int nr, volatile unsigned long *addr) { __atomic_fetch_and(&addr[nr / BITS_PER_LONG], ~(1UL << (nr % BITS_PER_LONG)), __ATOMIC_SEQ_CST); }
static inline int test_bit(unsigned int nr, const volatile unsigned long *addr) { return (__atomic_load_n(&addr[nr / BITS_PER_LONG], __ATOMIC_RELAXED) >> (nr % BITS_PER_LONG)) & 1; }
static inline int test_and_set_bit(unsigned int nr, volatile unsigned long *addr) {
    unsigned long m = 1UL << (nr % BITS_PER_LONG);
    return (__atomic_fetch_or(&addr[nr / BITS_PER_LONG], m, __ATOMIC_SEQ_CST) & m) != 0;
}
static inline int test_and_clear_bit(unsigned int nr, volatile unsigned long *addr) {
    unsigned long m = 1UL << (nr % BITS_PER_LONG);
    return (__atomic_fetch_and(&addr[nr / BITS_PER_LONG], ~m, __ATOMIC_SEQ_CST) & m) != 0;
}

static inline unsigned int hweight32(u32 w) { return (unsigned int)__builtin_popcount(w); }
static inline unsigned int hweight64(u64 w) { return (unsigned int)__builtin_popcountll(w); }
static inline unsigned int __ffs(unsigned long w) { return (unsigned int)__builtin_ctzl(w); }   // w must not be 0
static inline unsigned int __fls(unsigned long w) { return 63u - (unsigned int)__builtin_clzl(w); }
// XNU already provides fls() in libkern; use it in the kernel, define it on the host.
#ifndef KERNEL
static inline int fls(u32 w) { return w ? 32 - __builtin_clz(w) : 0; }
#endif

// Position of the first set bit in the mask, used by the REG_SET_FIELD style macros.
#define __bf_shf(mask) (__builtin_ffsll((long long)(mask)) - 1)
#define FIELD_GET(mask, reg) (((reg) & (mask)) >> __bf_shf(mask))
#define FIELD_PREP(mask, val) (((__typeof__(mask))(val) << __bf_shf(mask)) & (mask))

#endif
