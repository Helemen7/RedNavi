#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Everything the accelerator family calls while it handles WindowServer's client type 4 (the display pipe): dtrace, root. Run: sudo sh -c 'nohup sh /Users/Shared/ws_trace3.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_props3.txt
ERR=/Users/Shared/ws_trace_err3.txt
rm -f $OUT $ERR
echo "start $(date)" > $ERR
dtrace -q -o $OUT -n '
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN22IOGraphicsAccelerator213newUserClient*:entry /execname == "WindowServer" && (int)arg3 == 4/ { self->t = 1; printf("== newUserClient type 4\n"); }
fbt:com.apple.iokit.IOAcceleratorFamily2::entry /self->t/ { printf("> %s\n", probefunc); }
fbt:com.apple.iokit.IOAcceleratorFamily2::return /self->t/ { printf("< %s = 0x%x\n", probefunc, (int)arg1); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN22IOGraphicsAccelerator213newUserClient*:return /self->t/ { self->t = 0; printf("== returned 0x%x\n", (int)arg1); }
' >> $ERR 2>&1 &
DT=$!
sleep 8
echo "killing WindowServer $(pgrep -x WindowServer)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
sleep 70
kill $DT
chmod 644 $OUT $ERR
