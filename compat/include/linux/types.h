// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_TYPES_H
#define _LINUX_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint64_t phys_addr_t;
typedef uint64_t dma_addr_t;
typedef uint64_t resource_size_t;
typedef int64_t ktime_t;
typedef unsigned int gfp_t;
typedef long long loff_t;

#define __iomem
#define __user
#define __force
#define __init
#define __exit
#define __packed __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#define __maybe_unused __attribute__((unused))
#define __always_inline inline __attribute__((always_inline))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)

#endif
