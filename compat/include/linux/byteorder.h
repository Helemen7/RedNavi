// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_BYTEORDER_H
#define _LINUX_BYTEORDER_H

#include <linux/types.h>

// x86 only: little endian is the native order.
#define cpu_to_le16(x) ((__le16)(x))
#define cpu_to_le32(x) ((__le32)(x))
#define cpu_to_le64(x) ((__le64)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define cpu_to_be32(x) __builtin_bswap32((u32)(x))
#define be32_to_cpu(x) __builtin_bswap32((u32)(x))

static inline u32 get_unaligned_le32(const void *p) { u32 v; __builtin_memcpy(&v, p, 4); return v; }
static inline u16 get_unaligned_le16(const void *p) { u16 v; __builtin_memcpy(&v, p, 2); return v; }
static inline u64 get_unaligned_le64(const void *p) { u64 v; __builtin_memcpy(&v, p, 8); return v; }
static inline void put_unaligned_le32(u32 v, void *p) { __builtin_memcpy(p, &v, 4); }

#endif
