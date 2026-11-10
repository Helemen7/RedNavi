#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Turns the selector addresses of ws_msgs.txt (tools/ws_msgs.sh) into selector names with sel_getName: the shared cache is mapped at the same address in every process of a boot,
# so run this on the machine and in the same boot. usage: python3 ws_msgs_decode.py /Users/Shared/ws_msgs.txt
import ctypes, re, sys
objc = ctypes.CDLL('/usr/lib/libobjc.A.dylib')
objc.sel_getName.restype = ctypes.c_char_p
objc.sel_getName.argtypes = [ctypes.c_void_p]
rows = []
for l in open(sys.argv[1]):
    m = re.match(r'SEL ([0-9a-f]+)\s+(\d+)', l)
    if m:
        try: name = objc.sel_getName(int(m.group(1), 16)).decode('latin1')
        except Exception as e: name = '?' + str(e)
        rows.append((int(m.group(2)), name))
for c, n in sorted(rows, reverse=True): print('%6d  %s' % (c, n))
