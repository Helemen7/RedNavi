#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Regression tests for v1 and v2 IP Discovery table directories."""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "decode_discovery.py"


def build(version_major, table_count):
    """Build a minimal discovery header with an empty IP table."""
    table_base = 12 if version_major == 1 else 16
    ip_offset = table_base + table_count * 8
    data = bytearray(ip_offset + 14)
    struct.pack_into("<IHHHH", data, 0, 0x28211407, version_major, 3, 0, len(data))
    if version_major >= 2:
        struct.pack_into("<HH", data, 12, table_count, 0)
    for index in range(table_count):
        struct.pack_into(
            "<HHHH", data, table_base + index * 8,
            ip_offset if index == 0 else 0, 0, 14 if index == 0 else 0, 0,
        )
    struct.pack_into("<IHHIH", data, ip_offset, 0x53445049, 3, 14, 0, 0)
    struct.pack_into("<H", data, 8, sum(data[10:]) & 0xFFFF)
    return bytes(data)


class DiscoveryDirectoryTest(unittest.TestCase):
    def decode(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.bin"
            path.write_bytes(data)
            return subprocess.run(
                [sys.executable, str(TOOL), str(path)],
                capture_output=True, text=True, check=True,
            ).stdout

    def test_v1_has_six_table_descriptors(self):
        output = self.decode(build(1, 6))
        self.assertIn("table 5 NPS_INFO:", output)
        self.assertNotIn("table 6", output)

    def test_v2_keeps_seventh_table_name(self):
        output = self.decode(build(2, 7))
        self.assertIn("table 5 NPS_INFO:", output)
        self.assertIn("table 6 MEM_RESERVED_INFO:", output)


if __name__ == "__main__":
    unittest.main()
