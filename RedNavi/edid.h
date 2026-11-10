// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// EDID parsing and video timings, no kernel or libc dependencies beyond memcpy/memset, so it builds in the kext and in
// the host tests (tests/host/edid_test.cpp).
//
// Sources of modes, in order: detailed timing descriptors of the base block (the first is the preferred mode),
// CTA-861 video data blocks (the VICs in the table below), established timings, standard timings (generated with CVT
// reduced blanking). Interlaced modes are skipped.
#ifndef REDNAVI_EDID_H
#define REDNAVI_EDID_H

#include <stddef.h>
#include <stdint.h>

#define RN_EDID_MAX_MODES 32

struct rn_mode {
    uint32_t pclk_khz;
    uint16_t hactive, hfp, hsync, hbp, htotal;
    uint16_t vactive, vfp, vsync, vbp, vtotal;
    uint8_t hsync_pos, vsync_pos;      // 1 = positive polarity
    uint8_t preferred;
    uint8_t vic;                       // CTA-861 VIC when the mode came from one, else 0
    uint16_t refresh_hz;               // rounded
};

struct rn_edid_info {
    char name[16];                     // monitor name descriptor, "" if absent
    uint16_t manufacturer;             // 3 letters packed as in the EDID
    uint16_t product;
    uint16_t width_mm, height_mm;
    unsigned mode_count;
    struct rn_mode modes[RN_EDID_MAX_MODES];
};

#ifdef __cplusplus
extern "C" {
#endif

// Returns 0 on success, a negative value if the header or the first block's checksum is wrong. Extension blocks
// with a wrong checksum are ignored.
int rn_edid_parse(const uint8_t *edid, size_t len, struct rn_edid_info *info);

// CVT reduced blanking (VESA CVT 1.2) for a standard timing entry; returns 0 on success.
int rn_cvt_rb(unsigned hactive, unsigned vactive, unsigned refresh_hz, struct rn_mode *m);

// Pixel rate a mode needs on the link in bytes per second at 24 bits per pixel, and the lowest DisplayPort link
// rate (with 20% headroom) code (0x06 RBR 1.62, 0x0A HBR 2.7, 0x14 HBR2 5.4, 0x1E HBR3 8.1 Gbps per lane) that carries it on 4 lanes.
uint64_t rn_mode_bytes_per_second(const struct rn_mode *m);
int rn_min_dp_link_rate(const struct rn_mode *m, unsigned lanes);

#ifdef __cplusplus
}
#endif

#endif
