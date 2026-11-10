#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Read-only dump of the curated DCN 3.2 pipe register list on Linux (run as root, amdgpu loaded, monitor lit, text console).

    sudo python3 linux_dcn_pipe_dump.py                          # six safe groups into dcn_pipe_<pci device id>.txt
    sudo python3 linux_dcn_pipe_dump.py --out mydump.txt hpd_aux4 dig4_dp4 clocks otg0 opp0 mpc

Options: --out FILE (default dcn_pipe_<device>.txt), --reglist FILE (default dcn32_pipe_reglist.txt next to this script).
Every line is written to the output file and fsynced before the next register is read, and a group marker is written (and
fsynced) before the first register of each group, so if a read ever locks the machine the file shows the group and the last
register reached.

Groups, safest first: hpd_aux4 dig4_dp4 clocks otg0 opp0 mpc hubbub hubp0 dpp0 dcnvm dcio; for a monitor on another DIG: hpd_aux<N> and dig<N>_dp<N> (hpd_aux1 dig1_hdmi for DIG1); tools/linux_collect.py picks them.
Do NOT read the group unsafe_hdmi_afmt: the HDMI encoder and audio format blocks are gated when unused and reading them froze a machine. With no group names it reads the first six.
The list is for the DisplayPort path of the Navi 32 development board (HPD4/AUX4/DIG4): if your monitor is on another connector
the DIG4/HPD4 registers read as idle (DIG4_DIG_FE_CNTL 0). The DMU register bases are read from the kernel's IP discovery in sysfs
(hw_id 271) and fall back to Navi 32's.
"""
import glob
import os
import re
import struct
import sys

NAVI32_BASES = {0: 0x12, 1: 0xC0, 2: 0x34C0, 3: 0x9000}   # DMU block bases of the development card (Navi 32)
DEFAULT = ["hpd_aux4", "dig4_dp4", "clocks", "otg0", "opp0", "mpc"]


def find_card():
    """Returns (debugfs dir, PCI device id string, PCI address) of the first amdgpu card. The debugfs name file reads
    'amdgpu dev=0000:04:00.0 unique=...'; the device id comes from the PCI device in sysfs."""
    for d in sorted(glob.glob("/sys/kernel/debug/dri/*")):
        try:
            name = open(d + "/name").read()
            if "amdgpu" not in name or not os.path.exists(d + "/amdgpu_regs"):
                continue
            m = re.search(r"dev=([0-9a-fA-F:.]+)", name)
            bdf = m.group(1) if m else ""
            dev = open("/sys/bus/pci/devices/%s/device" % bdf).read().strip() if bdf else ""
            return d, dev, bdf
        except OSError:
            pass
    raise SystemExit("no amdgpu debugfs (amdgpu loaded? debugfs mounted? root?)")


def dmu_bases(bdf):
    """DMU (hw_id 271) base addresses from /sys/bus/pci/devices/<bdf>/ip_discovery, or None."""
    try:
        for inst in sorted(glob.glob("/sys/bus/pci/devices/%s/ip_discovery/die/0/*/[0-9]*" % bdf)):
            if open(inst + "/hw_id").read().strip() == "271":
                return {i: int(l, 16) for i, l in enumerate(open(inst + "/base_addr").read().split())}
    except (OSError, ValueError):
        pass
    return None


def dump(card, dev, reglist, out_path, groups, bases):
    """Reads the registers of the given groups into out_path (appending), fsyncing every line."""
    regs = [l.split() for l in open(reglist) if l.strip()]
    fd = os.open(card + "/amdgpu_regs", os.O_RDONLY)
    out = open(out_path, "a")
    values = {}

    def emit(text):
        out.write(text + "\n")
        out.flush()
        os.fsync(out.fileno())

    emit("# device %s, DMU bases %s" % (dev, " ".join(hex(bases[i]) for i in sorted(bases))))
    ones = 0                                         # consecutive reads of 0xffffffff: the card has stopped answering on the bus
    for g in groups:
        emit(f"# group {g}")
        for grp, name, idx, off in regs:
            if grp != g:
                continue
            addr = (bases[int(idx)] + int(off, 16)) * 4
            v, = struct.unpack("<I", os.pread(fd, 4, addr))
            values[name] = v
            emit(f"{name} {idx} {off} 0x{v:08x}")
            ones = ones + 1 if v == 0xFFFFFFFF else 0
            if ones >= 4:
                emit(f"# ABORT: the GPU stopped answering (4 reads of 0xffffffff in a row) after {name}")
                out.close()
                raise SystemExit("the GPU stopped answering after %s (see %s); stopped reading" % (name, out_path))
        emit(f"# done {g}")
    out.close()
    os.close(fd)
    return values


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    args = sys.argv[1:]
    out_path = reglist = None
    groups = []
    while args:
        a = args.pop(0)
        if a in ("-h", "--help"):
            sys.exit(__doc__)
        elif a == "--out" and args:
            out_path = args.pop(0)
        elif a == "--reglist" and args:
            reglist = args.pop(0)
        elif a.startswith("-"):
            sys.exit("unknown option %s\n\n%s" % (a, __doc__))
        else:
            groups.append(a)
    if len(groups) >= 2 and groups[0].endswith(".txt") and groups[1].endswith(".txt"):   # the old form: REGLIST OUT [GROUP...]
        reglist, out_path, groups = groups[0], groups[1], groups[2:]
    card, dev, bdf = find_card()
    out_path = out_path or "dcn_pipe_%s.txt" % (dev or "unknown")
    bases = dmu_bases(bdf)
    if bases is None:
        bases = NAVI32_BASES
        print("warning: no ip_discovery in sysfs, using Navi 32's DMU bases %s" % {i: hex(b) for i, b in bases.items()}, file=sys.stderr)
    elif bases != NAVI32_BASES:
        print("note: this card's DMU bases differ from Navi 32's: %s" % {i: hex(b) for i, b in bases.items()}, file=sys.stderr)
    dump(card, dev, reglist or os.path.join(here, "dcn32_pipe_reglist.txt"), out_path, groups or DEFAULT, bases)
    print("wrote %s" % out_path, file=sys.stderr)


if __name__ == "__main__":
    main()
