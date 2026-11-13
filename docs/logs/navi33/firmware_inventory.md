# RX 7600 / Navi 33 firmware inventory — evidence only

**Host:** CachyOS x86_64, kernel `7.3.1-1-cachyos`
**Package:** `linux-firmware-amdgpu 1:20261019-1`
**Device:** `1002:7480`
**Scope:** read-only filesystem inventory plus saved kernel log. No firmware was loaded, copied, replaced, or modified by this investigation.

## Interpretation

The files below exist in the installed firmware package and have names consistent with the Navi 33 IP versions. The matching kernel source declares or requests several of these names. This is **not** a per-file load attestation. The saved dmesg does not print every selected filename, so the list remains a candidate inventory rather than a final `rn_asic.firmware` value.

The log does show:
- SMU initialization succeeded; firmware version logged as `82.92.0`.
- VCN firmware metadata logged as ENC 1.23 / DEC 9 / revision 16.
- DMUB firmware was loaded via PSP; version `0x07002D00`.
- SDMA0/SDMA1 and unified VCN rings were enumerated.

These runtime messages support successful Linux initialization of those subsystems, but do not identify every exact blob file used.

The older saved dmesg excerpt above is from a different boot and must not be paired with the DRM capture or with the package hashes collected later. A current-boot journal capture is documented in `firmware-runtime-current-boot.md`; it reports SMU 82.97.0 and DMUB `0x07003600`, which agree with the DRM capture in `rx7600-frame121/amdgpu_info_raw.txt` (FW_VERSION types `0x0a` and `0x14`). The current journal also reports VCN ENC 1.24 / DEC 9 / revision 55. The nine SHA-256 values below describe the installed compressed files at inventory time only; they are not hashes of firmware loaded during the older boot. Neither the DRM capture nor the journal attests every requested blob filename.

## Candidate files present

All hashes below are SHA-256 of the installed compressed `.bin.zst` files, not hashes of decompressed firmware payloads.

| Subsystem | Installed file | SHA-256 | Evidence status |
|---|---|---|---|
| GC / GFX | `gc_11_0_2_pfp.bin.zst` | `c07bdae373e26591b211e6cce787feeafd7b09af057173c4040ad4e40b12fefb` | Present; matching `gfx_v11_0.c` requests IP-version-derived `*_pfp.bin` |
| GC / GFX | `gc_11_0_2_me.bin.zst` | `cf764f61b4007507e06f87cc2f15e1f35cff7f953d3fed0d69c528912d65765c` | Present; source requests `*_me.bin` |
| GC / GFX | `gc_11_0_2_mec.bin.zst` | `0079c003d8b0c35848147c60a444731c673ed259ffa997d31192236617cd6491` | Present; source requests `*_mec.bin` |
| GC / GFX | `gc_11_0_2_rlc.bin.zst` | `eb454e64c2a7084afa215f0c6003890310fe40570286bfc6d4d903d2c19417db` | Present; source has the RLC request path |
| PSP | `psp_13_0_7_sos.bin.zst` | `7a5b75f9d7d7bc1b7e83fbbee73bc5734a94f184ae03634a9d9848f8d63954b7` | Present; matching PSP source declares this firmware |
| SDMA | `sdma_6_0_2.bin.zst` | `e84bc945fd877c78eb15e2214646ff9256242ff27f3556945c22ba251f88996d` | Present; matching SDMA source declares this firmware |
| SMU | `smu_13_0_7.bin.zst` | `759ec572f6775c4a246f6ae2098d90615e9ec03a46bdd64438458b8465af90c0` | Present; matching SMU source declares this firmware |
| VCN | `vcn_4_0_4.bin.zst` → symlink to `vcn_4_0_0.bin.zst` | `e30ccfd69625f3f11f2ff643199283e57c5149ffd443cc38986a200d2bdb51ac` (target) | Requested name follows UVD 4.0.4; installed compressed symlink resolves to the hashed target |
| DMUB | `dcn_3_2_1_dmcub.bin.zst` | `2728d7dd6ed255028663afcea6e8ee9b8d3735676a52505e49974c9dae5a2ab4` | Present; dmesg confirms DMUB loaded via PSP, but not the filename |

## Additional GC files present

These are present in the same installed package, but this inventory does not assert that every one is required by the current RX 7600 boot path:

- `gc_11_0_2_imu.bin.zst`
- `gc_11_0_2_mes.bin.zst`
- `gc_11_0_2_mes1.bin.zst`
- `gc_11_0_2_mes_2.bin.zst`

## Not found in the installed package

- `gc_11_0_2_rlc_kicker.bin.zst`
- `psp_13_0_7_ta.bin.zst`

The missing `gc_11_0_2_rlc_kicker.bin.zst` is not expected for this RX 7600: the matching kernel's kicker-device list contains only device/revision pairs `744b:00` and `7551:c8`, not `7480:cf`; the ordinary `gc_11_0_2_rlc.bin.zst` is present. The missing PSP TA is different: the source trace below identifies it as a required request on the observed MP0 13.0.7 non-kicker path. This is an unresolved source-versus-filesystem/runtime discrepancy, not an optional-file assumption.

## Matching source provenance

CachyOS Linux tag `cachyos-7.3.1-1`, commit `36ae4c219cfbd02576048c7ca1d366cea5db8034`:

| Source | Git blob SHA | Relevant evidence |
|---|---|---|
| `drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c` | `7a006b22e4be06812288f720479a92d21adc03e5` | GC IP-version-derived prefix; PFP / ME / MEC / RLC firmware request paths |
| `drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c` | `cf3d2997fff88387223b22c62558ed83aaf1a13d` | Declares `sdma_6_0_2.bin` |
| `drivers/gpu/drm/amd/amdgpu/psp_v13_0.c` | `f9a3fbfc0634bd4512fea755999e81cb0174a4ca` | Declares `psp_13_0_7_sos.bin` and TA variant |
| `drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c` | `c4b2fe60da62bc3204ce54b6648c54e3a472afb8` | Declares `smu_13_0_7.bin`; decodes MP1 IP version for firmware prefix |
| `drivers/gpu/drm/amd/amdgpu/vcn_v4_0.c` | `c2ddf3cb368fde8141331682c0556ceac1f2a573` | Generic VCN 4.0 implementation selected for captured IP Discovery 4.0.4 |


## Exact request-path cross-check — CachyOS source tag

A read-only sparse checkout of CachyOS tag `cachyos-7.3.1-1` was inspected at commit `36ae4c219cfbd02576048c7ca1d366cea5db8034`. This section records source-derived request names and conditional branches; it does not claim that every requested file was loaded by the saved boot.

| IP block observed on RX 7600 | Source-derived firmware request | Required / condition | Installed inventory result |
|---|---|---|---|
| GC 11.0.2 | `gc_11_0_2_pfp.bin`, `gc_11_0_2_me.bin`, `gc_11_0_2_rlc.bin`, `gc_11_0_2_mec.bin` | All marked `AMDGPU_UCODE_REQUIRED`; RLC uses the ordinary path because `1002:7480 rev cf` is not in the source's kicker-device list | All four compressed files present |
| MP0 13.0.7 | `psp_13_0_7_sos.bin`, then `psp_13_0_7_ta.bin` | The 13.0.7 switch case calls both SOS and TA helpers; TA request is marked required. The kicker alternative is not selected for this device | SOS present; TA absent — unresolved blocker |
| SDMA0 6.0.2 | `sdma_6_0_2.bin` | Required request for instance 0; the v6.0 path duplicates the instance-0 firmware for its second SDMA instance | Present |
| MP1 13.0.7 | `smu_13_0_7.bin` | Required on the non-SR-IOV path; kicker alternative is not selected | Present |
| UVD / VCN 4.0.4 | `vcn_4_0_4.bin` | Required; IP-version decoder uses the `vcn_4_0_4` prefix. Discovery selects the generic VCN 4.0 / JPEG 4.0 block for 4.0.4 | `vcn_4_0_4.bin.zst` is a symlink to `vcn_4_0_0.bin.zst`; target hash is recorded above |
| DCN 3.2.1 | `dcn_3_2_1_dmcub.bin` | DCN 3.2.1 selector chooses this DMUB name | Present; saved runtime log reports DMUB loaded via PSP at `0x07003600`, but does not print the requested filename |

### PSP TA discrepancy — do not silently waive

The matching source's `psp_v13_0_init_microcode()` includes MP0 13.0.7 in the SOS-plus-TA branch. `psp_init_ta_microcode()` selects `psp_13_0_7_ta.bin` for a non-kicker device and marks the request `AMDGPU_UCODE_REQUIRED`. The source kicker list contains only `744b:00` and `7551:c8`, so this RX 7600 (`7480:cf`) does not select a TA kicker filename. A filesystem search of both `/usr/lib/firmware` and `/lib/firmware` found only `psp_13_0_7_sos.bin.zst`; no `psp_13_0_7_ta` file was found.

The saved current-boot journal reports DMUB loading via PSP and successful SMU initialization, but it does not include the complete PSP firmware-request trace or the exact TA filename. Those runtime lines therefore do not reconcile the required-source-path / missing-file discrepancy. Possible explanations must be tested against a complete same-boot log and the exact loaded module build; do not infer that TA is optional, invent a loaded blob, or mark PSP firmware provenance complete. Keep the Navi 33 profile unregistered until this is resolved.

### Source blob provenance

All blob IDs below are from commit `36ae4c219cfbd02576048c7ca1d366cea5db8034`:

| Source file | Git blob SHA |
|---|---|
| `drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c` | `7a006b22e4be06812288f720479a92d21adc03e5` |
| `drivers/gpu/drm/amd/amdgpu/psp_v13_0.c` | `f9a3fbfc0634bd4512fea755999e81cb0174a4ca` |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c` | `42adc8e738d893a7c78b3db0b7a03c6d73674326` |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.c` | `4f1c711df5bd72eb14d9c9c07f627a3acbe43f4a` |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_sdma.c` | `fbac732f3e01a65dbd32ebb4d07c223f7f57d285` |
| `drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c` | `c4b2fe60da62bc3204ce54b6648c54e3a472afb8` |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.c` | `17db7264269ef5b475e69a275e405577b7ea1b6c` |
| `drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm_dmub.c` | `9ff25bc8ee5ae5173e03b74bc966edab0d434450` |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c` | `bb672a923463005b5f46b7a3dc88c9ba7eb7f4bd` |

## Decision

Do not populate `rn_asic.firmware` from this inventory yet. The source-derived request names are now clearer, but the required PSP TA request is not accounted for by the installed-file inventory or saved runtime log. Resolve that discrepancy and obtain same-boot per-file load evidence before selecting a final firmware list. This document intentionally records source request paths, package presence, and runtime messages as separate evidence classes.
