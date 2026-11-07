#!/bin/sh
# Why the display pipe is not active when WindowServer ends a transaction (IOAccelDisplayPipeUserClient2::transactionEnd returns kIOReturnNotReady when IOAccelDisplayPipe::isActive(),
# the byte at pipe+0x298, is 0; the pipe is client+232). Prints the pipe's bytes at transactionEnd and the results of the family's init_framebuffer_resource and of the pipe's
# initFramebufferResource, 10 s, into /Users/Shared/k_txend.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/k_txend.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/k_txend.txt
ERR=/Users/Shared/k_txend_err.txt
rm -f $OUT $ERR
echo "start $(date)" > $ERR
dtrace -o $OUT -n '
fbt::_ZN29IOAccelDisplayPipeUserClient214transactionEndEP33IOAccelDisplayPipeTransactionArgs:entry
{
  this->p = *(uint64_t *)(arg0 + 232);
  @s["transactionEnd pipe+0x298 (active)", *(uint8_t *)(this->p + 664), "pipe+0x299", *(uint8_t *)(this->p + 665), "pipe+0x282", *(uint8_t *)(this->p + 642)] = count();
}
fbt::*IOAccelDisplayPipe*init_framebuffer_resource*:entry { self->p = arg0; }
fbt::*IOAccelDisplayPipe*init_framebuffer_resource*:return / self->p / { @r["init_framebuffer_resource ret", arg1 & 0xff, "active after", *(uint8_t *)(self->p + 664)] = count(); self->p = 0; }
fbt::*IOAccelDisplayPipe*:entry / probefunc == "_ZN18IOAccelDisplayPipe4initEP22IOGraphicsAccelerator2P21IOAccelDisplayMachineP13IOFramebufferj" / { @r["pipe init", 0, "", 0] = count(); }
tick-10s { printa(@s); printa(@r); exit(0); }' >> $ERR 2>&1
chmod 644 $OUT $ERR
