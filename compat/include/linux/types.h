// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// The guard is not the kernel's _LINUX_TYPES_H on purpose: on a Linux host the real header is included from here (below).
#ifndef RN_COMPAT_LINUX_TYPES_H
#define RN_COMPAT_LINUX_TYPES_H

#include <stddef.h>
#include <stdint.h>

// Host tests on Linux: this directory is in front of the system headers, and the C library's own headers include <linux/types.h>
// (from <sched.h>, for one) expecting the kernel's __u32 and friends. Hand them the real file, and take from the system what it
// already declares with its own types (__le16/32/64, loff_t).
#ifdef __linux__
#include_next <linux/types.h>
#include <sys/types.h>
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#ifndef __linux__
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef long long loff_t;
#endif
typedef uint64_t phys_addr_t;
typedef uint64_t dma_addr_t;
typedef uint64_t resource_size_t;
typedef int64_t ktime_t;
typedef unsigned int gfp_t;

#define __iomem
#define __user
#define __force
#define __init
#define __exit
#define __packed __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#define __maybe_unused __attribute__((unused))
#ifndef __always_inline                                         // the C library of a Linux host has it
#define __always_inline inline __attribute__((always_inline))
#endif
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)

#endif
