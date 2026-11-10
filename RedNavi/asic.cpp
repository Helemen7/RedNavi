// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// The ASIC table and the IP discovery lookups it needs. No kernel dependencies: this file is also built by the
// host tests (tests/host/asic_test.cpp).
#include "asic.h"
#include "dcn_replay.h"
#include "linux_info_navi32.h"

// Navi 32. Verified on the RX 7800 XT (docs/TEST_MACHINES.md). The RX 7700 XT is the same die and reports the same
// PCI device ID; nobody has run it, so it is not told apart here (unverified).
static const struct rn_asic_board kNavi32Boards[] = {
    {0x747E, "AMD Radeon RX 7800 XT", 16384, kRNReplay, sizeof(kRNReplay) / sizeof(kRNReplay[0])},
    {0, nullptr, 0, nullptr, 0},
};

static const struct rn_asic kAsics[] = {
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

uint32_t rn_disc_find_ip(const uint8_t *b, uint32_t len, uint16_t hw_id, bool *base64) {
    if (!b) return 0;
    auto u16 = [&](uint32_t o) -> uint32_t { return o + 2 <= len ? static_cast<uint32_t>(b[o] | (b[o + 1] << 8)) : 0u; };

    if (len < 16 || u16(4) >= 2) return 0;               // binary header v2 has another table layout
    uint32_t ipOff = u16(12);                            // table_list[0].offset (IP_DISCOVERY)
    uint32_t numDies = u16(ipOff + 12);
    bool wide = u16(ipOff + 4) == 4 && (ipOff + 78 < len) && (b[ipOff + 78] & 1);
    if (numDies > 16) numDies = 16;

    for (uint32_t d = 0; d < numDies; d++) {
        uint32_t pos = u16(ipOff + 14 + d * 4 + 2);      // die_info[d].die_offset, absolute
        uint32_t numIps = u16(pos + 2);
        pos += 4;
        for (uint32_t i = 0; i < numIps && pos + 8 <= len; i++) {
            if (u16(pos) == hw_id && b[pos + 2] == 0) {
                if (base64) *base64 = wide;
                return pos;
            }
            pos += 8 + b[pos + 3] * (wide ? 8 : 4);
        }
    }
    return 0;
}

bool rn_disc_ip_version(const uint8_t *table, uint32_t len, uint16_t hw_id, struct rn_ip_version *out) {
    uint32_t pos = rn_disc_find_ip(table, len, hw_id, nullptr);
    if (!pos) return false;                              // an entry never sits at offset 0: the binary header is there
    out->major = table[pos + 4];
    out->minor = table[pos + 5];
    out->rev = table[pos + 6];
    return true;
}

static bool sameVersion(const uint8_t *table, uint32_t len, uint16_t hw_id, const struct rn_ip_version &want) {
    struct rn_ip_version v;
    return rn_disc_ip_version(table, len, hw_id, &v) && v.major == want.major && v.minor == want.minor && v.rev == want.rev;
}

const struct rn_asic *rn_asic_lookup(const uint8_t *table, uint32_t len, uint16_t pci_device, const struct rn_asic_board **board) {
    for (const struct rn_asic &a : kAsics) {
        if (!sameVersion(table, len, RN_HWID_GC, a.gc) || !sameVersion(table, len, RN_HWID_DMU, a.dcn) ||
            !sameVersion(table, len, RN_HWID_MP0, a.mp0) || !sameVersion(table, len, RN_HWID_MP1, a.mp1) ||
            !sameVersion(table, len, RN_HWID_SDMA0, a.sdma))
            continue;
        for (const struct rn_asic_board *bd = a.boards; bd->pci_device; bd++) {
            if (bd->pci_device != pci_device) continue;
            if (board) *board = bd;
            return &a;
        }
    }
    return nullptr;
}
