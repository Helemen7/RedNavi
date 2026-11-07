#!/bin/sh
# Where in SkyLight "Unsupported sw composite destination." is logged from: user stack of every os_log call, counted per (format pointer, stack) (dtrace pid provider on the running
# WindowServer, root; the most frequent one is the message, comparing the string in dtrace fails on pages of the shared cache), into /Users/Shared/ws_ustack.txt. Does not restart WindowServer.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_ustack.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_ustack.txt
ERR=/Users/Shared/ws_ustack_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -x ustackframes=24 -p $PID -o $OUT -n '
pid$target:libsystem_trace.dylib:_os_log_impl:entry { @s[arg3, ustack(24)] = count(); }
tick-6s { exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
