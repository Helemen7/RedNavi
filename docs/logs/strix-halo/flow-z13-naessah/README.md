# AMD Radeon 8060S (Strix Halo APU, RDNA 3.5) IP Discovery Evidence — Naessah

This directory contains hardware evidence from the AMD Radeon 8060S Graphics integrated GPU (Ryzen AI Max+ 395 APU, Strix Halo, GFX1151) captured live on macOS Sequoia 15.8.2.

## Device and capture provenance

- GPU: AMD Radeon 8060S Graphics (Strix Halo APU, RDNA 3.5)
- PCI vendor/device: 1002:1586
- Subsystem: 1043:1fb3 (ASUSTeK Computer Inc., ROG Flow Z13 2025 GZ302EA)
- PCI revision: c1
- CPU: AMD Ryzen AI Max+ 395 (16 cores, 32 threads, Zen 5)
- RAM: 128 GB LPDDR5X UMA @ 8000 MHz (256-bit bus, 256 GB/s)
- Display: TL134ADXP03 (2560x1600 @ 60 Hz eDP internal panel)
- Host OS: macOS Sequoia 15.8.2 (24H232), x86_64
- Capture date: 2026-11-10
- Source: Live read by RedNaviProbe from the UMA carveout (`vramRead` at top of BAR0 carveout minus 64 KB).
- Raw artifact: amdgpu_discovery.bin (1,788 bytes)
- Capture kext: `RedNaviProbe` from `https://github.com/PURE-NAESSAH/RedNavi` (branch `unified-driver`, commit `331f1c8`), adapted to match PCI device ID `1002:1586` and read the IP Discovery binary out of UMA memory. The capture build ran exclusively in safe read-only discovery mode (`dumpDiscovery()` on PCI match `1002:1586`); no stages writing to the GPU were enabled and no boot-args were passed.

## Integrity and decode

- SHA-256 of the 1,788-byte binary capture: `3e5bfd35173c99648cc8de1b746499d1b4cca339712058b2b77741a682660e48`
- Binary signature: `0x28211407`
- Binary version: 1.0, size: 1,788 bytes, checksum: `0xa283` (PASS)
- IP Discovery table: signature `0x53445049` (IPDS), version 3, offset 60 (0x3c), size 1,316 bytes, checksum: `0x9354` (PASS)
- Parsed IP records: 51 across one die
- Offline parser validation command:
  ```
  python3 tools/decode_discovery.py docs/logs/strix-halo/flow-z13-naessah/amdgpu_discovery.bin > docs/logs/strix-halo/flow-z13-naessah/ip_discovery.txt
  ```
  *(Note: The raw decoder output currently includes an extraneous trailing table entry `MEM_RESERVED_INFO` at offset `0x5049` due to the parser reading the next table's `IPDS` ASCII signature as a directory entry; this artifact row was removed from `ip_discovery.txt` by hand).*
### Table directory

| Table | Offset | Checksum | Size |
|---|---:|---:|---:|
| IP_DISCOVERY | 60 (0x3c) | 0x9354 | 1316 |
| GC | 1376 (0x560) | 0x02e6 | 132 |
| HARVEST_INFO | 1508 (0x5e4) | 0x0131 | 136 |
| VCN_INFO | 1644 (0x66c) | 0x0148 | 96 |
| MALL_INFO | 1740 (0x6cc) | 0x0178 | 48 |
| NPS_INFO | 0 | 0x0000 | 0 (empty) |

### Key IP Blocks Identified *(as listed in the decoded table; none were probed on hardware)*

- GC: v11.5.1 (bases `0x1260 0xa000 0x2402c00 0x3c02c000`)
- DMU: v3.5.1 (bases `0x12 0xc0 0x34c0 0x9000 0x2403c00 0x3c03c000`)
- MMHUB: v3.3.1 (bases `0x13200 0x1a000 0x2408800 0x3c088000`)
- MP0 / MP1 / MP2: v14.0.1 (PSP 14)
- SMUIO / THM: v14.0.1 (SMU 14)
- SDMA0: v6.1.1
- UMC: v11.8.0 (8 instances [INFERENCE: unified memory architecture])
- UVD/VCN: v4.0.6 & v4.0.7
- VPE: v6.1.1 & v6.1.2
## Files in this directory

- `README.md`: this summary and provenance record.
- `amdgpu_discovery.bin`: the exact 1,788-byte IP Discovery table payload dumped by `RedNaviProbe`.
- `ip_discovery.txt`: the human-readable decode produced by `tools/decode_discovery.py`.

## Scope and limitations

This capture is read-only IP Discovery metadata extracted from VRAM by the probe. No register writes, firmware loading or GPU queue submissions are claimed for this evidence.
