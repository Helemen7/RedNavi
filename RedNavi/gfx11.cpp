#include <IOKit/IOLib.h>      // before the compat headers: their min() macro breaks libkern.h
#include "gfx11.h"
#include "gfx11_csb.h"

#include <string.h>

#include <linux/delay.h>
#include "gc_regs.h"
#include "mes_v11_api_def.h"
#include "osssys_regs.h"
#include "nbio_regs.h"
#include "mmhub_regs.h"

// rednavi_mes_host=1: the MES working memory (rings, write-back words, fences, contexts, MQDs, EOP buffers) lives in one host buffer mapped through the GART, as
// in Linux, where every one of these is a GTT buffer or a write-back slot. Its "offset" is chosen so that vaBase + offset is the GART address; the accessors
// below send an offset inside that range to the host mapping and everything else to VRAM through MM_INDEX/MM_DATA.
u64 gMesHostLogBase = 0;
volatile u8 *gMesHostMem = nullptr;
u64 gMesHostSize = 0;
static inline u32 mesRd32(struct rn_device *adev, u64 off) {
    if (gMesHostMem && off - gMesHostLogBase < gMesHostSize) return *reinterpret_cast<volatile u32 *>(gMesHostMem + (off - gMesHostLogBase));
    return rn_vram_rd32(adev, off);
}
static inline void mesWr32(struct rn_device *adev, u64 off, u32 v) {
    if (gMesHostMem && off - gMesHostLogBase < gMesHostSize) { *reinterpret_cast<volatile u32 *>(gMesHostMem + (off - gMesHostLogBase)) = v; return; }
    rn_vram_wr32(adev, off, v);
}
#define rn_vram_rd32 mesRd32
#define rn_vram_wr32 mesWr32


namespace {

// SDMA packet header: op in bits 7:0, sub_op in bits 15:8 (sdma_v6_0_pkt_open.h).
const u32 kSdmaOpWrite = 2, kSdmaSubopWriteLinear = 0;

inline u32 sdmaReg(struct rn_device *adev, u32 reg, u32 baseIdx) {
    return adev->reg_offset[RN_GC_HWIP][0][baseIdx] + reg;
}

}

// Note: the reg argument must reach RREG32_SOC15 unexpanded (it pastes reg##_BASE_IDX), so these are used directly below.
#define SDR(r) rn_rreg32(adev, sdmaReg(adev, r, r##_BASE_IDX))
#define SDW(r, v) rn_wreg32(adev, sdmaReg(adev, r, r##_BASE_IDX), (v))

extern "C" {

void rn_gart_clear(struct rn_device *adev, const struct rn_gmc *gmc) {
    const u64 pages = ((gmc->gart_end - gmc->gart_start) >> 12) + 1;
    for (u64 o = 0; o < pages * 8; o += 4) rn_vram_wr32(adev, gmc->pt_vram_offset + o, 0);   // every PTE invalid
}

static IOLock *gFlushLock;              // invalidation engine 17 is one request/acknowledge register pair: one flush at a time

// Invalidates the TLBs of every VMID in `mask` (bit n = VMID n) through engine 17, the one Linux uses for flushes done by register (amdgpu_gmc_flush_gpu_tlb).
static u32 gFlushFailures, gFlushLastAck;   // flushes whose acknowledge never came, and the last acknowledge register read
static int flushVmids(struct rn_device *adev, u32 mask) {
    if (gFlushLock) IOLockLock(gFlushLock);
    u32 req = 0;                                                             // gfxhub_v3_0_3_get_invalidate_req(flush type 0)
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG17_REQ, PER_VMID_INVALIDATE_REQ, mask);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG17_REQ, FLUSH_TYPE, 0);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG17_REQ, INVALIDATE_L2_PTES, 1);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG17_REQ, INVALIDATE_L2_PDE0, 1);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG17_REQ, INVALIDATE_L2_PDE1, 1);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG17_REQ, INVALIDATE_L2_PDE2, 1);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG17_REQ, INVALIDATE_L1_PTES, 1);
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG17_REQ, CLEAR_PROTECTION_FAULT_STATUS_ADDR, 0);
    WREG32_SOC15(RN_GC, 0, regGCVM_INVALIDATE_ENG17_REQ, req);
    int rc = -1;
    for (int i = 0; i < 100000; i++) {                                       // up to ~100 ms
        gFlushLastAck = RREG32_SOC15(RN_GC, 0, regGCVM_INVALIDATE_ENG17_ACK);
        if ((gFlushLastAck & mask) == mask) { rc = 0; break; }
        udelay(1);
    }
    if (rc) gFlushFailures++;
    // The engine acknowledged but a changed PTE was still used on hardware (gart-dynamic test): also do what rn_gfxhub_enable does to
    // start from empty caches, pulse the L1 TLB and L2 cache invalidation in GCVM_L2_CNTL2.
    u32 tmp = RREG32_SOC15(RN_GC, 0, regGCVM_L2_CNTL2);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_CNTL2, tmp);
    udelay(100);
    if (gFlushLock) IOLockUnlock(gFlushLock);
    return rc;
}

int rn_gart_flush(struct rn_device *adev) { return flushVmids(adev, 1); }

int rn_gart_update_page(struct rn_device *adev, const struct rn_gmc *gmc, u32 page, u64 bus) {
    if (bus) {
        rn_gart_map_page(adev, gmc, page, bus);
    } else {
        const u64 slot = gmc->pt_vram_offset + (u64)page * 8;
        rn_vram_wr32(adev, slot, 0); rn_vram_wr32(adev, slot + 4, 0);
    }
    return rn_gart_flush(adev);
}

u64 rn_vram_pte(u64 vram_offset) {            // a page of local VRAM at that offset (not the MC address: the PTE of a user address space takes the offset): valid, readable, writeable, executable, not system, not snooped, MTYPE_NC (0, Linux's default for local memory on GC 11)
    return (vram_offset & 0x0000FFFFFFFFF000ull) | 0x1ull | 0x10ull | 0x20ull | 0x40ull;
}

u64 rn_system_pte(u64 bus) {
    const u64 kMtypeUc = 3ull;
    return (bus & 0x0000FFFFFFFFF000ull) | 0x1ull | 0x2ull | 0x4ull | 0x10ull | 0x20ull | 0x40ull | (kMtypeUc << 48);
}

void rn_gart_map_page(struct rn_device *adev, const struct rn_gmc *gmc, u32 page, u64 bus) {
    // The PTE Linux writes for a GTT page: amdgpu_ttm_tt_pte_flags = amdgpu_ttm_tt_pde_flags (VALID | SYSTEM, plus SNOOPED for cached memory) | gart_pte_flags
    // (gmc_v11_0_gart_init: AMDGPU_PTE_MTYPE_NV10(MTYPE_UC) | AMDGPU_PTE_EXECUTABLE) | READABLE | WRITEABLE. Address in bits 47:12. EXECUTABLE matters: the command
    // processor fetching an indirect buffer, or the shader array fetching instructions, from a page without it takes a permission fault. MTYPE_UC is 3 (navi10_enum.h,
    // the enum that holds MTYPE_UC next to MTYPE_C_RW_US and the other cache-policy values; MTYPE_NC/WC/CC 0/1/2 are the TCC_MTYPE enum): bits 50:48 of the PTE.
    const u64 pte = rn_system_pte(bus);
    const u64 slot = gmc->pt_vram_offset + (u64)page * 8;
    rn_vram_wr32(adev, slot, lower_32_bits(pte));
    rn_vram_wr32(adev, slot + 4, upper_32_bits(pte));
}

#define VMR(reg, off) (adev->reg_offset[RN_GC_HWIP][0][reg##_BASE_IDX] + (reg) + (off))

// The user VMIDs (1 and up, the ones the MES does not own): a page table each in VRAM. Linux's setup_vmid_config (gmc_v11_0: amdgpu_vm_adjust_size(256 TB, 9, 3, 48))
// gives them PAGE_TABLE_DEPTH 3 and PAGE_TABLE_BLOCK_SIZE 0 over the 48-bit space: three directory levels of 512 entries (PDB2, PDB1, PDB0) above 512-entry page tables.
void rn_vm_enable_contexts(struct rn_device *adev, u64 startVa, u64 endVa, u32 depth, u32 firstVmid, u32 lastVmid) {
    u32 tmp = RREG32_SOC15(RN_GC, 0, regCP_DEBUG);                           // gfxhub_v3_0_set_fault_enable_default(true): the CP keeps going after a fault, faults go to the default page
    tmp = REG_SET_FIELD(tmp, CP_DEBUG, CPG_UTCL1_ERROR_HALT_DISABLE, 1);
    WREG32_SOC15(RN_GC, 0, regCP_DEBUG, tmp);
    tmp = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_CNTL);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, PDE1_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, PDE2_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, NACK_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, CRASH_ON_NO_RETRY_FAULT, 0);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, CRASH_ON_RETRY_FAULT, 0);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_CNTL, tmp);
    for (u32 vmid = firstVmid; vmid <= lastVmid; vmid++) {
        const u32 c = vmid - 1, a = 2 * (vmid - 1);                          // ctx_distance 1, ctx_addr_distance 2 (LO32 and HI32)
        tmp = rn_rreg32(adev, VMR(regGCVM_CONTEXT1_CNTL, c));
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, ENABLE_CONTEXT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, PAGE_TABLE_DEPTH, depth);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, PAGE_TABLE_BLOCK_SIZE, 0);
        tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT1_CNTL, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 0);   // no retry: a fault must not stall the L2 (no fault interrupt handler yet)
        rn_wreg32(adev, VMR(regGCVM_CONTEXT1_CNTL, c), tmp);
        rn_wreg32(adev, VMR(regGCVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32, a), (u32)(startVa >> 12));
        rn_wreg32(adev, VMR(regGCVM_CONTEXT1_PAGE_TABLE_START_ADDR_HI32, a), (u32)(startVa >> 44));
        rn_wreg32(adev, VMR(regGCVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32, a), (u32)(endVa >> 12));
        rn_wreg32(adev, VMR(regGCVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32, a), (u32)(endVa >> 44));
        rn_wreg32(adev, VMR(regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32, a), 0);
        rn_wreg32(adev, VMR(regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_HI32, a), 0);
    }
    WREG32_SOC15(RN_GC, 0, regGCVM_INVALIDATE_ENG17_ADDR_RANGE_LO32, 0xffffffffu);   // gfxhub_v3_0_program_invalidation: an invalidation covers the whole address range
    WREG32_SOC15(RN_GC, 0, regGCVM_INVALIDATE_ENG17_ADDR_RANGE_HI32, 0x1f);
}

// Points VMID `vmid` at a page table in VRAM (valid bit set, as VMID 0's base), or at nothing (tableVramOffset 0), then drops what the TLBs held for it.
int rn_vm_bind(struct rn_device *adev, u32 vmid, u64 tableVramOffset) {
    const u32 a = 2 * (vmid - 1);
    const u64 base = tableVramOffset ? (tableVramOffset | 1ull) : 0;
    rn_wreg32(adev, VMR(regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32, a), lower_32_bits(base));
    rn_wreg32(adev, VMR(regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_HI32, a), upper_32_bits(base));
    return flushVmids(adev, 1u << vmid);
}

void rn_vm_write_pte(struct rn_device *adev, u64 tableVramOffset, u32 index, u64 pte) {
    rn_vram_wr32(adev, tableVramOffset + (u64)index * 8, lower_32_bits(pte));
    rn_vram_wr32(adev, tableVramOffset + (u64)index * 8 + 4, upper_32_bits(pte));
}

int rn_vm_flush(struct rn_device *adev, u32 vmid) { return flushVmids(adev, 1u << vmid); }

void rn_vm_flush_stats(u32 out[2]) { out[0] = gFlushFailures; out[1] = gFlushLastAck; }

u64 rn_gart_read_pte(struct rn_device *adev, const struct rn_gmc *gmc, u32 page) {
    const u64 slot = gmc->pt_vram_offset + (u64)page * 8;
    return (u64)rn_vram_rd32(adev, slot) | ((u64)rn_vram_rd32(adev, slot + 4) << 32);
}

// GCVM_L2_PROTECTION_FAULT_STATUS keeps the first fault until this clears it (gmc_v11_0_process_interrupt).
void rn_vm_clear_fault(struct rn_device *adev) {
    u32 tmp = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_CNTL);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_CNTL, REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, CLEAR_PROTECTION_FAULT_STATUS_ADDR, 1));
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_CNTL, REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL, CLEAR_PROTECTION_FAULT_STATUS_ADDR, 0));
}

void rn_gart_unmap_page(struct rn_device *adev, const struct rn_gmc *gmc, u32 page) {
    const u64 slot = gmc->pt_vram_offset + (u64)page * 8;
    rn_vram_wr32(adev, slot, 0);
    rn_vram_wr32(adev, slot + 4, 0);
}

// A GART page that points at VRAM: VALID | EXECUTABLE | READABLE | WRITEABLE, no SYSTEM bit, the address is the MC address of the VRAM page.
void rn_gart_map_vram_page(struct rn_device *adev, const struct rn_gmc *gmc, u32 page, u64 mc) {
    const u64 pte = (mc & 0x0000FFFFFFFFF000ull) | 0x1ull | 0x10ull | 0x20ull | 0x40ull;
    const u64 slot = gmc->pt_vram_offset + (u64)page * 8;
    rn_vram_wr32(adev, slot, lower_32_bits(pte));
    rn_vram_wr32(adev, slot + 4, upper_32_bits(pte));
}

void rn_gfxhub_enable(struct rn_device *adev, const struct rn_gmc *gmc) {
    u32 tmp;

    // init_gart_aperture_regs: the VMID 0 page table (physical VRAM offset | valid) and the GART range in 4 KB pages
    u64 pt_base = gmc->pt_vram_offset | 1ull;
    WREG32_SOC15(RN_GC, 0, regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32, lower_32_bits(pt_base));
    WREG32_SOC15(RN_GC, 0, regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32, upper_32_bits(pt_base));
    WREG32_SOC15(RN_GC, 0, regGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32, (u32)(gmc->gart_start >> 12));
    WREG32_SOC15(RN_GC, 0, regGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32, (u32)(gmc->gart_start >> 44));
    WREG32_SOC15(RN_GC, 0, regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32, (u32)(gmc->gart_end >> 12));
    WREG32_SOC15(RN_GC, 0, regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32, (u32)(gmc->gart_end >> 44));

    // init_system_aperture_regs: AGP off (start above end), the system aperture covers VRAM
    const u64 agp_start = 0xFFFFFFFFFFFFull, agp_end = 0;
    WREG32_SOC15(RN_GC, 0, regGCMC_VM_AGP_BASE, 0);
    WREG32_SOC15(RN_GC, 0, regGCMC_VM_AGP_BOT, (u32)(agp_start >> 24));
    WREG32_SOC15(RN_GC, 0, regGCMC_VM_AGP_TOP, (u32)(agp_end >> 24));
    WREG32_SOC15(RN_GC, 0, regGCMC_VM_SYSTEM_APERTURE_LOW_ADDR, (u32)(min(gmc->fb_start, agp_start) >> 18));
    WREG32_SOC15(RN_GC, 0, regGCMC_VM_SYSTEM_APERTURE_HIGH_ADDR, (u32)(max(gmc->fb_end, agp_end) >> 18));
    WREG32_SOC15(RN_GC, 0, regGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB, (u32)(gmc->scratch_vram_offset >> 12));
    WREG32_SOC15(RN_GC, 0, regGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB, (u32)(gmc->scratch_vram_offset >> 44));
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32, (u32)(gmc->dummy_vram_offset >> 12));
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32, (u32)(gmc->dummy_vram_offset >> 44));
    tmp = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_CNTL2);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL2, ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_PROTECTION_FAULT_CNTL2, ENABLE_RETRY_FAULT_INTERRUPT, 1);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_CNTL2, tmp);

    // init_tlb_regs
    tmp = RREG32_SOC15(RN_GC, 0, regGCMC_VM_MX_L1_TLB_CNTL);
    tmp = REG_SET_FIELD(tmp, GCMC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 1);
    tmp = REG_SET_FIELD(tmp, GCMC_VM_MX_L1_TLB_CNTL, SYSTEM_ACCESS_MODE, 3);
    tmp = REG_SET_FIELD(tmp, GCMC_VM_MX_L1_TLB_CNTL, ENABLE_ADVANCED_DRIVER_MODEL, 1);
    tmp = REG_SET_FIELD(tmp, GCMC_VM_MX_L1_TLB_CNTL, SYSTEM_APERTURE_UNMAPPED_ACCESS, 0);
    tmp = REG_SET_FIELD(tmp, GCMC_VM_MX_L1_TLB_CNTL, ECO_BITS, 0);
    tmp = REG_SET_FIELD(tmp, GCMC_VM_MX_L1_TLB_CNTL, MTYPE, 3);              // MTYPE_UC
    WREG32_SOC15(RN_GC, 0, regGCMC_VM_MX_L1_TLB_CNTL, tmp);

    // init_cache_regs (no translate_further: bank select 9, big-K fragment 6)
    tmp = RREG32_SOC15(RN_GC, 0, regGCVM_L2_CNTL);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, ENABLE_L2_CACHE, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, ENABLE_L2_FRAGMENT_PROCESSING, 0);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, L2_PDE0_CACHE_TAG_GENERATION_MODE, 0);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, PDE_FAULT_CLASSIFICATION, 0);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, CONTEXT1_IDENTITY_ACCESS_MODE, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL, IDENTITY_MODE_FRAGMENT_SIZE, 0);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_CNTL, tmp);
    tmp = RREG32_SOC15(RN_GC, 0, regGCVM_L2_CNTL2);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_CNTL2, tmp);
    tmp = 0x80100007u;                                                       // gfxhub_v3_0_3.c redefines the default (gc_11_0_0 has 0x80120007)
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL3, BANK_SELECT, 9);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL3, L2_CACHE_BIGK_FRAGMENT_SIZE, 6);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_CNTL3, tmp);
    tmp = 0x000000c1u;
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL4, VMC_TAP_PDE_REQUEST_PHYSICAL, 0);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL4, VMC_TAP_PTE_REQUEST_PHYSICAL, 0);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_CNTL4, tmp);
    tmp = 0x00003fe0u;
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL5, L2_CACHE_SMALLK_FRAGMENT_SIZE, 0);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_CNTL5, tmp);

    // enable_system_domain (VMID 0, single level)
    tmp = RREG32_SOC15(RN_GC, 0, regGCVM_CONTEXT0_CNTL);
    tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT0_CNTL, ENABLE_CONTEXT, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT0_CNTL, PAGE_TABLE_DEPTH, 0);
    tmp = REG_SET_FIELD(tmp, GCVM_CONTEXT0_CNTL, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 0);
    WREG32_SOC15(RN_GC, 0, regGCVM_CONTEXT0_CNTL, tmp);

    // disable_identity_aperture
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32, 0xFFFFFFFF);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32, 0x0000000F);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32, 0);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32, 0);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32, 0);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32, 0);
}

// mmhub_v3_0_gart_enable (Linux mmhub_v3_0.c), the second memory hub. Linux programs it right after the gfxhub (gmc_v11_0_gart_enable) with the same page table and the same
// GART range; the MES, the SDMA and the multimedia engines reach memory through it. Until now only the gfxhub knew about the GART window, so an access to host memory from a
// client of the mmhub had no translation. The registers that Linux fills from compile-time defaults (L2_CNTL3/4/5) are read-modify-written from their reset value here,
// since the default header is not in the sources used; the fields set are the same ones.
void rn_mmhub_enable(struct rn_device *adev, const struct rn_gmc *gmc) {
    u32 tmp;
    // init_gart_aperture_regs
    const u64 pt_base = gmc->pt_vram_offset | 1ull;
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32, lower_32_bits(pt_base));
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32, upper_32_bits(pt_base));
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32, (u32)(gmc->gart_start >> 12));
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32, (u32)(gmc->gart_start >> 44));
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32, (u32)(gmc->gart_end >> 12));
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32, (u32)(gmc->gart_end >> 44));
    // init_system_aperture_regs (the same values as the gfxhub's)
    const u64 agp_start = 0xFFFFFFFFFFFFull, agp_end = 0;
    WREG32_SOC15(RN_MMHUB, 0, regMMMC_VM_AGP_BASE, 0);
    WREG32_SOC15(RN_MMHUB, 0, regMMMC_VM_AGP_BOT, (u32)(agp_start >> 24));
    WREG32_SOC15(RN_MMHUB, 0, regMMMC_VM_AGP_TOP, (u32)(agp_end >> 24));
    WREG32_SOC15(RN_MMHUB, 0, regMMMC_VM_SYSTEM_APERTURE_LOW_ADDR, (u32)(min(gmc->fb_start, agp_start) >> 18));
    WREG32_SOC15(RN_MMHUB, 0, regMMMC_VM_SYSTEM_APERTURE_HIGH_ADDR, (u32)(max(gmc->fb_end, agp_end) >> 18));
    WREG32_SOC15(RN_MMHUB, 0, regMMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB, (u32)(gmc->scratch_vram_offset >> 12));
    WREG32_SOC15(RN_MMHUB, 0, regMMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB, (u32)(gmc->scratch_vram_offset >> 44));
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32, (u32)(gmc->dummy_vram_offset >> 12));
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32, (u32)(gmc->dummy_vram_offset >> 44));
    tmp = RREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_PROTECTION_FAULT_CNTL2);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL2, ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY, 1);
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_PROTECTION_FAULT_CNTL2, tmp);
    // init_tlb_regs
    tmp = RREG32_SOC15(RN_MMHUB, 0, regMMMC_VM_MX_L1_TLB_CNTL);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 1);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, SYSTEM_ACCESS_MODE, 3);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, ENABLE_ADVANCED_DRIVER_MODEL, 1);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, SYSTEM_APERTURE_UNMAPPED_ACCESS, 0);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, ECO_BITS, 0);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, MTYPE, 3);              // MTYPE_UC
    WREG32_SOC15(RN_MMHUB, 0, regMMMC_VM_MX_L1_TLB_CNTL, tmp);
    // init_cache_regs (no translate_further: bank select 9, big-K fragment 6)
    tmp = RREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CNTL);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, ENABLE_L2_CACHE, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, ENABLE_L2_FRAGMENT_PROCESSING, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, L2_PDE0_CACHE_TAG_GENERATION_MODE, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, PDE_FAULT_CLASSIFICATION, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, CONTEXT1_IDENTITY_ACCESS_MODE, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, IDENTITY_MODE_FRAGMENT_SIZE, 0);
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CNTL, tmp);
    tmp = RREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CNTL2);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CNTL2, tmp);
    tmp = RREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CNTL3);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL3, BANK_SELECT, 9);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL3, L2_CACHE_BIGK_FRAGMENT_SIZE, 6);
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CNTL3, tmp);
    tmp = RREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CNTL4);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL4, VMC_TAP_PDE_REQUEST_PHYSICAL, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL4, VMC_TAP_PTE_REQUEST_PHYSICAL, 0);
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CNTL4, tmp);
    tmp = RREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CNTL5);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL5, L2_CACHE_SMALLK_FRAGMENT_SIZE, 0);
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CNTL5, tmp);
    // enable_system_domain (VMID 0, single level)
    tmp = RREG32_SOC15(RN_MMHUB, 0, regMMVM_CONTEXT0_CNTL);
    tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT0_CNTL, ENABLE_CONTEXT, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT0_CNTL, PAGE_TABLE_DEPTH, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT0_CNTL, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 0);
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_CONTEXT0_CNTL, tmp);
    // disable_identity_aperture
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32, 0xFFFFFFFF);
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32, 0x0000000F);
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32, 0);
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32, 0);
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32, 0);
    WREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32, 0);
}

// The registers rn_mmhub_enable changes, for a before/after look: context 0 control, TLB control, L2 control, page table base, start and end.
void rn_mmhub_state(struct rn_device *adev, u32 out[8]) {
    out[0] = RREG32_SOC15(RN_MMHUB, 0, regMMVM_CONTEXT0_CNTL);
    out[1] = RREG32_SOC15(RN_MMHUB, 0, regMMMC_VM_MX_L1_TLB_CNTL);
    out[2] = RREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_CNTL);
    out[3] = RREG32_SOC15(RN_MMHUB, 0, regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32);
    out[4] = RREG32_SOC15(RN_MMHUB, 0, regMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32);
    out[5] = RREG32_SOC15(RN_MMHUB, 0, regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32);
    out[6] = RREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_PROTECTION_FAULT_STATUS);
    out[7] = RREG32_SOC15(RN_MMHUB, 0, regMMVM_L2_PROTECTION_FAULT_ADDR_LO32);
}

void rn_sdma0_test(struct rn_device *adev, u64 vaBase, u64 ringOff, u32 ringBytes, u64 targetOff, struct rn_sdma_result *res) {
    memset(res, 0, sizeof(*res));
    const u64 ringMc = vaBase + ringOff;
    const u64 rptrOff = ringOff + 0x1000, wptrOff = ringOff + 0x1008;      // pointer shadows after the ring page
    const u64 targetMc = vaBase + targetOff;
    u32 tmp;

    for (u32 o = 0; o < ringBytes; o += 4) rn_vram_wr32(adev, ringOff + o, 0);
    rn_vram_wr32(adev, rptrOff, 0); rn_vram_wr32(adev, rptrOff + 4, 0);
    rn_vram_wr32(adev, wptrOff, 0); rn_vram_wr32(adev, wptrOff + 4, 0);
    rn_vram_wr32(adev, targetOff, 0xCAFEDEADu);
    res->pre_value = rn_vram_rd32(adev, targetOff);
    res->step = 1;

    // the packet: WRITE_LINEAR of one dword, as sdma_v6_0_ring_test_ring
    const u32 pkt[5] = {kSdmaOpWrite | (kSdmaSubopWriteLinear << 8), lower_32_bits(targetMc), upper_32_bits(targetMc), 0, 0xDEADBEEFu};
    for (u32 i = 0; i < 5; i++) rn_vram_wr32(adev, ringOff + i * 4, pkt[i]);

    // sdma_v6_0_gfx_resume_instance, instance 0, no doorbell
    SDW(regSDMA0_SEM_WAIT_FAIL_TIMER_CNTL, 0);
    u32 rbBufsz = 0;
    while ((4u << rbBufsz) < ringBytes) rbBufsz++;                          // order_base_2(ring_size / 4)
    u32 rbCntl = SDR(regSDMA0_QUEUE0_RB_CNTL);
    rbCntl = REG_SET_FIELD(rbCntl, SDMA0_QUEUE0_RB_CNTL, RB_SIZE, rbBufsz);
    rbCntl = REG_SET_FIELD(rbCntl, SDMA0_QUEUE0_RB_CNTL, RB_PRIV, 1);
    SDW(regSDMA0_QUEUE0_RB_CNTL, rbCntl);
    SDW(regSDMA0_QUEUE0_RB_RPTR, 0);
    SDW(regSDMA0_QUEUE0_RB_RPTR_HI, 0);
    SDW(regSDMA0_QUEUE0_RB_WPTR, 0);
    SDW(regSDMA0_QUEUE0_RB_WPTR_HI, 0);
    SDW(regSDMA0_QUEUE0_RB_WPTR_POLL_ADDR_LO, lower_32_bits(vaBase + wptrOff));
    SDW(regSDMA0_QUEUE0_RB_WPTR_POLL_ADDR_HI, upper_32_bits(vaBase + wptrOff));
    SDW(regSDMA0_QUEUE0_RB_RPTR_ADDR_HI, upper_32_bits(vaBase + rptrOff));
    SDW(regSDMA0_QUEUE0_RB_RPTR_ADDR_LO, lower_32_bits(vaBase + rptrOff) & 0xFFFFFFFCu);
    rbCntl = REG_SET_FIELD(rbCntl, SDMA0_QUEUE0_RB_CNTL, RPTR_WRITEBACK_ENABLE, 1);
    rbCntl = REG_SET_FIELD(rbCntl, SDMA0_QUEUE0_RB_CNTL, WPTR_POLL_ENABLE, 0);
    rbCntl = REG_SET_FIELD(rbCntl, SDMA0_QUEUE0_RB_CNTL, F32_WPTR_POLL_ENABLE, 1);
    SDW(regSDMA0_QUEUE0_RB_BASE, (u32)(ringMc >> 8));
    SDW(regSDMA0_QUEUE0_RB_BASE_HI, (u32)(ringMc >> 40));
    SDW(regSDMA0_QUEUE0_MINOR_PTR_UPDATE, 1);
    SDW(regSDMA0_QUEUE0_RB_WPTR, 0);
    SDW(regSDMA0_QUEUE0_RB_WPTR_HI, 0);
    tmp = SDR(regSDMA0_QUEUE0_DOORBELL);
    tmp = REG_SET_FIELD(tmp, SDMA0_QUEUE0_DOORBELL, ENABLE, 0);
    SDW(regSDMA0_QUEUE0_DOORBELL, tmp);
    SDW(regSDMA0_QUEUE0_MINOR_PTR_UPDATE, 0);
    tmp = SDR(regSDMA0_UTCL1_CNTL);
    tmp = REG_SET_FIELD(tmp, SDMA0_UTCL1_CNTL, RESP_MODE, 3);
    tmp = REG_SET_FIELD(tmp, SDMA0_UTCL1_CNTL, REDO_DELAY, 9);
    SDW(regSDMA0_UTCL1_CNTL, tmp);
    res->step = 2;

    tmp = SDR(regSDMA0_F32_CNTL);                                           // unhalt the engine
    tmp = REG_SET_FIELD(tmp, SDMA0_F32_CNTL, HALT, 0);
    tmp = REG_SET_FIELD(tmp, SDMA0_F32_CNTL, TH1_RESET, 0);
    SDW(regSDMA0_F32_CNTL, tmp);
    rbCntl = REG_SET_FIELD(rbCntl, SDMA0_QUEUE0_RB_CNTL, RB_ENABLE, 1);
    SDW(regSDMA0_QUEUE0_RB_CNTL, rbCntl);
    u32 ibCntl = SDR(regSDMA0_QUEUE0_IB_CNTL);
    ibCntl = REG_SET_FIELD(ibCntl, SDMA0_QUEUE0_IB_CNTL, IB_ENABLE, 1);
    SDW(regSDMA0_QUEUE0_IB_CNTL, ibCntl);
    res->step = 3;

    // With F32_WPTR_POLL_ENABLE the SDMA firmware polls the wptr shadow in memory (the doorbell path of sdma_v6_0_ring_set_wptr
    // stores wptr << 2 there before ringing), so the shadow is written as well as the register.
    rn_vram_wr32(adev, wptrOff, 5u << 2);
    rn_vram_wr32(adev, wptrOff + 4, 0);
    SDW(regSDMA0_QUEUE0_RB_WPTR, 5u << 2);                                  // 5 dwords, wptr in bytes
    SDW(regSDMA0_QUEUE0_RB_WPTR_HI, 0);
    res->step = 4;

    for (int i = 0; i < 2000; i++) {                                        // up to 2 s
        res->value = rn_vram_rd32(adev, targetOff);
        if (res->value == 0xDEADBEEFu) { res->ok = 1; break; }
        msleep(1);
    }
    res->rptr = SDR(regSDMA0_QUEUE0_RB_RPTR);
    res->wptr = SDR(regSDMA0_QUEUE0_RB_WPTR);
    res->f32_cntl = SDR(regSDMA0_F32_CNTL);
    res->rb_cntl = SDR(regSDMA0_QUEUE0_RB_CNTL);
    res->fault_status = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_STATUS);
    res->fault_addr_lo = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_LO32);
    res->fault_addr_hi = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_HI32);
    res->sdma_status = SDR(regSDMA0_STATUS_REG);
    res->sdma_status1 = SDR(regSDMA0_STATUS1_REG);
    res->ctx_status = SDR(regSDMA0_QUEUE0_CONTEXT_STATUS);
    res->version = SDR(regSDMA0_VERSION);
    res->cntl = SDR(regSDMA0_CNTL);
    res->power_cntl = SDR(regSDMA0_POWER_CNTL);
    res->checksum = SDR(regSDMA0_UCODE_CHECKSUM);
    res->freeze = SDR(regSDMA0_FREEZE);
    res->chicken = SDR(regSDMA0_CHICKEN_BITS);
    res->program = SDR(regSDMA0_PROGRAM);
    res->ring0 = rn_vram_rd32(adev, ringOff);
    res->ring3 = rn_vram_rd32(adev, ringOff + 12);
    res->rptr_shadow = rn_vram_rd32(adev, rptrOff);
    res->wptr_shadow = rn_vram_rd32(adev, wptrOff);
    res->step = 5;
}

// soc21_grbm_select: pick the ME / pipe / VMID / queue that the per-queue registers below refer to.
static void grbmSelect(struct rn_device *adev, u32 me, u32 pipe, u32 vmid, u32 queue) {
    u32 t = REG_SET_FIELD(0, GRBM_GFX_CNTL, PIPEID, pipe);
    t = REG_SET_FIELD(t, GRBM_GFX_CNTL, MEID, me);
    t = REG_SET_FIELD(t, GRBM_GFX_CNTL, VMID, vmid);
    t = REG_SET_FIELD(t, GRBM_GFX_CNTL, QUEUEID, queue);
    WREG32_SOC15(RN_GC, 0, regGRBM_GFX_CNTL, t);
}

#define RN_PACKET3(op, n) ((3u << 30) | (((n) & 0x3FFFu) << 16) | (((op) & 0xFFu) << 8))
#define RN_PACKET3_NOP 0x10u
#define RN_PACKET3_SET_UCONFIG_REG 0x79u
#define RN_PACKET3_SET_UCONFIG_REG_START 0x0000c000u

void rn_gfx_ring_test(struct rn_device *adev, u64 vaBase, u64 ringOff, u32 ringBytes, const struct rn_cp_start *pc,
                      struct rn_gfx_result *res) {
    memset(res, 0, sizeof(*res));
    const u64 ringVa = vaBase + ringOff;
    const u64 rptrOff = ringOff + 0x1000, wptrOff = ringOff + 0x1008;      // pointer shadows after the ring page
    u32 tmp;
    res->me_cntl_before = RREG32_SOC15(RN_GC, 0, regCP_ME_CNTL);
    res->cp_stat_before = RREG32_SOC15(RN_GC, 0, regCP_STAT);

    // gfx_v11_0_config_gfx_rs64: the entry point of each RS64 processor, then a reset pulse of its pipes
    for (u32 pipe = 0; pipe < 2; pipe++) {
        grbmSelect(adev, 0, pipe, 0, 0);
        WREG32_SOC15(RN_GC, 0, regCP_PFP_PRGRM_CNTR_START, (pc->pfp_hi << 30) | (pc->pfp_lo >> 2));
        WREG32_SOC15(RN_GC, 0, regCP_PFP_PRGRM_CNTR_START_HI, pc->pfp_hi >> 2);
    }
    grbmSelect(adev, 0, 0, 0, 0);
    tmp = RREG32_SOC15(RN_GC, 0, regCP_ME_CNTL);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE0_RESET, 1);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE1_RESET, 1);
    WREG32_SOC15(RN_GC, 0, regCP_ME_CNTL, tmp);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE0_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE1_RESET, 0);
    WREG32_SOC15(RN_GC, 0, regCP_ME_CNTL, tmp);
    for (u32 pipe = 0; pipe < 2; pipe++) {
        grbmSelect(adev, 0, pipe, 0, 0);
        WREG32_SOC15(RN_GC, 0, regCP_ME_PRGRM_CNTR_START, (pc->me_hi << 30) | (pc->me_lo >> 2));
        WREG32_SOC15(RN_GC, 0, regCP_ME_PRGRM_CNTR_START_HI, pc->me_hi >> 2);
    }
    grbmSelect(adev, 0, 0, 0, 0);
    tmp = RREG32_SOC15(RN_GC, 0, regCP_ME_CNTL);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE0_RESET, 1);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE1_RESET, 1);
    WREG32_SOC15(RN_GC, 0, regCP_ME_CNTL, tmp);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE0_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE1_RESET, 0);
    WREG32_SOC15(RN_GC, 0, regCP_ME_CNTL, tmp);
    for (u32 pipe = 0; pipe < 4; pipe++) {
        grbmSelect(adev, 1, pipe, 0, 0);
        WREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_PRGRM_CNTR_START, (pc->mec_lo >> 2) | (pc->mec_hi << 30));
        WREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_PRGRM_CNTR_START_HI, pc->mec_hi >> 2);
    }
    grbmSelect(adev, 0, 0, 0, 0);
    tmp = RREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_CNTL);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE0_RESET, 1);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE1_RESET, 1);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE2_RESET, 1);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE3_RESET, 1);
    WREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_CNTL, tmp);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE0_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE1_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE2_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE3_RESET, 0);
    WREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_CNTL, tmp);
    res->step = 1;

    // the packet of ring_test_ring: SET_UCONFIG_REG SCRATCH_REG0 = 0xDEADBEEF, padded with a NOP packet to 8 dwords
    const u32 scratchDw = 0xc000u + 0x40u;                                   // regSCRATCH_REG0 is at GC base 1 (0xa000) + 0x2040
    const u32 pkt[8] = {RN_PACKET3(RN_PACKET3_SET_UCONFIG_REG, 1), scratchDw - RN_PACKET3_SET_UCONFIG_REG_START, 0xDEADBEEFu,
                        RN_PACKET3(RN_PACKET3_NOP, 3), 0, 0, 0, 0};
    for (u32 o = 0; o < ringBytes; o += 4) rn_vram_wr32(adev, ringOff + o, 0);
    for (u32 i = 0; i < 8; i++) rn_vram_wr32(adev, ringOff + i * 4, pkt[i]);
    rn_vram_wr32(adev, rptrOff, 0); rn_vram_wr32(adev, rptrOff + 4, 0);
    rn_vram_wr32(adev, wptrOff, 0); rn_vram_wr32(adev, wptrOff + 4, 0);
    WREG32_SOC15(RN_GC, 0, regSCRATCH_REG0, 0xCAFEDEADu);

    // The gfx queue of GC 11 is a hardware queue (CP_GFX_HQD_*) that Linux fills from an in-memory MQD (gfx_v11_0_gfx_mqd_init);
    // the legacy CP_RB0_* registers are not what the CP runs, and with CP_GFX_MQD_BASE_ADDR at 0 the CP reads the MQD at address 0
    // when it starts and faults (CPG, GCVM_L2_PROTECTION_FAULT_STATUS 0xd32, seen on hardware). So the same values go straight
    // into the HQD registers, and a zeroed MQD page stands behind it.
    const u64 mqdVa = vaBase + ringOff + 0x2000;
    for (u32 o = 0; o < 0x1000; o += 4) rn_vram_wr32(adev, ringOff + 0x2000 + o, 0);
    grbmSelect(adev, 0, 0, 0, 0);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_ACTIVE, 0);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_MQD_BASE_ADDR, lower_32_bits(mqdVa) & 0xfffffffcu);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_MQD_BASE_ADDR_HI, upper_32_bits(mqdVa));
    tmp = regCP_GFX_MQD_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, VMID, 0);
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, PRIV_STATE, 1);
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, CACHE_POLICY, 0);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_MQD_CONTROL, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_VMID, 0);
    tmp = regCP_GFX_HQD_QUANTUM_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_QUANTUM, QUANTUM_EN, 1);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_QUANTUM, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_BASE, lower_32_bits(ringVa >> 8));
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_BASE_HI, upper_32_bits(ringVa >> 8));
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_RPTR_ADDR, lower_32_bits(vaBase + rptrOff) & 0xfffffffcu);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_RPTR_ADDR_HI, upper_32_bits(vaBase + rptrOff) & 0xFFFFu);
    WREG32_SOC15(RN_GC, 0, regCP_RB_WPTR_POLL_ADDR_LO, lower_32_bits(vaBase + wptrOff) & 0xfffffffcu);
    WREG32_SOC15(RN_GC, 0, regCP_RB_WPTR_POLL_ADDR_HI, upper_32_bits(vaBase + wptrOff) & 0xFFFFu);
    u32 qOrder = 0;
    while ((4u << qOrder) < ringBytes) qOrder++;                             // order_base_2(queue_size / 4)
    tmp = regCP_GFX_HQD_CNTL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BUFSZ, qOrder - 1);
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BLKSZ, qOrder - 3);
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, KMD_QUEUE, 1);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_CNTL, tmp);
    tmp = RREG32_SOC15(RN_GC, 0, regCP_RB_DOORBELL_CONTROL);                 // no doorbell: the wptr register is written directly
    tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 0);
    WREG32_SOC15(RN_GC, 0, regCP_RB_DOORBELL_CONTROL, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_RPTR, regCP_GFX_HQD_RPTR_DEFAULT);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_WPTR, 0);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_WPTR_HI, 0);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_ACTIVE, 1);
    res->step = 2;

    // gfx_v11_0_cp_gfx_start, without the clear state: MAX_CONTEXT, DEVICE_ID, then unhalt PFP and ME
    WREG32_SOC15(RN_GC, 0, regCP_MAX_CONTEXT, 7);
    WREG32_SOC15(RN_GC, 0, regCP_DEVICE_ID, 1);
    tmp = RREG32_SOC15(RN_GC, 0, regCP_ME_CNTL);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_HALT, 0);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_HALT, 0);
    tmp &= ~0x01000000u;   // bit 24 (reset default 1) reads 0 on Linux after its bring-up
    WREG32_SOC15(RN_GC, 0, regCP_ME_CNTL, tmp);
    res->me_cntl_imm = RREG32_SOC15(RN_GC, 0, regCP_ME_CNTL);
    mdelay(10);
    res->me_cntl_10ms = RREG32_SOC15(RN_GC, 0, regCP_ME_CNTL);
    res->rb0_cntl_rd = RREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_CNTL);
    res->rb0_base_rd = RREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_BASE);
    res->rb0_base_hi_rd = RREG32_SOC15(RN_GC, 0, regCP_GFX_MQD_BASE_ADDR);
    res->rb_active_rd = RREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_ACTIVE);
    res->max_ctx_rd = RREG32_SOC15(RN_GC, 0, regCP_MAX_CONTEXT);
    res->device_id_rd = RREG32_SOC15(RN_GC, 0, regCP_DEVICE_ID);
    res->pfp_pc_rd = RREG32_SOC15(RN_GC, 0, regCP_PFP_PRGRM_CNTR_START);
    res->me_pc_rd = RREG32_SOC15(RN_GC, 0, regCP_ME_PRGRM_CNTR_START);
    for (int i = 0; i < 1000; i++) {                                         // CP_STAT reads 0 once the CP is idle and up
        if (RREG32_SOC15(RN_GC, 0, regCP_STAT) == 0) break;
        udelay(1);
    }
    res->step = 3;

    rn_vram_wr32(adev, wptrOff, 8);                                          // the polled shadow as well as the register
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_WPTR, 8);                           // 8 dwords
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_WPTR_HI, 0);
    res->step = 4;
    static const u32 sampleDelayMs[8] = {0, 1, 1, 3, 5, 10, 30, 50};        // cumulative: 0, 1, 2, 5, 10, 20, 50, 100 ms
    for (u32 i = 0; i < 8; i++) {
        if (sampleDelayMs[i]) msleep(sampleDelayMs[i]);
        res->trace[i][0] = RREG32_SOC15(RN_GC, 0, regCP_ME_CNTL);
        res->trace[i][1] = RREG32_SOC15(RN_GC, 0, regCP_STAT);
        res->trace[i][2] = RREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_RPTR);
    }
    res->fault_status = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_STATUS);
    res->fault_addr_lo = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_LO32);
    res->fault_addr_hi = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_HI32);
    for (int i = 0; i < 2000; i++) {                                         // up to 2 s
        res->scratch = RREG32_SOC15(RN_GC, 0, regSCRATCH_REG0);
        if (res->scratch == 0xDEADBEEFu) { res->ok = 1; break; }
        msleep(1);
    }
    res->me_cntl = RREG32_SOC15(RN_GC, 0, regCP_ME_CNTL);
    res->cp_stat = RREG32_SOC15(RN_GC, 0, regCP_STAT);
    res->rptr = RREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_RPTR);
    res->wptr = RREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_WPTR);
    res->rptr_shadow = rn_vram_rd32(adev, rptrOff);
    res->ring0 = rn_vram_rd32(adev, ringOff);
}

static bool gMapped;                    // the compute queue in use was mapped through the KIQ (rn_kiq_map_compute)
static u32 gMappedWptr;
static u32 gRingFullWaits;           // submits that found the mapped ring without room and had to wait for the command processor
static u64 gMappedRingOff = 0;       // ringOff of the last mapped submit: the block that holds the rptr and wptr words (+0x1000, +0x1008)
static u64 gMappedRingAt = 0;        // where the mapped queue's ring data starts inside its block (kRingAt for a queue the MES mapped, 0 for the older KIQ-mapped one)
static u32 gMappedRingBytes = 0;     // its ring size, 0: the caller's
static u32 gMappedPipe = 0;
static u32 gMappedQueue = 3;           // queue index (ME 1, pipe 0) of the mapped queue: 3 through the KIQ, 4 through the MES
static const u32 kMappedDoorbellDword = 8;
static u32 gMappedDoorbell = kMappedDoorbellDword;   // doorbell dword of the mapped queue: 8 through the KIQ, in the user queue range (26..276) for a MES process queue
static IOLock *gSubmitLock;             // one writer at a time on the compute ring, its wptr and the doorbell (user clients, the watch timer)
static u32 gComputeWptr;                // running write pointer of the compute queue, in dwords
static volatile u8 *gDoorbellBase;      // mapped doorbell BAR, or null for plain register submission
static u32 gDoorbellDword;              // the compute queue's doorbell, in dwords

void rn_set_doorbell(volatile void *base, u32 dwordIndex) {
    gDoorbellBase = static_cast<volatile u8 *>(base);
    gDoorbellDword = dwordIndex;
}

void rn_doorbell_aperture_enable(struct rn_device *adev, int writeS2a, u32 before[3]) {
    u32 tmp = RREG32_SOC15(RN_NBIO, 0, regRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN);   // nbio_v4_3_enable_doorbell_aperture
    before[0] = tmp;
    before[1] = RREG32_SOC15(RN_NBIO, 0, regS2A_DOORBELL_ENTRY_0_CTRL);
    before[2] = RREG32_SOC15(RN_NBIO, 0, regS2A_DOORBELL_ENTRY_3_CTRL);
    if (!(tmp & RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN__BIF_DOORBELL_APER_EN_MASK)) {
        tmp = REG_SET_FIELD(tmp, RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN, BIF_DOORBELL_APER_EN, 1);
        WREG32_SOC15(RN_NBIO, 0, regRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN, tmp);
    }
    if (writeS2a) {
        WREG32_SOC15(RN_NBIO, 0, regS2A_DOORBELL_ENTRY_0_CTRL, 0x30000007);      // nbio_v4_3_gc_doorbell_init
        WREG32_SOC15(RN_NBIO, 0, regS2A_DOORBELL_ENTRY_3_CTRL, 0x3000000d);
    }
}

// Tells the compute queue (selected by grbmSelect) that the ring now ends at wptr dwords.
static void computeSubmit(struct rn_device *adev, u64 wptrShadowOff, u32 wptr) {
    gComputeWptr = wptr;
    if (gDoorbellBase) {
        *reinterpret_cast<volatile u64 *>(gDoorbellBase + gDoorbellDword * 4) = wptr;     // the doorbell carries the wptr
        return;
    }
    rn_vram_wr32(adev, wptrShadowOff, wptr);                                 // the polled shadow as well as the register
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO, wptr);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_HI, 0);
}

// What gfx_v11_0_hw_init does between the memory hub and the CP resume and RedNavi had skipped (gfx_v11_0_init_golden_registers, gfx_v11_0_constants_init
// with gfx_v11_0_init_compute_vmid and gfx_v11_0_init_gds_vmid, gfx_v11_0_cp_set_doorbell_range, gfx_v11_0_enable_gui_idle_interrupt(false)).
// Values for GC 11.0.3 and the Navi 10 style doorbell map; DEFAULT_SH_MEM_CONFIG = ADDRESS_MODE_64 (0) | ALIGNMENT_MODE_UNALIGNED (3 << 2) | prefetch (3 << 14).
void rn_gfx_constants_init(struct rn_device *adev) {
    u32 tmp;
    // gfx_v11_0_constants_init: the register read timeout, so that a read of a block that does not answer ends instead of hanging whoever polls it
    tmp = RREG32_SOC15(RN_GC, 0, regGRBM_CNTL);
    tmp = REG_SET_FIELD(tmp, GRBM_CNTL, READ_TIMEOUT, 0xff);
    WREG32_SOC15(RN_GC, 0, regGRBM_CNTL, tmp);
    // golden_settings_gc_11_0: TCP_CNTL bit 29
    tmp = RREG32_SOC15(RN_GC, 0, regTCP_CNTL);
    tmp = (tmp & ~0x20000000u) | 0x20000000u;
    WREG32_SOC15(RN_GC, 0, regTCP_CNTL, tmp);
    tmp = RREG32_SOC15(RN_GC, 0, regDB_RING_CONTROL);
    tmp = REG_SET_FIELD(tmp, DB_RING_CONTROL, COUNTER_CONTROL, 0);           // two gfx pipes
    WREG32_SOC15(RN_GC, 0, regDB_RING_CONTROL, tmp);
    const u32 shMemConfig = (0u << 0) | (3u << 2) | (3u << 14);
    for (u32 vmid = 0; vmid < 16; vmid++) {                                  // for_each_vmid_and_zero
        grbmSelect(adev, 0, 0, vmid, 0);
        WREG32_SOC15(RN_GC, 0, regSH_MEM_CONFIG, shMemConfig);
        if (vmid != 0) {
            tmp = REG_SET_FIELD(0, SH_MEM_BASES, PRIVATE_BASE, 0x1000u);     // private_aperture_start >> 48
            tmp = REG_SET_FIELD(tmp, SH_MEM_BASES, SHARED_BASE, 0x2000u);    // shared_aperture_start >> 48
            WREG32_SOC15(RN_GC, 0, regSH_MEM_BASES, tmp);
        }
    }
    // gfx_v11_0_init_compute_vmid: the KFD VMIDs (8 to 15) with LDS at 0x1 and scratch at 0x2 and the trap on
    for (u32 vmid = 8; vmid < 16; vmid++) {
        grbmSelect(adev, 0, 0, vmid, 0);
        WREG32_SOC15(RN_GC, 0, regSH_MEM_CONFIG, shMemConfig);
        WREG32_SOC15(RN_GC, 0, regSH_MEM_BASES, (1u << 16) | 2u);
        tmp = RREG32_SOC15(RN_GC, 0, regSPI_GDBG_PER_VMID_CNTL);
        tmp = REG_SET_FIELD(tmp, SPI_GDBG_PER_VMID_CNTL, TRAP_EN, 1);
        WREG32_SOC15(RN_GC, 0, regSPI_GDBG_PER_VMID_CNTL, tmp);
    }
    grbmSelect(adev, 0, 0, 0, 0);
    for (u32 vmid = 8; vmid < 16; vmid++) {                                  // no GDS, GWS or OA for them
        rn_wreg32(adev, adev->reg_offset[RN_GC_HWIP][0][regGDS_VMID0_BASE_BASE_IDX] + regGDS_VMID0_BASE + 2 * vmid, 0);
        rn_wreg32(adev, adev->reg_offset[RN_GC_HWIP][0][regGDS_VMID0_SIZE_BASE_IDX] + regGDS_VMID0_SIZE + 2 * vmid, 0);
        rn_wreg32(adev, adev->reg_offset[RN_GC_HWIP][0][regGDS_GWS_VMID0_BASE_IDX] + regGDS_GWS_VMID0 + vmid, 0);
        rn_wreg32(adev, adev->reg_offset[RN_GC_HWIP][0][regGDS_OA_VMID0_BASE_IDX] + regGDS_OA_VMID0 + vmid, 0);
    }
    // gfx_v11_0_cp_set_doorbell_range: AMDGPU_NAVI10_DOORBELL_GFX_RING0 0x8b and GFX_USERQUEUE_END 0xff for the graphics queues, KIQ 0 to USERQUEUE_END 0x8a for compute
    WREG32_SOC15(RN_GC, 0, regCP_RB_DOORBELL_RANGE_LOWER, (0x8bu * 2) << 2);
    WREG32_SOC15(RN_GC, 0, regCP_RB_DOORBELL_RANGE_UPPER, (0xffu * 2) << 2);
    WREG32_SOC15(RN_GC, 0, regCP_MEC_DOORBELL_RANGE_LOWER, 0);
    WREG32_SOC15(RN_GC, 0, regCP_MEC_DOORBELL_RANGE_UPPER, (0x8au * 2) << 2);
    // gfx_v11_0_rlc_resume, PSP load type: the RLC save/restore machine on (gfx_v11_0_rlc_enable_srm). The clear state buffer (gfx_v11_0_init_csb) is not done.
    tmp = RREG32_SOC15(RN_GC, 0, regRLC_SRM_CNTL);
    tmp |= RLC_SRM_CNTL__AUTO_INCR_ADDR_MASK;
    tmp |= RLC_SRM_CNTL__SRM_ENABLE_MASK;
    WREG32_SOC15(RN_GC, 0, regRLC_SRM_CNTL, tmp);
    // gfx_v11_0_enable_gui_idle_interrupt(false) on the two graphics pipes
    for (int ring = 0; ring < 2; ring++) {
        tmp = ring == 0 ? RREG32_SOC15(RN_GC, 0, regCP_INT_CNTL_RING0) : RREG32_SOC15(RN_GC, 0, regCP_INT_CNTL_RING1);
        tmp = ring == 0 ? REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CNTX_BUSY_INT_ENABLE, 0) : REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CNTX_BUSY_INT_ENABLE, 0);
        tmp = ring == 0 ? REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CNTX_EMPTY_INT_ENABLE, 0) : REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CNTX_EMPTY_INT_ENABLE, 0);
        tmp = ring == 0 ? REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CMP_BUSY_INT_ENABLE, 0) : REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CMP_BUSY_INT_ENABLE, 0);
        tmp = ring == 0 ? REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, GFX_IDLE_INT_ENABLE, 0) : REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, GFX_IDLE_INT_ENABLE, 0);
        if (ring == 0) WREG32_SOC15(RN_GC, 0, regCP_INT_CNTL_RING0, tmp); else WREG32_SOC15(RN_GC, 0, regCP_INT_CNTL_RING1, tmp);
    }
    // gfx_v11_0_cp_resume with amdgpu_async_gfx_ring: gfx_v11_0_cp_compute_enable(true) then gfx_v11_0_cp_gfx_enable(true), before the MES KIQ init.
    rn_mec_enable(adev);
    tmp = RREG32_SOC15(RN_GC, 0, regCP_ME_CNTL);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_HALT, 0);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_HALT, 0);
    WREG32_SOC15(RN_GC, 0, regCP_ME_CNTL, tmp);
    udelay(50);
}

// gfx_v11_0_cp_compute_enable(true): the four MEC pipes out of reset and active, ucode cache valid, MEC running.
void rn_mec_enable(struct rn_device *adev) {
    u32 tmp;
    tmp = RREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_CNTL);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_INVALIDATE_ICACHE, 0);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE0_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE1_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE2_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE3_RESET, 0);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE0_ACTIVE, 1);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE1_ACTIVE, 1);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE2_ACTIVE, 1);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE3_ACTIVE, 1);
    tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_HALT, 0);
    WREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_CNTL, tmp);
    udelay(50);
}

void rn_compute_ring_test(struct rn_device *adev, u64 vaBase, u64 ringOff, u32 ringBytes, struct rn_compute_result *res) {
    memset(res, 0, sizeof(*res));
    const u64 ringVa = vaBase + ringOff;
    const u64 rptrOff = ringOff + 0x1000, wptrOff = ringOff + 0x1008, mqdOff = ringOff + 0x2000, eopOff = ringOff + 0x4000;
    u32 tmp;

    rn_mec_enable(adev);
    res->step = 1;

    // the packet: SET_UCONFIG_REG SCRATCH_REG0 = 0xDEADBEEF (a compute ring is tested with it too), padded to 8 dwords
    const u32 scratchDw = 0xc000u + 0x40u;
    const u32 pkt[8] = {RN_PACKET3(RN_PACKET3_SET_UCONFIG_REG, 1), scratchDw - RN_PACKET3_SET_UCONFIG_REG_START, 0xDEADBEEFu,
                        RN_PACKET3(RN_PACKET3_NOP, 3), 0, 0, 0, 0};
    for (u32 o = 0; o < ringBytes; o += 4) rn_vram_wr32(adev, ringOff + o, 0);
    for (u32 i = 0; i < 8; i++) rn_vram_wr32(adev, ringOff + i * 4, pkt[i]);
    for (u32 o = 0; o < 0x1000; o += 4) rn_vram_wr32(adev, mqdOff + o, 0);
    for (u32 o = 0; o < 2048; o += 4) rn_vram_wr32(adev, eopOff + o, 0);
    rn_vram_wr32(adev, rptrOff, 0); rn_vram_wr32(adev, rptrOff + 4, 0);
    rn_vram_wr32(adev, wptrOff, 0); rn_vram_wr32(adev, wptrOff + 4, 0);
    WREG32_SOC15(RN_GC, 0, regSCRATCH_REG0, 0xBADC0DE0u);

    // gfx_v11_0_compute_mqd_init values, written like gfx_v11_0_kiq_init_register
    grbmSelect(adev, 1, 0, 0, 2);
    tmp = RREG32_SOC15(RN_GC, 0, regCP_PQ_WPTR_POLL_CNTL);                    // wptr polling on unless there is a doorbell
    tmp = REG_SET_FIELD(tmp, CP_PQ_WPTR_POLL_CNTL, EN, gDoorbellBase ? 0 : 1);
    WREG32_SOC15(RN_GC, 0, regCP_PQ_WPTR_POLL_CNTL, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE, 0);
    const u64 eopBase = (vaBase + eopOff) >> 8;
    WREG32_SOC15(RN_GC, 0, regCP_HQD_EOP_BASE_ADDR, lower_32_bits(eopBase));
    WREG32_SOC15(RN_GC, 0, regCP_HQD_EOP_BASE_ADDR_HI, upper_32_bits(eopBase));
    tmp = regCP_HQD_EOP_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_EOP_CONTROL, EOP_SIZE, 8);               // order_base_2(GFX11_MEC_HPD_SIZE 2048 / 4) - 1
    WREG32_SOC15(RN_GC, 0, regCP_HQD_EOP_CONTROL, tmp);
    tmp = regCP_HQD_PQ_DOORBELL_CONTROL_DEFAULT;                             // gfx_v11_0_compute_mqd_init
    if (gDoorbellBase) {
        tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_OFFSET, gDoorbellDword);
        tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 1);
        tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_SOURCE, 0);
        tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_HIT, 0);
        WREG32_SOC15(RN_GC, 0, regCP_MEC_DOORBELL_RANGE_LOWER, 0);          // kiq << 2 * 2 bytes = 0
        WREG32_SOC15(RN_GC, 0, regCP_MEC_DOORBELL_RANGE_UPPER, 0x8A * 2 * 4); // userqueue_end * 2 << 2
    }
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_MQD_BASE_ADDR, lower_32_bits(vaBase + mqdOff) & 0xfffffffcu);
    WREG32_SOC15(RN_GC, 0, regCP_MQD_BASE_ADDR_HI, upper_32_bits(vaBase + mqdOff));
    tmp = regCP_MQD_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_MQD_CONTROL, VMID, 0);
    WREG32_SOC15(RN_GC, 0, regCP_MQD_CONTROL, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_BASE, lower_32_bits(ringVa >> 8));
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_BASE_HI, upper_32_bits(ringVa >> 8));
    u32 qOrder = 0;
    while ((4u << qOrder) < ringBytes) qOrder++;                             // order_base_2(queue_size / 4)
    tmp = regCP_HQD_PQ_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, QUEUE_SIZE, qOrder - 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE, 9);         // order_base_2(4096 / 4) - 1
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, UNORD_DISPATCH, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, 1);   // Linux's compute MQD reads back with it set (allow_tunneling), docs/COMPAT.md
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, PRIV_STATE, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, KMD_QUEUE, 1);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_CONTROL, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR_REPORT_ADDR, lower_32_bits(vaBase + rptrOff) & 0xfffffffcu);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR_REPORT_ADDR_HI, upper_32_bits(vaBase + rptrOff) & 0xFFFFu);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_POLL_ADDR, lower_32_bits(vaBase + wptrOff) & 0xfffffffcu);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_POLL_ADDR_HI, upper_32_bits(vaBase + wptrOff) & 0xFFFFu);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR, 0);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO, 0);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_HI, 0);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_VMID, 0);
    tmp = regCP_HQD_PERSISTENT_STATE_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PERSISTENT_STATE, PRELOAD_SIZE, 0x55);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PERSISTENT_STATE, tmp);
    // The memory image of the queue (v11_compute_mqd, 512 dwords): the firmware of the MEC saves the queue state here and restores it when
    // the queue is switched back in, so with an empty page the queue came back empty after some idle time. These are the values of
    // gfx_v11_0_compute_mqd_init, the same that went into the registers above; offsets are those of struct v11_compute_mqd.
    {
        u32 doorbellCtl = regCP_HQD_PQ_DOORBELL_CONTROL_DEFAULT;
        if (gDoorbellBase) {
            doorbellCtl = REG_SET_FIELD(doorbellCtl, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_OFFSET, gDoorbellDword);
            doorbellCtl = REG_SET_FIELD(doorbellCtl, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 1);
        }
        u32 eopCtl = regCP_HQD_EOP_CONTROL_DEFAULT;
        eopCtl = REG_SET_FIELD(eopCtl, CP_HQD_EOP_CONTROL, EOP_SIZE, 8);
        u32 mqdCtl = regCP_MQD_CONTROL_DEFAULT;
        mqdCtl = REG_SET_FIELD(mqdCtl, CP_MQD_CONTROL, VMID, 0);
        u32 pqCtl = regCP_HQD_PQ_CONTROL_DEFAULT;
        pqCtl = REG_SET_FIELD(pqCtl, CP_HQD_PQ_CONTROL, QUEUE_SIZE, qOrder - 1);
        pqCtl = REG_SET_FIELD(pqCtl, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE, 9);
        pqCtl = REG_SET_FIELD(pqCtl, CP_HQD_PQ_CONTROL, UNORD_DISPATCH, 1);
        pqCtl = REG_SET_FIELD(pqCtl, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, 1);   // Linux's compute MQD reads back with it set (allow_tunneling), docs/COMPAT.md
        pqCtl = REG_SET_FIELD(pqCtl, CP_HQD_PQ_CONTROL, PRIV_STATE, 1);
        pqCtl = REG_SET_FIELD(pqCtl, CP_HQD_PQ_CONTROL, KMD_QUEUE, 1);
        u32 persist = regCP_HQD_PERSISTENT_STATE_DEFAULT;
        persist = REG_SET_FIELD(persist, CP_HQD_PERSISTENT_STATE, PRELOAD_SIZE, 0x55);
        u32 ibCtl = regCP_HQD_IB_CONTROL_DEFAULT;
        ibCtl = REG_SET_FIELD(ibCtl, CP_HQD_IB_CONTROL, MIN_IB_AVAIL_SIZE, 3);
        u32 quantum = 0;
        quantum = REG_SET_FIELD(quantum, CP_HQD_QUANTUM, QUANTUM_EN, 1);
        quantum = REG_SET_FIELD(quantum, CP_HQD_QUANTUM, QUANTUM_SCALE, 1);
        quantum = REG_SET_FIELD(quantum, CP_HQD_QUANTUM, QUANTUM_DURATION, 1);
        struct { u32 dw; u32 v; } mqd[] = {
            {0, 0xC0310800u}, {11, 1}, {23, 0xffffffffu}, {24, 0xffffffffu}, {26, 0xffffffffu}, {27, 0xffffffffu}, {32, 7},
            {165, lower_32_bits(eopBase)}, {166, upper_32_bits(eopBase)}, {167, eopCtl}, {143, doorbellCtl},
            {128, lower_32_bits(vaBase + mqdOff) & 0xfffffffcu}, {129, upper_32_bits(vaBase + mqdOff)}, {162, mqdCtl},
            {136, lower_32_bits(ringVa >> 8)}, {137, upper_32_bits(ringVa >> 8)}, {145, pqCtl},
            {139, lower_32_bits(vaBase + rptrOff) & 0xfffffffcu}, {140, upper_32_bits(vaBase + rptrOff) & 0xFFFFu},
            {141, lower_32_bits(vaBase + wptrOff) & 0xfffffffcu}, {142, upper_32_bits(vaBase + wptrOff) & 0xFFFFu},
            {132, persist}, {149, ibCtl}, {135, quantum}, {130, 1},
        };
        for (u32 i = 0; i < sizeof(mqd) / sizeof(mqd[0]); i++) rn_vram_wr32(adev, mqdOff + mqd[i].dw * 4, mqd[i].v);
    }
    WREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE, 1);
    if (gDoorbellBase) {                                                     // CP_PQ_STATUS.DOORBELL_ENABLE after the queue is active
        tmp = RREG32_SOC15(RN_GC, 0, regCP_PQ_STATUS);
        tmp = REG_SET_FIELD(tmp, CP_PQ_STATUS, DOORBELL_ENABLE, 1);
        WREG32_SOC15(RN_GC, 0, regCP_PQ_STATUS, tmp);
    }
    res->active_rd = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
    res->pq_control_rd = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_CONTROL);
    res->mqd_base_rd = RREG32_SOC15(RN_GC, 0, regCP_MQD_BASE_ADDR);
    res->step = 2;

    computeSubmit(adev, wptrOff, 8);                                         // 8 dwords
    res->step = 3;
    static const u32 sampleDelayMs[8] = {0, 1, 1, 3, 5, 10, 30, 50};        // cumulative: 0, 1, 2, 5, 10, 20, 50, 100 ms
    for (u32 i = 0; i < 8; i++) {
        if (sampleDelayMs[i]) msleep(sampleDelayMs[i]);
        res->trace[i][0] = RREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_CNTL);
        res->trace[i][1] = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
        res->trace[i][2] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
    }
    for (int i = 0; i < 2000; i++) {                                         // up to 2 s
        res->scratch = RREG32_SOC15(RN_GC, 0, regSCRATCH_REG0);
        if (res->scratch == 0xDEADBEEFu) { res->ok = 1; break; }
        msleep(1);
    }
    res->rptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
    res->wptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO);
    res->fault_status = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_STATUS);
    res->fault_addr_lo = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_LO32);
    res->fault_addr_hi = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_HI32);
    res->rptr_shadow = rn_vram_rd32(adev, rptrOff);
    res->ring0 = rn_vram_rd32(adev, ringOff);
    grbmSelect(adev, 0, 0, 0, 0);
}

#define RN_PACKET3_SET_SH_REG 0x76u
#define RN_PACKET3_SET_SH_REG_START 0x00002c00u
#define RN_PACKET3_DISPATCH_DIRECT 0x15u
#define RN_PACKET3_RELEASE_MEM 0x49u
#define RN_CACHE_FLUSH_AND_INV_TS_EVENT 0x14u

// Compute state registers (gc_11_0_x_offset.h, all in GC segment 0): the SET_SH_REG offset is the register's absolute dword minus 0x2c00.
#define RN_COMPUTE_START_X 0x1ba4u
#define RN_COMPUTE_NUM_THREAD_X 0x1ba7u
#define RN_COMPUTE_PGM_LO 0x1bacu
#define RN_COMPUTE_PGM_RSRC1 0x1bb2u
#define RN_COMPUTE_RESOURCE_LIMITS 0x1bb5u
#define RN_COMPUTE_PGM_RSRC3 0x1bc8u

void rn_compute_dispatch_test(struct rn_device *adev, u64 vaBase, u64 ringOff, u64 shaderOff, u64 fenceOff, u64 targetOff, u64 targetVaOverride,
                              volatile u32 *hostTarget, u32 intSel, int (*waitDone)(void *, u32), void *waitCtx, struct rn_dispatch_result *res) {
    memset(res, 0, sizeof(*res));
    const u64 shaderVa = vaBase + shaderOff, fenceVa = vaBase + fenceOff;
    const u64 wptrOff = ringOff + 0x1008;
    const u32 gcBase = adev->reg_offset[RN_GC_HWIP][0][0];
    #define SHREG(reg) (gcBase + (reg) - RN_PACKET3_SET_SH_REG_START)

    // the shader: s_endpgm (GFX11 SOPP opcode 48) alone, or a store of one dword followed by s_endpgm; s_code_end padding up to 256 bytes
    u32 code[16];
    u32 nc = 0;
    if (targetOff || hostTarget) {
        const u64 targetVa = hostTarget ? targetVaOverride : vaBase + targetOff;
        code[nc++] = 0xBE8000FFu; code[nc++] = lower_32_bits(targetVa);     // s_mov_b32 s0, <address low>
        code[nc++] = 0xBE8100FFu; code[nc++] = upper_32_bits(targetVa);     // s_mov_b32 s1, <address high>
        code[nc++] = 0x7E020280u;                                           // v_mov_b32 v1, 0           (offset)
        code[nc++] = 0x7E0402FFu; code[nc++] = 0x600DC0DEu;                 // v_mov_b32 v2, 0x600DC0DE  (data)
        code[nc++] = 0xDC6A0000u; code[nc++] = 0x00000201u;                 // global_store_b32 v1, v2, s[0:1]
        code[nc++] = 0xBC7C0000u;                                           // s_waitcnt_vscnt null, 0x0
        if (hostTarget) { *hostTarget = 0xDEAD0000u; res->target_before = *hostTarget; }
        else { rn_vram_wr32(adev, targetOff, 0xDEAD0000u); res->target_before = rn_vram_rd32(adev, targetOff); }
    }
    code[nc++] = 0xBFB00000u;                                               // s_endpgm
    for (u32 i = 0; i < nc; i++) rn_vram_wr32(adev, shaderOff + i * 4, code[i]);
    for (u32 o = nc * 4; o < 256; o += 4) rn_vram_wr32(adev, shaderOff + o, 0xBF9F0000u);
    rn_vram_wr32(adev, fenceOff, 0x0BADF00Du);
    res->fence_before = rn_vram_rd32(adev, fenceOff);

    u32 ring[64];
    u32 n = 0;
    ring[n++] = RN_PACKET3(RN_PACKET3_SET_SH_REG, 2); ring[n++] = SHREG(RN_COMPUTE_PGM_LO);
    ring[n++] = lower_32_bits(shaderVa >> 8); ring[n++] = upper_32_bits(shaderVa >> 8);
    ring[n++] = RN_PACKET3(RN_PACKET3_SET_SH_REG, 2); ring[n++] = SHREG(RN_COMPUTE_PGM_RSRC1);
    ring[n++] = 0x002C0000u;                                                 // float mode 0xc0, DX10_CLAMP
    ring[n++] = 0x00000080u;                                                 // TGID_X_EN
    ring[n++] = RN_PACKET3(RN_PACKET3_SET_SH_REG, 1); ring[n++] = SHREG(RN_COMPUTE_PGM_RSRC3); ring[n++] = 0;
    ring[n++] = RN_PACKET3(RN_PACKET3_SET_SH_REG, 3); ring[n++] = SHREG(RN_COMPUTE_START_X);
    ring[n++] = 0; ring[n++] = 0; ring[n++] = 0;
    ring[n++] = RN_PACKET3(RN_PACKET3_SET_SH_REG, 3); ring[n++] = SHREG(RN_COMPUTE_NUM_THREAD_X);
    ring[n++] = 1; ring[n++] = 1; ring[n++] = 1;
    ring[n++] = RN_PACKET3(RN_PACKET3_SET_SH_REG, 6); ring[n++] = SHREG(RN_COMPUTE_RESOURCE_LIMITS);
    ring[n++] = 0;                                                           // RESOURCE_LIMITS
    ring[n++] = 0xFFFFFFFFu;                                                 // STATIC_THREAD_MGMT_SE0
    ring[n++] = 0xFFFFFFFFu;                                                 // STATIC_THREAD_MGMT_SE1
    ring[n++] = 0;                                                           // TMPRING_SIZE
    ring[n++] = 0xFFFFFFFFu;                                                 // STATIC_THREAD_MGMT_SE2
    ring[n++] = 0xFFFFFFFFu;                                                 // STATIC_THREAD_MGMT_SE3
    ring[n++] = RN_PACKET3(RN_PACKET3_DISPATCH_DIRECT, 3);
    ring[n++] = 1; ring[n++] = 1; ring[n++] = 1;
    ring[n++] = 0x00000001u | 0x00000004u | 0x00000040u | 0x00008000u;      // COMPUTE_SHADER_EN | FORCE_START_AT_000 | ORDER_MODE | CS_W32_EN
    // RELEASE_MEM as gfx_v11_0_ring_emit_fence: flush, then write a 32-bit value to the fence address
    ring[n++] = RN_PACKET3(RN_PACKET3_RELEASE_MEM, 6);
    ring[n++] = (1u << 22) | (1u << 21) | (1u << 13) | (1u << 12) | (3u << 25) | RN_CACHE_FLUSH_AND_INV_TS_EVENT | (5u << 8);
    ring[n++] = (1u << 29) | (intSel << 24);                                 // DATA_SEL 1: 32-bit data; INT_SEL 2: interrupt once the write is confirmed
    ring[n++] = lower_32_bits(fenceVa); ring[n++] = upper_32_bits(fenceVa);
    ring[n++] = 0xF00DF00Du; ring[n++] = 0; ring[n++] = 0;
    while (((8 + n) & 7) != 0) { ring[n++] = RN_PACKET3(RN_PACKET3_NOP, 0); ring[n++] = 0; }   // the ring already holds 8 dwords
    res->step = 1;

    const u32 start = 8;                                                     // after the SCRATCH_REG0 packet of the first test
    for (u32 i = 0; i < n; i++) rn_vram_wr32(adev, ringOff + (start + i) * 4, ring[i]);
    grbmSelect(adev, 1, 0, 0, 2);
    computeSubmit(adev, wptrOff, start + n);
    res->step = 2;
    if (waitDone) {                                                          // no fence polling: the caller waits for the interrupt
        waitDone(waitCtx, 1500);
        res->fence = rn_vram_rd32(adev, fenceOff);
        res->ok = res->fence == 0xF00DF00Du;
    } else {
        static const u32 sampleDelayMs[6] = {0, 1, 4, 15, 80, 400};          // cumulative: 0, 1, 5, 20, 100, 500 ms
        for (u32 i = 0; i < 6; i++) {
            if (sampleDelayMs[i]) msleep(sampleDelayMs[i]);
            res->trace[i][0] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
            res->trace[i][1] = rn_vram_rd32(adev, fenceOff);
        }
        for (int i = 0; i < 1500; i++) {                                     // up to 1.5 s more
            res->fence = rn_vram_rd32(adev, fenceOff);
            if (res->fence == 0xF00DF00Du) { res->ok = 1; break; }
            msleep(1);
        }
    }
    res->rptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
    res->wptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO);
    res->active = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
    res->scratch = RREG32_SOC15(RN_GC, 0, regSCRATCH_REG0);
    if (hostTarget) res->target = *hostTarget;
    else if (targetOff) res->target = rn_vram_rd32(adev, targetOff);
    res->fault_status = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_STATUS);
    res->fault_addr_lo = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_LO32);
    res->fault_addr_hi = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_HI32);
    grbmSelect(adev, 0, 0, 0, 0);
    #undef SHREG
}

void rn_ih_enable(struct rn_device *adev, u64 vaBase, u64 ringOff, u32 ringBytes, u64 wptrShadowOff, int msi) {
    u32 tmp;
    for (u32 o = 0; o < ringBytes; o += 4) rn_vram_wr32(adev, ringOff + o, 0);
    rn_vram_wr32(adev, wptrShadowOff, 0); rn_vram_wr32(adev, wptrShadowOff + 4, 0);

    // ih_v6_0_toggle_interrupts(false): ring and interrupts off, pointers to 0
    tmp = RREG32_SOC15(RN_OSSSYS, 0, regIH_RB_CNTL);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, RB_ENABLE, 0);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, ENABLE_INTR, 0);
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_CNTL, tmp);
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_RPTR, 0);
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_WPTR, 0);

    // nbio_v4_3_ih_control: no dummy read (there is no MSI and no system dummy page here), and the ring is in VRAM, so non-snooped
    WREG32_SOC15(RN_NBIO, 0, regBIF_BX0_INTERRUPT_CNTL2, 0);
    tmp = RREG32_SOC15(RN_NBIO, 0, regBIF_BX0_INTERRUPT_CNTL);
    tmp = REG_SET_FIELD(tmp, BIF_BX0_INTERRUPT_CNTL, IH_DUMMY_RD_OVERRIDE, 1);
    tmp = REG_SET_FIELD(tmp, BIF_BX0_INTERRUPT_CNTL, IH_DUMMY_RD_EN, 0);
    tmp = REG_SET_FIELD(tmp, BIF_BX0_INTERRUPT_CNTL, IH_REQ_NONSNOOP_EN, 1);
    WREG32_SOC15(RN_NBIO, 0, regBIF_BX0_INTERRUPT_CNTL, tmp);

    // ih_v6_0_enable_ring
    const u64 ringVa = vaBase + ringOff;
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_BASE, lower_32_bits(ringVa >> 8));
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_BASE_HI, upper_32_bits(ringVa >> 8) & 0xff);
    u32 order = 0;
    while ((4u << order) < ringBytes) order++;                               // order_base_2(ring_size / 4)
    tmp = RREG32_SOC15(RN_OSSSYS, 0, regIH_RB_CNTL);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, MC_SPACE, 4);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_ENABLE, 1);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, RB_SIZE, order);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_WRITEBACK_ENABLE, 1);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, MC_SNOOP, 0);                       // VRAM
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, MC_RO, 0);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, MC_VMID, 0);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, RPTR_REARM, msi ? 1 : 0);
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_CNTL, tmp);
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_WPTR_ADDR_LO, lower_32_bits(vaBase + wptrShadowOff));
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_WPTR_ADDR_HI, upper_32_bits(vaBase + wptrShadowOff) & 0xFFFFu);
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_WPTR, 0);
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_RPTR, 0);
    WREG32_SOC15(RN_OSSSYS, 0, regIH_DOORBELL_RPTR, 0);                      // no doorbell for the read pointer

    // ih_v6_0_toggle_interrupts(true): clear the overflow bit by pulsing it, then enable the ring and the interrupts
    tmp = RREG32_SOC15(RN_OSSSYS, 0, regIH_RB_CNTL);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, RB_ENABLE, 1);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 0);
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_CNTL, tmp);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1);
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_CNTL, tmp);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 0);
    tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, ENABLE_INTR, 1);
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_CNTL, tmp);

    // gfx_v11_0_set_compute_eop_interrupt_state: the end-of-pipe timestamp interrupt of MEC 1 pipe 0 (the queue under test)
    tmp = RREG32_SOC15(RN_GC, 0, regCP_ME1_PIPE0_INT_CNTL);
    tmp = REG_SET_FIELD(tmp, CP_ME1_PIPE0_INT_CNTL, TIME_STAMP_INT_ENABLE, 1);
    WREG32_SOC15(RN_GC, 0, regCP_ME1_PIPE0_INT_CNTL, tmp);
}

u32 rn_ih_ack(struct rn_device *adev, u32 ringBytes) {
    const u32 wptr = RREG32_SOC15(RN_OSSSYS, 0, regIH_RB_WPTR);
    WREG32_SOC15(RN_OSSSYS, 0, regIH_RB_RPTR, wptr & (ringBytes - 1));
    return wptr;
}

void rn_ih_read(struct rn_device *adev, u64 ringOff, u64 wptrShadowOff, struct rn_ih_result *res) {
    memset(res, 0, sizeof(*res));
    res->rb_cntl_rd = RREG32_SOC15(RN_OSSSYS, 0, regIH_RB_CNTL);
    res->wptr_reg = RREG32_SOC15(RN_OSSSYS, 0, regIH_RB_WPTR);
    res->rptr_reg = RREG32_SOC15(RN_OSSSYS, 0, regIH_RB_RPTR);
    res->base_rd = RREG32_SOC15(RN_OSSSYS, 0, regIH_RB_BASE);
    res->cp_int_rd = RREG32_SOC15(RN_GC, 0, regCP_ME1_PIPE0_INT_CNTL);
    res->wptr_shadow = rn_vram_rd32(adev, wptrShadowOff);
    for (u32 i = 0; i < 16; i++) res->entry[i] = rn_vram_rd32(adev, ringOff + i * 4);
}

void rn_sdma0_run(struct rn_device *adev, u64 vaBase, u64 ringOff, u32 ringBytes, const u32 *pkt, u32 dwords, struct rn_sdma_run_result *res) {
    memset(res, 0, sizeof(*res));
    const u64 ringMc = vaBase + ringOff;
    const u64 rptrOff = ringOff + 0x1000, wptrOff = ringOff + 0x1008;
    u32 tmp;

    tmp = SDR(regSDMA0_QUEUE0_RB_CNTL);                                     // stop the queue before it is reprogrammed
    tmp = REG_SET_FIELD(tmp, SDMA0_QUEUE0_RB_CNTL, RB_ENABLE, 0);
    SDW(regSDMA0_QUEUE0_RB_CNTL, tmp);
    for (u32 o = 0; o < ringBytes; o += 4) rn_vram_wr32(adev, ringOff + o, 0);
    for (u32 i = 0; i < dwords; i++) rn_vram_wr32(adev, ringOff + i * 4, pkt[i]);
    rn_vram_wr32(adev, rptrOff, 0); rn_vram_wr32(adev, rptrOff + 4, 0);
    rn_vram_wr32(adev, wptrOff, 0); rn_vram_wr32(adev, wptrOff + 4, 0);

    SDW(regSDMA0_SEM_WAIT_FAIL_TIMER_CNTL, 0);
    u32 rbBufsz = 0;
    while ((4u << rbBufsz) < ringBytes) rbBufsz++;
    u32 rbCntl = SDR(regSDMA0_QUEUE0_RB_CNTL);
    rbCntl = REG_SET_FIELD(rbCntl, SDMA0_QUEUE0_RB_CNTL, RB_SIZE, rbBufsz);
    rbCntl = REG_SET_FIELD(rbCntl, SDMA0_QUEUE0_RB_CNTL, RB_PRIV, 1);
    SDW(regSDMA0_QUEUE0_RB_CNTL, rbCntl);
    SDW(regSDMA0_QUEUE0_RB_RPTR, 0);
    SDW(regSDMA0_QUEUE0_RB_RPTR_HI, 0);
    SDW(regSDMA0_QUEUE0_RB_WPTR, 0);
    SDW(regSDMA0_QUEUE0_RB_WPTR_HI, 0);
    SDW(regSDMA0_QUEUE0_RB_WPTR_POLL_ADDR_LO, lower_32_bits(vaBase + wptrOff));
    SDW(regSDMA0_QUEUE0_RB_WPTR_POLL_ADDR_HI, upper_32_bits(vaBase + wptrOff));
    SDW(regSDMA0_QUEUE0_RB_RPTR_ADDR_HI, upper_32_bits(vaBase + rptrOff));
    SDW(regSDMA0_QUEUE0_RB_RPTR_ADDR_LO, lower_32_bits(vaBase + rptrOff) & 0xFFFFFFFCu);
    rbCntl = REG_SET_FIELD(rbCntl, SDMA0_QUEUE0_RB_CNTL, RPTR_WRITEBACK_ENABLE, 1);
    rbCntl = REG_SET_FIELD(rbCntl, SDMA0_QUEUE0_RB_CNTL, WPTR_POLL_ENABLE, 0);
    rbCntl = REG_SET_FIELD(rbCntl, SDMA0_QUEUE0_RB_CNTL, F32_WPTR_POLL_ENABLE, 1);
    SDW(regSDMA0_QUEUE0_RB_BASE, (u32)(ringMc >> 8));
    SDW(regSDMA0_QUEUE0_RB_BASE_HI, (u32)(ringMc >> 40));
    SDW(regSDMA0_QUEUE0_MINOR_PTR_UPDATE, 1);
    SDW(regSDMA0_QUEUE0_RB_WPTR, 0);
    SDW(regSDMA0_QUEUE0_RB_WPTR_HI, 0);
    tmp = SDR(regSDMA0_QUEUE0_DOORBELL);
    tmp = REG_SET_FIELD(tmp, SDMA0_QUEUE0_DOORBELL, ENABLE, 0);
    SDW(regSDMA0_QUEUE0_DOORBELL, tmp);
    SDW(regSDMA0_QUEUE0_MINOR_PTR_UPDATE, 0);
    tmp = SDR(regSDMA0_F32_CNTL);
    tmp = REG_SET_FIELD(tmp, SDMA0_F32_CNTL, HALT, 0);
    tmp = REG_SET_FIELD(tmp, SDMA0_F32_CNTL, TH1_RESET, 0);
    SDW(regSDMA0_F32_CNTL, tmp);
    rbCntl = REG_SET_FIELD(rbCntl, SDMA0_QUEUE0_RB_CNTL, RB_ENABLE, 1);
    SDW(regSDMA0_QUEUE0_RB_CNTL, rbCntl);

    rn_vram_wr32(adev, wptrOff, dwords << 2);
    SDW(regSDMA0_QUEUE0_RB_WPTR, dwords << 2);
    SDW(regSDMA0_QUEUE0_RB_WPTR_HI, 0);
    for (int i = 0; i < 2000; i++) {
        res->rptr = SDR(regSDMA0_QUEUE0_RB_RPTR);
        if (res->rptr == (dwords << 2)) { res->ok = 1; break; }
        msleep(1);
    }
    res->wptr = SDR(regSDMA0_QUEUE0_RB_WPTR);
    res->rptr_shadow = rn_vram_rd32(adev, rptrOff);
    res->fault_status = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_STATUS);
    res->fault_addr_lo = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_LO32);
    res->fault_addr_hi = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_HI32);
}

u32 rn_build_dispatch_ib(struct rn_device *adev, u32 *ring, u64 shaderVa) {
    const u32 gcBase = adev->reg_offset[RN_GC_HWIP][0][0];
    #define SHREG(reg) (gcBase + (reg) - RN_PACKET3_SET_SH_REG_START)
    u32 n = 0;
    ring[n++] = RN_PACKET3(RN_PACKET3_SET_SH_REG, 2); ring[n++] = SHREG(RN_COMPUTE_PGM_LO);
    ring[n++] = lower_32_bits(shaderVa >> 8); ring[n++] = upper_32_bits(shaderVa >> 8);
    ring[n++] = RN_PACKET3(RN_PACKET3_SET_SH_REG, 2); ring[n++] = SHREG(RN_COMPUTE_PGM_RSRC1);
    ring[n++] = 0x002C0000u; ring[n++] = 0x00000080u;
    ring[n++] = RN_PACKET3(RN_PACKET3_SET_SH_REG, 1); ring[n++] = SHREG(RN_COMPUTE_PGM_RSRC3); ring[n++] = 0;
    ring[n++] = RN_PACKET3(RN_PACKET3_SET_SH_REG, 3); ring[n++] = SHREG(RN_COMPUTE_START_X);
    ring[n++] = 0; ring[n++] = 0; ring[n++] = 0;
    ring[n++] = RN_PACKET3(RN_PACKET3_SET_SH_REG, 3); ring[n++] = SHREG(RN_COMPUTE_NUM_THREAD_X);
    ring[n++] = 1; ring[n++] = 1; ring[n++] = 1;
    ring[n++] = RN_PACKET3(RN_PACKET3_SET_SH_REG, 6); ring[n++] = SHREG(RN_COMPUTE_RESOURCE_LIMITS);
    ring[n++] = 0; ring[n++] = 0xFFFFFFFFu; ring[n++] = 0xFFFFFFFFu; ring[n++] = 0; ring[n++] = 0xFFFFFFFFu; ring[n++] = 0xFFFFFFFFu;
    ring[n++] = RN_PACKET3(RN_PACKET3_DISPATCH_DIRECT, 3);
    ring[n++] = 1; ring[n++] = 1; ring[n++] = 1;
    ring[n++] = 0x00000001u | 0x00000004u | 0x00000040u | 0x00008000u;
    #undef SHREG
    return n;
}

int rn_compute_lock_init(void) {
    if (!gSubmitLock) gSubmitLock = IOLockAlloc();
    if (!gFlushLock) gFlushLock = IOLockAlloc();
    return gSubmitLock && gFlushLock ? 0 : -ENOMEM;
}

void rn_compute_lock_free(void) {
    if (gSubmitLock) { IOLockFree(gSubmitLock); gSubmitLock = nullptr; }
    if (gFlushLock) { IOLockFree(gFlushLock); gFlushLock = nullptr; }
}

static int computeSubmitLocked(struct rn_device *adev, u64 vaBase, u64 ringOff, u32 ringBytes, u64 ibVa, u32 ibDwords, u64 fenceVa, u32 fenceValue, u32 vmid) {
    (void)vaBase;
    const u32 mask = ringBytes / 4 - 1;
    const u64 wptrOff = ringOff + 0x1008;
    u32 pk[24];
    u32 n = 0;
    // gfx_v11_0_ring_emit_ib_compute: INDIRECT_BUFFER, address (dword aligned), upper 16 bits, size | VALID | VMID << 24
    pk[n++] = RN_PACKET3(0x3Fu, 2);
    pk[n++] = lower_32_bits(ibVa) & 0xfffffffcu;
    pk[n++] = upper_32_bits(ibVa) & 0xFFFFu;
    pk[n++] = (1u << 23) | (ibDwords & 0xFFFFFu) | ((vmid & 0xFu) << 24);
    pk[n++] = RN_PACKET3(RN_PACKET3_RELEASE_MEM, 6);
    pk[n++] = (1u << 22) | (1u << 21) | (1u << 13) | (1u << 12) | (3u << 25) | RN_CACHE_FLUSH_AND_INV_TS_EVENT | (5u << 8);
    pk[n++] = (1u << 29);
    pk[n++] = lower_32_bits(fenceVa); pk[n++] = upper_32_bits(fenceVa);
    pk[n++] = fenceValue; pk[n++] = 0; pk[n++] = 0;
    // Pad to a multiple of 8 dwords with one NOP packet. (The earlier loop added two dwords at a time to an odd count, never reached a
    // multiple of 8, and ran off the end of pk[]: the kernel stack overflow of 2026-10-30.)
    u32 pad = (8u - (n & 7u)) & 7u;
    if (pad == 1) pad = 9;
    if (pad) {
        pk[n++] = RN_PACKET3(RN_PACKET3_NOP, pad - 2);
        for (u32 i = 1; i < pad; i++) pk[n++] = 0;
    }
    if (n + gComputeWptr - gComputeWptr > mask) return -1;
    if (gMapped) {                                                           // the queue mapped through the KIQ: ring and wptr are its own
        const u32 w = gMappedWptr;
        const u32 mmask = gMappedRingBytes ? gMappedRingBytes / 4 - 1 : mask;
        const u64 data = ringOff + gMappedRingAt;
        gMappedRingOff = ringOff;
        // Room in the ring. The rptr write-back word holds the read position modulo the ring size (measured 2026-11-01: it wraps while the
        // wptr counts on), so "used" is (w - rptr) & mmask. The ring is never filled completely: a full ring would look empty.
        if (n > mmask) return -EINVAL;
        u32 spins = 0, rptr = rn_vram_rd32(adev, ringOff + 0x1000);
        while (n > mmask - ((w - rptr) & mmask)) {
            if (spins++ == 0) gRingFullWaits++;
            if (spins > 200000) return -EAGAIN;                                 // about 2 s without the read position moving: the queue is not draining; nothing was written
            udelay(10);
            const u32 now = rn_vram_rd32(adev, ringOff + 0x1000);
            if (now != rptr) { rptr = now; spins = 1; }                         // the queue is working through a long backlog (a stress test: 2.3 s per 32 submits): not stuck
        }
        for (u32 i = 0; i < n; i++) rn_vram_wr32(adev, data + ((w + i) & mmask) * 4, pk[i]);
        gMappedWptr = w + n;
        rn_vram_wr32(adev, wptrOff, w + n); rn_vram_wr32(adev, wptrOff + 4, 0);          // the write-back word of the wptr, as mesRingSubmit keeps it
        (void)rn_vram_rd32(adev, data + (((w + n - 1) & mmask) * 4));                    // the ring writes must have landed before the kick
        __sync_synchronize();
        rn_hdp_flush(adev);
        *reinterpret_cast<volatile u64 *>(gDoorbellBase + gMappedDoorbell * 4) = w + n;
        return 0;
    }
    const u32 w = gComputeWptr;
    for (u32 i = 0; i < n; i++) rn_vram_wr32(adev, ringOff + ((w + i) & mask) * 4, pk[i]);
    grbmSelect(adev, 1, 0, 0, 2);
    computeSubmit(adev, wptrOff, w + n);
    grbmSelect(adev, 0, 0, 0, 0);
    return 0;
}

int rn_compute_submit_ib(struct rn_device *adev, u64 vaBase, u64 ringOff, u32 ringBytes, u64 ibVa, u32 ibDwords, u64 fenceVa, u32 fenceValue, u32 vmid) {
    if (gSubmitLock) IOLockLock(gSubmitLock);                                // slot reservation, packet writes, flush and doorbell are one critical section
    const int rc = computeSubmitLocked(adev, vaBase, ringOff, ringBytes, ibVa, ibDwords, fenceVa, fenceValue, vmid);
    if (gSubmitLock) IOLockUnlock(gSubmitLock);
    return rc;
}

void rn_gc_safe_mode(struct rn_device *adev, int enter) {
    if (!enter) { WREG32_SOC15(RN_GC, 0, regRLC_SAFE_MODE, RLC_SAFE_MODE__CMD_MASK); return; }
    WREG32_SOC15(RN_GC, 0, regRLC_SAFE_MODE, RLC_SAFE_MODE__CMD_MASK | (1u << RLC_SAFE_MODE__MESSAGE__SHIFT));
    for (int i = 0; i < 100000; i++) {                                       // the RLC clears CMD when the GC is awake
        if (!(RREG32_SOC15(RN_GC, 0, regRLC_SAFE_MODE) & RLC_SAFE_MODE__CMD_MASK)) break;
        udelay(1);
    }
}

void rn_compute_diag(struct rn_device *adev, u32 out[11]) {
    grbmSelect(adev, 1, gMapped ? gMappedPipe : 0, 0, gMapped ? gMappedQueue : 2);
    out[0] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
    out[1] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO);
    out[2] = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
    grbmSelect(adev, 0, 0, 0, 0);
    out[3] = RREG32_SOC15(RN_GC, 0, regCP_STAT);
    out[4] = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_STATUS);
    out[5] = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_LO32);
    out[6] = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_HI32);
    out[7] = gComputeWptr;
    out[8] = rn_rreg32(adev, adev->reg_offset[RN_GC_HWIP][0][0] + 0x35);                  // SDMA0_VERSION
    out[9] = rn_rreg32(adev, adev->reg_offset[RN_GC_HWIP][0][0] + 0xda4);                 // GRBM_STATUS
    out[10] = rn_rreg32(adev, adev->reg_offset[RN_GC_HWIP][0][1] + 0x4e6b);               // RLC_GPM_STAT
}

void rn_compute_ring_state(struct rn_device *adev, u32 out[4]) {
    if (gSubmitLock) IOLockLock(gSubmitLock);
    out[0] = gMapped ? rn_vram_rd32(adev, gMappedRingOff + 0x1000) : 0;       // rptr write-back word of the mapped queue
    out[1] = gMapped ? rn_vram_rd32(adev, gMappedRingOff + 0x1008) : 0;       // wptr word
    out[2] = gMappedWptr;
    out[3] = gRingFullWaits;
    if (gSubmitLock) IOLockUnlock(gSubmitLock);
}

void rn_compute_diag2(struct rn_device *adev, u32 out[8]) {
    grbmSelect(adev, 1, gMapped ? gMappedPipe : 0, 0, gMapped ? gMappedQueue : 2);
    out[0] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
    out[1] = RREG32_SOC15(RN_GC, 0, regCP_PQ_STATUS);
    out[2] = RREG32_SOC15(RN_GC, 0, regCP_MEC_DOORBELL_RANGE_LOWER);
    out[3] = RREG32_SOC15(RN_GC, 0, regCP_MEC_DOORBELL_RANGE_UPPER);
    out[4] = RREG32_SOC15(RN_NBIO, 0, regRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN);
    out[5] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_BASE);
    out[6] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_CONTROL);
    out[7] = RREG32_SOC15(RN_GC, 0, regCP_MQD_BASE_ADDR);
    grbmSelect(adev, 0, 0, 0, 0);
}

// MQD image for a compute queue (struct v11_compute_mqd, offsets in dwords), as gfx_v11_0_compute_mqd_init fills it.
static void writeComputeMqd(struct rn_device *adev, u64 vaBase, u64 ringOff, u32 ringBytes, u64 mqdOff, u64 eopOff, u32 doorbellDword,
                            bool mesQueue = false, u64 ringAt = 0) {
    const u64 ringVa = vaBase + ringOff + ringAt, rptrOff = ringOff + 0x1000, wptrOff = ringOff + 0x1008;
    const u64 rptrVa = vaBase + rptrOff, wptrVa = vaBase + wptrOff;
    const u64 eopBase = (vaBase + eopOff) >> 8;
    u32 qOrder = 0;
    while ((4u << qOrder) < ringBytes) qOrder++;
    u32 doorbellCtl = regCP_HQD_PQ_DOORBELL_CONTROL_DEFAULT;
    doorbellCtl = REG_SET_FIELD(doorbellCtl, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_OFFSET, doorbellDword);
    doorbellCtl = REG_SET_FIELD(doorbellCtl, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 1);
    u32 eopCtl = regCP_HQD_EOP_CONTROL_DEFAULT;
    eopCtl = REG_SET_FIELD(eopCtl, CP_HQD_EOP_CONTROL, EOP_SIZE, 8);
    u32 mqdCtl = regCP_MQD_CONTROL_DEFAULT;
    mqdCtl = REG_SET_FIELD(mqdCtl, CP_MQD_CONTROL, VMID, 0);
    u32 pqCtl = regCP_HQD_PQ_CONTROL_DEFAULT;
    pqCtl = REG_SET_FIELD(pqCtl, CP_HQD_PQ_CONTROL, QUEUE_SIZE, qOrder - 1);
    pqCtl = REG_SET_FIELD(pqCtl, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE, mesQueue ? 0 : 9);   // mes_v11_0_mqd_init ends up with 0 here
    pqCtl = REG_SET_FIELD(pqCtl, CP_HQD_PQ_CONTROL, UNORD_DISPATCH, 1);
    pqCtl = REG_SET_FIELD(pqCtl, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, mesQueue ? 0 : 1);
    pqCtl = REG_SET_FIELD(pqCtl, CP_HQD_PQ_CONTROL, PRIV_STATE, 1);
    pqCtl = REG_SET_FIELD(pqCtl, CP_HQD_PQ_CONTROL, KMD_QUEUE, 1);
    if (mesQueue) pqCtl = REG_SET_FIELD(pqCtl, CP_HQD_PQ_CONTROL, NO_UPDATE_RPTR, 1);
    u32 persist = regCP_HQD_PERSISTENT_STATE_DEFAULT;
    persist = REG_SET_FIELD(persist, CP_HQD_PERSISTENT_STATE, PRELOAD_SIZE, 0x55);
    u32 ibCtl = regCP_HQD_IB_CONTROL_DEFAULT;
    ibCtl = REG_SET_FIELD(ibCtl, CP_HQD_IB_CONTROL, MIN_IB_AVAIL_SIZE, 3);
    const u32 quantum = regCP_HQD_QUANTUM_DEFAULT;                           // Linux's compute MQD has the quantum at 0, not enabled
    for (u32 o = 0; o < 512 * 4; o += 4) rn_vram_wr32(adev, mqdOff + o, 0);
    for (u32 o = 0; o < ringBytes; o += 4) rn_vram_wr32(adev, ringOff + ringAt + o, 0);
    for (u32 o = 0; o < 2048; o += 4) rn_vram_wr32(adev, eopOff + o, 0);
    rn_vram_wr32(adev, rptrOff, 0); rn_vram_wr32(adev, rptrOff + 4, 0);
    rn_vram_wr32(adev, wptrOff, 0); rn_vram_wr32(adev, wptrOff + 4, 0);
    struct { u32 dw; u32 v; } mqd[] = {
        {0, 0xC0310800u}, {11, 1}, {23, 0xffffffffu}, {24, 0xffffffffu}, {26, 0xffffffffu}, {27, 0xffffffffu}, {32, 7},
        {165, lower_32_bits(eopBase)}, {166, upper_32_bits(eopBase)}, {167, eopCtl}, {143, doorbellCtl},
        {128, lower_32_bits(vaBase + mqdOff) & 0xfffffffcu}, {129, upper_32_bits(vaBase + mqdOff)}, {162, mqdCtl},
        {136, lower_32_bits(ringVa >> 8)}, {137, upper_32_bits(ringVa >> 8)}, {145, pqCtl},
        {139, lower_32_bits(rptrVa) & 0xfffffffcu}, {140, upper_32_bits(rptrVa) & 0xFFFFu},
        {141, lower_32_bits(wptrVa) & 0xfffffffcu}, {142, upper_32_bits(wptrVa) & 0xFFFFu},
        {132, persist}, {149, ibCtl}, {135, quantum}, {130, 1},
        {133, mesQueue ? 0u : 2u}, {134, mesQueue ? 0u : 15u},   // cp_hqd_pipe_priority and cp_hqd_queue_priority: Linux's kernel compute queues carry 2 and 15
    };
    for (u32 i = 0; i < sizeof(mqd) / sizeof(mqd[0]); i++) rn_vram_wr32(adev, mqdOff + mqd[i].dw * 4, mqd[i].v);
    (void)rn_vram_rd32(adev, mqdOff + 511 * 4);        // the MQD, ring and EOP writes are posted through the register path: make sure they have landed
    (void)rn_vram_rd32(adev, eopOff + 2044);
    __sync_synchronize();
}

// gfx_v11_0_kiq_init_register: copy the MQD values into the CP_HQD_* registers of the queue selected by grbmSelect and activate it.
static void programHqdFromMqd(struct rn_device *adev, u64 vaBase, u64 ringOff, u32 ringBytes, u64 mqdOff, u64 eopOff, u32 doorbellDword) {
    const u64 ringVa = vaBase + ringOff, rptrOff = ringOff + 0x1000, wptrOff = ringOff + 0x1008;
    const u64 eopBase = (vaBase + eopOff) >> 8;
    u32 qOrder = 0;
    while ((4u << qOrder) < ringBytes) qOrder++;
    u32 tmp;
    tmp = RREG32_SOC15(RN_GC, 0, regCP_PQ_WPTR_POLL_CNTL);
    tmp = REG_SET_FIELD(tmp, CP_PQ_WPTR_POLL_CNTL, EN, 0);
    WREG32_SOC15(RN_GC, 0, regCP_PQ_WPTR_POLL_CNTL, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE, 0);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_EOP_BASE_ADDR, lower_32_bits(eopBase));
    WREG32_SOC15(RN_GC, 0, regCP_HQD_EOP_BASE_ADDR_HI, upper_32_bits(eopBase));
    tmp = regCP_HQD_EOP_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_EOP_CONTROL, EOP_SIZE, 8);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_EOP_CONTROL, tmp);
    tmp = regCP_HQD_PQ_DOORBELL_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_OFFSET, doorbellDword);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 1);
    WREG32_SOC15(RN_GC, 0, regCP_MEC_DOORBELL_RANGE_LOWER, 0);
    WREG32_SOC15(RN_GC, 0, regCP_MEC_DOORBELL_RANGE_UPPER, 0x8A * 2 * 4);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_MQD_BASE_ADDR, lower_32_bits(vaBase + mqdOff) & 0xfffffffcu);
    WREG32_SOC15(RN_GC, 0, regCP_MQD_BASE_ADDR_HI, upper_32_bits(vaBase + mqdOff));
    tmp = regCP_MQD_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_MQD_CONTROL, VMID, 0);
    WREG32_SOC15(RN_GC, 0, regCP_MQD_CONTROL, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_BASE, lower_32_bits(ringVa >> 8));
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_BASE_HI, upper_32_bits(ringVa >> 8));
    tmp = regCP_HQD_PQ_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, QUEUE_SIZE, qOrder - 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE, 9);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, UNORD_DISPATCH, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, 1);   // Linux's compute MQD reads back with it set (allow_tunneling), docs/COMPAT.md
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, PRIV_STATE, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, KMD_QUEUE, 1);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_CONTROL, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR_REPORT_ADDR, lower_32_bits(vaBase + rptrOff) & 0xfffffffcu);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR_REPORT_ADDR_HI, upper_32_bits(vaBase + rptrOff) & 0xFFFFu);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_POLL_ADDR, lower_32_bits(vaBase + wptrOff) & 0xfffffffcu);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_POLL_ADDR_HI, upper_32_bits(vaBase + wptrOff) & 0xFFFFu);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR, 0);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO, 0);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_HI, 0);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_VMID, 0);
    tmp = regCP_HQD_PERSISTENT_STATE_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PERSISTENT_STATE, PRELOAD_SIZE, 0x55);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PERSISTENT_STATE, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE, 1);
    tmp = RREG32_SOC15(RN_GC, 0, regCP_PQ_STATUS);
    tmp = REG_SET_FIELD(tmp, CP_PQ_STATUS, DOORBELL_ENABLE, 1);
    WREG32_SOC15(RN_GC, 0, regCP_PQ_STATUS, tmp);
}

// Read-only look at the MES: CP_MES_CNTL, and per pipe (MEID 3) the instruction pointer twice, the trap registers and a scratch GP register.
// out: [0] CP_MES_CNTL, then per pipe 0 and 1: instr_pntr, instr_pntr 10 ms later, mepc, mcause, mbadaddr, gp0, gp3 (7 values each).
void rn_mes_state(struct rn_device *adev, u32 out[15]) {
    out[0] = RREG32_SOC15(RN_GC, 0, regCP_MES_CNTL);
    for (u32 pipe = 0; pipe < 2; pipe++) {
        u32 *o = out + 1 + pipe * 7;
        grbmSelect(adev, 3, pipe, 0, 0);
        o[0] = RREG32_SOC15(RN_GC, 0, regCP_MES_INSTR_PNTR);
        msleep(10);
        o[1] = RREG32_SOC15(RN_GC, 0, regCP_MES_INSTR_PNTR);
        o[2] = RREG32_SOC15(RN_GC, 0, regCP_MES_MEPC_LO);
        o[3] = RREG32_SOC15(RN_GC, 0, regCP_MES_MCAUSE_LO);
        o[4] = RREG32_SOC15(RN_GC, 0, regCP_MES_MBADADDR_LO);
        o[5] = RREG32_SOC15(RN_GC, 0, regCP_MES_GP0_LO);
        o[6] = RREG32_SOC15(RN_GC, 0, regCP_MES_GP3_LO);
        grbmSelect(adev, 0, 0, 0, 0);
    }
}


// ---- MES (mes_v11_0.c). On GFX11 the KIQ is the second MES pipe (ME 3, pipe 1, firmware mes1) and the scheduler is the first (ME 3,
// pipe 0, firmware mes_2). Both are halted after the PSP has loaded their images; the steps below follow mes_v11_0_kiq_hw_init and
// mes_v11_0_hw_init, with a progress callback before each so that the last one that ran is known if the machine stops.
// Native GFX11 ring sizes (amdgpu_ring_init: max_dw 1024 times sched_hw_submission: 8 for the MES scheduler, 256 for the KIQ, here a quarter of Linux's 1 MB; 2 for compute rings).
static const u32 kKiqRingBytes = 0x10000, kSchedRingBytes = 0x8000, kKcqRingBytes = 0x2000;
static const u64 kRingAt = 0x5000;        // a MES queue block: rptr +0x1000, wptr +0x1008, MQD +0x2000, EOP +0x4000, ring data from +0x5000
// The MES working area. The event log Linux gives the firmware is AMDGPU_MES_LOG_BUFFER_SIZE (0x4000) followed by the MSCRATCH area (0x40000): 0x44000 in all, as the
// size of the debugfs amdgpu_mes_event_log of the same card shows (713728 bytes of text at 82 per 32 of data). The firmware writes into the scratch area while the log is on; an
// area of 0x8000 was too small, the writes ran past it (a GCVM fault on the page after the host buffer, and in VRAM mode over the GART page table behind it).
static const u64 kMesAreaBytes = 0xA0000;   // kiq 0x00000, scheduler 0x20000, shared 0x30000, queue 0x40000, log 0x50000 (0x44000), dbgext 0x94000
static const u32 kMesDbStart = 1024;      // mes->db_start_dw_offset: the page after the kernel doorbells (amdgpu_doorbell_create_kernel_doorbells)
static const u32 kMesKiqDoorbell = 0x0C << 1, kMesSchedDoorbell = 0x0B << 1;      // AMDGPU_NAVI10_DOORBELL_MES_RING1/0, in dwords

// mes_v11_0_queue_init_register: one MES queue into the HQD registers of (ME 3, pipe).
static void mesQueueInitRegister(struct rn_device *adev, u64 vaBase, u64 ringOff, u32 ringBytes, u32 pipe, u64 mqdOff, u32 doorbellDword) {
    const u64 ringVa = vaBase + ringOff + kRingAt, rptrOff = ringOff + 0x1000, wptrOff = ringOff + 0x1008;
    u32 qOrder = 0;
    while ((4u << qOrder) < ringBytes) qOrder++;
    grbmSelect(adev, 3, pipe, 0, 0);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_VMID, 0);
    u32 tmp = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 0);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_MQD_BASE_ADDR, lower_32_bits(vaBase + mqdOff) & 0xfffffffcu);
    WREG32_SOC15(RN_GC, 0, regCP_MQD_BASE_ADDR_HI, upper_32_bits(vaBase + mqdOff));
    WREG32_SOC15(RN_GC, 0, regCP_MQD_CONTROL, 0);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_BASE, lower_32_bits(ringVa >> 8));
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_BASE_HI, upper_32_bits(ringVa >> 8));
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR_REPORT_ADDR, lower_32_bits(vaBase + rptrOff) & 0xfffffffcu);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR_REPORT_ADDR_HI, upper_32_bits(vaBase + rptrOff) & 0xFFFFu);
    tmp = regCP_HQD_PQ_CONTROL_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, QUEUE_SIZE, qOrder - 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE, 0);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, UNORD_DISPATCH, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, 0);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, PRIV_STATE, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, KMD_QUEUE, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, NO_UPDATE_RPTR, 1);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_CONTROL, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_POLL_ADDR, lower_32_bits(vaBase + wptrOff) & 0xfffffff8u);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_POLL_ADDR_HI, upper_32_bits(vaBase + wptrOff) & 0xFFFFu);
    tmp = 0;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_OFFSET, doorbellDword);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 1);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL, tmp);
    tmp = regCP_HQD_PERSISTENT_STATE_DEFAULT;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PERSISTENT_STATE, PRELOAD_SIZE, 0x55);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_PERSISTENT_STATE, tmp);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE, 1);
    grbmSelect(adev, 0, 0, 0, 0);
}

// soc21_flush_hdp / nbio_v4_3: the HDP flush is a write of 0 to BIF_BX_PF0_HDP_MEM_COHERENCY_FLUSH_CNTL (NBIO segment 2, dword 0xf7; KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL is
// offset 0 of its remap page, which for a native function is the register itself), read back to know it is done. It pushes CPU writes that went through the HDP (BAR0 VRAM
// writes) to memory; for host memory behind the GART the ordering that matters is the CPU's own store order (the barrier before it), but Linux does both before a kick.
void rn_hdp_flush(struct rn_device *adev) {
    const u32 reg = adev->reg_offset[RN_NBIO_HWIP][0][2] + 0xf7;
    rn_wreg32(adev, reg, 0);
    (void)rn_rreg32(adev, reg);
}

// Append one 64-dword API frame (or any dword run) to a MES ring at the monitonic write pointer, publish the pointer and ring the doorbell.
// The packets go in back to back and amdgpu_ring_commit pads the commit to the ring's align_mask with the ring's nop: the MES KIQ ring is gfx_v11_0_ring_funcs_kiq (align_mask
// 0xff, nop PACKET3(NOP, 0x3FFF) = 0xFFFF1000), the MES scheduler ring is mes_v11_0_ring_funcs (align_mask 1, nop 0). A zero dword in the middle of a PM4 stream is a type 0 header,
// which is what the KIQ ring used to get after its 7 dword MAP_QUEUES.
static const u32 kKiqNop = 0xFFFF1000u;
static void mesRingSubmit(struct rn_device *adev, u64 ringOff, u32 ringBytes, u32 alignMask, u32 nop, u64 wptrShadowOff, u32 doorbellDword, u64 *wptr, const u32 *dw, u32 n) {
    const u32 ringDwords = ringBytes / 4;
    const u64 data = ringOff + kRingAt;
    for (u32 i = 0; i < n; i++) rn_vram_wr32(adev, data + ((*wptr + i) % ringDwords) * 4, dw[i]);
    *wptr += n;
    for (u32 pad = alignMask + 1 - (*wptr & alignMask); pad && pad <= alignMask; pad--, (*wptr)++)
        rn_vram_wr32(adev, data + (*wptr % ringDwords) * 4, nop);
    rn_vram_wr32(adev, wptrShadowOff, lower_32_bits(*wptr));
    rn_vram_wr32(adev, wptrShadowOff + 4, upper_32_bits(*wptr));
    // The ring is written through MM_INDEX/MM_DATA, the doorbell goes straight through the doorbell BAR: the two paths are not ordered, so the firmware can see the
    // doorbell and read a ring that does not hold the packets yet. Linux writes its rings in host memory and flushes the HDP before the doorbell. A read through the
    // register path after the writes cannot complete before they have.
    (void)rn_vram_rd32(adev, data + (((*wptr - 1) % ringDwords) * 4));
    (void)rn_vram_rd32(adev, wptrShadowOff + 4);
    __sync_synchronize();
    rn_hdp_flush(adev);
    *reinterpret_cast<volatile u64 *>(gDoorbellBase + doorbellDword * 4) = *wptr;
}

static void mesPcSample(struct rn_device *adev, u32 pipe, u32 *pc, u32 *pc10) {
    grbmSelect(adev, 3, pipe, 0, 0);
    *pc = RREG32_SOC15(RN_GC, 0, regCP_MES_INSTR_PNTR);
    msleep(10);
    *pc10 = RREG32_SOC15(RN_GC, 0, regCP_MES_INSTR_PNTR);
    grbmSelect(adev, 0, 0, 0, 0);
}

// Every active HQD of ME 1: up to 8 entries of {pipe * 8 + queue, doorbell control, rptr, wptr}, then the count in out[32].
void rn_scan_hqds(struct rn_device *adev, u32 out[33]) {
    u32 n = 0;
    memset(out, 0, 33 * sizeof(u32));
    for (u32 pipe = 0; pipe < 4; pipe++)
        for (u32 q = 0; q < 8; q++) {
            grbmSelect(adev, 1, pipe, 0, q);
            if (!(RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE) & 1u)) continue;
            if (n < 8) {
                out[n * 4] = pipe * 8 + q;
                out[n * 4 + 1] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
                out[n * 4 + 2] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
                out[n * 4 + 3] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO);
            }
            n++;
        }
    grbmSelect(adev, 0, 0, 0, 0);
    out[32] = n;
}

// CP_MES_DOORBELL_CONTROL1..6 as seen from each MES pipe (they may be banked per pipe): out[0..5] pipe 0, out[6..11] pipe 1.
void rn_mes_doorbell_regs(struct rn_device *adev, u32 out[12]) {
    for (u32 pipe = 0; pipe < 2; pipe++) {
        grbmSelect(adev, 3, pipe, 0, 0);
        out[pipe * 6 + 0] = RREG32_SOC15(RN_GC, 0, regCP_MES_DOORBELL_CONTROL1);
        out[pipe * 6 + 1] = RREG32_SOC15(RN_GC, 0, regCP_MES_DOORBELL_CONTROL2);
        out[pipe * 6 + 2] = RREG32_SOC15(RN_GC, 0, regCP_MES_DOORBELL_CONTROL3);
        out[pipe * 6 + 3] = RREG32_SOC15(RN_GC, 0, regCP_MES_DOORBELL_CONTROL4);
        out[pipe * 6 + 4] = RREG32_SOC15(RN_GC, 0, regCP_MES_DOORBELL_CONTROL5);
        out[pipe * 6 + 5] = RREG32_SOC15(RN_GC, 0, regCP_MES_DOORBELL_CONTROL6);
    }
    grbmSelect(adev, 0, 0, 0, 0);
}

// amdgpu_ring_test_helper on the KIQ ring (gfx_v11_0_ring_test_ring): SCRATCH_REG0 is set to 0xCAFEDEAD, a WRITE_DATA with write confirm puts 0xDEADBEEF in it
// through the ring, and the register is read back. Returns the register and the KIQ pipe's program counter afterwards.
static void mesKiqRingTest(struct rn_device *adev, u64 kiqOff, u64 *kiqWptr, u32 *scratch, u32 *pc) {
    WREG32_SOC15(RN_GC, 0, regSCRATCH_REG0, 0xCAFEDEADu);
    u32 pk[8];
    pk[0] = RN_PACKET3(0x37u, 3);                                                 // WRITE_DATA
    pk[1] = 1u << 16;                                                             // write confirm, destination register
    pk[2] = 0xa000u + regSCRATCH_REG0;                                            // SCRATCH_REG0, absolute dword
    pk[3] = 0;
    pk[4] = 0xDEADBEEFu;
    pk[5] = RN_PACKET3(0x10u, 1); pk[6] = 0; pk[7] = 0;                           // NOP to the 8-dword boundary
    mesRingSubmit(adev, kiqOff, kKiqRingBytes, 0xff, kKiqNop, kiqOff + 0x1008, kMesKiqDoorbell, kiqWptr, pk, 5);
    for (int i = 0; i < 100; i++) {
        if (RREG32_SOC15(RN_GC, 0, regSCRATCH_REG0) == 0xDEADBEEFu) break;
        msleep(1);
    }
    *scratch = RREG32_SOC15(RN_GC, 0, regSCRATCH_REG0);
    grbmSelect(adev, 3, 1, 0, 0);
    *pc = RREG32_SOC15(RN_GC, 0, regCP_MES_INSTR_PNTR);
    grbmSelect(adev, 0, 0, 0, 0);
}

// What rn_mes_start leaves for later scheduler frames (a reset, then a re-map): where the scheduler ring and the completion words are, how far the ring has been
// written, the last completion number used, and the legacy queue that was mapped.
static struct {
    bool valid;
    u64 vaBase, schedOff, qOff, ringFenceOff, apiStatusOff, schedWptr;
    u32 seq, doorbell, pipe, queue, schedVersion;
} gMesCtx;

// One scheduler frame after boot, as the sendFrame of rn_mes_start does it: the frame, then QUERY_SCHEDULER_STATUS whose fence proves the scheduler got through it.
static bool mesSendLate(struct rn_device *adev, void *frame, u32 statusDwordOff, u32 *apiStatus) {
    u32 *f = static_cast<u32 *>(frame);
    const u64 stAddr = gMesCtx.vaBase + gMesCtx.apiStatusOff;
    f[statusDwordOff] = lower_32_bits(stAddr); f[statusDwordOff + 1] = upper_32_bits(stAddr);
    f[statusDwordOff + 2] = 1; f[statusDwordOff + 3] = 0;
    rn_vram_wr32(adev, gMesCtx.apiStatusOff, 0); rn_vram_wr32(adev, gMesCtx.apiStatusOff + 4, 0);
    const u32 seq = ++gMesCtx.seq;
    union MESAPI__QUERY_MES_STATUS q;
    memset(&q, 0, sizeof(q));
    q.header.type = MES_API_TYPE_SCHEDULER;
    q.header.opcode = MES_SCH_API_QUERY_SCHEDULER_STATUS;
    q.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
    q.api_status.api_completion_fence_addr = gMesCtx.vaBase + gMesCtx.ringFenceOff;
    q.api_status.api_completion_fence_value = seq;
    mesRingSubmit(adev, gMesCtx.schedOff, kSchedRingBytes, 1, 0, gMesCtx.schedOff + 0x1008, kMesSchedDoorbell, &gMesCtx.schedWptr, f, API_FRAME_SIZE_IN_DWORDS);
    mesRingSubmit(adev, gMesCtx.schedOff, kSchedRingBytes, 1, 0, gMesCtx.schedOff + 0x1008, kMesSchedDoorbell, &gMesCtx.schedWptr, reinterpret_cast<const u32 *>(&q), API_FRAME_SIZE_IN_DWORDS);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    for (int i = 0; i < 2000; i++) {
        if (rn_vram_rd32(adev, gMesCtx.ringFenceOff) >= seq) break;
        msleep(1);
    }
    *apiStatus = rn_vram_rd32(adev, gMesCtx.apiStatusOff);
    return rn_vram_rd32(adev, gMesCtx.ringFenceOff) >= seq && *apiStatus != 0;
}

void rn_mes_start(struct rn_device *adev, u64 vaBase, u64 base, const struct rn_gmc *gmc, const u32 start[4], u32 maxStep, void (*progress)(void *, u32), void *ctx,
                  struct rn_mes_result *res) {
    static_assert(sizeof(union MESAPI_SET_HW_RESOURCES) == 256 && sizeof(union MESAPI__ADD_QUEUE) == 256 &&
                  sizeof(union MESAPI__QUERY_MES_STATUS) == 256, "MES API frames are 64 dwords");
    memset(res, 0, sizeof(*res));
    res->bootload = RREG32_SOC15(RN_GC, 0, regRLC_RLCS_BOOTLOAD_STATUS);        // gfx_v11_0_wait_for_rlc_autoload_complete wants BOOTLOAD_COMPLETE (bit 31) and CP_STAT 0
    res->rlc_srm = RREG32_SOC15(RN_GC, 0, regRLC_SRM_CNTL);
    res->cp_stat0 = RREG32_SOC15(RN_GC, 0, regCP_STAT);
    res->cp_me_cntl = RREG32_SOC15(RN_GC, 0, regCP_ME_CNTL);                    // Linux after its bring-up: 0x00000000
    res->mec_cntl = RREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_CNTL);                // Linux: 0x3c000000
    res->rlc_sched = RREG32_SOC15(RN_GC, 0, regRLC_CP_SCHEDULERS);              // Linux: 0x000030e8
    if (!gDoorbellBase) return;                                // the rings are driven by doorbell writes
    // VRAM map (all below the GART page table at 0x0C100000): MES KIQ ring at base, MES scheduler ring at base + 0x10000, shared words at +0x20000,
    // the legacy compute queue at +0x30000. In each: ring, rptr at +0x1000, wptr at +0x1008, MQD at +0x2000, EOP at +0x4000. The firmware log
    // (mes_dbgext, opt-in) is a 16 KB area at +0x50000, the last thing before the page table.
    const u64 kiqOff = base, schedOff = base + 0x20000, sharedOff = base + 0x30000, qOff = base + 0x40000;
    const u64 schCtxOff = sharedOff, queryFenceOff = sharedOff + 0x10, ringFenceOff = sharedOff + 0x20, apiStatusOff = sharedOff + 0x100;
    const u32 qPipe = 0, qQueue = 0;   // a kernel-owned queue (outside the MES hqd mask 0xc), as Linux maps its first compute ring
    const u32 legDb = 6;               // mec_ring0 << 1

    // What the previous boot left in the card (a warm reboot may not reset it): MES control and both pipes' program counters, the scheduler and KIQ queues'
    // HQD state, the RLC scheduler register, the MEC queues that are active. Read before anything of this boot's touches the MES.
    res->init_state[0] = RREG32_SOC15(RN_GC, 0, regCP_MES_CNTL);
    res->init_state[1] = RREG32_SOC15(RN_GC, 0, regRLC_CP_SCHEDULERS);
    for (u32 pipe = 0; pipe < 2; pipe++) {
        grbmSelect(adev, 3, pipe, 0, 0);
        res->init_state[2 + pipe] = RREG32_SOC15(RN_GC, 0, regCP_MES_INSTR_PNTR);
        res->init_state[4 + pipe] = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
        res->init_state[6 + pipe] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
    }
    for (u32 pipe = 0; pipe < 4; pipe++)
        for (u32 q = 0; q < 8; q++) {
            grbmSelect(adev, 1, pipe, 0, q);
            if (RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE) & 1u) res->init_state[8] |= 1u << (pipe * 8 + q);
        }
    grbmSelect(adev, 0, 0, 0, 0);
    res->init_state[9] = RREG32_SOC15(RN_GC, 0, regRLC_RLCS_BOOTLOAD_STATUS);
    // VRAM keeps its contents over a warm reboot, and the MES writes its scheduler context, fences and logs into this area: a buffer Linux allocates zeroed
    // (amdgpu_bo_create_kernel clears it) would otherwise start here with whatever the previous boot left in it. Everything between the base and the GART page table.
    for (u64 o = 0; o < kMesAreaBytes; o += 4) rn_vram_wr32(adev, base + o, 0);
    // step 1: enable (mes_v11_0_enable): reset both pipes, give each its program counter, then un-halt and activate both
    if (progress) progress(ctx, 1);
    const u64 logOff = base + 0x50000, logVa = vaBase + logOff;           // MES event log (amdgpu_mes_log_enable): history in the first half, scratch in the second
    for (u32 o = 0; o < 0x8000; o += 4) rn_vram_wr32(adev, logOff + o, 0);
    WREG32_SOC15(RN_GC, 0, regCP_MES_MSCRATCH_LO, lower_32_bits(logVa + 0x4000));
    WREG32_SOC15(RN_GC, 0, regCP_MES_MSCRATCH_HI, upper_32_bits(logVa + 0x4000));
    u32 d = RREG32_SOC15(RN_GC, 0, regCP_MES_CNTL);
    d = REG_SET_FIELD(d, CP_MES_CNTL, MES_PIPE0_RESET, 1);
    d = REG_SET_FIELD(d, CP_MES_CNTL, MES_PIPE1_RESET, 1);
    WREG32_SOC15(RN_GC, 0, regCP_MES_CNTL, d);
    for (u32 pipe = 0; pipe < 2; pipe++) {
        const u64 pc = ((static_cast<u64>(start[pipe * 2 + 1]) << 32) | start[pipe * 2]) >> 2;
        grbmSelect(adev, 3, pipe, 0, 0);
        WREG32_SOC15(RN_GC, 0, regCP_MES_PRGRM_CNTR_START, lower_32_bits(pc));
        WREG32_SOC15(RN_GC, 0, regCP_MES_PRGRM_CNTR_START_HI, upper_32_bits(pc));
    }
    grbmSelect(adev, 0, 0, 0, 0);
    d = 0;
    d = REG_SET_FIELD(d, CP_MES_CNTL, MES_PIPE0_ACTIVE, 1);
    d = REG_SET_FIELD(d, CP_MES_CNTL, MES_PIPE1_ACTIVE, 1);
    WREG32_SOC15(RN_GC, 0, regCP_MES_CNTL, d);
    udelay(500);
    res->cntl = RREG32_SOC15(RN_GC, 0, regCP_MES_CNTL);
    mesPcSample(adev, 0, &res->p0_pc, &res->p0_pc10);
    mesPcSample(adev, 1, &res->p1_pc, &res->p1_pc10);
    // The firmware version each pipe reports (mes_v11_0_get_fw_version reads CP_MES_GP3_LO) right after the start: proves which image the PSP really loaded.
    grbmSelect(adev, 3, 0, 0, 0);
    res->fw_ver0 = RREG32_SOC15(RN_GC, 0, regCP_MES_GP3_LO);
    grbmSelect(adev, 3, 1, 0, 0);
    res->fw_ver1 = RREG32_SOC15(RN_GC, 0, regCP_MES_GP3_LO);
    grbmSelect(adev, 0, 0, 0, 0);
    res->step = 1;
    if (maxStep < 2) return;

    // step 2: tell the RLC which queue is the KIQ (ME 3, pipe 1, queue 0), then bring the KIQ queue up through the registers
    if (progress) progress(ctx, 2);
    d = RREG32_SOC15(RN_GC, 0, regRLC_CP_SCHEDULERS);
    d &= 0xffffff00u;
    d |= (3u << 5) | (1u << 3) | 0u;
    WREG32_SOC15(RN_GC, 0, regRLC_CP_SCHEDULERS, d | 0x80);
    writeComputeMqd(adev, vaBase, kiqOff, kKiqRingBytes, kiqOff + 0x2000, kiqOff + 0x4000, kMesKiqDoorbell, true, kRingAt);
    for (u32 o = 0; o < kKiqRingBytes; o += 4) rn_vram_wr32(adev, kiqOff + kRingAt + o, kKiqNop);   // amdgpu_ring_clear_ring: the KIQ ring starts full of NOPs
    mesQueueInitRegister(adev, vaBase, kiqOff, kKiqRingBytes, 1, kiqOff + 0x2000, kMesKiqDoorbell);
    grbmSelect(adev, 3, 1, 0, 0);
    res->kiq_active = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
    res->kiq_doorbell_ctl = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
    grbmSelect(adev, 0, 0, 0, 0);
    res->step = 2;
    if (maxStep < 3) return;

    // step 3: the scheduler's MQD, then MAP_QUEUES on the KIQ ring (MES engine, ME field 2, engine select 5) loads it into pipe 0
    if (progress) progress(ctx, 3);
    writeComputeMqd(adev, vaBase, schedOff, kSchedRingBytes, schedOff + 0x2000, schedOff + 0x4000, kMesSchedDoorbell, true, kRingAt);
    u64 kiqWptr = 0;
    {
        u32 pk[8];
        const u64 mqdVa = vaBase + schedOff + 0x2000, wptrVa = vaBase + schedOff + 0x1008;
        pk[0] = RN_PACKET3(0xA2u, 5);                                                  // PACKET3_MAP_QUEUES
        pk[1] = (0u << 4) | (0u << 8) | (0u << 13) | (0u << 16) | (2u << 18) | (0u << 21) | (0u << 24) | (5u << 26) | (1u << 29);
        pk[2] = kMesSchedDoorbell << 2;
        pk[3] = lower_32_bits(mqdVa); pk[4] = upper_32_bits(mqdVa);
        pk[5] = lower_32_bits(wptrVa); pk[6] = upper_32_bits(wptrVa);
        pk[7] = 0;                                                                     // align_mask 1: an even number of dwords
        mesRingSubmit(adev, kiqOff, kKiqRingBytes, 0xff, kKiqNop, kiqOff + 0x1008, kMesKiqDoorbell, &kiqWptr, pk, 7);
    }
    // GRBM_GFX_CNTL is one register the MES firmware also uses: it selects the HQD of the queue it is programming and then writes the HQD registers. A host write
    // to it while the KIQ pipe is mid-way through this MAP_QUEUES (the polling loop below used to flip it every millisecond, and so did the program counter
    // trace) redirects the firmware's HQD writes to another queue. Linux never touches it while the MES runs. So: no GRBM access at all until the KIQ has had
    // plenty of time to finish (a few milliseconds are enough for it); progress is then read once, not polled.
    msleep(300);
    for (int i = 0; i < 1000; i++) {                                                   // until the firmware has loaded the scheduler queue
        grbmSelect(adev, 3, 0, 0, 0);
        res->sched_active = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
        grbmSelect(adev, 0, 0, 0, 0);
        if (res->sched_active & 1u) break;
        msleep(1);
    }
    grbmSelect(adev, 3, 0, 0, 0);                                                      // what the KIQ made of the scheduler's MQD: the HQD against what the MQD says
    res->sched_hqd[0] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_BASE);
    res->sched_hqd[1] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_BASE_HI);
    res->sched_hqd[2] = RREG32_SOC15(RN_GC, 0, regCP_MQD_BASE_ADDR);
    res->sched_hqd[3] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_CONTROL);
    res->sched_hqd[4] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
    res->sched_hqd[5] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
    res->sched_hqd[6] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO);
    res->sched_hqd[7] = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
    res->sched_hqd[8] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR_REPORT_ADDR);
    res->sched_hqd[9] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_POLL_ADDR);
    grbmSelect(adev, 3, 1, 0, 0);
    res->kiq_rptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
    res->kiq_doorbell_ctl = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
    grbmSelect(adev, 0, 0, 0, 0);
    mesPcSample(adev, 0, &res->p0_pc, &res->p0_pc10);
    mesPcSample(adev, 1, &res->p1_pc, &res->p1_pc10);
    mesKiqRingTest(adev, kiqOff, &kiqWptr, &res->ktest[0], &res->ktest[1]);   // the KIQ right after it mapped the scheduler queue
    res->step = 3;
    if (maxStep < 4 || !(res->sched_active & 1u)) return;

    // Send one API frame to the scheduler followed by QUERY_SCHEDULER_STATUS whose completion fence is the proof that the scheduler got through
    // the first (mes_v11_0_submit_pkt_and_poll_completion). The frame's own completion word is at apiStatusOff.
    u64 schedWptr = 0;
    u32 seq = 0;
    auto sendFrame = [&](void *frame, u32 statusDwordOff) -> bool {
        u32 *f = static_cast<u32 *>(frame);
        // api_status {fence_addr, fence_value} sits at dword statusDwordOff of the frame
        const u64 stAddr = vaBase + apiStatusOff;
        f[statusDwordOff] = lower_32_bits(stAddr); f[statusDwordOff + 1] = upper_32_bits(stAddr);
        f[statusDwordOff + 2] = 1; f[statusDwordOff + 3] = 0;
        rn_vram_wr32(adev, apiStatusOff, 0); rn_vram_wr32(adev, apiStatusOff + 4, 0);
        seq++;
        union MESAPI__QUERY_MES_STATUS q;
        memset(&q, 0, sizeof(q));
        q.header.type = MES_API_TYPE_SCHEDULER;
        q.header.opcode = MES_SCH_API_QUERY_SCHEDULER_STATUS;
        q.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
        q.api_status.api_completion_fence_addr = vaBase + ringFenceOff;
        q.api_status.api_completion_fence_value = seq;
        mesRingSubmit(adev, schedOff, kSchedRingBytes, 1, 0, schedOff + 0x1008, kMesSchedDoorbell, &schedWptr, f, API_FRAME_SIZE_IN_DWORDS);
        mesRingSubmit(adev, schedOff, kSchedRingBytes, 1, 0, schedOff + 0x1008, kMesSchedDoorbell, &schedWptr, reinterpret_cast<const u32 *>(&q), API_FRAME_SIZE_IN_DWORDS);
        // dma_rmb()/smp_rmb() before the fence poll, as amdgpu_fence_wait_polling reads the fence behind a read barrier: nothing read below may be satisfied from before it
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        for (int i = 0; i < 2000; i++) {
            if (rn_vram_rd32(adev, ringFenceOff) >= seq) break;
            msleep(1);
        }
        res->ring_fence = rn_vram_rd32(adev, ringFenceOff);
        res->api_status = rn_vram_rd32(adev, apiStatusOff);
        return res->ring_fence >= seq && res->api_status != 0;
    };
    rn_vram_wr32(adev, ringFenceOff, 0); rn_vram_wr32(adev, ringFenceOff + 4, 0);
    rn_vram_wr32(adev, schCtxOff, 0); rn_vram_wr32(adev, schCtxOff + 4, 0);
    rn_vram_wr32(adev, queryFenceOff, 0); rn_vram_wr32(adev, queryFenceOff + 4, 0);

    // 3b: the MES firmware's own log. The v11 API has a MISC opcode that hands the scheduler a circular buffer, and the firmware's debug
    // extension writes text records into it: the only way to see what it does, or waits for, inside a frame. Linux arms the same thing at the
    // end of mes_v11_0_hw_init (amdgpu_mes_dbgext_start, module parameter amdgpu_mes_dbgext_buffer_size) and expects nothing else from the
    // driver: the buffer size goes in the first dword as that function seeds it, and the header is left by the firmware, so the layout is
    // read out of the hardware rather than assumed.
    //
    // Where the buffer lives is a difference worth keeping switchable: Linux allocates it as a GTT BO in host memory and hands the firmware
    // its GART address, unlike the event log above, which is in VRAM. Two runs with it in VRAM (options word 0, then 3) had the frame
    // completed and the buffer untouched, not even a header (7800 XT, 2026-10-31), so the host buffer is the remaining arrangement to try.

    // step 4: SET_HW_RESOURCES
    if (progress) progress(ctx, 4);
    {
        union MESAPI_SET_HW_RESOURCES p;
        memset(&p, 0, sizeof(p));
        p.header.type = MES_API_TYPE_SCHEDULER;
        p.header.opcode = MES_SCH_API_SET_HW_RSRC;
        p.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
        p.vmid_mask_mmhub = 0xff00;
        p.vmid_mask_gfxhub = 0xff00;
        p.paging_vmid = 0;
        p.g_sch_ctx_gpu_mc_ptr = vaBase + schCtxOff;
        p.query_status_fence_gpu_mc_ptr = vaBase + queryFenceOff;
        // Linux (6.x, same card, docs/logs/linux-reference/mes_dmesg_linux.txt) reports compute_hqd_mask 0x0000000c: the MES schedules only queues 2 and 3 of a
        // pipe. 0xf0 named queues the scheduler never uses, so it found no hardware queue to load a process queue into. Pipe 0 is left to the hand-programmed queues.
        // The legacy path is the real one (no hand-made queue is expected in the way): all four pipes, queues 2 and 3, as Linux. The process path (step 7 and up)
        // keeps pipe 0 for the hand-programmed queues.
        p.compute_hqd_mask[0] = maxStep >= 7 ? 0 : 0xc;
        for (int i = 1; i < 4; i++) p.compute_hqd_mask[i] = 0xc;
        p.gfx_hqd_mask[0] = 0x2; p.sdma_hqd_mask[0] = 0xfc; p.sdma_hqd_mask[1] = 0xfc;
        for (int i = 0; i < AMD_PRIORITY_NUM_LEVELS; i++) p.aggregated_doorbells[i] = kMesDbStart + i * 2;   // one doorbell per priority level, in the page after the kernel doorbells (amdgpu_mes_doorbell_init)
        for (int i = 0; i < 5; i++) {
            p.gc_base[i] = adev->reg_offset[RN_GC_HWIP][0][i];
            p.mmhub_base[i] = adev->reg_offset[RN_MMHUB_HWIP][0][i];
            p.osssys_base[i] = adev->reg_offset[RN_OSSSYS_HWIP][0][i];
        }
        p.disable_reset = 1;
        p.enable_mes_event_int_logging = 1;                // the event log area (see logOff); Linux sends it with amdgpu.mes_log_enable
        p.event_intr_history_gpu_mc_ptr = logVa;
        p.use_different_vmid_compute = 1;
        p.enable_reg_active_poll = 1;
        p.enable_level_process_quantum_check = 1;
        res->hw_res_ok = sendFrame(&p, offsetof(union MESAPI_SET_HW_RESOURCES, api_status) / 4);
    }
    if (res->hw_res_ok) {                                  // scheduler versions from 0x52 want SET_HW_RESOURCES_1 as well (cleaner shader fence)
        union MESAPI_SET_HW_RESOURCES_1 p1;
        memset(&p1, 0, sizeof(p1));
        p1.header.type = MES_API_TYPE_SCHEDULER;
        p1.header.opcode = MES_SCH_API_SET_HW_RSRC_1;
        p1.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
        for (u32 o = 0; o < 0x1000; o += 4) rn_vram_wr32(adev, sharedOff + 0x200 + o, 0);   // the fence area must start at zero (it read 0xffffffff in VRAM and the scheduler then spun in ADD_QUEUE)
        p1.enable_mes_info_ctx = 1;
        p1.cleaner_shader_fence_mc_addr = vaBase + sharedOff + 0x200;
        res->hw_res_ok = sendFrame(&p1, offsetof(union MESAPI_SET_HW_RESOURCES_1, api_status) / 4);
        res->hw_res1_status = res->api_status;
    }

    mesPcSample(adev, 0, &res->p0_pc, &res->p0_pc10);
    res->step = 4;
    if (!res->hw_res_ok) {                                  // the scheduler did not answer SET_HW_RESOURCES: what it saw (HQD pointers, the ring head, the fence words)
        grbmSelect(adev, 3, 0, 0, 0);
        res->sched_rptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
        res->sched_wptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO);
        res->sched_hqd[5] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
        res->sched_hqd[6] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO);
        res->sched_hqd[4] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
        res->p0_pc_end = RREG32_SOC15(RN_GC, 0, regCP_MES_INSTR_PNTR);
        grbmSelect(adev, 0, 0, 0, 0);
        for (u32 i = 0; i < 4; i++) res->ring_head[i] = rn_vram_rd32(adev, schedOff + kRingAt + i * 4);
        res->ring_head[4] = rn_vram_rd32(adev, schedOff + 0x1000);          // rptr word
        res->ring_head[5] = rn_vram_rd32(adev, schedOff + 0x1008);          // wptr word
        res->ring_head[6] = rn_vram_rd32(adev, ringFenceOff);
        res->ring_head[7] = rn_vram_rd32(adev, apiStatusOff);
    }
    if (maxStep < 5 || !res->hw_res_ok) return;

    // step 5: map a legacy kernel compute queue (ME 1) through the scheduler (mes_v11_0_map_legacy_queue)
    if (progress) progress(ctx, 5);
    // Which MEC hardware queues are active right now (bit pipe * 8 + queue)? The map of a legacy queue starts with the HQD being free; a queue left
    // active by an earlier hand-programmed test would make the scheduler wait for it to drain (the event log shows its map operation started and never ended).
    for (u32 pipe = 0; pipe < 4; pipe++)
        for (u32 q = 0; q < 8; q++) {
            grbmSelect(adev, 1, pipe, 0, q);
            if (RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE) & 1u) res->hqd_active |= 1u << (pipe * 8 + q);
        }
    grbmSelect(adev, 0, 0, 0, 0);
    gMappedDoorbell = legDb;
    writeComputeMqd(adev, vaBase, qOff, kKcqRingBytes, qOff + 0x2000, qOff + 0x4000, legDb, false, kRingAt);
    {
        // amdgpu_gfx_mes_enable_kcq: SET_RESOURCES on the KIQ ring with every queue in the mask comes before the legacy queue is mapped.
        // Dwords 4 and 5 are the cleaner shader address; RedNavi sends none (Linux's shader is not copied, and sending a one-instruction shader
        // changed nothing on the hardware, 2026-11-01).
        u32 pk[8];
        pk[0] = RN_PACKET3(0xA0u, 6);
        pk[1] = (0u << 0) | (0xau << 16) | (0u << 29);
        pk[2] = 0xffffffffu; pk[3] = 0xffffffffu; pk[4] = 0; pk[5] = 0; pk[6] = 0; pk[7] = 0;
        mesRingSubmit(adev, kiqOff, kKiqRingBytes, 0xff, kKiqNop, kiqOff + 0x1008, kMesKiqDoorbell, &kiqWptr, pk, 8);
        msleep(300);                                                      // no GRBM access while the KIQ handles it (see the step 3 comment)
        mesKiqRingTest(adev, kiqOff, &kiqWptr, &res->ktest[2], &res->ktest[3]);   // and after SET_RESOURCES, as amdgpu_gfx_mes_enable_kcq does
        grbmSelect(adev, 3, 1, 0, 0);
        res->kiq_rptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);       // 0x10 once both the MAP_QUEUES and the SET_RESOURCES are consumed
        grbmSelect(adev, 0, 0, 0, 0);
    }
    // The MQD must be 4 KB aligned and a full struct v11_compute_mqd (512 dwords, written and zeroed by writeComputeMqd above).
    if (maxStep >= 7) {
        // The scheduler hierarchy as mes_userq_map builds it: the v11 API has no separate ADD_PROCESS or ADD_GANG opcode; the process context
        // (4 KB) and the gang context (4 KB) are buffers handed to ADD_QUEUE, which creates the process and the gang the first time it sees them.
        const u64 procCtxOff = sharedOff + 0x2000, gangCtxOff = sharedOff + 0x3000;
        // The process runs in a VM context, so the queue's ring, write-back words, MQD and EOP buffer need virtual addresses it can translate: map
        // the six VRAM pages of the queue (ring, rptr/wptr page, MQD, EOP) into the GART window and use those addresses everywhere.
        const u32 procDoorbell = kMesDbStart + 64;                      // in the MES kernel page, clear of the aggregated doorbells
        gMappedDoorbell = procDoorbell;
        const u32 gartPage = 12000;
        for (u32 k = 0; k < 8; k++) rn_gart_map_vram_page(adev, gmc, gartPage + k, vaBase + qOff + k * 4096ull);
        const u64 vaQ = gmc->gart_start + gartPage * 4096ull - qOff;         // vaQ + qOff is the GART address of the queue's first page
        writeComputeMqd(adev, vaQ, qOff, 4096, qOff + 0x2000, qOff + 0x4000, procDoorbell);
        for (u32 o = 0; o < 0x2000; o += 4) rn_vram_wr32(adev, procCtxOff + o, 0);
        if (maxStep >= 8) {
            union MESAPI__SET_SCHEDULING_CONFIG c;
            memset(&c, 0, sizeof(c));
            c.header.type = MES_API_TYPE_SCHEDULER;
            c.header.opcode = MES_SCH_API_SET_SCHEDULING_CONFIG;
            c.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
            for (int i = 0; i < AMD_PRIORITY_NUM_LEVELS; i++) {
                c.grace_period_other_levels[i] = 50000;
                c.process_quantum_for_level[i] = 100000;
                c.process_grace_period_same_level[i] = 50000;
            }
            c.normal_yield_percent = 10;
            res->sched_cfg_ok = sendFrame(&c, offsetof(union MESAPI__SET_SCHEDULING_CONFIG, api_status) / 4);
        }
        union MESAPI__ADD_QUEUE p;
        memset(&p, 0, sizeof(p));
        p.header.type = MES_API_TYPE_SCHEDULER;
        p.header.opcode = MES_SCH_API_ADD_QUEUE;
        p.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
        p.process_id = 1;
        p.page_table_base_addr = vaBase + 0x0C100000;                  // the GART page table of the VM context 0
        p.process_va_start = 0;
        p.process_va_end = 0xfffffffffull;                              // max_pfn - 1
        p.process_quantum = 100000;
        p.gang_quantum = 10000;
        p.process_context_addr = vaBase + procCtxOff;
        p.gang_context_addr = vaBase + gangCtxOff;
        p.inprocess_gang_priority = AMD_PRIORITY_LEVEL_NORMAL;
        p.gang_global_priority_level = AMD_PRIORITY_LEVEL_NORMAL;
        p.doorbell_offset = procDoorbell;
        p.mqd_addr = vaQ + qOff + 0x2000;
        p.wptr_addr = vaQ + qOff + 0x1008;
        p.queue_type = MES_QUEUE_TYPE_COMPUTE;
        p.vm_context_cntl = RREG32_SOC15(RN_GC, 0, regGCVM_CONTEXT0_CNTL);
        p.paging = 0;
        rn_gc_safe_mode(adev, 1);
        res->add_ok = sendFrame(&p, offsetof(union MESAPI__ADD_QUEUE, api_status) / 4);
        rn_gc_safe_mode(adev, 0);
    } else {
        union MESAPI__ADD_QUEUE p;
        memset(&p, 0, sizeof(p));
        p.header.type = MES_API_TYPE_SCHEDULER;
        p.header.opcode = MES_SCH_API_ADD_QUEUE;
        p.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
        p.pipe_id = qPipe;
        p.queue_id = qQueue;
        p.doorbell_offset = legDb;
        p.mqd_addr = vaBase + qOff + 0x2000;
        p.wptr_addr = vaBase + qOff + 0x1008;
        p.queue_type = MES_QUEUE_TYPE_COMPUTE;
        p.map_legacy_kq = 1;
        rn_gc_safe_mode(adev, 1);
        res->add_ok = sendFrame(&p, offsetof(union MESAPI__ADD_QUEUE, api_status) / 4);
        rn_gc_safe_mode(adev, 0);
    }
    // The process path lets the MES pick the hardware queue itself (from the mask 0xf0): look for the HQD that carries our doorbell.
    u32 foundPipe = qPipe, foundQueue = qQueue;
    for (u32 pipe = 0; pipe < 4 && !(res->q_active & 1u); pipe++)
        for (u32 q = 0; q < 8; q++) {
            grbmSelect(adev, 1, pipe, 0, q);
            const u32 act = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE), db = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
            if ((act & 1u) && ((db >> 2) & 0x3ffffffu) == gMappedDoorbell) {
                res->q_active = act; res->q_doorbell_ctl = db; foundPipe = pipe; foundQueue = q;
                break;
            }
        }
    grbmSelect(adev, 0, 0, 0, 0);
    res->q_pipe = foundPipe; res->q_queue = foundQueue;
    res->fault_status = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_STATUS);
    res->fault_addr[0] = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_LO32);   // the address that faulted, with the status above
    res->fault_addr[1] = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_HI32);
    res->ring_fence_late = rn_vram_rd32(adev, ringFenceOff);
    res->rlc_sched = RREG32_SOC15(RN_GC, 0, regRLC_CP_SCHEDULERS);              // again, after the KIQ setting: Linux 0x000030e8
    res->cp_me_cntl = RREG32_SOC15(RN_GC, 0, regCP_ME_CNTL);
    res->api_status = rn_vram_rd32(adev, apiStatusOff);
    grbmSelect(adev, 3, 0, 0, 0);
    res->sched_version = RREG32_SOC15(RN_GC, 0, regCP_MES_GP3_LO);
    res->mepc = RREG32_SOC15(RN_GC, 0, regCP_MES_MEPC_LO);
    res->mcause = RREG32_SOC15(RN_GC, 0, regCP_MES_MCAUSE_LO);
    res->p0_pc_end = RREG32_SOC15(RN_GC, 0, regCP_MES_INSTR_PNTR);
    res->sched_rptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
    res->sched_wptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO);
    grbmSelect(adev, 3, 1, 0, 0);
    res->kiq_version = RREG32_SOC15(RN_GC, 0, regCP_MES_GP3_LO);
    grbmSelect(adev, 0, 0, 0, 0);
    res->step = 5;
    if ((res->q_active & 1u) || res->add_ok) { gMapped = true; gMappedWptr = 0; gMappedRingAt = kRingAt; gMappedRingBytes = kKcqRingBytes; gMappedPipe = foundPipe; gMappedQueue = foundQueue; }   // later rn_compute_submit_ib calls use this queue
    if (gMapped) gMesCtx = {true, vaBase, schedOff, qOff, ringFenceOff, apiStatusOff, schedWptr, seq, legDb, foundPipe, foundQueue, res->sched_version};
}

// Gets a hung legacy compute queue going again, the way Linux resets a kernel compute queue through the MES (gfx_v11_0_reset_kcq, mes_v11_0_reset_queue_mmio,
// amdgpu_gfx_mes_reset_queue_start): dequeue the hardware queue and reset its waves, have the MES remove it (REMOVE_QUEUE with unmap_legacy_queue and remove_queue_after_reset),
// put the MQD and the ring back as they were at boot and have the MES map it again (ADD_QUEUE with map_legacy_kq). Everything that was in the ring or running is lost. Returns 0, or
// -ENODEV (no mapped queue), -EIO with info[0] the step that failed: 1 the hardware queue did not go inactive, 2 REMOVE_QUEUE, 3 ADD_QUEUE, 4 the queue is not active afterwards, 5 it is mapped but takes no work (a second pass resets the MEC pipe first; info[2] then holds the first pass's failure, 5).
// After a bad access of the command processor itself (an IB that cannot be fetched) the dequeue and the MES frames can all succeed and the compute pipe still does not run
// anything: Linux then resets the MEC pipe (mes_v11_0_reset_compute_pipe_mmio). The pipe is held in reset, the hardware queues on it are cleared, and the pipe goes back
// to the start of its firmware; every queue on it is lost, here the one MES mapped.
static void mecPipeReset(struct rn_device *adev, u32 pipe) {
    u32 hold = RREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_CNTL);
    const u32 release = REG_SET_FIELD(hold, CP_MEC_RS64_CNTL, MEC_PIPE0_RESET, 0);
    hold = REG_SET_FIELD(hold, CP_MEC_RS64_CNTL, MEC_PIPE0_RESET, 1);          // pipe 0 is the only one RedNavi maps a queue on
    (void)pipe;
    WREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_CNTL, hold);
    for (u32 q = 0; q < 8; q++) {
        grbmSelect(adev, 1, 0, 0, q);
        WREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE, 0);
        WREG32_SOC15(RN_GC, 0, regCP_HQD_DEQUEUE_REQUEST, 0);
    }
    grbmSelect(adev, 0, 0, 0, 0);
    WREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_CNTL, release);
    msleep(5);
}

// Does the mapped queue run what it is given? A two-dword NOP into the ring and the read pointer word must follow the write pointer (the ring was just rebuilt, write pointer 0).
static bool mappedQueueAlive(struct rn_device *adev) {
    const u32 mmask = gMappedRingBytes / 4 - 1;
    const u64 data = gMesCtx.qOff + gMappedRingAt, wptrOff = gMesCtx.qOff + 0x1008;
    const u32 w = gMappedWptr;
    rn_vram_wr32(adev, data + ((w) & mmask) * 4, RN_PACKET3(RN_PACKET3_NOP, 0));
    rn_vram_wr32(adev, data + ((w + 1) & mmask) * 4, 0);
    gMappedWptr = w + 2;
    rn_vram_wr32(adev, wptrOff, w + 2); rn_vram_wr32(adev, wptrOff + 4, 0);
    (void)rn_vram_rd32(adev, data + ((w + 1) & mmask) * 4);
    __sync_synchronize();
    rn_hdp_flush(adev);
    *reinterpret_cast<volatile u64 *>(gDoorbellBase + gMappedDoorbell * 4) = w + 2;
    for (int i = 0; i < 100; i++) {
        if (rn_vram_rd32(adev, gMesCtx.qOff + 0x1000) == ((w + 2) & mmask)) return true;
        msleep(1);
    }
    return false;
}

static int queueResetPass(struct rn_device *adev, u32 info[4], bool pipeReset) {
    if (gSubmitLock) IOLockLock(gSubmitLock);                                // no submit while the ring and the wptr are rebuilt
    int rc = -EIO;
    rn_gc_safe_mode(adev, 1);
    if (pipeReset) mecPipeReset(adev, gMesCtx.pipe);
    grbmSelect(adev, 1, gMesCtx.pipe, 0, gMesCtx.queue);
    WREG32_SOC15(RN_GC, 0, regCP_HQD_DEQUEUE_REQUEST, 2);
    WREG32_SOC15(RN_GC, 0, regSPI_COMPUTE_QUEUE_RESET, 1);
    bool inactive = false;
    for (int i = 0; i < 100000; i++) {                                       // up to ~100 ms, as adev->usec_timeout
        if (!(RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE) & 1u)) { inactive = true; break; }
        udelay(1);
    }
    info[1] = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
    grbmSelect(adev, 0, 0, 0, 0);
    if (!inactive) { info[0] = 1; goto out; }
    {
        union MESAPI__REMOVE_QUEUE r;
        memset(&r, 0, sizeof(r));
        r.header.type = MES_API_TYPE_SCHEDULER;
        r.header.opcode = MES_SCH_API_REMOVE_QUEUE;
        r.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
        r.doorbell_offset = gMesCtx.doorbell;
        r.gang_context_addr = 0;
        r.pipe_id = gMesCtx.pipe;
        r.queue_id = gMesCtx.queue;
        r.unmap_legacy_queue = 1;
        r.queue_type = MES_QUEUE_TYPE_COMPUTE;
        if ((gMesCtx.schedVersion & 0xfffu) >= 0x60) r.remove_queue_after_reset = 1;
        u32 st = 0;
        if (!mesSendLate(adev, &r, offsetof(union MESAPI__REMOVE_QUEUE, api_status) / 4, &st)) { info[0] = 2; info[2] = st; goto out; }
    }
    // amdgpu_gfx_mqd_reset_restore: the MQD as it was built at boot, the ring empty, the read and write words back at 0
    writeComputeMqd(adev, gMesCtx.vaBase, gMesCtx.qOff, kKcqRingBytes, gMesCtx.qOff + 0x2000, gMesCtx.qOff + 0x4000, gMesCtx.doorbell, false, kRingAt);
    for (u32 o = 0; o < kKcqRingBytes; o += 4) rn_vram_wr32(adev, gMesCtx.qOff + kRingAt + o, 0);
    for (u32 o = 0; o < 16; o += 4) rn_vram_wr32(adev, gMesCtx.qOff + 0x1000 + o, 0);
    gMappedWptr = 0;
    {
        union MESAPI__ADD_QUEUE p;
        memset(&p, 0, sizeof(p));
        p.header.type = MES_API_TYPE_SCHEDULER;
        p.header.opcode = MES_SCH_API_ADD_QUEUE;
        p.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
        p.pipe_id = gMesCtx.pipe;
        p.queue_id = gMesCtx.queue;
        p.doorbell_offset = gMesCtx.doorbell;
        p.mqd_addr = gMesCtx.vaBase + gMesCtx.qOff + 0x2000;
        p.wptr_addr = gMesCtx.vaBase + gMesCtx.qOff + 0x1008;
        p.queue_type = MES_QUEUE_TYPE_COMPUTE;
        p.map_legacy_kq = 1;
        u32 st = 0;
        if (!mesSendLate(adev, &p, offsetof(union MESAPI__ADD_QUEUE, api_status) / 4, &st)) { info[0] = 3; info[2] = st; goto out; }
    }
    grbmSelect(adev, 1, gMesCtx.pipe, 0, gMesCtx.queue);
    info[3] = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
    grbmSelect(adev, 0, 0, 0, 0);
    if (!(info[3] & 1u)) { info[0] = 4; goto out; }
    if (!mappedQueueAlive(adev)) { info[0] = 5; goto out; }                // mapped and active, and still does not take work
    rc = 0;
out:
    rn_gc_safe_mode(adev, 0);
    if (gSubmitLock) IOLockUnlock(gSubmitLock);
    return rc;
}

int rn_compute_queue_reset(struct rn_device *adev, u32 info[4]) {
    memset(info, 0, 4 * sizeof(u32));
    if (!gMesCtx.valid || !gMapped) return -ENODEV;
    int rc = queueResetPass(adev, info, false);
    if (rc != 0 && info[0] == 5) {                                          // the queue is back but the pipe does not run it: reset the pipe and map again
        u32 first[4];
        memcpy(first, info, sizeof(first));
        rc = queueResetPass(adev, info, true);
        info[2] = first[0];                                                 // the failure of the first pass is kept for the caller to see
        if (rc == 0) info[0] = 0;
    }
    return rc;
}

void rn_kiq_map_compute(struct rn_device *adev, u64 vaBase, u64 kiqOff, u64 queueOff, u32 maxStep, void (*progress)(void *, u32),
                        void *progressCtx, bool rlcSched, u32 kiqPipe, u32 kiqQueue, struct rn_kiq_result *res) {
    memset(res, 0, sizeof(*res));
    const u32 kiqMe = 1, kiqDoorbellDword = 0;
    const u32 qMe = 1, qPipe = 0, qQueue = 3;
    const u32 ringBytes = 4096;

    // gfx_v11_0_kiq_setting: tell the RLC which queue is the KIQ
    if (progress) progress(progressCtx, 1);
    // Writing this register hung the machine (step 1 alone, 2026-10-30), so it is skipped unless rlcSched is set.
    u32 tmp;
    if (rlcSched) {
        tmp = RREG32_SOC15(RN_GC, 0, regRLC_CP_SCHEDULERS);
        tmp &= 0xffffff00u;
        tmp |= (kiqMe << 5) | (kiqPipe << 3) | kiqQueue;
        WREG32_SOC15(RN_GC, 0, regRLC_CP_SCHEDULERS, tmp | 0x80);
    }
    res->step = 1;
    if (maxStep < 2) return;

    // the KIQ itself: MQD image, then the registers
    if (progress) progress(progressCtx, 2);
    writeComputeMqd(adev, vaBase, kiqOff, ringBytes, kiqOff + 0x2000, kiqOff + 0x4000, kiqDoorbellDword);
    grbmSelect(adev, kiqMe, kiqPipe, 0, kiqQueue);
    programHqdFromMqd(adev, vaBase, kiqOff, ringBytes, kiqOff + 0x2000, kiqOff + 0x4000, kiqDoorbellDword);
    grbmSelect(adev, 0, 0, 0, 0);
    res->step = 2;
    if (maxStep < 3) return;

    // the compute queue's complete MQD (not activated through registers: the firmware will load it)
    if (progress) progress(progressCtx, 3);
    writeComputeMqd(adev, vaBase, queueOff, ringBytes, queueOff + 0x2000, queueOff + 0x4000, kMappedDoorbellDword);
    res->step = 3;
    if (maxStep < 4) return;

    // KIQ commands: SET_RESOURCES (which queues it may map), MAP_QUEUES (this one)
    u32 pk[24];
    u32 n = 0;
    pk[n++] = RN_PACKET3(0xA0u, 6);                                           // PACKET3_SET_RESOURCES
    pk[n++] = (0u << 0) | (0xau << 16) | (0u << 29);                         // vmid mask 0, unmap latency 0xa, queue type 0 (KIQ)
    pk[n++] = (1u << qQueue) | 0u; pk[n++] = 0;                              // queue mask: pipe 0 queue 3
    pk[n++] = 0; pk[n++] = 0; pk[n++] = 0; pk[n++] = 0;                      // no cleaner shader, oac, gds
    pk[n++] = RN_PACKET3(0xA2u, 5);                                           // PACKET3_MAP_QUEUES
    pk[n++] = (0u << 4) | (0u << 8) | (qQueue << 13) | (qPipe << 16) | (qMe << 18) | (0u << 21) | (0u << 24) | (0u << 26) | (1u << 29);
    pk[n++] = kMappedDoorbellDword << 2;
    const u64 mqdVa = vaBase + queueOff + 0x2000, wptrVa = vaBase + queueOff + 0x1008;
    pk[n++] = lower_32_bits(mqdVa); pk[n++] = upper_32_bits(mqdVa);
    pk[n++] = lower_32_bits(wptrVa); pk[n++] = upper_32_bits(wptrVa);
    // Pad to a multiple of 8 dwords with one NOP packet. (The earlier loop added two dwords at a time to an odd count, never reached a
    // multiple of 8, and ran off the end of pk[]: the kernel stack overflow of 2026-10-30.)
    u32 pad = (8u - (n & 7u)) & 7u;
    if (pad == 1) pad = 9;
    if (pad) {
        pk[n++] = RN_PACKET3(RN_PACKET3_NOP, pad - 2);
        for (u32 i = 1; i < pad; i++) pk[n++] = 0;
    }
    for (u32 i = 0; i < n; i++) rn_vram_wr32(adev, kiqOff + i * 4, pk[i]);
    if (progress) progress(progressCtx, 4);
    res->step = 4;
    *reinterpret_cast<volatile u64 *>(gDoorbellBase + kiqDoorbellDword * 4) = n;     // the KIQ's own doorbell (slot 0)

    for (int i = 0; i < 1000; i++) {                                         // until the firmware has loaded the queue
        grbmSelect(adev, qMe, qPipe, 0, qQueue);
        res->map_active = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
        grbmSelect(adev, 0, 0, 0, 0);
        if (res->map_active & 1u) break;
        msleep(1);
    }
    grbmSelect(adev, qMe, qPipe, 0, qQueue);
    res->map_pq_control = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_CONTROL);
    res->map_doorbell_ctl = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
    res->map_rptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
    res->map_wptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO);
    grbmSelect(adev, kiqMe, kiqPipe, 0, kiqQueue);
    res->kiq_active = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
    res->kiq_rptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
    res->kiq_wptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO);
    res->kiq_doorbell_ctl = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
    res->kiq_pq_control = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_CONTROL);
    res->kiq_pq_base = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_BASE);
    res->kiq_pq_status = RREG32_SOC15(RN_GC, 0, regCP_PQ_STATUS);
    res->cp_stat = RREG32_SOC15(RN_GC, 0, regCP_STAT);
    msleep(200);
    res->kiq_rptr_late = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
    grbmSelect(adev, 0, 0, 0, 0);
    res->ring_dw0 = rn_vram_rd32(adev, kiqOff);
    res->ring_dw1 = rn_vram_rd32(adev, kiqOff + 4);
    res->fault_status = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_STATUS);
    res->fault_addr_lo = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_LO32);
    res->ok = (res->map_active & 1u) != 0;
    if (res->ok) { gMapped = true; gMappedWptr = 0; }                        // later rn_compute_submit_ib calls use the mapped queue
}

}


// ---------------------------------------------------------------------------------------------------------------------------------------------------- graphics queue
// The kernel graphics queue of GC 11: ME 0, pipe 0, queue 0, doorbell index 0x8B (AMDGPU_NAVI10_DOORBELL_GFX_RING0) times two in dwords, as gfx_v11_0_gfx_ring_init has it.
// Linux maps it through the MES (amdgpu_gfx_enable_kgq: mes_v11_0_map_legacy_queue with queue_type AMDGPU_RING_TYPE_GFX), from an MQD that gfx_v11_0_gfx_mqd_init fills
// (struct v11_gfx_mqd: the dword numbers below are those of v11_structs.h), and then gfx_v11_0_cp_gfx_start sends the clear state preamble through the ring.
#ifndef CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK
#define CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT 0x2          // bits 27:2, gc_11_0_3_sh_mask.h
#define CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK 0x0FFFFFFCL
#endif
static const u32 kGfxRingBytes = 0x8000, kGfxDoorbell = 0x8B << 1;
static u32 gGfxWptr;                       // dwords written to the graphics ring so far (the ring is addressed by this counter modulo its size)
static u32 gGfxLastVmid = 0xffffffffu;      // the VMID of the last job: a different one is a context switch
static u64 gGfxBlk;                        // VRAM offset of the queue's block: rptr +0x1000, wptr +0x1008, MQD +0x2000, ring data +0x5000 (the same layout as a compute queue's)

static void writeGfxMqd(struct rn_device *adev, u64 vaBase, u64 blk, u32 ringBytes, u32 doorbellDword) {
    const u64 mqdOff = blk + 0x2000, ringOff = blk + kRingAt, rptrOff = blk + 0x1000, wptrOff = blk + 0x1008;
    const u64 ringVa = vaBase + ringOff;
    u32 qOrder = 0;
    while ((4u << qOrder) < ringBytes) qOrder++;                            // order_base_2(queue_size / 4)
    u32 mqdCtl = regCP_GFX_MQD_CONTROL_DEFAULT;
    mqdCtl = REG_SET_FIELD(mqdCtl, CP_GFX_MQD_CONTROL, VMID, 0);
    mqdCtl = REG_SET_FIELD(mqdCtl, CP_GFX_MQD_CONTROL, PRIV_STATE, 1);
    mqdCtl = REG_SET_FIELD(mqdCtl, CP_GFX_MQD_CONTROL, CACHE_POLICY, 0);
    u32 quantum = regCP_GFX_HQD_QUANTUM_DEFAULT;
    quantum = REG_SET_FIELD(quantum, CP_GFX_HQD_QUANTUM, QUANTUM_EN, 1);
    u32 hqdCntl = regCP_GFX_HQD_CNTL_DEFAULT;
    hqdCntl = REG_SET_FIELD(hqdCntl, CP_GFX_HQD_CNTL, RB_BUFSZ, qOrder - 1);
    hqdCntl = REG_SET_FIELD(hqdCntl, CP_GFX_HQD_CNTL, RB_BLKSZ, qOrder - 3);
    hqdCntl = REG_SET_FIELD(hqdCntl, CP_GFX_HQD_CNTL, KMD_QUEUE, 1);
    u32 db = regCP_RB_DOORBELL_CONTROL_DEFAULT;
    db = REG_SET_FIELD(db, CP_RB_DOORBELL_CONTROL, DOORBELL_OFFSET, doorbellDword);
    db = REG_SET_FIELD(db, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 1);
    for (u32 o = 0; o < 512 * 4; o += 4) rn_vram_wr32(adev, mqdOff + o, 0);
    for (u32 o = 0; o < ringBytes; o += 4) rn_vram_wr32(adev, ringOff + o, 0);
    for (u32 o = 0; o < 16; o += 4) rn_vram_wr32(adev, blk + 0x1000 + o, 0);
    struct { u32 dw; u32 v; } mqd[] = {
        {128, lower_32_bits(vaBase + mqdOff) & 0xfffffffcu}, {129, upper_32_bits(vaBase + mqdOff)},   // cp_mqd_base_addr
        {162, mqdCtl},                                                                                 // cp_gfx_mqd_control
        {131, 0},                                                                                      // cp_gfx_hqd_vmid
        {134, 0},                                                                                      // cp_gfx_hqd_queue_priority: low
        {135, quantum},
        {136, lower_32_bits(ringVa >> 8)}, {137, upper_32_bits(ringVa >> 8)},                          // cp_gfx_hqd_base
        {139, lower_32_bits(vaBase + rptrOff) & 0xfffffffcu}, {140, upper_32_bits(vaBase + rptrOff) & 0xFFFFu},   // cp_gfx_hqd_rptr_addr
        {141, lower_32_bits(vaBase + wptrOff) & 0xfffffffcu}, {142, upper_32_bits(vaBase + wptrOff) & 0xFFFFu},   // cp_rb_wptr_poll_addr
        {143, db},                                                                                     // cp_rb_doorbell_control
        {145, hqdCntl},                                                                                // cp_gfx_hqd_cntl
        {138, regCP_GFX_HQD_RPTR_DEFAULT},                                                             // cp_gfx_hqd_rptr
        {130, 1},                                                                                      // cp_gfx_hqd_active
    };
    for (u32 i = 0; i < sizeof(mqd) / sizeof(mqd[0]); i++) rn_vram_wr32(adev, mqdOff + mqd[i].dw * 4, mqd[i].v);
    (void)rn_vram_rd32(adev, mqdOff + 511 * 4);
    __sync_synchronize();
}

// Dwords to the graphics ring, the write pointer shadow, and the doorbell (a 64-bit write of the running dword count).
static void gfxRingEmit(struct rn_device *adev, const u32 *pk, u32 n) {
    const u32 mask = kGfxRingBytes / 4 - 1;
    const u64 data = gGfxBlk + kRingAt;
    const u32 w = gGfxWptr;
    for (u32 i = 0; i < n; i++) rn_vram_wr32(adev, data + ((w + i) & mask) * 4, pk[i]);
    gGfxWptr = w + n;
    rn_vram_wr32(adev, gGfxBlk + 0x1008, w + n); rn_vram_wr32(adev, gGfxBlk + 0x100C, 0);
    (void)rn_vram_rd32(adev, data + (((w + n - 1) & mask) * 4));
    __sync_synchronize();
    rn_hdp_flush(adev);
    *reinterpret_cast<volatile u64 *>(gDoorbellBase + kGfxDoorbell * 4) = w + n;
}

int rn_gfx_submit_ib(struct rn_device *adev, u64 ibVa, u32 ibDwords, u64 fenceVa, u32 fenceValue, u32 vmid) {
    if (!gGfxBlk) return -EINVAL;
    u32 pk[24];
    u32 n = 0;
    // What amdgpu_ib_schedule sends for a job on the graphics ring: FRAME_CONTROL start, CONTEXT_CONTROL (load enable, plus the load flags when the context changed since the
    // last job: gfx_v11_0_ring_emit_cntxcntl), the INDIRECT_BUFFER (no INDIRECT_BUFFER_VALID bit on the graphics ring: gfx_v11_0_ring_emit_ib_gfx), the fence, FRAME_CONTROL end.
    const bool ctxSwitch = gGfxLastVmid != vmid;
    gGfxLastVmid = vmid;
    pk[n++] = RN_PACKET3(0x90u, 0); pk[n++] = 0;                              // PACKET3_FRAME_CONTROL, FRAME_CMD(0) = start
    pk[n++] = RN_PACKET3(0x28u, 1); pk[n++] = 0x80000000u | (ctxSwitch ? (0x8001u | 0x01000000u | 0x10002u) : 0u); pk[n++] = 0;
    pk[n++] = RN_PACKET3(0x3Fu, 2);                                          // INDIRECT_BUFFER
    pk[n++] = lower_32_bits(ibVa) & 0xfffffffcu;
    pk[n++] = upper_32_bits(ibVa) & 0xFFFFu;
    pk[n++] = (ibDwords & 0xFFFFFu) | ((vmid & 0xFu) << 24);
    pk[n++] = RN_PACKET3(RN_PACKET3_RELEASE_MEM, 6);
    pk[n++] = (1u << 22) | (1u << 21) | (1u << 13) | (1u << 12) | (3u << 25) | RN_CACHE_FLUSH_AND_INV_TS_EVENT | (5u << 8);
    pk[n++] = (1u << 29);
    pk[n++] = lower_32_bits(fenceVa); pk[n++] = upper_32_bits(fenceVa);
    pk[n++] = fenceValue; pk[n++] = 0; pk[n++] = 0;
    pk[n++] = RN_PACKET3(0x90u, 0); pk[n++] = 1u << 28;                       // FRAME_CMD(1) = end
    u32 pad = (8u - (n & 7u)) & 7u;
    if (pad == 1) pad = 9;
    if (pad) {
        pk[n++] = RN_PACKET3(RN_PACKET3_NOP, pad - 2);
        for (u32 i = 1; i < pad; i++) pk[n++] = 0;
    }
    const u32 mask = kGfxRingBytes / 4 - 1;
    if (gSubmitLock) IOLockLock(gSubmitLock);
    int rc = 0;
    u32 spins = 0, rptr = rn_vram_rd32(adev, gGfxBlk + 0x1000);
    while (n > mask - ((gGfxWptr - rptr) & mask)) {                         // the ring is never filled completely, a full ring would look empty
        if (spins++ == 0) gRingFullWaits++;
        if (spins > 200000) { rc = -EAGAIN; break; }
        udelay(10);
        const u32 now = rn_vram_rd32(adev, gGfxBlk + 0x1000);
        if (now != rptr) { rptr = now; spins = 1; }
    }
    if (rc == 0) gfxRingEmit(adev, pk, n);
    if (gSubmitLock) IOLockUnlock(gSubmitLock);
    return rc;
}

void rn_gfx_queue_map(struct rn_device *adev, struct rn_gfxq_result *res) {
    memset(res, 0, sizeof(*res));
    if (!gMesCtx.valid || !gDoorbellBase) return;
    const u64 vaBase = gMesCtx.vaBase, blk = gMesCtx.qOff + 0x5C000;       // after the MES event log (base + 0x50000 .. 0x94000)
    gGfxBlk = blk;
    gGfxWptr = 0;
    rn_gc_safe_mode(adev, 1);
    // gfx_v11_0_cp_set_doorbell_range: the graphics doorbells, from gfx_ring0 to the end of the graphics user queues (0x100)
    WREG32_SOC15(RN_GC, 0, regCP_RB_DOORBELL_RANGE_LOWER, (0x8B * 2) << 2);
    WREG32_SOC15(RN_GC, 0, regCP_RB_DOORBELL_RANGE_UPPER, (0x100 * 2) << 2);
    res->step = 1;
    // The hand-programmed graphics ring test left its hardware queue active: the MES maps a free one.
    grbmSelect(adev, 0, 0, 0, 0);
    res->hqd_active_before = RREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_ACTIVE);
    WREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_ACTIVE, 0);
    for (int i = 0; i < 1000 && (RREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_ACTIVE) & 1u); i++) udelay(1);
    writeGfxMqd(adev, vaBase, blk, kGfxRingBytes, kGfxDoorbell);
    res->step = 2;
    {
        union MESAPI__ADD_QUEUE p;
        memset(&p, 0, sizeof(p));
        p.header.type = MES_API_TYPE_SCHEDULER;
        p.header.opcode = MES_SCH_API_ADD_QUEUE;
        p.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
        p.pipe_id = 0;
        p.queue_id = 0;
        p.doorbell_offset = kGfxDoorbell;
        p.mqd_addr = vaBase + blk + 0x2000;
        p.wptr_addr = vaBase + blk + 0x1008;
        p.queue_type = MES_QUEUE_TYPE_GFX;
        p.map_legacy_kq = 1;
        u32 st = 0;
        res->add_ok = mesSendLate(adev, &p, offsetof(union MESAPI__ADD_QUEUE, api_status) / 4, &st);
        res->add_status = st;
    }
    res->step = 3;
    grbmSelect(adev, 0, 0, 0, 0);
    res->hqd_active = RREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_ACTIVE);
    res->hqd_cntl = RREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_CNTL);
    res->hqd_base = RREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_BASE);
    res->mqd_base = RREG32_SOC15(RN_GC, 0, regCP_GFX_MQD_BASE_ADDR);
    res->doorbell_ctl = RREG32_SOC15(RN_GC, 0, regCP_RB_DOORBELL_CONTROL);
    res->cp_stat = RREG32_SOC15(RN_GC, 0, regCP_STAT);
    res->me_cntl = RREG32_SOC15(RN_GC, 0, regCP_ME_CNTL);
    if (!(res->hqd_active & 1u)) goto out;
    res->step = 4;
    {   // gfx_v11_0_cp_gfx_start: the clear state, the default values of all context registers (gfx11_csb.h), between the preamble packets, then CLEAR_STATE, which makes the
        // command processor load them into every context it creates. Without the defaults a new context starts with every register 0 and a draw produces nothing.
        const u32 head[] = {RN_PACKET3(0x4Au, 0), 2u << 28,                     // PREAMBLE_CNTL: begin clear state
                            RN_PACKET3(0x28u, 1), 0x80000000u, 0x80000000u};    // CONTEXT_CONTROL
        gfxRingEmit(adev, head, sizeof(head) / sizeof(head[0]));
        for (u32 e = 0; e < sizeof(kGfx11CsbExtents) / sizeof(kGfx11CsbExtents[0]); e++) {
            const u32 hdr[] = {RN_PACKET3(0x69u, kGfx11CsbExtents[e].count), kGfx11CsbExtents[e].first_reg - 0xA000u};   // SET_CONTEXT_REG
            gfxRingEmit(adev, hdr, 2);
            gfxRingEmit(adev, kGfx11CsbExtents[e].values, kGfx11CsbExtents[e].count);
        }
        const u32 tail[] = {RN_PACKET3(0x69u, 1), 0xA1ECu - 0xA000u, 0,           // SET_CONTEXT_REG PA_SC_TILE_STEERING_OVERRIDE = 0 (gfx.config.pa_sc_tile_steering_override)
                            RN_PACKET3(0x4Au, 0), 3u << 28,                       // PREAMBLE_CNTL: end clear state
                            RN_PACKET3(0x12u, 0), 0};                             // CLEAR_STATE
        gfxRingEmit(adev, tail, sizeof(tail) / sizeof(tail[0]));
        res->csb_dwords = 5 + 934 + 2 * 7 + sizeof(tail) / sizeof(tail[0]);
    }
    {   // the ring test of Linux: SET_UCONFIG_REG SCRATCH_REG0
        const u32 scratchDw = 0xc000u + 0x40u;
        WREG32_SOC15(RN_GC, 0, regSCRATCH_REG0, 0xCAFEDEADu);
        const u32 pk[] = {RN_PACKET3(RN_PACKET3_SET_UCONFIG_REG, 1), scratchDw - RN_PACKET3_SET_UCONFIG_REG_START, 0xDEADBEEFu};
        gfxRingEmit(adev, pk, 3);
        for (int i = 0; i < 2000; i++) { if (RREG32_SOC15(RN_GC, 0, regSCRATCH_REG0) == 0xDEADBEEFu) break; udelay(10); }
        res->scratch = RREG32_SOC15(RN_GC, 0, regSCRATCH_REG0);
    }
    if (res->scratch != 0xDEADBEEFu) goto out;
    res->step = 5;
    {   // WRITE_DATA to memory (ME, DST_SEL 5, WR_CONFIRM)
        const u64 target = vaBase + blk + 0x1010;
        rn_vram_wr32(adev, blk + 0x1010, 0xCAFEDEADu);
        const u32 pk[] = {RN_PACKET3(0x37u, 3), (5u << 8) | (1u << 20), lower_32_bits(target), upper_32_bits(target), 0x600DC0DEu};
        gfxRingEmit(adev, pk, 5);
        for (int i = 0; i < 2000; i++) { if (rn_vram_rd32(adev, blk + 0x1010) == 0x600DC0DEu) break; udelay(10); }
        res->write_data = rn_vram_rd32(adev, blk + 0x1010);
    }
    if (res->write_data != 0x600DC0DEu) goto out;
    res->step = 6;
    {   // RELEASE_MEM, as the compute submit writes its fence
        const u64 fenceVa = vaBase + blk + 0x1018;
        rn_vram_wr32(adev, blk + 0x1018, 0);
        const u32 pk[] = {RN_PACKET3(RN_PACKET3_RELEASE_MEM, 6),
                          (1u << 22) | (1u << 21) | (1u << 13) | (1u << 12) | (3u << 25) | RN_CACHE_FLUSH_AND_INV_TS_EVENT | (5u << 8), (1u << 29),
                          lower_32_bits(fenceVa), upper_32_bits(fenceVa), 0x77u, 0, 0};
        gfxRingEmit(adev, pk, 8);
        for (int i = 0; i < 2000; i++) { if (rn_vram_rd32(adev, blk + 0x1018) == 0x77u) break; udelay(10); }
        res->fence = rn_vram_rd32(adev, blk + 0x1018);
    }
    if (res->fence == 0x77u) res->step = 7;
out:
    grbmSelect(adev, 0, 0, 0, 0);
    res->rptr = RREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_RPTR);
    res->wptr = RREG32_SOC15(RN_GC, 0, regCP_GFX_HQD_WPTR);
    res->rptr_word = rn_vram_rd32(adev, blk + 0x1000);
    res->fault_status = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_STATUS);
    res->fault_lo = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_LO32);
    res->fault_hi = RREG32_SOC15(RN_GC, 0, regGCVM_L2_PROTECTION_FAULT_ADDR_HI32);
    rn_gc_safe_mode(adev, 0);
}
