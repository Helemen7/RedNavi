#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Which class receives newTextureWithDescriptor:offset:bytesPerRow: (and who creates the buffers) in the running WindowServer (dtrace objc provider, root), into /Users/Shared/ws_objc.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_objc.sh >/dev/null 2>&1 &'      (6 s, does not restart WindowServer)
OUT=/Users/Shared/ws_objc.txt
ERR=/Users/Shared/ws_objc_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -x ustackframes=8 -p $PID -o $OUT -n '
objc$target:*:*newTextureWithDescriptor*offset*:entry { @tex[probemod, probefunc] = count(); }
objc$target:*:*newBufferWith*:entry { @buf[probemod, probefunc] = count(); }
objc$target:RedNaviMTLTexture:*initWithDevice*:entry { @cal[probemod, probefunc] = count(); }
objc$target:RedNaviMTLDevice:*newTextureWithDescriptor*:entry { @cal[probemod, probefunc] = count(); }
objc$target:*:*texture*Buffer*:entry { @alt[probemod, probefunc] = count(); }
objc$target:*:*newTexture*:entry { @alt[probemod, probefunc] = count(); }
objc$target:*:*newTiledTexture*:entry { @alt[probemod, probefunc] = count(); }
objc$target:*:*initStandin*:entry { @alt[probemod, probefunc] = count(); }
objc$target:*:*bufferClass*:entry { @alt[probemod, probefunc] = count(); }
tick-6s { printa("TEX %s %s %@d\n", @tex); printa("BUF %s %s %@d\n", @buf); printa("CAL %s %s %@d\n", @cal); printa("ALT %s %s %@d\n", @alt); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
