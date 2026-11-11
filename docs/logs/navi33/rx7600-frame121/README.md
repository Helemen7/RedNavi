# RX 7600 (Navi 33) IP Discovery evidence — Frame121

This directory contains a read-only AMDGPU IP Discovery capture from the RX 7600 installed in the contributor's CachyOS Linux system. It is provided as hardware evidence for RedNavi's multi-ASIC Navi 33 work.

## Device and capture provenance

- GPU: AMD Radeon RX 7600, Navi 33
- PCI vendor/device: 1002:7480
- Subsystem: 148c:2421 (PowerColor / Tul Corporation)
- PCI BDF: 0000:03:00.0
- PCI revision: cf
- Host OS: CachyOS Linux (Arch Linux family), x86_64
- Kernel: 7.3.1-1-cachyos
- Capture date: 2026-11-01 (Asia/Bangkok local time)
- Source: AMDGPU debugfs IP Discovery export (amdgpu_discovery)
- Raw artifact: amdgpu_discovery.bin (10,240 bytes)

The host and kernel provenance are recorded in the accompanying RX 7600 Linux evidence report. The raw file was copied without modifying its bytes.

## Integrity and decode

- SHA-256 of the complete 10,240-byte capture: e6381d289c19cb097a8366216bcad579623457cce5adbafae9b81b355164a2a8
- Binary signature: 0x28211407
- Binary version: 1.3
- Header-declared binary size: 1,280 bytes
- Trailing bytes: 8,960 bytes (padding; excluded from parsing)
- Binary checksum: stored/calculated 0x5784 — PASS
- IP Discovery table: signature 0x53445049 (IPDS), version 3, offset 60, size 808
- IPDS checksum: stored/calculated 0x49a2 — PASS
- Parsed IP records: 39 across one die
- Strict offline parser validation: PASS
- Kernel sysfs cross-check: 37 named aliases matched; all 37 matched IP version and base-address metadata

### Correct v1 table directory

The v1 header uses six 8-byte table_info entries (offset, checksum, size, padding).

| Table | Offset | Checksum | Size |
|---|---:|---:|---:|
| IP_DISCOVERY | 60 | 0x49a2 | 808 |
| GC | 868 | 0x02d7 | 132 |
| HARVEST_INFO | 1000 | 0x0131 | 136 |
| VCN_INFO | 1136 | 0x0149 | 96 |
| MALL_INFO | 1232 | 0x0157 | 48 |
| NPS_INFO | 0 | 0x0000 | 0 (empty) |

### Relationship to the initially submitted artifact

The previously submitted PR artifact had the same first 1,280 bytes as this capture (the complete header-declared binary region). Its trailing 8,960 padding bytes differed, which resulted in a different SHA-256 for the full 10,240-byte file. This PR update replaces it with the byte-for-byte CachyOS capture and records that capture's full-file hash. The parsed binary payload and table checksums are unchanged.

## Scope and limitations

- This is a raw, read-only discovery capture plus offline parsing; no register writes, firmware changes, GPU reset, or GPU initialization were performed as part of this capture.
- The 10 KiB debugfs export contains a 1,280-byte header-declared binary region plus trailing padding. Binary parsing and checksum claims apply to declared regions only.
- Hardware IDs are left numeric; no speculative IP-block naming is asserted here.
- Base addresses are metadata only and must not be treated as instructions for register access.
- This evidence confirms readable Navi 33 IP Discovery metadata on Linux. It does not demonstrate macOS support, firmware loading by RedNavi, GPU queue submission by RedNavi, Metal, or WindowServer acceleration.

## Suggested use

Use the raw artifact to independently validate the Navi 33 IP block/instance/version inventory and inform asic.cpp / rn_asic_board definitions. Independently verify each mapping against Linux kernel sources and RedNavi's own abstractions before enabling hardware access.

## Additional Linux DRM_AMDGPU_INFO evidence

A separate read-only DRM_AMDGPU_INFO response snapshot was captured on 2026-11-11. See [amdgpu_info.md](amdgpu_info.md) for provenance, interpretation and limitations, and [amdgpu_info_raw.txt](amdgpu_info_raw.txt) for the original hexadecimal ioctl responses. This is separate from the IP Discovery binary capture above.
