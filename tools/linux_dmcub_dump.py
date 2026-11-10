#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Dump the DMCUB registers of an RX 7800 XT from a Linux live system (run as root).

Reads through amdgpu's debugfs register file, so nothing is written. Compare the output with the
"dmcub-snap-*" properties the RedNavi probe publishes on macOS (docs/logs).

    sudo python3 linux_dmcub_dump.py
"""
import glob
import os
import struct

DMU_BASE = 0x34C0   # base index 2 of the DMU block in this card's IP discovery table

REGS = {
    "CW0_BASE": 0x1A5, "CW1_BASE": 0x1A6, "CW2_BASE": 0x1A7, "CW3_BASE": 0x1A8, "CW4_BASE": 0x1A9,
    "CW5_BASE": 0x1AA, "CW6_BASE": 0x1AB, "CW7_BASE": 0x1AC,
    "CW0_TOP": 0x1AD, "CW1_TOP": 0x1AE, "CW2_TOP": 0x1AF, "CW3_TOP": 0x1B0, "CW4_TOP": 0x1B1,
    "CW5_TOP": 0x1B2, "CW6_TOP": 0x1B3, "CW7_TOP": 0x1B4,
    "CW0_OFFSET": 0x1B5, "CW0_OFFSET_HI": 0x1B6, "CW1_OFFSET": 0x1B7, "CW1_OFFSET_HI": 0x1B8,
    "CW2_OFFSET": 0x1B9, "CW2_OFFSET_HI": 0x1BA, "CW3_OFFSET": 0x1BB, "CW3_OFFSET_HI": 0x1BC,
    "CW4_OFFSET": 0x1BD, "CW4_OFFSET_HI": 0x1BE, "CW5_OFFSET": 0x1BF, "CW5_OFFSET_HI": 0x1C0,
    "CW6_OFFSET": 0x1C1, "CW6_OFFSET_HI": 0x1C2, "CW7_OFFSET": 0x1C3, "CW7_OFFSET_HI": 0x1C4,
    "REGION5_OFFSET": 0x198, "REGION5_OFFSET_HI": 0x199, "REGION5_TOP": 0x1A2,
    "TMR_AXI_SPACE": 0x21A, "SEC_CNTL": 0x1CE, "MEM_CNTL": 0x1CF,
    "INBOX1_BASE": 0x1D4, "INBOX1_SIZE": 0x1D5, "INBOX1_WPTR": 0x1D6, "INBOX1_RPTR": 0x1D7,
    "OUTBOX0_BASE": 0x1D8, "OUTBOX0_SIZE": 0x1D9, "OUTBOX1_BASE": 0x1DC, "OUTBOX1_SIZE": 0x1DD,
    "SCRATCH0": 0x1E3, "SCRATCH1": 0x1E4, "SCRATCH2": 0x1E5, "SCRATCH7": 0x1EA, "SCRATCH14": 0x1F1,
    "SCRATCH15": 0x1F2, "CNTL": 0x1F6, "CNTL2": 0x200, "FAULT_ADDR": 0x1FA, "TIMER": 0x1FD,
    "MMHUBBUB_SOFT_RESET": 0x342,
}


def find_card():
    for d in sorted(glob.glob("/sys/kernel/debug/dri/*")):
        try:
            if "amdgpu" in open(d + "/name").read() and os.path.exists(d + "/amdgpu_regs"):
                vendor = open(d + "/device/vendor").read().strip() if os.path.exists(d + "/device/vendor") else ""
                dev = open(d + "/device/device").read().strip() if os.path.exists(d + "/device/device") else ""
                if dev in ("", "0x747e"):
                    return d
        except OSError:
            pass
    raise SystemExit("no amdgpu debugfs for 1002:747e found (is amdgpu loaded, debugfs mounted, running as root?)")


def main():
    card = find_card()
    print("card", card)
    fd = os.open(card + "/amdgpu_regs", os.O_RDONLY)
    for name, dword in REGS.items():
        v, = struct.unpack("<I", os.pread(fd, 4, (DMU_BASE + dword) * 4))
        print(f"{name:<22} 0x{v:08x}")


if __name__ == "__main__":
    main()
