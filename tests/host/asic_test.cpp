// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Host tests for RedNavi/asic.cpp. Run with `make test`.
// With a file argument (an IP discovery table as raw bytes) it prints what the lookup makes of it instead:
//   build/asic_test table.bin [pci device ID in hex]
#include "../../RedNavi/asic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

struct Ip { uint16_t hwId; uint8_t instance, major, minor, rev; std::vector<uint32_t> bases; };

static void put16(std::vector<uint8_t> &t, size_t o, uint32_t v) { t[o] = v & 0xFF; t[o + 1] = (v >> 8) & 0xFF; }

// A discovery table in the layout of Linux include/discovery.h: binary header v1, IP table v3 (32-bit bases), one die.
static std::vector<uint8_t> build(const std::vector<Ip> &ips) {
    const uint32_t ipOff = 0x3c, dieOff = ipOff + 14 + 16 * 4;
    std::vector<uint8_t> t(dieOff + 4, 0);
    put16(t, 4, 1);              // binary header version_major
    put16(t, 12, ipOff);         // table_list[0].offset
    put16(t, ipOff + 4, 3);      // ip_discovery_header.version
    put16(t, ipOff + 12, 1);     // num_dies
    put16(t, ipOff + 14 + 2, dieOff);
    put16(t, dieOff + 2, static_cast<uint32_t>(ips.size()));
    for (const Ip &ip : ips) {
        size_t o = t.size();
        t.resize(o + 8 + ip.bases.size() * 4, 0);
        put16(t, o, ip.hwId);
        t[o + 2] = ip.instance;
        t[o + 3] = static_cast<uint8_t>(ip.bases.size());
        t[o + 4] = ip.major; t[o + 5] = ip.minor; t[o + 6] = ip.rev;
        for (size_t i = 0; i < ip.bases.size(); i++) { put16(t, o + 8 + i * 4, ip.bases[i] & 0xFFFF); put16(t, o + 10 + i * 4, ip.bases[i] >> 16); }
    }
    return t;
}

// The five blocks of the RX 7800 XT, with the versions and bases of docs/logs/ioreg-rednavi-747e.txt, between blocks the lookup must skip.
static std::vector<Ip> navi32() {
    return {
        {22, 0, 13, 0, 0, {0x16c00, 0x2401800}},                                       // CLKA
        {RN_HWID_DMU, 0, 3, 2, 0, {0x12, 0xc0, 0x34c0, 0x9000, 0x2403c00}},
        {RN_HWID_GC, 1, 9, 9, 9, {0x1}},                                               // not instance 0: skipped
        {RN_HWID_GC, 0, 11, 0, 3, {0x1260, 0xa000, 0x1c000, 0x2402c00}},
        {RN_HWID_MP0, 0, 13, 0, 10, {0x16000, 0xdc0000, 0xe00000, 0xe40000, 0x243fc00}},
        {RN_HWID_MP1, 0, 13, 0, 10, {0x16000, 0xdc0000, 0xe00000, 0xe40000, 0x243fc00}},
        {RN_HWID_SDMA0, 0, 6, 0, 3, {0x1260, 0xa000, 0x1c000, 0x2402c00}},
    };
}

static void testVersions() {
    std::vector<uint8_t> t = build(navi32());
    uint32_t len = static_cast<uint32_t>(t.size());
    rn_ip_version v = {};
    CHECK(rn_disc_ip_version(t.data(), len, RN_HWID_GC, &v) && v.major == 11 && v.minor == 0 && v.rev == 3);
    CHECK(rn_disc_ip_version(t.data(), len, RN_HWID_DMU, &v) && v.major == 3 && v.minor == 2 && v.rev == 0);
    CHECK(rn_disc_ip_version(t.data(), len, RN_HWID_SDMA0, &v) && v.major == 6 && v.minor == 0 && v.rev == 3);
    CHECK(!rn_disc_ip_version(t.data(), len, 12, &v));                    // no VCN in this table
    bool wide = true;
    uint32_t pos = rn_disc_find_ip(t.data(), len, RN_HWID_GC, &wide);
    CHECK(pos && !wide && t[pos + 3] == 4 && t[pos + 8] == 0x60 && t[pos + 9] == 0x12);
}

static void testLookup() {
    std::vector<uint8_t> t = build(navi32());
    uint32_t len = static_cast<uint32_t>(t.size());
    const rn_asic_board *board = nullptr;
    const rn_asic *a = rn_asic_lookup(t.data(), len, 0x747E, &board);
    CHECK(a && !strcmp(a->name, "Navi 32") && board && board->vram_mb == 16384 && !strcmp(board->model, "AMD Radeon RX 7800 XT"));
    CHECK(!rn_asic_lookup(t.data(), len, 0x7480, nullptr));              // right versions, a board nobody has run
    // The data behind the row: the DRM_AMDGPU_INFO answers start with the device ID, and the board has its display registers.
    CHECK(a && a->drm_info && a->drm_info->dev_info_size == 448 && a->drm_info->dev_info[0] == 0x7e && a->drm_info->dev_info[1] == 0x74);
    CHECK(a && a->drm_info->fw_count == 16 && a->drm_info->max_ibs_count == 10 && a->drm_info->hw_ip_info[0][0] == 0x0b);
    CHECK(board && board->replay && board->replay_count > 100);
    CHECK(board && board->dp.aux == 4 && board->dp.phy_id == 4 && board->dp.hpd_sel == 5 && board->dp.connector_id == 19 && board->dp.lanes == 4);

    std::vector<Ip> other = navi32();
    other[3].rev = 2;                                                     // GC 11.0.2 is Navi 33
    t = build(other);
    CHECK(!rn_asic_lookup(t.data(), static_cast<uint32_t>(t.size()), 0x747E, nullptr));

    other = navi32();
    other.pop_back();                                                     // a table without SDMA
    t = build(other);
    CHECK(!rn_asic_lookup(t.data(), static_cast<uint32_t>(t.size()), 0x747E, nullptr));
}

static void testBad() {
    std::vector<uint8_t> t = build(navi32());
    rn_ip_version v;
    CHECK(!rn_disc_ip_version(nullptr, 0, RN_HWID_GC, &v));
    for (uint32_t len = 0; len < t.size(); len++) rn_asic_lookup(t.data(), len, 0x747E, nullptr);   // truncated tables must not read past the end (run under ASan to see it)
    CHECK(!rn_asic_lookup(t.data(), 0x3c, 0x747E, nullptr));
    put16(t, 4, 2);                                                       // binary header v2: another layout, not parsed
    CHECK(!rn_asic_lookup(t.data(), static_cast<uint32_t>(t.size()), 0x747E, nullptr));
}

// A table captured from a real card of another die (docs/logs/navi33/rx7600-frame121, an RX 7600 on Linux; run from the repository root, as `make test` does).
// It must read as GC 11.0.2 / DCN 3.2.1 / MP0 and MP1 13.0.7 / SDMA 6.0.2 and match no row, whatever the device ID is: such a card stays read-only.
static void testRealNavi33() {
    FILE *f = fopen("docs/logs/navi33/rx7600-frame121/amdgpu_discovery.bin", "rb");
    if (!f) { printf("skipped: the Navi 33 capture is not in this checkout\n"); return; }
    std::vector<uint8_t> t(1 << 16);
    t.resize(fread(t.data(), 1, t.size(), f));
    fclose(f);
    uint32_t len = static_cast<uint32_t>(t.size());
    rn_ip_version v = {};
    CHECK(rn_disc_ip_version(t.data(), len, RN_HWID_GC, &v) && v.major == 11 && v.minor == 0 && v.rev == 2);
    CHECK(rn_disc_ip_version(t.data(), len, RN_HWID_DMU, &v) && v.major == 3 && v.minor == 2 && v.rev == 1);
    CHECK(rn_disc_ip_version(t.data(), len, RN_HWID_MP0, &v) && v.major == 13 && v.minor == 0 && v.rev == 7);
    CHECK(rn_disc_ip_version(t.data(), len, RN_HWID_MP1, &v) && v.major == 13 && v.minor == 0 && v.rev == 7);
    CHECK(rn_disc_ip_version(t.data(), len, RN_HWID_SDMA0, &v) && v.major == 6 && v.minor == 0 && v.rev == 2);
    for (uint16_t dev : {0x7480, 0x747E, 0x744C, 0x1586}) CHECK(!rn_asic_lookup(t.data(), len, dev, nullptr));
}

int main(int argc, char **argv) {
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        std::vector<uint8_t> t(1 << 16);
        t.resize(fread(t.data(), 1, t.size(), f));
        fclose(f);
        uint32_t len = static_cast<uint32_t>(t.size());
        static const struct { const char *name; uint16_t hwId; } kBlocks[] = {{"GC", RN_HWID_GC}, {"DCN", RN_HWID_DMU}, {"MP0", RN_HWID_MP0}, {"MP1", RN_HWID_MP1}, {"SDMA", RN_HWID_SDMA0}};
        for (const auto &b : kBlocks) {
            rn_ip_version v;
            if (rn_disc_ip_version(t.data(), len, b.hwId, &v)) printf("%-4s %u.%u.%u\n", b.name, v.major, v.minor, v.rev);
            else printf("%-4s not found\n", b.name);
        }
        uint16_t dev = argc > 2 ? static_cast<uint16_t>(strtoul(argv[2], nullptr, 16)) : 0x747E;
        const rn_asic *a = rn_asic_lookup(t.data(), len, dev, nullptr);
        printf("device 0x%04x: %s\n", dev, a ? a->name : "not in the ASIC table (read-only)");
        return 0;
    }
    testVersions();
    testLookup();
    testBad();
    testRealNavi33();
    printf(failures ? "%d FAILED\n" : "all asic tests passed\n", failures);
    return failures ? 1 : 0;
}
