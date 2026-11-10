// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Host test: the mode-dependent register values computed by RedNavi/mode_regs.cpp against Linux register captures of the
// same card at 1280x720@60, 1920x1080@60 and 1920x1080@144 (docs/logs/linux-reference/modes/pipe_*.txt). Run from the
// repository root with `make test`.
#include "../../RedNavi/mode_regs.h"
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

typedef std::map<std::string, uint32_t> Dump;

static Dump load(const char *file) {
    Dump d;
    FILE *f = fopen((std::string("docs/logs/linux-reference/modes/") + file).c_str(), "r");
    if (!f) { printf("cannot open %s (run from the repository root)\n", file); exit(2); }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char name[200], idx[16], off[16], val[16];
        if (line[0] != '#' && sscanf(line, "%199s %15s %15s %15s", name, idx, off, val) == 4) d[name] = strtoul(val, nullptr, 16);
    }
    fclose(f);
    return d;
}

struct Case { const char *file; rn_mode mode; };

static void checkExact(const Dump &cap, const Dump &base, const rn_mode &m, const char *label) {
    static const char *const exact[] = {
        "DP_DTO0_PHASE", "OTG0_OTG_H_TOTAL", "OTG0_OTG_H_BLANK_START_END", "OTG0_OTG_H_SYNC_A", "OTG0_OTG_V_TOTAL",
        "OTG0_OTG_V_TOTAL_MIN", "OTG0_OTG_V_TOTAL_MAX", "OTG0_OTG_V_BLANK_START_END", "OTG0_OTG_V_SYNC_A",
        "OTG0_OTG_H_TIMING_CNTL", "OTG0_PIXEL_RATE_CNTL",
        "OTG0_OTG_VSTARTUP_PARAM", "DP4_DP_MSA_TIMING_PARAM1", "DP4_DP_MSA_TIMING_PARAM2", "DP4_DP_MSA_TIMING_PARAM3",
        "DP4_DP_MSA_TIMING_PARAM4", "DPG0_DPG_DIMENSIONS", "HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION",
        "HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION", "DSCL0_RECOUT_SIZE", "DSCL0_MPC_SIZE", "DSCL0_LB_MEMORY_CTRL",
        "HUBPREQ0_DST_DIMENSIONS", "HUBPREQ0_REF_FREQ_TO_PIX_FREQ",
    };
    rn_mode_ctx ctx = {&m, rn_pitch_pixels(m.hactive), 4};
    for (const char *name : exact) {
        auto c = cap.find(name), b = base.find(name);
        if (c == cap.end() || b == base.end()) { printf("FAIL %s: %s missing from a capture\n", label, name); failures++; continue; }
        uint32_t out = 0;
        int handled = rn_mode_register(name, b->second, &ctx, &out);
        if (!handled || out != c->second) {
            printf("FAIL %s: %s computed 0x%08x (handled %d), Linux 0x%08x\n", label, name, out, handled, c->second);
            failures++;
        }
    }
    uint32_t bo = 0;
    rn_mode_register("HUBPREQ0_BLANK_OFFSET_0", base.at("HUBPREQ0_BLANK_OFFSET_0"), &ctx, &bo);
    CHECK((bo >> 16) == (cap.at("HUBPREQ0_BLANK_OFFSET_0") >> 16));
    // The pitch field of the luma plane (low 16 bits), the high half belongs to the chroma pitch.
    uint32_t out = 0;
    rn_mode_register("HUBPREQ0_DCSURF_SURFACE_PITCH", 0x77f, &ctx, &out);
    CHECK((out & 0xFFFF) == (cap.at("HUBPREQ0_DCSURF_SURFACE_PITCH") & 0xFFFF));
    // VUPDATE: low half is fitted exactly, the high half is a two-step approximation; VREADY the same.
    rn_mode_register("OTG0_OTG_VUPDATE_PARAM", 0, &ctx, &out);
    CHECK((out & 0xFFFF) == (cap.at("OTG0_OTG_VUPDATE_PARAM") & 0xFFFF));
    CHECK((out >> 16) == (cap.at("OTG0_OTG_VUPDATE_PARAM") >> 16));
    rn_mode_register("OTG0_OTG_VREADY_PARAM", 0, &ctx, &out);
    CHECK(out == cap.at("OTG0_OTG_VREADY_PARAM"));
}

// Within `tolPercent` of the Linux value, for the registers that follow a scaling law rather than an exact formula.
static void checkLaw(const Dump &cap, const Dump &base, const rn_mode &m, const char *label, unsigned mask, unsigned shift) {
    static const char *const laws[] = {"HUBPREQ0_DCN_TTU_QOS_WM", "HUBPREQ0_DCN_SURF0_TTU_CNTL0", "HUBPREQ0_DCN_SURF0_TTU_CNTL1",
                                       "HUBPREQ0_PER_LINE_DELIVERY", "HUBPREQ0_PER_LINE_DELIVERY_PRE",
                                       "HUBPREQ0_DST_AFTER_SCALER"};
    rn_mode_ctx ctx = {&m, rn_pitch_pixels(m.hactive), 4};
    (void)mask; (void)shift;
    for (const char *name : laws) {
        uint32_t out = 0;
        rn_mode_register(name, base.at(name), &ctx, &out);
        uint32_t want = cap.at(name);
        // compare the significant field (QOS_WM and TTU_CNTL0 keep a constant in the other half)
        unsigned sh = !strcmp(name, "HUBPREQ0_DCN_TTU_QOS_WM") ? 16 : 0;
        uint32_t a = (out >> sh) & 0xFFFF, b = (want >> sh) & 0xFFFF;
        uint32_t diff = a > b ? a - b : b - a;
        if (diff > 1 && diff * 100 > b * 1) { printf("FAIL %s: %s computed 0x%x, Linux 0x%x (law off by more than 1%%)\n", label, name, a, b); failures++; }
    }
}

int main() {
    Dump c720 = load("pipe_720p60.txt"), c1080 = load("pipe_1080p60.txt"), c144 = load("pipe_1080p144.txt");
    const rn_mode m720 = {74250, 1280, 110, 40, 220, 1650, 720, 5, 5, 20, 750, 1, 1, 0, 4, 60};
    const rn_mode m1080 = *rn_base_mode();
    const rn_mode m144 = {325070, 1920, 24, 32, 58, 2034, 1080, 3, 5, 22, 1110, 1, 1, 0, 0, 144};   // as Linux programmed it
    checkExact(c720, c1080, m720, "720p60");
    checkExact(c1080, c1080, m1080, "1080p60");
    checkExact(c144, c1080, m144, "1080p144");
    checkLaw(c720, c1080, m720, "720p60", 0, 0);
    checkLaw(c144, c1080, m144, "1080p144", 0, 0);

    // The base mode keeps its quirks, other modes get the Linux mode-switch values; polarity bits follow the mode.
    rn_mode vga = {25200, 640, 16, 96, 48, 800, 480, 10, 2, 33, 525, 0, 0, 0, 1, 60};
    rn_mode_ctx cv = {&vga, 640, 4};
    uint32_t out = 0;
    CHECK(rn_mode_register("OTG0_OTG_H_SYNC_A_CNTL", 0, &cv, &out) && out == 1);
    CHECK(rn_mode_register("OTG0_OTG_V_SYNC_A_CNTL", 0, &cv, &out) && out == 1);
    CHECK(rn_mode_register("DP4_DP_MSA_TIMING_PARAM3", 0, &cv, &out) && out == ((2u | 0x8000u) | ((96u | 0x8000u) << 16)));
    CHECK(!rn_mode_register("OTG0_OTG_STATUS_POSITION", 0, &cv, &out));
    CHECK(rn_pitch_pixels(800) == 832 && rn_pitch_pixels(1920) == 1920 && rn_pitch_pixels(720) == 768);
    // DISPCLK: the VBIOS value 0x54195454 (about 165 MHz) is enough up to 1080p60 and too low for 325 MHz, where Linux uses 0x34.
    int changed = 0;
    CHECK(rn_dispclk_register(0x54195454, &m1080, &changed) == 0x54195454 && !changed);
    CHECK(rn_dispclk_register(0x54195454, &m720, &changed) == 0x54195454 && !changed);
    uint32_t dv = rn_dispclk_register(0x54195454, &m144, &changed);
    CHECK(changed && (dv >> 24) == 0x34 && ((dv >> 8) & 0x7F) == 0x34 && (dv & 0x7F) == 0x34 && ((dv >> 16) & 0xFF) == 0x19);
    CHECK((rn_dispclk_register(0x341f3434, &m144, &changed) == 0x341f3434) && !changed);       // already fast enough
    printf(failures ? "%d FAILED\n" : "all mode register tests passed\n", failures);
    return failures ? 1 : 0;
}
