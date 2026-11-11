#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Host tests for tools/compare_discovery.py. Run from the repository root (`make test` does):
    python3 -I tests/host/compare_discovery_test.py
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "compare_discovery.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import compare_discovery as cd  # noqa: E402


def build(ips):
    """A discovery table (binary header v1, IP table v3, one die) of [(hw_id, inst, (major, minor, rev), [bases])]."""
    ip_off = 0x3c
    die_off = ip_off + 14 + 16 * 4
    body = bytearray()
    for hw, inst, (ma, mi, rev), bases in ips:
        body += struct.pack("<HBBBBBB", hw, inst, len(bases), ma, mi, rev, 0)
        for b in bases:
            body += struct.pack("<I", b)
    t = bytearray(die_off + 4)
    struct.pack_into("<IH", t, 0, cd.dd.BINARY_SIGNATURE, 1)
    struct.pack_into("<H", t, 12, ip_off)
    struct.pack_into("<IHHIH", t, ip_off, cd.dd.DISCOVERY_TABLE_SIGNATURE, 3, 0, 0, 1)
    struct.pack_into("<HH", t, ip_off + 14, 0, die_off)
    struct.pack_into("<HH", t, die_off, 0, len(ips))
    return bytes(t + body)


def linux_text(ips, alias=False):
    lines = ["# /sys/class/drm/card0/device vendor 0x1002 device 0x7480 revision 0xcf"]
    for hw, inst, (ma, mi, rev), bases in ips:
        for first in ([str(hw), cd.name(hw)] if alias else [str(hw)]):
            lines.append("%s inst %d hw_id %d num_instance %d version %d.%d.%d bases %s" % (
                first, inst, hw, inst, ma, mi, rev, " ".join("0x%08X" % b for b in bases)))
    return "\n".join(lines) + "\n"


IPS = [
    (11, 0, (11, 0, 2), [0x1260, 0xA000, 0x1C000, 0x2402C00]),
    (271, 0, (3, 2, 1), [0x12, 0xC0, 0x34C0, 0x9000, 0x2403C00]),
    (150, 0, (8, 11, 0), [0x14000, 0x2425800]),
    (150, 1, (8, 11, 0), [0x54000, 0x2425C00]),
    (10, 0, (9, 0, 0), []),                                  # a block without base addresses prints "bases " and nothing else
]


class Compare(unittest.TestCase):
    def test_same(self):
        self.assertEqual(cd.compare(cd.parse_binary(build(IPS)), cd.parse_linux(linux_text(IPS))), [])

    def test_sysfs_lists_a_block_under_its_number_and_its_name(self):
        self.assertEqual(cd.compare(cd.parse_binary(build(IPS)), cd.parse_linux(linux_text(IPS, alias=True))), [])

    def test_version(self):
        other = [(hw, i, (3, 1, 4) if hw == 271 else v, b) for hw, i, v, b in IPS]
        d = cd.compare(cd.parse_binary(build(IPS)), cd.parse_linux(linux_text(other)))
        self.assertEqual(len(d), 1)
        self.assertIn("DMU", d[0])
        self.assertIn("3.2.1 on macOS, 3.1.4 on Linux", d[0])

    def test_base_address(self):
        other = [(hw, i, v, [0x99] + b[1:] if hw == 11 else b) for hw, i, v, b in IPS]
        d = cd.compare(cd.parse_binary(build(IPS)), cd.parse_linux(linux_text(other)))
        self.assertEqual(len(d), 1)
        self.assertIn("base addresses differ", d[0])

    def test_missing_and_extra_blocks(self):
        mac = IPS[:3] + [(99, 0, (1, 0, 0), [0x10])]
        d = cd.compare(cd.parse_binary(build(mac)), cd.parse_linux(linux_text(IPS[:4])))
        self.assertEqual(len(d), 2)
        self.assertTrue(any(x.startswith("only in the Linux report: UMC (hw_id 150) instance 1") for x in d))
        self.assertTrue(any(x.startswith("only in the macOS read: hw99 (hw_id 99) instance 0") for x in d))

    def test_bad_input(self):
        with self.assertRaises(ValueError):
            cd.parse_linux("# nothing here\n")
        with self.assertRaises(ValueError):
            cd.parse_linux("this is not a block line\n")
        with self.assertRaises(ValueError):
            cd.parse_binary(b"\0" * 64)
        t = linux_text(IPS) + linux_text([(11, 0, (11, 0, 3), [1])]).splitlines()[1] + "\n"
        with self.assertRaises(ValueError):
            cd.parse_linux(t)                                  # the same block twice with other contents
        for n in range(len(build(IPS))):                       # truncated tables raise, they never read past the end silently
            try:
                cd.parse_binary(build(IPS)[:n])
            except (ValueError, struct.error):
                pass


class RealCapture(unittest.TestCase):
    """The RX 7600 (1002:7480) of docs/logs/navi33: the raw table read through sysfs and the text Linux printed for the same card."""
    raw = os.path.join(ROOT, "docs/logs/navi33/rx7600-frame121/amdgpu_discovery.bin")
    txt = os.path.join(ROOT, "docs/logs/navi33/ip_discovery.txt")

    @unittest.skipUnless(os.path.exists(raw) and os.path.exists(txt), "the Navi 33 capture is not in this checkout")
    def test_agree(self):
        r = subprocess.run([sys.executable, "-I", TOOL, self.raw, self.txt], capture_output=True, text=True)
        self.assertEqual((r.returncode, r.stdout.split(":")[0]), (0, "SAME"), r.stdout + r.stderr)

    @unittest.skipUnless(os.path.exists(raw) and os.path.exists(os.path.join(ROOT, "docs/logs/navi31/ip_discovery.txt")), "captures missing")
    def test_another_die_does_not(self):
        r = subprocess.run([sys.executable, "-I", TOOL, self.raw, os.path.join(ROOT, "docs/logs/navi31/ip_discovery.txt")], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn("13.0.7 on macOS, 13.0.0 on Linux", r.stdout)

    def test_unreadable_input_is_an_error(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt") as f:
            f.write("not a table\n")
            f.flush()
            r = subprocess.run([sys.executable, "-I", TOOL, f.name, f.name], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)


if __name__ == "__main__":
    unittest.main()
