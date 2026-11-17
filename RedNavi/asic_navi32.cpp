// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Navi 32 profile data. Keep ASIC-specific board, replay, DRM snapshot, firmware
// and register provenance out of the generic discovery/lookup implementation.
#include "asic.h"
#include "dcn_replay.h"
#include "dcn_replay_navi32_747e_7700xt.h"
#include "linux_info_navi32.h"

// Verified on the RX 7800 XT (docs/TEST_MACHINES.md). The RX 7700 XT is the same die and reports the same PCI device ID; it has 12 GB, which tells the two
// apart (rn_asic_lookup). Nobody has run the kext on it: its row is data read from a Linux capture (docs/logs/navi32/rx7700xt-gigabyte), write_ok is 0, and the probe stays read-only.
static const struct rn_asic_board kNavi32Boards[] = {
    // VBIOS path: DisplayPort on UNIPHY2 (transmitter E, phy id 4), HPD pin 5, AUX 4, 4 lanes.
    {0x747E, "AMD Radeon RX 7800 XT", 16384, kRNReplay, sizeof(kRNReplay) / sizeof(kRNReplay[0]), {4, 4, 5, 19, 4, 4}, 15872, 1},
    // Nominal 12 GB (the Linux log says 12272M, the register 16 MB under the nominal size like on the 7800 XT). The TMR offset is from the Linux log of the board ("reserve 0xa700000 from 0x82e0000000"),
    // not run on macOS. The replay table is the test pattern part (groups 1 to 9) of the pipe capture, DIG front end 0 / SYMCLKA, made with tools/gen_dcn_replay.py --dig 0 --max-group 9: the capture has
    // no plane registers (so no scanout groups) and was taken at 3840x2160 with a custom EDID. The DisplayPort path is known only as far as the DIG (0); AUX, PHY id and HPD pin are not in the capture.
    {0x747E, "AMD Radeon RX 7700 XT", 12288, kRNReplay_navi32_747e_7700xt, sizeof(kRNReplay_navi32_747e_7700xt) / sizeof(kRNReplay_navi32_747e_7700xt[0]), {0, 0, 0, 0, 0, 0}, 11776, 0},
    {0, nullptr, 0, nullptr, 0, {0, 0, 0, 0, 0, 0}, 0, 0},
};

extern const struct rn_asic kRNAsicNavi32 = {
        "Navi 32",
        {11, 0, 3}, {3, 2, 0}, {13, 0, 10}, {13, 0, 10}, {6, 0, 3},
        kNavi32Boards,
        &kRNLinuxInfo_navi32,
        "psp_13_0_10_sos.bin smu_13_0_10.bin dcn_3_2_0_dmcub.bin gc_11_0_3_pfp.bin gc_11_0_3_me.bin gc_11_0_3_mec.bin "
        "gc_11_0_3_rlc.bin gc_11_0_3_imu.bin gc_11_0_3_mes_2.bin gc_11_0_3_mes1.bin sdma_6_0_3.bin",
        "gc_regs.h (gc_11_0_3) mmhub_regs.h (mmhub_3_0_0) osssys_regs.h (osssys_6_0_0) nbio_regs.h (nbio_4_3_0) dcn_reglist.h (dcn_3_2_0)",
        "linux_info_navi32.h",
        "dcn_replay.h (docs/logs/linux-reference/dcn_pipe_linux.txt, 1920x1080 at 60 Hz on DP)",
};
