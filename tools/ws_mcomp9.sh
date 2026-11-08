#!/bin/sh
# During an animation: the Compositor* that render_update receives, the object (this) of CompositorSW / CompositorMetal CompositeLayersToDestination, and counts of both, into /Users/Shared/ws_mcomp9.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_mcomp9.sh >/dev/null 2>&1 &'   then minimize and restore a window repeatedly for 8 s.
OUT=/Users/Shared/ws_mcomp9.txt
ERR=/Users/Shared/ws_mcomp9_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:SkyLight:*CDDisplay*render_update*:entry { @a["render_update compositor", arg2] = count(); }
pid$target:SkyLight:*CompositorSW*CompositeLayersToDestination*:entry { @a["SW composite this", arg0] = count(); }
pid$target:SkyLight:*CompositorMetal*CompositeLayersToDestination*:entry { @a["Metal composite this", arg0] = count(); }
tick-8s { printa("%s %d %@d\n", @a); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
