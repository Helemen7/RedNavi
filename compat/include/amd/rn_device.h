// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef RN_AMD_DEVICE_H
#define RN_AMD_DEVICE_H

// The minimal stand-in for struct amdgpu_device: register access in the style amdgpu uses
// (RREG32, WREG32, RREG32_SOC15) so code ported from amdgpu can keep its register macros.

#include <linux/device.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/kernel.h>

enum rn_hw_ip {
    RN_MP0_HWIP,
    RN_MP1_HWIP,
    RN_DCE_HWIP,
    RN_GC_HWIP,
    RN_SDMA0_HWIP,
    RN_NBIO_HWIP,
    RN_MMHUB_HWIP,
    RN_SMUIO_HWIP,
    RN_OSSSYS_HWIP,
    RN_HWIP_MAX,
};

#define RN_MAX_INST 4
#define RN_MAX_SEG 6

// Byte offsets of the index/data pair that reaches any register or VRAM address.
#define RN_MM_INDEX 0x00
#define RN_MM_DATA 0x04
#define RN_MM_INDEX_HI 0x18

// Registers past the end of the register BAR (the doorbell routing of the NBIO, for one, is some 20 MB in) are reached through the NBIO's
// RSMU index/data pair, which is what amdgpu uses on these ASICs (nbio_v4_3_get_pcie_index_offset). Byte offsets in the BAR: NBIF segment 1
// base 0x14 dwords for the index and the next dword for the data. MM_INDEX does not reach those registers.
#define RN_RSMU_INDEX 0x50
#define RN_RSMU_DATA 0x54

struct rn_device {
    struct device dev;
    volatile u8 *rmmio;             // mapped register BAR
    size_t rmmio_size;
    u32 reg_offset[RN_HWIP_MAX][RN_MAX_INST][RN_MAX_SEG];   // dword base of each IP segment (IP discovery)
    spinlock_t mmio_idx_lock;
};

#ifdef __cplusplus
extern "C" {
#endif

void rn_device_init(struct rn_device *adev, const char *name, volatile void *rmmio, size_t rmmio_size);
u32 rn_rreg32(struct rn_device *adev, u32 reg);               // reg in dwords, like amdgpu

// VRAM through MM_INDEX / MM_DATA / MM_INDEX_HI, by offset from the start of VRAM (works whatever size BAR0 has).
u32 rn_vram_rd32(struct rn_device *adev, u64 vram_offset);
void rn_vram_wr32(struct rn_device *adev, u64 vram_offset, u32 v);
void rn_wreg32(struct rn_device *adev, u32 reg, u32 v);

#ifdef __cplusplus
}
#endif

#define RREG32(reg) rn_rreg32(adev, (reg))
#define WREG32(reg, v) rn_wreg32(adev, (reg), (v))
#define RREG32_SOC15(ip, inst, reg) rn_rreg32(adev, adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + (reg))
#define WREG32_SOC15(ip, inst, reg, v) rn_wreg32(adev, adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + (reg), (v))
#define SOC15_REG_OFFSET(ip, inst, reg) (adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + (reg))

#define REG_SET_FIELD(orig, reg, field, val) (((orig) & ~reg##__##field##_MASK) | (reg##__##field##_MASK & ((u32)(val) << reg##__##field##__SHIFT)))
#define REG_GET_FIELD(value, reg, field) (((value) & reg##__##field##_MASK) >> reg##__##field##__SHIFT)

#endif
