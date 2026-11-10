// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_DMA_MAPPING_H
#define _LINUX_DMA_MAPPING_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/slab.h>

#ifdef __cplusplus
extern "C" {
#endif

// Physically contiguous, zeroed memory the GPU can reach: *dma_handle is the address to give the hardware.
// Assumes no IOMMU in the way (OpenCore drops the DMAR table by default); check this before trusting it on a
// machine with VT-d active.
void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t flags);
void dma_free_coherent(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle);

#ifdef __cplusplus
}
#endif

#endif
