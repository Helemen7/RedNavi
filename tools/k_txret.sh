#!/bin/sh
# Return values (as 32-bit hex) of the display pipe user client's transaction calls, and which functions run inside transactionEnd, 10 s, into /Users/Shared/k_txret.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/k_txret.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/k_txret.txt
ERR=/Users/Shared/k_txret_err.txt
rm -f $OUT $ERR
echo "start $(date)" > $ERR
dtrace -o $OUT -n '
fbt::*IOAccelDisplayPipeUserClient2*:return / probefunc != "_ZN29IOAccelDisplayPipeUserClient217sendAsyncResult64EPyiS0_j" / { @r[probefunc, arg1 & 0xffffffff] = count(); }
fbt::*IOAccelDisplayPipe*:return / probefunc == "_ZN18IOAccelDisplayPipe15transaction_endEP29IOAccelDisplayPipeUserClient2P33IOAccelDisplayPipeTransactionArgs" / { @r[probefunc, arg1 & 0xffffffff] = count(); }
fbt::_ZN29IOAccelDisplayPipeUserClient214transactionEndEP33IOAccelDisplayPipeTransactionArgs:entry { self->in = 1; }
fbt::_ZN29IOAccelDisplayPipeUserClient214transactionEndEP33IOAccelDisplayPipeTransactionArgs:return { self->in = 0; }
fbt::*IOAccel*:entry / self->in / { @c[probefunc] = count(); }
tick-10s { printa("ret %s %x %@d\n", @r); printa("in transactionEnd: %s %@d\n", @c); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
