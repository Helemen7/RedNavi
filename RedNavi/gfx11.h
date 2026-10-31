// GC 11 memory hub and SDMA 6 bring-up written against the compat layer (struct rn_device, RREG32_SOC15). The sequences follow
// Linux: gfxhub_v3_0_gart_enable and sdma_v6_0_gfx_resume_instance / sdma_v6_0_ring_test_ring.
#ifndef REDNAVI_GFX11_H
#define REDNAVI_GFX11_H

#include <amd/rn_device.h>

#define RN_VRAM_MC_BASE 0x8000000000ull       // MC address of the start of VRAM on this card (MMHUB FB_LOCATION_BASE 0x8000)

struct rn_gmc {
    u64 fb_start, fb_end;                     // VRAM in MC addresses
    u64 gart_start, gart_end;                 // GART aperture (VMID 0 page table)
    u64 pt_vram_offset;                       // VRAM offset of the VMID 0 page table; rn_gfxhub_enable fills it with PTEs mapping
                                              // each GART page to the VRAM offset equal to its address (identity window)
    u64 scratch_vram_offset;                  // pages used as default and fault pages
    u64 dummy_vram_offset;
};

struct rn_sdma_result {
    u32 step;                                 // 0 = not started, higher = how far it got
    u32 rptr, wptr, f32_cntl, rb_cntl, value; // registers/memory read after the test
    u32 pre_value;                            // the target right after it was filled, to check the VRAM path itself
    u32 fault_status, fault_addr_lo, fault_addr_hi;   // GCVM_L2_PROTECTION_FAULT_*
    u32 sdma_status, sdma_status1, ctx_status;
    u32 version, cntl, power_cntl, checksum, freeze, chicken, program;   // is the block reachable at all?
    u32 ring0, ring3, rptr_shadow, wptr_shadow;                          // memory read back after the test
    int ok;
};

// Program counter start of the RS64 CP firmware, from the firmware headers (gfx_firmware_header_v2_0.ucode_start_addr_lo/hi).
struct rn_sdma_run_result {
    u32 rptr, wptr, rptr_shadow;
    u32 fault_status, fault_addr_lo, fault_addr_hi;   // GCVM_L2_PROTECTION_FAULT_*
    int ok;                                           // the read pointer reached the end of the packets
};

struct rn_kiq_result {
    u32 step;
    u32 kiq_active, kiq_rptr, kiq_wptr;           // the KIQ queue after the commands
    u32 map_active, map_pq_control, map_doorbell_ctl, map_rptr, map_wptr;   // the mapped compute queue, read from its HQD
    u32 fault_status, fault_addr_lo;
    u32 kiq_doorbell_ctl, kiq_pq_control, kiq_pq_base, kiq_pq_status, cp_stat, kiq_rptr_late, ring_dw0, ring_dw1;   // diagnostics of the KIQ after the doorbell
    int ok;
};

struct rn_cp_start {
    u32 pfp_lo, pfp_hi, me_lo, me_hi, mec_lo, mec_hi;
};

struct rn_gfx_result {
    u32 step;                                 // 0 = not started, higher = how far it got
    u32 me_cntl_before, cp_stat_before;       // CP_ME_CNTL and CP_STAT before the CP is touched
    u32 me_cntl, cp_stat, rptr, wptr, scratch;
    u32 rptr_shadow, ring0;                   // memory read back after the test
    u32 trace[8][3];                          // CP_ME_CNTL, CP_STAT, CP_RB0_RPTR at 0, 1, 2, 5, 10, 20, 50, 100 ms after the wptr write
    u32 fault_status, fault_addr_lo, fault_addr_hi;   // GCVM_L2_PROTECTION_FAULT_*, after the test
    u32 me_cntl_imm, me_cntl_10ms;            // CP_ME_CNTL right after the unhalt write, and 10 ms later
    u32 rb0_cntl_rd, rb0_base_rd, rb0_base_hi_rd, rb_active_rd, max_ctx_rd, device_id_rd, pfp_pc_rd, me_pc_rd;   // read back of what was written
    int ok;
};

struct rn_compute_result {
    u32 step;
    u32 scratch, active_rd, pq_control_rd, mqd_base_rd, rptr, wptr, rptr_shadow, ring0;
    u32 trace[8][3];                          // CP_MEC_RS64_CNTL, CP_HQD_ACTIVE, CP_HQD_PQ_RPTR at 0, 1, 2, 5, 10, 20, 50, 100 ms after the wptr write
    u32 fault_status, fault_addr_lo, fault_addr_hi;
    int ok;
};

struct rn_dispatch_result {
    u32 step;
    u32 fence, fence_before, rptr, wptr, active, scratch;
    u32 target, target_before;                // the dword the shader stores to (0 if no store was asked for)
    u32 trace[6][2];                          // CP_HQD_PQ_RPTR and the fence at 0, 1, 5, 20, 100, 500 ms after the wptr write
    u32 fault_status, fault_addr_lo, fault_addr_hi;
    int ok;
};

struct rn_ih_result {
    u32 step;
    u32 rb_cntl_rd, wptr_reg, wptr_shadow, rptr_reg, base_rd, cp_int_rd;
    u32 entry[16];                            // the first two interrupt vectors of the ring (8 dwords each)
};

#ifdef __cplusplus
extern "C" {
#endif

// Programs the GC memory hub: system aperture on the VRAM range, VMID 0 enabled with its page table, L1 TLB and L2 cache.
void rn_gfxhub_enable(struct rn_device *adev, const struct rn_gmc *gmc);

// The VMID 0 page table (VRAM, at gmc.pt_vram_offset) maps the GART window gmc.gart_start..gart_end in 4 KB pages. rn_gfxhub_enable
// zeroes it; rn_gart_map_page then makes page `page` of the window point at a system memory page (bus address as the IOMMU or the
// physical address gives it: valid, system, snooped, readable, writeable). Do this before rn_gfxhub_enable, which also flushes the TLBs.
void rn_gart_map_page(struct rn_device *adev, const struct rn_gmc *gmc, u32 page, u64 bus_addr);
void rn_gart_clear(struct rn_device *adev, const struct rn_gmc *gmc);
void rn_gart_map_vram_page(struct rn_device *adev, const struct rn_gmc *gmc, u32 page, u64 mc_addr);   // a GART page that points at VRAM
// With the hub already on: change or remove a PTE (bus 0 = unmap), then flush VMID 0's TLBs through invalidation engine 17, the one
// Linux reserves for the GART (gmc_v11_0_flush_vm_hub). Returns 0 once the engine acknowledges, -1 on timeout.
int rn_gart_update_page(struct rn_device *adev, const struct rn_gmc *gmc, u32 page, u64 bus_addr);
int rn_gart_flush(struct rn_device *adev);

// Runs a packet list on SDMA0's gfx ring (the setup of rn_sdma0_test, reprogrammed) and waits for the read pointer to reach its end.
// Addresses inside the packets are whatever the SDMA can translate: MC addresses in the VRAM aperture, or GART addresses.
void rn_sdma0_run(struct rn_device *adev, u64 va_base, u64 ring_vram_offset, u32 ring_bytes, const u32 *pkt, u32 dwords,
                  struct rn_sdma_run_result *res);

// Starts SDMA0 on a ring of ring_bytes at ring_vram_offset (the read and write pointer shadows follow in the same page) and
// asks it to write a magic value to target_vram_offset. Everything lives in VRAM, so no GART mapping is needed.
// va_base is added to the offsets to form the addresses the SDMA uses: RN_VRAM_MC_BASE (system aperture) or 0 (the identity GART window).
void rn_sdma0_test(struct rn_device *adev, u64 va_base, u64 ring_vram_offset, u32 ring_bytes, u64 target_vram_offset, struct rn_sdma_result *res);

// First command on the graphics ring (gfx_v11_0_config_gfx_rs64, gfx_v11_0_cp_gfx_resume and gfx_v11_0_ring_test_ring): points the CP
// at its firmware entry, programs ring 0 of the gfx pipe 0 in VRAM (no doorbell, no MES) and asks it to write a magic value to
// SCRATCH_REG0 with SET_UCONFIG_REG. Needs rn_gfxhub_enable and the RLC booted. Written from the Linux sources, NOT yet run on hardware.
void rn_gfx_ring_test(struct rn_device *adev, u64 va_base, u64 ring_vram_offset, u32 ring_bytes, const struct rn_cp_start *pc,
                      struct rn_gfx_result *res);

// First command on a compute queue: the MEC pipes are activated and one hardware queue (MEC 1, pipe 0, queue 2) is programmed straight
// into the CP_HQD_* registers the way gfx_v11_0_kiq_init_register does for the KIQ, then SET_UCONFIG_REG writes SCRATCH_REG0.
// Needs rn_gfx_ring_test to have run first (it points the MEC at its firmware). Written from the Linux sources.
void rn_compute_ring_test(struct rn_device *adev, u64 va_base, u64 ring_vram_offset, u32 ring_bytes, struct rn_compute_result *res);

// First shader dispatch: on the compute queue that rn_compute_ring_test left active, a shader that is a single s_endpgm is set up
// (SET_SH_REG: program address, resources, thread counts) and run as one workgroup of one thread (DISPATCH_DIRECT), followed by a
// RELEASE_MEM that writes 0xF00DF00D to a fence in VRAM when the dispatch is done. Written from nvd.h and the register headers, and
// from the packet layout of gfx_v11_0_ring_emit_fence; NOT yet run on hardware.
// With target_vram_offset nonzero the shader is not just s_endpgm: it stores 0x600DC0DE to that VRAM dword (global_store_b32, the
// instruction words hand-assembled from the encodings in LLVM's MC tests, llvm/test/MC/AMDGPU/gfx11_asm_*.s).
void rn_compute_dispatch_test(struct rn_device *adev, u64 va_base, u64 ring_vram_offset, u64 shader_vram_offset, u64 fence_vram_offset,
                              u64 target_vram_offset, u64 target_gpu_va, volatile u32 *host_target, u32 int_sel,
                              int (*wait_done)(void *ctx, u32 timeout_ms), void *wait_ctx, struct rn_dispatch_result *res);
// (With wait_done set, the fence is not polled: after the submission the caller's function waits, e.g. for the interrupt, and the fence
// is read once afterwards.)
// (With target_gpu_va and host_target set, the shader stores to that GPU address instead - a GART-mapped host page - and the result is
// read from host_target; target_vram_offset is then ignored.)

// Interrupt handler (ih_v6_0_irq_init, simplified): ring 0 of OSSSYS in VRAM, its write pointer written back to memory, no MSI and no
// doorbell; the CP's end-of-pipe timestamp interrupt for MEC pipe 0 is enabled. A RELEASE_MEM with int_sel 2 then makes the CP
// push an interrupt vector into the ring, which rn_ih_read shows. Written from the Linux sources; NOT yet run on hardware.
// Doorbells: the NBIO doorbell aperture is switched on, and while rn_set_doorbell has a base, the compute queue is programmed with a
// doorbell (CP_HQD_PQ_DOORBELL_CONTROL, wptr polling off) and every submission is a 64-bit write of the wptr to that doorbell instead of a
// register write. base is the mapped doorbell BAR (BAR2), dword_index the queue's doorbell in dwords (mec_ring0 << 1 = 6 here).
// before[0..2] receive DOORBELL_APER_EN, S2A_DOORBELL_ENTRY_0_CTRL and _3_CTRL as they were; APER_EN is only written if it was 0, and the
// two S2A entries (nbio_v4_3_gc_doorbell_init) only if write_s2a is set.
void rn_doorbell_aperture_enable(struct rn_device *adev, int write_s2a, u32 before[3]);
void rn_set_doorbell(volatile void *base, u32 dword_index);

// msi: IH_RB_CNTL.RPTR_REARM, the ring re-arms the message-signalled interrupt when the read pointer is written.
// Command submission on the compute queue that rn_compute_ring_test programmed (through its doorbell if one is set): appends an
// INDIRECT_BUFFER packet for the command buffer at ib_va (ib_dwords PM4 dwords) and a RELEASE_MEM that writes fence_value to fence_va
// (a GPU address, VRAM aperture or GART) when the buffer has executed. The ring is addressed by the running wptr counter, so it wraps.
int rn_compute_submit_ib(struct rn_device *adev, u64 va_base, u64 ring_vram_offset, u32 ring_bytes, u64 ib_va, u32 ib_dwords,
                         u64 fence_va, u32 fence_value);
// Fills ib with the PM4 of a 1x1x1 dispatch of the shader at shader_va (the packets of rn_compute_dispatch_test, without the fence)
// and returns the number of dwords.
// What the compute queue and the memory hub say now: HQD read pointer, wptr, active, CP_STAT, the VM fault status and address.
void rn_compute_diag(struct rn_device *adev, u32 out[11]);   // out[8..10]: SDMA0_VERSION (0x600 when the GC is powered), GRBM_STATUS, RLC_GPM_STAT
// RLC safe mode (gfx_v11_0_set_safe_mode): keeps the graphics block out of GFXOFF while it is being used; enter 0 leaves it again.
// The doorbell and queue registers as they are now (compare with the values that were programmed): HQD doorbell control, CP_PQ_STATUS, the
// MEC doorbell range (lower, upper), NBIO DOORBELL_APER_EN, HQD PQ base, PQ control, MQD base.
void rn_compute_diag2(struct rn_device *adev, u32 out[8]);
void rn_mes_state(struct rn_device *adev, u32 out[15]);
void rn_gfx_constants_init(struct rn_device *adev);   // gfx_v11_0_hw_init steps between the memory hub and the CP resume (golden TCP_CNTL, read timeout, SH_MEM per VMID, doorbell ranges)
extern u32 gMesDm[2][256];   // MES event log windows at +0x900 and +0x1300 (1 KB each), read after the last frame; not in rn_mes_result, the kernel stack is 16 KB
extern u32 gMesMqd[512];   // the legacy queue's MQD read back after the last frame (rednavi_mes_dm=1)
extern u64 gMesHostWbVa;            // rednavi_mes_hostwb=1: the legacy queue's rptr/wptr words in host memory (GART address) instead of VRAM
extern volatile u32 *gMesHostWbMem;
extern u64 gMesHostRingVa;          // rednavi_mes_hostring=1: the legacy queue's ring (8 KB) in host memory, with the write-back words
extern volatile u32 *gMesHostRingMem;
extern u32 gMesKiqRing[96];   // the first 96 dwords of the MES KIQ ring after the failed map
extern bool gMesCleaner;      // rednavi_mes_cleaner=1: a cleaner shader (one s_endpgm) and its address in the KIQ SET_RESOURCES, as gfx11_kiq_set_resources sends it
extern bool gMesHwResLinux;   // rednavi_mes_hwres=1: SET_HW_RESOURCES with Linux's gds_size 0x1000, oversubscription_timer 50 and disable_mes_log 1
extern bool gMesEarlyMap;  // rednavi_mes_earlymap=1: a MAP_QUEUES for the MEC queue on the KIQ ring before the scheduler gets any frame
extern bool gMesPostMap;   // rednavi_mes_post=1: run one WRITE_DATA through the MES-mapped queue right after the map and watch its doorbell control
extern bool gMesSelfMap;   // rednavi_mes_selfmap=1: after the stuck map, put the scheduler's MAP_QUEUES on the KIQ ring ourselves
extern u32 gMesPipeRegs[2][32];   // per-pipe MES registers, order in kMesPipeRegDw (gfx11.cpp)
extern const u32 kMesPipeRegDw[32];
extern u32 gKiqTrace[3][32];   // KIQ pc runs {pc, repeats}: before the first packet, while it is processed, after
extern u64 gMesHostLogBase;       // rednavi_mes_host=1: logical offset of the host buffer (GART address minus the VRAM MC base), its CPU mapping and size
extern volatile u8 *gMesHostMem;
extern u64 gMesHostSize;
extern bool gMesDmDump;   // rednavi_mes_dm=1: read the two event log windows after the last frame
extern u32 gMesDbg[2][256];   // MES firmware log (mes_dbgext): the first 2 KB of the buffer, dwords 0..3 are the header the firmware leaves
extern bool gMesDbgExt;   // rednavi_mes_dbgext=1 (host buffer, as Linux) or 2 (the VRAM area): hand the scheduler a log buffer after the resource frames and dump what it wrote
extern u64 gMesDbgHostVa;     // the log buffer's GART address when it lives in host memory; 0 means the VRAM area at base + 0x50000
extern void *gMesDbgHostMem;  // its CPU mapping, for the seed and the dump
void rn_mec_enable(struct rn_device *adev);   // MEC pipes out of reset and running, no queue
void rn_mes_doorbell_regs(struct rn_device *adev, u32 out[12]);   // CP_MES_DOORBELL_CONTROL1..6 per MES pipe
void rn_scan_hqds(struct rn_device *adev, u32 out[33]);   // the active ME 1 queues: 8 x {pipe*8+queue, doorbell control, rptr, wptr}, count

struct rn_mes_result {
    u32 step;
    u32 cntl, p0_pc, p0_pc10, p1_pc, p1_pc10;     // CP_MES_CNTL and the instruction pointers of both pipes (the second 10 ms after the first)
    u32 kiq_active, kiq_doorbell_ctl, kiq_rptr;   // the MES KIQ queue (ME 3, pipe 1)
    u32 sched_active;                              // the scheduler queue (ME 3, pipe 0) loaded through MAP_QUEUES
    u32 ring_fence, api_status;                    // QUERY_SCHEDULER_STATUS fence and the last frame's completion word
    int hw_res_ok, add_ok;
    u32 hw_res1_status;
    int dbg_ok; u32 dbg_status;                    // SETUP_MES_DBGEXT frame and its completion word (opt-in, rednavi_mes_dbgext=1)
    u32 early[6];                                  // rednavi_mes_earlymap=1: KIQ rptr, then the target HQD's ACTIVE, PQ_BASE, MQD_BASE, DOORBELL_CONTROL, and our KIQ wptr
    u32 post[10];                                  // rednavi_mes_post=1: db control before the doorbell, the target word, db control/rptr/active after, db control at 0.05/0.5/2/5/10 s
    u32 init_state[10];                            // at the start of rn_mes_start: CP_MES_CNTL, RLC_CP_SCHEDULERS, pc pipe 0/1, HQD ACTIVE me3 pipe 0/1, doorbell control me3 pipe 0/1, MEC active queue mask, bootload status
    u32 snap[3][10];                               // queue state after the map, after the first doorbell, after the second: db ctl, rptr, wptr, active, persistent state, pq control, MEC pc, rptr word, wptr word, MQD dw 142
    u32 post3[5];                                  // doorbell control after each of the five later doorbells
    u32 post2[5];                                  // result word of each of the five later packets (0x600DC0DE: ran), after 0.1, 0.5, 2, 6 and 15 s from the first doorbell
    u32 ktest[6];                                  // KIQ ring test (WRITE_DATA to SCRATCH_REG0, 0xDEADBEEF expected) and the KIQ pc after it: after the scheduler queue map, after SET_RESOURCES, after the stuck map
    u32 kiq_pc_pre[2];                             // the same pointer after SET_RESOURCES, before ADD_QUEUE (the KIQ pipe idle)
    u32 kiq_pc_end[4];                             // CP_MES_INSTR_PNTR of the KIQ pipe (ME 3 pipe 1), four samples after the failed map
    u32 mec_pc[16];                                // CP_MEC_RS64_INSTR_PNTR of MEC pipes 0..3: [0..3] before the map, [4..15] three samples after it
    u32 kiq_end[16];                                // KIQ HQD wptr, rptr, doorbell control, active and ring dwords 16..19 after the failed map
    u32 parity[2];                                 // [0] cleaner shader address sent in the KIQ SET_RESOURCES (>> 8, low half), [1] bit 0 rednavi_mes_cleaner, bit 1 rednavi_mes_hwres
    u32 hqd_active;                                // MEC queues with CP_HQD_ACTIVE set before the legacy map, bit pipe * 8 + queue
    u32 cp_me_cntl, mec_cntl, rlc_sched;           // CP_ME_CNTL, CP_MEC_RS64_CNTL, RLC_CP_SCHEDULERS before the MES start
    u32 bootload, rlc_srm, cp_stat0;               // RLC_RLCS_BOOTLOAD_STATUS, RLC_SRM_CNTL and CP_STAT before the MES start
    u32 fw_ver0, fw_ver1;                         // CP_MES_GP3_LO of each pipe right after the start: the version of the image that runs
    u32 mes_db[6];                                // CP_MES_DOORBELL_CONTROL1..6 after the queue was added
    int sched_cfg_ok;
    u32 q_pipe, q_queue;                          // the hardware queue the MES gave ours
    u32 log_hist[16], log_scratch[16];            // first dwords of the two halves of the MES event log
    u32 q_active, q_doorbell_ctl, fault_status;    // the legacy compute queue mapped by ADD_QUEUE
    u32 sched_version, kiq_version, sched_rptr, sched_wptr, mepc, mcause, ring_fence_late, p0_pc_end;   // diagnostics after the last stage
};
// Bring up the MES in stages (docs/COMPAT.md): start[] = {pipe0 start lo, hi, pipe1 start lo, hi} from the firmware headers. Needs the doorbell
// aperture mapped (rn_doorbell_aperture_enable and rn_set_doorbell). base is a VRAM offset with 0x40000 bytes free (0x54000 when the log
// buffer is in VRAM: gMesDbgExt with no host buffer, rednavi_mes_dbgext=2; with the host buffer of rednavi_mes_dbgext=1 the area is unused).
void rn_mes_start(struct rn_device *adev, u64 va_base, u64 base, const struct rn_gmc *gmc, const u32 start[4], u32 max_step, void (*progress)(void *ctx, u32 step),
                  void *ctx, struct rn_mes_result *res);   // read-only: MES control and the state of its two pipes
void rn_gc_safe_mode(struct rn_device *adev, int enter);
u32 rn_build_dispatch_ib(struct rn_device *adev, u32 *ib, u64 shader_va);

// The way Linux runs compute queues (gfx_v11_0_kiq_* and gfx11_kiq_map_queues): the KIQ, a queue the firmware knows (ME 1, pipe 3,
// queue 0, announced to the RLC with RLC_CP_SCHEDULERS), is programmed through the CP_HQD_* registers; the compute queue is then handed to
// the firmware with SET_RESOURCES and MAP_QUEUES from a complete MQD in memory (ME 1, pipe 0, queue 3, doorbell slot mec_ring0 + 1 =
// dword 8). Afterwards rn_compute_submit_ib and rn_compute_diag work on that mapped queue. Needs a doorbell (rn_set_doorbell).
// kiq_off and queue_off are VRAM offsets of two 24 KB areas (ring, MQD at +0x2000, EOP at +0x4000).
// max_step stops early, to find a step that hangs the machine: 1 RLC_CP_SCHEDULERS, 2 + program the KIQ registers, 3 + the compute queue's
// MQD, 4 + SET_RESOURCES/MAP_QUEUES and the KIQ doorbell. progress (if set) receives the step about to run, before it runs.
void rn_kiq_map_compute(struct rn_device *adev, u64 va_base, u64 kiq_off, u64 queue_off, u32 max_step, void (*progress)(void *ctx, u32 step),
                        void *progress_ctx, bool rlc_sched, u32 kiq_pipe, u32 kiq_queue, struct rn_kiq_result *res);

void rn_ih_enable(struct rn_device *adev, u64 va_base, u64 ring_vram_offset, u32 ring_bytes, u64 wptr_shadow_vram_offset, int msi);
// Acknowledge: the read pointer is set to the current write pointer (which re-arms the next MSI). Returns the write pointer.
u32 rn_ih_ack(struct rn_device *adev, u32 ring_bytes);
void rn_ih_read(struct rn_device *adev, u64 ring_vram_offset, u64 wptr_shadow_vram_offset, struct rn_ih_result *res);

#ifdef __cplusplus
}
#endif

#endif
