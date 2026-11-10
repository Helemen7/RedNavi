#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Why IOPresentmentBegin returns NULL (CoreDisplay: "Fallback GPU Display Pipe transaction submission failed"): return values of the IOPresentment set-up functions in the
# running WindowServer, and the IOKit user-client calls made from inside them, into /Users/Shared/ws_mcomp13.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_mcomp13.sh >/dev/null 2>&1 &'   (then move a window for 8 s; to see the set-up itself restart WindowServer a few seconds after starting this)
OUT=/Users/Shared/ws_mcomp13.txt
ERR=/Users/Shared/ws_mcomp13_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:IOPresentment:*IOPresentmentCreateForRegistryService*:return { @r[probefunc, arg1] = count(); }
pid$target:IOPresentment:*IOPresentmentInitialize*:return { @r[probefunc, arg1 & 0xff] = count(); }
pid$target:IOPresentment:*IOPresentmentBegin*:return { @r[probefunc, arg1 != 0] = count(); }
pid$target:IOPresentment:*_IOPresentmentGetDeviceCount*:return { @r[probefunc, arg1 & 0xff] = count(); }
pid$target:IOPresentment:*IOPresentmentGetMap*:return { @r[probefunc, arg1 != 0] = count(); }
pid$target:IOPresentment:*IOPresentmentCopyCapabilities*:return { @r[probefunc, arg1 != 0] = count(); }
pid$target:IOPresentment:*ClientInitialized*:return { @r[probefunc, arg1 & 0xff] = count(); }
pid$target:IOKit:IOServiceOpen:return { @r["IOServiceOpen", arg1] = count(); }
pid$target:IOKit:IOConnectCallMethod:return { @r["IOConnectCallMethod", arg1] = count(); }
pid$target:IOKit:IOConnectCallStructMethod:return { @r["IOConnectCallStructMethod", arg1] = count(); }
tick-8s { printa("%s %d %@d\n", @r); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
