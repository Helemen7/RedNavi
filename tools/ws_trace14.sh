#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Why the Metal plug-in's allocations fail ("vkAllocateMemory ... out of device memory"): return values of the probe kext's buffer allocator, GPU page-table mapper and page-table
# page allocator while WindowServer restarts (dtrace, root), counted per value, into /Users/Shared/ws_props14.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_trace14.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_props14.txt
ERR=/Users/Shared/ws_trace_err14.txt
D=/Users/Shared/ws_trace14.d
rm -f $OUT $ERR
echo "start $(date)" > $ERR
cat > $D <<'EOD'
fbt:org.rednavi.RedNavi:*createVramBo*:return { @bo[arg1 == 0xffffffff ? "createVramBo FAIL(-1)" : "createVramBo ok"] = count(); }
fbt:org.rednavi.RedNavi:*vmMap*:return { @map[arg1 & 0xff] = count(); }
fbt:org.rednavi.RedNavi:*vmPageAlloc*:return /arg1 == 0/ { @pg["vmPageAlloc returned 0 (pool empty)"] = count(); }
fbt:org.rednavi.RedNavi:*vmPageAlloc*:return /arg1 != 0/ { @pg["vmPageAlloc ok"] = count(); }
tick-70s { printa("%s %@d\n", @bo); printa("vmMap return %d: %@d\n", @map); printa("%s %@d\n", @pg); exit(0); }
EOD
dtrace -Z -q -o $OUT -s $D >> $ERR 2>&1 &
DT=$!
sleep 6
echo "killing WindowServer $(pgrep -x WindowServer)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
wait $DT
chmod 644 $OUT $ERR
