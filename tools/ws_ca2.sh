#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Why CoreAnimation's command buffers are released without commit: CA::OGL functions with commit / abort / cancel / discard / flush / finish / submit / error in the name, and every
# selector the plug-in's command buffer receives, counted for 8 s into /Users/Shared/ws_ca2.txt. Run in the foreground: sudo sh /Users/Shared/ws_ca2.sh
OUT=/Users/Shared/ws_ca2.txt
ERR=/Users/Shared/ws_ca2_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:QuartzCore:*MetalContext*ommit*:entry, pid$target:QuartzCore:*MetalContext*bort*:entry, pid$target:QuartzCore:*MetalContext*ancel*:entry,
pid$target:QuartzCore:*MetalContext*iscard*:entry, pid$target:QuartzCore:*MetalContext*lush*:entry, pid$target:QuartzCore:*MetalContext*inish*:entry,
pid$target:QuartzCore:*MetalContext*ubmit*:entry, pid$target:QuartzCore:*MetalContext*rror*:entry, pid$target:QuartzCore:*MetalContext*command_buffer*:entry,
pid$target:QuartzCore:*MetalContext*ailed*:entry { @c[probefunc] = count(); }
objc$target:RedNaviMTLCommandBuffer:*:entry { @o[probefunc] = count(); }
tick-8s { printa("ca %s %@d\n", @c); printa("rn %s %@d\n", @o); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
