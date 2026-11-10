#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Decode an AMD IP discovery binary dumped by the RedNavi probe kext.

Accepted inputs (auto-detected):
  * the raw binary,
  * a text file with the probe's log lines ("RedNavi: DISC 0010: aaaaaaaa bbbbbbbb cccccccc dddddddd"),
  * the output of `ioreg -rn RedNaviProbe -l -x` (the "ip-discovery" = <hex> property).

Layout follows drivers/gpu/drm/amd/include/discovery.h in the Linux kernel.

    python3 tools/decode_discovery.py dump.txt
"""
import re
import struct
import sys

BINARY_SIGNATURE = 0x28211407
DISCOVERY_TABLE_SIGNATURE = 0x53445049  # "IPDS"
TABLE_NAMES = ["IP_DISCOVERY", "GC", "HARVEST_INFO", "VCN_INFO", "MALL_INFO", "NPS_INFO", "MEM_RESERVED_INFO"]

# From drivers/gpu/drm/amd/include/soc15_hw_ip.h.
HW_IDS = {
    1: "MP1", 2: "MP2", 3: "THM", 4: "SMUIO", 5: "FUSE", 6: "CLKA", 10: "PWR", 11: "GC", 12: "UVD/VCN",
    13: "AUDIO_AZ", 14: "ACP", 15: "DCI", 16: "DCO", 17: "XDMA", 18: "DCEAZ", 19: "SDPMUX", 20: "NTB",
    21: "VPE", 22: "UMSCH", 24: "IOHC", 28: "L2IMU", 32: "VCE", 34: "MMHUB", 35: "ATHUB",
    36: "DBGU_NBIO", 37: "DFX", 38: "DBGU0", 39: "DBGU1", 40: "OSSSYS", 41: "HDP", 42: "SDMA0",
    43: "SDMA1", 44: "ISP", 45: "DBGU_IO", 46: "DF", 47: "CLKB", 48: "FCH", 49: "DFX_DAP",
    50: "L1IMU_PCIE", 51: "L1IMU_NBIF", 52: "L1IMU_IOAGR", 53: "L1IMU3", 54: "L1IMU4", 55: "L1IMU5",
    56: "L1IMU6", 57: "L1IMU7", 58: "L1IMU8", 59: "L1IMU9", 60: "L1IMU10", 61: "L1IMU11", 62: "L1IMU12",
    63: "L1IMU13", 64: "L1IMU14", 65: "L1IMU15", 66: "WAFLC", 67: "FCH_USB_PD", 68: "SDMA2",
    69: "SDMA3", 70: "PCIE", 80: "PCS", 89: "DDCL", 90: "SST", 91: "LSDMA", 100: "IOAGR", 108: "NBIF",
    124: "IOAPIC", 128: "SYSTEMHUB", 144: "NTBCCP", 150: "UMC", 168: "SATA", 170: "USB", 176: "CCXSEC",
    200: "XGMI", 216: "XGBE", 255: "MP0", 271: "DMU", 272: "DIO", 274: "DAZ", 294: "ATU", 295: "AIGC",
}


def load(path):
    raw = open(path, "rb").read()
    if raw[:4] == struct.pack("<I", BINARY_SIGNATURE):
        return raw
    text = raw.decode("utf-8", "replace")
    lines = re.findall(r"DISC\s+([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{8}\s*){1,4})", text)
    if lines:
        out = bytearray()
        for off, words in sorted(((int(o, 16), w) for o, w in lines), key=lambda x: x[0]):
            if off != len(out):
                sys.exit(f"gap or duplicate in log at offset 0x{off:x} (have 0x{len(out):x} bytes)")
            for w in words.split():
                out += struct.pack("<I", int(w, 16))
        return bytes(out)
    m = re.search(r'"ip-discovery"\s*=\s*<([0-9a-fA-F]+)>', text)
    if m:
        return bytes.fromhex(m.group(1))
    sys.exit("no discovery data found in input")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    b = load(sys.argv[1])
    sig, vmaj, vmin, csum, size = struct.unpack_from("<IHHHH", b, 0)
    if sig != BINARY_SIGNATURE:
        sys.exit(f"bad binary signature 0x{sig:08x}")
    print(f"binary header: version {vmaj}.{vmin}, size {size}, checksum 0x{csum:04x}, have {len(b)} bytes")
    if size <= len(b):
        calc = sum(b[10:size]) & 0xFFFF
        print(f"  checksum {'OK' if calc == csum else 'MISMATCH (computed 0x%04x)' % calc}")
    else:
        print("  dump shorter than binary_size, checksum skipped")

    if vmaj >= 2:
        num_tables, _ = struct.unpack_from("<HH", b, 12)
        tbl_base = 16
    else:
        num_tables, tbl_base = len(TABLE_NAMES), 12
    tables = {}
    for i in range(num_tables):
        off, tcsum, tsize, _ = struct.unpack_from("<HHHH", b, tbl_base + i * 8)
        name = TABLE_NAMES[i] if i < len(TABLE_NAMES) else f"table{i}"
        tables[i] = (off, tsize)
        print(f"  table {i} {name}: offset 0x{off:x} size {tsize} checksum 0x{tcsum:04x}")

    ip_off, _ = tables[0]
    tsig, tver, tsize, tid, num_dies = struct.unpack_from("<IHHIH", b, ip_off)
    if tsig != DISCOVERY_TABLE_SIGNATURE:
        sys.exit(f"bad IP table signature 0x{tsig:08x}")
    dies = [struct.unpack_from("<HH", b, ip_off + 14 + 4 * i) for i in range(min(num_dies, 16))]
    pad_off = ip_off + 14 + 4 * 16
    base64 = tver == 4 and (b[pad_off] & 1)
    print(f"ip table: version {tver}, {num_dies} die(s), {'64' if base64 else '32'}-bit base addresses")

    for die_id, die_off in dies:
        _, num_ips = struct.unpack_from("<HH", b, die_off)
        pos = die_off + 4
        print(f"die {die_id}: {num_ips} IP entries")
        for _ in range(num_ips):
            hw, inst, nbase, major, minor, rev, extra = struct.unpack_from("<HBBBBBB", b, pos)
            pos += 8
            fmt, width = ("<Q", 8) if base64 else ("<I", 4)
            bases = [struct.unpack_from(fmt, b, pos + width * k)[0] for k in range(nbase)]
            pos += width * nbase
            name = HW_IDS.get(hw, f"hw{hw}")
            print(f"  {name:<10} inst {inst}  v{major}.{minor}.{rev}  bases {' '.join('0x%x' % x for x in bases)}")


if __name__ == "__main__":
    main()
