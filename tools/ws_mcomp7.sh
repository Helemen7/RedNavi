#!/bin/sh
# Which compositor really renders: entries into CompositorSW / CompositorMetal methods in the running WindowServer, counted by function, into /Users/Shared/ws_mcomp7.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_mcomp7.sh >/dev/null 2>&1 &'  (minimize a window while it runs)
OUT=/Users/Shared/ws_mcomp7.txt
ERR=/Users/Shared/ws_mcomp7_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:SkyLight:*CompositorSW*:entry { @c["SW", probefunc] = count(); }
pid$target:SkyLight:*CompositorMetal*:entry { @c["Metal", probefunc] = count(); }
pid$target:SkyLight:*MetalComposite*:entry { @c["MetalFn", probefunc] = count(); }
tick-8s { trunc(@c, 25); printa("%s %s %@d\n", @c); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
