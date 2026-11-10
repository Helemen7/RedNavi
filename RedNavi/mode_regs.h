// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Mode-dependent DCN 3.2 register values for the replay table (RedNavi/dcn_replay.h), no kernel dependencies.
//
// The replay table holds the registers of one lit pipe: 1920x1080@60 with a linear XRGB8888 plane, as Linux left them.
// For any other mode the registers below are recomputed. Exact formulas (checked against Linux captures at
// 1280x720@60, 1920x1080@60 and 1920x1080@144 in tests/host/mode_regs_test.cpp): OTG timing, DP MSA, the DP DTO,
// sizes and pitch, HUBP refcyc_per_htotal and ref-to-pixel-freq. Laws that scale with pixel clock or line time, taken
// from the same captures: TTU and per-line-delivery values, the urgent-bandwidth fractions. Everything else the
// display mode math (DML) would produce is kept as in the baseline, which is the part to watch for underflow.
#ifndef REDNAVI_MODE_REGS_H
#define REDNAVI_MODE_REGS_H

#include <stdint.h>

#include "edid.h"

struct rn_mode_ctx {
    const struct rn_mode *mode;
    unsigned pitch_pixels;             // scanout pitch in pixels (width rounded up to 64, 256-byte lines)
    unsigned lanes;                    // DisplayPort lanes in use
};

#ifdef __cplusplus
extern "C" {
#endif

// The timing the replay table was captured at: CTA VIC 16, 1920x1080@60, 148.5 MHz.
const struct rn_mode *rn_base_mode(void);

unsigned rn_pitch_pixels(unsigned width);

// If the register `name` of the replay table depends on the mode, stores its new value in *out and returns 1;
// baseValue is the value in the table. Returns 0 for registers that stay as they are.
int rn_mode_register(const char *name, uint32_t baseValue, const struct rn_mode_ctx *ctx, uint32_t *out);

// DENTIST_DISPCLK_CNTL for a mode: the VBIOS leaves DISPCLK at about 165 MHz, enough up to a pixel clock of roughly 160 MHz.
// Above that the divider code (bits 30-24, copied to the two DPPCLK dividers in bits 14-8 and 6-0) must be lowered: the DENTIST
// VCO is 4.3 GHz and the divider is in quarter steps, DISPCLK = 4.3 GHz * 4 / units, with 1.75% headroom as Linux uses
// (330.769 MHz for 325.07 MHz, code 0x34). Returns the value to write; *changed is 1 if it differs from `current`.
uint32_t rn_dispclk_register(uint32_t current, const struct rn_mode *mode, int *changed);

#ifdef __cplusplus
}
#endif

#endif
