# Navi 33 / RX 7600 evidence and ASIC profile draft

This directory collects the read-only evidence and documentation-only candidate profile for AMD Radeon RX 7600 (Navi 33, PCI ID `1002:7480`) prepared for RedNavi Issue #7.

## Start here

1. [ASIC profile draft](ASIC_PROFILE_DRAFT.md) — five-key ASIC identity candidate, board metadata, DRM info, known gaps and integration boundary.
2. [Machine-readable candidate](asic_profile_candidate.json) — structured evidence-only profile; it is not consumed by RedNavi.
3. [Generated Linux DRM info snapshot](linux_info_navi33.h) — 440-byte DEV_INFO plus HW_IP_COUNT / HW_IP_INFO / firmware metadata from the captured Linux kernel. Not included by `RedNavi/asic.cpp`.
4. [Firmware inventory](firmware_inventory.md) — installed package candidates, file hashes and source/load-state caveats.
5. [Current-boot firmware runtime evidence](firmware-runtime-current-boot.md) — SMU / DMUB runtime versions and SDMA / VCN ring enumeration; exact firmware filenames remain unattested.
6. [Register-header references](register_header_references.md) — matching CachyOS source header families and Git blob provenance.
7. [Live connector evidence](connectors-live-2026-11-11.md) — RX 7600 `card1` to PCI BDF mapping, DP-1 state and sanitized EDID details.
8. [Linux ABI smoke test](../../../tools/linux_amdgpu_abi_smoke.c) — read-only live DEV_INFO ioctl test against matching kernel UAPI headers.
9. [Validation record](VALIDATION.md) — host tests, live ABI / connector checks, generator reproducibility, hashes and platform limitation.
10. [Raw IP Discovery evidence](rx7600-frame121/README.md) — capture provenance, SHA-256 and parser validation.

## Current identity candidate

| ASIC key | Value |
|---|---:|
| GC | 11.0.2 |
| DMU / DCN | 3.2.1 |
| MP0 | 13.0.7 |
| MP1 | 13.0.7 |
| SDMA0 | 6.0.2 |

The source inventory contains 39 IP records. HW IDs 282 (13.0.2) and 286 (4.0.2) remain semantically unmapped.

## Integration boundary

**Evidence only; not an enabled ASIC.** No RX 7600 row was added to `RedNavi/asic.cpp`; no PCI match was added to either Info.plist; no firmware, register header, display replay or accelerator path was enabled. A card that does not match the live ASIC table must continue to use the existing read-only fallback.

The full RedNavi host test suite has not been validated on this CachyOS host. The Navi 33 structural test, ASIC lookup test, EDID test and mode-register test were compiled and run individually. The full `make test` target remains unvalidated on CachyOS because its compatibility-layer tests hit Linux host-header / errno build issues.
