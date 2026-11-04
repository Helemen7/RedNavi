// rn_vk_selftest(): RADV inside the calling process. The shader is mesa-bridge/test/vk_dispatch.comp (v[i] = i * 3 + push), the same one vk_dispatch runs with the Vulkan loader.
#include "rn_vk.h"
#include <stdio.h>
#include "../mesa-bridge/test/vk_dispatch_spv.h"

__attribute__((visibility("default"))) int rn_vk_selftest(const char *dir) {
    char err[200] = "";
    rn_vk *vk = rn_vk_open(dir, err, sizeof(err));
    if (!vk) { fprintf(stderr, "rn_vk_open failed: %s\n", err); return 10; }
    fprintf(stderr, "rn_vk: device %s\n", rn_vk_device_name(vk));
    const uint32_t groups = 4, count = groups * 64, add = 7;
    rn_vk_buffer *b = rn_vk_buffer_create(vk, count * 4);
    rn_vk_compute *c = rn_vk_compute_create(vk, (const uint32_t *)vk_dispatch_spv, vk_dispatch_spv_len, 1, 4);
    if (!b || !c) { fprintf(stderr, "buffer or pipeline failed\n"); return 11; }
    uint32_t *p = rn_vk_buffer_contents(b);
    for (uint32_t i = 0; i < count; i++) p[i] = 0xdeadbeef;
    int rc = rn_vk_dispatch(vk, c, &b, NULL, &add, groups, 1, 1);
    if (rc) { fprintf(stderr, "dispatch failed: %d\n", rc); return 12; }
    uint32_t bad = 0;
    for (uint32_t i = 0; i < count; i++) if (p[i] != i * 3u + add) bad++;
    fprintf(stderr, "rn_vk_selftest: %u of %u values wrong\n", bad, count);
    rn_vk_compute_destroy(vk, c);
    rn_vk_buffer_destroy(vk, b);
    rn_vk_close(vk);
    return bad ? 1 : 0;
}
