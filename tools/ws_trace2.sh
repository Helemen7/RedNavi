#!/bin/sh
# What the accelerator family does for WindowServer's display pipe request (dtrace, root): entry and return of the family's DisplayPipe / DisplayMachine / newUserClient functions while WindowServer starts.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_trace2.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_props2.txt
ERR=/Users/Shared/ws_trace_err2.txt
rm -f $OUT $ERR
echo "start $(date)" > $ERR
dtrace -q -o $OUT -n '
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN*DisplayPipe*:entry /execname == "WindowServer" || execname == "kernel_task"/ { printf("> %s\n", probefunc); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN*DisplayPipe*:return /execname == "WindowServer" || execname == "kernel_task"/ { printf("< %s = 0x%x\n", probefunc, (int)arg1); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN*DisplayMachine*:entry /execname == "WindowServer" || execname == "kernel_task"/ { printf("> %s\n", probefunc); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN*DisplayMachine*:return /execname == "WindowServer" || execname == "kernel_task"/ { printf("< %s = 0x%x\n", probefunc, (int)arg1); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN22IOGraphicsAccelerator213newUserClient*:entry /execname == "WindowServer"/ { printf("> newUserClient type %d\n", (int)arg3); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN22IOGraphicsAccelerator213newUserClient*:return /execname == "WindowServer"/ { printf("< newUserClient = 0x%x\n", (int)arg1); }
' >> $ERR 2>&1 &
DT=$!
sleep 6
echo "killing WindowServer $(pgrep -x WindowServer)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
sleep 70
kill $DT
chmod 644 $OUT $ERR
