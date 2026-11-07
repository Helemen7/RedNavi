#!/bin/sh
# Who creates and picks CompositorSW when WindowServer starts: dtrace -W waits for the next WindowServer, the script then kills the running one; the stacks of every CompositorSW and
# CompositorMetal entry (counted per stack, rare ones are the constructors and the choice) go to /Users/Shared/ws_start_sw.txt. Run: sudo sh -c 'nohup sh /Users/Shared/ws_start_sw.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_start_sw.txt
ERR=/Users/Shared/ws_start_sw_err.txt
rm -f $OUT $ERR
echo "start $(date)" > $ERR
dtrace -Z -x ustackframes=14 -W WindowServer -o $OUT -n '
pid$target:SkyLight:*CompositorSW*:entry { @sw[probefunc, ustack(14)] = count(); }
pid$target:SkyLight:*CompositorMetal*:entry { @mt[probefunc, ustack(14)] = count(); }
pid$target:SkyLight:*CDDisplay*render_update*:entry { @ru[probefunc, ustack(8)] = count(); }
tick-45s { printa(@sw); printa(@mt); printa(@ru); exit(0); }' >> $ERR 2>&1 &
sleep 3
echo "killing WindowServer $(pgrep -x WindowServer) $(date)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
sleep 55
chmod 644 $OUT $ERR
