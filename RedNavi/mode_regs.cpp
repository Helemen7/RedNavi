// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#include "mode_regs.h"

#include <string.h>

namespace {

const rn_mode kBase = {148500, 1920, 88, 44, 148, 2200, 1080, 4, 5, 36, 1125, 1, 1, 0, 16, 60};

uint32_t sat32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(v); }

// base * num / den, rounded to nearest, in 64 bits.
uint32_t scale(uint32_t base, uint64_t num, uint64_t den) {
    if (!den) return base;
    return sat32((static_cast<uint64_t>(base) * num + den / 2) / den);
}

// Line time ratio mode/base: (htotal / pclk) / (base.htotal / base.pclk).
uint32_t byLine(uint32_t base, const rn_mode &m) {
    return scale(base, static_cast<uint64_t>(m.htotal) * kBase.pclk_khz, static_cast<uint64_t>(kBase.htotal) * m.pclk_khz);
}
// Pixel clock ratio: base.pclk / pclk.
uint32_t byInvPclk(uint32_t base, const rn_mode &m) { return scale(base, kBase.pclk_khz, m.pclk_khz); }
// Time to fetch one line of active pixels: (hactive / pclk) / (base.hactive / base.pclk).
uint32_t byActive(uint32_t base, const rn_mode &m) {
    return scale(base, static_cast<uint64_t>(m.hactive) * kBase.pclk_khz, static_cast<uint64_t>(kBase.hactive) * m.pclk_khz);
}

uint32_t lbPartitions(unsigned width) {
    unsigned p = width ? 20480u / width : 16u;      // fits 1920 -> 10, 1280 -> 16 (Linux captures)
    return p > 16 ? 16 : (p ? p : 1);
}

}

namespace {

// Dentist divider code (DID) <-> divider in quarter steps, from dc/clk_mgr (dentist_get_divider_from_did).
uint32_t unitsFromDid(uint32_t did) {
    if (did < 0x08) did = 0x08;
    if (did < 0x40) return 8 + (did - 0x08);
    if (did < 0x60) return 64 + 2 * (did - 0x40);
    if (did < 0x7e) return 128 + 4 * (did - 0x60);
    return 248 + 264 * (did - 0x7e);
}

// Largest code whose divider is not above `units` (so the clock is not below the request).
uint32_t didFromUnits(uint32_t units) {
    if (units <= 8) return 0x08;
    if (units < 64) return 0x08 + (units - 8);
    if (units < 128) return 0x40 + (units - 64) / 2;
    if (units < 248) return 0x60 + (units - 128) / 4;
    return 0x7e;
}

const uint64_t kDentistVcoQuarterKhz = 4ull * 4300000;     // vco 4.3 GHz, divider counted in quarters

}

extern "C" {

uint32_t rn_dispclk_register(uint32_t current, const struct rn_mode *mode, int *changed) {
    uint32_t curDid = (current >> 24) & 0x7F;
    uint64_t curKhz = kDentistVcoQuarterKhz / unitsFromDid(curDid);
    uint64_t needKhz = static_cast<uint64_t>(mode->pclk_khz) * 10175 / 10000;      // 1.75% headroom
    *changed = 0;
    if (curKhz >= needKhz || !needKhz) return current;
    uint32_t did = didFromUnits(static_cast<uint32_t>(kDentistVcoQuarterKhz / needKhz));
    *changed = 1;
    return (did << 24) | (current & 0x00FF0000u) | (did << 8) | did;
}

const struct rn_mode *rn_base_mode(void) { return &kBase; }

unsigned rn_pitch_pixels(unsigned width) { return (width + 63u) & ~63u; }

int rn_mode_register(const char *name, uint32_t base, const struct rn_mode_ctx *c, uint32_t *out) {
    const rn_mode &m = *c->mode;
    const bool isBase = m.pclk_khz == kBase.pclk_khz && m.htotal == kBase.htotal && m.vtotal == kBase.vtotal &&
                        m.hactive == kBase.hactive && m.vactive == kBase.vactive;
    const uint32_t hblank = m.hsync + m.hbp, vblank = m.vsync + m.vbp;
    uint32_t v;

#define IS(n) (!strcmp(name, n))
#define SET(x) do { *out = (x); return 1; } while (0)

    // Pixel clock: DP stream clock DTO (modulo stays), and the OTG pixel rate bits Linux sets per mode.
    if (IS("DP_DTO0_PHASE")) SET(sat32(static_cast<uint64_t>(m.pclk_khz) * 500));
    if (IS("OTG0_PIXEL_RATE_CNTL")) SET(isBase ? base : (base & ~0x10000u));
    if (IS("OTG0_OTG_H_TIMING_CNTL")) SET(isBase ? base : (base & ~0x20000u));

    // OTG timing
    if (IS("OTG0_OTG_H_TOTAL")) SET(m.htotal - 1u);
    if (IS("OTG0_OTG_H_BLANK_START_END")) SET((m.htotal - m.hfp) | (hblank << 16));
    if (IS("OTG0_OTG_H_SYNC_A")) SET(static_cast<uint32_t>(m.hsync) << 16);
    if (IS("OTG0_OTG_H_SYNC_A_CNTL")) SET((base & ~1u) | (m.hsync_pos ? 0u : 1u));
    if (IS("OTG0_OTG_V_TOTAL") || IS("OTG0_OTG_V_TOTAL_MIN") || IS("OTG0_OTG_V_TOTAL_MAX")) SET(m.vtotal - 1u);
    if (IS("OTG0_OTG_V_BLANK_START_END")) SET((m.vtotal - m.vfp) | (vblank << 16));
    if (IS("OTG0_OTG_V_SYNC_A")) SET(static_cast<uint32_t>(m.vsync) << 16);
    if (IS("OTG0_OTG_V_SYNC_A_CNTL")) SET((base & ~1u) | (m.vsync_pos ? 0u : 1u));
    if (IS("OTG0_OTG_VSTARTUP_PARAM")) SET(static_cast<uint32_t>(m.vtotal - m.vactive) - 1u);
    if (IS("OTG0_OTG_VUPDATE_PARAM")) SET((static_cast<uint32_t>(m.hactive <= 1280 ? 182 : 247) << 16) | ((m.htotal + 2u) / 4u));
    if (IS("OTG0_OTG_VREADY_PARAM")) SET(m.hactive <= 1280 ? 236u : 328u);

    // DP main stream attributes
    if (IS("DP4_DP_MSA_TIMING_PARAM1")) SET(static_cast<uint32_t>(m.vtotal) | (static_cast<uint32_t>(m.htotal) << 16));
    if (IS("DP4_DP_MSA_TIMING_PARAM2")) SET(vblank | (hblank << 16));
    if (IS("DP4_DP_MSA_TIMING_PARAM3"))
        SET((m.vsync | (m.vsync_pos ? 0u : 0x8000u)) | ((m.hsync | (m.hsync_pos ? 0u : 0x8000u)) << 16));
    if (IS("DP4_DP_MSA_TIMING_PARAM4")) SET(static_cast<uint32_t>(m.vactive) | (static_cast<uint32_t>(m.hactive) << 16));

    // Sizes
    v = static_cast<uint32_t>(m.hactive) | (static_cast<uint32_t>(m.vactive) << 16);
    if (IS("DPG0_DPG_DIMENSIONS")) SET(static_cast<uint32_t>(m.vactive) | (static_cast<uint32_t>(m.hactive) << 16));
    if (IS("HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION") || IS("HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION_C") ||
        IS("HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION") || IS("HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION_C") ||
        IS("DSCL0_RECOUT_SIZE") || IS("DSCL0_MPC_SIZE")) SET(v);
    if (IS("HUBPREQ0_DCSURF_SURFACE_PITCH")) SET((base & 0xFFFF0000u) | (c->pitch_pixels - 1u));
    if (IS("DSCL0_LB_MEMORY_CTRL")) {                          // two partition counts (bits 24-31 and 16-23), 0x1f00 stays
        uint32_t p = lbPartitions(m.hactive);
        SET((base & 0x0000FFFFu) | (p << 24) | (p << 16));
    }

    // HUBP timing in reference cycles (refclk 100 MHz)
    if (IS("HUBPREQ0_DST_DIMENSIONS")) SET(sat32(static_cast<uint64_t>(m.htotal) * 12800000ull / m.pclk_khz));
    if (IS("HUBPREQ0_REF_FREQ_TO_PIX_FREQ")) SET(sat32(26214400000ull / m.pclk_khz));

    // Scaling laws fitted on the Linux captures
    if (IS("HUBPREQ0_DCN_TTU_QOS_WM")) SET(byLine(base >> 16, m) << 16);
    if (IS("HUBPREQ0_DCN_SURF0_TTU_CNTL0")) SET((base & 0xFFFF0000u) | (byInvPclk(base & 0xFFFF, m) & 0xFFFF));
    if (IS("HUBPREQ0_DCN_SURF0_TTU_CNTL1")) SET(byInvPclk(base, m));
    if (IS("HUBPREQ0_PER_LINE_DELIVERY") || IS("HUBPREQ0_PER_LINE_DELIVERY_PRE")) SET(byActive(base, m));
    if (IS("HUBPREQ0_NOM_PARAMETERS_5")) SET(byLine(base, m));
    // End of vertical blank in lines (the high half; Linux: 25 at 720p, 41 at 1080p60, 27 at 1080p144), low half unknown, kept.
    if (IS("HUBPREQ0_BLANK_OFFSET_0")) SET((base & 0x0000FFFFu) | (vblank << 16));
    if (IS("HUBPREQ0_DST_AFTER_SCALER")) SET(byInvPclk(base, m));          // 148 / 74 / 33 at 74.25 / 148.5 / 325 MHz
    if (!strncmp(name, "DCHUBBUB_ARB_FRAC_URG_BW_NOM_", 30) && m.pclk_khz > kBase.pclk_khz)
        SET(scale(base, m.pclk_khz, kBase.pclk_khz));                  // more bandwidth, higher urgent fraction
#undef IS
#undef SET
    return 0;
}

}
