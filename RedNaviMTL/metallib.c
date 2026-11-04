#include "metallib.h"
#include <string.h>

static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

int rn_metallib_parse(const uint8_t *d, size_t n, rn_mtl_func *out, unsigned max, unsigned *count) {
    if (count) *count = 0;
    if (n < 0x58 || memcmp(d, "MTLB", 4)) return 1;
    const uint64_t fl_off = rd64(d + 0x18), fl_size = rd64(d + 0x20), bc_off = rd64(d + 0x48), bc_size = rd64(d + 0x50);
    if (fl_off > n || fl_size > n - fl_off || fl_size < 1 || bc_off > n || bc_size > n - bc_off) return 2;
    // the size at 0x20 counts the bytes after the u32 count
    if (fl_size > n - fl_off - 4) return 2;
    const uint32_t cnt = rd32(d + fl_off);
    const uint8_t *p = d + fl_off + 4, *fl_end = p + fl_size;
    unsigned found = 0;
    for (uint32_t f = 0; f < cnt; f++) {
        if (fl_end - p < 4) return 3;
        const uint32_t sz = rd32(p);
        if (sz < 8 || (uint64_t)(fl_end - p) < sz) return 3;
        const uint8_t *q = p + 4, *e = p + sz;
        rn_mtl_func fn;
        memset(&fn, 0, sizeof(fn));
        uint64_t offt[3] = {0, 0, 0};
        int have_name = 0, have_offt = 0, have_size = 0;
        while (e - q >= 4) {
            if (!memcmp(q, "ENDT", 4)) break;
            if (e - q < 6) return 3;
            const uint32_t len = rd16(q + 4);
            const uint8_t *data = q + 6;
            if ((uint64_t)(e - data) < len) return 3;
            if (!memcmp(q, "NAME", 4) && len > 0) { size_t l = len < sizeof(fn.name) ? len : sizeof(fn.name) - 1; memcpy(fn.name, data, l); fn.name[l] = 0; have_name = 1; }
            else if (!memcmp(q, "TYPE", 4) && len >= 1) fn.type = data[0];
            else if (!memcmp(q, "OFFT", 4) && len >= 24) { for (int i = 0; i < 3; i++) offt[i] = rd64(data + 8 * i); have_offt = 1; }
            else if (!memcmp(q, "MDSZ", 4) && len >= 8) { fn.bitcode_size = rd64(data); have_size = 1; }
            q = data + len;
        }
        if (!have_name || !have_offt || !have_size) return 4;
        fn.bitcode_off = bc_off + offt[2];
        if (fn.bitcode_off > n || fn.bitcode_size > n - fn.bitcode_off || fn.bitcode_size < 4) return 5;
        // LLVM bitcode, plain ("BC" 0xC0DE) or in the wrapper (0x0B17C0DE)
        const uint8_t *bc = d + fn.bitcode_off;
        const int plain = bc[0] == 'B' && bc[1] == 'C' && bc[2] == 0xC0 && bc[3] == 0xDE;
        const int wrapped = bc[0] == 0xDE && bc[1] == 0xC0 && bc[2] == 0x17 && bc[3] == 0x0B;
        if (!plain && !wrapped) return 6;
        if (found < max && out) out[found] = fn;
        found++;
        p += sz;
    }
    if (count) *count = found;
    return 0;
}
