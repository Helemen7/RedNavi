#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""List the display paths in a VBIOS image: connectors, encoders, DDC/AUX and HPD lines.

Reads the ATOM display object table (displayobjectinfo, v1.4 or v1.5) and the GPIO pin table, the way
dc/bios/bios_parser2.c does. Layouts follow drivers/gpu/drm/amd/include/atomfirmware.h.

    python3 tools/parse_vbios_objects.py ioreg.txt        # or a raw VBIOS image
"""
import struct
import sys

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from check_vbios import load  # noqa: E402

OBJ_TYPES = {1: "GPU", 2: "ENCODER", 3: "CONNECTOR", 4: "ROUTER", 5: "GENERIC"}
# Raw ATOM object ids (radeon ObjectID.h / amdgpu ObjectID.h).
ENCODERS = {0x1E: "UNIPHY", 0x1F: "KLDSCP_LVTMA", 0x20: "UNIPHY1", 0x21: "UNIPHY2", 0x22: "UNIPHY3"}
CONNECTORS = {1: "SINGLE_LINK_DVI_I", 2: "DUAL_LINK_DVI_I", 3: "SINGLE_LINK_DVI_D", 4: "DUAL_LINK_DVI_D",
              5: "VGA", 12: "HDMI_TYPE_A", 14: "LVDS", 19: "DISPLAY_PORT", 20: "EDP", 24: "USBC"}
RECORDS = {1: "I2C", 2: "HPD_INT", 3: "CONNECTOR_CAP", 4: "CONNECTOR_SPEED", 9: "GPIO_CNTL",
           16: "HPDPIN_LUT", 17: "AUXDDC_LUT", 20: "ENCODER_CAP", 21: "BRACKET_LAYOUT", 22: "FORCED_TMDS_CAP",
           23: "DISP_CONNECTOR_CAPS", 25: "BRACKET_LAYOUT_V2"}
DEVICE_TAGS = {0x0002: "LCD1", 0x0020: "LCD2", 0x0008: "DFP1", 0x0080: "DFP2", 0x0200: "DFP3",
               0x0400: "DFP4", 0x0800: "DFP5", 0x0040: "DFP6"}


def u16(b, o): return struct.unpack_from("<H", b, o)[0]


def obj(oid):
    t, e, i = (oid >> 12) & 7, (oid >> 8) & 15, oid & 0xFF
    name = ENCODERS.get(i) if t == 2 else CONNECTORS.get(i) if t == 3 else None
    return f"{OBJ_TYPES.get(t, t)} {name or hex(i)} (enum {e}, id 0x{oid:04x})"


def records(b, off):
    out = []
    while off + 2 <= len(b):
        rt, rs = b[off], b[off + 1]
        if rt == 0xFF or rs < 2:
            break
        body = b[off + 2:off + rs]
        name = RECORDS.get(rt, f"type{rt}")
        if rt == 1 and len(body) >= 2:
            out.append(f"I2C/DDC line id 0x{body[0]:02x}, slave 0x{body[1]:02x}")
        elif rt == 2 and len(body) >= 2:
            out.append(f"HPD pin id {body[0]}, plug-in state {body[1]}")
        elif rt == 17:
            out.append("AUX/DDC LUT " + " ".join(f"{x:02x}" for x in body[:8]))
        elif rt == 16:
            out.append("HPD pin LUT " + " ".join(f"{x:02x}" for x in body[:8]))
        elif rt == 3 and len(body) >= 2:
            out.append(f"connector caps 0x{u16(body, 0):04x}")
        elif rt == 20 and len(body) >= 4:
            caps = struct.unpack_from("<I", body, 0)[0]
            names = [n for bit, n in ((1, "MST"), (2, "HBR2"), (4, "HDMI 6G"), (8, "HBR3"), (0x10, "DP2"),
                                      (0x20, "UHBR10"), (0x40, "UHBR13.5"), (0x80, "UHBR20"), (0x100, "USB-C")) if caps & bit]
            out.append(f"encoder caps 0x{caps:x} [{', '.join(names)}]")
        elif rt == 4 and len(body) >= 4:
            out.append(f"connector max speed {struct.unpack_from('<I', body, 0)[0]} MHz")
        else:
            out.append(f"{name} {' '.join(f'{x:02x}' for x in body)}")
        off += rs
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    b = load(sys.argv[1])
    rom = u16(b, 0x48)
    if b[rom + 4:rom + 8] != b"ATOM":
        sys.exit("no ATOM ROM header")
    master = u16(b, rom + 32)
    lst = master + 4
    names = ["utilitypipeline", "multimedia_info", "smc_dpm_info", "sw3", "firmwareinfo", "sw5", "lcd_info", "sw7",
             "smu_info", "sw9", "sw10", "vram_usagebyfirmware", "gpio_pin_lut", "sw13", "gfx_info", "powerplayinfo",
             "sw16", "sw17", "sw18", "sw19", "sw20", "sw21", "displayobjectinfo", "indirectioaccess", "umc_info"]
    tables = {n: u16(b, lst + 2 * i) for i, n in enumerate(names)}
    print("data tables:", {k: hex(v) for k, v in tables.items() if k in ("displayobjectinfo", "gpio_pin_lut", "dce_info")})

    gp = tables["gpio_pin_lut"]
    if gp:
        size, fmt, cont = struct.unpack_from("<HBB", b, gp)
        print(f"\nGPIO pin table v{fmt}.{cont}, {(size - 4) // 12} entries (I2C pairs have bit 7 of gpio_id set)")
        for i in range((size - 4) // 12):
            reg, shift, mshift, gid, _ = struct.unpack_from("<IBBBB", b, gp + 4 + 12 * i)
            kind = f"I2C engine {(gid >> 4) & 7} lane mux {gid & 15}" if gid & 0x80 else "GPIO"
            print(f"  gpio_id 0x{gid:02x}  {kind:<28} reg 0x{reg:x} shift {shift}")

    do = tables["displayobjectinfo"]
    size, fmt, cont = struct.unpack_from("<HBB", b, do)
    devs, npath = struct.unpack_from("<HB", b, do + 4)
    print(f"\ndisplay object table v{fmt}.{cont}: {npath} paths, supported devices 0x{devs:04x}")
    for i in range(npath):
        p = do + 8 + 16 * i
        if cont >= 5:
            oid, rec, enc, _, _, _, tag, _ = struct.unpack_from("<8H", b, p)
            ext = 0
        else:
            oid, rec, enc, ext, erec, xrec, tag = struct.unpack_from("<6HH", b, p)
        print(f"\npath {i}: device {DEVICE_TAGS.get(tag, hex(tag))} (tag 0x{tag:04x})")
        print(f"  connector: {obj(oid)}")
        print(f"  encoder:   {obj(enc)}")
        if ext:
            print(f"  ext enc:   {obj(ext)}")
        if rec:
            for line in records(b, do + rec):
                print(f"    record: {line}")


if __name__ == "__main__":
    main()
