#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Compare the IP discovery table the probe read on macOS with the one Linux exports in sysfs.

    python3 tools/compare_discovery.py ioreg.txt ip_discovery.txt

First argument: what tools/decode_discovery.py accepts (the output of `ioreg -rn RedNaviProbe -l -x`, the probe's log lines,
or the raw binary). Second argument: the text written by tools/linux_ip_discovery.py (step 0 of docs/ADDING_A_GPU.md).

Every block is matched on (hardware id, instance) and its version and base addresses are compared. The exit status is 0
when the two agree on all of them and 1 when they do not, or when an input cannot be read. Read only: it opens the two files.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decode_discovery as dd  # noqa: E402

LINUX_LINE = re.compile(
    r"^\s*\S+\s+inst\s+\d+\s+hw_id\s+(\d+)\s+num_instance\s+(\d+)\s+version\s+(\d+)\.(\d+)\.(\d+)\s+bases\s*((?:0x[0-9a-fA-F]+\s*)*)$")


def parse_binary(b):
    """{(hw_id, instance): ((major, minor, rev), [bases])} of a discovery table (layout of Linux discovery.h)."""
    if len(b) < 16:
        raise ValueError("the table is shorter than its header")
    sig, vmaj = struct.unpack_from("<IH", b, 0)
    if sig != dd.BINARY_SIGNATURE:
        raise ValueError("bad binary signature 0x%08x" % sig)
    tbl_base = 16 if vmaj >= 2 else 12
    ip_off = struct.unpack_from("<H", b, tbl_base)[0]
    tsig, tver, _, _, num_dies = struct.unpack_from("<IHHIH", b, ip_off)
    if tsig != dd.DISCOVERY_TABLE_SIGNATURE:
        raise ValueError("bad IP table signature 0x%08x" % tsig)
    pad_off = ip_off + 14 + 4 * 16
    wide = tver == 4 and bool(b[pad_off] & 1)
    out = {}
    for d in range(min(num_dies, 16)):
        _, die_off = struct.unpack_from("<HH", b, ip_off + 14 + 4 * d)
        num_ips = struct.unpack_from("<H", b, die_off + 2)[0]
        pos = die_off + 4
        for _ in range(num_ips):
            hw, inst, nbase, major, minor, rev, _ = struct.unpack_from("<HBBBBBB", b, pos)
            pos += 8
            width = 8 if wide else 4
            bases = [int.from_bytes(b[pos + width * k:pos + width * (k + 1)], "little") for k in range(nbase)]
            pos += width * nbase
            key = (hw, inst)
            if key in out:
                raise ValueError("hardware id %d instance %d appears twice (die %d)" % (hw, inst, d))
            out[key] = ((major, minor, rev), bases)
    return out


def parse_linux(text):
    """The same dictionary from the text of tools/linux_ip_discovery.py."""
    out = {}
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        m = LINUX_LINE.match(line)
        if not m:
            raise ValueError("line %d is not an IP block line: %s" % (n, line.strip()[:80]))
        key = (int(m.group(1)), int(m.group(2)))
        value = ((int(m.group(3)), int(m.group(4)), int(m.group(5))), [int(x, 16) for x in m.group(6).split()])
        # sysfs lists a block under its number and under its name (35 and ATHUB): the same block twice is one block.
        if key in out and out[key] != value:
            raise ValueError("line %d: hardware id %d instance %d appears twice with different contents" % (n, key[0], key[1]))
        out[key] = value
    if not out:
        raise ValueError("no IP block lines found")
    return out


def name(hw):
    return dd.HW_IDS.get(hw, "hw%d" % hw)


def compare(mac, linux):
    """List of human readable differences; empty when the two tables agree."""
    diffs = []
    for key in sorted(set(mac) | set(linux)):
        label = "%s (hw_id %d) instance %d" % (name(key[0]), key[0], key[1])
        if key not in linux:
            diffs.append("only in the macOS read: %s v%d.%d.%d" % ((label,) + mac[key][0]))
        elif key not in mac:
            diffs.append("only in the Linux report: %s v%d.%d.%d" % ((label,) + linux[key][0]))
        else:
            (mv, mb), (lv, lb) = mac[key], linux[key]
            if mv != lv:
                diffs.append("%s: version %d.%d.%d on macOS, %d.%d.%d on Linux" % ((label,) + mv + lv))
            if mb != lb:
                diffs.append("%s: base addresses differ, macOS %s, Linux %s" % (label, " ".join("0x%x" % x for x in mb) or "none", " ".join("0x%x" % x for x in lb) or "none"))
    return diffs


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 1
    try:
        mac = parse_binary(dd.load(argv[1]))
        linux = parse_linux(open(argv[2], encoding="utf-8", errors="replace").read())
    except (OSError, ValueError, struct.error) as e:
        sys.stderr.write("cannot compare: %s\n" % e)
        return 1
    diffs = compare(mac, linux)
    if diffs:
        print("DIFFERENT: %d differences over %d blocks" % (len(diffs), len(set(mac) | set(linux))))
        for d in diffs:
            print("  " + d)
        return 1
    print("SAME: %d blocks, versions and base addresses agree" % len(mac))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
