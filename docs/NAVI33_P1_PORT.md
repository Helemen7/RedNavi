# Navi 33 P1 bring-up: PSP13, GMC11 and GFX11/MES

Status: **host-only port of three P1 subsystems**, 2026-11-05. This branch does not make RX 7600 boot, load firmware or render a frame. It neither enables the PCI ID in the production Kext nor performs MMIO/VRAM writes.

## What was migrated

| Module | New RedNavi file | Original Cobalt64 evidence | Current scope |
| --- | --- | --- | --- |
| PSP 13.0.7 / GC 11.0.2 firmware | `RedNavi/navi33_psp_plan.h` | `FIRMWARE_LOAD_DESIGN.md`, `Navi33Psp13IpFw.h`, `rx7600-fw-plan.tsv` | 9 reference firmware files; SHA-256 *reference values*; 27 image slices; page-aligned 2,801,664-byte staging layout; **SMU-first** PSP LOAD_IP_FW transfer order; no AUTOLOAD_RLC operation |
| GMC11 / GPU VM | `RedNavi/navi33_vm_plan.h` | `Navi33Gmc11Vm.h`, `Navi33Gfx11Vmid.h`, `DMA_QUEUE_BRINGUP.md` | Four-level 4-KiB page-table mapping descriptor, separate table/leaf pages, read/write/execute flags, CPU-visible bounds, forbidden-range and alignment guards; no VRAM allocator or actual VMID/GART updates |
| GFX11 / MES | `RedNavi/navi33_mes_plan.h` | `Navi33GfxMes.h`, `Navi33Gfx11MesStart.h`, `GFX_MAP_DESIGN.md` | Descriptor for two MES firmware start addresses, CP_MES control sequence, and seven-DWORD MES MAP_QUEUES packet; no KIQ ring, hardware queue ownership or command submission |
| Regression testing | `tests/host/navi33_p1_test.cpp`; `make test`; `.github/workflows/navi33-p1.yml` | Cobalt64 host test strategy | Positive/negative host tests and Linux GCC/Clang CI; macOS's existing build workflow also runs `make test` |

These are **independent RedNavi implementations**, using publicly documented Linux register/firmware definitions and source-observed donor facts; they do **not** copy Cobalt64's full drivers or their wire ABIs.

## Specific known hardware results

- Cobalt64 RX 7600 Tahoe GPU-passthrough run (2026-10-18) reached PSP 13.0.7 and successfully loaded the 27 firmware records; its subsequent RLC autoload was accepted. See [donor firmware design](https://github.com/darrenintr/Cobalt64/blob/feature/navi33-dma-backend/src/navi33-bringup/FIRMWARE_LOAD_DESIGN.md).
- Cobalt64 later demonstrated 65,536 correct RADV compute values (2026-10-20), but its GFX HQD remained `MAPPED=0`. The KIQ / graphics queue issue is not solved by the offline packet plan. See [donor acceleration record](https://github.com/darrenintr/Cobalt64/blob/feature/navi33-dma-backend/src/navi33-bringup/ACCELERATION.md).
- RedNavi's current hardware paths, including `RedNavi/gfx11.cpp` and `RedNavi/kern_probe.cpp`, remain specialized for **Navi 32 GC 11.0.3**. Do not call their methods with Navi 33 register offsets or firmware blobs.

## Explicit limitations / safety

1. The PSP planner accepts caller-provided, externally computed SHA-256 strings. It only compares them with reference hashes; it **does not calculate the hash of firmware bytes or validate cryptographic signatures**. A future loader must authenticate bytes independently and enforce staging lifetime/ownership.
2. The GMC planner returns page-table *descriptions* with VRAM-relative offsets; it does not allocate/lease VRAM or prove actual GPU MC addresses, IOMMU translations, correct CPU cache synchronization, a matching VMID or an invalidated TLB. The `externallyOwned` flag is test data, not a security or ownership check.
3. The MES planner emits an unsubmitted register/PM4 *description*; it does not imply a working scheduler, correctly pinned MQD, GFX queue, fence or reset strategy. The `exclusiveOwner` flag alone is never proof of hardware ownership.
4. All three modules have explicit compile-time `k*HardwareWritesEnabled=false` properties and are **not included by any production Kext source**. No non-read-only probe or boot-arg is enabled.
5. Neither OpenCore GOP framebuffer nor SSH access alone counts as independent recovery on the user's sole RX 7600. Do not activate writes without an owner-safe recovery plan.

## Next engineering steps

1. Build a **per-ASIC table** per [RedNavi upstream issue #7](https://github.com/Helemen7/RedNavi/issues/7): keep Navi32 unchanged and list Navi33 as read-only diagnostic only. Keep register maps selected from real IP Discovery versions, not just PCI ID.
2. For PSP, reimplement a protected loader adapter using the real Linux GC 11.0.2 / PSP 13.0.7 firmware headers and the donor's verified SMU-first ordering; unit-test all rejection cases, then validate on **a separate, recoverable RX 7600 test setup**. The host planner deliberately omits `AUTOLOAD_RLC` because it changes GPU state.
3. For GMC, add exact gfx1102/MMHUB3.0.2 register maps, VRAM reservation/lease and ownership-proven BAR0 mappings. Validate two hub contexts, page-table invalidation ACK and fence/pinned-memory lifecycle before enabling a VMID.
4. For MES, compare RedNavi's working Navi32 KIQ/MES pipeline with the RX 7600's documented G42 `MAP_QUEUES` timeout. Host-check ring NOP padding (256 DWORDs), doorbell remap, MQD fields, firmware version, memory type and GPU-visible fence; do not speculate that changing a constant will fix it.
5. Only after actual RX 7600 memory, compute and graphics tests pass, wire the existing RedNavi RADV/Metal stack to the `gfx1102` device. Prove a real rendered pixel readback and later WindowServer presentation independently.

**Acceptance before a merge to main:** host CI and original Navi32 CI green; no changes to production Kext matching/boot behavior; a clear staged plan for future hardware checks. Actual native RX 7600 hardware acceleration remains unimplemented.
