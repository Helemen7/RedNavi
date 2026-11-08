#!/bin/sh
# The functions that decide whether WindowServer composes the display with Metal: calls and return values of HardwareSupportsMetalCompositor, HaveMetalCompositorForAccelerator,
# WSMetalCompositorEnabled and WSGetCompositorMetal in the running WindowServer (dtrace pid provider, root, 10 s), into /Users/Shared/ws_mcomp.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_mcomp.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_mcomp.txt
ERR=/Users/Shared/ws_mcomp_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -x ustackframes=10 -p $PID -o $OUT -n '
pid$target:SkyLight:*HardwareSupportsMetalCompositor*:entry { @call["HardwareSupportsMetalCompositor entry", ustack(6)] = count(); }
pid$target:SkyLight:*HardwareSupportsMetalCompositor*:return { @ret["HardwareSupportsMetalCompositor return", arg1] = count(); }
pid$target:SkyLight:*HaveMetalCompositorForAccelerator*:entry { @call["HaveMetalCompositorForAccelerator entry", ustack(6)] = count(); }
pid$target:SkyLight:*HaveMetalCompositorForAccelerator*:return { @ret["HaveMetalCompositorForAccelerator return", arg1] = count(); }
pid$target:SkyLight:*WSMetalCompositorEnabled*:return { @ret["WSMetalCompositorEnabled return", arg1] = count(); }
pid$target:SkyLight:*WSGetCompositorMetal*:return { @ret["WSGetCompositorMetal return", arg1] = count(); }
tick-10s { printa(@ret); printa(@call); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
