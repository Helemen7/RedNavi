#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Look at the SMU (MP1) mailbox of the RX 7800 XT on Linux, and optionally send the same harmless messages
RedNavi sends on macOS, to compare behaviour (run as root, amdgpu loaded, debugfs mounted).

    sudo python3 linux_smu_mailbox.py read  out.txt        # registers only
    sudo python3 linux_smu_mailbox.py send  out.txt        # registers, then GetSmuVersion, GetDriverIfVersion, TestMessage
    sudo python3 linux_smu_mailbox.py send  out.txt bar5   # same, but through BAR5 (sysfs resource5), amdgpu NOT loaded

The 'bar5' variant is the one that matches RedNavi: boot Linux with the kernel parameter modprobe.blacklist=amdgpu
(the screen stays on the iGPU or the firmware framebuffer), so nothing but this script touches the card. A
dpm=0 boot is not usable on this card (black screen).

Every line is fsynced before the next access, so a lock-up leaves a trail. 'send' follows the Linux protocol
(smu_msg_v1_send_msg): wait for a non-zero response, clear C2PMSG_90, write the argument to C2PMSG_82, the
message to C2PMSG_66, poll C2PMSG_90. The messages are read-only queries, but they share the mailbox with the
running driver: do it right after boot, with nothing else touching the GPU, and preferably in the two boots
described in docs/COMPAT.md (normal, and with the kernel parameter amdgpu.dpm=0).
"""
import glob
import os
import struct
import sys
import time

MP1_BASE = 0x16000                        # this card's IP discovery table: MP1 base index 0
MP0_BASE = 0x16000                        # MP0 shares the block
REGS = [("MP1_C2PMSG_%d" % n, MP1_BASE + 0x282 + (n - 66)) for n in range(66, 91)]
REGS += [("MP0_C2PMSG_33", MP0_BASE + 0x61), ("MP0_C2PMSG_35", MP0_BASE + 0x63), ("MP0_C2PMSG_81", MP0_BASE + 0x91)]
SMN_FW_FLAGS = 0x03B00000 | 0x3010024      # MP1_Public | smnMP1_FIRMWARE_FLAGS, read through the PCIe index/data debugfs file
MSGS = [("GetSmuVersion", 0x2, 0), ("GetDriverIfVersion", 0x3, 0), ("TestMessage", 0x1, 0x12345678)]
RESP = {0x1: "OK", 0xFF: "Failed", 0xFE: "UnknownCmd", 0xFD: "CmdRejectedPrereq", 0xFC: "CmdRejectedBusy"}


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


def find_bar5():
    for d in sorted(glob.glob("/sys/bus/pci/devices/*")):
        try:
            if open(d + "/vendor").read().strip() == "0x1002" and open(d + "/device").read().strip() == "0x747e":
                return d
        except OSError:
            pass
    raise SystemExit("no 1002:747e on the PCI bus")


def main():
    if len(sys.argv) not in (3, 4) or sys.argv[1] not in ("read", "send") or (len(sys.argv) == 4 and sys.argv[3] != "bar5"):
        sys.exit(__doc__)
    bar5 = len(sys.argv) == 4
    out = open(sys.argv[2], "a")
    mode = sys.argv[1]

    def emit(text):
        out.write(text + "\n")
        out.flush()
        os.fsync(out.fileno())

    if bar5:
        import mmap
        dev = find_bar5()
        with open(dev + "/enable", "w") as f:           # memory decode on, no driver bound
            f.write("1")
        rfd = os.open(dev + "/resource5", os.O_RDWR | os.O_SYNC)
        mm = mmap.mmap(rfd, 512 * 1024)
        emit("# BAR5 via " + dev)

        def rd(dword):
            return struct.unpack_from("<I", mm, dword * 4)[0]

        def wr(dword, v):
            struct.pack_into("<I", mm, dword * 4, v)
    else:
        card = find_card()
        fd = os.open(card + "/amdgpu_regs", os.O_RDWR if mode == "send" else os.O_RDONLY)

        def rd(dword):
            return struct.unpack("<I", os.pread(fd, 4, dword * 4))[0]

        def wr(dword, v):
            os.pwrite(fd, struct.pack("<I", v), dword * 4)

    emit("# kernel cmdline: " + open("/proc/cmdline").read().strip())
    emit("# date: " + time.strftime("%Y-%m-%d %H:%M:%S"))
    emit("# registers (dword offset, value)")
    for name, off in REGS:
        emit("%s 0x%05x 0x%08x" % (name, off, rd(off)))

    if bar5:
        NBIF1 = 0x14                                    # NBIF base index 1: RSMU_INDEX at +0, RSMU_DATA at +1
        wr(NBIF1, SMN_FW_FLAGS)
        v = rd(NBIF1 + 1)
        emit("MP1_FIRMWARE_FLAGS (SMN 0x%08x) 0x%08x  interrupts_enabled=%d" % (SMN_FW_FLAGS, v, v & 1))
    elif os.path.exists(card + "/amdgpu_regs_pcie"):
        pcie = card + "/amdgpu_regs_pcie"
        fp = os.open(pcie, os.O_RDONLY)
        v = struct.unpack("<I", os.pread(fp, 4, SMN_FW_FLAGS))[0]
        emit("MP1_FIRMWARE_FLAGS (SMN 0x%08x) 0x%08x  interrupts_enabled=%d" % (SMN_FW_FLAGS, v, v & 1))
    else:
        emit("# no amdgpu_regs_pcie file, MP1_FIRMWARE_FLAGS not read")

    if mode == "send":
        msg66, arg82, resp90 = MP1_BASE + 0x282, MP1_BASE + 0x292, MP1_BASE + 0x29A
        for name, msg, arg in MSGS:
            deadline = time.time() + 1.0
            while time.time() < deadline and rd(resp90) == 0:      # previous message finished?
                time.sleep(0.001)
            if rd(resp90) == 0:
                emit("%s: previous response never arrived, not sending" % name)
                break
            wr(resp90, 0)
            wr(arg82, arg)
            wr(msg66, msg)
            t0 = time.time()
            deadline = t0 + 1.0
            while time.time() < deadline and rd(resp90) == 0:
                time.sleep(0.0002)
            r = rd(resp90)
            emit("%s: msg 0x%x arg 0x%08x -> response 0x%x (%s) after %.1f ms, argument register 0x%08x" % (
                name, msg, arg, r, RESP.get(r, "none" if r == 0 else "?"), (time.time() - t0) * 1000, rd(arg82)))
            if r == 0:
                break
        emit("# registers after")
        for name, off in REGS[:3] + REGS[16:25]:
            emit("%s 0x%05x 0x%08x" % (name, off, rd(off)))
    emit("# done")


if __name__ == "__main__":
    main()
