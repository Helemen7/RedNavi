#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Dry run of the DMUB framebuffer layout that amdgpu builds for DCN 3.2 (dmub_srv_calc_region_info).

Reads the DMCUB firmware (dcn_3_2_0_dmcub.bin), finds the firmware meta block and prints each
window with its offset and size. Nothing here touches hardware.

    python3 tools/dmub_layout.py dcn_3_2_0_dmcub.bin 57856     # second argument: VBIOS size in bytes

Constants come from drivers/gpu/drm/amd/display/dmub (dmub_srv.c, dmub_cmd.h); the struct sizes
(shared state header, FAMS2 config, cursor offload) were obtained by compiling dmub_cmd.h.
"""
import struct
import sys

PSP_HEADER = 0x100
PSP_FOOTER = 0x100
META_MAGIC = 0x444D5542
STACK = 128 * 1024
CONTEXT = 512 * 1024
RB_SIZE = 8192                  # 64 byte commands * 128 entries
MAILBOX = 2 * RB_SIZE
SCRATCH = 1024
IB_MEM = 3148                   # sizeof(struct dmub_fams2_config_v2)
SHARED_STATE_HDR = 1536         # (DMUB_SHARED_STATE_FEATURE__LAST + 1) * 256
LSDMA = 64 * 1024
CURSOR_OFFLOAD = 14232          # sizeof(struct dmub_cursor_offload_v1)
NAMES = ["INST_CONST", "STACK", "BSS_DATA", "VBIOS", "MAILBOX", "TRACEBUFF", "FW_STATE",
         "SCRATCH_MEM", "IB_MEM", "SHARED_STATE", "LSDMA_BUFFER", "CURSOR_OFFLOAD"]


def align(v, f):
    return (v + f - 1) // f * f


def find_meta(inst):
    """Mirror dmub_srv_get_fw_meta_info_from_raw_fw for a combined region (no bss data): try
    footer sizes of 256 then 512 bytes, and a 0 to 15 byte alignment gap before the footer."""
    for footer in (PSP_FOOTER, 2 * PSP_FOOTER):
        size = len(inst) - footer
        for i in range(16):
            end = size - i
            start = end - 64
            if start < 0:
                continue
            magic, fw_region, trace, version = struct.unpack_from("<IIII", inst, start)
            if magic == META_MAGIC:
                shared, = struct.unpack_from("<I", inst, start + 20)
                return dict(fw_region_size=fw_region, trace_buffer_size=trace, fw_version=version,
                            shared_state_size=shared, inst_size=size, footer=footer)
    return None


def layout(fw, vbios):
    """Return (meta, inst_bytes, windows, total) for a DMCUB firmware image and a VBIOS size.

    windows is a list of (name, base, size) with size already rounded to 64 bytes."""
    uoff, = struct.unpack_from("<I", fw, 24)
    inst_bytes, bss_bytes = struct.unpack_from("<II", fw, 32)
    inst_all = fw[uoff + PSP_HEADER:uoff + inst_bytes]
    meta = find_meta(inst_all)
    if not meta:
        raise ValueError("firmware meta block not found")
    sizes = [meta["inst_size"], STACK + CONTEXT, bss_bytes, vbios, MAILBOX, meta["trace_buffer_size"],
             meta["fw_region_size"], align(SCRATCH, 64), align(IB_MEM, 64),
             max(SHARED_STATE_HDR, meta["shared_state_size"]), LSDMA, align(CURSOR_OFFLOAD, 64)]
    windows = []
    top = 0
    for name, size in zip(NAMES, sizes):
        base = align(top, 256)
        end = base + align(size, 64)
        windows.append((name, base, end - base))
        top = end
    return meta, inst_all[:meta["inst_size"]], windows, align(top, 4096)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    fw = open(sys.argv[1], "rb").read()
    meta, _inst, windows, total = layout(fw, int(sys.argv[2]))
    print(f"firmware version 0x{meta['fw_version']:08x}, inst_const {meta['inst_size']} bytes "
          f"(footer {meta['footer']}), fw_region {meta['fw_region_size']}, trace {meta['trace_buffer_size']}, "
          f"shared state {meta['shared_state_size']}")
    print(f"{'window':<16}{'offset':>10}{'size':>10}")
    for name, base, size in windows:
        print(f"{name:<16}{base:>#10x}{size:>10}")
    print(f"total {total} bytes ({total / 1024:.0f} KiB)")


if __name__ == "__main__":
    main()
