#!/bin/sh
# Who asks WSGetCompositorForAccelerator and with which arguments; where the display's accelerator id comes from (dtrace pid provider, root, 6 s), into /Users/Shared/ws_mcomp4.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_mcomp4.sh >/dev/null 2>&1 &'  (move a window while it runs)
OUT=/Users/Shared/ws_mcomp4.txt
ERR=/Users/Shared/ws_mcomp4_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -x ustackframes=12 -p $PID -o $OUT -n '
pid$target:SkyLight:*WSGetCompositorForAccelerator*:entry { @s["entry", arg0, arg1, ustack(10)] = count(); }
pid$target:SkyLight:*CDDisplay*compositor*:entry { @c[probefunc, ustack(8)] = count(); }
pid$target:CoreDisplay:*ccelerator*:entry { @a[probefunc] = count(); }
pid$target:SkyLight:*ccelerator*:entry { @b[probefunc] = count(); }
tick-6s { printa("%s %x %x %k %@d\n", @s); printa(@c); printa("CoreDisplay %s %@d\n", @a); printa("SkyLight %s %@d\n", @b); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
