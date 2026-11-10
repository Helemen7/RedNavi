// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Host tests for RedNavi/edid.cpp. Run with `make test`.
#include "../../RedNavi/edid.h"
#include <stdio.h>
#include <string.h>

// The base block and CTA extension of a Samsung F24G3xTF as the probe read it over AUX, with the serial number
// removed and the checksum recomputed.
static const uint8_t kEdid[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x4c, 0x2d, 0x0b, 0x71, 0x00, 0x00, 0x00, 0x00,
    0x06, 0x1f, 0x01, 0x04, 0xb5, 0x35, 0x1d, 0x78, 0x3b, 0xb6, 0xe0, 0xa3, 0x56, 0x50, 0xa0, 0x27,
    0x0e, 0x50, 0x54, 0xbf, 0xef, 0x00, 0x81, 0x80, 0x81, 0xbc, 0x95, 0x00, 0x95, 0x3c, 0xb3, 0x00,
    0xb3, 0x3c, 0xd1, 0xc0, 0xd1, 0xfc, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
    0x45, 0x00, 0x0f, 0x28, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xff, 0x00, 0x30, 0x0a, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x30,
    0x90, 0xb4, 0xb4, 0x22, 0x01, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc,
    0x00, 0x46, 0x32, 0x34, 0x47, 0x33, 0x78, 0x54, 0x46, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x01, 0xf0,
    0x02, 0x03, 0x26, 0xf1, 0x4b, 0x01, 0x03, 0x05, 0x14, 0x04, 0x13, 0x1f, 0x12, 0x02, 0x11, 0x90,
    0x23, 0x09, 0x07, 0x07, 0x83, 0x01, 0x00, 0x00, 0x6d, 0x1a, 0x00, 0x00, 0x02, 0x01, 0x30, 0x90,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x86, 0x6f, 0x80, 0xa0, 0x70, 0x38, 0x40, 0x40, 0x30, 0x20,
    0x35, 0x00, 0x0f, 0x28, 0x21, 0x00, 0x00, 0x1e, 0xfb, 0x7e, 0x80, 0x72, 0x70, 0x38, 0x1e, 0x40,
    0x18, 0x20, 0x35, 0x00, 0x0f, 0x28, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd,
};

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const rn_mode *find(const rn_edid_info &i, unsigned w, unsigned h, unsigned r) {
    for (unsigned k = 0; k < i.mode_count; k++)
        if (i.modes[k].hactive == w && i.modes[k].vactive == h && i.modes[k].refresh_hz == r) return &i.modes[k];
    return nullptr;
}

static void testParse() {
    rn_edid_info info;
    CHECK(rn_edid_parse(kEdid, sizeof(kEdid), &info) == 0);
    CHECK(!strcmp(info.name, "F24G3xTF"));
    CHECK(info.mode_count > 5);
    CHECK(info.modes[0].preferred == 1 && info.modes[0].hactive == 1920 && info.modes[0].vactive == 1080);
    CHECK(info.modes[0].pclk_khz == 148500 && info.modes[0].htotal == 2200 && info.modes[0].vtotal == 1125);
    CHECK(info.modes[0].hfp == 88 && info.modes[0].hsync == 44 && info.modes[0].hbp == 148);
    CHECK(info.modes[0].vfp == 4 && info.modes[0].vsync == 5 && info.modes[0].vbp == 36);
    CHECK(info.modes[0].hsync_pos == 1 && info.modes[0].vsync_pos == 1 && info.modes[0].refresh_hz == 60);
    const rn_mode *m720 = find(info, 1280, 720, 60);
    CHECK(m720 && m720->pclk_khz == 74250 && m720->htotal == 1650 && m720->vtotal == 750 && m720->vic == 4);
    const rn_mode *m1080p50 = find(info, 1920, 1080, 50);
    CHECK(m1080p50 && m1080p50->pclk_khz == 148500 && m1080p50->htotal == 2640);
    CHECK(find(info, 640, 480, 60));
    for (unsigned a = 0; a < info.mode_count; a++)                       // no duplicates
        for (unsigned b = a + 1; b < info.mode_count; b++)
            CHECK(!(info.modes[a].hactive == info.modes[b].hactive && info.modes[a].vactive == info.modes[b].vactive &&
                    info.modes[a].refresh_hz == info.modes[b].refresh_hz));
    for (unsigned k = 0; k < info.mode_count; k++) {                      // totals add up
        const rn_mode &m = info.modes[k];
        CHECK(m.htotal == m.hactive + m.hfp + m.hsync + m.hbp);
        CHECK(m.vtotal == m.vactive + m.vfp + m.vsync + m.vbp);
    }
}

static void testBad() {
    rn_edid_info info;
    uint8_t bad[256];
    memcpy(bad, kEdid, 256);
    bad[20] ^= 1;                                                        // breaks the first checksum
    CHECK(rn_edid_parse(bad, 256, &info) < 0);
    memcpy(bad, kEdid, 256);
    bad[255] ^= 1;                                                       // breaks the extension only
    CHECK(rn_edid_parse(bad, 256, &info) == 0 && info.mode_count >= 1 && !find(info, 1280, 720, 60) == !find(info, 1280, 720, 60));
    CHECK(rn_edid_parse(kEdid, 64, &info) < 0);
    memcpy(bad, kEdid, 256);
    bad[0] = 1;
    CHECK(rn_edid_parse(bad, 256, &info) < 0);
}

static void testCvt() {
    rn_mode m;
    CHECK(rn_cvt_rb(1920, 1080, 60, &m) == 0 && m.pclk_khz == 138500 && m.htotal == 2080 && m.vtotal == 1111);
    CHECK(m.hsync == 32 && m.hfp == 48 && m.hbp == 80 && m.vfp == 3 && m.vsync == 5 && m.hsync_pos == 1 && m.vsync_pos == 0);
    CHECK(rn_cvt_rb(1280, 720, 60, &m) == 0 && m.pclk_khz == 64000 && m.vtotal == 741);
    CHECK(rn_cvt_rb(1024, 768, 60, &m) == 0 && m.pclk_khz == 56000 && m.vtotal == 790 && m.vsync == 4);
    CHECK(rn_cvt_rb(0, 768, 60, &m) < 0);
}

static void testLinkRate() {
    rn_mode m;
    CHECK(rn_cvt_rb(1920, 1080, 60, &m) == 0 && rn_min_dp_link_rate(&m, 4) == 0x06);
    rn_edid_info info;
    rn_edid_parse(kEdid, sizeof(kEdid), &info);
    CHECK(rn_min_dp_link_rate(&info.modes[0], 4) == 0x06);
    CHECK(rn_min_dp_link_rate(&info.modes[0], 1) == 0x1E);                // 445 MB/s with headroom needs HBR3 on one lane
    m = info.modes[0];
    m.pclk_khz = 594000;                                                  // 4K60
    CHECK(rn_min_dp_link_rate(&m, 4) == 0x1E);                            // 1782 MB/s is above 80% of HBR2 x4 (1728)
    m.pclk_khz = 325070;
    CHECK(rn_min_dp_link_rate(&m, 4) == 0x14);                            // 1080p144: HBR2 like Linux
    m.pclk_khz = 5000000;
    CHECK(rn_min_dp_link_rate(&m, 4) < 0);
}

int main() {
    testParse();
    testBad();
    testCvt();
    testLinkRate();
    printf(failures ? "%d FAILED\n" : "all edid tests passed\n", failures);
    return failures ? 1 : 0;
}
