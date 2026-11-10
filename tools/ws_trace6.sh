#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# What IOFramebuffer (IOGraphicsFamily) does for WindowServer's attribute and notification traffic (dtrace, root): names and the first arguments, into /Users/Shared/ws_props6.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_trace6.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_props6.txt
ERR=/Users/Shared/ws_trace_err6.txt
rm -f $OUT $ERR
echo "start $(date)" > $ERR
dtrace -q -o $OUT -n '
fbt:com.apple.iokit.IOGraphicsFamily:_ZN13IOFramebuffer*WSAA*:entry { printf("> %s %x %x %x\n", probefunc, (int)arg1, (int)arg2, (int)arg3); }
fbt:com.apple.iokit.IOGraphicsFamily:_ZN13IOFramebuffer*WSAA*:return { printf("< %s = 0x%x\n", probefunc, (int)arg1); }
fbt:com.apple.iokit.IOGraphicsFamily:_ZN13IOFramebuffer*Notification*:entry { printf("> %s %x %x\n", probefunc, (int)arg1, (int)arg2); }
fbt:com.apple.iokit.IOGraphicsFamily:_ZN13IOFramebuffer*ttribute*:entry /execname == "WindowServer" || execname == "kernel_task"/ { printf("> %s %x %x\n", probefunc, (int)arg1, (int)arg2); }
fbt:com.apple.iokit.IOGraphicsFamily:_ZN13IOFramebuffer*ttribute*:return /execname == "WindowServer" || execname == "kernel_task"/ { printf("< %s = 0x%x\n", probefunc, (int)arg1); }
fbt:com.apple.iokit.IOGraphicsFamily:_ZN13IOFramebuffer*ower*:entry { printf("> %s %x\n", probefunc, (int)arg1); }
fbt:com.apple.iokit.IOGraphicsFamily:_ZN13IOFramebuffer16probeAccelerator*:entry { printf("> %s\n", probefunc); }
' >> $ERR 2>&1 &
DT=$!
sleep 8
echo "killing WindowServer $(pgrep -x WindowServer)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
sleep 60
kill $DT
chmod 644 $OUT $ERR
