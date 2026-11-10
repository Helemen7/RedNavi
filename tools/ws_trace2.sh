#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# What the family's display pipe and display machine functions do for WindowServer's startup (dtrace, root): entries and returns, into /Users/Shared/ws_props2.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_trace2.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_props2.txt
ERR=/Users/Shared/ws_trace_err2.txt
rm -f $OUT $ERR
echo "start $(date)" > $ERR
dtrace -q -o $OUT -n '
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN*DisplayPipe*:entry { printf("> %s\n", probefunc); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN*DisplayPipe*:return { printf("< %s = 0x%x\n", probefunc, (int)arg1); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZNK*DisplayPipe*:entry { printf("> %s\n", probefunc); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZNK*DisplayPipe*:return { printf("< %s = 0x%x\n", probefunc, (int)arg1); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN*DisplayMachine*:entry { printf("> %s\n", probefunc); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN*DisplayMachine*:return { printf("< %s = 0x%x\n", probefunc, (int)arg1); }
' >> $ERR 2>&1 &
DT=$!
sleep 8
echo "killing WindowServer $(pgrep -x WindowServer)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
sleep 60
kill $DT
chmod 644 $OUT $ERR
