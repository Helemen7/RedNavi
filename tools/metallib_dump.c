// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Lists the functions of a .metallib with RedNaviMTL/metallib.c: tools/metallib_dump FILE...
#include "../RedNaviMTL/metallib.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    int bad = 0;
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { perror(argv[a]); bad++; continue; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) { bad++; fclose(f); continue; } fclose(f);
        rn_mtl_func fn[256]; unsigned cnt = 0;
        int rc = rn_metallib_parse(d, n, fn, 256, &cnt);
        printf("%s: %ld bytes, rc %d, %u functions\n", argv[a], n, rc, cnt);
        if (rc) bad++;
        for (unsigned i = 0; i < cnt && i < 256; i++) printf("  type %u  %-40s bitcode %llu bytes at 0x%llx\n", fn[i].type, fn[i].name, (unsigned long long)fn[i].bitcode_size, (unsigned long long)fn[i].bitcode_off);
        free(d);
    }
    return bad ? 1 : 0;
}
