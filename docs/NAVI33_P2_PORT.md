# Navi 33 P2 port: RADV gfx1102 metadata and DCN 3.2.1 display groundwork

Status (2026-11-05): **source port and host-tested implementation**, stacked **on** `feature/navi33-p1-port`, not on `main`. This is intentionally not a functional RX 7600 macOS driver. The code is not wired into RedNavi's running Kext, Mesa DLL or the IOFramebuffer. It cannot enable Metal, provide accelerated desktop composition or replace the UEFI GOP framebuffer.

## Base and provenance

- Base: `darrenintr/RedNavi:feature/navi33-p1-port` (the PSP13 firmware, GMC11 VM and GFX11/MES host planners from [PR #2](https://github.com/darrenintr/RedNavi/pull/2)).
- Cobalt64 source: `feature/navi33-dma-backend` at `e431bd9a0a07d654aea38bb54b57c47e4120c02f` and DCN fork `feature/navi33-dcn321-native-modeset` at `2f12d46084ce9d1a88b9c8cd629b4c6aef432a9f`.
- Mesa references: donor `mesa-patches/0010-*` (gfx1102 selection by GC IP version) and `0011-*` (low virtual-address range when the kernel has no high VA), together with Cobalt64's 2026-10-20 RADV GPU-passthrough compute records.
- DCN references: donor `src/dcn321/dcn321_regs.h`, `dcn321.c`, host-mode tests and Linux `dcn_3_2_1_offset.h` / `dcn_3_2_1_sh_mask.h`.
- RedNavi is BSD-3-Clause; donor and public Linux register definitions are MIT. Modules are independently implemented, not a bulk source import; preserve the source/provenance notes.

## P2 code now available

### A. gfx1102 RADV admission

`mesa-bridge/include/rn_navi33_radv_admission.h` is a portable, fail-closed metadata checker:

- Requires real reported PCI vendor/device `1002:7480`, **GC 11.0.2** (`0x0b0002`), matching Linux DEV_INFO identity and GC11 family, nonzero reported CU and RB counts.
- Rejects canned Navi32 (`0x747e`) or gfx1201 (`family 152`) info. A forced Mesa canned device or a selected GC version alone is not an identity proof.
- Accepts the *shape* of a low-only VM when `high_va_first==high_va_max==0`, as in the Cobalt64 `0011` case. Checks the low range against the first 47 bits; rejects bad alignment, overlapping, half-specified or inconsistent high-VA ranges.
- Distinguishes valid metadata from a genuinely tested runtime backend: `native_backend_verified` must be supplied by a separate verifier, and even when set, `contract.can_submit` remains **false**. This is a data contract, **not** an RDNA3 RADV execution backend.
- Does not modify the production `mesa-bridge/patches/0001-radv-darwin-iokit.patch` or the Navi32 `ac_apple_iokit.c`; that legacy Mesa code and the donor's N48N backend have different UAPIs. Any future Mesa port must implement the real gfx1102 device path and keep a single source-of-truth for the in-tree and standalone bridges.

### B. DCN 3.2.1 read-only snapshot and flip preflight

`RedNavi/navi33_dcn321.h` is a platform-independent C++17 module:

- Defines **per-pipe** OTG and HUBP/HUBPREQ register addresses for all four DCN 3.2.1 pipes using IP Discovery **DCN segment 2**, rather than importing the Navi32 register replay. All offsets are dwords converted to BAR byte offsets at the read boundary.
- `snapshot()` reads a narrow, bounded set of clocked/lively-pipe values through a user-supplied callback. It only runs when the caller separately attests that the selected pipe is alive and safe to sample. Reads themselves can still hang on a wrongly identified power-gated block; **no adapter is connected to live hardware**.
- Extracts OTG clock/enable, frame counter, update/flip pending, linear pixel layout, viewport, pitch, and surface MC address.
- `planFlip()` validates a caller-claimed pinned VRAM surface, pitch, alignment, visible geometry, format, DCC/TMZ, rotation, update locks and VRAM bounds. Returns the **high-then-low MC-address pair**; it has no register-write mechanism.
- Neither a successful snapshot nor a computed flip means DP/HDMI link training, modesetting, VBlank interrupt, IOFramebuffer registration, GPU acceleration or WindowServer presentation works.

### C. Host testing and CI

- `tests/host/navi33_p2_test.cpp`: positive and negative RADV metadata and VA cases, synthetic 4-pipe register map readout, simulated frame-counter wrap, BAR safety, blocked inactive pipes, VRAM/pitch/lock/format/recovery guards.
- `Makefile`: includes P2 under existing `make test`, which also reruns P1.
- `.github/workflows/navi33-p2.yml`: GCC and Clang with `-fsanitize=address,undefined`. The existing macOS build workflow continues to compile Debug/Release Kexts and run all host tests.

## What is NOT done

1. Nothing is installed or loaded onto the RX 7600; no new PCI device match, kernel BAR writes, boot-args, KC injection, modified VBIOS or GPU reset.
2. No actual RADV gfx1102 device enumeration through RedNavi. Current Mesa build and native RedNavi `RN_UAPI_VERSION=11` still serve the Navi32 path. Cobalt64's N48N donor transport is incompatible and is not cherry-picked.
3. No DCN321 hardware writer, clock/DMUB/HPD/AUX/link training, safely owned scanout buffer, vblank/synchronization, surface flipping or native macOS framebuffer support.
4. Caller-provided assertions such as `native_backend_verified`, `safeReadConfirmed` and `independentlyPinned` are **untrusted flags**, not actual proof. They must only be set by independently implemented, auditable runtime ownership and probe checks, with a separate recovery GPU/device.
5. No Metal / AIR → SPIR-V port or verified offscreen pixel readback yet.

## Next milestones / acceptance gate

1. Implement a per-ASIC kernel data path with **read-only** RX7600 hardware identification, including genuine per-device `DEV_INFO`, GC IP version, active VRAM/GTT and memory ranges, avoiding hardcoded Navi32 data. Ensure the same metadata reaches the Mesa patch's implementation, not only a standalone test helper.
2. Complete P1 actual hardware bring-up through independently recoverable hardware and verified firmware, GMC11 page tables/VMID, GPU ownership, GFX11 MES/KIQ fences.
3. Port the RADV gfx1102 winsys using RedNavi's own UAPI, with stable buffers, synchronized VA, a real compute submission and 65,536-word readback equivalence to Cobalt64. Then prove GFX queue mapping and a physically GPU-rendered triangle.
4. Only after the rendering backend works, safely bring up DCN321 display/link and WindowServer-compatible scanout/flip. Do **not** use the sole live display GPU to test register writes without an independent reset path.

**Merge policy:** keep this as a separate Draft PR based on P1, not `main`; preserve clean upstream Navi32 functionality and require CI green before merging in dependency order.
