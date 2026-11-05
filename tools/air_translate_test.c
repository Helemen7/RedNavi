// Takes one kernel of a .metallib through the plug-in's translator interface (RedNaviMTL/rn_air.c, metallib.c) and the librn_air2spirv.dylib in DIR, and writes the SPIR-V:
//   tools/air_translate_test FILE.metallib FUNCTION DIR OUT.spv      (METAL2VULKAN_LLVM_DIS and METAL2VULKAN_SPIRV_VAL may point at the tools)
#include "../RedNaviMTL/metallib.h"
#include "../RedNaviMTL/rn_air.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc < 5) { puts("usage: air_translate_test FILE.metallib FUNCTION DIR OUT.spv"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) return 1; fclose(f);
    rn_mtl_func fn[256]; unsigned cnt = 0;
    if (rn_metallib_parse(d, n, fn, 256, &cnt)) { puts("not a metallib"); return 1; }
    for (unsigned i = 0; i < cnt; i++) {
        if (strcmp(fn[i].name, argv[2])) continue;
        rn_air_kernel_info info; memset(&info, 0, sizeof(info));
        uint32_t *spv = NULL; size_t bytes = 0; char err[400] = "";
        int rc = rn_air_translate(argv[3], d + fn[i].bitcode_off, fn[i].bitcode_size, fn[i].name, &info, &spv, &bytes, err, sizeof(err));
        printf("rn_air_translate -> %d %s\n", rc, err);
        if (rc) return 1;
        printf("SPIR-V %zu bytes, %u bindings, local size %u %u %u (spec ids %u %u %u)\n", bytes, info.binding_count, info.local_size[0], info.local_size[1], info.local_size[2],
               info.local_size_spec_ids[0], info.local_size_spec_ids[1], info.local_size_spec_ids[2]);
        for (unsigned b = 0; b < info.binding_count; b++) printf("  Metal %s(%u) -> binding %u, %s, dim %u\n", (const char *[]){"buffer", "texture", "storage image", "sampler", "static sampler"}[info.bindings[b].kind % 5], info.bindings[b].metal_index, info.bindings[b].binding, info.bindings[b].writable ? "writable" : "read-only", info.bindings[b].dim);
        FILE *o = fopen(argv[4], "wb"); fwrite(spv, 1, bytes, o); fclose(o);
        rn_air_free(spv, bytes);
        return 0;
    }
    puts("function not found");
    return 1;
}
