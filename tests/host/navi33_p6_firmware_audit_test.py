# SPDX-License-Identifier: BSD-3-Clause
"""Tests use disposable fake files; never read or include licensed firmware."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import navi33_p6_firmware_verify as audit


class Navi33P6FirmwareAuditTests(unittest.TestCase):
    def test_repository_manifest_is_well_formed(self):
        entries = audit.read_manifest(audit._DEFAULT_MANIFEST)
        self.assertEqual(len(entries), 9)
        self.assertEqual(entries[0]["name"], "gc_11_0_2_pfp")
        self.assertEqual(entries[-1]["name"], "smu_13_0_7")
        self.assertEqual(len({x["name"] for x in entries}), 9)

    def test_uncompressed_bytes_and_sha256(self):
        content = b"synthetic test data only"
        entry = {
            "name": "fake_gc", "size": len(content),
            "sha256": hashlib.sha256(content).hexdigest(),
        }
        with tempfile.TemporaryDirectory() as t:
            directory = Path(t)
            self.assertEqual(audit.audit_one(entry, directory)["status"], "missing")
            (directory / "fake_gc.bin").write_bytes(content)
            self.assertEqual(audit.audit_one(entry, directory)["status"], "ok")
            entry["sha256"] = "0" * 64
            self.assertEqual(audit.audit_one(entry, directory)["status"], "wrong_sha256")
            entry["sha256"] = hashlib.sha256(content).hexdigest()
            entry["size"] -= 1
            self.assertEqual(audit.audit_one(entry, directory)["status"], "wrong_size")

    def test_manifest_rejects_inconsistent_count(self):
        with tempfile.TemporaryDirectory() as t:
            manifest = Path(t) / "bad.h"
            manifest.write_text(
                'constexpr unsigned kFileCount = 2u;\\n'
                'constexpr FileInfo kFiles[kFileCount] = {\\n'
                '{"one", 1u, "' + "a"*64 + '"},\\n'
                '};\\n', encoding="utf-8"
            )
            with self.assertRaises(ValueError):
                audit.read_manifest(manifest)


if __name__ == "__main__":
    unittest.main()
