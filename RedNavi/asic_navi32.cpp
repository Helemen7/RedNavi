// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Navi 32 profile data. Keep ASIC-specific board, replay, DRM snapshot, firmware
// and register provenance out of the generic discovery/lookup implementation.
#include "asic.h"
#include "dcn_replay.h"
#include "linux_info_navi32.h"

// Verified on the RX 7800 XT (docs/TEST_MACHINES.md). The RX 7700 XT is the same
// die and reports the same PCI device ID; nobody has run it, so it is unverified.
static const struct rn_asic_board kNavi32Boards[] = {
    // VBIOS path: DisplayPort on UNIPHY2 (transmitter E, phy id 4), HPD pin 5, AUX 4, 4 lanes.
    {0x747E, "AMD Radeon RX 7800 XT", 16384, kRNReplay, sizeof(kRNReplay) / sizeof(kRNReplay[0]), {4, 4, 5, 19, 4}},
    {0, nullptr, 0, nullptr, 0, {0, 0, 0, 0, 0}},
};

extern const struct rn_asic kRNAsics[] = {
    {
        "Navi 32",
        {11, 0, 3}, {3, 2, 0}, {13, 0, 10}, {13, 0, 10}, {6, 0, 3},
        kNavi32Boards,
        &kRNLinuxInfo_navi32,
        "psp_13_0_10_sos.bin smu_13_0_10.bin dcn_3_2_0_dmcub.bin gc_11_0_3_pfp.bin gc_11_0_3_me.bin gc_11_0_3_mec.bin "
        "gc_11_0_3_rlc.bin gc_11_0_3_imu.bin gc_11_0_3_mes_2.bin gc_11_0_3_mes1.bin sdma_6_0_3.bin",
        "gc_regs.h (gc_11_0_3) mmhub_regs.h (mmhub_3_0_0) osssys_regs.h (osssys_6_0_0) nbio_regs.h (nbio_4_3_0) dcn_reglist.h (dcn_3_2_0)",
        "linux_info_navi32.h",
        "dcn_replay.h (docs/logs/linux-reference/dcn_pipe_linux.txt, 1920x1080 at 60 Hz on DP)",
    },
};

extern const uint32_t kRNAsicCount = sizeof(kRNAsics) / sizeof(kRNAsics[0]);
