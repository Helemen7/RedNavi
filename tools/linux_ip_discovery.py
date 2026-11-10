#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Print the IP blocks of an AMD GPU (IP discovery: hardware id, version, instance, base addresses) from sysfs on Linux.

    python3 linux_ip_discovery.py > ip_discovery.txt        # first AMD GPU
    python3 linux_ip_discovery.py 0x7480 > ip_discovery.txt # a given PCI device id

Needs a kernel that exports /sys/class/drm/cardN/device/ip_discovery (Linux 6.x with amdgpu loaded). No root, read-only.
"""
import glob
import os
import sys


def read(path):
    try:
        return open(path).read().split()
    except OSError:
        return []


def main():
    want = sys.argv[1].lower() if len(sys.argv) > 1 else None
    for dev in sorted(glob.glob("/sys/class/drm/card[0-9]*/device")):
        if "".join(read(dev + "/vendor")) != "0x1002":
            continue
        did = "".join(read(dev + "/device")).lower()
        if want and did != want:
            continue
        print("# %s vendor 0x1002 device %s revision %s" % (dev, did, "".join(read(dev + "/revision"))))
        root = dev + "/ip_discovery"
        if not os.path.isdir(root):
            sys.exit("no %s (kernel too old, or amdgpu not loaded)" % root)
        for die in sorted(glob.glob(root + "/die/*")):
            for ip in sorted(glob.glob(die + "/*")):
                for inst in sorted(glob.glob(ip + "/[0-9]*")):
                    fields = {k: "".join(read(inst + "/" + k)) for k in ("hw_id", "num_instance", "major", "minor", "revision")}
                    bases = " ".join(read(inst + "/base_addr"))
                    print("%s inst %s hw_id %s num_instance %s version %s.%s.%s bases %s" % (
                        os.path.basename(ip), os.path.basename(inst), fields["hw_id"], fields["num_instance"],
                        fields["major"], fields["minor"], fields["revision"], bases))
        return
    sys.exit("no AMD GPU found")


if __name__ == "__main__":
    main()
