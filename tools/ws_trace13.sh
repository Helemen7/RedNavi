#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# What WindowServer gets back from the display pipe client before it closes it: the value setPipeIndex writes (from the accelerator, +0x178), the size getCapabilitiesData writes, and the
# first 64 bytes of the capabilities data (dtrace, root), into /Users/Shared/ws_props13.txt. Needs the hand-made client (boot-arg rednavi_accel2_pipe_hand=1), as ws_trace12.sh.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_trace13.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_props13.txt
ERR=/Users/Shared/ws_trace_err13.txt
D=/Users/Shared/ws_trace13.d
rm -f $OUT $ERR
echo "start $(date)" > $ERR
cat > $D <<'EOD'
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN29IOAccelDisplayPipeUserClient212setPipeIndexEjPy:entry { self->idx = (int)arg1; self->out = arg2; self->uc = arg0; }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN29IOAccelDisplayPipeUserClient212setPipeIndexEjPy:return /self->out/ { printf("setPipeIndex(%d) -> %x, out value %x\n", self->idx, (int)arg1, *(uint32_t *)self->out); self->out = 0; }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN29IOAccelDisplayPipeUserClient219getCapabilitiesDataEPhPy:entry { self->cbuf = arg1; self->csize = arg2; self->cin = *(uint64_t *)arg2; }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN29IOAccelDisplayPipeUserClient219getCapabilitiesDataEPhPy:return /self->csize/ { printf("getCapabilitiesData -> %x, size in %d out %d\n", (int)arg1, (int)self->cin, (int)*(uint64_t *)self->csize); self->csize = 0; }
fbt:com.apple.iokit.IOGraphicsFamily:*IOFramebuffer*registerForInterruptType*:entry { printf("FB registerForInterruptType(type %x) by %s\n", (int)arg1, execname); }
fbt:com.apple.iokit.IOGraphicsFamily:*IOFramebuffer*registerForInterruptType*:return { printf("FB registerForInterruptType -> %x\n", (int)arg1); }
fbt:com.apple.iokit.IOGraphicsFamily:*IOFramebuffer*unregisterInterrupt*:entry { printf("FB unregisterInterrupt\n"); }
fbt:com.apple.iokit.IOGraphicsFamily:*IOFramebuffer*setInterruptState*:entry { printf("FB setInterruptState\n"); }
fbt:com.apple.iokit.IOGraphicsFamily:*IOFramebuffer*connectFlags*:entry { printf("FB connectFlags\n"); }
fbt:com.apple.iokit.IOGraphicsFamily:*IOFramebuffer*getTimingInfoForDisplayMode*:entry { printf("FB getTimingInfoForDisplayMode\n"); }
fbt:com.apple.iokit.IOGraphicsFamily:*IOFramebuffer*getVRAMRange*:entry { printf("FB getVRAMRange\n"); }
fbt:com.apple.iokit.IOGraphicsFamily:*IOFramebuffer*setCursor*:entry { printf("FB %s\n", probefunc); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN18IOAccelDisplayPipe20enableVBLInterruptEv:entry { printf("PIPE enableVBLInterrupt\n"); }
fbt:com.apple.iokit.IOAcceleratorFamily2:*IOAccelDisplayPipe*signalTransaction*:entry { printf("PIPE %s\n", probefunc); }
EOD
dtrace -Z -q -o $OUT -s $D >> $ERR 2>&1 &
DT=$!
sleep 8
echo "killing WindowServer $(pgrep -x WindowServer)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
sleep 40
kill $DT
chmod 644 $OUT $ERR
