// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// The ASIC table and the IP discovery lookups it needs. No kernel dependencies:
// this file is also built by the host tests (tests/host/asic_test.cpp).
#include "asic.h"

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
    for (uint32_t i = 0; i < kRNAsicCount; ++i) {
        const struct rn_asic &a = kRNAsics[i];
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
