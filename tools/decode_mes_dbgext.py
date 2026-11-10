#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Decode the MES firmware log that the probe arms with `rednavi_mes_dbgext=1`.

The probe hands the MES scheduler a 16 KB buffer with MESAPI_MISC__SETUP_MES_DBGEXT (the v11 API's
"mes_dbgext") and, after the queue frames, publishes the first 2 KB of it as two dictionaries of
256 numbers: "mes-dbg-lo" (keys l000..l3fc, bytes 0..1023) and "mes-dbg-hi" (h000..h3fc, bytes
1024..2047). "mes-start" carries the frame's result in dbg_ok and dbg_status.

Usage:
  ioreg -rn RedNaviProbe -l -x > ioreg.txt
  python3 tools/decode_mes_dbgext.py ioreg.txt

Nothing here assumes the record layout: the first four dwords are the header the firmware leaves
(rptr, wptr, buffer_size, header_size), and each record is tried with the two layouts the Linux
driver describes (8-byte header with a 24-bit length, and 4-byte header with a 16-bit length).
Whichever validates is printed, with the raw bytes of the first bad record as a fallback. Only the
dumped window is walked, so a record that starts past 2 KB is reported, not decoded.
"""

import re
import struct
import sys

MAX_ITEM = 2048


def prop_dict(text, name):
    """The {...} body of property `name` in ioreg output (None if absent)."""
    i = text.find('"%s"' % name)
    if i < 0:
        return None
    j = text.find("{", i)
    if j < 0:
        return None
    depth = 0
    for k in range(j, len(text)):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                return text[j:k + 1]
    return None


def half_bytes(body, half, size=1024):
    """Rebuild one 1 KB half from keys like l000 = 4294967295 (or 0xffffffff)."""
    out = bytearray(size)
    got = 0
    for m in re.finditer(r'"([lh])([0-9a-fA-F]{3})"\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)', body):
        if m.group(1) != half:
            continue
        off = int(m.group(2), 16)
        if off + 4 > size:
            continue
        struct.pack_into("<I", out, off, int(m.group(3), 0) & 0xFFFFFFFF)
        got += 1
    return bytes(out), got


def printable(b):
    return "".join(chr(c) if (32 <= c < 127 or c in (9, 10, 13)) else "\\x%02x" % c for c in b)


def parse_item(buf, off):
    """(type, length, header length) for a record at `off`, or None if neither layout validates."""
    h = buf[off:off + 8]
    if len(h) < 8:
        return None
    # 8-byte header: type, xor signature, 24-bit big-endian length
    ln = (h[2] << 16) | (h[3] << 8) | h[4]
    if (h[0] ^ h[2] ^ h[3] ^ h[4] ^ h[5] ^ h[6]) == h[1] and 8 < ln <= MAX_ITEM:
        return h[0], ln, 8
    # 4-byte header: type, xor signature, 16-bit big-endian length
    ln = (h[2] << 8) | h[3]
    if (h[0] ^ h[2] ^ h[3]) == h[1] and 4 < ln <= MAX_ITEM:
        return h[0], ln, 4
    return None


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    text = open(sys.argv[1], errors="replace").read()

    body = prop_dict(text, "mes-dbg-lo")
    if body is None:
        print("no mes-dbg-lo property: run with rednavi_mes_step=5 rednavi_mes_dbgext=1")
        return 1
    body_hi = prop_dict(text, "mes-dbg-hi")
    lo, nlo = half_bytes(body, "l")
    hi, nhi = half_bytes(body_hi, "h") if body_hi else (bytes(1024), 0)
    buf = lo + hi
    print("dumped %d + %d dwords" % (nlo, nhi))

    m = re.search(r'"dbg_ok"\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)', text)
    s = re.search(r'"dbg_status"\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)', text)
    if m or s:
        print("SETUP_MES_DBGEXT: ok=%s status=%s" % (
            int(m.group(1), 0) if m else "?", int(s.group(1), 0) if s else "?"))

    rptr, wptr, size, hdr = struct.unpack_from("<4I", buf, 0)
    print("header: rptr=%#x wptr=%#x buffer_size=%#x header_size=%#x" % (rptr, wptr, size, hdr))

    if not (0 < size <= 1 << 20) or not (0 < hdr <= 64) or hdr >= size:
        print("no plausible header: the firmware did not initialize the buffer (frame rejected, or "
              "the image has no debug-extension support)")
        print("raw: %s" % printable(buf[:256]))
        return 1
    if rptr == wptr:
        print("the buffer holds no record (rptr == wptr): the firmware wrote nothing")
        return 0
    if rptr < hdr or rptr >= size or wptr >= size:
        print("rptr/wptr outside the buffer (%#x/%#x, header_size %#x, buffer_size %#x)"
              % (rptr, wptr, hdr, size))
        return 1

    off = rptr
    items = 0
    while off != wptr:
        if off + 8 > len(buf):
            print("... the next record is at %#x, past the %d bytes dumped (mes-dbg-lo/hi)"
                  % (off, len(buf)))
            break
        it = parse_item(buf, off)
        if it is None:
            print("@%#x: no valid record header; raw bytes: %s"
                  % (off, printable(buf[off:off + 48])))
            break
        typ, ln, hlen = it
        if off + ln > len(buf):
            print("... the record at %#x is truncated by the dumped window" % off)
            break
        name = {0x80: "MSG", 0x81: "ASSERT", 0x82: "HALT"}.get(typ, "0x%02x" % typ)
        print("%#06x %-6s %s" % (off, name, printable(buf[off + hlen:off + ln]).rstrip()))
        items += 1
        off += ln
        if off >= size:
            off = hdr + (off - size)
    if items == 0:
        print("no records decoded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
