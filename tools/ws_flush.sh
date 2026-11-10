#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Which Objective-C selectors SkyLight's MetalContext::Flush sends (the commit of CoreAnimation's command buffer should be among them) and the arguments of Flush, 8 s, into /Users/Shared/ws_flush.txt
# (selectors by address: python3 tools/ws_msgs_decode.py /Users/Shared/ws_flush.txt on the desktop, same boot). Run in the foreground: sudo sh /Users/Shared/ws_flush.sh
OUT=/Users/Shared/ws_flush.txt
ERR=/Users/Shared/ws_flush_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:SkyLight:*MetalContext*Flush*:entry { self->in = 1; @args["Flush args", (int)(arg1 & 0xff), (int)arg2, (int)arg3, (int)(arg4 & 0xff)] = count(); }
pid$target:SkyLight:*MetalContext*Flush*:return { self->in = 0; }
pid$target:libobjc.A.dylib:objc_msgSend:entry /self->in/ { @sel[arg1] = count(); }
tick-8s { printa("%s %d %d %d %d %@d\n", @args); printa("SEL %x %@d\n", @sel); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
