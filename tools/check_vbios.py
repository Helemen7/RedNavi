#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Check a VBIOS image dumped by the RedNavi probe (the "vbios" property of RedNaviProbe).

Accepts the raw image or the output of `ioreg -rn RedNaviProbe -l -x`. Prints the PCI ROM and
ATOM headers and the PCI IDs found in the PCIR structure.

    python3 tools/check_vbios.py ioreg.txt
"""
import re
import struct
import sys


def load(path):
    raw = open(path, "rb").read()
    if raw[:2] == b"\x55\xaa":
        return raw
    m = re.search(r'"vbios"\s*=\s*<([0-9a-fA-F]+)>', raw.decode("utf-8", "replace"))
    if not m:
        sys.exit("no vbios data found")
    return bytes.fromhex(m.group(1))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    b = load(sys.argv[1])
    print(f"{len(b)} bytes, signature {b[0]:02x}{b[1]:02x}, image size field {b[2] * 512}")
    pcir, = struct.unpack_from("<H", b, 0x18)
    if b[pcir:pcir + 4] == b"PCIR":
        vid, did = struct.unpack_from("<HH", b, pcir + 4)
        print(f"PCIR at 0x{pcir:x}: vendor {vid:04x} device {did:04x}")
    else:
        print("no PCIR structure at the pointer in the header")
    atom, = struct.unpack_from("<H", b, 0x48)
    if atom + 8 <= len(b):
        print(f"ATOM ROM header at 0x{atom:x}, signature {b[atom + 4:atom + 8]!r}")
    cs = sum(b) & 0xFF
    print(f"byte sum {'OK (0)' if cs == 0 else cs}")


if __name__ == "__main__":
    main()
