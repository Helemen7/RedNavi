#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# The accelerator id WindowServer asks the Metal compositor map for, and what comes back (dtrace pid provider, root, 8 s), into /Users/Shared/ws_mcomp3.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_mcomp3.sh >/dev/null 2>&1 &'  (move a window while it runs)
OUT=/Users/Shared/ws_mcomp3.txt
ERR=/Users/Shared/ws_mcomp3_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:SkyLight:*WSGetCompositorForAccelerator*:entry { self->a0 = arg0; self->a1 = arg1; }
pid$target:SkyLight:*WSGetCompositorForAccelerator*:return { @acc["WSGetCompositorForAccelerator arg0", self->a0, "arg1", self->a1, "returned", arg1] = count(); }
pid$target:SkyLight:*CompositorMetal*GPURegistryID*:return { @reg["CompositorMetal::GPURegistryID returned", arg1] = count(); }
pid$target:SkyLight:*CDDisplay*compositor*:return { @cd["CDDisplay::compositor() returned", arg1] = count(); }
pid$target:SkyLight:*metal_compositor_map*:return { @mp["metal_compositor_map() returned", arg1] = count(); }
tick-8s { printa("%s %x %s %x %s %x %@d\n", @acc); printa("%s %x %@d\n", @reg); printa("%s %x %@d\n", @cd); printa("%s %x %@d\n", @mp); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
