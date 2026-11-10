// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#include "edid.h"

#include <string.h>

namespace {

// Common progressive CTA-861 video formats (VIC, active size, pixel clock, horizontal and vertical timing).
struct Vic {
    uint8_t vic;
    uint32_t pclk_khz;
    uint16_t ha, hfp, hs, hbp, va, vfp, vs, vbp;
    uint8_t hpos, vpos;
};

const Vic kVics[] = {
    {1, 25200, 640, 16, 96, 48, 480, 10, 2, 33, 0, 0},
    {2, 27000, 720, 16, 62, 60, 480, 9, 6, 30, 0, 0},
    {3, 27000, 720, 16, 62, 60, 480, 9, 6, 30, 0, 0},
    {4, 74250, 1280, 110, 40, 220, 720, 5, 5, 20, 1, 1},
    {16, 148500, 1920, 88, 44, 148, 1080, 4, 5, 36, 1, 1},
    {17, 27000, 720, 12, 64, 68, 576, 5, 5, 39, 0, 0},
    {18, 27000, 720, 12, 64, 68, 576, 5, 5, 39, 0, 0},
    {19, 74250, 1280, 440, 40, 220, 720, 5, 5, 20, 1, 1},
    {31, 148500, 1920, 528, 44, 148, 1080, 4, 5, 36, 1, 1},
    {32, 74250, 1920, 638, 44, 148, 1080, 4, 5, 36, 1, 1},
    {33, 74250, 1920, 528, 44, 148, 1080, 4, 5, 36, 1, 1},
    {34, 74250, 1920, 88, 44, 148, 1080, 4, 5, 36, 1, 1},
    {63, 297000, 1920, 88, 44, 148, 1080, 4, 5, 36, 1, 1},
    {64, 297000, 1920, 528, 44, 148, 1080, 4, 5, 36, 1, 1},
    {93, 297000, 3840, 1276, 88, 296, 2160, 8, 10, 72, 1, 1},
    {94, 297000, 3840, 1056, 88, 296, 2160, 8, 10, 72, 1, 1},
    {95, 297000, 3840, 176, 88, 296, 2160, 8, 10, 72, 1, 1},
    {96, 594000, 3840, 1056, 88, 296, 2160, 8, 10, 72, 1, 1},
    {97, 594000, 3840, 176, 88, 296, 2160, 8, 10, 72, 1, 1},
};

// Established timings that have a fixed VESA DMT definition we carry.
const Vic kDmt[] = {
    {0, 25175, 640, 16, 96, 48, 480, 10, 2, 33, 0, 0},      // 640x480@60 (same as VIC 1, DMT clock)
    {0, 40000, 800, 40, 128, 88, 600, 1, 4, 23, 1, 1},      // 800x600@60
    {0, 65000, 1024, 24, 136, 160, 768, 3, 6, 29, 0, 0},    // 1024x768@60
};

uint16_t refreshOf(const rn_mode &m) {
    uint64_t total = static_cast<uint64_t>(m.htotal) * m.vtotal;
    if (!total) return 0;
    return static_cast<uint16_t>((static_cast<uint64_t>(m.pclk_khz) * 1000 + total / 2) / total);
}

void fillFromVic(const Vic &v, rn_mode *m) {
    memset(m, 0, sizeof(*m));
    m->pclk_khz = v.pclk_khz;
    m->hactive = v.ha; m->hfp = v.hfp; m->hsync = v.hs; m->hbp = v.hbp;
    m->htotal = static_cast<uint16_t>(v.ha + v.hfp + v.hs + v.hbp);
    m->vactive = v.va; m->vfp = v.vfp; m->vsync = v.vs; m->vbp = v.vbp;
    m->vtotal = static_cast<uint16_t>(v.va + v.vfp + v.vs + v.vbp);
    m->hsync_pos = v.hpos; m->vsync_pos = v.vpos;
    m->vic = v.vic;
    m->refresh_hz = refreshOf(*m);
}

bool sameMode(const rn_mode &a, const rn_mode &b) {
    return a.hactive == b.hactive && a.vactive == b.vactive && a.refresh_hz == b.refresh_hz;
}

void addMode(rn_edid_info *info, const rn_mode &m) {
    if (!m.pclk_khz || !m.htotal || !m.vtotal) return;
    for (unsigned i = 0; i < info->mode_count; i++)
        if (sameMode(info->modes[i], m)) return;
    if (info->mode_count >= RN_EDID_MAX_MODES) return;
    info->modes[info->mode_count++] = m;
}

bool checksumOk(const uint8_t *block) {
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) sum = static_cast<uint8_t>(sum + block[i]);
    return sum == 0;
}

// 18-byte detailed timing descriptor. Returns false for monitor descriptors and interlaced modes.
bool parseDtd(const uint8_t *d, rn_mode *m) {
    uint32_t clk = static_cast<uint32_t>(d[0] | (d[1] << 8)) * 10u;
    if (!clk) return false;
    if (d[17] & 0x80) return false;                       // interlaced
    memset(m, 0, sizeof(*m));
    m->pclk_khz = clk;
    m->hactive = static_cast<uint16_t>(d[2] | ((d[4] & 0xF0) << 4));
    uint16_t hblank = static_cast<uint16_t>(d[3] | ((d[4] & 0x0F) << 8));
    m->vactive = static_cast<uint16_t>(d[5] | ((d[7] & 0xF0) << 4));
    uint16_t vblank = static_cast<uint16_t>(d[6] | ((d[7] & 0x0F) << 8));
    m->hfp = static_cast<uint16_t>(d[8] | ((d[11] & 0xC0) << 2));
    m->hsync = static_cast<uint16_t>(d[9] | ((d[11] & 0x30) << 4));
    m->vfp = static_cast<uint16_t>((d[10] >> 4) | ((d[11] & 0x0C) << 2));
    m->vsync = static_cast<uint16_t>((d[10] & 0x0F) | ((d[11] & 0x03) << 4));
    m->htotal = static_cast<uint16_t>(m->hactive + hblank);
    m->vtotal = static_cast<uint16_t>(m->vactive + vblank);
    if (hblank < m->hfp + m->hsync || vblank < m->vfp + m->vsync) return false;
    m->hbp = static_cast<uint16_t>(hblank - m->hfp - m->hsync);
    m->vbp = static_cast<uint16_t>(vblank - m->vfp - m->vsync);
    m->hsync_pos = (d[17] >> 1) & 1;                       // digital separate sync: bit 1 hsync, bit 2 vsync positive
    m->vsync_pos = (d[17] >> 2) & 1;
    m->refresh_hz = refreshOf(*m);
    return m->hactive && m->vactive;
}

void parseCta(const uint8_t *block, rn_edid_info *info) {
    if (block[0] != 0x02) return;
    uint8_t dtdOffset = block[2];
    if (dtdOffset < 4 || dtdOffset > 127) return;
    for (unsigned off = 4; off < dtdOffset;) {
        uint8_t tag = block[off] >> 5, len = block[off] & 0x1F;
        if (off + 1 + len > dtdOffset) break;
        if (tag == 2) {                                   // video data block: one VIC per byte
            for (unsigned i = 0; i < len; i++) {
                uint8_t vic = block[off + 1 + i] & 0x7F;
                for (const Vic &v : kVics) {
                    if (v.vic != vic) continue;
                    rn_mode m;
                    fillFromVic(v, &m);
                    addMode(info, m);
                }
            }
        }
        off += 1 + len;
    }
    for (unsigned off = dtdOffset; off + 18 <= 127; off += 18) {      // extra detailed timings
        rn_mode m;
        if (parseDtd(block + off, &m)) addMode(info, m);
    }
}

}

extern "C" {

int rn_cvt_rb(unsigned hactive, unsigned vactive, unsigned refresh_hz, struct rn_mode *m) {
    if (!hactive || !vactive || !refresh_hz) return -1;
    // Vertical sync width from the aspect ratio, as CVT defines it.
    unsigned vsync;
    if (vactive * 4 == hactive * 3) vsync = 4;
    else if (vactive * 16 == hactive * 9) vsync = 5;
    else if (vactive * 16 == hactive * 10) vsync = 6;
    else if (vactive * 4 == hactive * 5) vsync = 7;
    else if (vactive * 9 == hactive * 15) vsync = 7;
    else vsync = 10;
    const unsigned rbHBlank = 160, rbHSync = 32, rbHFp = 48, rbVFp = 3, minVBp = 6, minVBlankUs = 460;
    // h_period (ns*1000 resolution kept in 64-bit): (1e6/refresh - 460) / vactive microseconds.
    uint64_t frameUs1000 = 1000000ull * 1000 / refresh_hz;                      // frame time in 1e-3 us
    uint64_t hPeriod1000 = (frameUs1000 - minVBlankUs * 1000ull) / vactive;     // line time in 1e-3 us
    unsigned vbi = static_cast<unsigned>(minVBlankUs * 1000ull / hPeriod1000) + 1;
    unsigned minVbi = rbVFp + vsync + minVBp;
    if (vbi < minVbi) vbi = minVbi;
    unsigned vtotal = vactive + vbi;
    unsigned htotal = hactive + rbHBlank;
    uint64_t pclk = static_cast<uint64_t>(refresh_hz) * htotal * vtotal / 1000;             // kHz
    pclk = pclk / 250 * 250;                                                                // 0.25 MHz steps
    memset(m, 0, sizeof(*m));
    m->pclk_khz = static_cast<uint32_t>(pclk);
    m->hactive = static_cast<uint16_t>(hactive); m->hfp = rbHFp; m->hsync = rbHSync;
    m->hbp = static_cast<uint16_t>(rbHBlank - rbHFp - rbHSync);
    m->htotal = static_cast<uint16_t>(htotal);
    m->vactive = static_cast<uint16_t>(vactive); m->vfp = rbVFp; m->vsync = static_cast<uint16_t>(vsync);
    m->vbp = static_cast<uint16_t>(vbi - rbVFp - vsync);
    m->vtotal = static_cast<uint16_t>(vtotal);
    m->hsync_pos = 1; m->vsync_pos = 0;
    m->refresh_hz = refreshOf(*m);
    return 0;
}

int rn_edid_parse(const uint8_t *edid, size_t len, struct rn_edid_info *info) {
    static const uint8_t kHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    memset(info, 0, sizeof(*info));
    if (len < 128 || memcmp(edid, kHeader, 8) != 0 || !checksumOk(edid)) return -1;

    info->manufacturer = static_cast<uint16_t>((edid[8] << 8) | edid[9]);
    info->product = static_cast<uint16_t>(edid[10] | (edid[11] << 8));
    info->width_mm = static_cast<uint16_t>(edid[21] * 10);          // cm in the base block
    info->height_mm = static_cast<uint16_t>(edid[22] * 10);

    // Detailed timings and descriptors: 4 slots from byte 54.
    bool first = true;
    for (int i = 0; i < 4; i++) {
        const uint8_t *d = edid + 54 + 18 * i;
        rn_mode m;
        if (d[0] || d[1]) {
            if (parseDtd(d, &m)) {
                if (first) m.preferred = 1;
                first = false;
                addMode(info, m);
            }
        } else if (d[3] == 0xFC) {                                  // monitor name
            unsigned n = 0;
            for (int k = 5; k < 18 && n < sizeof(info->name) - 1; k++) {
                if (d[k] == '\n') break;
                info->name[n++] = static_cast<char>(d[k]);
            }
            info->name[n] = 0;
        }
    }

    // Extension blocks.
    unsigned ext = edid[126];
    for (unsigned b = 1; b <= ext && (b + 1) * 128 <= len; b++) {
        const uint8_t *block = edid + b * 128;
        if (checksumOk(block)) parseCta(block, info);
    }

    // Established timings (only the ones with a definition in kDmt).
    const uint8_t est0 = edid[35], est1 = edid[36];
    if (est0 & 0x20) { rn_mode m; fillFromVic(kDmt[0], &m); addMode(info, m); }
    if (est0 & 0x01) { rn_mode m; fillFromVic(kDmt[1], &m); addMode(info, m); }
    if (est1 & 0x08) { rn_mode m; fillFromVic(kDmt[2], &m); addMode(info, m); }

    // Standard timings: 8 entries from byte 38, generated with CVT reduced blanking.
    for (int i = 0; i < 8; i++) {
        uint8_t a = edid[38 + 2 * i], b = edid[39 + 2 * i];
        if (a == 0x01 && b == 0x01) continue;
        unsigned hactive = (a + 31u) * 8, refresh = (b & 0x3F) + 60u, vactive = 0;
        switch (b >> 6) {
        case 0: vactive = hactive * 10 / 16; break;     // 16:10 (1:1 in EDID 1.0 is not handled)
        case 1: vactive = hactive * 3 / 4; break;
        case 2: vactive = hactive * 4 / 5; break;
        case 3: vactive = hactive * 9 / 16; break;
        }
        rn_mode m;
        if (rn_cvt_rb(hactive, vactive, refresh, &m) == 0) addMode(info, m);
    }
    return 0;
}

uint64_t rn_mode_bytes_per_second(const struct rn_mode *m) {
    return static_cast<uint64_t>(m->pclk_khz) * 1000 * 3;
}

int rn_min_dp_link_rate(const struct rn_mode *m, unsigned lanes) {
    // Payload per lane: link rate (Gbps) * 0.8 (8b/10b) / 8 bits = bytes; a mode may use 80% of it. Codes: 0x06 1.62, 0x0A 2.7, 0x14 5.4, 0x1E 8.1.
    static const struct { int code; uint64_t bytes_per_lane; } rates[] = {
        {0x06, 162000000ull}, {0x0A, 270000000ull}, {0x14, 540000000ull}, {0x1E, 810000000ull},
    };
    uint64_t need = rn_mode_bytes_per_second(m);
    for (const auto &r : rates)
        if (need * 10 <= r.bytes_per_lane * lanes * 8) return r.code;      // keep 20% headroom (blanking, overhead)
    return -1;
}

}
