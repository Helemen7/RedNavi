#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Functions with "ompositor" in the name that run inside WS::Updater::UpdateDisplays on the way to render_update, with their return values, into /Users/Shared/ws_mcomp11.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_mcomp11.sh >/dev/null 2>&1 &'   then move/minimize a window for 8 s.
OUT=/Users/Shared/ws_mcomp11.txt
ERR=/Users/Shared/ws_mcomp11_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:SkyLight:*UpdateDisplays*:entry { self->ud = 1; }
pid$target:SkyLight:*UpdateDisplays*:return { self->ud = 0; }
pid$target:SkyLight:*ompositor*:entry /self->ud/ { @e[probefunc] = count(); }
pid$target:SkyLight:*CDDisplay*:entry /self->ud/ { @e[probefunc] = count(); }
pid$target:SkyLight:*CDDisplay*:return /self->ud/ { @r[probefunc, arg1] = count(); }
pid$target:SkyLight:*ompositor*:return /self->ud/ { @r[probefunc, arg1] = count(); }
tick-8s { printa("entry %s %@d\n", @e); printa("ret %s %d %@d\n", @r); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
