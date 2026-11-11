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

A separate current-boot journal capture was collected on 2026-11-11 and is documented in `firmware-runtime-current-boot.md`. It reports SMU 82.97.0, VCN ENC 1.24 / DEC 9 / revision 55, DMUB runtime version `0x07003600`, and SDMA / VCN ring enumeration. These values differ from the older saved dmesg excerpt above (SMU 82.92.0 and VCN ENC 1.23), so the captures are retained separately rather than silently merged. Neither capture prints every requested firmware filename.

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
| VCN | `vcn_4_0_0.bin.zst` | `e30ccfd69625f3f11f2ff643199283e57c5149ffd443cc38986a200d2bdb51ac` | Present; version family matches observed VCN IP |
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

Absence from the current package inventory is not proof that the kernel attempted to load either file. The matching source contains conditional and optional firmware paths; determine which apply before treating these as blockers.

## Matching source provenance

CachyOS Linux tag `cachyos-7.3.1-1`, commit `36ae4c219cfbd02576048c7ca1d366cea5db8034`:

| Source | Git blob SHA | Relevant evidence |
|---|---|---|
| `drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c` | `7a006b22e4be06812288f720479a92d21adc03e5` | GC IP-version-derived prefix; PFP / ME / MEC / RLC firmware request paths |
| `drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c` | `cf3d2997fff88387223b22c62558ed83aaf1a13d` | Declares `sdma_6_0_2.bin` |
| `drivers/gpu/drm/amd/amdgpu/psp_v13_0.c` | `f9a3fbfc0634bd4512fea755999e81cb0174a4ca` | Declares `psp_13_0_7_sos.bin` and TA variant |
| `drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c` | `c4b2fe60da62bc3204ce54b6648c54e3a472afb8` | Declares `smu_13_0_7.bin`; decodes MP1 IP version for firmware prefix |
| `drivers/gpu/drm/amd/amdgpu/vcn_v4_0.c` | `c2ddf3cb368fde8141331682c0556ceac1f2a573` | Generic VCN 4.0 implementation selected for captured IP Discovery 4.0.4 |

## Decision

Do not yet populate `rn_asic.firmware` from this inventory. First prove the exact per-IP filename selection for this boot and decide which optional GC / PSP / DMUB / VCN files RedNavi actually needs. This document intentionally records package presence separately from firmware load state.
