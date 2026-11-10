#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Looks inside the display machine when WindowServer's transaction asks isFramebufferActive: the array at +0x110 and what it points to (dtrace, root). Run: sudo sh -c 'nohup sh /Users/Shared/ws_trace5.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_props5.txt
ERR=/Users/Shared/ws_trace_err5.txt
rm -f $OUT $ERR
echo "start $(date)" > $ERR
dtrace -q -o $OUT -n '
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZNK21IOAccelDisplayMachine19isFramebufferActiveEj:entry {
  self->m = arg0; self->i = arg1;
  this->a = *(uint64_t *)(arg0 + 0x110);
  printf("isFramebufferActive(%d) machine %p array %p\n", (int)arg1, arg0, this->a);
  printf("  array: %p %p %p %p\n", *(uint64_t *)(this->a), *(uint64_t *)(this->a + 8), *(uint64_t *)(this->a + 16), *(uint64_t *)(this->a + 24));
  printf("  array: %p %p %p %p\n", *(uint64_t *)(this->a + 32), *(uint64_t *)(this->a + 40), *(uint64_t *)(this->a + 48), *(uint64_t *)(this->a + 56));
  printf("  array: %p %p %p %p\n", *(uint64_t *)(this->a + 64), *(uint64_t *)(this->a + 72), *(uint64_t *)(this->a + 80), *(uint64_t *)(this->a + 88));
  printf("  array: %p %p %p %p\n", *(uint64_t *)(this->a + 96), *(uint64_t *)(this->a + 104), *(uint64_t *)(this->a + 112), *(uint64_t *)(this->a + 120));
  printf("  machine +0x100: %p %p %p %p\n", *(uint64_t *)(arg0 + 0x100), *(uint64_t *)(arg0 + 0x108), *(uint64_t *)(arg0 + 0x110), *(uint64_t *)(arg0 + 0x118));
}
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZNK21IOAccelDisplayMachine19isFramebufferActiveEj:return { printf("  -> %d\n", (int)arg1); }
' >> $ERR 2>&1 &
DT=$!
sleep 8
echo "killing WindowServer $(pgrep -x WindowServer)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
sleep 50
kill $DT
chmod 644 $OUT $ERR
