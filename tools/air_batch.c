// Runs every function of a .metallib through the plug-in's translator interface (RedNaviMTL/rn_air.c, metallib.c) and the librn_air2spirv.dylib in DIR, and counts what works and why
// the rest does not. Read-only: it writes nothing but a summary. A way to measure how much of a real shader library (the ones that ship in macOS, for instance) the translator covers.
//   tools/air_batch FILE.metallib DIR [stride [max]]     (stride n: every n-th function; max: stop after that many tried)
#include "../RedNaviMTL/metallib.h"
#include "../RedNaviMTL/rn_air.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void rn_log(const char *fmt, ...) { (void)fmt; }

typedef struct { char key[160]; unsigned n; char example[128]; } bucket;
static bucket buckets[200];
static unsigned nbuckets;

// the message with names, numbers and addresses cut out, so that the same reason groups together
static void normalise(const char *err, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = err; *p && o + 1 < n && o < 120; p++) {
        if (isdigit((unsigned char)*p)) { if (o && out[o - 1] != '#') out[o++] = '#'; continue; }
        out[o++] = *p;
    }
    out[o] = 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { puts("usage: air_batch FILE.metallib DIR [stride [max]]"); return 2; }
    const unsigned stride = argc > 3 ? (unsigned)atoi(argv[3]) : 1, max = argc > 4 ? (unsigned)atoi(argv[4]) : 1u << 30;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) return 1; fclose(f);
    // Some system libraries (QuartzCore's) are a fat file with one metallib per target: the slice is chosen by AIR_SLICE (default 0)
    if (n > 8 && d[0] == 0xca && d[1] == 0xfe && d[2] == 0xba && d[3] == 0xbe) {
        const unsigned nslices = (d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7];
        const unsigned want = getenv("AIR_SLICE") ? (unsigned)atoi(getenv("AIR_SLICE")) : 0;
        const unsigned char *base = d;
        for (unsigned i = 0; i < nslices && i < 32; i++) {
            const unsigned char *e = base + 8 + 20 * i;
            const unsigned off = (e[8] << 24) | (e[9] << 16) | (e[10] << 8) | e[11], sz = (e[12] << 24) | (e[13] << 16) | (e[14] << 8) | e[15];
            printf("slice %u: cpu type %02x%02x%02x%02x subtype %02x%02x%02x%02x, offset %u, size %u%s\n", i, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], off, sz, i == want ? "  <- used" : "");
            if (i == want) { d = (unsigned char *)base + off; n = sz; }
        }
    }
    unsigned total = 0;
    rn_mtl_func *fn = malloc(sizeof(*fn) * 200000);
    if (rn_metallib_parse(d, n, fn, 200000, &total)) { puts("not a metallib"); return 1; }
    unsigned bytype[3] = {0, 0, 0}, ok[3] = {0, 0, 0}, tried = 0;
    for (unsigned i = 0; i < total; i++) if (fn[i].type < 3) bytype[fn[i].type]++;
    printf("%s: %u functions (%u vertex, %u fragment, %u kernel)\n", argv[1], total, bytype[0], bytype[1], bytype[2]);
    for (unsigned i = 0; i < total && tried < max; i += stride) {
        const unsigned t = fn[i].type < 3 ? fn[i].type : 2;
        rn_air_kernel_info info; memset(&info, 0, sizeof(info));
        uint32_t *spv = NULL; size_t bytes = 0; char err[400] = "";
        const int stage = t == 0 ? RN_AIR_VERTEX : t == 1 ? RN_AIR_FRAGMENT : RN_AIR_KERNEL;
        const int rc = rn_air_translate_stage(argv[2], stage, d + fn[i].bitcode_off, fn[i].bitcode_size, fn[i].name, &info, &spv, &bytes, err, sizeof(err));
        tried++;
        if (!rc) { ok[t]++; rn_air_free(spv, bytes); continue; }
        char key[160]; normalise(err, key, sizeof(key));
        unsigned b = 0;
        while (b < nbuckets && strcmp(buckets[b].key, key)) b++;
        if (b == nbuckets && nbuckets < 200) { snprintf(buckets[b].key, sizeof(buckets[b].key), "%s", key); snprintf(buckets[b].example, sizeof(buckets[b].example), "%s: %.90s", fn[i].name, err); nbuckets++; }
        if (b < nbuckets) buckets[b].n++;
    }
    printf("tried %u: translated %u vertex, %u fragment, %u kernel\n", tried, ok[0], ok[1], ok[2]);
    for (unsigned b = 0; b < nbuckets; b++) printf("  %5u x %s\n         e.g. %s\n", buckets[b].n, buckets[b].key, buckets[b].example);
    return 0;
}
