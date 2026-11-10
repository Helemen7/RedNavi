#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""One-command data collection for a new AMD GPU on Linux: IP discovery, connectors, firmware log lines and the display pipe
registers of the output that is lit. Read-only. Run as root, from a text console (Ctrl+Alt+F3), with amdgpu loaded and the
monitor on:

    sudo python3 tools/linux_collect.py

It writes rednavi_report_<pci device id>_<date>/ and rednavi_report_<...>.tar.gz next to where you run it. Attach the
.tar.gz to the GitHub issue and say which monitor and which connector (DP/HDMI) you used.

What is collected, and what is not:
  - the PCI id, BARs and kernel version (lspci, uname, os-release); serial numbers lines are dropped
  - the IP blocks with versions and register bases (from sysfs ip_discovery)
  - the connector names, their status and mode list (from /sys/class/drm). The EDID is NOT collected: it carries the
    monitor serial number
  - the amdgpu lines of the kernel log (firmware files, IP blocks, VRAM)
  - the DCN register groups of the lit output: the clock group first, which says which DIG drives the monitor, then the
    HPD/AUX and DIG registers of that DIG and the OTG0/OPP0/MPC state
"""
import datetime
import glob
import importlib.util
import os
import re
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def groups_for(fe):
    """The HPD/AUX and DIG groups of DIG front end `fe`, using the names in the pipe register list."""
    dig = {1: "dig1_hdmi", 4: "dig4_dp4"}.get(fe, "dig%d_dp%d" % (fe, fe))
    return ["hpd_aux%d" % fe, dig]


def main():
    if os.geteuid() != 0:
        sys.exit("run as root: sudo python3 tools/linux_collect.py")
    dump = load("linux_dcn_pipe_dump")
    card, dev, bdf = dump.find_card()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M")
    out = "rednavi_report_%s_%s" % (dev, stamp)
    os.makedirs(out)
    print("collecting into %s" % out)

    # system and PCI (serial number lines dropped)
    info = ["kernel: " + run(["uname", "-r"]).strip()]
    osr = open("/etc/os-release").read() if os.path.exists("/etc/os-release") else ""
    info += [l for l in osr.splitlines() if l.startswith(("NAME=", "VERSION="))]
    lspci = run(["lspci", "-nnvv", "-d", "1002:"])
    lspci = "\n".join(l for l in lspci.splitlines() if not re.search(r"serial", l, re.I))
    write(out + "/system.txt", "\n".join(info) + "\n\n" + lspci)

    # IP discovery
    write(out + "/ip_discovery.txt", run([sys.executable, os.path.join(HERE, "linux_ip_discovery.py"), dev]))

    # connectors: names, state, modes; no EDID
    lines = []
    for c in sorted(glob.glob("/sys/class/drm/card[0-9]*-*")):
        def rd(f):
            try:
                return open(c + "/" + f).read().split()
            except OSError:
                return []
        lines.append("%s status=%s enabled=%s modes=%s" % (os.path.basename(c), "".join(rd("status")), "".join(rd("enabled")),
                                                            ",".join(rd("modes")[:12])))
    write(out + "/connectors.txt", "\n".join(lines) + "\n")

    # kernel log lines of the driver
    log = run(["dmesg"]) or run(["journalctl", "-k", "--no-pager"])
    write(out + "/amdgpu_dmesg.txt", "\n".join(l for l in log.splitlines() if re.search(r"amdgpu|\[drm\]", l, re.I)) + "\n")

    # the DCN registers: clocks first (which DIG is lit), then that DIG
    bases = dump.dmu_bases(bdf) or dump.NAVI32_BASES
    reglist = os.path.join(HERE, "dcn32_pipe_reglist.txt")
    pipe = out + "/dcn_pipe.txt"
    values = dump.dump(card, dev, reglist, pipe, ["clocks"], bases)
    lit = []
    for i, letter in enumerate("ABCDEF"):
        v = values.get("SYMCLK%s_CLOCK_ENABLE" % letter)
        if v is not None and v & 1:
            lit.append((letter, (v >> 8) & 7))
    if not lit:
        print("no lit output found: is the monitor on, and are you on a text console with amdgpu loaded?")
        groups = ["otg0", "opp0", "mpc"]
    else:
        print("lit: " + ", ".join("SYMCLK%s with DIG front end %d" % (l, fe) for l, fe in lit))
        groups = []
        for _, fe in lit:
            for g in groups_for(fe):
                if g not in groups:
                    groups.append(g)
        groups += ["otg0", "opp0", "mpc"]
    values.update(dump.dump(card, dev, reglist, pipe, groups, bases))
    asleep = values.get("OTG0_OTG_CONTROL") == 0xFFFFFFFF
    if asleep:
        print("WARNING: the display engine registers read 0xffffffff, so the GPU is probably in a power-saving state (runtime PM) and "
              "this dump is not usable.\nKeep the screen awake (move the mouse, no screen blanking), then run\n"
              "    echo on | sudo tee /sys/bus/pci/devices/%s/power/control\nand rerun this script." % bdf)
        write(out + "/WARNING_display_asleep.txt", "OTG0_OTG_CONTROL read 0xffffffff: the display engine was powered down during the dump.\n")
    write(out + "/lit_outputs.txt", "\n".join("SYMCLK%s DIG front end %d" % x for x in lit) + "\n")

    tar = out + ".tar.gz"
    with tarfile.open(tar, "w:gz") as t:
        t.add(out)
    print("done: attach %s to the issue (and tell us the monitor model and the cable: DP or HDMI)" % tar)


if __name__ == "__main__":
    main()
