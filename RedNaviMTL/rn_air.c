// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#include "rn_air.h"
#include "rn_vk.h"
#include <mach-o/loader.h>
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
typedef int (*translate_tg_fn)(unsigned, const void *, size_t, const char *, const rn_air_const *, unsigned, const rn_air_tg_length *, unsigned, rn_air_kernel_info *, uint32_t **, size_t *, char *, size_t);
static translate_tg_fn g_translate_tg;
static free_fn g_free;
typedef int (*stitch_fn)(const char *, const rn_air_stitch_fn *, unsigned, const rn_air_stitch_node *, unsigned, char **, size_t *, char *, size_t);
static stitch_fn g_stitch;
typedef int (*stitch_function_fn)(const char *, const rn_air_dag_fn *, unsigned, const rn_air_stitch_node *, unsigned, int, char **, size_t *, char *, size_t);
static stitch_function_fn g_stitch_function;
typedef int (*link_visible_fn)(const void *, size_t, const rn_air_stitch_fn *, unsigned, char **, size_t *, char *, size_t);
static link_visible_fn g_link_visible;

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
    g_translate_tg = (translate_tg_fn)dlsym(g_lib, "rn_air_translate_stage_tg");
    g_free = (free_fn)dlsym(g_lib, "rn_air_free");
    g_stitch = (stitch_fn)dlsym(g_lib, "rn_air_stitch");
    g_stitch_function = (stitch_function_fn)dlsym(g_lib, "rn_air_stitch_function");
    g_link_visible = (link_visible_fn)dlsym(g_lib, "rn_air_link_visible");
    if (!g_translate || !g_free) { snprintf(err, errlen, "%s does not export rn_air_translate_stage and rn_air_free", path); g_translate = NULL; return 1; }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------- translation cache
// Translating one function takes seconds (QuartzCore asks for a hundred specializations when WindowServer starts), so the result is kept on disk, keyed by a hash of everything that
// goes into it: the translator library (its build UUID), the stage, the entry, the bitcode and the function constants. Only successes are kept. The directory is the user's own cache
// directory (confstr), mode 0700, and is used only if it belongs to this user and nobody else can write it: SPIR-V from there goes to the GPU. RN_AIR_NO_CACHE=1 turns it off.
#define RN_CACHE_MAGIC 0x31434e52u            // "RNC1"
typedef struct { uint32_t magic; uint32_t info_size; uint64_t spirv_bytes; uint64_t key; } rn_cache_head;

static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; return h; }

static int dir_writable(const char *dir) {      // WindowServer's sandbox lets it stat and mkdir its cache directory but not create a file in it: only a file that can be made counts
    char probe[1100];
    snprintf(probe, sizeof(probe), "%s.probe.%d", dir, (int)getpid());
    const int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return 0;
    close(fd); unlink(probe);
    return 1;
}

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
            strlcat(dir, "/", sizeof(dir));
            if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 022) && dir_writable(dir)) state = 1;
        }
        if (state < 0 && !getenv("RN_AIR_NO_CACHE")) {            // WindowServer's sandbox lets it neither write its user cache directory nor make a directory under /private/var/tmp, but it can make files there: the same cache as flat files named with a prefix (the owner and mode of each file are checked on load)
            snprintf(dir, sizeof(dir), "/private/var/tmp/rednavi-aircache-%d-", (int)geteuid());
            if (dir_writable(dir)) state = 1;
        }
    }
    const int ok = state > 0;
    pthread_mutex_unlock(&mu);
    return ok ? dir : NULL;
}

// What identifies this build of the translator: the linker's UUID of the library (the same after the installer copies and signs it again, which changes its time and size on disk);
// without one, its size and time.
static uint64_t lib_identity(const char *path) {
    uint64_t id = 0;
    const int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        struct mach_header_64 mh;
        if (pread(fd, &mh, sizeof(mh), 0) == (ssize_t)sizeof(mh) && mh.magic == MH_MAGIC_64 && mh.sizeofcmds < (1u << 20)) {
            uint8_t *cmds = malloc(mh.sizeofcmds);
            if (cmds && pread(fd, cmds, mh.sizeofcmds, sizeof(mh)) == (ssize_t)mh.sizeofcmds)
                for (size_t at = 0; at + sizeof(struct load_command) <= mh.sizeofcmds;) {
                    struct load_command lc; memcpy(&lc, cmds + at, sizeof(lc));
                    if (lc.cmdsize < sizeof(lc) || at + lc.cmdsize > mh.sizeofcmds) break;
                    if (lc.cmd == LC_UUID && lc.cmdsize >= sizeof(struct uuid_command)) { id = fnv(14695981039346656037ull, cmds + at + sizeof(lc), 16); break; }
                    at += lc.cmdsize;
                }
            free(cmds);
        }
        close(fd);
    }
    struct stat st;
    if (!id) id = stat(path, &st) == 0 ? ((uint64_t)st.st_size << 20) ^ (uint64_t)st.st_mtimespec.tv_sec : 1;
    return id;
}
static uint64_t lib_stamp;
static uint64_t cache_key(const char *dir, int stage, const void *bitcode, size_t bytes, const char *entry, const rn_air_const *consts, unsigned nconsts) {
    if (!lib_stamp) {
        char path[1100];
        snprintf(path, sizeof(path), "%s/librn_air2spirv.dylib", dir ? dir : rn_vk_dir());
        lib_stamp = lib_identity(path);
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
    snprintf(path, sizeof(path), "%s%016llx.spv", cdir, (unsigned long long)key);
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 0;
    struct stat fst;
    if (fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode) || fst.st_uid != geteuid() || (fst.st_mode & 022)) { close(fd); return 0; }
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
    snprintf(path, sizeof(path), "%s%016llx.spv", cdir, (unsigned long long)key);
    snprintf(tmp, sizeof(tmp), "%s.%d.%p", path, (int)getpid(), (void *)&path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    rn_cache_head h = {RN_CACHE_MAGIC, (uint32_t)sizeof(*info), spirv_bytes, key};
    int ok = write(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) && write(fd, info, sizeof(*info)) == (ssize_t)sizeof(*info) && write(fd, spirv, spirv_bytes) == (ssize_t)spirv_bytes;
    close(fd);
    if (ok) rename(tmp, path); else unlink(tmp);
}

// With the file /Users/Shared/rednavi-airmiss, each cache miss is written to /private/var/tmp/rednavi-airmiss-<uid>.log: what went into the key, part by part, to find the part that
// differs between two runs of the same program.
static void cache_miss_log(uint64_t key, int stage, const void *bitcode, size_t bytes, const char *entry, const rn_air_const *consts, unsigned nconsts) {
    static int on = -1; if (on < 0) on = access("/Users/Shared/rednavi-airmiss", F_OK) == 0;
    if (!on) return;
    char line[4096];
    size_t n = (size_t)snprintf(line, sizeof(line), "pid %d key %016llx stage %d entry %s bytes %zu bitcode %016llx consts %u", (int)getpid(), (unsigned long long)key, stage, entry, bytes,
                                (unsigned long long)fnv(14695981039346656037ull, bitcode, bytes), nconsts);
    for (unsigned i = 0; i < nconsts && n < sizeof(line) - 200; i++) {
        n += (size_t)snprintf(line + n, sizeof(line) - n, " [%d %s %u:", consts[i].index, consts[i].name ? consts[i].name : "", consts[i].size);
        for (unsigned b = 0; b < consts[i].size && b < 32; b++) n += (size_t)snprintf(line + n, sizeof(line) - n, "%02x", ((const uint8_t *)consts[i].data)[b]);
        n += (size_t)snprintf(line + n, sizeof(line) - n, "]");
    }
    char path[128];
    snprintf(path, sizeof(path), "/private/var/tmp/rednavi-airmiss-%d.log", (int)geteuid());
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    line[n++] = '\n';
    if (write(fd, line, n) != (ssize_t)n) {}
    close(fd);
}

int rn_air_translate_stage(const char *dir, int stage, const void *bitcode, size_t bytes, const char *entry, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen) {
    return rn_air_translate_stage_fc(dir, stage, bitcode, bytes, entry, NULL, 0, info, spirv, spirv_bytes, err, errlen);
}

int rn_air_translate_stage_fc(const char *dir, int stage, const void *bitcode, size_t bytes, const char *entry, const rn_air_const *consts, unsigned nconsts, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen) {
    return rn_air_translate_stage_tg(dir, stage, bitcode, bytes, entry, consts, nconsts, NULL, 0, info, spirv, spirv_bytes, err, errlen);
}

int rn_air_translate_stage_tg(const char *dir, int stage, const void *bitcode, size_t bytes, const char *entry, const rn_air_const *consts, unsigned nconsts, const rn_air_tg_length *tgs, unsigned ntgs, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen) {
    if (load(dir, err, errlen)) return 1;
    if (nconsts && !g_translate_fc) { snprintf(err, errlen, "the AIR translator library has no rn_air_translate_stage_fc (function constants)"); return 1; }
    if (ntgs && !g_translate_tg) { snprintf(err, errlen, "the AIR translator library has no rn_air_translate_stage_tg (threadgroup lengths)"); return 1; }
    const char *cdir = cache_dir();
    uint64_t key = 0;
    if (cdir) {
        key = cache_key(dir, stage, bitcode, bytes, entry, consts, nconsts);
        for (unsigned i = 0; i < ntgs; i++) key = fnv(key, &tgs[i], sizeof(tgs[i]));   // each set of lengths is its own module
        if (cache_load(cdir, key, info, spirv, spirv_bytes)) return 0;
        cache_miss_log(key, stage, bitcode, bytes, entry, consts, nconsts);
    }
    const int rc = ntgs ? g_translate_tg((unsigned)stage, bitcode, bytes, entry, consts, nconsts, tgs, ntgs, info, spirv, spirv_bytes, err, errlen)
                 : nconsts ? g_translate_fc((unsigned)stage, bitcode, bytes, entry, consts, nconsts, info, spirv, spirv_bytes, err, errlen)
                           : g_translate((unsigned)stage, bitcode, bytes, entry, info, spirv, spirv_bytes, err, errlen);
    if (!rc && cdir) cache_store(cdir, key, info, *spirv, *spirv_bytes);
    if (!rc && getenv("RN_DUMP_SPV_ALL")) {           // debugging: every module this process translates, as <dir>/<entry>.spv
        char path[1100]; snprintf(path, sizeof(path), "%s/%s.spv", getenv("RN_DUMP_SPV_ALL"), entry);
        FILE *f = fopen(path, "wb"); if (f) { fwrite(*spirv, 1, *spirv_bytes, f); fclose(f); }
    }
    return rc;
}

int rn_air_translate(const char *dir, const void *bitcode, size_t bytes, const char *entry, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen) {
    return rn_air_translate_stage(dir, RN_AIR_KERNEL, bitcode, bytes, entry, info, spirv, spirv_bytes, err, errlen);
}

void rn_air_free(uint32_t *spirv, size_t bytes) {
    if (forget(spirv)) { free(spirv); return; }
    if (g_free) g_free(spirv, bytes);
}

int rn_air_stitch(const char *dir, const char *kernel, const rn_air_stitch_fn *fns, unsigned nfns, const rn_air_stitch_node *nodes, unsigned nnodes, char **text, size_t *text_bytes, char *err, size_t errlen) {
    if (load(dir, err, errlen)) return 1;
    if (!g_stitch) { snprintf(err, errlen, "the AIR translator library has no rn_air_stitch (function stitching)"); return 1; }
    return g_stitch(kernel, fns, nfns, nodes, nnodes, text, text_bytes, err, errlen);
}

void rn_air_stitch_free(char *text) { free(text); }

int rn_air_stitch_function(const char *dir, const char *function, const rn_air_dag_fn *fns, unsigned nfns, const rn_air_stitch_node *nodes, unsigned nnodes, int output, char **text, size_t *text_bytes, char *err, size_t errlen) {
    if (load(dir, err, errlen)) return 1;
    if (!g_stitch_function) { snprintf(err, errlen, "the AIR translator library has no rn_air_stitch_function (DAG functions)"); return 1; }
    return g_stitch_function(function, fns, nfns, nodes, nnodes, output, text, text_bytes, err, errlen);
}

int rn_air_link_visible(const char *dir, const void *kernel, size_t bytes, const rn_air_stitch_fn *fns, unsigned nfns, char **text, size_t *text_bytes, char *err, size_t errlen) {
    if (load(dir, err, errlen)) return 1;
    if (!g_link_visible) { snprintf(err, errlen, "the AIR translator library has no rn_air_link_visible (linked functions)"); return 1; }
    return g_link_visible(kernel, bytes, fns, nfns, text, text_bytes, err, errlen);
}
