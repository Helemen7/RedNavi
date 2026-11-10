// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Read-only: prints the C strings of a loaded system framework that contain one of the keywords.
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
int main(int argc, char **argv) {
    const char *path = argv[1];
    void *h = dlopen(path, RTLD_LAZY);
    if (!h) { fprintf(stderr, "cannot load\n"); return 1; }
    const char *last = strrchr(path, '/');
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *n = _dyld_get_image_name(i);
        const char *l = n ? strrchr(n, '/') : NULL;
        if (!l || strcmp(l, last)) continue;
        const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
        unsigned long sz = 0;
        const char *p = (const char *)getsectiondata(mh, "__TEXT", "__cstring", &sz);
        for (unsigned long o = 0; p && o < sz;) {
            const char *s = p + o; size_t len = strlen(s);
            for (int k = 2; k < argc; k++) if (len > 3 && len < 300 && strcasestr(s, argv[k])) { puts(s); break; }
            o += len + 1;
        }
        break;
    }
    return 0;
}
