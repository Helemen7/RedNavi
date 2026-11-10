// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#include "rn_air.h"
#include "rn_vk.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef void (*free_fn)(void *, size_t);

static void *g_lib;
typedef int (*translate_stage_fn)(unsigned, const void *, size_t, const char *, rn_air_kernel_info *, uint32_t **, size_t *, char *, size_t);
static translate_stage_fn g_translate;
typedef int (*translate_fc_fn)(unsigned, const void *, size_t, const char *, const rn_air_const *, unsigned, rn_air_kernel_info *, uint32_t **, size_t *, char *, size_t);
static translate_fc_fn g_translate_fc;
static free_fn g_free;

static int load(const char *dir, char *err, size_t errlen) {
    if (g_translate) return 0;
    if (!dir) dir = rn_vk_dir();
    char path[1024];
    snprintf(path, sizeof(path), "%s/librn_air2spirv.dylib", dir);
    // The translator runs llvm-dis and spirv-val from DIR; they find their libraries (libLLVM, libSPIRV-Tools) there only through the library path, which a child process takes from the
    // environment (inside a process that did not start with it, such as WindowServer, it is only for the children).
    {
        const char *old = getenv("DYLD_LIBRARY_PATH");
        char lp[2048];
        snprintf(lp, sizeof(lp), "%s%s%s", dir, old && *old ? ":" : "", old && *old ? old : "");
        setenv("DYLD_LIBRARY_PATH", lp, 1);
    }
    g_lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) { snprintf(err, errlen, "no AIR translator: %s", dlerror()); return 1; }
    // tools next to the library, if any
    static const char *const tools[][2] = {{"METAL2VULKAN_LLVM_DIS", "llvm-dis"}, {"METAL2VULKAN_SPIRV_VAL", "spirv-val"}};
    for (unsigned i = 0; i < 2; i++) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s/%s", dir, tools[i][1]);
        if (!getenv(tools[i][0]) && access(tp, X_OK) == 0) setenv(tools[i][0], tp, 0);
    }
    g_translate = (translate_stage_fn)dlsym(g_lib, "rn_air_translate_stage");
    g_translate_fc = (translate_fc_fn)dlsym(g_lib, "rn_air_translate_stage_fc");
    g_free = (free_fn)dlsym(g_lib, "rn_air_free");
    if (!g_translate || !g_free) { snprintf(err, errlen, "%s does not export rn_air_translate_stage and rn_air_free", path); g_translate = NULL; return 1; }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------- translation cache
// Translating one function takes seconds (QuartzCore asks for a hundred specializations when WindowServer starts), so the result is kept on disk, keyed by a hash of everything that
// goes into it: the translator library (size and time), the stage, the entry, the bitcode and the function constants. Only successes are kept. The directory is the user's own cache
// directory (confstr), mode 0700, and is used only if it belongs to this user and nobody else can write it: SPIR-V from there goes to the GPU. RN_AIR_NO_CACHE=1 turns it off.
#define RN_CACHE_MAGIC 0x31434e52u            // "RNC1"
typedef struct { uint32_t magic; uint32_t info_size; uint64_t spirv_bytes; uint64_t key; } rn_cache_head;

static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; return h; }

static const char *cache_dir(void) {
    static char dir[1024];
    static int state;                              // 0 not tried, 1 usable, -1 not
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (!state) {
        state = -1;
        char base[900];
        if (!getenv("RN_AIR_NO_CACHE") && confstr(_CS_DARWIN_USER_CACHE_DIR, base, sizeof(base))) {
            snprintf(dir, sizeof(dir), "%sorg.rednavi.metal", base);
            mkdir(dir, 0700);
            struct stat st;
            if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 022)) state = 1;
        }
    }
    const int ok = state > 0;
    pthread_mutex_unlock(&mu);
    return ok ? dir : NULL;
}

static uint64_t lib_stamp;
static uint64_t cache_key(const char *dir, int stage, const void *bitcode, size_t bytes, const char *entry, const rn_air_const *consts, unsigned nconsts) {
    if (!lib_stamp) {
        struct stat st; char path[1100];
        snprintf(path, sizeof(path), "%s/librn_air2spirv.dylib", dir ? dir : rn_vk_dir());
        lib_stamp = stat(path, &st) == 0 ? ((uint64_t)st.st_size << 20) ^ (uint64_t)st.st_mtimespec.tv_sec : 1;
    }
    uint64_t h = 14695981039346656037ull;
    h = fnv(h, &lib_stamp, sizeof(lib_stamp));
    h = fnv(h, &stage, sizeof(stage));
    h = fnv(h, entry, strlen(entry) + 1);
    h = fnv(h, bitcode, bytes);
    for (unsigned i = 0; i < nconsts; i++) {
        h = fnv(h, &consts[i].index, sizeof(int32_t));
        if (consts[i].name) h = fnv(h, consts[i].name, strlen(consts[i].name) + 1);
        h = fnv(h, &consts[i].size, sizeof(uint32_t));
        h = fnv(h, consts[i].data, consts[i].size);
    }
    return h;
}

// Buffers handed out from the cache are malloc'd here, not by the translator: rn_air_free tells them apart.
static void **g_cached; static size_t g_ncached, g_capcached;
static pthread_mutex_t g_cmu = PTHREAD_MUTEX_INITIALIZER;
static void remember(void *p) {
    pthread_mutex_lock(&g_cmu);
    if (g_ncached == g_capcached) { g_capcached = g_capcached ? g_capcached * 2 : 64; g_cached = realloc(g_cached, g_capcached * sizeof(void *)); }
    g_cached[g_ncached++] = p;
    pthread_mutex_unlock(&g_cmu);
}
static int forget(void *p) {
    int found = 0;
    pthread_mutex_lock(&g_cmu);
    for (size_t i = 0; i < g_ncached; i++) if (g_cached[i] == p) { g_cached[i] = g_cached[--g_ncached]; found = 1; break; }
    pthread_mutex_unlock(&g_cmu);
    return found;
}

static int cache_load(const char *cdir, uint64_t key, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/%016llx.spv", cdir, (unsigned long long)key);
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 0;
    rn_cache_head h;
    int ok = 0;
    uint32_t *buf = NULL;
    if (read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) && h.magic == RN_CACHE_MAGIC && h.key == key && h.info_size == sizeof(*info) && h.spirv_bytes && h.spirv_bytes < (64u << 20) && !(h.spirv_bytes & 3)) {
        buf = malloc(h.spirv_bytes);
        if (buf && read(fd, info, sizeof(*info)) == (ssize_t)sizeof(*info) && read(fd, buf, h.spirv_bytes) == (ssize_t)h.spirv_bytes && buf[0] == 0x07230203u) ok = 1;
    }
    close(fd);
    if (!ok) { free(buf); return 0; }
    remember(buf);
    *spirv = buf; *spirv_bytes = h.spirv_bytes;
    return 1;
}

static void cache_store(const char *cdir, uint64_t key, const rn_air_kernel_info *info, const uint32_t *spirv, size_t spirv_bytes) {
    char path[1200], tmp[1260];
    snprintf(path, sizeof(path), "%s/%016llx.spv", cdir, (unsigned long long)key);
    snprintf(tmp, sizeof(tmp), "%s.%d.%p", path, (int)getpid(), (void *)&path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    rn_cache_head h = {RN_CACHE_MAGIC, (uint32_t)sizeof(*info), spirv_bytes, key};
    int ok = write(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) && write(fd, info, sizeof(*info)) == (ssize_t)sizeof(*info) && write(fd, spirv, spirv_bytes) == (ssize_t)spirv_bytes;
    close(fd);
    if (ok) rename(tmp, path); else unlink(tmp);
}

int rn_air_translate_stage(const char *dir, int stage, const void *bitcode, size_t bytes, const char *entry, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen) {
    return rn_air_translate_stage_fc(dir, stage, bitcode, bytes, entry, NULL, 0, info, spirv, spirv_bytes, err, errlen);
}

int rn_air_translate_stage_fc(const char *dir, int stage, const void *bitcode, size_t bytes, const char *entry, const rn_air_const *consts, unsigned nconsts, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen) {
    if (load(dir, err, errlen)) return 1;
    if (nconsts && !g_translate_fc) { snprintf(err, errlen, "the AIR translator library has no rn_air_translate_stage_fc (function constants)"); return 1; }
    const char *cdir = cache_dir();
    uint64_t key = 0;
    if (cdir) {
        key = cache_key(dir, stage, bitcode, bytes, entry, consts, nconsts);
        if (cache_load(cdir, key, info, spirv, spirv_bytes)) return 0;
    }
    const int rc = nconsts ? g_translate_fc((unsigned)stage, bitcode, bytes, entry, consts, nconsts, info, spirv, spirv_bytes, err, errlen)
                           : g_translate((unsigned)stage, bitcode, bytes, entry, info, spirv, spirv_bytes, err, errlen);
    if (!rc && cdir) cache_store(cdir, key, info, *spirv, *spirv_bytes);
    return rc;
}

int rn_air_translate(const char *dir, const void *bitcode, size_t bytes, const char *entry, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen) {
    return rn_air_translate_stage(dir, RN_AIR_KERNEL, bitcode, bytes, entry, info, spirv, spirv_bytes, err, errlen);
}

void rn_air_free(uint32_t *spirv, size_t bytes) {
    if (forget(spirv)) { free(spirv); return; }
    if (g_free) g_free(spirv, bytes);
}
