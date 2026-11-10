// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_KERNEL_H
#define _LINUX_KERNEL_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/bits.h>
#include <linux/printk.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define offsetof_member(T, m) offsetof(T, m)
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
// XNU's sys/param.h has its own min/max.
#undef min
#undef max
#define min(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define clamp(v, lo, hi) min(max(v, lo), hi)
#define swap(a, b) do { __typeof__(a) _t = (a); (a) = (b); (b) = _t; } while (0)

#define lower_32_bits(n) ((u32)((n) & 0xffffffffu))
#define upper_32_bits(n) ((u32)(((n) >> 16) >> 16))

#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline void *ERR_PTR(long error) { return (void *)error; }
static inline long PTR_ERR(const void *ptr) { return (long)ptr; }
static inline int IS_ERR(const void *ptr) { return (int)IS_ERR_VALUE(ptr); }
static inline int IS_ERR_OR_NULL(const void *ptr) { return !ptr || (int)IS_ERR_VALUE(ptr); }

#define might_sleep()
#define lockdep_assert_held(l)
#define barrier() __asm__ volatile("" ::: "memory")
#define READ_ONCE(x) (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))

#endif
