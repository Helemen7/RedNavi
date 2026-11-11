# RX 7600 firmware runtime evidence — 2026-11-11

**Device:** RX 7600, PCI BDF `0000:03:00.0`, PCI ID `1002:7480`
**Kernel:** CachyOS `7.3.1-1-cachyos`
**Capture host:** `cachyos` (the 20:15 boot associated with the DRM capture)
**Capture:** current boot kernel journal, read-only; filtered lines are preserved in `firmware-runtime-current-boot.log`.

## Observed runtime results

- Linux selected the generic IP block implementations `psp_v13_0_0`, `smu_v13_0_0`, `gfx_v11_0_0`, `sdma_v6_0_0`, `vcn_v4_0_0`, and `jpeg_v4_0_0`.
- DMUB was reported as loaded via PSP with runtime version `0x07003600`; the kernel later reported DMUB hardware initialized with the same version.
- VCN reported ENC 1.24, DEC 9, VEP 0, revision 55.
- SMU reported firmware version 82.97.0 and initialized successfully.
- The kernel enumerated `sdma0`, `sdma1`, `vcn_unified_0`, and `jpeg_dec` rings.

## What this proves — and what it does not

This is stronger runtime evidence than package presence alone: the running Linux driver initialized SMU and DMUB and enumerated SDMA / VCN-related rings on this RX 7600.

It still does **not** identify the exact firmware filename requested for each subsystem. The kernel log reports selected IP blocks and runtime versions, but does not print the requested blob paths for all components. Therefore the nine SHA-256-verified package files in `firmware_inventory.md` remain source-matched candidates, not per-file load attestations. Do not populate RedNavi's `rn_asic.firmware` field solely from this log.

The retained lines all come from host `cachyos` and the 20:15 boot associated with the DRM capture. A separate 14:25 excerpt from host `cachyos-x8664` was excluded because it belongs to a different boot. The capture boot contains an initial timestamp of 20:15:38 +07 while the host clock was later corrected by time synchronization; do not correlate wall-clock timestamps with other captures without checking the boot ID / monotonic timestamps.

No GPU registers were read or written by this capture. No firmware was copied, replaced, loaded manually, or modified. No GPU workload was submitted and no reset or reconfiguration was performed.
