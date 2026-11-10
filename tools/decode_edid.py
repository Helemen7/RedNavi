#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Decode the EDID the RedNavi probe read over AUX (the "edid" property of RedNaviProbe).

Accepts a raw EDID file or the output of `ioreg -rn RedNaviProbe -l -x`. Prints the vendor, product,
monitor name, the detailed timings of the base block and the CTA-861 extension's video data blocks.

    python3 tools/decode_edid.py ioreg.txt
"""
import re
import struct
import sys


def load(path):
    raw = open(path, "rb").read()
    if raw[:8] == bytes.fromhex("00ffffffffffff00"):
        return raw
    m = re.search(r'"edid"\s*=\s*<([0-9a-fA-F]+)>', raw.decode("utf-8", "replace"))
    if not m:
        sys.exit("no edid data found")
    return bytes.fromhex(m.group(1))


def dtd(d):
    clk = struct.unpack_from("<H", d, 0)[0] * 10  # kHz
    if not clk:
        return None
    ha = d[2] | ((d[4] & 0xF0) << 4)
    hb = d[3] | ((d[4] & 0x0F) << 8)
    va = d[5] | ((d[7] & 0xF0) << 4)
    vb = d[6] | ((d[7] & 0x0F) << 8)
    ht, vt = ha + hb, va + vb
    return f"{ha}x{va} @ {clk * 1000 / (ht * vt):.2f} Hz, pixel clock {clk / 1000:.2f} MHz, total {ht}x{vt}"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    e = load(sys.argv[1])
    print(f"{len(e)} bytes, base block checksum {'OK' if sum(e[:128]) % 256 == 0 else 'BAD'}")
    m = (e[8] << 8) | e[9]
    vendor = "".join(chr(((m >> s) & 31) + 64) for s in (10, 5, 0))
    print(f"vendor {vendor}, product 0x{struct.unpack_from('<H', e, 10)[0]:04x}, EDID {e[18]}.{e[19]}, "
          f"made week {e[16]} of {1990 + e[17]}, {e[21]}x{e[22]} cm, {e[126]} extension block(s)")
    for off in (54, 72, 90, 108):
        d = e[off:off + 18]
        if d[0] or d[1]:
            print(f"detailed timing: {dtd(d)}")
        elif d[3] == 0xFC:
            print("monitor name:", d[5:18].split(b"\n")[0].decode("ascii", "replace"))
        elif d[3] == 0xFF:
            print("serial string:", d[5:18].split(b"\n")[0].decode("ascii", "replace"))
        elif d[3] == 0xFD:
            print(f"range limits: V {d[5]}-{d[6]} Hz, H {d[7]}-{d[8]} kHz, max pixel clock {d[9] * 10} MHz")
    if len(e) >= 256 and e[128] == 0x02:
        x = e[128:256]
        dtd_off = x[2]
        print(f"CTA-861 extension rev {x[1]}, {x[3] & 15} native DTDs")
        i = 4
        while i < dtd_off:
            tag, ln = x[i] >> 5, x[i] & 31
            body = x[i + 1:i + 1 + ln]
            if tag == 2:
                print("  video modes (VICs):", ", ".join(str(b & 0x7F) + ("*" if b & 0x80 else "") for b in body))
            elif tag == 1:
                print(f"  audio block, {ln // 3} descriptors")
            elif tag == 3:
                oui = body[2] << 16 | body[1] << 8 | body[0]
                print(f"  vendor block OUI {oui:06x}")
            i += 1 + ln
        j = dtd_off
        while j + 18 <= 127 and (x[j] or x[j + 1]):
            print(f"  detailed timing: {dtd(x[j:j + 18])}")
            j += 18


if __name__ == "__main__":
    main()
