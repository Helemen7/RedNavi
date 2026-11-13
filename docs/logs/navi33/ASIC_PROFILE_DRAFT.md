# Navi 33 ASIC Profile — Draft for RedNavi Issue #7

**Status:** evidence-backed draft; not an enabled ASIC row
**Contributor:** Frame121
**Candidate GPU:** AMD Radeon RX 7600 (Navi 33)
**PCI identity:** vendor/device `1002:7480`, subsystem `148c:2421`, revision `0xcf`, BDF `0000:03:00.0`
**Host:** CachyOS Linux x86_64, kernel `7.3.1-1-cachyos`

> Safety / integration boundary: this document does not add RX 7600 to `RedNavi/asic.cpp`, either Info.plist match list, or any accelerator match list. The RX 7600 must remain on the existing read-only fallback until the maintainer has reviewed the profile and all write-stage dependencies are selected by ASIC. Do not use this draft to enable hardware writes.

## 1. Five-key ASIC lookup candidate

RedNavi's current lookup key uses five IP Discovery versions: GC, DMU (published as DCN in the row), MP0, MP1, and SDMA0.

| RedNavi lookup field | IP Discovery HW ID | RX 7600 observed version | Evidence |
|---|---:|---:|---|
| `gc` | 11 | **11.0.2** | RX 7600 IP Discovery, 39-record inventory |
| `dcn` (DMU) | 271 | **3.2.1** | RX 7600 IP Discovery, 39-record inventory |
| `mp0` | 255 | **13.0.7** | RX 7600 IP Discovery, 39-record inventory |
| `mp1` | 1 | **13.0.7** | RX 7600 IP Discovery, 39-record inventory |
| `sdma` (SDMA0) | 42 | **6.0.2** | RX 7600 IP Discovery, 39-record inventory |

These values are observed on the contributor's RX 7600 capture. They are a candidate match key, not permission to enable the ASIC.

## 2. Board identity candidate

| Field | Observed value | Integration status |
|---|---|---|
| Model | AMD Radeon RX 7600 | Observed |
| PCI device ID | `0x7480` | Observed; do not add to match list yet |
| Vendor ID | `0x1002` | Observed |
| Subsystem ID | `0x148c:0x2421` | Observed |
| PCI revision | `0xcf` | Observed |
| VRAM | 8 GiB class / 8192 MiB reported by host inventory | Candidate; confirm DEV_INFO interpretation before using as board `vram_mb` |
| Display replay | Not selected | Requires a safe, verified display-pipe capture |
| DisplayPort connector | `card1-DP-1` connected on this capture | Direct DRM status + EDID; does not establish AUX / PHY / HPD / lane register values |
| DisplayPort path | Not selected | Requires a separately validated DCN pipe / PHY mapping before any replay is proposed |
| HDMI path | Disconnected in the live capture | Older connector snapshot reports HDMI-A-2; treat as a separate capture, not proof of the current DP-1 path |

No `rn_asic_board` initializer is proposed yet. In particular, do not reuse the RX 7800 XT's DP4 / UNIPHY2 / AUX4 / HPD5 / four-lane path or its replay table.

## 3. IP inventory highlights

The source inventory contains 39 records across one die. Binary and IP-table checksums pass. The prior RedNavi Navi 33 capture comparison reports 39/39 matching keys, versions, and base-address lists.

The two records that still lack semantic names are intentionally kept numeric:

| HW ID | Instance | Version | Status |
|---:|---:|---:|---|
| 282 | 0 | 13.0.2 | Unmapped; do not guess |
| 286 | 0 | 4.0.2 | Unmapped; do not guess |

The complete per-record inventory is in `docs/logs/navi33/ip_discovery.txt`; the raw IP Discovery capture and its provenance are under `docs/logs/navi33/rx7600-frame121/`. Base addresses are metadata only; this draft does not authorize register access.

## 4. DRM_AMDGPU_INFO / DEV_INFO

Read-only capture source:

`docs/logs/navi33/rx7600-frame121/amdgpu_info_raw.txt`

- Device node: `/dev/dri/renderD128`
- `ACCEL_WORKING`: successful query, value 1. This is a driver status flag, not proof of a submitted/completed workload.
- `DEV_INFO`: successful response, **440 bytes** in this capture.
- `HW_IP_COUNT`: successful types 0, 1, 2, 6, 7, 8; unsupported types return errno 22.
- `HW_IP_INFO`: instance 0 responses captured for all ten type IDs; unsupported queries may return zero-filled structures. Instance 1 returns errno 22 under this UAPI.
- `MAX_IBS`: 40 bytes (10 little-endian u32 values).
- `FW_VERSION`: 45 response records were captured; generator selects index 0 per type.

**DEV_INFO size investigation — resolved for this capture:** The matching CachyOS 7.3.1-1 UAPI header defines `struct drm_amdgpu_info_device` as 440 bytes; a small C compile-time/runtime size probe against that exact header reports `sizeof(...) == 440`. The older Navi 32 generated header is 448 bytes because it came from a different kernel UAPI snapshot; that size must not be imposed on every ASIC. RedNavi stores an explicit `rn_drm_info.dev_info_size`, and `infoQuery()` copies `min(caller_buffer_size, captured_size)` after zeroing the caller buffer, so the kernel-side response path is length-aware. The generated Navi 33 header below therefore retains all 440 captured bytes. A live Linux userspace ABI smoke test against the matching 440-byte kernel-build UAPI now passes; see `tools/linux_amdgpu_abi_smoke.c` and `VALIDATION.md`. A macOS-side `infoQuery()` integration test remains required before enabling the row. The separately installed libdrm header on this host describes a 448-byte layout; it was not substituted for the matching kernel-build UAPI definition.

The raw query answers are committed alongside this document in `rx7600-frame121/amdgpu_info_raw.txt`. Reproduction command (run from the RedNavi repository root): `python3 tools/gen_linux_info_header.py docs/logs/navi33/rx7600-frame121/amdgpu_info_raw.txt docs/logs/navi33/linux_info_navi33.h navi33`. It produced a 440-byte DEV_INFO array and 16 unique firmware metadata types (index 0 per type). A host structural test checks the generated lengths and PCI ID. No firmware image is included in this draft.

## 5. Firmware observations — not a firmware selection list

The DRM capture reports version/feature metadata for GFX, SDMA, PSP, VCN, DMCUB, MES and other firmware types. These are driver-reported metadata values, not proof that each image was loaded, started, or is operational.

The host inventory records installed-file SHA-256 values and now includes a source-derived request-path matrix from the exact CachyOS 7.3.1-1 tag in `docs/logs/navi33/firmware_inventory.md`. The GFX, SDMA, SMU, VCN and DMUB request names can be derived from the observed IP versions and matching source branches; the VCN 4.0.4 request resolves through an installed compressed symlink to the 4.0.0 payload. A significant unresolved discrepancy remains: the matching MP0 13.0.7 source path requests `psp_13_0_7_ta.bin` as required for this non-kicker device, but no such file is present in the searched firmware paths. The 2026-11-11 and 2026-11-14 runtime captures confirm SMU initialization, VCN firmware metadata, DMUB loading via PSP, and SDMA / VCN ring enumeration, but do not attest every requested filename or reconcile the missing base TA. Keep the final `rn_asic.firmware` field unset until this discrepancy and same-boot per-file load provenance are resolved.

## 6. Register-header and implementation candidates

The matching CachyOS 7.3.1-1 source cross-check is committed as `docs/logs/navi33/cachyos-kernel-source-crosscheck-2026-11-11.md`; generated-header provenance and family comparisons are in `docs/logs/navi33/register_header_references.md`. The Linux generic header families overlap with several headers already generated in RedNavi, but this does not establish that the current write sequence is safe on Navi 33. The current register-header candidates are:

- GC 11.0.2 — matching Linux GFX and SDMA sources include generic `gc_11_0_0` register headers. The existing RedNavi `gc_regs.h` banner also says it was generated from `gc_11_0_0`; however `asic.cpp` labels it `gc_11_0_3`, a metadata discrepancy recorded for maintainer review.
- SDMA 6.0.2 — the matching Linux driver uses shared GC 11.0.0, HDP 6.0.0, NBIO v4.3 and SDMA v6.0 packet definitions; no `sdma_6_0_2_offset.h` / `sh_mask.h` was found in the matching source directory.
- DCN 3.2.1 — RedNavi's existing display list comes from DCN 3.2.0 offsets; verify exact DCN 3.2.1 / DMU differences and board-specific display path.
- NBIO 4.3.1, MMHUB 3.0.2, OSSSYS 6.0.2 — current generated RedNavi headers use the generic 4.3.0 / 3.0.0 / 6.0.0 header families respectively.
- VCN IP Discovery 4.0.4 — matching CachyOS source selects generic `vcn_v4_0_ip_block` and `jpeg_v4_0_ip_block`; the VCN 4.0 path uses a unified ring. This is Linux source evidence, not a macOS implementation.

Do not copy or relabel the current Navi 32 generated register headers. Follow the repository's rule: generated headers must include the exact generator command and matching Linux header provenance.

## 7. Gaps before an upstream-ready ASIC row

1. Add / run a macOS-side `infoQuery()` integration test using the captured 440-byte payload and verify caller-buffer truncation / zero-fill behavior. Linux live ioctl ABI smoke test is now passed.
2. Resolve the required PSP 13.0.7 TA request versus missing installed blob, then capture same-boot per-file firmware provenance. Other request names and conditional branches are source-traced in `firmware_inventory.md`; do not treat source requests or package presence as load attestations.
3. Identify the correct GC / SDMA / DCN / NBIO / MMHUB / OSSSYS register headers from the matching Linux source.
4. Obtain a safe and independently verified DCN pipe / PHY / VBIOS connector mapping for the live DP-1 path; current connector + EDID evidence is not enough to define a display replay. No Navi 32 replay constants may be reused by assumption.
5. Refactor all write stages to select their firmware, register headers, memory layout, and display data from the ASIC profile before adding a Navi 33 row to the live table.
6. Add host tests proving the RX 7600 candidate remains unmatched / read-only until the complete profile and write-stage selection are implemented.

## 8. Evidence provenance

- IP Discovery capture: `docs/logs/navi33/rx7600-frame121/amdgpu_discovery.bin`
- IP Discovery report: `docs/logs/navi33/ip_discovery.txt`
- Capture provenance and integrity: `docs/logs/navi33/rx7600-frame121/README.md`
- DRM raw capture: `docs/logs/navi33/rx7600-frame121/amdgpu_info_raw.txt`
- Matching CachyOS kernel source cross-check: `docs/logs/navi33/cachyos-kernel-source-crosscheck-2026-11-11.md`
- Generated DRM snapshot: `docs/logs/navi33/linux_info_navi33.h` (440-byte DEV_INFO; not referenced by the live ASIC table)
- Live Linux ABI smoke test: `tools/linux_amdgpu_abi_smoke.c` (440-byte matching kernel UAPI; read-only ioctl passed)
- Live connector / EDID evidence: `docs/logs/navi33/connectors-live-2026-11-11.md` (DP-1 connected; no register writes)
- Structural host test: `tests/host/navi33_profile_test.cpp` (included in `make test`)
- Latest firmware runtime capture: `docs/logs/navi33/firmware-runtime-2026-11-14.md`
- Firmware inventory: `docs/logs/navi33/firmware_inventory.md`
- Register-header references: `docs/logs/navi33/register_header_references.md`
- Kernel source tag: `CachyOS/linux` `cachyos-7.3.1-1`, commit `36ae4c219cfbd02576048c7ca1d366cea5db8034`
