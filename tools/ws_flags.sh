#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Which branch WSGetCompositorForAccelerator takes in the running WindowServer, with instruction-offset probes on the function (dtrace pid provider, root, 8 s), counted:
#   +24: active_compositor was just loaded (printed), +52: about to test the accelerator id (printed), +65: both tests passed, the Metal compositor is returned, +78: the software compositor is returned.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_flags.sh >/dev/null 2>&1 &'  (move a window while it runs)
OUT=/Users/Shared/ws_flags.txt
ERR=/Users/Shared/ws_flags_err.txt
rm -f $OUT $ERR
PID=$(pgrep -x WindowServer | head -1)
echo "start $(date) WindowServer $PID" > $ERR
dtrace -Z -p $PID -o $OUT -n '
pid$target:SkyLight:*WSGetCompositorForAccelerator*:24 { @a["+24 active_compositor (eax)", uregs[R_RAX] & 0xffffffff] = count(); }
pid$target:SkyLight:*WSGetCompositorForAccelerator*:52 { @a["+52 accelerator id (ebx)", uregs[R_RBX] & 0xffffffff] = count(); }
pid$target:SkyLight:*WSGetCompositorForAccelerator*:65 { @a["+65 both tests passed: Metal", 0] = count(); }
pid$target:SkyLight:*WSGetCompositorForAccelerator*:78 { @a["+78 software compositor", 0] = count(); }
tick-8s { printa("%s %x %@d\n", @a); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
