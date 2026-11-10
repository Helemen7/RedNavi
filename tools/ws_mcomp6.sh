#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Return values (not arguments) of the compositor selection in the running WindowServer: WSMetalCompositorEnabled (active_compositor == 0 && metal_lovin),
# WSGetCompositorForAccelerator, HaveMetalCompositorForAccelerator, WSGetCompositorMetal; counted by value, into /Users/Shared/ws_mcomp6.txt.
# dtrace pid provider, root, 8 s. Run: sudo sh -c 'nohup sh /Users/Shared/ws_mcomp6.sh >/dev/null 2>&1 &'  (move a window while it runs)
OUT=/Users/Shared/ws_mcomp6.txt
ERR=/Users/Shared/ws_mcomp6_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:SkyLight:WSMetalCompositorEnabled:return { @ret["WSMetalCompositorEnabled", arg1 & 0xff] = count(); }
pid$target:SkyLight:HaveMetalCompositorForAccelerator:return { @ret["HaveMetalCompositorForAccelerator", arg1 & 0xff] = count(); }
pid$target:SkyLight:WSGetCompositorMetal:return { @ret["WSGetCompositorMetal(nonzero)", arg1 != 0] = count(); }
pid$target:SkyLight:*WSGetCompositorForAccelerator*:return { @ret["WSGetCompositorForAccelerator", arg1] = count(); }
pid$target:SkyLight:*activate_compositor*:entry { @ret["activate_compositor_*", 0] = count(); }
pid$target:SkyLight:WSCompositorActivateDefault:entry { @ret["WSCompositorActivateDefault", 0] = count(); }
tick-8s { printa("%s %d %@d\n", @ret); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
