#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Turn the GC 11.0.3 / SDMA 6.0.3 firmware set into a C header for local builds (git-ignored, firmware is not redistributed).

    python3 tools/gen_gc_fw_header.py <directory with the linux-firmware amdgpu files> RedNavi/gc_fw.h
    python3 tools/gen_gc_fw_header.py <directory> --list          # only print the manifest

It reproduces, in the order Linux sends them (the AMDGPU_UCODE_ID enum order), the images that amdgpu hands to the PSP with
GFX_CMD_ID_LOAD_IP_FW when the firmware load type is PSP, which is the default for GC 11: amdgpu_ucode_init_single_fw and
amdgpu_gfx_cp_init_microcode / amdgpu_gfx_rlc_init_microcode / imu_v11_0_init_microcode / mes_v11_0_init_microcode /
sdma_v6_0_init_microcode in the Linux kernel. Needs gc_11_0_3_{pfp,me,mec,rlc,imu,mes_2,mes1}.bin and sdma_6_0_3.bin.
"""
import struct
import sys

# GFX_FW_TYPE values (psp_gfx_if.h)
T = {"RLC_V": 7, "RLC_G": 8, "RLC_RESTORE_LIST_GPM_MEM": 20, "RLC_RESTORE_LIST_SRM_MEM": 21, "RLC_RESTORE_LIST_SRM_CNTL": 22,
     "RLC_P": 25, "RLC_IRAM": 26, "CP_MES": 33, "MES_STACK": 34, "RLC_DRAM_BOOT": 48, "IMU_I": 68, "IMU_D": 69,
     "SDMA_UCODE_TH0": 71, "SDMA_UCODE_TH1": 72, "CP_MES_KIQ": 81, "MES_KIQ_STACK": 82, "RS64_PFP": 87, "RS64_ME": 88,
     "RS64_MEC": 89, "RS64_PFP_P0_STACK": 90, "RS64_PFP_P1_STACK": 91, "RS64_ME_P0_STACK": 92, "RS64_ME_P1_STACK": 93,
     "RS64_MEC_P0_STACK": 94, "RS64_MEC_P1_STACK": 95, "RS64_MEC_P2_STACK": 96, "RS64_MEC_P3_STACK": 97}


def u32(d, off):
    return struct.unpack_from("<I", d, off)[0]


def manifest(fwdir):
    def rd(name):
        return open(f"{fwdir}/{name}", "rb").read()

    pfp, me, mec, rlc, imu = (rd(f"gc_11_0_3_{n}.bin") for n in ("pfp", "me", "mec", "rlc", "imu"))
    mes, mes1, sdma = rd("gc_11_0_3_mes_2.bin"), rd("gc_11_0_3_mes1.bin"), rd("sdma_6_0_3.bin")
    out = []

    def add(name, typ, data, off, size):
        if size:
            out.append((name, T[typ], data[off:off + size]))

    def array_off(d):
        return u32(d, 24)

    for d in (pfp, me, mec, rlc, imu, mes, mes1, sdma):
        if u32(d, 0) != len(d):
            sys.exit("firmware file with a wrong size field")

    # SDMA 6.0 uses header v2.0: context thread, then control thread
    if struct.unpack_from("<HH", sdma, 8) != (2, 0):
        sys.exit("unexpected SDMA firmware header version")
    add("SDMA_UCODE_TH0", "SDMA_UCODE_TH0", sdma, array_off(sdma), u32(sdma, 36))
    add("SDMA_UCODE_TH1", "SDMA_UCODE_TH1", sdma, u32(sdma, 48), u32(sdma, 52))

    # CP, RS64 (gfx_firmware_header_v2_0: ucode size at 36, data size at 44, data offset at 48)
    for d in (pfp, me, mec):
        if struct.unpack_from("<HH", d, 8) != (2, 0):
            sys.exit("CP firmware is not header v2.0 (RS64)")
    add("CP_RS64_PFP", "RS64_PFP", pfp, array_off(pfp), u32(pfp, 36))
    add("CP_RS64_ME", "RS64_ME", me, array_off(me), u32(me, 36))
    add("CP_RS64_MEC", "RS64_MEC", mec, array_off(mec), u32(mec, 36))
    for n in range(2):
        add(f"CP_RS64_PFP_P{n}_STACK", f"RS64_PFP_P{n}_STACK", pfp, u32(pfp, 48), u32(pfp, 44))
    for n in range(2):
        add(f"CP_RS64_ME_P{n}_STACK", f"RS64_ME_P{n}_STACK", me, u32(me, 48), u32(me, 44))
    for n in range(4):
        add(f"CP_RS64_MEC_P{n}_STACK", f"RS64_MEC_P{n}_STACK", mec, u32(mec, 48), u32(mec, 44))

    # MES: scheduler pipe (mes_2) then KIQ pipe (mes1); header v1.0: ucode size/offset at 36/40, data size/offset at 48/52
    add("CP_MES", "CP_MES", mes, u32(mes, 40), u32(mes, 36))
    add("CP_MES_DATA", "MES_STACK", mes, u32(mes, 52), u32(mes, 48))
    add("CP_MES1", "CP_MES_KIQ", mes1, u32(mes1, 40), u32(mes1, 36))
    add("CP_MES1_DATA", "MES_KIQ_STACK", mes1, u32(mes1, 52), u32(mes1, 48))

    # IMU (header v1.0: iram size at 32, dram size at 40; the dram image follows the iram image)
    add("IMU_I", "IMU_I", imu, array_off(imu), u32(imu, 32))
    add("IMU_D", "IMU_D", imu, array_off(imu) + u32(imu, 32), u32(imu, 40))

    # RLC, header v2.3
    major, minor = struct.unpack_from("<HH", rlc, 8)
    if major != 2 or minor not in (3,):
        sys.exit(f"RLC firmware header {major}.{minor} is not handled (only 2.3)")
    add("RLC_RESTORE_LIST_CNTL", "RLC_RESTORE_LIST_SRM_CNTL", rlc, u32(rlc, 120), u32(rlc, 116))
    add("RLC_RESTORE_LIST_GPM_MEM", "RLC_RESTORE_LIST_GPM_MEM", rlc, u32(rlc, 136), u32(rlc, 132))
    add("RLC_RESTORE_LIST_SRM_MEM", "RLC_RESTORE_LIST_SRM_MEM", rlc, u32(rlc, 152), u32(rlc, 148))
    add("RLC_IRAM", "RLC_IRAM", rlc, u32(rlc, 160), u32(rlc, 156))
    add("RLC_DRAM", "RLC_DRAM_BOOT", rlc, u32(rlc, 168), u32(rlc, 164))
    add("RLC_P", "RLC_P", rlc, u32(rlc, 184), u32(rlc, 180))
    add("RLC_V", "RLC_V", rlc, u32(rlc, 200), u32(rlc, 196))
    add("RLC_G", "RLC_G", rlc, array_off(rlc), u32(rlc, 20))      # common header: ucode_size_bytes at 20
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    items = manifest(sys.argv[1])
    for name, typ, data in items:
        print(f"{name:28s} type {typ:3d} {len(data):8d} bytes")
    print(f"{len(items)} images, {sum(len(d) for _, _, d in items)} bytes")
    if sys.argv[2] == "--list":
        return
    with open(sys.argv[2], "w") as f:
        f.write(f"// Generated by tools/gen_gc_fw_header.py. Do not commit.\n")
        f.write(f"#define kRNGcFwCount {len(items)}u\n")
        f.write("struct RNGcFw { const char *name; unsigned int type; unsigned int offset; unsigned int size; };\n")
        f.write("static const RNGcFw kRNGcFw[] = {\n")
        off = 0
        for name, typ, data in items:
            f.write(f'    {{"{name}", {typ}u, {off}u, {len(data)}u}},\n')
            off += len(data)
        f.write("};\n")
        # program counter start of the RS64 CP firmware (gfx_firmware_header_v2_0: ucode_start_addr_lo/hi at 52/56), for gfx11.cpp
        for n in ("pfp", "me", "mec"):
            d = open(f"{sys.argv[1]}/gc_11_0_3_{n}.bin", "rb").read()
            f.write(f"#define kRNGc{n.capitalize()}StartLo 0x{u32(d, 52):x}u\n#define kRNGc{n.capitalize()}StartHi 0x{u32(d, 56):x}u\n")
        # MES: program counter start of the two pipes (mes_firmware_header_v1_0: mes_uc_start_addr_lo/hi at 56/60), 64-bit, for gfx11.cpp
        for n, fn in (("0", "mes_2"), ("1", "mes1")):
            d = open(f"{sys.argv[1]}/gc_11_0_3_{fn}.bin", "rb").read()
            f.write(f"#define kRNMes{n}StartLo 0x{u32(d, 56):x}u\n#define kRNMes{n}StartHi 0x{u32(d, 60):x}u\n")
        f.write("static const unsigned char kRNGcFwBlob[] = {\n")
        blob = b"".join(d for _, _, d in items)
        for j in range(0, len(blob), 16):
            f.write("    " + ", ".join(f"0x{x:02x}" for x in blob[j:j + 16]) + ",\n")
        f.write("};\n")


if __name__ == "__main__":
    main()
