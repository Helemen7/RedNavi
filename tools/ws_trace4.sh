#!/bin/sh
# What the display pipe functions of the family return while WindowServer runs (no restart): 20 seconds, to /Users/Shared/ws_props4.txt. Run: sudo sh /Users/Shared/ws_trace4.sh
OUT=/Users/Shared/ws_props4.txt
rm -f $OUT
dtrace -q -o $OUT -n '
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN*DisplayPipe*:entry { printf("> %s\n", probefunc); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN*DisplayPipe*:return { printf("< %s = 0x%x\n", probefunc, (int)arg1); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZNK*DisplayPipe*:return { printf("< %s = 0x%x\n", probefunc, (int)arg1); }
tick-20s { exit(0); }
'
chmod 644 $OUT
