#!/bin/sh
# Who creates, enqueues and commits the plug-in's command buffers in WindowServer: the callers (3 frames) of commandBuffer creation, enqueue and commit, counted for 8 s, into /Users/Shared/ws_ca3.txt.
# Run in the foreground: sudo sh /Users/Shared/ws_ca3.sh
OUT=/Users/Shared/ws_ca3.txt
ERR=/Users/Shared/ws_ca3_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
objc$target:RedNaviMTLCommandBuffer:-initWithQueue?device?:entry { @c["create", ustack(4)] = count(); }
objc$target:RedNaviMTLCommandBuffer:-enqueue:entry { @c["enqueue", ustack(4)] = count(); }
objc$target:RedNaviMTLCommandBuffer:-commit:entry { @c["commit", ustack(4)] = count(); }
tick-8s { printa("%s %k %@d\n", @c); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
