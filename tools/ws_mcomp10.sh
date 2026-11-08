#!/bin/sh
# What the display update gets from the compositor choice: inside WS::Displays::Update::Update, the accelerator id given to WSGetCompositorForAccelerator, whether WSGetCompositorMetal
# is reached and what it returns, and CompositorMetal::new_compositor (id, result), into /Users/Shared/ws_mcomp10.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_mcomp10.sh >/dev/null 2>&1 &'   then move/minimize a window for 8 s.
OUT=/Users/Shared/ws_mcomp10.txt
ERR=/Users/Shared/ws_mcomp10_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:SkyLight:*Displays*Update*Update*:entry { self->u = 1; }
pid$target:SkyLight:*Displays*Update*Update*:return { self->u = 0; }
pid$target:SkyLight:*WSGetCompositorForAccelerator*:entry /self->u/ { @a["Update: ForAccelerator acc", arg0] = count(); }
pid$target:SkyLight:WSGetCompositorMetal:entry /self->u/ { @a["Update: GetCompositorMetal acc", arg0] = count(); self->m = 1; }
pid$target:SkyLight:WSGetCompositorMetal:return /self->m/ { @a["Update: GetCompositorMetal ret", arg1] = count(); self->m = 0; }
pid$target:SkyLight:*CompositorMetal*new_compositor*:entry { @a["new_compositor acc", arg0] = count(); }
pid$target:SkyLight:*CompositorMetal*new_compositor*:return { @a["new_compositor ret", arg1] = count(); }
tick-8s { printa("%s %d %@d\n", @a); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
