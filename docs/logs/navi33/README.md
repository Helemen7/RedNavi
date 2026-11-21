# Navi 33 / RX 7600 evidence index

The authoritative interpretation and integration boundary are in [ASIC_PROFILE_DRAFT.md](ASIC_PROFILE_DRAFT.md). This directory contains supporting evidence and focused validation records.

## Primary documents

- [ASIC profile draft](ASIC_PROFILE_DRAFT.md) — candidate five-key identity, board facts, DRM information, limitations and integration boundary.
- [Validation record](VALIDATION.md) — exact host tests and live Linux checks, including what remains unvalidated.
- [Firmware inventory](firmware_inventory.md) — installed candidates, hashes and load-attestation limits.
- [Current-boot firmware runtime evidence](firmware-runtime-current-boot.md) — 2026-11-11 SMU / DMUB versions and SDMA / VCN ring enumeration.
- [Latest firmware runtime evidence](firmware-runtime-2026-11-14.md) — current kernel/module metadata, PSP filesystem check and runtime messages; preserves the unresolved TA provenance gap.
- [Register-header references](register_header_references.md) — header families, provenance and unresolved metadata discrepancy.
- [CachyOS kernel source cross-check](cachyos-kernel-source-crosscheck-2026-11-11.md) — source-level interpretation of DRM query semantics and VCN 4.0.
- [Live connector evidence](connectors-live-2026-11-11.md) — card-to-PCI mapping, DP-1 status and sanitized EDID details.

## Captures and generated artifacts

- [Raw IP Discovery capture and provenance](rx7600-frame121/README.md) — binary capture, checksums and parser validation.
- [Decoded IP Discovery inventory](ip_discovery.txt) — 39 parsed IP records.
- [Raw DRM_AMDGPU_INFO answers](rx7600-frame121/amdgpu_info_raw.txt) — read-only Linux ioctl responses.
- [Generated Linux DRM info header](linux_info_navi33.h) — 440-byte DEV_INFO and captured metadata; not included by the live ASIC table.
- [Linux ABI smoke test source](../../../tools/linux_amdgpu_abi_smoke.c) — parameterized, read-only DEV_INFO ioctl check.
