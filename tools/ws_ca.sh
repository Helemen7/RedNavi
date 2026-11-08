#!/bin/sh
# What CoreAnimation's Metal renderer does inside WindowServer: counts of the CA::OGL::MetalContext functions that run in 8 s, and of the Metal calls it makes on the command buffer / encoders
# (objc provider on the plug-in's classes), into /Users/Shared/ws_ca.txt. Run in the foreground: sudo sh /Users/Shared/ws_ca.sh
OUT=/Users/Shared/ws_ca.txt
ERR=/Users/Shared/ws_ca_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:QuartzCore:*MetalContext*:entry { @c[probefunc] = count(); }
pid$target:QuartzCore:*CA*OGL*Renderer*render*:entry { @c[probefunc] = count(); }
objc$target:RedNaviMTLCommandBuffer:*:entry { @o[probefunc] = count(); }
objc$target:RedNaviMTLRenderCommandEncoder:*:entry { @o[probefunc] = count(); }
tick-8s { trunc(@c, 40); trunc(@o, 40); printa("ca %s %@d\n", @c); printa("rn %s %@d\n", @o); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
