#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Argument and return value of WSGetCompositorForAccelerator, its callers, and the Compositor* that CDDisplay::render_update receives, counted, into /Users/Shared/ws_mcomp8.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_mcomp8.sh >/dev/null 2>&1 &'   (minimize a window while it runs)
OUT=/Users/Shared/ws_mcomp8.txt
ERR=/Users/Shared/ws_mcomp8_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:SkyLight:*WSGetCompositorForAccelerator*:entry { self->acc = arg0; @arg["acc", arg0] = count(); @caller["caller", ustack(3)] = count(); }
pid$target:SkyLight:*WSGetCompositorForAccelerator*:return { @arg["ret", arg1] = count(); }
pid$target:SkyLight:*CDDisplay*render_update*:entry { @arg["render_update compositor ptr", arg2] = count(); }
pid$target:SkyLight:*compositor_sw:entry { @arg["x", 0] = count(); }
tick-8s { printa("%s %d %@d\n", @arg); printa(@caller); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
