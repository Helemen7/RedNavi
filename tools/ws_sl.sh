#!/bin/sh
# SkyLight's MetalContext / CompositorMetal calls counted for 8 s, with the first return values of Flush, into /Users/Shared/ws_sl.txt. Run in the foreground: sudo sh /Users/Shared/ws_sl.sh
OUT=/Users/Shared/ws_sl.txt
ERR=/Users/Shared/ws_sl_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:SkyLight:*MetalContext*:entry { @c[probefunc] = count(); }
pid$target:SkyLight:*CompositorMetal*:entry { @c[probefunc] = count(); }
pid$target:SkyLight:*MetalComposite*:entry { @c[probefunc] = count(); }
tick-8s { trunc(@c, 60); printa("%s %@d\n", @c); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
