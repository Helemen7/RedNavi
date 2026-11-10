#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Why the display surface has no IOSurface (and so WindowServer composes in software): CoreDisplay::UseIOPresentment() result, which path GetNextSurface takes
# (DisplaySurfacePool = IOSurface, CGXAcquireDisplayDeviceSurface = legacy), into /Users/Shared/ws_mcomp12.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_mcomp12.sh >/dev/null 2>&1 &'   then move/minimize a window for 8 s.
OUT=/Users/Shared/ws_mcomp12.txt
ERR=/Users/Shared/ws_mcomp12_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target::*UseIOPresentment*:return { @a["UseIOPresentment ret", arg1 & 0xff] = count(); }
pid$target::*DisplaySurfacePool*GetNextSurface*:entry { @a["DisplaySurfacePool::GetNextSurface", 0] = count(); }
pid$target::CGXAcquireDisplayDeviceSurface:entry { @a["CGXAcquireDisplayDeviceSurface", 0] = count(); }
pid$target::CGXAcquireDisplayDeviceSurface:entry { @a["SkyLight CGXAcquireDisplayDeviceSurface", 0] = count(); }
pid$target::*Display*GetNextSurface*:entry { @a["Display::GetNextSurface", 0] = count(); }
tick-8s { printa("%s %d %@d\n", @a); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
