# ASIC profile migration audit — Navi 32 / Navi 33

**Date:** 2026-11-12  
**Scope:** source-only audit of the current `frame121/navi32-profile-data-split` branch. This document does not enable a new ASIC, alter a hardware path, or claim hardware validation.

## Current split boundary

PR #19 moves the Navi 32 `rn_asic` row, board identity, DP path, replay reference, DRM snapshot reference, firmware names, and register provenance strings into `RedNavi/asic_navi32.cpp`. `RedNavi/asic.cpp` now contains discovery parsing and generic lookup. The Makefile links the new translation unit into the kext and host ASIC test.

This is a **profile-data split**, not yet a complete architecture split. The profile's `firmware` and `reg_headers` members are descriptive strings; `asic.h` explicitly says firmware and register headers are compiled in and are not selected through the row yet.

## Remaining ASIC-coupled implementation inventory

| Area | Current location / evidence | Why it remains coupled | Safe next step |
|---|---|---|---|
| GC register definitions | `RedNavi/gc_regs.h`; generated header currently identifies GC 11.0.3 while Navi 33's observed GC is 11.0.2 | One global register namespace is compiled for the current write path | Introduce named register-family interfaces only after checking symbol compatibility and generated-header provenance; keep existing Navi 32 mapping unchanged |
| DCN register definitions and replay | `RedNavi/dcn_reglist.h`, `RedNavi/dcn_replay.h`, `RedNavi/mode_regs.cpp` | Replay is board/display-path-specific; Navi 33 DP-1 has no verified AUX/PHY/HPD mapping | Keep replay disabled for Navi 33; model display capability as an absent/validated profile component |
| Firmware build selection | `Makefile` global `RN_HAVE_PSP_FW`, `RN_HAVE_SMU_FW`, `RN_HAVE_GC_FW`, `RN_HAVE_DMUB_FW`; generated `psp_fw.h`, `smu_fw.h`, `gc_fw.h`, `dmub_fw.h` | Feature macros are global rather than selected by ASIC profile | Build a manifest/schema first; do not change load or PSP paths until per-ASIC provenance and regression evidence exist |
| GFX queue layout | `RedNavi/gfx11.cpp` around ring offsets `+0x1000`, `+0x1008`, `+0x2000`, `+0x4000`; ring setup and HQD/MQD routines | These offsets feed memory writes and queue programming | Extract constants into a passive profile description only after proving all current Navi 32 call sites consume the same values |
| MES layout | `RedNavi/gfx11.cpp` around `kKiqRingBytes`, `kSchedRingBytes`, `kKcqRingBytes`, `kRingAt`, `kMesAreaBytes`, `kMesDbStart`, `kMesKiqDoorbell`, `kMesSchedDoorbell` | MES memory allocation, firmware ABI and doorbell values are used by active write paths | First document source/version provenance and add host-only layout tests; no runtime selection yet |
| GFX doorbell ranges | `RedNavi/gfx11.cpp` around `regCP_RB_DOORBELL_RANGE_LOWER/UPPER` and values derived from `0x8b` / `0xff` | Direct register writes and queue mapping | Leave untouched pending exact per-ASIC source comparison |
| NBIO doorbell aperture | `RedNavi/gfx11.cpp` around `regRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN` and `regS2A_DOORBELL_ENTRY_0_CTRL` | Direct NBIO register writes | Keep out of any Navi 33 path until NBIO 4.3.1 compatibility is established |
| PSP / SMU / DMUB firmware paths | `RedNavi/kern_probe.cpp`, `RedNavi/smu13.cpp`, optional generated firmware headers | Firmware loading and PSP/TMR interactions have hardware side effects | Audit only; no firmware loading, copying, flashing, or TMR operations for RX 7600 |

## Navi 33 evidence state

Observed RX 7600 identity: PCI `1002:7480`, subsystem `148c:2421`, revision `0xcf`. The captured five-key IP Discovery tuple is GC 11.0.2, DMU/DCN 3.2.1, MP0 13.0.7, MP1 13.0.7, SDMA0 6.0.2. The committed ASIC host test checks these versions against the real capture and asserts that the RX 7600 remains unmatched/read-only.

`docs/logs/navi33/ASIC_PROFILE_DRAFT.md` is an evidence-backed draft, not a live ASIC row. It deliberately leaves board replay and DP PHY/AUX/HPD mapping unselected. The draft also records unresolved register-header family questions: GC 11.0.2 vs shared GC 11.0.0 definitions, DCN 3.2.1 vs the current 3.2.0 list, and NBIO 4.3.1 / MMHUB 3.0.2 / OSSSYS 6.0.2 vs current generic headers.

## Firmware provenance status

`docs/logs/navi33/firmware_inventory.md` records nine installed compressed firmware candidates with SHA-256 values and matching source declarations for several request paths. `firmware-runtime-current-boot.md` independently records runtime SMU 82.97.0 initialization, DMUB `0x07003600` loaded via PSP, VCN ENC 1.24 / DEC 9 / revision 55, and SDMA / VCN ring enumeration.

These evidence classes must remain distinct:

1. **Package presence + hash:** proves a compressed file existed in the installed package at inventory time.
2. **Kernel source request path:** proves the matching source can request a filename under the relevant IP-block path.
3. **Runtime subsystem evidence:** proves the kernel reported initialization/version/ring state.
4. **Exact per-boot blob attestation:** not yet available for every candidate filename.

Therefore do not populate Navi 33 `rn_asic.firmware` from candidate filenames alone. Exact requested filename selection, optional/conditional components, and load success for each required blob remain unresolved. Runtime versions are not interchangeable with blob hashes or filename attestations.

## Host validation available on Frame121 Windows host

- `python -I tests/host/decode_discovery_test.py`: 2 tests passed.
- `python -I tests/host/compare_discovery_test.py`: 9 tests passed.
- Native C/C++ host and kext builds were not run: `clang`, `clang++`, `make`, CMake and Ninja are absent; WSL is not installed on this host.
- No macOS integration test, hardware write, GPU workload, firmware load, or RX 7600 Metal acceleration test is claimed by this audit.

## Exit criteria before adding a Navi 33 live row

- All active write stages select register families, firmware manifests, memory/queue layouts and display data through an ASIC-specific interface.
- Navi 32 RX 7800 XT regression tests and real-hardware validation pass after each migration.
- Navi 33 register families and firmware filenames have exact source provenance; optional and required blobs are distinguished.
- macOS `infoQuery()` integration test passes for the 440-byte captured DEV_INFO ABI.
- RX 7600 connector/PHY/AUX/HPD mapping is independently verified before any display replay is proposed.
- Host tests prove RX 7600 stays read-only until every gate is explicitly satisfied.

Until then, Navi 33 remains a draft profile and RX 7600 remains read-only.
