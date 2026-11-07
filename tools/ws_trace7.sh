#!/bin/sh
# The arguments of the framebuffer resource calls of the display machine and pipe while WindowServer starts (dtrace, root), into /Users/Shared/ws_props7.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_trace7.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_props7.txt
ERR=/Users/Shared/ws_trace_err7.txt
rm -f $OUT $ERR
echo "start $(date)" > $ERR
dtrace -q -o $OUT -n '
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN21IOAccelDisplayMachine23initFramebufferResourceEjjP13IOFramebufferP16IOAccelResource2:entry { printf("machine.initFramebufferResource(a=%d b=%d fb=%p res=%p)\n", (int)arg1, (int)arg2, arg3, arg4); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN21IOAccelDisplayMachine23initFramebufferResourceEjjP13IOFramebufferP16IOAccelResource2:return { printf("  -> %x\n", (int)arg1); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN18IOAccelDisplayPipe23initFramebufferResourceEjP16IOAccelResource2:entry { printf("pipe.initFramebufferResource(a=%d res=%p)\n", (int)arg1, arg2); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN18IOAccelDisplayPipe25init_framebuffer_resourceEP16IOAccelResource2:entry { printf("pipe.init_framebuffer_resource(res=%p)\n", arg1); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZNK21IOAccelDisplayMachine*getFramebufferResource*:entry { printf("machine.getFramebufferResource(%d,%d)\n", (int)arg1, (int)arg2); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZNK21IOAccelDisplayMachine*getFramebufferResource*:return { printf("  -> %p\n", arg1); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZNK21IOAccelDisplayMachine19isFramebufferActiveEj:entry { printf("machine.isFramebufferActive(%d)\n", (int)arg1); }
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZNK21IOAccelDisplayMachine19isFramebufferActiveEj:return { printf("  -> %d\n", (int)arg1); }
' >> $ERR 2>&1 &
DT=$!
sleep 8
echo "killing WindowServer $(pgrep -x WindowServer)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
sleep 50
kill $DT
chmod 644 $OUT $ERR
