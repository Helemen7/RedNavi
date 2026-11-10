#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Read the GC / SDMA registers RedNavi programs (RedNavi/gc_regs.h) on Linux, to see what a healthy card shows (run as root).

    sudo python3 linux_gc_regs.py ../RedNavi/gc_regs.h out.txt         # amdgpu loaded, through debugfs amdgpu_regs
    sudo python3 linux_gc_regs.py ../RedNavi/gc_regs.h out.txt bar5    # driver not loaded (modprobe.blacklist=amdgpu), through BAR5

GC register bases of this card (IP discovery): 0x1260 0xa000 0x1c000 0x2402c00. Every line is fsynced before the next read.
"""
import glob
import mmap
import os
import re
import struct
import sys

GC_BASES = [0x1260, 0xA000, 0x1C000, 0x2402C00]
NBIO_BASES = [0x0, 0x14, 0xD20, 0x10400, 0x241B000, 0x4040000]     # NBIF segments of this card (IP discovery)
EXTRA = [("MP1_C2PMSG_66", 0x16282), ("MP1_C2PMSG_82", 0x16292), ("MP1_C2PMSG_90", 0x1629A), ("MP0_C2PMSG_81", 0x16091)]


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    regs = []
    for path in sys.argv[1].split(","):       # several headers may be given, comma separated: gc_regs.h,nbio_regs.h
        text = open(path).read()
        bases = NBIO_BASES if "nbio" in os.path.basename(path) else GC_BASES
        for name, off in re.findall(r"#define reg(\w+) (0x[0-9a-fA-F]+)\n", text):
            m = re.search(r"#define reg%s_BASE_IDX (\d+)" % re.escape(name), text)
            if m:
                regs.append((name, bases[int(m.group(1))] + int(off, 16)))
    regs += EXTRA
    out = open(sys.argv[2], "a")

    def emit(line):
        out.write(line + "\n")
        out.flush()
        os.fsync(out.fileno())

    if len(sys.argv) == 4 and sys.argv[3] == "bar5":
        dev = None
        for d in sorted(glob.glob("/sys/bus/pci/devices/*")):
            try:
                if open(d + "/vendor").read().strip() == "0x1002" and open(d + "/device").read().strip() == "0x747e":
                    dev = d
            except OSError:
                pass
        if not dev:
            sys.exit("no 1002:747e")
        with open(dev + "/enable", "w") as f:
            f.write("1")
        mm = mmap.mmap(os.open(dev + "/resource5", os.O_RDWR | os.O_SYNC), 512 * 1024)
        read = lambda dword: struct.unpack_from("<I", mm, dword * 4)[0]
        emit("# BAR5 via " + dev)
    else:
        card = None
        for d in sorted(glob.glob("/sys/kernel/debug/dri/*")):
            try:
                if "amdgpu" in open(d + "/name").read() and os.path.exists(d + "/amdgpu_regs"):
                    card = d
                    break
            except OSError:
                pass
        if not card:
            sys.exit("no amdgpu debugfs (driver loaded? root?)")
        fd = os.open(card + "/amdgpu_regs", os.O_RDONLY)
        # At idle the GC is power-gated (GFXOFF) and every GC/SDMA register reads 0: keep it awake while reading.
        gfxoff = None
        if os.path.exists(card + "/amdgpu_gfxoff"):
            gfxoff = os.open(card + "/amdgpu_gfxoff", os.O_RDWR)
            os.write(gfxoff, struct.pack("<I", 0))
        read = lambda dword: struct.unpack("<I", os.pread(fd, 4, dword * 4))[0]
    if len(sys.argv) == 3:
        emit("# gfxoff control: " + ("disabled" if gfxoff is not None else "amdgpu_gfxoff debugfs file not found; boot with amdgpu.ppfeaturemask=0xffff7fff"))
    emit("# kernel cmdline: " + open("/proc/cmdline").read().strip())
    for name, dword in regs:
        if len(sys.argv) == 4 and dword * 4 + 4 > 512 * 1024:      # only the BAR5 route cannot reach beyond the register BAR
            emit("%s 0x%x outside BAR5" % (name, dword))
            continue
        emit("%s 0x%05x 0x%08x" % (name, dword, read(dword)))
    emit("# done")


if __name__ == "__main__":
    main()
