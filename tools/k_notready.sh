#!/bin/sh
# Which kernel function returns kIOReturnNotReady (0xe00002d8) to the display pipe user client calls that WindowServer makes (CoreDisplay's transaction submission fails with it).
# fbt on the IOAccelDisplayPipe user clients, the pipe, the display machine and the accelerator; counts by function and prints one stack. 10 s, into /Users/Shared/k_notready.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/k_notready.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/k_notready.txt
ERR=/Users/Shared/k_notready_err.txt
rm -f $OUT $ERR
echo "start $(date)" > $ERR
dtrace -x stackframes=10 -o $OUT -n '
fbt::*IOAccelDisplay*:return, fbt::*IOGraphicsAccelerator2*:return, fbt::*IOAccelShared*:return
/ arg1 == 0xe00002d8 /
{ @n[probemod, probefunc] = count(); }
fbt::*IOAccelDisplayPipeUserClient2*:entry { @e[probefunc] = count(); }
fbt::*IOAccelDisplayPipe*:return / arg1 == 0xe00002d8 / { @s[probefunc, stack()] = count(); }
tick-10s { printa("ret NotReady %s %s %@d\n", @n); printa("entry %s %@d\n", @e); printa(@s); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
