// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// kmalloc and friends on top of rn_raw_alloc. Every block carries a 16-byte header with its size, so kfree and
// krealloc work without the caller passing it.
#include <linux/slab.h>
#include <rn/hooks.h>
#include <string.h>

#define RN_SLAB_HDR 16

static void *slabAlloc(size_t size, gfp_t flags) {
    if (size > (size_t)-1 - RN_SLAB_HDR) return nullptr;
    uint8_t *raw = static_cast<uint8_t *>(rn_raw_alloc(size + RN_SLAB_HDR));
    if (!raw) return nullptr;
    *reinterpret_cast<size_t *>(raw) = size;
    void *p = raw + RN_SLAB_HDR;
    if (flags & __GFP_ZERO) memset(p, 0, size);
    return p;
}

extern "C" {

void *kmalloc(size_t size, gfp_t flags) { return slabAlloc(size, flags); }
void *kzalloc(size_t size, gfp_t flags) { return slabAlloc(size, flags | __GFP_ZERO); }

void *kcalloc(size_t n, size_t size, gfp_t flags) {
    if (size && n > (size_t)-1 / size) return nullptr;
    return slabAlloc(n * size, flags | __GFP_ZERO);
}

void kfree(const void *ptr) {
    if (!ptr) return;
    uint8_t *raw = const_cast<uint8_t *>(static_cast<const uint8_t *>(ptr)) - RN_SLAB_HDR;
    rn_raw_free(raw, *reinterpret_cast<size_t *>(raw) + RN_SLAB_HDR);
}

void *krealloc(void *ptr, size_t size, gfp_t flags) {
    if (!ptr) return slabAlloc(size, flags);
    if (!size) { kfree(ptr); return nullptr; }
    size_t old = *reinterpret_cast<size_t *>(static_cast<uint8_t *>(ptr) - RN_SLAB_HDR);
    if (size <= old) return ptr;
    void *n = slabAlloc(size, flags);
    if (!n) return nullptr;                       // the old block stays valid, like in Linux
    memcpy(n, ptr, old);
    kfree(ptr);
    return n;
}

char *kstrdup(const char *s, gfp_t flags) {
    if (!s) return nullptr;
    size_t len = strlen(s) + 1;
    char *d = static_cast<char *>(slabAlloc(len, flags));
    if (d) memcpy(d, s, len);
    return d;
}

void *kmemdup(const void *src, size_t len, gfp_t flags) {
    void *d = slabAlloc(len, flags);
    if (d) memcpy(d, src, len);
    return d;
}

}
