// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Dumps the scanout surface (VRAM offset 0x08000000, 1920x1080, pitch 7680) as raw BGRA to a file through the probe's user client, one line in STEP (default 4), 64 pixels per call.
// usage: rn_peek_dump OUT.raw [STEP [OFFSET [PITCH]]]; STEP lines are skipped (the file holds 1920 x (1080 / STEP) pixels).
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define RN_UC_VRAM_PEEK 18
struct peek { uint64_t offset; uint32_t count; uint32_t pad; };
struct peek_res { uint32_t v[64]; };
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    unsigned step = argc > 2 ? (unsigned)atoi(argv[2]) : 4;
    uint64_t base = argc > 3 ? strtoull(argv[3], NULL, 0) : 0x08000000ULL;
    uint32_t pitch = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 7680;
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedNaviProbe"));
    io_connect_t c;
    if (!s || IOServiceOpen(s, mach_task_self(), 0, &c) != KERN_SUCCESS) { fprintf(stderr, "cannot open the probe\n"); return 1; }
    FILE *f = fopen(argv[1], "wb");
    for (unsigned y = 0; y < 1080; y += step)
        for (unsigned x = 0; x < 1920; x += 64) {
            struct peek in = {base + (uint64_t)y * pitch + (uint64_t)x * 4, 64, 0};
            struct peek_res out; size_t outn = sizeof out;
            if (IOConnectCallStructMethod(c, RN_UC_VRAM_PEEK, &in, sizeof in, &out, &outn) != KERN_SUCCESS) { fprintf(stderr, "peek failed at y=%u x=%u\n", y, x); return 1; }
            fwrite(out.v, 4, 64, f);
        }
    fclose(f);
    return 0;
}
