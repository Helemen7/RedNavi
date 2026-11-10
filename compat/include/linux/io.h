// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_IO_H
#define _LINUX_IO_H

#include <linux/types.h>

static inline u8 readb(const volatile void *a) { return *(const volatile u8 *)a; }
static inline u16 readw(const volatile void *a) { return *(const volatile u16 *)a; }
static inline u32 readl(const volatile void *a) { return *(const volatile u32 *)a; }
static inline u64 readq(const volatile void *a) { return *(const volatile u64 *)a; }
static inline void writeb(u8 v, volatile void *a) { *(volatile u8 *)a = v; }
static inline void writew(u16 v, volatile void *a) { *(volatile u16 *)a = v; }
static inline void writel(u32 v, volatile void *a) { *(volatile u32 *)a = v; }
static inline void writeq(u64 v, volatile void *a) { *(volatile u64 *)a = v; }

#define mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define wmb() __atomic_thread_fence(__ATOMIC_RELEASE)

static inline void memcpy_toio(volatile void *dst, const void *src, size_t n) {
    volatile u8 *d = (volatile u8 *)dst; const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
}
static inline void memcpy_fromio(void *dst, const volatile void *src, size_t n) {
    u8 *d = (u8 *)dst; const volatile u8 *s = (const volatile u8 *)src;
    while (n--) *d++ = *s++;
}

#endif
