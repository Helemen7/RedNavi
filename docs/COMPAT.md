# Compat layer (LinuxKPI-style)

Goal: let code ported from amdgpu (SMU 13, later DC, GFX, MES) build in the kext with as few edits as possible, instead of re-writing every register access by hand like the probe does. It is written from scratch under BSD-3; FreeBSD's LinuxKPI and drm-kmod were read only for ideas.

State (2026-10-28): the layer builds into `RedNavi.kext` and its pure parts have host tests (`make test`). **It has not been loaded on a real machine yet.** The first consumer is the SMU message stage (`RedNavi/smu13.cpp`, boot-arg `rednavi_smu=1`), which is the first thing to run on hardware.

## Layout

| Path | What |
|---|---|
| `compat/include/linux/*.h` | Linux headers: `types`, `errno`, `bits`, `kernel` (container_of, min/max, ERR_PTR, READ_ONCE), `printk`, `device`, `list`, `atomic` (+ kref), `spinlock`, `mutex`, `completion`, `delay`, `jiffies`, `ktime`, `slab`, `io`, `byteorder`, `firmware`, `workqueue`, `dma-mapping` |
| `compat/include/amd/rn_device.h` | Minimal stand-in for `struct amdgpu_device`: `RREG32`, `WREG32`, `RREG32_SOC15`, `SOC15_REG_OFFSET`, `REG_SET_FIELD` |
| `compat/include/rn/hooks.h` | The few functions the pure headers need from their environment (log, alloc, sleep, clock, yield) |
| `compat/src/rn_hooks.cpp` | Kernel implementation of the hooks (`IOLogv`, `IOMalloc`, `IOSleep`, `clock_get_uptime`, `panic`) |
| `compat/src/rn_slab.cpp` | `kmalloc` family with a size header, so `kfree` and `krealloc` need no size |
| `compat/src/rn_firmware.cpp` | `request_firmware` from a registry of blobs (`rn_firmware_register("amdgpu/smu_13_0_10.bin", ...)`); nothing is read from disk |
| `compat/src/rn_workqueue.cpp` | `work_struct`, `delayed_work`, `schedule_work` ... on `thread_call` |
| `compat/src/rn_dma.cpp` | `dma_alloc_coherent` on a physically contiguous `IOBufferMemoryDescriptor` |
| `compat/src/rn_reg.cpp` | Register access: direct inside the BAR, `MM_INDEX`/`MM_DATA` past its end |
| `tests/host/` | Host tests and the host version of the hooks |

## Known limits

- Spinlocks do not mask interrupts (nothing runs in interrupt context yet). Mutexes are not recursive and have no owner; a contended one yields with `IOSleep(1)`.
- One jiffy is one millisecond (`HZ` 1000).
- Work items own a `thread_call`; call `rn_work_destroy` before freeing one that was queued. All workqueues are the same shared pool.
- `dma_alloc_coherent` assumes the address from `getPhysicalAddress()` is what the GPU must use, i.e. no IOMMU. Verify on a machine with VT-d on.
- GFP flags are accepted but only `__GFP_ZERO` does anything.
- Not done yet: wait queues, `dma_fence`, IDR/xarray, `ww_mutex`, PCI helpers beyond what the probe does, interrupts, DRM structures.

## Adding a header

Put it under `compat/include/linux/` only if Linux code includes it by that name. Keep it free of IOKit so it runs in the host tests, and route anything kernel-specific through `rn/hooks.h`. Add a check to `tests/host/compat_test.cpp`.

## SMU stage

`rn_smu13_send` follows `smu_msg_v1_send_msg` from Linux: wait for the previous response, clear `C2PMSG_90`, write the argument to `C2PMSG_82`, the message to `C2PMSG_66`, poll `C2PMSG_90` (1 = OK). `rednavi_smu=1` sends `GetSmuVersion`, `GetDriverIfVersion` and `TestMessage` after the PSP and display bring-up and publishes `smu-info` (and `smu-info-early`, registers only, read before any write) on the probe node. Message numbers come from `pmfw_if/smu_v13_0_0_ppsmc.h`; SMU 13.0.10 uses those tables. Messages that change clocks or features are not sent.

**Result (2026-10-29): the SMU answers once its firmware has been loaded through the PSP.** Without that step none of the three messages gets a response, with the registers looking healthy (`C2PMSG_90` = 1, `MP1_FIRMWARE_FLAGS` bit 0 set). In `psp_load_non_psp_fw`, Linux first calls `psp_load_smu_fw` whenever the PSP supports autoload (MP0 13.0.10 does), which sends `GFX_CMD_ID_LOAD_IP_FW` with type `GFX_FW_TYPE_SMU` (18) and the ucode array of `smu_13_0_10.bin` (the 261632 bytes at `ucode_array_offset_bytes`); an earlier reading of `psp_hw_start` made it look skipped, which was wrong. The probe does this with boot-arg `rednavi_smu_load=1` (`loadSmuFw`, right after `SETUP_TMR`, header from `tools/gen_smu_fw_header.py`, git-ignored). The sOS accepts it (status 0) and the SMU then answers: `GetSmuVersion` returns 0x00505c00 (the version of the file we loaded; the IFWI one was 0x00505600), `GetDriverIfVersion` 0x40 and `TestMessage` 0x12345679, the same values Linux gets. The "Linux comparison run" section below documents how the difference was found; the GC/IMU/RLC hypothesis there turned out not to be needed for this.

### Linux comparison run

`tools/linux_smu_mailbox.py` reads the MP1 mailbox on Linux and, with `send`, sends the same three messages with the same protocol. Two runs on the card:

1. Normal boot (the driver has initialised the SMU): `sudo python3 linux_smu_mailbox.py send smu_mailbox_normal.txt`.
2. Boot with the kernel parameter `modprobe.blacklist=amdgpu` so no driver touches the card, then read BAR5 directly, like RedNavi does: `sudo python3 linux_smu_mailbox.py send smu_mailbox_nodriver.txt bar5`. (`amdgpu.dpm=0` gives a black screen on this card and is not usable.)

If the SMU answers in run 2, the difference is in what RedNavi does around the mailbox; if it does not but answers in run 1, it needs something the Linux driver does during its power-management init. Results go to `docs/logs/linux-reference/`.

**Result of the Linux runs (2026-10-29)** (`docs/logs/linux-reference/smu_mailbox_normal.txt`, `smu_mailbox_nodriver.txt`):

| | Normal boot (driver ran) | `modprobe.blacklist=amdgpu`, BAR5 only |
|---|---|---|
| `MP0_C2PMSG_81` (sOS alive) | `0x030b09a0` | 0 |
| `MP0_C2PMSG_35` | 0 | `0x80000000` (bootloader waiting) |
| `MP1_FIRMWARE_FLAGS` | 1 | 1 |
| `MP1_C2PMSG_90` before | 1 | 1 |
| `GetSmuVersion` | answered in 0.3 ms: `0x00505600` | no response in 1 s |
| `GetDriverIfVersion`, `TestMessage` | `0x40`, `0x12345679` (OK) | not sent after the first timeout |

So the protocol and the offsets in `rn_smu13_send` are right: the same sequence of writes gets an answer on Linux once the driver has run. Without the driver (sOS not running) the SMU does not answer, which is also what RedNavi saw. RedNavi's late attempt has the sOS running, the ring, LOAD_TOC and SETUP_TMR done, and still gets no answer, so something else in Linux's init order unlocks the mailbox. In the driver-run state `MP1_C2PMSG_71` = 5 and `_74` = 0x14 are set (zero in the no-driver state). Candidates, untested: the non-PSP firmware loads Linux does before the SMU init (IMU, RLC, SDMA, MES, DMUB, TOC), or a PSP step after SETUP_TMR.

**RedNavi's state at the first message (2026-10-29)**, same registers from the probe (`smu-info`): `MP0_C2PMSG_81` = 0x0231bf63 (sOS alive) and `MP0_C2PMSG_35` = 0, like the Linux driver-run column; but `MP1_C2PMSG_71` = 0 and `_74` = 0 (Linux with driver: 5 and 0x14), `MP1_C2PMSG_66` = 0, `_82` = 0, `_90` = 1, `MP1_FIRMWARE_FLAGS` = 1. So the PSP side matches what Linux has after its PSP init, and the SMU side looks like the no-driver state.

Working hypothesis, not tested: in Linux the SMU is first queried (`smu_hw_init`, after `amdgpu_device_fw_loading`) when the PSP has already loaded the GC, SDMA, MES, IMU and RLC firmware into the TMR and started the RLC autoload (`GFX_CMD_ID_AUTOLOAD_RLC`). The SMU firmware may wait for the IMU/RLC handshake before it opens the mailbox. Checking this means loading `gc_11_0_3_{pfp,me,mec,rlc,imu}.bin` and friends through `LOAD_IP_FW`, which is also the first step of M4 (GFX11 bring-up). The display does not depend on it; the SMU would only be needed for clock management.

## M4 step 1: the graphics core boots (2026-10-29)

With `rednavi_smu_load=1 rednavi_gc_load=1 rednavi_smu=1 rednavi_smu_enable=1` the GC block comes up, in this order (the order Linux uses, `psp_load_non_psp_fw` then `smu_smc_hw_setup`):

1. `LOAD_IP_FW` of `smu_13_0_10.bin` (type 18).
2. `LOAD_IP_FW` of 26 images in `AMDGPU_UCODE_ID` order, with the types and image offsets Linux uses (`tools/gen_gc_fw_header.py`, local header `RedNavi/gc_fw.h`): SDMA threads 0 and 1, CP RS64 PFP/ME/MEC and their stacks, MES scheduler and KIQ with their data, IMU I/D, the RLC restore lists, IRAM, DRAM, P, V and G. The sOS accepts all of them (status 0).
3. `GFX_CMD_ID_AUTOLOAD_RLC` (0x21) after `RLC_G`. Accepted, and `GFX_IMU_SCRATCH_0` shows the IMU firmware version 0xb204700, so the IMU runs, but the RLC does not finish booting: `RLC_RLCS_BOOTLOAD_STATUS` stays 0 for 2 s.
4. SMU messages `GetSmuVersion` etc., then `EnableAllSmuFeatures` (0x6). `SetAllowedFeaturesMaskLow/High` are rejected with CmdRejectedPrereq (SCPM, the PSP owns the allowed mask). Right after the enable the RLC boots: `BOOTLOAD_STATUS` 0xc000001f (BOOTLOAD_COMPLETE, bit 31), `CP_STAT` 0, `RLC_CNTL` 1, `RLC_GPM_STAT` 0xb40016, `GRBM_STATUS` 0x302c. Running SMU features: 0x0003ebbc_71ffffff.

So the RLC autoload waits for the SMU features to be enabled. Next for acceleration: memory access for the engines (the VRAM is reachable by physical MC address through the system aperture the VBIOS left; GART for system memory comes later), then an SDMA ring and a first command (write a value to VRAM, check it from the CPU), then the graphics ring (MES/KIQ).

## M4 step 2: the first SDMA command (2026-10-29)

`rednavi_sdma_test=1` (after the step 1 boot-args) programs the GC memory hub and SDMA0, puts a ring of one `WRITE_LINEAR` packet in VRAM and checks the target from the CPU. Verified on the RX 7800 XT: the SDMA writes `0xDEADBEEF` to VRAM, no fault (`sdma-test`: `ok` 1, `rptr` 0x14, `fault_status` 0).

What it took, in order of how long each cost:

- **GC 11.0.3 has its own register offsets for the memory hub.** `gmc_v11_0.c` picks `gfxhub_v3_0_3` for IP 11.0.3, which includes `gc_11_0_3_offset.h`: `GCMC_VM_*`, `GCVM_L2_*` and `GCVM_CONTEXT0_*` sit 0x10 (or 4) higher than in `gc_11_0_0_offset.h`. With the 11.0.0 numbers the writes landed on unrelated GC registers and the reads gave zeros (also on Linux, see below). `RedNavi/gc_regs.h` is now generated from the 11.0.3 headers (`tools/gen_gc_regs.py`). `gfxhub_v3_0_3.c` also redefines `GCVM_L2_CNTL3_DEFAULT` as 0x80100007 (11.0.0: 0x80120007). The SDMA registers are the same in both. Check: before any write, `GCMC_VM_FB_LOCATION_BASE/TOP` now read 0x8000/0x83ff, what the VBIOS programmed.
- **The register bases of the compat layer were never filled.** `gpu.reg_offset` was set in `start()` before the IP discovery table is parsed, so every `RREG32_SOC15` in the SDMA code hit dword `reg` instead of `base + reg` and the "ring registers that hold what we wrote" were plain low registers. They are now set at the start of `sdmaTest`.
- **GFXOFF.** With the GC in GFXOFF every GC and SDMA register reads 0 (on Linux too: `docs/logs/linux-reference/gc_sdma_regs_linux.txt` was read with GFXOFF disabled through the `amdgpu_gfxoff` debugfs file; its GCMC/GCVM lines were read at the 11.0.0 offsets and mean nothing). Here the SDMA powers up right after `EnableAllSmuFeatures` (`SDMA0_VERSION` 0x600, as on Linux) and `DisallowGfxOff` is accepted.
- **The SMU mailbox reads 0 shortly after the enable** (`C2PMSG_90` = 0, no answer) while the SDMA stays up; not understood, and not needed for this test. Do not wait for the mailbox: the old 10 s wait is skipped while `SDMA0_VERSION` is nonzero.
- The ring and the target are addressed by MC address (`0x8000000000 + VRAM offset`) through the system aperture; no GART mapping is used. The SDMA firmware polls the wptr shadow in memory (`F32_WPTR_POLL_ENABLE`), which is written together with the register.

Not done: a graphics ring (KIQ/MES), system memory (GART), interrupts, doorbells, and getting the SMU mailbox to answer after the enable.

## M4 step 3: the first graphics ring command (2026-10-30)

`rednavi_gfx_test=1` (on top of `rednavi_sdma_test=1`, which turns the memory hub on) starts the command processor and runs `SET_UCONFIG_REG SCRATCH_REG0 = 0xDEADBEEF` from a ring in VRAM, as `gfx_v11_0_ring_test_ring` does. Verified on the RX 7800 XT: `SCRATCH_REG0` reads `0xDEADBEEF`, the CP stays up (`CP_ME_CNTL` 0x01000000, only CE_HALT left), no VM fault (`gfx-test`, `gfx-trace`).

The sequence (from `gfx_v11_0.c`): the RS64 entry points of PFP, ME and MEC from the firmware headers (`ucode_start_addr_lo/hi`, now emitted into `gc_fw.h`) with a reset pulse of each pipe (`gfx_v11_0_config_gfx_rs64`); the hardware queue registers; unhalt PFP and ME; write the wptr.

What it took:

- **The CP starts halted** (`CP_ME_CNTL` 0x15000000: CE, PFP and ME halt) after the RLC autoload, and it accepts the unhalt write.
- **The ring is a hardware queue, not `CP_RB0_*`.** The first attempt programmed `CP_RB0_*` as `gfx_v11_0_cp_gfx_resume` does. The CP unhalted, ran for under a millisecond and halted itself, with a VM fault (`GCVM_L2_PROTECTION_FAULT_STATUS` 0xd32: client CPG, mapping error, read, address 0). In GC 11 the gfx queue is `CP_GFX_HQD_*`, filled by Linux from an MQD (`gfx_v11_0_gfx_mqd_init`), and the CP reads the MQD at `CP_GFX_MQD_BASE_ADDR`, which was 0. Writing the HQD registers with the values the MQD would hold, and pointing `CP_GFX_MQD_BASE_ADDR` at a zeroed page, fixed it. (Linux's default path maps the queue through the KIQ or MES instead; this is the same state reached by registers.)
- Decoding a fault: `CID` indexes the client list in `gfxhub_v3_0_3.c` (6 = CPG), `RW` 0 is a read.

Not done: KIQ/MES, compute rings, doorbells, the GFX clear state and golden registers, interrupts, and doing any of this from system memory (GART). A page-sized zeroed MQD works for one ring; real queue management needs the MQD filled in.

## M4 step 4: the first compute queue command (2026-10-30)

`rednavi_compute_test=1` (after `rednavi_gfx_test=1`, which points the MEC at its firmware) activates the four MEC pipes (`gfx_v11_0_cp_compute_enable`) and programs one hardware queue (MEC 1, pipe 0, queue 2) straight into the `CP_HQD_*` registers with the values `gfx_v11_0_compute_mqd_init` would put in the MQD, the way `gfx_v11_0_kiq_init_register` does for the KIQ. A `SET_UCONFIG_REG SCRATCH_REG0 = 0xDEADBEEF` packet runs from a ring in VRAM. Verified on the RX 7800 XT (`compute-test`): `SCRATCH_REG0` reads `0xDEADBEEF`, the read pointer reaches 8 dwords within a millisecond, no VM fault, the queue stays active.

Notes: no doorbell (`CP_PQ_WPTR_POLL_CNTL.EN` on, the wptr is written into the polled shadow in memory and into `CP_HQD_PQ_WPTR_LO`); EOP buffer of 2 KB (`EOP_SIZE` 8); `CP_MQD_BASE_ADDR` points at a page with only the header (`0xC0310800`) set. Queue 2 of pipe 0 was chosen because MES leaves queues 2 and 3 of each MEC pipe to the driver (`compute_hqd_mask` 0xc in the Linux log); MES itself is not running here.

Not done: a shader dispatch, KIQ/MES, doorbells, interrupts.

## M4 step 5: the first shader dispatch (2026-10-30)

`rednavi_dispatch_test=1` (after `rednavi_compute_test=1`, whose queue it reuses) runs a compute dispatch: a shader that is one `s_endpgm` (GFX11 SOPP opcode 48, `0xBFB00000`, followed by `s_code_end` padding to 256 bytes) in VRAM, `SET_SH_REG` packets for the program address (`COMPUTE_PGM_LO/HI`), `COMPUTE_PGM_RSRC1/2/3`, `COMPUTE_START_X..Z`, `COMPUTE_NUM_THREAD_X..Z`, the resource limits and the thread management masks, then `DISPATCH_DIRECT` 1x1x1 (initiator `COMPUTE_SHADER_EN | FORCE_START_AT_000 | ORDER_MODE | CS_W32_EN`) and a `RELEASE_MEM` (the layout of `gfx_v11_0_ring_emit_fence`) that writes `0xF00DF00D` to a fence in VRAM. Verified on the RX 7800 XT (`dispatch-test`): the fence changes from `0x0BADF00D` to `0xF00DF00D` within a millisecond, the read pointer reaches the end of the 48 dwords, no VM fault.

The SET_SH_REG offset of a compute register is its absolute dword (GC segment 0 base `0x1260` plus the header offset, e.g. `COMPUTE_PGM_LO` 0x1bac) minus `0x2c00`. The `RELEASE_MEM` with `CACHE_FLUSH_AND_INV_TS_EVENT` (0x14) and `EVENT_INDEX` 5 runs when the dispatch has finished, which is what makes the fence mean "the shader is done".

The `PGM_RSRC1/2` values (`0x002C0000`, `0x80`) are plausible defaults, not checked against compiler output; this shader touches no memory and no registers, so the first shader that does will test them.

Not done: a shader that writes memory (needs GFX11 machine code for a global store and a VGPR address), shader memory configuration (`SH_MEM_*`), queue management through MES, GART, interrupts.

### A shader that writes memory (2026-10-30)

With `rednavi_dispatch_store=1` the dispatch of step 5 runs a shader that stores a dword instead of just ending. The machine code is written by hand, one 32-bit word at a time, from the encodings in LLVM's assembler tests (`llvm/test/MC/AMDGPU/gfx11_asm_*.s`, which print each instruction next to its bytes):

| Instruction | Words |
|---|---|
| `s_mov_b32 s0, <address low>` | `0xBE8000FF`, literal |
| `s_mov_b32 s1, <address high>` | `0xBE8100FF`, literal |
| `v_mov_b32 v1, 0` | `0x7E020280` |
| `v_mov_b32 v2, 0x600DC0DE` | `0x7E0402FF`, literal |
| `global_store_b32 v1, v2, s[0:1]` | `0xDC6A0000`, `0x00000201` |
| `s_waitcnt_vscnt null, 0x0` | `0xBC7C0000` |
| `s_endpgm` | `0xBFB00000` |

Verified on the RX 7800 XT: the target dword in VRAM goes from `0xDEAD0000` to `0x600DC0DE`, the `RELEASE_MEM` fence is written, no VM fault. So a real wave runs, executes scalar and vector moves and a global store through the GPU VM (the system aperture on the VRAM range), with `PGM_RSRC1` `0x002C0000` and `PGM_RSRC2` `0x80`, and without any `SH_MEM_*` setup for VMID 0.

`v_mov_b32` encoding: bits 8:0 source (literal 255, inline zero 128), 16:9 opcode (1), 24:17 destination VGPR, 31:25 `0x3F`. `s_mov_b32`: `0xBE80FF00 | ...` with the destination SGPR in bits 22:16 (`0xBE8000FF` is `s0`, `0xBE8100FF` is `s1`).

## M4 step 6: the interrupt handler (2026-10-30)

`rednavi_ih_test=1` (on top of the dispatch test) sets up ring 0 of the interrupt handler (OSSSYS 6.0, `ih_v6_0.c`) in VRAM and makes the fence packet of the dispatch ask for an interrupt. Verified on the RX 7800 XT (`ih-test`): the IH write pointer and its write-back copy in memory both read `0x20`, and the ring holds one vector for the queue under test:

| dword | value | meaning |
|---|---|---|
| 0 | `0x0024b514` | client id `0x14` (GRBM_CP), source id `0xb5` = 181 (`CP_EOP_INTERRUPT`), ring id `0x24` = ME 1, pipe 0, queue 2 |
| 1 | `0xca1b4f94` | timestamp (low) |
| 2 | `0x80000000` | timestamp (high) 0, timestamp source flag |

The setup follows `ih_v6_0_irq_init`: ring off, `nbio_v4_3_ih_control` (no dummy read since there is no MSI and no system dummy page; `IH_REQ_NONSNOOP_EN` because the ring is in VRAM), ring base and size (`IH_RB_CNTL.RB_SIZE` is `order_base_2(bytes / 4)`), `MC_SPACE` 4, write-pointer write-back address, then the ring and the interrupt enable. The CP side needs `TIME_STAMP_INT_ENABLE` in `CP_ME1_PIPE0_INT_CNTL`, and the `RELEASE_MEM` packet `INT_SEL` field set to 2.

Linux keeps this ring in system memory; here it is in VRAM and the IH wrote to it, so VRAM works for `MC_SPACE` 4 with the system aperture on.

The register headers for OSSSYS and NBIO are generated the same way as the GC one (`tools/osssys_regs_spec.txt`, `tools/nbio_regs_spec.txt`, `RedNavi/osssys_regs.h`, `RedNavi/nbio_regs.h`).

Not done: delivering the interrupt to the CPU. There is no MSI yet, so the CPU still reads the write pointer; nothing wakes a thread. The doorbell for the read pointer is off too.

## M4 step 7: doorbells (2026-10-30)

`rednavi_doorbell_test=1 rednavi_doorbell_s2a=1` makes the compute queue of the earlier steps work from a doorbell: BAR2 (the doorbell BAR, 2 MB here) is mapped, the NBIO doorbell aperture is switched on (`RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN.BIF_DOORBELL_APER_EN`), the two doorbell steering entries are written (`S2A_DOORBELL_ENTRY_0_CTRL` 0x30000007 and `_3_CTRL` 0x3000000d, `nbio_v4_3_gc_doorbell_init`), and the queue is programmed with `CP_HQD_PQ_DOORBELL_CONTROL` (`DOORBELL_OFFSET` 6 dwords = slot `mec_ring0` 3, `DOORBELL_EN`), the MEC doorbell range (0 to `userqueue_end` 0x8A, in bytes `*2*4`), `CP_PQ_STATUS.DOORBELL_ENABLE` and wptr polling off. A submission is then a 64-bit write of the wptr to BAR2 + 0x18. Verified on the RX 7800 XT: the scratch register packet, the shader dispatch with its store and fence, and the IH vector all work with no register or shadow write telling the CP about the new wptr.

What cost the time: the S2A registers are about 20 MB into the register space, past the 512 KB BAR. The compat layer reached such registers through `MM_INDEX`/`MM_DATA`, which does not reach them (they read `0xffffffff`), and writing through it blanked the display, so the first attempt looked like "the doorbell breaks the screen". Registers past the BAR are reached through the NBIO's RSMU index/data pair (BAR bytes 0x50 and 0x54, what amdgpu uses on these ASICs); with that, the S2A entries read `0x0` before the write (not configured by the VBIOS) and the write is harmless. `rn_rreg32`/`rn_wreg32` now use it.

Not done: doorbells for the gfx queue, SDMA and the IH read pointer; delivering interrupts to the CPU (MSI); the doorbell index assignment for more than one queue.

## M4 step 8: system memory through the GART (2026-10-30)

`rednavi_gart_test=1` allocates four pages of host memory (`IOBufferMemoryDescriptor`, physically contiguous), gets the address the device must use from an `IODMACommand` (`kIODMACommandOutputHost64`, mapped through the system mapper if there is one; here the bus address is `0x2f383000`, a plain physical address), and writes four system PTEs into the VMID 0 page table in VRAM before `rn_gfxhub_enable`. The GART window is 256 pages at GPU address `0x2000000000`, outside the VRAM aperture, and the PTE flags are `VALID | SYSTEM | SNOOPED | READABLE | WRITEABLE` with the page address in bits 47:12.

Verified on the RX 7800 XT (`gart-test`, `rn_sdma0_run`): SDMA0 writes `0x11223344` to the first dword of the host page (WRITE_LINEAR, CPU reads it back from the buffer), and COPY_LINEAR copies the second dword `0x55667788` from the host page into VRAM; `GCVM_L2_PROTECTION_FAULT_STATUS` stays 0. With `rednavi_dispatch_gart=1` the compute shader of step 5 stores `0x600DC0DE` to GPU address `0x2000000008`, and the CPU reads it from the host buffer (`dispatch-test.target`); the shader and the ring stay in VRAM.

Notes: the page table is cleared before the pages are mapped and not by `rn_gfxhub_enable`, which flushes the L1 TLBs and the L2 when it switches the hub on, so the mapping has to be in place first. COPY_LINEAR layout (`sdma_v6_0_emit_copy_buffer`): header (op 1, sub-op 0), byte count minus one, a flags dword, source address, destination address.

Not done: mapping and unmapping at runtime (a TLB invalidation through `GCVM_INVALIDATE_ENG*_REQ` is needed when a PTE changes after the hub is on), more than a few pages, per-process page tables (VMID > 0), the ring and the IH ring in system memory.

## M4 step 9: the interrupt reaches the CPU as an MSI (2026-10-30)

`rednavi_msi_test=1` (with `rednavi_ih_test=1`) registers a message-signalled interrupt for the GPU, and the dispatch test waits for it instead of polling the fence. The PCI node of the GPU lists two interrupt sources: index 0 of type 1 (level, legacy INTx) and index 1 of type `0x10000` (`kIOInterruptTypePCIMessaged`, the MSI); the others are absent. The first attempt looked for type 0 (edge) and found nothing: for PCI devices the MSI is flagged by `kIOInterruptTypePCIMessaged`, not by the edge type.

The handler is an `IOInterruptEventSource` on the MSI index, run on the PCI work loop. It acknowledges the IH ring by writing the read pointer from the write pointer (`rn_ih_ack`), which re-arms the next message (`IH_RB_CNTL.RPTR_REARM` is set when MSI is on), and counts. Verified on the RX 7800 XT (`msi-test`): `msi_count` 1 and `msi_last_wptr` 0x20 after the fence interrupt of the dispatch, the fence read once afterwards is there, no panic. The fence is not polled: `rn_compute_dispatch_test` takes a wait callback for that.

Not done: more than one interrupt in a row (only one is tested), the other interrupt sources of the IH (page faults, SDMA), an interrupt-driven wait on a thread instead of a sleep loop, and the IH ring in system memory.

### GART mappings at run time (2026-10-30)

With the hub already on (`gart-dynamic`, `gart-dynamic2`, verified on the RX 7800 XT):

- A **new** GART page mapped after `rn_gfxhub_enable` is used correctly by the very next access: SDMA wrote `0xC0FFEE00` through a page that had never been mapped before, and it landed in the host page the PTE names. The PTE written through `MM_INDEX` reads back correct (`0x22188067`: valid, system, snooped, readable, writeable, address bits 47:12).
- **Reusing an address** does not work yet. After pointing an already used GART page at another host page, the next access still goes to the old one, whatever the flush: invalidation engine 17 with the request layout of `gfxhub_v3_0_3_get_invalidate_req` (it acknowledges at once), then also the `GCVM_L2_CNTL2` L1 TLB and L2 cache invalidation that `rn_gfxhub_enable` uses, with 50 ms pauses either side. Not understood. Untested ideas: invalidating from the SDMA ring the way `sdma_v6_0_ring_emit_vm_flush` does, the HDP flush Linux does first, an MMHUB invalidation too, stopping the queue first.
- So the buffer code must hand out GART addresses that are never reused, and an unmap is only safe for an address that will not be mapped again. A 1 TB GART window of 4 KB pages is plenty for a session.

## M4 step 10: buffers, command buffers and the user client (2026-10-30)

Verified on the RX 7800 XT:

- **Host buffer objects** (`RedNaviProbe::createHostBo`): `IOBufferMemoryDescriptor` pages, bus addresses from `IODMACommand`, mapped as consecutive pages of the GART window (now 16384 pages = 64 MB at `0x2000000000`) at addresses that are never reused. The PTEs need `EXECUTABLE` too: without it the command processor, fetching an indirect buffer from such a page, takes a permission fault (`GCVM_L2_PROTECTION_FAULT_STATUS` 0x880: client CPF, read, execute permission), which cost one test run to find.
- **Command buffers** (`rn_compute_submit_ib`): an `INDIRECT_BUFFER` packet (`gfx_v11_0_ring_emit_ib_compute`: address, upper 16 bits, size | VALID | VMID << 24) plus a `RELEASE_MEM` fence, appended to the compute ring by a running write pointer counter, then a doorbell write. `rednavi_bo_test=1` puts a shader and a dispatch command buffer in one host buffer, the result and the fence in another, and the GPU writes `0x600DC0DE` and the fence there (`bo-test`). Both the shader and the command buffer are fetched from host memory through the GART.
- **User client** (`RedNaviClient`, `tools/rn_test.c`): `IOServiceOpen` on `RedNaviProbe`, selectors for buffer creation, submit, fence wait, info and queue diagnostics, `IOConnectMapMemory64` to map a buffer into the app. Administrator privilege is required unless boot-arg `rednavi_uc_anyone=1`. The application side works: it creates two buffers, maps them, writes a shader and a command buffer, and submits. The GPU did not execute that submission, for the reason below.

**Open: a compute queue programmed through the registers does not survive.** The queue of steps 4 to 9 (ME 1, pipe 0, queue 2, written into `CP_HQD_*` by hand, as `gfx_v11_0_kiq_init_register` does for the KIQ) runs every command of the boot-time tests, then stops working: the next doorbell, whenever it comes (1 second or 90 seconds later, from the kernel or from the application), leaves the queue with `CP_HQD_PQ_DOORBELL_CONTROL.DOORBELL_EN` cleared, the read pointer back at 0, `PQ_EMPTY` clear (work pending) and `CP_PQ_STATUS.DOORBELL_UPDATED` set, and nothing executes. Checked and ruled out: the PCI command register (bus master stays on), GFXOFF (`SDMA0_VERSION` still 0x600), a VM fault (status 0), idleness (a resubmission one second after the test fails the same way), the MQD image being overwritten by the firmware (identical before and after), and the doorbell mapping. The firmware of the MEC appears to take back a queue that was not mapped through it. Linux maps compute queues with `SET_RESOURCES` and `MAP_QUEUES` on the KIQ, from a complete MQD.

**`rn_kiq_map_compute` / `rednavi_kiq_test=1`** implements that (RLC_CP_SCHEDULERS, the KIQ programmed through registers on ME 1, pipe 3, queue 0, then SET_RESOURCES and MAP_QUEUES for ME 1, pipe 0, queue 3 with a complete `v11_compute_mqd`). **On the first run it stopped the machine** (no answer on the network, black screen, fans on) at a step that is not known. `rednavi_kiq_step=N` runs only the first N steps (1 RLC_CP_SCHEDULERS, 2 the KIQ registers, 3 the compute MQD, 4 the packets and the KIQ doorbell) and `kiq-progress` shows the step about to run, so a boot with steps 1, 2, 3 in turn finds the one that hangs. A boot-arg left in the OpenCore config that hangs the machine hangs it at every boot: boot from the backup EFI stick, mount the internal EFI and delete `rednavi_kiq_test=1` from `NVRAM > Add > 7C436110-AB2A-4BBB-A880-FE41995C9F82 > boot-args` in `EFI/OC/config.plist` (`config.plist.pre-kiq` is the copy from before it was added).

### KIQ result (2026-10-30)

The earlier hang of `rednavi_kiq_test=1` was a bug in this code, not the hardware: the padding loop of the KIQ command buffer added two dwords at a time to an odd count, never reached a multiple of 8, and overran `pk[24]` on the kernel stack (panic, page fault at `rn_kiq_map_compute+0x443`). With the padding fixed the stages run without a hang or a fault, `RLC_CP_SCHEDULERS` included.

The KIQ still does not execute its commands. With the KIQ at pipe 3 queue 0 (and, in a second run, pipe 0 queue 1), with and without the `RLC_CP_SCHEDULERS` write (`rednavi_kiq_rlc=1`): the ring holds the right packets (read back from VRAM), the doorbell reaches the HQD (`CP_HQD_PQ_WPTR_LO` = 0x18), but the read pointer stays 0 for 200 ms, no fault is reported, and `CP_HQD_PQ_DOORBELL_CONTROL` reads 0x80000000 afterwards: DOORBELL_HIT set and DOORBELL_EN cleared. That is the same state the hand-programmed compute queue ends in after its first post-boot doorbell. So the firmware drops any queue that is not its own on a doorbell. The first hypothesis (the MES drops queues that are not its own) was wrong: the MES was halted (`CP_MES_CNTL` 0x40000000, program counters stuck at 0x800) and nothing was running it. On GFX11 the real KIQ is the second MES pipe (ME 3, pipe 1), not an MEC queue; see the next section.

### MES bring-up (2026-10-30), `rn_mes_start` and boot-arg `rednavi_mes_step=N`

Verified on the RX 7800 XT, following `mes_v11_0.c`. The PSP had already placed the MES images (`CP_MES`, `CP_MES1` and their data); they only need to be started.

1. Step 1, enable: reset both pipes, write the 64-bit start address of each (from the firmware header, `uc_start_addr` 0xfffffffff0005000 for both, shifted right by 2) to `CP_MES_PRGRM_CNTR_START(_HI)` with `GRBM_GFX_CNTL` MEID 3 and the pipe, then write `MES_PIPE0_ACTIVE | MES_PIPE1_ACTIVE`. Result: `CP_MES_CNTL` 0x0c000000, both instruction pointers stable (idle loops).
2. Step 2: `RLC_CP_SCHEDULERS` = (3 << 5) | (1 << 3) | 0x80, the MES KIQ queue (ME 3, pipe 1, queue 0, doorbell `MES_RING1 << 1`) by MQD plus registers. Works.
3. Step 3: MQD of the scheduler (ME 3, pipe 0, doorbell `MES_RING0 << 1`) and a MAP_QUEUES (ME field 2, engine select 5) on the KIQ ring. The KIQ consumes it (`CP_HQD_PQ_RPTR` 8) and the scheduler queue becomes active. Works.
4. Step 4: `SET_HW_RESOURCES` and, because the scheduler version is >= 0x52, `SET_HW_RESOURCES_1`, each followed by `QUERY_SCHEDULER_STATUS` whose fence is the proof. Both complete (fence reaches 2). Works. The frames come from `RedNavi/mes_v11_api_def.h` (MIT, copied from Linux).
5. Step 5: `SET_RESOURCES` (queue mask all ones) on the KIQ ring, then `ADD_QUEUE` with `map_legacy_kq` for ME 1 pipe 0 queue 1. **Does not complete**: the scheduler reads the frame (`rptr` = `wptr`) but never signals the fence, its instruction pointer is not at the idle address (0x2cb8 instead of 0x7834), `CP_HQD_ACTIVE` of the target stays 0, no VM fault. Queue 4 (inside the MES hqd mask 0xf0) behaved the same, so the queue number is not the cause. The MES event log (`disable_mes_log` 0) holds only an echo of the hardware resources.

Notes: the MES KIQ doorbell control reads 0xc0000060 after use (enable and hit set, offset 24), unlike the MEC queues that lose `DOORBELL_EN`. Register reads under `GRBM_GFX_CNTL` while the MES runs can return values of another pipe (version registers swapped between runs), so treat the per-pipe diagnostics with care. Open: what `ADD_QUEUE` for a legacy queue is waiting for. Candidates: a process or gang context, GFXOFF/RLC safe mode around the HQD programming, the MQD content, `SET_RESOURCES` details (cleaner shader address). All unverified.

Tried afterwards without effect on the `ADD_QUEUE` stall (2026-10-30): zeroing the cleaner shader fence area before `SET_HW_RESOURCES_1` (it read 0xffffffff in VRAM), and turning off the `rednavi_queue_watch` and `rednavi_keepalive` timers that rewrite `GRBM_GFX_CNTL` every ~100 ms. What the stalled scheduler shows: its instruction pointer cycles through 0x2cb4..0x2cb8 (a polling loop), the MQD in VRAM is untouched, the target HQD stays inactive. Still untried: a process and gang context via a normal (non-legacy) `ADD_QUEUE`, RLC safe mode around the call, the cleaner shader address in `SET_RESOURCES`, and a different MQD (a dump of what Linux writes would settle it).

### MES: the process and gang path (2026-10-30, later)

`rednavi_mes_step=7` sends `ADD_QUEUE` the way `mes_userq_map` does: a process context buffer and a gang context buffer (4 KB each, zeroed, in VRAM), `process_id` 1, process and gang quanta 100000 and 10000, normal priorities, the queue's MQD (4 KB aligned, a full 512 dword `struct v11_compute_mqd`), its wptr address and doorbell, `page_table_base_addr` set to the GART page table and `vm_context_cntl` read from `GCVM_CONTEXT0_CNTL`, all inside RLC safe mode. The v11 API has no `ADD_PROCESS` or `ADD_GANG` opcode (the contexts are implicit in `ADD_QUEUE`).

**Result: `ADD_QUEUE` completes** (fence 3, completion word 1, the scheduler returns to idle). The legacy mapping (`map_legacy_kq`) of the same queue never completed. Correction (advisor review, 2026-10-31): these are two different firmware paths, and Linux's `map_legacy_queue` sends no process or gang context, so the process path completing does not show that the legacy one was waiting for contexts. It only shows that the firmware acknowledges the process frame. The process path also cannot work with the current VM setup: with `vmid_mask` 0xff00 the MES gives the process a VMID from 8 to 15, and `rn_gfxhub_enable` only configures context 0 (the page table handed over is the flat GART table laid out for the context 0 range), so "no fault" only means nothing was fetched. Verified on the 7800 XT: only that the frame completes.

**What does not work yet:** the queue is not loaded into an HQD. After a doorbell write (dword 8) and 1.5 s, a scan of all ME 1 HQDs (`hqd-scan` property) shows only the two leftovers of the hand-programmed queue (pipe 0 queue 2, and an entry at queue 6, both with doorbell dword 6, rptr = wptr = 0x38); nothing carries doorbell dword 8, no VM fault. Giving `SET_HW_RESOURCES` aggregated doorbells (dwords 100, 102, ...) changed nothing. Also unresolved: the ring, wptr and MQD addresses are VRAM MC addresses, while the process now runs in a VM context whose page table is the GART table, so even a loaded queue might not reach them. Next candidates: map the ring and MQD through the GART (VRAM pages or host pages) and use those VAs, check how the doorbell reaches the MES (aggregated doorbell range and the `CP_MES_DOORBELL_CONTROL` registers), and whether the MES needs `SET_SCHEDULING_CONFIG`. All unverified.

Tried on top of the process path, each with `ADD_QUEUE` still completing and the queue still not appearing in any HQD (`hqd-scan` unchanged, no VM fault): the queue's ring, write-back words, MQD and EOP buffer at GART virtual addresses (six pages of the GART window pointing at the VRAM pages, `rn_gart_map_vram_page`), `SET_SCHEDULING_CONFIG` before `ADD_QUEUE` (completes, fence 4; `rednavi_mes_step=8`), and a doorbell in the user queue range (dword 40, not 8). `CP_MEC_DOORBELL_RANGE` is 0..0x450, which covers all of these. Not yet looked at: how the MES sees a doorbell (the `CP_MES_DOORBELL_CONTROL1..5` registers, the aggregated doorbell mechanism), whether a queue is only loaded when the process quantum timer fires, and whether the doorbell write needs to land in the doorbell BAR page the MES was told about. Unverified.

### MES doorbells (2026-10-31)

Linux puts the MES kernel doorbells (`aggregated_doorbells[i]` = `db_start_dw_offset + 2 * i`) in the page after the kernel doorbells (`amdgpu_doorbell_create_kernel_doorbells`: "an extra page for MES kernel usages"), dword 1024 and up. The aggregated doorbells and the process queue's doorbell (dword 1088) were moved there; `ADD_QUEUE` still completes and the queue is still not loaded. `CP_MES_DOORBELL_CONTROL1..6` read 0 both before and after the doorbell write, selected on each MES pipe, so they are not what carries the queue doorbells here (or they are not readable this way). The NBIO S2A entries are the Linux defaults (`0x30000007`, `0x3000000d`); whether a doorbell in the second page reaches the GC aperture is not checked. The decisive data would be a Linux boot of this card dumping the same registers (`CP_MEC_DOORBELL_RANGE_*`, `CP_MES_DOORBELL_CONTROL*`, the S2A entries, the HQDs of ME 1 and the MES pipes, `RLC_CP_SCHEDULERS`) with `tools/linux_gc_regs.py`, once with a compute workload running.

### Linux reference for the MES (2026-10-31)

`docs/logs/linux-reference/mes_doorbell_regs_linux.txt` and `mes_dmesg_linux.txt`: the same card under Linux 6.x with `vkcube` running (`tools/linux_gc_regs.py` with the GC and NBIO headers). What it shows: `compute_hqd_mask` 0x0c, `gfx_hqd_mask` 0x02, `sdma_hqd_mask` 0xfc, VMID masks 0xff00; `RLC_CP_SCHEDULERS` 0x000030e8; `CP_MEC_DOORBELL_RANGE` 0..0x450; `CP_MES_DOORBELL_CONTROL1..6` all 0 (so they are unused); `CP_MES_CNTL` 0x0c000000; `CP_MES_IC_BASE` and `CP_MES_DC_BASE` programmed; the MES KIQ is `mes_kiq_3.1.0`. The S2A entries cannot be read through debugfs (0xffffffff).

RedNavi now sends `compute_hqd_mask` 0xc on pipes 1 to 3 (pipe 0 left to the hand-programmed queues) and `gfx_hqd_mask` 0x2. `ADD_QUEUE` with process and gang contexts still completes, and a scan of the ME 1 HQDs after the doorbell still shows no queue with our doorbell. The two HQDs it does list (pipe 0, queue 2 and queue 6, both doorbell dword 6, rptr = wptr = 0x38) are not understood: the doorbell field is the hand-programmed queue's and queue 6 is outside any mask, so they may be one HQD seen through an aliased queue select. Open: whether the process queue is loaded only when work is submitted through a doorbell that the MES itself watches, what Linux's `CP_MES_IC_BASE` programming (PSP versus RLC autoload) implies for our MES image, and the interaction of the hand-programmed queues on pipe 0 with the MES scheduler.

Plan after the advisor review: the legacy kernel-queue path (VMID 0) is the target, since it is what Linux uses for its kernel compute rings and what M4/M5 need; the process path is parked. Next experiment: a bring-up with no hand-made queue at all (MEC pipes un-halted and the doorbell aperture and BAR mapped, nothing else), then `rn_mes_start` with `compute_hqd_mask` 0xc on all four pipes and a legacy `ADD_QUEUE` for ME 1 pipe 0 queue 0 with doorbell dword 6 (`mec_ring0 << 1`), the MQD as `gfx_v11_0_compute_mqd_init` writes it, and `bo-test`/`rn_test` as the verdict (the HQD scan is supporting evidence). Also to fetch on Linux: `dmesg | grep "uses VM inv eng"` for the ring names and `uname -r`, and the debugfs `amdgpu_mqd_comp_1.0.0` file if present, to diff Linux's MQD against `writeComputeMqd`.

### Clean legacy experiment and GC-awake test (2026-10-31)

Boot-args `rednavi_mes_clean=1` (no hand-programmed queue; `rn_mec_enable` and the doorbell aperture only; then the MES with `compute_hqd_mask` 0xc on all four pipes and a legacy `ADD_QUEUE` for ME 1 pipe 0 queue 0, doorbell dword 6) and `rednavi_mes_clean=2` (the MES up to step 4 first, then the hand-programmed queue), plus `rednavi_gc_awake=1` (RLC safe mode held for the whole session).

- Legacy `ADD_QUEUE` with nothing else running: **still hangs** the scheduler (fence 2, instruction pointer in the 0x2cb4..0x2cb8 loop). So neither the hand-programmed queues, nor the HQD mask, nor a missing MEC/doorbell setup was the cause.
- A hand-programmed queue created after the MES is up: dies exactly as before (the first resubmit after the boot tests finds `CP_HQD_PQ_DOORBELL_CONTROL` = 0x80000018: hit set, enable cleared). The MES does not change it.
- The same with the RLC safe mode held (GC kept out of GFXOFF): no change. GFXOFF is not the cause of that death.

What stays constant across every configuration: the queue works for the doorbells of the boot tests (compute test, dispatch, IH, MSI, `bo-test`, all pass) and is gone on the first doorbell after the tests, a few seconds later; and the MES legacy map hangs. Unexplored: a time-based mechanism in the MEC firmware (quantum or idle preemption: our MQD sets `CP_HQD_QUANTUM` enable, the register path does not), and the exact MQD Linux uses. The data that would settle it is Linux's MQD for `comp_1.0.0` (debugfs `amdgpu_mqd_comp_1.0.0` if present, or the `umr` tool) compared dword by dword with `writeComputeMqd`.

### Linux's compute MQD (2026-10-31)

`docs/logs/linux-reference/mqd_linux.txt` (Linux 7.0, `vkcube` running): the compute rings are `comp_1.<pipe>.<queue>` for pipes 0 to 3 and queues 0 and 1; `amdgpu_mqd_comp_1.0.0` is the MQD of ME 1 pipe 0 queue 0. Differences found against `writeComputeMqd`: `CP_HQD_PQ_CONTROL` reads 0xf030890a, so `TUNNEL_DISPATCH` is 1 (RedNavi set 0); the quantum dword is 0 (RedNavi enabled it); everything else we set matches (header 0xc0310800, static thread management 0xffffffff, `compute_misc_reserved` 7, persistent state 0x0be05501, `CP_HQD_IB_CONTROL` 0x300000, EOP control 8, mqd control 0x100, doorbell offset 6 with enable). The remaining nonzero dwords (88-91, 114, 146-148, 160, 168-169, 180, pipe and queue priority) are written by the firmware or are priorities. MES and KIQ MQDs are not exposed by this kernel.

RedNavi now uses `TUNNEL_DISPATCH` 1 and a zero quantum for compute queues (the MES queues keep tunnel 0 as in `mes_v11_0.c`). Result on the 7800 XT (checked that the kext on the machine is the build with the change): the hand-programmed queue still dies on the first resubmit after the boot tests, identically. So the MQD content is not the cause.

Everything tried so far that did not change either symptom (queue death after the boot tests, legacy `ADD_QUEUE` spinning): fence area, doorbell page, HQD masks including Linux's 0xc, `SET_SCHEDULING_CONFIG`, aggregated doorbells, GART addresses, process and gang contexts (process path completes but cannot work with the current VM setup), a clean bring-up with no hand queue, the MES started before the hand queue, the RLC safe mode held, Linux's MQD fields. What has not been varied: how the queue is created (always by registers or by a MES frame; never through the KIQ MAP_QUEUES path that Linux used before the MES, which hung the scheduler's own KIQ path differently), and the firmware images themselves (the MES and MEC versions here are newer than the Linux kernel's, `CP_MES_GP3_LO` 0x01026096 against 0x01025086), so a firmware/API mismatch in `ADD_QUEUE` is possible. Unverified.

### MES firmware version (2026-10-31)

The MES images RedNavi loaded so far are the newest in linux-firmware (`cc6d98c1`, 2026-10-16: `CP_MES_GP3_LO` 0x01026096 for the scheduler, 0x06038113 for the KIQ pipe). Linux 7.0 on the same card reports 0x01025086 for the scheduler. Swapping in the pair from linux-firmware `c6feb538` (2026-02-25: `gc_11_0_3_mes_2.bin` and `gc_11_0_3_mes1.bin`, regenerated with `tools/gen_gc_fw_header.py`; not committed) made the machine report **0x01025086 and 0x06038108** (property `mes-fw-version`, read right after the start), so the PSP did take the older images and the version now equals Linux's. Result: legacy `ADD_QUEUE` still never completes (fence 2, the scheduler's instruction pointer at 0x2a3a, a different loop address because it is a different build). So the firmware version is not the cause. Everything checked so far that is part of the MES, the MQD and the frames matches Linux; what is left is context from the rest of the bring-up that Linux does and RedNavi does not (the full CP/RLC initialisation order, the graphics ring being started with the MES, SMU state, interrupts routed to the MES, RLC autoload versus the PSP loads we do one by one). One experiment that would tell: Linux with `amdgpu.mes=0` on this card, if it boots, shows the KIQ-on-MEC queue setup that RedNavi first tried, with its registers.

### Following Linux's gfx_v11_0_hw_init step by step (2026-10-31)

The approach from here: do everything Linux does between the memory hub and the first queue, in its order, and test after each batch. Implemented and run on the 7800 XT, no effect yet on the two symptoms (legacy `ADD_QUEUE` spinning, hand-programmed queue dying on the first doorbell after the boot tests):

1. `rn_gfx_constants_init` (`rednavi_gc_init=1`): `GRBM_CNTL.READ_TIMEOUT` 0xff, golden `TCP_CNTL` bit 29, `DB_RING_CONTROL`, `SH_MEM_CONFIG`/`SH_MEM_BASES` for VMIDs 0 to 15, the KFD VMIDs 8 to 15 (`SH_MEM_BASES` 0x00010002, trap on, no GDS/GWS/OA), the graphics and compute doorbell ranges (0x8b/0xff and 0/0x8a), the GUI idle interrupts off.
2. The RLC save/restore machine on (`RLC_SRM_CNTL` = 3, as `gfx_v11_0_rlc_enable_srm`).
3. Checked against `gfx_v11_0_wait_for_rlc_autoload_complete`: `RLC_RLCS_BOOTLOAD_STATUS` = 0xc000001f (`BOOTLOAD_COMPLETE` set) and `CP_STAT` = 0 before the MES starts, so the RLC and CP state Linux waits for is there.
4. Already identical: the PFP/ME/MEC start addresses and pipe resets (`gfx_v11_0_config_gfx_rs64`), the doorbell aperture, the memory hub.

Not done yet from the same function: `gfx_v11_0_init_csb` (the clear state buffer), `gfx_v11_0_tcp_harvest`, `gfx_v11_0_cp_gfx_resume` (graphics ring with its clear-state start), the interrupt enables. And the power side, which Linux does and RedNavi only partly does: the SMU. Observed in the SMU log of this machine: `SetAllowedFeaturesMaskHigh/Low` are rejected (result -35) while `EnableAllSmuFeatures` succeeds (running features 0x71ffffff / 0x3ebbc), and the SMU mailbox stops answering some seconds after the enable, the same few seconds after which the hand-programmed queue dies. That timing match is the best lead: Linux's `smu_smc_hw_setup` runs many more messages (power limits, DPM tables, the pptable) before the card is considered up.

### SMU setup messages after the feature enable (2026-10-31)

`rednavi_smu_setup=1` (needs `rednavi_smu_enable=1`) sends what Linux's `smu_smc_hw_setup`, `smu_hw_init` and `smu_late_init` send after `EnableAllSmuFeatures` and RedNavi did not, read from `smu_v13_0_0_ppt.c` and `smu_v13_0.c` (SMU 13.0.10 uses the 13.0.0 tables): `TransferTableSmu2Dram` for the combo pptable (accepted, the table lands in the driver table in VRAM; not parsed yet, so `OverridePcieParameters` and the thermal range are still missing), the DPM tables (`GetDpmFreqByIndex` with level 0xff for the count, then every level, for SOC, GFX, UCLK, FCLK, VCLK, DCLK, DCFCLK), and `NotifyPowerSource` (AC). `SetAllowedFeaturesMask` is not sent by Linux when SCPM is on; the rejection we see matches that.

Run on the 7800 XT: every message is accepted. The clock tables read back plausibly (GFX 500 and 2675 MHz at levels 0 and 1, UCLK 96, 456, 772, 1218 MHz, FCLK 8 levels from 601 to 2301 MHz), the mailbox still answered after the sequence and SDMA0_VERSION stayed 0x600. Result for the queues: unchanged. `rn_test` still FAILs, the legacy `ADD_QUEUE` still never completes (scheduler fence 2). So the missing SMU messages are not the cause of either symptom. Still not sent: `OverridePcieParameters`, `SetPptLimit` handling, `smu_set_performance_level` (soft min/max per clock), the thermal setup (`smu_v13_0_start_thermal_controller` writes THM registers), the overdrive default table.

### CP enable and the register diff before ADD_QUEUE (2026-10-31)

Order in Linux's `gfx_v11_0_cp_resume` (checked in the source): doorbell ranges, `cp_compute_enable(true)` and `cp_gfx_enable(true)` (with `amdgpu_async_gfx_ring`), MES KIQ init, `kcq_resume` (the legacy `ADD_QUEUE`), and only then `cp_gfx_resume` with its `CLEAR_STATE`. So the graphics ring's clear state cannot be a prerequisite of the compute map, and `cp_gfx_resume` was not implemented. `gfx_v11_0_init_csb` (which runs earlier, from `rlc_resume`) is still open.

With `rednavi_gc_init=1` RedNavi now also does the two enables. Registers read at the end of the MES start, compared with `mes_doorbell_regs_linux.txt`: `CP_ME_CNTL` 0 (was 0x01000000 until bit 24 was cleared), `CP_MEC_RS64_CNTL` 0x3c000000, `RLC_CP_SCHEDULERS` 0x30e8, `CP_MES_CNTL` 0x0c000000. All four equal Linux now, and legacy `ADD_QUEUE` still never completes (`add_ok` 0, fence 2), `rn_test` still FAILs. They are published in `mes-fw-version`.

### MMHUB bases and the MES data memory (2026-10-31)

Two things tried to find what the scheduler waits for in legacy `ADD_QUEUE`. (1) `reg_offset[MMHUB]` was never filled, so `SET_HW_RESOURCES` gave the MES `mmhub_base` = 0; it is filled now from the IP discovery (property `mes-bases`: `mmhub0` 0x1a000, `mmhub1` 0x2408800). No change: `add_ok` 0, fence 2, scheduler still in its loop (PC 0x2a39). (2) `rednavi_mes_dm=1` reads the scheduler's data memory through `CP_MES_DM_INDEX_ADDR/DATA` (0x5c00/0x5c01, ME 3 pipe 0) after the last frame. Every dword read 0xffffffff, with the MES running and with it halted first, so the registers are not used that way (the Linux driver never touches them); the diagnostic stays opt-in and is not useful as it is. The `rn_mes_result` buffers for it were moved out of the struct after a kernel stack overflow: `sdmaTest` already uses about 12 KB of the 16 KB kernel stack, so nothing large may go into that frame.

### The MES firmware's own log (2026-10-31)

`mes_v11_api_def.h` has carried the v11 debug-extension (`mes_dbgext`) since it was copied: `MESAPI_MISC__SETUP_MES_DBGEXT`, a MISC frame that hands the scheduler a circular buffer (`MES_DBGEXT_INIT_DATA.dbg_ext_mc_addr`) into which the firmware writes text records. Linux arms exactly this at the end of `mes_v11_0_hw_init` (`amdgpu_mes_dbgext_start`, module parameter `amdgpu_mes_dbgext_buffer_size`, default off) and needs nothing else from the driver: the buffer size goes in the first dword, the options word is 0x2 in interrupt mode and 0x3 when polling (bit 1 enables logging, bit 0 in that layout means the driver polls instead of taking an interrupt per record), and the driver only drains the buffer afterwards. This is the one mechanism that shows what the firmware does *inside* a frame, and it runs on the same scheduler ring RedNavi already drives, so it needs no PSP work and no DM registers.

Implemented as `rednavi_mes_dbgext=1` (needs `rednavi_mes_step>=4`): a 16 KB buffer at `base + 0x50000` (below the GART page table at 0x0C100000), the MISC frame sent after the scheduler queue is up and before `SET_HW_RESOURCES`, so the records of `SET_HW_RESOURCES`, `SET_HW_RESOURCES_1` and the `ADD_QUEUE` attempt are all in it. The result of the frame is published as `dbg_ok`/`dbg_status` in `mes-start`, the buffer as `mes-dbg-lo`/`mes-dbg-hi` (first 2 KB, 256 numbers each, keys `l000`..`h3fc`), and `tools/decode_mes_dbgext.py` rebuilds the bytes and prints the records. Its layout is not assumed: the first four dwords are the header the firmware leaves (`rptr`, `wptr`, `buffer_size`, `header_size`), and both record layouts Linux describes are tried (8-byte header with a 24-bit length, 4-byte header with a 16-bit), with the raw bytes printed when neither validates.

Not run on hardware yet. Two outcomes are both informative: a header the firmware wrote and records around the `ADD_QUEUE` attempt (what it waits for, or an assert), or an untouched buffer with `dbg_ok` 0 (the image has no debug-extension support, which closes that road and says nothing about the queue).

Still different from Linux in the same frame: `SET_HW_RESOURCES.gds_size` is 0 here and 0x1000 in `mes_v11_0_set_hw_resources` (from `adev->gds.gds_size`, `gfx_v11_0_...`). Never tried; it is a one-line change if the log does not settle the question.

The data memory (`CP_MES_DM_INDEX_ADDR/DATA`, 0x5c00/0x5c01 at base index 1 = 0xa000) is not the way in: every read returned 0xffffffff with the MES running and halted, which is the same pattern the NBIO S2A entries give when the block is not readable through that aperture, not a value the firmware left behind. The Linux driver never touches those registers either.

### The debug extension completes the frame and writes nothing (2026-10-31)

Run on the 7800 XT with `rednavi_mes_step=5 rednavi_mes_dbgext=1`: `dbg_ok` 1, `dbg_status` 1, `add_ok` 0. The 16 KB buffer at `base + 0x50000` was unchanged: only the seed in the first dword (0x4000), so `rptr` read back as 0x4000, and dwords 1 to 3 zero. No header, no records, after `SET_HW_RESOURCES`, `SET_HW_RESOURCES_1` and the `ADD_QUEUE` attempt.

`dbg_ok`/`dbg_status` say less than they look: the completion word is the API fence the frame carries, so it only says the scheduler consumed the frame, and it carries no result code. A frame that completes and leaves the buffer untouched therefore means one of three things, and the data does not separate them: logging is off, the image has no debug extension and completes every MISC opcode, or the firmware wrote to an address that is not the one we handed it.

The suspects, and what the Linux source says. The options word: the v11 `MES_DBGEXT_INIT_DATA` defines only bit 0 (one host interrupt per record), so the first version sent 0, but `amdgpu_mes_dbgext_start` never sends 0 either. It translates its own options into the layout the firmware reads, where bit 1 enables logging, and passes that word verbatim through `misc_op` into `dbgext_init_data.u64_all`: 0x2 in interrupt mode, 0x3 when polling. Bit 1 is therefore the enable-logging bit, and zero options explain exactly what was seen, a setup handler that returns success and writes nothing at all, not even a header. Both 0x2 and 0x3 were then run on the card and the buffer stayed empty, so bit 1 is right and is not the whole story; the frame now sends 0x3, what Linux sends when it polls.

Two differences from Linux were left, and both are now implemented: Linux puts this buffer in host memory (a GTT BO, handed over as its GART address, unlike the event log, which is in VRAM), and it arms the log after `SET_HW_RESOURCES_1`, not before `SET_HW_RESOURCES`. `rednavi_mes_dbgext=1` allocates a 16 KB host BO in the GART window and passes its GART address, `=2` keeps the VRAM area at `base + 0x50000` for comparison, and either way the log is armed after both resource frames and before the `ADD_QUEUE` frame. `mes-dbg-where` publishes which of the two was used (`host` 1 or 0).

The question-closer on the Linux side, if a Linux boot happens anyway: `amdgpu.mes_dbgext_buffer_size=8` on a kernel that has the feature, then `dmesg | grep dbgext`. That shows whether this exact firmware image logs at all on this card, independent of where RedNavi puts the buffer.

Run of `rednavi_mes_dbgext=1` on the 7800 XT (2026-10-31): with the options word 0x2 and with 0x3 (what `amdgpu_mes_dbgext_start_locked` sends in polling mode: bit 1 logging, bit 0 `host_poll_msg` with inverted sense) the frame is consumed (`dbg_ok` 1) and the 16 KB buffer in VRAM stays exactly as seeded: no header, no record, after `SET_HW_RESOURCES`, `_1` and the failed `ADD_QUEUE`. Linux's own comment says the feature needs an MES image built with debug-extension support; either this release image has none, or the firmware only logs into a host (GTT) buffer. The host buffer and the arm position are implemented since, not run yet.

The decisive question, if it can be asked on Linux: `amdgpu.mes_dbgext_buffer_size=8` on a kernel that has the parameter (`ls /sys/module/amdgpu/parameters | grep dbgext`), then `dmesg | grep dbgext`. That runs the same firmware image with Linux's own driver and its GTT buffer, so it separates "this image has no debug extension" from "RedNavi hands it the buffer wrongly" without another macOS boot. Note the firmware version Linux loads may differ from the one RedNavi's headers carry; if the log works there, the version is worth recording from the same dmesg.

### The MES event log on Linux and on RedNavi (2026-10-31)

Linux 7.0 on this card with `amdgpu.mes_log_enable=1` shows the firmware's event log in `debugfs/dri/N/amdgpu_mes_event_log` (`docs/logs/linux-reference/`, not committed here: the raw dump is in the session notes). Layout seen there, with the same layout in RedNavi's log area (`mes-evlog-a/b`, `rednavi_mes_dm=1`): from +0x1300 a timeline of API frames, 7 dwords each: opcode, start time (64 bit), end time (64 bit), 0, 1; opcode 0xb is QUERY_SCHEDULER_STATUS and 2 is ADD_QUEUE, which takes about 0xE20 ticks on Linux. From +0x900 a table of per-queue records, 8 dwords each, keyed by the queue's doorbell offset in dwords (Linux: 6, 8, 0xa, ... 0x14 for the eight compute rings, 0x116 for the graphics ring): `id, start time, end time, state`. From +0x4000 the scratch area with the headers of the frames the firmware received (`0x000400e1` is a MISC frame, `0x000400b1` a QUERY).

RedNavi's run: the timeline shows SET_HW_RESOURCES, QUERY, the `_1` frame, QUERY and then ADD_QUEUE with a start time and no end time; the queue table has one record, for doorbell 6 (our legacy queue's doorbell), with a start time, end time 0 and state 2 where every Linux record ends with state 3. So the scheduler accepts the frame, creates the queue record and then never finishes mapping it. No MEC queue was active before the map (`hqd_active` 0), so a busy HQD is not the reason. The CP_MES_DM_INDEX registers (0x5c00/0x5c01) read 0xffffffff, the dbgext buffer stays empty (the 7.0 kernel has no dbgext parameter either), and the program counter samples that fill the first dwords of the log area in `log_hist` are ours, not firmware output.

### The legacy queue's MQD against Linux's, and host write-back words (2026-10-31)

`rednavi_mes_dm=1` also publishes RedNavi's MQD as the MES left it (`mes-mqd-a/b`); diffed dword by dword against `mqd_linux.txt` (`amdgpu_mqd_comp_1.0.0`). Every field the driver writes is equal except the addresses (RedNavi: VRAM addresses for ring, rptr and wptr; Linux: host memory for those three, VRAM for the MQD and EOP), the ring size (4 KB against 8 KB, `CP_HQD_PQ_CONTROL` QUEUE_SIZE 9 against 10) and `CP_HQD_PQ_DOORBELL_CONTROL` bit 29 (set on Linux after the doorbell hit; hardware state). The other differing dwords (1, 88 to 91, 114, 133, 134, 146 to 148, 160, 168, 169, 180, 182) are runtime state the CP writes back into a running queue; ours are 0 because the queue never ran.

`rednavi_mes_hostwb=1` puts the rptr report and wptr words of the legacy queue in a host page through the GART, and gives `ADD_QUEUE` that wptr address, as Linux's write-back slots work. No change: `add_ok` 0, fence 2, scheduler PC 0x2a3c. The ring itself is still in VRAM; Linux's is in host memory, the one structural difference left in the queue description.

`rednavi_mes_hostring=1` (2026-10-31) also moves the legacy queue's ring to host memory through the GART, 8 KB (`QUEUE_SIZE` 10) with the write-back words, so the queue description matches Linux's in every address and size. Result: no change, `add_ok` 0, fence 2, scheduler PC 0x2a39. With this the MQD, the frame, the call order, the register state read back (CP, RLC, MES), the register bases handed to the MES, the SMU setup and the MEC queue state are all equal to Linux's, and the scheduler still creates the queue record and never maps it.

### What the scheduler puts on the KIQ ring (2026-10-31)

Reading the KIQ ring after the stuck `ADD_QUEUE` (`mes-kiq-ring`, `mes-kiq-end`) showed what the scheduler does: it appends a standard PM4 `MAP_QUEUES` to the MES KIQ ring (`0xc005a200`, dw1 `0x20000000` = one queue, compute engine, ME 1, pipe 0, queue 0, doorbell 6, our MQD and wptr addresses), padded with NOPs to a 16-dword boundary, and rings the KIQ doorbell. KIQ read and write pointers both end at 0x50, so the KIQ firmware consumed it. The target HQD (ME 1 pipe 0 queue 0) stays in its reset state afterwards (`CP_HQD_ACTIVE` 0, `PQ_BASE` 0, `MQD_BASE` 0). So the part that fails is the KIQ executing the map of a MEC queue, not the scheduler's frame. The Linux KIQ ring (`mqd_linux.txt`) holds only the driver's own packets: the scheduler uses it the same way there and the HQD gets programmed.

`rednavi_mes_selfmap=1` sends that same packet from the driver after the scheduler is stuck: the KIQ read pointer stays at 0x50 (it does not consume the second packet), so after the scheduler's `MAP_QUEUES` the KIQ pipe is blocked, most likely waiting for the MEC pipe 0 to acknowledge the map.

Two differences found by review of the Linux 7.0 sources were also tried, each alone and together, and change nothing: `rednavi_mes_cleaner=1` (the KIQ `SET_RESOURCES` carries a cleaner shader address, here a single `s_endpgm`, as `gfx11_kiq_set_resources` does; `mes-parity` shows the address sent) and `rednavi_mes_hwres=1` (`SET_HW_RESOURCES` with gds_size 0x1000, oversubscription_timer 50, disable_mes_log 1).

Program counters around the map (2026-10-31, properties `mes-mec-pc`, `mes-kiq-pc`): all four MEC pipes (`CP_MEC_RS64_INSTR_PNTR`, ME 1) sit at 0x90f5 before the map and at the same value in three samples after it, so the MEC never leaves its idle loop. The KIQ pipe (`CP_MES_INSTR_PNTR`, ME 3 pipe 1) is at 0x51ad both when idle (after our SET_RESOURCES) and after the failed map, so it is not stuck inside the map either. Reading: the scheduler's `MAP_QUEUES` is consumed by the KIQ pipe, which goes back to idle without touching the target HQD and without waking the MEC, and the scheduler waits for the queue to appear. Why the KIQ firmware drops the packet is not known. The `selfmap` packet sent afterwards is not consumed either (read pointer stays 0x50), so the KIQ no longer follows the doorbell the way it did for the first packets. (An earlier note here said the KIQ blocks waiting for the MEC; the program counters do not support that.)

`rednavi_mes_earlymap=1` (2026-10-31): the same standard `MAP_QUEUES` (ME 1, pipe 0, queue 0, our MQD) is put on the KIQ ring before the scheduler has been given any frame, together with the `SET_RESOURCES` that precedes it in `amdgpu_gfx_mes_enable_kcq`. The KIQ consumes it (read pointer reaches the write pointer) and the target HQD stays in its reset state (`CP_HQD_ACTIVE` 0, `PQ_BASE` 0, `MQD_BASE` 0, `DOORBELL_CONTROL` 0). So the scheduler is not what makes the map fail: the MES KIQ firmware does not program a MEC queue for a `MAP_QUEUES` from RedNavi, with or without the scheduler's involvement, while it does map the MES scheduler's own queue (engine select 5). What Linux has that RedNavi lacks for that case is unknown; the MEC pipes sit idle at 0x90f5 throughout, whether Linux's do is not measured.

### Linux pipe state against RedNavi's (2026-10-31)

`tools/linux_pipe_pcs.py` (debugfs ring-bank select) on the same card: scheduler pipe (ME 3 pipe 0) idle at 0x7104, KIQ pipe (ME 3 pipe 1) idle at 0x518c, the four MEC pipes at 0x9041; `CP_MES_GP3_LO` 0x01025086 (scheduler) and 0x06038108 (KIQ), equal to RedNavi's, so the firmware versions match. RedNavi: scheduler 0x7104 before the first frame and 0x2a3a to 0x2a3c while stuck, KIQ pipe 0x51ad (before and after the map, also with the cleaner shader address in `SET_RESOURCES`), MEC 0x90f5. The idle addresses of the KIQ pipe and the MEC differ by a few instructions with the same MES version, so they are not a clean test of "idle" (the MEC image is a different build, the KIQ one may park in a different spot of the same loop).

Per-pipe MES registers read back on RedNavi (`mes-pipe0-regs`, `mes-pipe1-regs`, 32 registers per pipe): pipe 0 and 1 equal in everything but the program counters, `CP_MES_MIE_LO` (0x31ffe on the scheduler while stuck, 0x1ffe on the KIQ pipe and on Linux at idle) and the MSCRATCH pointer (set only when the event log is on). With `rednavi_mes_hwres=1` the event log and MSCRATCH are now off (the Linux default without `mes_log_enable`); the outcome is the same, `add_ok` 0 and the scheduler at 0x2a3c.

The Linux HQD descriptors (same file) show the target shape: every compute ring of Linux is a queue 0 or 1 of ME 1 pipe 0..3, `CP_HQD_PQ_DOORBELL_CONTROL` 0x60000018 for the one RedNavi maps (doorbell 6), `CP_HQD_PERSISTENT_STATE` 0x0be05501, and the MES KIQ queue has `CP_HQD_PQ_DOORBELL_CONTROL` 0x60 (enable bit clear).

### Firmware parity with the Ubuntu 7.0 package (2026-10-31)

Hashes of the firmware Linux 7.0 (Ubuntu, `/lib/firmware/amdgpu`, decompressed) loads on this card against the set RedNavi had been built with: `gc_11_0_3_mes1.bin`, `gc_11_0_3_mes_2.bin` and `gc_11_0_3_imu.bin` are byte-identical; `gc_11_0_3_pfp.bin`, `_me.bin`, `_mec.bin`, `_rlc.bin`, `sdma_6_0_3.bin` and `smu_13_0_10.bin` were different builds (PFP and ME images of 98144 and 188368 bytes against 92592 and 175616 in Ubuntu's, the MEC image the same size with other contents, SMU 0x505c00 against Ubuntu's 0x505600). `psp_13_0_10_sos.bin` and `_ta.bin` also differ in size from what was in the scratch set (not compared byte for byte, RedNavi's PSP header was generated earlier from a file that is no longer around).

RedNavi rebuilt with the whole Ubuntu GC, SDMA and SMU set: it boots, the RLC autoload completes as before (`bootload` 0xc000001f, `cp_stat` 0), the MEC pipes now idle at 0x9041 like Linux's (they were at 0x90f5 with the old MEC image). Nothing else changes: legacy `ADD_QUEUE` stuck (`add_ok` 0, fence 2, scheduler at 0x2a3a), KIQ pipe at 0x51ad (Linux 0x518c with the same KIQ image). So firmware builds are not the reason either. Not replaced yet: the PSP SOS and TA.

### BREAKTHROUGH: a ring test on the KIQ after the scheduler queue is mapped (2026-10-31)

Legacy `ADD_QUEUE` completes (`add_ok` 1, fence 3, scheduler back at its idle 0x7104, and ME 1 pipe 0 queue 0 is active with `CP_HQD_PQ_DOORBELL_CONTROL` 0x60000018 and `CP_MQD_BASE_ADDR` 0x0c0d2000, the same doorbell control Linux shows for its first compute ring) when the driver runs the same ring test on the KIQ ring that Linux runs (`amdgpu_ring_test_helper` / `gfx_v11_0_ring_test_ring`): `SCRATCH_REG0` set to 0xCAFEDEAD, then a `WRITE_DATA` with write confirm putting 0xDEADBEEF into it through the ring, sent right after the scheduler queue has been mapped and again after `SET_RESOURCES`. Before this, RedNavi checked only the KIQ read pointer after `SET_RESOURCES`. The KIQ pipe's program counter is 0x518c after the first test, the idle value Linux shows for 3000 of 3000 samples; without it the pipe sat at 0x51ad, which was not idle: it stayed parked there and ignored the later `MAP_QUEUES` (also the one the scheduler issues for a legacy queue).

SCRATCH_REG0 still reads 0xCAFEDEAD afterwards (the register write path of that WRITE_DATA is not what mattered; the packet the KIQ pipe executes is). That matches `mes_kiq_pc` and the success of the next map, but why the KIQ needs a normal PM4 packet after the scheduler queue is mapped is not understood: Linux does it as part of `mes_v11_0_kiq_enable_queue` (`amdgpu_ring_test_helper(kiq_ring)`), which RedNavi replicated for the scheduler queue only in the register-polling form.

### The mapped queue after the map: what runs and when it stops (2026-10-31)

Reliability first. With the KIQ ring test of `mes_v11_0_kiq_enable_queue` in place the legacy `ADD_QUEUE` succeeds in about half of the boots (13 of 25 counted since it was added, same kext, same boot-args, identical card state at the start: `mes-init` shows both MES pipes halted at 0x800, no queue active, in every boot) and fails in the other half with the KIQ pipe parked at 0x51ad instead of Linux's 0x518c. `rednavi_gc_awake=1` did not change that in a way that holds (3 of 3, then 0 of 2 with the same arguments); the `rednavi_mes_post=1` boot-arg only seemed to matter. A retry of the map after a pipe reset does not work (8 attempts, all failed even with the KIQ at 0x518c), so the failed state is not recoverable without a new boot. Whatever decides it happens in the first packets the KIQ pipe sees.

When the map succeeds the queue works: a `WRITE_DATA` through its ring and the doorbell right after the map is executed (`0x600DC0DE` in memory, HQD rptr and wptr both 0x10, `CP_HQD_PQ_DOORBELL_CONTROL` 0x60000018 before and 0xe0000018 after, `mes-snap0/1/2`). Doorbells 0.1 s and 0.5 s after the first also run. Later doorbells are not executed and the enable bit of the doorbell control is cleared (0xa0000018): at 2 s in two boots, between 2 and 6 s in the third. With no doorbell at all the enable bit stayed set for 20 s and more (earlier run with one-second samples), so it is not a plain timer; the failing doorbell is one that arrives after the queue has been idle for roughly 1.5 to 4 s. A Linux queue idle for minutes keeps 0x60000018, so what wakes an idle queue is different or missing here. Not found yet. This is the same failure the hand-programmed queues had.
