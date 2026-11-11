# RX 7600 (Navi 33) IP Discovery evidence — Frame121

This is a read-only Linux sysfs/debugfs capture supplied for RedNavi's multi-ASIC Navi 33 work.

## Device and capture

- GPU: AMD Radeon RX 7600, Navi 33
- PCI vendor/device: 1002:7480
- Subsystem: 148c:2421 (PowerColor / Tul Corporation)
- PCI BDF: 0000:03:00.0
- Host: Archcraft (Arch Linux family), x86_64
- Kernel: 7.2.11-arch1-1
- Capture date: 2026-10-23 (Asia/Bangkok local time)
- Source: /sys/kernel/debug/dri/1/amdgpu_discovery
- Raw artifact: amdgpu_discovery.bin (10,240 bytes)

## Integrity and decode

- SHA-256: 4b809b3279bd9fd164456f5fcdcb9743d2b4289acdd77f9ac2a2cd77b75e6c89
- Binary signature: 0x28211407
- Binary version: 1.3
- Header-declared binary size: 1,280 bytes (the captured file is 10,240 bytes)
- Binary checksum: stored/calculated 0x5784 — PASS
- IPDS: signature 0x53445049 (IPDS), version 3, offset 60, size 808
- IPDS checksum: stored/calculated 0x49a2 — PASS
- Parsed IP records: 39 across one die
- Strict project parser validation: PASS

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

## Scope and limitations

- This is a raw, read-only discovery capture plus offline parsing; no register writes, firmware changes, or GPU initialization were performed for this capture.
- The 10 KiB capture contains a 1,280-byte header-declared binary region plus trailing bytes. The binary/IPDS checksum claims apply to their declared regions; they do not establish that trailing bytes belong to the binary.
- Hardware IDs are left numeric; no speculative IP-block naming is asserted here.
- Base addresses are metadata only and must not be treated as instructions for register access.
- This evidence confirms readable Navi 33 IP Discovery metadata only. It does not demonstrate macOS support, firmware loading, GPU queue submission, Metal, or WindowServer acceleration.
- ipdiscovery-analysis.md in the originating research tree contains a preliminary table-directory decode that was superseded. The corrected directory above is taken from the strict parser validation result.

## Suggested use

Use the raw artifact to independently validate the Navi 33 IP block/instance/version inventory and inform asic.cpp / rn_asic_board definitions. Please independently verify each mapping against Linux kernel sources and RedNavi's own abstractions before enabling any hardware access.
