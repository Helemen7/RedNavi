#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Prints the attr-table of RedNaviFB (what WindowServer asks the framebuffer and what it was answered): ioreg -rc RedNaviFB -w0 | python3 tools/rn_fbtable.py
import re, struct, sys
m = re.search(r'"attr-table" = <([0-9a-f]*)>', sys.stdin.read())
b = bytes.fromhex(m.group(1)) if m else b''
for i in range(0, len(b), 24):
    k, sel, res, cnt, val = struct.unpack('<IIiIQ', b[i:i + 24])
    print({1: 'set', 2: 'get', 3: 'setconn', 4: 'getconn'}.get(k, k), sel.to_bytes(4, 'little')[::-1].decode('latin1'), 'res=0x%x' % (res & 0xffffffff), 'n=%d' % cnt, 'val=0x%x' % val)
