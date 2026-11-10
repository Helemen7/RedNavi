#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Which Objective-C selectors QuartzCore sends inside CA::OGL::MetalContext::copy_image_to_texture (the function that logs "nil client storage texture from buffer"), counted by selector address (dtrace cannot read the names: they live in the shared cache; tools/ws_msgs_decode.py turns them into names), into
# /Users/Shared/ws_msgs.txt (dtrace pid provider on the running WindowServer, root, 6 s, WindowServer is not restarted).
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_msgs.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_msgs.txt
ERR=/Users/Shared/ws_msgs_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:QuartzCore:*copy_image_to_texture*:entry { self->in = 1; @fn["entered"] = count(); }
pid$target:QuartzCore:*copy_image_to_texture*:return { self->in = 0; }
pid$target:libobjc.A.dylib:objc_msgSend:entry /self->in/ { @sel[arg1] = count(); }
pid$target:libobjc.A.dylib:objc_msgSend_stret:entry /self->in/ { @sel[arg2] = count(); }
pid$target:libobjc.A.dylib:objc_retain:entry /self->in/ { @ret = count(); }
tick-6s { printa("%s %@d\n", @fn); printa("SEL %x %@d\n", @sel); printa("RETAIN %@d\n", @ret); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
