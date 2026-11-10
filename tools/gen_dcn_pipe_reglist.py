#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Build a curated, ordered register list for one DCN 3.2 display pipe: OTG/OPP/DPG/MPC 0, DIG/DP 4, clocks.

The full list of tools/dcn32_reglist.txt hangs the machine when read (on macOS and on Linux): some blocks are
power- or clock-gated. This list keeps only what a DisplayPort test pattern on path 0 touches and orders it in
groups, safest first, so a stall can be pinned to a group. Format: "group name base_idx dword_offset".

    python3 tools/gen_dcn_pipe_reglist.py dcn_3_2_0_offset.h
"""
import re
import sys

GROUPS = [
    ("hpd_aux4", r"HPD4_|DP_AUX4_|DC_GPIO_HPD"),
    ("dig4_dp4", r"DIG4_|DP4_"),
    ("clocks", r"DCCG|DPPCLK|DPREFCLK|DENTIST|DISPCLK|PHYPLL|DP_DTO|OTG_PIXEL_RATE_DIV|DTBCLK|SYMCLK|DSCCLK|DIO_|DMU_CLK|DC_MEM_GLOBAL"),
    ("otg0", r"OTG0_|OPTC_|ODM0_|VTG0_|CONTROL$"),
    ("opp0", r"OPPBUF0_|OPP_PIPE0_|OPP_|DPG0_|FMT0_|DPG_|OPP_PIPE_CONTROL"),
    ("mpc", r"MPC_OUT0_|MPC_|MPCC0_|MPC_CRC"),
    ("hubbub", r"DCHUBBUB_|DCHUB_"),
    ("dcio", r"DCIO_|RDPCSPIPE4_|DC_GPIO_|DC_REF_CLK|DC_PAD"),
    # Plane groups: pipe 0 only. Not in the first dump because a full read stalled; read them one group at a time.
    ("hubp0", r"HUBP0_|HUBPREQ0_|HUBPRET0_|SURFACE|DCHUBP0"),
    ("dpp0", r"CNVC_CFG0_|CNVC_CUR0_|DSCL0_|CM0_|DPP_TOP0_|CURSOR0_|DPP0_"),
    ("dcnvm", r"DCN_VM_|DCN_"),
    # A monitor on another DIG (an RX 7600 on HDMI-A-2 has SYMCLKB enabled, front end 1): the same two groups as for DIG4.
    # The AUX data FIFO registers (AUX_SW_DATA, AUX_LS_DATA) are left out: on an HDMI connector the AUX channel is unused and reading
    # DP_AUX1_AUX_SW_DATA made an RX 7600 stop answering on the bus (every later read was 0xffffffff, then it froze; issue #6).
    ("hpd_aux1", r"HPD1_|DP_AUX1_AUX_(?!SW_DATA|LS_DATA)"),
    ("dig1_hdmi", r"DIG1_|DP1_"),
] + [(f"hpd_aux{n}", rf"HPD{n}_|DP_AUX{n}_AUX_(?!SW_DATA|LS_DATA)") for n in (0, 2, 3, 5)] + [(f"dig{n}_dp{n}", rf"DIG{n}_|DP{n}_") for n in (0, 2, 3, 5)] + [
    # NOT safe: the HDMI encoder and the audio format blocks are gated when unused, and reading them froze an RX 7600 (issue #6).
    # Not part of any default, and tools/linux_collect.py never reads them.
    ("unsafe_hdmi_afmt", r"AFMT\d_|HDMI_|HDMICHARCLK|HDMISTREAMCLK"),
]
PLANE_GROUPS = ("hubp0", "dpp0")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    regs, base = {}, {}
    for line in open(sys.argv[1]):
        m = re.match(r"#define reg(\w+)_BASE_IDX\s+(\d+)", line)
        if m:
            base[m.group(1)] = int(m.group(2))
            continue
        m = re.match(r"#define reg(\w+)\s+(0x[0-9a-fA-F]+)", line)
        if m:
            regs[m.group(1)] = int(m.group(2), 16)
    seen, total = set(), 0
    with open("tools/dcn32_pipe_reglist.txt", "w") as f:
        for g, pat in GROUPS:
            rx = re.compile(pat)
            names = sorted((n for n in regs if rx.match(n) and n not in seen and base.get(n) is not None
                            and (g in PLANE_GROUPS or not n.startswith(("DMCUB", "AZ", "HUBP", "CM", "DSC", "DWB", "MCIF", "CURSOR", "CNVC")))),
                           key=lambda n: (base[n], regs[n]))
            for n in names:
                seen.add(n)
                f.write(f"{g} {n} {base[n]} 0x{regs[n]:x}\n")
            print(f"{g:<10} {len(names)}")
            total += len(names)
    print("total", total)


if __name__ == "__main__":
    main()
