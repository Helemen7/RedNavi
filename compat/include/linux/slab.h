// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_SLAB_H
#define _LINUX_SLAB_H

#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// GFP_ATOMIC and GFP_KERNEL are the same here: the allocator never sleeps on a lock of ours.
#define GFP_KERNEL 0u
#define GFP_ATOMIC 0u
#define GFP_NOWAIT 0u
#define __GFP_ZERO 0x100u

void *kmalloc(size_t size, gfp_t flags);
void *kzalloc(size_t size, gfp_t flags);
void *kcalloc(size_t n, size_t size, gfp_t flags);
void *krealloc(void *ptr, size_t size, gfp_t flags);
void kfree(const void *ptr);
char *kstrdup(const char *s, gfp_t flags);
void *kmemdup(const void *src, size_t len, gfp_t flags);

#define kvmalloc(size, flags) kmalloc(size, flags)
#define kvzalloc(size, flags) kzalloc(size, flags)
#define kvfree(ptr) kfree(ptr)
#define kmalloc_array(n, size, flags) kcalloc(n, size, flags)

#ifdef __cplusplus
}
#endif

#endif
