#!/usr/bin/env python3
"""P6.1 RX7600 firmware audit: compare *uncompressed* files with P1 manifest.

Read-only host utility. It does not download, install, bundle, submit or
redistribute AMD firmware. A matching hash is a necessary but not sufficient
condition for safe PSP submission; it grants no hardware ownership.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import lzma
from pathlib import Path
import re
import subprocess
import sys

_ROOT = Path(__file__).resolve().parents[1]
_DEFAULT_MANIFEST = _ROOT / "RedNavi" / "navi33_psp_plan.h"
_RECORD = re.compile(r'^\s*\{"([a-zA-Z0-9_]+)",\s*(\d+)u,\s*"([a-f0-9]{64})"\},?\s*$', re.M)


def read_manifest(path: Path) -> list[dict]:
    source = path.read_text(encoding="utf-8")
    header = re.search(r"constexpr unsigned kFileCount = (\d+)u", source)
    if header is None:
        raise ValueError("P1 manifest missing kFileCount")
    start = source.find("constexpr FileInfo kFiles[kFileCount] = {")
    if start < 0:
        raise ValueError("P1 manifest missing kFiles")
    stop = source.find("};", start)
    if stop < 0:
        raise ValueError("P1 manifest unterminated")
    records = [
        {"name": m.group(1), "size": int(m.group(2)), "sha256": m.group(3)}
        for m in _RECORD.finditer(source[start:stop])
    ]
    if len(records) != int(header.group(1)):
        raise ValueError("P1 firmware count or syntax does not match manifest")
    if len({x["name"] for x in records}) != len(records):
        raise ValueError("duplicate firmware name")
    return records


def read_firmware(directory: Path, name: str) -> tuple[Path, bytes]:
    for extension in (".bin", ".bin.zst", ".bin.xz"):
        path = directory / (name + extension)
        if not path.is_file():
            continue
        if extension == ".bin":
            return path, path.read_bytes()
        if extension == ".bin.xz":
            return path, lzma.decompress(path.read_bytes())
        # Ubuntu's linux-firmware may store compressed .bin.zst images.
        try:
            out = subprocess.run(
                ["zstd", "-q", "-d", "-c", str(path)],
                capture_output=True, check=True
            )
        except FileNotFoundError as exc:
            raise RuntimeError("install zstd: sudo apt install zstd") from exc
        except subprocess.CalledProcessError as exc:
            raise RuntimeError("zstd decompression failed: " +
                               exc.stderr.decode("utf-8", errors="replace").strip()) from exc
        return path, out.stdout
    raise FileNotFoundError(f"{name}.bin[.zst|.xz] not present in {directory}")


def audit_one(entry: dict, directory: Path) -> dict:
    result = {
        "name": entry["name"], "status": "missing",
        "expected_size": entry["size"], "expected_sha256": entry["sha256"],
    }
    try:
        path, data = read_firmware(directory, entry["name"])
        result["path"] = str(path)
        result["actual_size"] = len(data)
        result["actual_sha256"] = hashlib.sha256(data).hexdigest()
        if len(data) != entry["size"]:
            result["status"] = "wrong_size"
        elif result["actual_sha256"] != entry["sha256"]:
            result["status"] = "wrong_sha256"
        else:
            result["status"] = "ok"
    except FileNotFoundError as exc:
        result["error"] = str(exc)
    except (OSError, RuntimeError, lzma.LZMAError) as exc:
        result["status"] = "unreadable"
        result["error"] = str(exc)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware-dir", type=Path, default=Path("/lib/firmware/amdgpu"),
                        help="directory of official AMD firmware .bin[.zst|.xz] files")
    parser.add_argument("--manifest", type=Path, default=_DEFAULT_MANIFEST,
                        help="P1 reference header (defaults to RedNavi/navi33_psp_plan.h)")
    parser.add_argument("--json", action="store_true", help="emit JSON only")
    args = parser.parse_args(argv)
    try:
        entries = read_manifest(args.manifest)
        results = [audit_one(entry, args.firmware_dir) for entry in entries]
    except (OSError, ValueError) as exc:
        print(f"firmware audit: {exc}", file=sys.stderr)
        return 2
    all_match = all(r["status"] == "ok" for r in results)
    if args.json:
        print(json.dumps({"all_match": all_match, "results": results}, indent=2))
    else:
        for r in results:
            print(f"{r['status']:12} {r['name']} " +
                  (f"({r['error']})" if "error" in r else
                   f"(bytes {r['actual_size']}/{r['expected_size']})"))
        print("PASS: all P1 firmware hashes match" if all_match else
              "FAIL: firmware missing or different; do not use for P6 PSP loading")
    return 0 if all_match else 1


if __name__ == "__main__":
    raise SystemExit(main())
