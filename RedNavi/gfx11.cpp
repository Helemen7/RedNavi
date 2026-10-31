#include "gfx11.h"

#include <string.h>

#include <linux/delay.h>
#include "gc_regs.h"
#include "mes_v11_api_def.h"
#include "osssys_regs.h"
#include "nbio_regs.h"

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

int rn_gart_flush(struct rn_device *adev) {
    u32 req = 0;                                                             // gfxhub_v3_0_3_get_invalidate_req(vmid 0, flush type 0)
    req = REG_SET_FIELD(req, GCVM_INVALIDATE_ENG17_REQ, PER_VMID_INVALIDATE_REQ, 1);
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
        if (RREG32_SOC15(RN_GC, 0, regGCVM_INVALIDATE_ENG17_ACK) & 1u) { rc = 0; break; }
        udelay(1);
    }
    // The engine acknowledged but a changed PTE was still used on hardware (gart-dynamic test): also do what rn_gfxhub_enable does to
    // start from empty caches, pulse the L1 TLB and L2 cache invalidation in GCVM_L2_CNTL2.
    u32 tmp = RREG32_SOC15(RN_GC, 0, regGCVM_L2_CNTL2);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS, 1);
    tmp = REG_SET_FIELD(tmp, GCVM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
    WREG32_SOC15(RN_GC, 0, regGCVM_L2_CNTL2, tmp);
    udelay(100);
    return rc;
}

int rn_gart_update_page(struct rn_device *adev, const struct rn_gmc *gmc, u32 page, u64 bus) {
    if (bus) {
        rn_gart_map_page(adev, gmc, page, bus);
    } else {
        const u64 slot = gmc->pt_vram_offset + (u64)page * 8;
        rn_vram_wr32(adev, slot, 0); rn_vram_wr32(adev, slot + 4, 0);
    }
    return rn_gart_flush(adev);
}

void rn_gart_map_page(struct rn_device *adev, const struct rn_gmc *gmc, u32 page, u64 bus) {
    // AMDGPU_PTE_VALID | SYSTEM | SNOOPED | EXECUTABLE | READABLE | WRITEABLE, address in bits 47:12. EXECUTABLE matters: the command
    // processor fetching an indirect buffer, or the shader array fetching instructions, from a page without it takes a permission fault.
    const u64 pte = (bus & 0x0000FFFFFFFFF000ull) | 0x1ull | 0x2ull | 0x4ull | 0x10ull | 0x20ull | 0x40ull;
    const u64 slot = gmc->pt_vram_offset + (u64)page * 8;
    rn_vram_wr32(adev, slot, lower_32_bits(pte));
    rn_vram_wr32(adev, slot + 4, upper_32_bits(pte));
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
static u32 gMappedPipe = 0;
static u32 gMappedQueue = 3;           // queue index (ME 1, pipe 0) of the mapped queue: 3 through the KIQ, 4 through the MES
static const u32 kMappedDoorbellDword = 8;
static u32 gMappedDoorbell = kMappedDoorbellDword;   // doorbell dword of the mapped queue: 8 through the KIQ, in the user queue range (26..276) for a MES process queue
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

int rn_compute_submit_ib(struct rn_device *adev, u64 vaBase, u64 ringOff, u32 ringBytes, u64 ibVa, u32 ibDwords, u64 fenceVa, u32 fenceValue) {
    (void)vaBase;
    const u32 mask = ringBytes / 4 - 1;
    const u64 wptrOff = ringOff + 0x1008;
    u32 pk[24];
    u32 n = 0;
    // gfx_v11_0_ring_emit_ib_compute: INDIRECT_BUFFER, address (dword aligned), upper 16 bits, size | VALID | VMID << 24
    pk[n++] = RN_PACKET3(0x3Fu, 2);
    pk[n++] = lower_32_bits(ibVa) & 0xfffffffcu;
    pk[n++] = upper_32_bits(ibVa) & 0xFFFFu;
    pk[n++] = (1u << 23) | (ibDwords & 0xFFFFFu);
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
        for (u32 i = 0; i < n; i++) rn_vram_wr32(adev, ringOff + ((w + i) & mask) * 4, pk[i]);
        gMappedWptr = w + n;
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
bool gMesPostMap = false;
bool gMesSelfMap = false;
bool gMesEarlyMap = false;
u64 gMesHostWbVa = 0;            // rednavi_mes_hostwb=1: GART address of a host page that holds the legacy queue's rptr (+0) and wptr (+8), as Linux's write-back slots do
volatile u32 *gMesHostWbMem = nullptr;
u64 gMesHostRingVa = 0;          // rednavi_mes_hostring=1: the legacy queue's 8 KB ring in host memory too (GART address), as Linux allocates it
volatile u32 *gMesHostRingMem = nullptr;
static bool gWbHostNow = false;  // set around the one writeComputeMqd call that builds that queue

static void writeComputeMqd(struct rn_device *adev, u64 vaBase, u64 ringOff, u32 ringBytes, u64 mqdOff, u64 eopOff, u32 doorbellDword,
                            bool mesQueue = false) {
    const bool hostRing = gWbHostNow && gMesHostRingVa != 0 && gMesHostRingMem != nullptr;
    const u64 ringVa = hostRing ? gMesHostRingVa : vaBase + ringOff, rptrOff = ringOff + 0x1000, wptrOff = ringOff + 0x1008;
    const bool hostWb = gWbHostNow && gMesHostWbVa != 0 && gMesHostWbMem != nullptr;
    const u64 rptrVa = hostWb ? gMesHostWbVa : vaBase + rptrOff, wptrVa = hostWb ? gMesHostWbVa + 8 : vaBase + wptrOff;
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
    if (hostRing) for (u32 o = 0; o < ringBytes; o += 4) gMesHostRingMem[o / 4] = 0;
    else for (u32 o = 0; o < ringBytes; o += 4) rn_vram_wr32(adev, ringOff + o, 0);
    for (u32 o = 0; o < 2048; o += 4) rn_vram_wr32(adev, eopOff + o, 0);
    rn_vram_wr32(adev, rptrOff, 0); rn_vram_wr32(adev, rptrOff + 4, 0);
    rn_vram_wr32(adev, wptrOff, 0); rn_vram_wr32(adev, wptrOff + 4, 0);
    if (hostWb) for (u32 i = 0; i < 4; i++) gMesHostWbMem[i] = 0;
    struct { u32 dw; u32 v; } mqd[] = {
        {0, 0xC0310800u}, {11, 1}, {23, 0xffffffffu}, {24, 0xffffffffu}, {26, 0xffffffffu}, {27, 0xffffffffu}, {32, 7},
        {165, lower_32_bits(eopBase)}, {166, upper_32_bits(eopBase)}, {167, eopCtl}, {143, doorbellCtl},
        {128, lower_32_bits(vaBase + mqdOff) & 0xfffffffcu}, {129, upper_32_bits(vaBase + mqdOff)}, {162, mqdCtl},
        {136, lower_32_bits(ringVa >> 8)}, {137, upper_32_bits(ringVa >> 8)}, {145, pqCtl},
        {139, lower_32_bits(rptrVa) & 0xfffffffcu}, {140, upper_32_bits(rptrVa) & 0xFFFFu},
        {141, lower_32_bits(wptrVa) & 0xfffffffcu}, {142, upper_32_bits(wptrVa) & 0xFFFFu},
        {132, persist}, {149, ibCtl}, {135, quantum}, {130, 1},
    };
    for (u32 i = 0; i < sizeof(mqd) / sizeof(mqd[0]); i++) rn_vram_wr32(adev, mqdOff + mqd[i].dw * 4, mqd[i].v);
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
static const u32 kMesRingBytes = 4096;
static const u32 kMesDbgBytes = 0x4000;   // mes_dbgext buffer, at base + 0x50000
static const u32 kMesDbStart = 1024;      // mes->db_start_dw_offset: the page after the kernel doorbells (amdgpu_doorbell_create_kernel_doorbells)
static const u32 kMesKiqDoorbell = 0x0C << 1, kMesSchedDoorbell = 0x0B << 1;      // AMDGPU_NAVI10_DOORBELL_MES_RING1/0, in dwords

// mes_v11_0_queue_init_register: one MES queue into the HQD registers of (ME 3, pipe).
static void mesQueueInitRegister(struct rn_device *adev, u64 vaBase, u64 ringOff, u32 pipe, u64 mqdOff, u32 doorbellDword) {
    const u64 ringVa = vaBase + ringOff, rptrOff = ringOff + 0x1000, wptrOff = ringOff + 0x1008;
    u32 qOrder = 0;
    while ((4u << qOrder) < kMesRingBytes) qOrder++;
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

// Append one 64-dword API frame (or any dword run) to a MES ring at the monitonic write pointer, publish the pointer and ring the doorbell.
static void mesRingSubmit(struct rn_device *adev, u64 ringOff, u64 wptrShadowOff, u32 doorbellDword, u64 *wptr, const u32 *dw, u32 n) {
    const u32 ringDwords = kMesRingBytes / 4;
    for (u32 i = 0; i < n; i++) rn_vram_wr32(adev, ringOff + ((*wptr + i) % ringDwords) * 4, dw[i]);
    *wptr += n;
    rn_vram_wr32(adev, wptrShadowOff, lower_32_bits(*wptr));
    rn_vram_wr32(adev, wptrShadowOff + 4, upper_32_bits(*wptr));
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

bool gMesDmDump = false;
u32 gMesDm[2][256];
u32 gMesMqd[512];
u32 gMesPipeRegs[2][32];
const u32 kMesPipeRegDw[32] = {0xc800,0xc89d,0xc801,0xc802,0xc807,0xc80e,0xc813,0xc814,0xc815,0xc816,0xc817,0xc818,0xc81a,0xc81c,0xc81e,0xc820,0xc837,0xc843,0xc844,0xc845,0xc846,0xc847,0xc848,0xc849,0xc84a,0xc84b,0xc84c,0xf850,0xf851,0xf852,0xf854,0xf855};
u32 gMesKiqRing[96];
bool gMesCleaner = false;
bool gMesHwResLinux = false;
bool gMesDbgExt = false;
u64 gMesDbgHostVa = 0;
void *gMesDbgHostMem = nullptr;
u32 gMesDbg[2][256];

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
    mesRingSubmit(adev, kiqOff, kiqOff + 0x1008, kMesKiqDoorbell, kiqWptr, pk, 8);
    for (int i = 0; i < 100; i++) {
        if (RREG32_SOC15(RN_GC, 0, regSCRATCH_REG0) == 0xDEADBEEFu) break;
        msleep(1);
    }
    *scratch = RREG32_SOC15(RN_GC, 0, regSCRATCH_REG0);
    grbmSelect(adev, 3, 1, 0, 0);
    *pc = RREG32_SOC15(RN_GC, 0, regCP_MES_INSTR_PNTR);
    grbmSelect(adev, 0, 0, 0, 0);
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
    const u64 kiqOff = base, schedOff = base + 0x10000, sharedOff = base + 0x20000, qOff = base + 0x30000;
    const u64 schCtxOff = sharedOff, queryFenceOff = sharedOff + 0x10, ringFenceOff = sharedOff + 0x20, apiStatusOff = sharedOff + 0x100;
    const u32 qPipe = 0, qQueue = 0;   // a kernel-owned queue (outside the MES hqd mask 0xc), as Linux maps its first compute ring
    const u32 legDb = 6;               // mec_ring0 << 1

    // step 1: enable (mes_v11_0_enable): reset both pipes, give each its program counter, then un-halt and activate both
    if (progress) progress(ctx, 1);
    const u64 logOff = base + 0x40000, logVa = vaBase + logOff;           // MES event log (amdgpu_mes_log_enable): history in the first half, scratch in the second
    for (u32 o = 0; o < 0x8000; o += 4) rn_vram_wr32(adev, logOff + o, 0);
    if (!gMesHwResLinux) {                                  // rednavi_mes_hwres=1: as Linux without mes_log_enable, MSCRATCH stays 0 (the firmware enables more interrupts when it is set)
        WREG32_SOC15(RN_GC, 0, regCP_MES_MSCRATCH_LO, lower_32_bits(logVa + 0x4000));
        WREG32_SOC15(RN_GC, 0, regCP_MES_MSCRATCH_HI, upper_32_bits(logVa + 0x4000));
    }
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
    writeComputeMqd(adev, vaBase, kiqOff, kMesRingBytes, kiqOff + 0x2000, kiqOff + 0x4000, kMesKiqDoorbell, true);
    mesQueueInitRegister(adev, vaBase, kiqOff, 1, kiqOff + 0x2000, kMesKiqDoorbell);
    grbmSelect(adev, 3, 1, 0, 0);
    res->kiq_active = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
    res->kiq_doorbell_ctl = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
    grbmSelect(adev, 0, 0, 0, 0);
    res->step = 2;
    if (maxStep < 3) return;

    // step 3: the scheduler's MQD, then MAP_QUEUES on the KIQ ring (MES engine, ME field 2, engine select 5) loads it into pipe 0
    if (progress) progress(ctx, 3);
    writeComputeMqd(adev, vaBase, schedOff, kMesRingBytes, schedOff + 0x2000, schedOff + 0x4000, kMesSchedDoorbell, true);
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
        mesRingSubmit(adev, kiqOff, kiqOff + 0x1008, kMesKiqDoorbell, &kiqWptr, pk, 8);
    }
    for (int i = 0; i < 1000; i++) {                                                   // until the firmware has loaded the scheduler queue
        grbmSelect(adev, 3, 0, 0, 0);
        res->sched_active = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
        grbmSelect(adev, 0, 0, 0, 0);
        if (res->sched_active & 1u) break;
        msleep(1);
    }
    grbmSelect(adev, 3, 1, 0, 0);
    res->kiq_rptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
    res->kiq_doorbell_ctl = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
    grbmSelect(adev, 0, 0, 0, 0);
    mesPcSample(adev, 0, &res->p0_pc, &res->p0_pc10);
    mesPcSample(adev, 1, &res->p1_pc, &res->p1_pc10);
    mesKiqRingTest(adev, kiqOff, &kiqWptr, &res->ktest[0], &res->ktest[1]);   // the KIQ right after it mapped the scheduler queue
    res->step = 3;
    if (maxStep < 4 || !(res->sched_active & 1u)) return;

    if (gMesEarlyMap) {
        // rednavi_mes_earlymap=1: the KIQ's MAP_QUEUES for a MEC queue with no scheduler involved (it has not been given any frame yet). If the HQD of ME 1
        // pipe 0 queue 0 comes up here, the KIQ firmware, the MQD and the MEC are fine and what differs is the scheduler's request.
        gWbHostNow = false;
        writeComputeMqd(adev, vaBase, qOff, 4096, qOff + 0x2000, qOff + 0x4000, legDb);
        u32 pk[24];
        memset(pk, 0, sizeof(pk));
        pk[0] = RN_PACKET3(0xA0u, 6);                                      // SET_RESOURCES as amdgpu_gfx_mes_enable_kcq sends it
        pk[1] = (0xau << 16);
        pk[2] = 0xffffffffu; pk[3] = 0xffffffffu;
        pk[8] = RN_PACKET3(0xA2u, 5);                                      // MAP_QUEUES: one queue, compute engine, ME 1, pipe 0, queue 0
        pk[9] = 1u << 29;
        pk[10] = legDb << 2;
        pk[11] = lower_32_bits(vaBase + qOff + 0x2000); pk[12] = upper_32_bits(vaBase + qOff + 0x2000);
        pk[13] = lower_32_bits(vaBase + qOff + 0x1008); pk[14] = upper_32_bits(vaBase + qOff + 0x1008);
        pk[15] = RN_PACKET3(0x10u, 7);                                     // NOP to the 8-dword boundary
        mesRingSubmit(adev, kiqOff, kiqOff + 0x1008, kMesKiqDoorbell, &kiqWptr, pk, 24);
        msleep(50);
        grbmSelect(adev, 3, 1, 0, 0);
        res->early[0] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
        grbmSelect(adev, 1, 0, 0, 0);
        res->early[1] = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
        res->early[2] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_BASE);
        res->early[3] = RREG32_SOC15(RN_GC, 0, regCP_MQD_BASE_ADDR);
        res->early[4] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
        res->early[5] = static_cast<u32>(kiqWptr);
        grbmSelect(adev, 0, 0, 0, 0);
    }

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
        mesRingSubmit(adev, schedOff, schedOff + 0x1008, kMesSchedDoorbell, &schedWptr, f, API_FRAME_SIZE_IN_DWORDS);
        mesRingSubmit(adev, schedOff, schedOff + 0x1008, kMesSchedDoorbell, &schedWptr, reinterpret_cast<const u32 *>(&q), API_FRAME_SIZE_IN_DWORDS);
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
    const u64 dbgOff = base + 0x50000;
    const bool dbgHost = gMesDbgHostVa != 0 && gMesDbgHostMem != nullptr;
    const u64 dbgVa = dbgHost ? gMesDbgHostVa : vaBase + dbgOff;
    auto dumpMesDbg = [&]() {
        if (!gMesDbgExt) return;
        if (dbgHost) {                                     // the first 2 KB, out of the host mapping instead of VRAM
            const volatile u32 *p = static_cast<const volatile u32 *>(gMesDbgHostMem);
            for (u32 i = 0; i < 512; i++) gMesDbg[i >> 8][i & 0xff] = p[i];
            return;
        }
        for (u32 i = 0; i < 512; i++) gMesDbg[i >> 8][i & 0xff] = rn_vram_rd32(adev, dbgOff + i * 4);
    };

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
        p.disable_mes_log = gMesHwResLinux ? 1 : 0;
        if (gMesHwResLinux) {                              // the three fields mes_v11_0_set_hw_resources (Linux 7.0) sets and RedNavi left at 0
            p.gds_size = 0x1000;                           // adev->gds.gds_size for GFX11 (gfx_v11_0_set_gds_init)
            p.oversubscription_timer = 50;
        }
        if (!gMesHwResLinux) {                             // Linux without amdgpu.mes_log_enable (the default) sends neither; they also change the scheduler's CP_MES_MIE (0x31ffe against 0x1ffe)
            p.enable_mes_event_int_logging = 1;
            p.event_intr_history_gpu_mc_ptr = logVa;
        }
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

    // 4b: arm the log here, as Linux does at the end of mes_v11_0_hw_init: after both resource frames, before the first queue frame, so the
    // records of the ADD_QUEUE attempt are in it. The options word is not what the v11 header suggests: MES_DBGEXT_INIT_DATA defines only
    // bit 0 (one host interrupt per record), yet Linux never sends 0. amdgpu_mes_dbgext_start translates its own options into the layout the
    // firmware reads, where bit 1 enables logging and bit 0 is host_poll_msg with the sense inverted, and passes 0x3 when it polls, 0x2 when
    // it takes interrupts. 0x3 is what is sent here: nothing in RedNavi services the MES host interrupt, and reading the buffer afterwards is
    // the poll. Note that the VRAM runs of 2026-10-31 already sent 0x3 and still had the buffer untouched.
    if (gMesDbgExt) {
        if (dbgHost) {
            volatile u32 *p = static_cast<volatile u32 *>(gMesDbgHostMem);
            for (u32 i = 0; i < kMesDbgBytes / 4; i++) p[i] = 0;
            p[0] = kMesDbgBytes;                           // the size in the first dword, as amdgpu_mes_dbgext_start seeds it
        } else {
            for (u32 o = 0; o < kMesDbgBytes; o += 4) rn_vram_wr32(adev, dbgOff + o, 0);
            rn_vram_wr32(adev, dbgOff, kMesDbgBytes);
        }
        union MESAPI__MISC m;
        memset(&m, 0, sizeof(m));
        m.header.type = MES_API_TYPE_SCHEDULER;
        m.header.opcode = MES_SCH_API_MISC;
        m.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
        m.opcode = MESAPI_MISC__SETUP_MES_DBGEXT;
        m.dbgext_init_data.dbg_ext_mc_addr = dbgVa;
        m.dbgext_init_data.u64_all = 3;
        res->dbg_ok = sendFrame(&m, offsetof(union MESAPI__MISC, api_status) / 4);
        res->dbg_status = res->api_status;
    }
    mesPcSample(adev, 0, &res->p0_pc, &res->p0_pc10);
    res->step = 4;
    dumpMesDbg();                                          // the records of the setup, even when the run stops at this step
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
    for (u32 pipe = 0; pipe < 4; pipe++) { grbmSelect(adev, 1, pipe, 0, 0); res->mec_pc[pipe] = RREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_INSTR_PNTR); }
    grbmSelect(adev, 0, 0, 0, 0);
    gMappedDoorbell = legDb;
    gWbHostNow = true;
    writeComputeMqd(adev, vaBase, qOff, gMesHostRingVa != 0 ? 8192 : 4096, qOff + 0x2000, qOff + 0x4000, legDb);
    gWbHostNow = false;
    {
        // amdgpu_gfx_mes_enable_kcq: SET_RESOURCES on the KIQ ring with every queue in the mask comes before the legacy queue is mapped
        // Dwords 4 and 5 are the cleaner shader's MC address >> 8. Linux 7.0 enables the cleaner shader on GC 11.0.3 with these firmware versions
        // (gfx_v11_0_sw_init: ME >= 2280, PFP >= 2370, MEC >= 2450, MES >= 99) and sends its address here; RedNavi sent 0. The scheduler does touch
        // the cleaner path in ADD_QUEUE (with the SET_HW_RESOURCES_1 fence reading 0xffffffff it spun there), so a null shader is a suspect.
        // Linux's shader binary is not copied: a single s_endpgm (GFX11 encoding 0xbfb00000) is enough to see whether the address matters.
        u64 cleanerMc = 0;
        if (gMesCleaner) {
            const u64 shOff = sharedOff + 0x1800;                          // 256-byte aligned, clear of the fence area (+0x200..+0x1200) and the contexts (+0x2000)
            for (u32 o = 0; o < 0x100; o += 4) rn_vram_wr32(adev, shOff + o, 0xbfb00000u);
            cleanerMc = (vaBase + shOff) >> 8;
        }
        res->parity[0] = lower_32_bits(cleanerMc);
        res->parity[1] = (gMesCleaner ? 1u : 0u) | (gMesHwResLinux ? 2u : 0u);
        u32 pk[8];
        pk[0] = RN_PACKET3(0xA0u, 6);
        pk[1] = (0u << 0) | (0xau << 16) | (0u << 29);
        pk[2] = 0xffffffffu; pk[3] = 0xffffffffu; pk[4] = lower_32_bits(cleanerMc); pk[5] = upper_32_bits(cleanerMc); pk[6] = 0; pk[7] = 0;
        mesRingSubmit(adev, kiqOff, kiqOff + 0x1008, kMesKiqDoorbell, &kiqWptr, pk, 8);
        msleep(20);
        mesKiqRingTest(adev, kiqOff, &kiqWptr, &res->ktest[2], &res->ktest[3]);   // and after SET_RESOURCES, as amdgpu_gfx_mes_enable_kcq does
        grbmSelect(adev, 3, 1, 0, 0);
        res->kiq_rptr = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);       // 0x10 once both the MAP_QUEUES and the SET_RESOURCES are consumed
        for (u32 i = 0; i < 2; i++) { res->kiq_pc_pre[i] = RREG32_SOC15(RN_GC, 0, regCP_MES_INSTR_PNTR); udelay(47); }   // the KIQ pipe idle, SET_RESOURCES consumed
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
        for (u32 k = 0; k < 6; k++) rn_gart_map_vram_page(adev, gmc, gartPage + k, vaBase + qOff + k * 4096ull);
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
        p.wptr_addr = (gMesHostWbVa != 0 && gMesHostWbMem != nullptr) ? gMesHostWbVa + 8 : vaBase + qOff + 0x1008;
        p.queue_type = MES_QUEUE_TYPE_COMPUTE;
        p.map_legacy_kq = 1;
        rn_gc_safe_mode(adev, 1);
        res->add_ok = sendFrame(&p, offsetof(union MESAPI__ADD_QUEUE, api_status) / 4);
        rn_gc_safe_mode(adev, 0);
    }
    if (gMesPostMap && res->add_ok) {
        // rednavi_mes_post=1: the first thing done with the queue the MES has just mapped, before anything else touches it: a WRITE_DATA to VRAM through its
        // ring and the doorbell, then the doorbell control register at growing delays. The hand-programmed queues died at their first doorbell with the enable
        // bit cleared; this shows whether a queue the MES mapped does the same, and when.
        const u64 target = qOff + 0x6000;
        rn_vram_wr32(adev, target, 0xCAFEDEADu);
        u32 pk[16];
        memset(pk, 0, sizeof(pk));
        pk[0] = RN_PACKET3(0x37u, 3);                                      // WRITE_DATA, memory destination, write confirm
        pk[1] = (1u << 20) | (5u << 8);
        pk[2] = lower_32_bits(vaBase + target); pk[3] = upper_32_bits(vaBase + target);
        pk[4] = 0x600DC0DEu;
        pk[5] = RN_PACKET3(0x10u, 9);                                      // NOP to 16 dwords
        for (u32 i = 0; i < 16; i++) rn_vram_wr32(adev, qOff + i * 4, pk[i]);
        rn_vram_wr32(adev, qOff + 0x1008, 16); rn_vram_wr32(adev, qOff + 0x100c, 0);
        grbmSelect(adev, 1, 0, 0, 0);
        res->post[0] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);                  // before the doorbell
        grbmSelect(adev, 0, 0, 0, 0);
        *reinterpret_cast<volatile u64 *>(gDoorbellBase + legDb * 4) = 16;
        for (int i = 0; i < 200 && rn_vram_rd32(adev, target) != 0x600DC0DEu; i++) msleep(1);
        res->post[1] = rn_vram_rd32(adev, target);                                              // 0x600DC0DE if the queue ran it
        grbmSelect(adev, 1, 0, 0, 0);
        res->post[2] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
        res->post[3] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
        res->post[4] = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
        grbmSelect(adev, 0, 0, 0, 0);
        static const u32 waits[] = {50, 450, 1500, 3000, 5000};                                 // ms, so sampled at 0.05, 0.5, 2, 5, 10 s after the doorbell
        for (u32 w = 0; w < 5; w++) {
            msleep(waits[w]);
            grbmSelect(adev, 1, 0, 0, 0);
            res->post[5 + w] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
            grbmSelect(adev, 0, 0, 0, 0);
        }
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
    msleep(1000);
    res->ring_fence_late = rn_vram_rd32(adev, ringFenceOff);
    // The MES KIQ queue (ME 3 pipe 1 queue 0) after the failed map: did the scheduler hand it anything? Its HQD pointers and the ring after our own packets.
    grbmSelect(adev, 3, 1, 0, 0);
    res->kiq_end[0] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO);
    res->kiq_end[1] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
    res->kiq_end[2] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
    res->kiq_end[3] = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
    grbmSelect(adev, 0, 0, 0, 0);
    grbmSelect(adev, 1, 0, 0, 0);                                      // the target of the scheduler's MAP_QUEUES: ME 1 pipe 0 queue 0
    res->kiq_end[4] = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
    res->kiq_end[5] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_BASE);
    res->kiq_end[6] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_CONTROL);
    res->kiq_end[7] = RREG32_SOC15(RN_GC, 0, regCP_MQD_BASE_ADDR);
    res->kiq_end[8] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_DOORBELL_CONTROL);
    res->kiq_end[9] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PERSISTENT_STATE);
    res->kiq_end[10] = RREG32_SOC15(RN_GC, 0, regCP_HQD_VMID);
    res->kiq_end[11] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_WPTR_LO);
    grbmSelect(adev, 0, 0, 0, 0);
    for (u32 k = 0; k < 3; k++) {                                       // MEC pipes' program counters after the map, three samples apart
        for (u32 pipe = 0; pipe < 4; pipe++) { grbmSelect(adev, 1, pipe, 0, 0); res->mec_pc[4 + k * 4 + pipe] = RREG32_SOC15(RN_GC, 0, regCP_MEC_RS64_INSTR_PNTR); }
        udelay(100);
    }
    grbmSelect(adev, 0, 0, 0, 0);
    for (u32 i = 0; i < 96; i++) gMesKiqRing[i] = rn_vram_rd32(adev, kiqOff + i * 4);   // everything on the KIQ ring: our packets and what the scheduler appended
    if (!res->add_ok) { kiqWptr = res->kiq_end[0]; mesKiqRingTest(adev, kiqOff, &kiqWptr, &res->ktest[4], &res->ktest[5]); }   // and after the stuck map: does the KIQ still run packets?
    if (gMesSelfMap && !res->add_ok) {
        // rednavi_mes_selfmap=1: send the MAP_QUEUES the scheduler put on the KIQ ring ourselves (same packet: ME 1 pipe 0 queue 0, our MQD), after the scheduler
        // is stuck. If the KIQ firmware programs the HQD for our packet, the KIQ and the MQD are fine and the scheduler's own request is what fails.
        kiqWptr = res->kiq_end[0];
        u32 pk[16];
        memset(pk, 0, sizeof(pk));
        pk[0] = RN_PACKET3(0xA2u, 5);
        pk[1] = 1u << 29;                                                  // one queue, compute engine, ME 1, pipe 0, queue 0
        pk[2] = legDb << 2;
        pk[3] = lower_32_bits(vaBase + qOff + 0x2000); pk[4] = upper_32_bits(vaBase + qOff + 0x2000);
        pk[5] = lower_32_bits(vaBase + qOff + 0x1008); pk[6] = upper_32_bits(vaBase + qOff + 0x1008);
        pk[7] = RN_PACKET3(0x10u, 7);                                      // NOP to the 16-dword boundary
        mesRingSubmit(adev, kiqOff, kiqOff + 0x1008, kMesKiqDoorbell, &kiqWptr, pk, 16);
        msleep(50);
        grbmSelect(adev, 3, 1, 0, 0);
        res->kiq_end[12] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_RPTR);
        grbmSelect(adev, 1, 0, 0, 0);
        res->kiq_end[13] = RREG32_SOC15(RN_GC, 0, regCP_HQD_ACTIVE);
        res->kiq_end[14] = RREG32_SOC15(RN_GC, 0, regCP_HQD_PQ_BASE);
        res->kiq_end[15] = RREG32_SOC15(RN_GC, 0, regCP_MQD_BASE_ADDR);
        grbmSelect(adev, 0, 0, 0, 0);
    }   // dwords 16..19: after the SET_RESOURCES and MAP_QUEUES packets
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
    for (u32 i = 0; i < 4; i++) { res->kiq_pc_end[i] = RREG32_SOC15(RN_GC, 0, regCP_MES_INSTR_PNTR); udelay(53 * (i + 1)); }   // the KIQ pipe's own program counter after the failed map
    for (u32 pipe = 0; pipe < 2; pipe++) {                              // the per-pipe MES registers of both pipes, for the same read on Linux (tools/linux_pipe_pcs.py)
        grbmSelect(adev, 3, pipe, 0, 0);
        for (u32 i = 0; i < 32; i++) gMesPipeRegs[pipe][i] = rn_rreg32(adev, kMesPipeRegDw[i]);
    }
    grbmSelect(adev, 3, 1, 0, 0);
    res->kiq_version = RREG32_SOC15(RN_GC, 0, regCP_MES_GP3_LO);
    grbmSelect(adev, 0, 0, 0, 0);
    res->mes_db[0] = RREG32_SOC15(RN_GC, 0, regCP_MES_DOORBELL_CONTROL1);
    res->mes_db[1] = RREG32_SOC15(RN_GC, 0, regCP_MES_DOORBELL_CONTROL2);
    res->mes_db[2] = RREG32_SOC15(RN_GC, 0, regCP_MES_DOORBELL_CONTROL3);
    res->mes_db[3] = RREG32_SOC15(RN_GC, 0, regCP_MES_DOORBELL_CONTROL4);
    res->mes_db[4] = RREG32_SOC15(RN_GC, 0, regCP_MES_DOORBELL_CONTROL5);
    res->mes_db[5] = RREG32_SOC15(RN_GC, 0, regCP_MES_DOORBELL_CONTROL6);
    for (u32 i = 0; i < 16; i++) { res->log_hist[i] = rn_vram_rd32(adev, logOff + i * 4); res->log_scratch[i] = rn_vram_rd32(adev, logOff + 0x4000 + i * 4); }
    // what the hung scheduler leaves behind: a series of instruction pointers, the queue's MQD and write-back words, the cleaner shader fence
    grbmSelect(adev, 3, 0, 0, 0);
    for (u32 i = 0; i < 8; i++) { res->log_hist[i] = RREG32_SOC15(RN_GC, 0, regCP_MES_INSTR_PNTR); udelay(37 * (i + 1)); }
    grbmSelect(adev, 0, 0, 0, 0);
    res->log_hist[8] = rn_vram_rd32(adev, qOff + 0x2000);
    res->log_hist[9] = rn_vram_rd32(adev, qOff + 0x2000 + 130 * 4);
    res->log_hist[10] = rn_vram_rd32(adev, qOff + 0x2000 + 143 * 4);
    res->log_hist[11] = rn_vram_rd32(adev, sharedOff + 0x200);
    res->log_hist[12] = rn_vram_rd32(adev, qOff + 0x1008);
    res->log_hist[13] = rn_vram_rd32(adev, qOff + 0x1000);
    res->log_hist[14] = rn_vram_rd32(adev, apiStatusOff);
    res->log_hist[15] = rn_rreg32(adev, adev->reg_offset[RN_GC_HWIP][0][0] + 0xda4);   // GRBM_STATUS
    // Two windows of the MES event log (the same file Linux shows in debugfs amdgpu_mes_event_log, where the first records sit at 0x900 and the API
    // timeline at 0x1300 and up, 7 dwords each: opcode, start time (64 bit), end time (64 bit), 0, 1). The CP_MES_DM_INDEX registers read 0xffffffff, so this
    // is the way to see what the firmware wrote for our frames.
    if (gMesDmDump) {
        for (u32 i = 0; i < 512; i++) gMesMqd[i] = rn_vram_rd32(adev, qOff + 0x2000 + i * 4);   // the legacy queue's MQD as the MES saw it, for the diff with Linux's
        for (u32 i = 0; i < 256; i++) {
            gMesDm[0][i] = rn_vram_rd32(adev, logOff + 0x900 + i * 4);
            gMesDm[1][i] = rn_vram_rd32(adev, logOff + 0x1300 + i * 4);
        }
    }
    dumpMesDbg();   // again after the queue frames: the records the ADD_QUEUE path wrote while it was stuck
    res->step = 5;
    if ((res->q_active & 1u) || res->add_ok) { gMapped = true; gMappedWptr = 0; gMappedPipe = foundPipe; gMappedQueue = foundQueue; }   // later rn_compute_submit_ib calls use this queue
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
