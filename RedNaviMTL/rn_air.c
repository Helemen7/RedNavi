#include "rn_air.h"
#include "rn_vk.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef int (*translate_fn)(const void *, size_t, const char *, rn_air_kernel_info *, uint32_t **, size_t *, char *, size_t);
typedef void (*free_fn)(void *);

static void *g_lib;
static translate_fn g_translate;
static free_fn g_free;

static int load(const char *dir, char *err, size_t errlen) {
    if (g_translate) return 0;
    if (!dir) dir = rn_vk_dir();
    char path[1024];
    snprintf(path, sizeof(path), "%s/librn_air2spirv.dylib", dir);
    g_lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) { snprintf(err, errlen, "no AIR translator: %s", dlerror()); return 1; }
    g_translate = (translate_fn)dlsym(g_lib, "rn_air_translate");
    g_free = (free_fn)dlsym(g_lib, "rn_air_free");
    if (!g_translate || !g_free) { snprintf(err, errlen, "%s does not export rn_air_translate and rn_air_free", path); g_translate = NULL; return 1; }
    return 0;
}

int rn_air_translate(const char *dir, const void *bitcode, size_t bytes, const char *entry, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen) {
    if (load(dir, err, errlen)) return 1;
    return g_translate(bitcode, bytes, entry, info, spirv, spirv_bytes, err, errlen);
}

void rn_air_free(uint32_t *spirv) { if (g_free) g_free(spirv); }
