#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Dump the DCN 3.2 display registers of an RX 7800 XT from a Linux live system (run as root, read-only).

Run it while the monitor is lit by amdgpu (for example the desktop of the live session). It reads every
register of tools/dcn32_reglist.txt through amdgpu's debugfs register file and prints "NAME offset value".
Output is flushed line by line, so a partial file survives if a read ever stalls.

    sudo python3 linux_dcn_dump.py dcn32_reglist.txt > dcn_linux.txt
"""
import glob
import os
import struct
import sys

DMU_BASE2 = 0x34C0


def find_card():
    for d in sorted(glob.glob("/sys/kernel/debug/dri/*")):
        try:
            if "amdgpu" in open(d + "/name").read() and os.path.exists(d + "/amdgpu_regs"):
                dev = open(d + "/device/device").read().strip() if os.path.exists(d + "/device/device") else ""
                if dev in ("", "0x747e"):
                    return d
        except OSError:
            pass
    raise SystemExit("no amdgpu debugfs for 1002:747e (amdgpu loaded? debugfs mounted? root?)")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    fd = os.open(find_card() + "/amdgpu_regs", os.O_RDONLY)
    for line in open(sys.argv[1]):
        name, off = line.split()
        v, = struct.unpack("<I", os.pread(fd, 4, (DMU_BASE2 + int(off, 16)) * 4))
        print(f"{name} {off} 0x{v:08x}", flush=True)


if __name__ == "__main__":
    main()
