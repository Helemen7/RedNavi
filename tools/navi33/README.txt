RedNavi, RX 7600 (Navi 33): read-only Linux register dump of GC 11.0.2

Needs: Linux booted with amdgpu loaded on the RX 7600, root, python3.
The script only reads GC/SDMA/MP registers (no display blocks) and writes a text file.

    sudo python3 linux_gc_regs.py gc_regs_navi33.h out_gc.txt

The script keeps GFXOFF disabled while it reads (writes 0 to debugfs amdgpu_gfxoff), because GC registers read 0 otherwise.
If it says "no amdgpu debugfs": run  sudo mount -t debugfs none /sys/kernel/debug  and try again.
If the card is not the first amdgpu device, set RN_DEVID=0x7480 (default).

Please send out_gc.txt, and the output of:  uname -r ; cat /proc/cmdline
