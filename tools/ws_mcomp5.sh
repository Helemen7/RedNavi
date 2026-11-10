#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# What WSGetCompositorForAccelerator calls before it returns (functions entered inside it, in SkyLight, CoreDisplay, IOKit and libsystem), counted, into /Users/Shared/ws_mcomp5.txt.
# dtrace pid provider on the running WindowServer, root, 6 s. Run: sudo sh -c 'nohup sh /Users/Shared/ws_mcomp5.sh >/dev/null 2>&1 &'  (move a window while it runs)
OUT=/Users/Shared/ws_mcomp5.txt
ERR=/Users/Shared/ws_mcomp5_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:SkyLight:*WSGetCompositorForAccelerator*:entry { self->in = 1; self->depth = 0; }
pid$target:SkyLight:*WSGetCompositorForAccelerator*:return { self->in = 0; }
pid$target:SkyLight::entry /self->in/ { @fn["SkyLight", probefunc] = count(); }
pid$target:CoreDisplay::entry /self->in/ { @fn["CoreDisplay", probefunc] = count(); }
pid$target:IOKit::entry /self->in/ { @fn["IOKit", probefunc] = count(); }
pid$target:libsystem_kernel.dylib::entry /self->in/ { @fn["kernel", probefunc] = count(); }
pid$target:libsystem_c.dylib:*:entry /self->in/ { @fn["libc", probefunc] = count(); }
tick-6s { printa("%s %s %@d\n", @fn); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
