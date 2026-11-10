#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""List the components inside an AMD PSP firmware file (psp_*_sos.bin, header v2.0).

Layout follows psp_firmware_header_v2_0 in drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h:
a 32-byte common header, a component count, then {type, version, offset, size} entries.
Offsets are relative to ucode_array_offset_bytes.

    python3 tools/parse_psp_fw.py psp_13_0_10_sos.bin
"""
import struct
import sys

NAMES = ["UNKNOWN", "SOS", "SYS_DRV", "KDB", "TOC", "SPL", "RL", "SOC_DRV", "INTF_DRV",
         "DBG_DRV", "RAS_DRV", "IPKEYMGR_DRV", "SPDM_DRV"]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    b = open(sys.argv[1], "rb").read()
    size, hsz, hmaj, hmin, imaj, imin, uver, usz, uoff, _crc = struct.unpack_from("<IIHHHHIIII", b, 0)
    print(f"size {size}, header v{hmaj}.{hmin}, IP {imaj}.{imin}, ucode version 0x{uver:x}, array at {uoff}")
    if hmaj != 2:
        sys.exit("only header v2.x is handled")
    count, = struct.unpack_from("<I", b, 32)
    for i in range(count):
        t, v, off, sz = struct.unpack_from("<IIII", b, 36 + 16 * i)
        name = NAMES[t] if t < len(NAMES) else f"type{t}"
        print(f"  {name:<13} version 0x{v:x}  file offset {uoff + off:>7}  size {sz}")


if __name__ == "__main__":
    main()
