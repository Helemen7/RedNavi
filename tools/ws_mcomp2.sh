#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Every SkyLight function with "ompositor" in its name that runs in the running WindowServer, counted (dtrace pid provider, root, 8 s), and the Compositor* that render_update receives,
# into /Users/Shared/ws_mcomp2.txt. Run: sudo sh -c 'nohup sh /Users/Shared/ws_mcomp2.sh >/dev/null 2>&1 &'  (move a window while it runs)
OUT=/Users/Shared/ws_mcomp2.txt
ERR=/Users/Shared/ws_mcomp2_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:SkyLight:*ompositor*:entry { @fn[probefunc] = count(); }
pid$target:SkyLight:*render_update*:entry { @ru["render_update compositor arg", arg2] = count(); }
pid$target:SkyLight:*WSGetCompositorMetal*:return { @gm["WSGetCompositorMetal returned", arg1] = count(); }
tick-8s { printa("%-60s %@d\n", @fn); printa("%s %x %@d\n", @ru); printa("%s %x %@d\n", @gm); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
