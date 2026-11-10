#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# The framebuffer notification events the display pipe receives (dtrace, root): display_change_handler(refcon, framebuffer, event, info), into /Users/Shared/ws_props8.txt. Run: sudo sh -c 'nohup sh /Users/Shared/ws_trace8.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_props8.txt
ERR=/Users/Shared/ws_trace_err8.txt
rm -f $OUT $ERR
echo "start $(date)" > $ERR
dtrace -q -o $OUT -n '
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN18IOAccelDisplayPipe22display_change_handler*:entry { printf("handler event=%d\n", (int)arg3); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN18IOAccelDisplayPipe22display_change_handler*:return { printf("  -> %d\n", (int)arg1); }
fbt:com.apple.iokit.IOGraphicsFamily:_ZN13IOFramebuffer27deliverFramebufferNotification*:entry { printf("deliverFramebufferNotification index=%d\n", (int)arg1); }
' >> $ERR 2>&1 &
DT=$!
sleep 8
echo "killing WindowServer $(pgrep -x WindowServer)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
sleep 40
kill $DT
chmod 644 $OUT $ERR
