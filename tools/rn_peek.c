// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Reads 4 pixels at a grid of points of the scanout surface (VRAM offset 0x08000000, pitch 7680, 1920x1080) through the probe's user client (RN_UC_VRAM_PEEK), to see what the display shows
// when the screen looks blank. usage: rn_peek [OFFSET_IN_VRAM [PITCH]]
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define RN_UC_VRAM_PEEK 18
struct peek { uint64_t offset; uint32_t count; uint32_t pad; };
struct peek_res { uint32_t v[64]; };
int main(int argc, char **argv) {
    uint64_t base = argc > 1 ? strtoull(argv[1], NULL, 0) : 0x08000000ULL;
    uint32_t pitch = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 7680;
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedNaviProbe"));
    if (!s) { fprintf(stderr, "no RedNaviProbe\n"); return 1; }
    io_connect_t c;
    if (IOServiceOpen(s, mach_task_self(), 0, &c) != KERN_SUCCESS) { fprintf(stderr, "cannot open the probe\n"); return 1; }
    const int ys[] = {0, 100, 270, 540, 810, 1000, 1070}, xs[] = {0, 200, 480, 960, 1440, 1700, 1900};
    for (unsigned yi = 0; yi < sizeof ys / sizeof *ys; yi++) {
        printf("y=%4d:", ys[yi]);
        for (unsigned xi = 0; xi < sizeof xs / sizeof *xs; xi++) {
            struct peek in = {base + (uint64_t)ys[yi] * pitch + (uint64_t)xs[xi] * 4, 1, 0};
            struct peek_res out; size_t outn = sizeof out;
            kern_return_t kr = IOConnectCallStructMethod(c, RN_UC_VRAM_PEEK, &in, sizeof in, &out, &outn);
            printf(" %s%08x", kr == KERN_SUCCESS ? "" : "ERR", kr == KERN_SUCCESS ? out.v[0] : (unsigned)kr);
        }
        printf("\n");
    }
    return 0;
}
