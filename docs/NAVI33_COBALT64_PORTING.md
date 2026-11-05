# Navi 33 (RX 7600) port audit: Cobalt64 -> RedNavi

Status: first-pass source audit, 2026-11-05. **No Navi 33 hardware support is enabled by this change.** This is a plan for the personal RedNavi fork's `Exploration` branch, not evidence that RedNavi runs on an RX 7600.

## Source revisions and limits

- Destination: [darrenintr/RedNavi](https://github.com/darrenintr/RedNavi), based on upstream [Helemen7/RedNavi](https://github.com/Helemen7/RedNavi) at `d6a8da73bf337d059aa35749d23738de16e1116d`. The original fork and its `Exploration` branch were identical to upstream when this audit began.
- Donor: [Cobalt64 `feature/navi33-dma-backend`](https://github.com/darrenintr/Cobalt64/tree/e431bd9a0a07d654aea38bb54b57c47e4120c02f) at `e431bd9a0a07d654aea38bb54b57c47e4120c02f` (2026-10-21).
- DCN donor: [Cobalt64 `feature/navi33-dcn321-native-modeset`](https://github.com/darrenintr/Cobalt64/tree/2f12d46084ce9d1a88b9c8cd629b4c6aef432a9f) at `2f12d46084ce9d1a88b9c8cd629b4c6aef432a9f` (2026-10-17).
- Source-code licences differ (Cobalt64: MIT; RedNavi: BSD-3-Clause). Follow RedNavi's `CLAUDE.md` policy: **do not paste donor source wholesale**. Use documented hardware facts and Linux amdgpu MIT definitions to implement and test an independent RedNavi backend. Keep attribution and notices where required; do not commit firmware binaries, firmware-derived headers, VBIOS dumps or unredacted EDIDs.

## Evidence, not assumptions

- **Cobalt64 GPU passthrough, 2026-10-18:** its Tahoe VM test records show PSP 13.0.7 accepting the Navi 33 IP firmware sequence, with SMU loaded first, followed by the other images and `AUTOLOAD_RLC`. Source: [FIRMWARE_LOAD_DESIGN.md](https://github.com/darrenintr/Cobalt64/blob/feature/navi33-dma-backend/src/navi33-bringup/FIRMWARE_LOAD_DESIGN.md). This is not a fresh RedNavi test or a bare-metal result.
- **Cobalt64 GPU passthrough, 2026-10-20:** `ACCELERATION.md` records two RADV compute runs with 65,536 correct words each; later 0.0.55/0.0.56 tests repeat compute and 200 submissions. GFX mapping still failed: `GFX_HQD_ACTIVE=1`, `MAPPED=0`; no Metal-rendered frame or GPU-composited desktop was demonstrated. [Record](https://github.com/darrenintr/Cobalt64/blob/feature/navi33-dma-backend/src/navi33-bringup/ACCELERATION.md).
- **RedNavi RX 7800 XT:** Vulkan compute/graphics, a single-display Vulkan swapchain and the first Metal compute route are documented on Navi 32. These are **not** Navi 33 hardware results. [RedNavi roadmap](https://github.com/Helemen7/RedNavi/blob/main/docs/ROADMAP.md).
- **Cobalt64 DCN 3.2.1:** the independent layer is mock-tested and its register subset is sourced from Linux; it has not demonstrated GOP-independent macOS display output on an RX 7600. [DCN321 README](https://github.com/darrenintr/Cobalt64/blob/feature/navi33-dcn321-native-modeset/src/dcn321/README.md).

## Port candidates in priority order

| Priority | Donor: Cobalt64 | RedNavi destination | Transfer and acceptance criteria |
| --- | --- | --- | --- |
| P0 | `tests/rx7600_ipd_fixture.h`, IP discovery captures, silicon-version tests | New isolated Navi 33 facts and `tests/host/`; eventually per-ASIC table in `RedNavi/` | Bring in *hardware facts and negative tests*, not guessed runtime support. Match PCI `1002:7480` and GC 11.0.2, SDMA 6.0.2, PSP/SMU 13.0.7, DCN 3.2.1, MMHUB 3.0.2. Reject Navi 32. Keep all writes off. |
| P1 | `FIRMWARE_LOAD_DESIGN.md`, `Navi33Psp13IpFw.h`, `fwplan.py`, firmware-plan fixtures | `tools/gen_*_fw_header.py`, `RedNavi/kern_probe.cpp`, new pure firmware-planning tests | Reimplement the *27-record plan*, SMU-first order, segment offsets and `AUTOLOAD_RLC` gate. Validate with synthetic/copyright-safe test data and the donor record manifest; no firmware blobs checked in. Existing 7800 XT PSP path must remain unchanged. |
| P1 | `Navi33Gmc11Vm.h`, `Navi33Gfx11Vmid.h`, `Navi33DarwinVmGradient.cpp`, `DMA_QUEUE_BRINGUP.md` | Separate gfx1102 MMHUB/GFXHUB register definitions, memory and VM tests; eventually `RedNavi/gfx11.cpp` | Use per-IP register maps. Verify physical VRAM windows, host-memory DMA mappings, TLB ACKs, persistent buffer lifetime and quarantine. Do **not** assume CPU-physical address equals a GPU IOVA. |
| P1 | `GFX_MAP_DESIGN.md`, `Navi33Gfx11MesStart.h`, `Navi33GfxMes.h`, draft Route B PR #22 | gfx1102-specific setup for RedNavi's MES / graphics-queue path | Preserve RedNavi's working Navi 32 scheduler implementation. Compare MQD, doorbells, KIQ padding and `MAP_QUEUES` against Cobalt64's failed G40/G42 traces. Route B is offline/host-tested only; no claim of working Navi 33 GFX queue. |
| P2 | Mesa patches `0010` and `0011`, `Navi33N48nClient`, `ACCELERATION.md` | `mesa-bridge/`, per-ASIC `DRM_AMDGPU_INFO` and `RedNavi/rn_uapi.h` | Reuse gfx1102 *identity, caps and validated test cases*, not the Navi48/N48N wire ABI. Query genuine firmware/CU/VRAM information; never present a Navi 32 canned device as Navi 33. Prove repeated Vulkan compute and then 3D pixel readback. |
| P2 | `src/dcn321/dcn321_regs.h`, `dcn321.c`, DCN321 mock tests | A separate Navi 33 DCN register/mode module; later `RedNaviFB` | Port the host-tested bounds, timing and plane/flip guards after checking Linux definitions. Do not transplant Navi 32's DP4/DIG4 replay onto an HDMI/DIG1 board. No single-GPU modeset without a known recovery/display path. |
| P3 | Cobalt64 Metal proof harness, `tools/native/navi33metal/` | RedNavi's existing `RedNaviMTL/` and acceptance tests | Prefer RedNavi's own newer Metal/RADV path. Adapt *proof conditions* (real gfx1102, compute, draw, IOSurface, WindowServer), not the Navi48-specific Metal bundle. Only publish a device after genuinely GPU-backed command submission. |

The donor's C++ classes, kernel callbacks and user-client structs are not ABI-compatible with RedNavi's `RN_UAPI_VERSION=11`. This is a subsystem port, **not** a git-merge or cherry-pick of the Cobalt64 bring-up branch.

## First implementation slice in this fork

- `RedNavi/navi33_facts.h` is a new **unconnected**, host-portable description of a verified Navi 33 IP-version fingerprint. It cannot perform GPU I/O and explicitly keeps write enablement false.
- `tests/host/navi33_facts_test.cpp` checks the known fingerprint and rejects a wrong GPU/device or key IP versions.
- `make test` runs the additional host-only test. The production Kext, its personalities, PCI match list, firmware setup, and RADV behaviour are unchanged.

## Proposed next stages / stop conditions

1. **Per-ASIC framework:** create the table proposed in [upstream issue #7](https://github.com/Helemen7/RedNavi/issues/7), keep `1002:7480` in read-only diagnostic mode; run the existing RX 7800 XT host tests unchanged.
2. **PSP/firmware offline tests:** independent Navi 33 firmware plan reproduces Cobalt64's validated segment order; reject wrong signatures, lengths, ranges or firmware versions.
3. **RX 7600 non-destructive observation:** verify real PCI BAR mappings and live IP Discovery / PSP / VRAM / queue snapshots on the exact hardware and boot mode. Distinguish GPU passthrough from bare-metal results.
4. **Safe GPU execution gate:** before any write, obtain exclusive GPU/VRAM/queue ownership, a verified DMA mapping and recoverable reset/display path. The RX 7600 is the user's sole graphics card; console/SSH access is not independent hardware recovery.
5. **Compute parity:** reproduce the donor's 65,536-word RADV test and repeated submission checks without faking the device; record kernel fence retirement and verified buffer contents.
6. **Graphics parity:** solve the GFX queue mapping failure, submit a triangle, read back and verify the actual GPU-rendered pixels.
7. **Display then Metal:** validate Navi 33 DP/HDMI, mode/flip and GPU-owned surfaces, followed by RedNavi's Metal pipeline, IOSurface and WindowServer. None of these achievements are implied by firmware loading or compute success.

**Do not** add the RX 7600 PCI ID to an active register-writing Kext personality, reuse Navi 32 firmware headers/replay tables, set a Metal-ready capability based on a compiler/CI success, or execute Cobalt64's experimental Queue0/Queue1 writer on the sole display GPU.
