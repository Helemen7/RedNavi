#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Order of events around WindowServer's display pipe client (type 4) while WindowServer starts (dtrace, root), into /Users/Shared/ws_props12.txt.
# Needs the hand-made client (boot-arg rednavi_accel2_pipe_hand=1). Shows, in time order: WindowServer's service opens (connect type), its IOConnect method calls (selector) and memory
# maps (memory type), the display pipe client's functions with return values, who runs clientClose (process and kernel stack), and IOAccelDisplayPipe::get_user_client with its caller.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_trace12.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_props12.txt
ERR=/Users/Shared/ws_trace_err12.txt
D=/Users/Shared/ws_trace12.d
rm -f $OUT $ERR
echo "start $(date)" > $ERR
cat > $D <<'EOD'
#pragma D option stackframes=14
fbt::is_io_service_open_extended:entry /execname == "WindowServer"/ { printf("%d OPEN type=%d\n", (int)(timestamp/1000000), (int)arg2); }
fbt::is_io_service_open_extended:return /execname == "WindowServer"/ { printf("%d OPEN-RET %x\n", (int)(timestamp/1000000), (int)arg1); }
fbt::is_io_connect_method:entry /execname == "WindowServer"/ { printf("%d CALL selector=%d\n", (int)(timestamp/1000000), (int)arg1); }
fbt::is_io_connect_map_memory_into_task:entry /execname == "WindowServer"/ { printf("%d MAPMEM type=%d\n", (int)(timestamp/1000000), (int)arg1); }
fbt::is_io_connect_set_notification_port:entry /execname == "WindowServer"/ { printf("%d NOTIFPORT type=%d\n", (int)(timestamp/1000000), (int)arg1); }
fbt::iokit_user_client_trap:entry /execname == "WindowServer"/ { printf("%d TRAP\n", (int)(timestamp/1000000)); }
fbt:com.apple.iokit.IOAcceleratorFamily2:*IOAccelDisplayPipeUserClient2*:entry { printf("%d UC %s (%s pid %d)\n", (int)(timestamp/1000000), probefunc, execname, pid); }
fbt:com.apple.iokit.IOAcceleratorFamily2:*IOAccelDisplayPipeUserClient2*:return { printf("%d UC-RET %s -> %x\n", (int)(timestamp/1000000), probefunc, (int)arg1); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN29IOAccelDisplayPipeUserClient211clientCloseEv:entry { printf("%d CLOSE by %s pid %d\n", (int)(timestamp/1000000), execname, pid); stack(); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZNK18IOAccelDisplayPipe15get_user_clientEv:entry { printf("%d GET_USER_CLIENT caller\n", (int)(timestamp/1000000)); stack(); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZNK18IOAccelDisplayPipe15get_user_clientEv:return { printf("%d GET_USER_CLIENT -> %p\n", (int)(timestamp/1000000), arg1); }
EOD
dtrace -Z -q -o $OUT -s $D >> $ERR 2>&1 &
DT=$!
sleep 8
echo "killing WindowServer $(pgrep -x WindowServer)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
sleep 50
kill $DT
chmod 644 $OUT $ERR
