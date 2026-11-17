// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Navi 33 profile data (RX 7600, GC 11.0.2). The boards are known from Linux captures only (docs/logs/navi33/): nobody has run the kext on one, and the register
// offsets, firmware and layout constants compiled into the kext are those of Navi 32. So every board here has write_ok 0 and the probe stays read-only on it
// until the constants of this die are behind the row (docs/ADDING_A_GPU.md, issue #7).
#include "asic.h"
#include "linux_info_navi33.h"

static const struct rn_asic_board kNavi33Boards[] = {
    // Gigabyte RX 7600: the Linux log says 8176M of VRAM and "reserve 0x1300000 from 0x81fc000000 for PSP TMR", so the TMR is at 0x1fc000000 = 8128 MB.
    // No display replay yet: the capture lights DIG front end 0 (SYMCLKA), the Navi 32 table drives DIG4, and the DCN version is 3.2.1, not the 3.2.0 of that table.
    {0x7480, "AMD Radeon RX 7600", 8192, nullptr, 0, {0, 0, 0, 0, 0, 0}, 8128, 0},
    {0, nullptr, 0, nullptr, 0, {0, 0, 0, 0, 0, 0}, 0, 0},
};

extern const struct rn_asic kRNAsicNavi33 = {
        "Navi 33",
        {11, 0, 2}, {3, 2, 1}, {13, 0, 7}, {13, 0, 7}, {6, 0, 2},
        kNavi33Boards,
        &kRNLinuxInfo_navi33,
        "none selected yet (Linux uses psp_13_0_7_sos.bin smu_13_0_7.bin dcn_3_2_1_dmcub.bin gc_11_0_2_*.bin sdma_6_0_2.bin)",
        "none yet (gc_11_0_2, mmhub, osssys, nbio and dcn_3_2_1 headers are to be generated)",
        "linux_info_navi33.h",
        "none yet (docs/logs/navi33/rx7600-gigabyte/dcn_pipe.txt is the capture a replay table would be made from)",
};
