#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Who sets the "active" byte of the display pipe (pipe+0x298)? Run on a Mac whose Intel (or AMD) accelerator works, read only (dtrace, root).
# Traces every function of IOAccelDisplayPipe / IGAccelDisplayPipe / IOAccelDisplayMachine / IGAccelDisplayMachine; after each return it reads pipe+0x282 and +0x298 (bytes)
# and machine+0x10c (uint32), and prints the call (only) whenever one of them changed since the last print, with its arguments and the kernel stack, so the first call that takes
# +0x298 from 0 to 1 shows with its caller.  The offsets come from a study of another driver and are only measured on the RedNavi desktop.
#
# Run: sudo sh -c 'nohup sh tools/pipe_active_trace.sh >/dev/null 2>&1 &'      (writes /Users/Shared/pipe_active.txt and pipe_active_err.txt)
# It then puts the displays to sleep and wakes them, and kills WindowServer, to make the pipe go through its power and start-up paths again; move the mouse if the screen stays dark.
OUT=/Users/Shared/pipe_active.txt
ERR=/Users/Shared/pipe_active_err.txt
rm -f $OUT $ERR
echo "start $(date)" > $ERR
D=/Users/Shared/pipe_active.d
cat > $D <<'EOD'
int st_a; int st_d; int st_n; int seen;
fbt::*18IOAccelDisplayPipe*:entry, fbt::*17IGAccelDisplayPipe*:entry
{ this->d = ++self->d; self->o[this->d] = arg0; self->a1[this->d] = arg1; self->a2[this->d] = arg2; self->k[this->d] = 0; }
fbt::*21IOAccelDisplayMachine*:entry, fbt::*20IGAccelDisplayMachine*:entry
{ this->d = ++self->d; self->o[this->d] = arg0; self->a1[this->d] = arg1; self->a2[this->d] = arg2; self->k[this->d] = 1; }

fbt::*18IOAccelDisplayPipe*:return, fbt::*17IGAccelDisplayPipe*:return,
fbt::*21IOAccelDisplayMachine*:return, fbt::*20IGAccelDisplayMachine*:return
/ self->d > 0 /
{
  this->d = self->d; this->o = self->o[this->d];
  this->p = self->k[this->d] ? *(uint64_t *)(this->o + 0x88) : this->o;
  this->m = self->k[this->d] ? this->o : *(uint64_t *)(this->o + 0x90);
  this->a = this->p ? *(uint8_t *)(this->p + 0x298) : 255;
  this->w = this->p ? *(uint8_t *)(this->p + 0x282) : 255;
  this->n = this->m ? *(uint32_t *)(this->m + 0x10c) : 255;
  this->chg = (!seen) || this->a != st_a || this->w != st_d || this->n != st_n;
}
fbt::*18IOAccelDisplayPipe*:return, fbt::*17IGAccelDisplayPipe*:return,
fbt::*21IOAccelDisplayMachine*:return, fbt::*20IGAccelDisplayMachine*:return
/ self->d > 0 && this->chg /
{
  printf("%d %s %s(%d,%d) ret %x pipe=%p [298]=%d [282]=%d machine[10c]=%d  CHANGED (was %d %d %d) depth %d\n", (int)(timestamp / 1000000), probemod, probefunc,
         (int)self->a1[this->d], (int)self->a2[this->d], (int)arg1, this->p, this->a, this->w, this->n, st_a, st_d, st_n, this->d);
  stack();
  st_a = this->a; st_d = this->w; st_n = this->n; seen = 1;
}
fbt::*18IOAccelDisplayPipe*:return, fbt::*17IGAccelDisplayPipe*:return,
fbt::*21IOAccelDisplayMachine*:return, fbt::*20IGAccelDisplayMachine*:return
/ self->d > 0 /
{ self->d--; }
EOD
dtrace -q -x stackframes=12 -o $OUT -s $D >> $ERR 2>&1 &
DT=$!
sleep 6
echo "display sleep $(date)" >> $ERR
pmset displaysleepnow >> $ERR 2>&1
sleep 8
echo "wake $(date)" >> $ERR
caffeinate -u -t 2 >> $ERR 2>&1
sleep 8
echo "killing WindowServer $(pgrep -x WindowServer) $(date)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
sleep 40
kill $DT
chmod 644 $OUT $ERR
