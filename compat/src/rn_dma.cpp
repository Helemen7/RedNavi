// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <linux/dma-mapping.h>
#include <linux/spinlock.h>
#include <string.h>

#define RN_DMA_MAX 32

namespace {
struct Alloc {
    void *vaddr;
    IOBufferMemoryDescriptor *desc;
};
Alloc gAllocs[RN_DMA_MAX];
spinlock_t gDmaLock = __SPIN_LOCK_UNLOCKED(gDmaLock);
}

extern "C" {

void *dma_alloc_coherent(struct device *, size_t size, dma_addr_t *dma_handle, gfp_t) {
    IOBufferMemoryDescriptor *d = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, 0x0000FFFFFFFFF000ULL);
    if (!d) return nullptr;
    void *v = d->getBytesNoCopy();
    memset(v, 0, size);
    int slot = -1;
    spin_lock(&gDmaLock);
    for (int i = 0; i < RN_DMA_MAX; i++) if (!gAllocs[i].vaddr) { gAllocs[i] = { v, d }; slot = i; break; }
    spin_unlock(&gDmaLock);
    if (slot < 0) { d->release(); return nullptr; }
    *dma_handle = d->getPhysicalAddress();
    return v;
}

void dma_free_coherent(struct device *, size_t, void *cpu_addr, dma_addr_t) {
    IOBufferMemoryDescriptor *d = nullptr;
    spin_lock(&gDmaLock);
    for (int i = 0; i < RN_DMA_MAX; i++) if (gAllocs[i].vaddr == cpu_addr) { d = gAllocs[i].desc; gAllocs[i] = { nullptr, nullptr }; break; }
    spin_unlock(&gDmaLock);
    if (d) d->release();
}

}
