// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// rn_vk_selftest(): RADV inside the calling process. The shader is mesa-bridge/test/vk_dispatch.comp (v[i] = i * 3 + push), the same one vk_dispatch runs with the Vulkan loader.
#include "rn_vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../mesa-bridge/test/vk_dispatch_spv.h"

// RN_VK_COPYTEST=WxH: what direct scanout does first. A linear image over host memory (the scanout stands in) is copied to an image on the card, that one to a second one, and the
// second is read back and compared with the pattern. Run with RADV_DEBUG variants to see which copy path the card survives.
static int copytest(rn_vk *vk, uint32_t w, uint32_t h) {
    const size_t bytes = ((size_t)w * h * 4 + 4095) & ~(size_t)4095;
    uint32_t *host = valloc(bytes);
    if (!host) return 20;
    for (size_t i = 0; i < (size_t)w * h; i++) host[i] = 0xff000000u | (uint32_t)(i * 2654435761u >> 8);
    rn_vk_image *src = NULL;
    if (getenv("RN_VK_COPYTEST_SCANOUT")) {     // the real scanout surface as the source (read only): the size is the display's, and the pattern is whatever the screen shows
        int (*scan)(uint32_t, uint32_t *, uint32_t *, uint32_t *, void **, size_t *) = rn_vk_symbol(vk, "rn_bridge_scanout");
        uint32_t bw = 0, bh = 0, bp = 0; void *cpu = NULL; size_t len = 0;
        const int r = scan ? scan(0, &bw, &bh, &bp, &cpu, &len) : -1;
        fprintf(stderr, "copytest: scanout surface -> %d, %ux%u pitch %u, %zu bytes at %p\n", r, bw, bh, bp, len, cpu);
        if (r || !cpu) return 25;
        w = bw; h = bh; free(host); host = cpu;
        src = rn_vk_image_from_host(vk, RN_VK_BGRA8, w, h, cpu, bp, len);
    } else src = rn_vk_image_from_host(vk, RN_VK_BGRA8, w, h, host, (size_t)w * 4, bytes);
    if (!src) { fprintf(stderr, "copytest: no host image (%s)\n", rn_vk_image_last_error()); return 21; }
    rn_vk_image *a = rn_vk_image_create(vk, RN_VK_BGRA8, w, h), *b = rn_vk_image_create(vk, RN_VK_BGRA8, w, h);
    if (!a || !b) { fprintf(stderr, "copytest: no card image (%s)\n", rn_vk_image_last_error()); return 22; }
    int rc = rn_vk_image_copy(vk, src, a);
    fprintf(stderr, "copytest: host image to card image -> %d\n", rc);
    if (rc) return 23;
    rc = rn_vk_image_copy(vk, a, b);
    fprintf(stderr, "copytest: card image to card image -> %d\n", rc);
    if (rc) return 24;
    if (getenv("RN_VK_COPYTEST_CLEAR")) {       // a cleared card image, and a card image written after a clear, copied into a linear image over host memory (what the scanout copy does)
        uint32_t *out = valloc(bytes);
        rn_vk_image *lin = out ? rn_vk_image_from_host(vk, RN_VK_BGRA8, w, h, out, (size_t)w * 4, bytes) : NULL;
        if (!lin) { fprintf(stderr, "copytest: no linear destination (%s)\n", rn_vk_image_last_error()); return 26; }
        memset(out, 0x55, bytes);
        const rn_vk_target t = {.image = b, .clear = 1, .color = {0.2f, 0.4f, 0.6f, 1.0f}};
        rc = rn_vk_render_pass(vk, &t, 1, NULL, 0);
        fprintf(stderr, "copytest: clear of a card image -> %d\n", rc);
        rc = rn_vk_image_copy(vk, b, lin);
        size_t badc = 0, firstc = 0;
        for (size_t i = 0; i < (size_t)w * h; i++) if (out[i] != 0xff336699u) { if (!badc) firstc = i; badc++; }
        fprintf(stderr, "copytest: cleared card image to linear host image -> %d, %zu of %zu texels wrong", rc, badc, (size_t)w * h);
        if (badc) fprintf(stderr, " (first at x %zu y %zu: %08x)", firstc % w, firstc / w, out[firstc]);
        fprintf(stderr, "\n");
        rc = rn_vk_image_copy(vk, a, b);          // the pattern over the cleared image
        if (!rc) rc = rn_vk_image_copy(vk, b, lin);
        size_t badp = 0, firstp = 0;
        for (size_t i = 0; i < (size_t)w * h; i++) if (out[i] != host[i]) { if (!badp) firstp = i; badp++; }
        fprintf(stderr, "copytest: pattern over the cleared image, then to the linear host image -> %d, %zu texels wrong", rc, badp);
        if (badp) fprintf(stderr, " (first at x %zu y %zu: %08x, expected %08x)", firstp % w, firstp / w, out[firstp], host[firstp]);
        fprintf(stderr, "\n");
        if (badc || badp) return 2;
    }
    const uint32_t *got = rn_vk_image_staging(b);
    size_t bad = 0, first = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) if (got[i] != host[i]) { if (!bad) first = i; bad++; }
    fprintf(stderr, "copytest %ux%u: %zu of %zu texels wrong", w, h, bad, (size_t)w * h);
    if (bad) fprintf(stderr, " (first at x %zu y %zu: %08x, expected %08x)", first % w, first / w, got[first], host[first]);
    fprintf(stderr, "\n");
    return bad ? 1 : 0;
}

__attribute__((visibility("default"))) int rn_vk_selftest(const char *dir) {
    char err[200] = "";
    rn_vk *vk = rn_vk_open(dir, err, sizeof(err));
    if (!vk) { fprintf(stderr, "rn_vk_open failed: %s\n", err); return 10; }
    fprintf(stderr, "rn_vk: device %s\n", rn_vk_device_name(vk));
    if (getenv("RN_VK_COPYTEST")) { unsigned w = 0, h = 0; if (sscanf(getenv("RN_VK_COPYTEST"), "%ux%u", &w, &h) != 2) { w = 1920; h = 1080; } return copytest(vk, w, h); }
    const uint32_t groups = 4, count = groups * 64, add = 7;
    rn_vk_buffer *b = rn_vk_buffer_create(vk, count * 4);
    const rn_vk_binding bind0 = {0, 1, RN_VK_BUFFER, NULL};
    rn_vk_compute *c = rn_vk_compute_create(vk, (const uint32_t *)vk_dispatch_spv, vk_dispatch_spv_len, 1, &bind0, 4, 0);
    if (!b || !c) { fprintf(stderr, "buffer or pipeline failed\n"); return 11; }
    uint32_t *p = rn_vk_buffer_contents(b);
    for (uint32_t i = 0; i < count; i++) p[i] = 0xdeadbeef;
    const rn_vk_arg arg0 = {.buffer = b};
    int rc = rn_vk_dispatch(vk, c, &arg0, &add, (const uint32_t[3]){groups, 1, 1}, NULL);
    if (rc) { fprintf(stderr, "dispatch failed: %d\n", rc); return 12; }
    uint32_t bad = 0;
    for (uint32_t i = 0; i < count; i++) if (p[i] != i * 3u + add) bad++;
    fprintf(stderr, "rn_vk_selftest: %u of %u values wrong\n", bad, count);
    rn_vk_compute_destroy(vk, c);
    rn_vk_buffer_destroy(vk, b);
    rn_vk_close(vk);
    return bad ? 1 : 0;
}
