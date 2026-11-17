// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

/*
 * Replays one draw of a WindowServer pass capture (NNN_dK.bin with NNN_dK_v.spv and NNN_dK_f.spv, written by RedNaviMTL's pass dump for the analytic border and disc pipelines) through the
 * same Vulkan layer the plug-in uses (RedNaviMTL/rn_vk.c), alone on a cleared target, and prints the alpha it produces in the draw's rectangle. It tells a wrong shape that comes out of the
 * translated shader and its inputs from one that comes from the run-time state of a pass.
 *
 *   clang -O1 -arch x86_64 -mmacosx-version-min=12 -I/opt/local/include -IRedNaviMTL tools/rn_replay.c RedNaviMTL/rn_vk.c RedNaviMTL/rn_vk_selftest.c -o rn_replay
 *   ./rn_replay <capture dir> <NNN_dK> <vk dir> [blend 0|1]       (the vk dir holds libvulkan_radeon.dylib)
 *
 * Verified on hardware: not yet. The static samplers of the dumped pipelines are replaced by a linear clamp sampler (their state is not in the dump).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rn_vk.h"

typedef struct { uint32_t stage, index, len; const uint8_t *data; } Buf;
static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n + 1); if (fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; }
    fclose(f); return b;
}

static rn_vk *g_vk; static rn_vk_image *g_tgt;   // shared by the draws of a list: the same target, drawn into one pass after the other

static int one(char **argv, const char *name, int blend, int last) {
    argv[2] = (char *)name;
    char path[512], err[256] = "";
    size_t nb, nv, nf;
    snprintf(path, sizeof(path), "%s/%s.bin", argv[1], argv[2]); uint8_t *bin = slurp(path, &nb);
    snprintf(path, sizeof(path), "%s/%s_v.spv", argv[1], argv[2]); uint8_t *vs = slurp(path, &nv);
    snprintf(path, sizeof(path), "%s/%s_f.spv", argv[1], argv[2]); uint8_t *fs = slurp(path, &nf);
    if (!bin || !vs || !fs) { fprintf(stderr, "cannot read the capture files\n"); return 2; }
    const uint32_t *h = (const uint32_t *)bin;
    if (h[0] != 0x31444e52u) { fprintf(stderr, "bad magic\n"); return 2; }
    const uint32_t verts = h[1], insts = h[2];
    const int x0 = (int)h[4], y0 = (int)h[5], x1 = (int)h[6], y1 = (int)h[7];
    const uint32_t tw = h[8], th = h[9], nbufs = h[10];
    Buf bufs[32]; const uint8_t *p = bin + 48;
    for (uint32_t i = 0; i < nbufs && i < 32; i++) { uint32_t *q = (uint32_t *)p; bufs[i] = (Buf){q[0], q[1], q[2], p + 12}; p += 12 + q[2]; }
    uint32_t nbind[2]; rn_vk_binding binds[2][32]; uint32_t mi[2][32];
    static rn_vk_sampler_desc lin = {.min_filter = 1, .mag_filter = 1, .address = {1, 1, 1}, .lod_max = 1000};
    if (getenv("RN_PX")) { lin.min_filter = lin.mag_filter = 0; lin.address[0] = lin.address[1] = lin.address[2] = 0; lin.pixel_coords = 1; }   // the sampler of the dumped Metal pipelines: nearest, clamp to zero, unnormalized coordinates
    for (int s = 0; s < 2; s++) {
        nbind[s] = *(const uint32_t *)p; p += 4;
        for (uint32_t i = 0; i < nbind[s] && i < 32; i++) {
            const uint32_t *q = (const uint32_t *)p; p += 12;
            binds[s][i] = (rn_vk_binding){q[0], 0, q[1], q[1] == RN_VK_STATIC_SAMPLER ? &lin : NULL}; mi[s][i] = q[2];
        }
    }
    uint32_t nva = 0, nvl = 0; rn_vk_vattr va_[16]; rn_vk_vlayout vl_[16];
    if (p + 4 > bin + nb) {         // an older dump without the vertex input: the fixed-function vertex shader takes float4, float2, float2 and an RGBA8 unorm colour (locations 0 to 3; the bytes 28 32 32 ff at offset 32 of the first vertex read as a colour) from Metal buffer 1, stride 48
        nva = 4; nvl = 1;
        va_[0] = (rn_vk_vattr){0, 1, 109, 0}; va_[1] = (rn_vk_vattr){1, 1, 103, 16}; va_[2] = (rn_vk_vattr){2, 1, 103, 24}; va_[3] = (rn_vk_vattr){3, 1, 37, 32};
        vl_[0] = (rn_vk_vlayout){1, 48, 0};
    } else {
    nva = *(const uint32_t *)p; p += 4;
    for (uint32_t i = 0; i < nva && i < 16; i++, p += 16) { const uint32_t *q = (const uint32_t *)p; va_[i] = (rn_vk_vattr){q[0], q[1], q[2], q[3]}; }
    nvl = *(const uint32_t *)p; p += 4;
    for (uint32_t i = 0; i < nvl && i < 16; i++, p += 12) { const uint32_t *q = (const uint32_t *)p; vl_[i] = (rn_vk_vlayout){q[0], q[1], q[2]}; }
    }
    for (uint32_t i = 0; i < nva; i++) printf("  attribute location %u binding %u VkFormat %u offset %u\n", va_[i].location, va_[i].binding, va_[i].format, va_[i].offset);
    for (uint32_t i = 0; i < nvl; i++) printf("  layout binding %u stride %u per instance %u\n", vl_[i].binding, vl_[i].stride, vl_[i].per_instance);
    double vport[6] = {0, 0, (double)tw, (double)th, 0, 1}; uint32_t scis[4] = {(uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}; uint32_t dst[12] = {0}; int have_state = 0;
    if (p + 6 * 8 + 4 * 4 + 12 * 4 <= bin + nb) { memcpy(vport, p, 48); p += 48; memcpy(scis, p, 16); p += 16; memcpy(dst, p, 48); p += 48; have_state = 1; }
    uint32_t indexed = 0, idx32 = 0, first_vertex = 0, idx_bytes = 0; int32_t base_vertex = 0; const uint8_t *idx_data = NULL;
    if (have_state && p + 20 <= bin + nb) {   // the indexing: indexed, 32-bit, base vertex, first vertex, index bytes, then the indices
        const uint32_t *q = (const uint32_t *)p; indexed = q[0]; idx32 = q[1]; base_vertex = (int32_t)q[2]; first_vertex = q[3]; idx_bytes = q[4]; p += 20; idx_data = p;
        printf("  indexed %u, %s indices, base vertex %d, first vertex %u, %u index bytes\n", indexed, idx32 ? "32-bit" : "16-bit", base_vertex, first_vertex, idx_bytes);
    }
    rn_vk *vk = g_vk ? g_vk : rn_vk_open(argv[3], err, sizeof(err)); g_vk = vk;
    if (!vk) { fprintf(stderr, "rn_vk_open: %s\n", err); return 2; }
    printf("%s: %u vertices x %u, rect (%d,%d)-(%d,%d) of %ux%u, device %s\n", argv[2], verts, insts, x0, y0, x1, y1, tw, th, rn_vk_device_name(vk));
    rn_vk_render_desc rd; memset(&rd, 0, sizeof(rd));
    rd.vs = (const uint32_t *)vs; rd.vs_bytes = nv; rd.fs = (const uint32_t *)fs; rd.fs_bytes = nf;
    rd.nvbind = nbind[0]; rd.vbinds = binds[0]; rd.nfbind = nbind[1]; rd.fbinds = binds[1];
    rd.nvattr = nva; rd.vattrs = va_; rd.nvlayout = nvl; rd.vlayouts = vl_;
    rd.ntargets = 1; rd.target_formats[0] = RN_VK_BGRA8; rd.topology = 3; rd.front_clockwise = 1;
    rd.blend[0].write_mask = 15;
    if (have_state && !blend) { rd.blend[0].enabled = (int)dst[0]; rd.blend[0].src_rgb = dst[1]; rd.blend[0].dst_rgb = dst[2]; rd.blend[0].op_rgb = dst[3]; rd.blend[0].src_a = dst[4]; rd.blend[0].dst_a = dst[5]; rd.blend[0].op_a = dst[6]; rd.blend[0].write_mask = dst[7]; rd.cull_mode = dst[9]; rd.front_clockwise = (int)dst[10]; }
    if (blend) { rd.blend[0].enabled = 1; rd.blend[0].src_rgb = 1; rd.blend[0].dst_rgb = 7; rd.blend[0].src_a = 1; rd.blend[0].dst_a = 7; }   // VK_BLEND_FACTOR_ONE, ONE_MINUS_SRC_ALPHA
    rn_vk_render *pipe = rn_vk_render_create(vk, &rd);
    if (!pipe) { fprintf(stderr, "RADV refused the pipeline\n"); return 3; }
    rn_vk_arg va[32], fa[32]; memset(va, 0, sizeof(va)); memset(fa, 0, sizeof(fa));
    const int first_draw = !g_tgt; rn_vk_image *tgt = g_tgt ? g_tgt : rn_vk_image_create(vk, RN_VK_BGRA8, tw, th); g_tgt = tgt;
    rn_vk_image *zero = rn_vk_image_create(vk, RN_VK_RGBA8, 1, 1); memset(rn_vk_image_staging(zero), 0, 4); rn_vk_image_host_wrote(zero);
    rn_vk_sampler *smp = rn_vk_sampler_create(vk, &lin);
    for (int s = 0; s < 2; s++) for (uint32_t i = 0; i < nbind[s]; i++) {
        rn_vk_arg *a = s ? &fa[i] : &va[i];
        const uint32_t kind = binds[s][i].kind;
        if (kind == RN_VK_BUFFER) {
            for (uint32_t j = 0; j < nbufs; j++) if (bufs[j].stage == (uint32_t)s && bufs[j].index == mi[s][i]) {
                rn_vk_buffer *b = rn_vk_buffer_create(vk, bufs[j].len); memcpy(rn_vk_buffer_contents(b), bufs[j].data, bufs[j].len); a->buffer = b;
            }
            if (!a->buffer) { fprintf(stderr, "no buffer for stage %d index %u\n", s, mi[s][i]); return 3; }
        } else if (kind == RN_VK_SAMPLED_IMAGE && getenv("RN_TEX")) {   // RN_TEX=<raw file of one byte per texel>, RN_TEXW x RN_TEXH, bound as an A8 texture
            static rn_vk_image *timg; size_t tn; uint8_t *tb = timg ? NULL : slurp(getenv("RN_TEX"), &tn);
            if (!timg) { const uint32_t w_ = (uint32_t)atoi(getenv("RN_TEXW")), h_ = (uint32_t)atoi(getenv("RN_TEXH")); timg = rn_vk_image_create(vk, RN_VK_A8, w_, h_); if (tb) memcpy(rn_vk_image_staging(timg), tb, (size_t)w_ * h_); rn_vk_image_host_wrote(timg); }
            a->image = timg;
        } else if (kind == RN_VK_SAMPLED_IMAGE || kind == RN_VK_STORAGE_IMAGE) a->image = zero;
        else if (kind == RN_VK_SAMPLER) a->sampler = smp;
        else if (kind == RN_VK_INPUT_ATTACHMENT) a->image = tgt;      // framebuffer fetch reads the render target
    }
    rn_vk_target t = {.image = tgt, .clear = first_draw, .color = {0, 0, 0, 0}}; const int bg = getenv("RN_CLEAR") != NULL; if (bg) { t.color[0] = t.color[1] = t.color[2] = 0.5f; t.color[3] = 1; }
    rn_vk_draw d; memset(&d, 0, sizeof(d));
    d.pipe = pipe; d.vargs = va; d.fargs = fa; d.has_scissor = !getenv("RN_NOSCISSOR"); d.has_viewport = have_state; memcpy(d.viewport, vport, 48); if (have_state) memcpy(d.scissor, scis, 16); d.scissor[0] = (uint32_t)x0; d.scissor[1] = (uint32_t)y0; d.scissor[2] = (uint32_t)(x1 - x0); d.scissor[3] = (uint32_t)(y1 - y0);
    d.vertex_count = verts; d.instance_count = insts ? insts : 1; d.first_vertex = first_vertex;
    if (indexed && idx_bytes) {
        rn_vk_buffer *ib = rn_vk_buffer_create(vk, idx_bytes); memcpy(rn_vk_buffer_contents(ib), idx_data, idx_bytes);
        d.index_buffer = ib; d.index_offset = 0; d.index32 = (int)idx32; d.base_vertex = base_vertex;
        if (verts * (idx32 ? 4 : 2) > idx_bytes) printf("  note: the dump holds %u of %u index bytes\n", idx_bytes, verts * (idx32 ? 4 : 2));
    }
    rn_vk_vbuf vb[16]; memset(vb, 0, sizeof(vb)); d.vbufs = vb;
    for (uint32_t i = 0; i < nvl; i++) for (uint32_t j = 0; j < nbufs; j++) if (bufs[j].stage == 0 && bufs[j].index == vl_[i].binding) {
        rn_vk_buffer *b = rn_vk_buffer_create(vk, bufs[j].len); memcpy(rn_vk_buffer_contents(b), bufs[j].data, bufs[j].len); vb[vl_[i].binding].buffer = b; if (getenv("RN_VOFF")) vb[vl_[i].binding].offset = (uint64_t)atoi(getenv("RN_VOFF"));   // bind the buffer at an offset, as a pass does when its draws share one buffer
    }
    int rc;
    if (getenv("RN_ONEPASS")) {   // the draws of the list as one render pass over one shared vertex buffer, each at its own offset (the first draw's buffer, 96 bytes further for each draw), as WindowServer encodes the nine slices of a shadow
        static rn_vk_draw gd[16]; static rn_vk_arg gva[16][32], gfa[16][32]; static rn_vk_vbuf gvb[16][16]; static rn_vk_buffer *shared; static unsigned gn; static rn_vk_render *gpipe;
        if (!shared) for (uint32_t i = 0; i < nvl; i++) for (uint32_t j = 0; j < nbufs; j++) if (bufs[j].stage == 0 && bufs[j].index == vl_[i].binding) { shared = rn_vk_buffer_create(vk, bufs[j].len); memcpy(rn_vk_buffer_contents(shared), bufs[j].data, bufs[j].len); }
        if (!gpipe) gpipe = pipe;
        memcpy(gva[gn], va, sizeof(va)); memcpy(gfa[gn], fa, sizeof(fa));
        for (uint32_t i = 0; i < nvl; i++) { gvb[gn][vl_[i].binding].buffer = shared; gvb[gn][vl_[i].binding].offset = (uint64_t)gn * verts * vl_[i].stride;
            if (getenv("RN_FIRSTV")) { gvb[gn][vl_[i].binding].offset = 0; d.first_vertex = gn * verts; }          // the same slices by first vertex: one binding, no offsets
            if (getenv("RN_SEPBUF")) { rn_vk_buffer *sb = rn_vk_buffer_create(vk, verts * vl_[i].stride); memcpy(rn_vk_buffer_contents(sb), (uint8_t *)rn_vk_buffer_contents(shared) + (uint64_t)gn * verts * vl_[i].stride, verts * vl_[i].stride); gvb[gn][vl_[i].binding] = (rn_vk_vbuf){sb, 0}; } }
        gd[gn] = d; gd[gn].pipe = gpipe; gd[gn].vargs = gva[gn]; gd[gn].fargs = gfa[gn]; gd[gn].vbufs = gvb[gn]; gn++;
        if (!last) return 0;
        rc = rn_vk_render_pass(vk, &t, 1, gd, gn);
    } else
    rc = rn_vk_render_pass(vk, &t, 1, &d, 1);
    printf("render pass: %d\n", rc);
    if (!last) return rc;
    const uint8_t *px = rn_vk_image_staging(tgt);
    {   // anywhere on the target: pixels written, their bounding box and the largest value of each channel
        unsigned cnt = 0, mx[4] = {0}; int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
        for (uint32_t y = 0; y < th; y++) for (uint32_t x = 0; x < tw; x++) {
            const uint8_t *q = px + 4 * ((size_t)y * tw + x);
            if (!(q[0] | q[1] | q[2] | q[3])) continue;
            cnt++; for (int c = 0; c < 4; c++) if (q[c] > mx[c]) mx[c] = q[c];
            if ((int)x < bx0) bx0 = (int)x; if ((int)y < by0) by0 = (int)y; if ((int)x > bx1) bx1 = (int)x; if ((int)y > by1) by1 = (int)y;
        }
        printf("pixels written %u, box (%d,%d)-(%d,%d), max B %u G %u R %u A %u\n", cnt, bx0, by0, bx1, by1, mx[0], mx[1], mx[2], mx[3]);
    }
    // the alpha of the rectangle: one character per pixel, " .:-=+*#%@" by coverage; the whole rectangle if it is at most 120 x 40, else its top-left corner
    const char *ramp = " .:-=+*#%@";
    int w = x1 - x0, hh = y1 - y0; if (w > 120) w = 120; if (hh > 40) hh = 40;
    unsigned nz = 0, partial = 0;
    for (int y = 0; y < hh; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t *q = px + 4 * ((size_t)(y0 + y) * tw + (x0 + x));
            unsigned a = q[3];
            if (bg) { unsigned dm = 0; for (int c = 0; c < 4; c++) { const int dv = abs((int)q[c] - (c == 3 ? 255 : 128)); if ((unsigned)dv > dm) dm = (unsigned)dv; } a = dm > 255 ? 255 : dm; }   // with a background: how far the pixel is from it
            nz += a != 0; partial += a != 0 && a != 255;
            if (getenv("RN_NUM")) printf("%3u ", a); else putchar(ramp[a * 9 / 255]);
        }
        putchar('\n');
    }
    printf("covered %u, partly covered %u of %d x %d\n", nz, partial, w, hh);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: rn_replay <capture dir> <NNN_dK>[,<NNN_dK>...] <vk dir> [blend 0|1]\n"); return 2; }
    const int blend = argc > 4 ? atoi(argv[4]) : 0;
    char *list = strdup(argv[2]), *save = NULL, *names[64]; int n = 0;
    for (char *t = strtok_r(list, ",", &save); t && n < 64; t = strtok_r(NULL, ",", &save)) names[n++] = t;
    int rc = 0;
    for (int i = 0; i < n; i++) rc = one(argv, names[i], blend, i == n - 1);
    return rc;
}
