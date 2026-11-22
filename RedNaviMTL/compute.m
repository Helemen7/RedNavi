// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// The compute path of the Metal plug-in on RADV (docs/M6_METAL_ON_RADV.md, step 1): a command queue, command buffers, a compute command encoder, buffers and compute pipeline
// states that run on rn_vk. These are classes of their own that declare the Metal protocols (not subclasses of the vendor classes of the Apple drivers, which are tied to the
// kernel's command path); Metal.framework only needs them to answer the protocols' methods. Only what the compute path uses is implemented; anything else sent to them is a
// bug to find (unrecognized selector), and the render and blit encoders are nil on purpose: WindowServer and Core Animation must stay on their software fallback.
//
// Gate: the device methods below hand out these objects only in a process that has RN_METAL_COMPUTE set in its environment. Without it newCommandQueue* answer nil, as before,
// and newBuffer* go to the superclass. A real command queue given to every process would make Core Animation ask for render pipelines at once.
//
// Private entry for the shader bypass (until the AIR to SPIR-V translator exists): -[RedNaviMTLDevice rn_newComputePipelineWithSPIRV:bufferCount:pushBytes:] returns an
// id<MTLComputePipelineState> built from raw SPIR-V (entry point "main"). Buffers are bound at indices 0..bufferCount-1 with -setBuffer:offset:atIndex: (offset must be 0),
// -setBytes:length:atIndex: with an index >= bufferCount fills the push constants.
#include <mach/mach_time.h>
#include <os/lock.h>
#include <dlfcn.h>
#import <Foundation/Foundation.h>
#pragma clang diagnostic ignored "-Wunguarded-availability-new"   // protocol methods newer than the deployment target are answered when Metal asks
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <IOSurface/IOSurfaceRef.h>
#include <ImageIO/ImageIO.h>                    // RN_DUMP_PASSES
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include "rn_vk.h"
#include "rn_air.h"
#include "metallib.h"

extern void rn_log(const char *fmt, ...);
extern int rn_diag;                      // diagnostics on (plugin.m): /Users/Shared/rednavi-log, or a test process

// Which processes get the plug-in's queues, textures and the rest. Until now only the ones started with RN_METAL_COMPUTE in their environment, so WindowServer kept its software
// compositor. The environment variable still works; for every process (WindowServer included) the files /Users/Shared/rednavi-on (switch it on) and /Users/Shared/rednavi-off (wins
// over everything, the way back: it can be created over SSH without a password and is read at the next start of a process, or of the machine) decide. Both are looked at once per process.
static BOOL rn_gate(void) {
    static BOOL on;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        struct stat st;
        if (stat("/Users/Shared/rednavi-off", &st) == 0) { on = NO; return; }
        if (getenv("RN_METAL_COMPUTE") != NULL) { on = YES; return; }
        // rednavi-on empty: every process; with lines in it: only the processes with these names (one per line), so a test can start with WindowServer alone
        FILE *f = fopen("/Users/Shared/rednavi-on", "r");
        // A sandboxed process (Maps, the icon services) cannot read /Users/Shared: the profile refuses it with EPERM. It cannot see the files, so it takes the default of the project: on.
        // A file that does not exist (ENOENT) is the old default, off.
        if (!f) { if (errno == EPERM || errno == EACCES) on = YES; return; }
        char line[128], name[64]; extern const char *getprogname(void);
        strlcpy(name, getprogname(), sizeof(name));
        BOOL listed = NO, any = NO;
        while (fgets(line, sizeof(line), f)) { line[strcspn(line, "\r\n")] = 0; if (!line[0]) continue; any = YES; if (!strcmp(line, name)) listed = YES; }
        fclose(f);
        on = !any || listed;
    });
    return on;
}

static _Atomic long g_tex_live, g_tex_made; static _Atomic unsigned long g_tex_bytes;      // live textures and their bytes (the host copy and the image each take that much), for the log

// Diagnosis of leaks (2026-11-07: `leaks WindowServer` found thousands of buffers and textures nobody references): every buffer and texture notes where it was created (a short call stack,
// hashed), the log of the commit statistics lists the creation sites with the most objects still alive. Costs one backtrace per object; RN_SITES=0 in the environment turns it off.
#include <execinfo.h>
#define RN_NSITES 128
#define RN_MTL_SLOTS 31                 // binding slots per stage an encoder keeps (rn_slot_ok)
static struct rn_site { uint64_t h; long live; long made; char kind; char where[360]; } g_sites[RN_NSITES];
static os_unfair_lock g_sites_lock = OS_UNFAIR_LOCK_INIT;
static uint64_t rn_site_enter(char kind) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("RN_SITES"); off = !rn_diag || (e && e[0] == '0'); }
    if (off) return 0;
    void *bt[12];
    int n = backtrace(bt, 12);
    uint64_t h = 1469598103934665603ull ^ (uint64_t)kind;
    for (int i = 1; i < n; i++) { h ^= (uint64_t)(uintptr_t)bt[i]; h *= 1099511628211ull; }
    os_unfair_lock_lock(&g_sites_lock);
    struct rn_site *e = NULL;
    for (int i = 0; i < RN_NSITES; i++) { if (g_sites[i].h == h) { e = &g_sites[i]; break; } if (!g_sites[i].h && !e) e = &g_sites[i]; }
    if (e && e->h != h) {                              // a new site: keep the names of the first frames (the plug-in's own are skipped to show the caller)
        e->h = h; e->kind = kind; e->where[0] = 0;
        char **sym = backtrace_symbols(bt, n);
        if (sym) { for (int i = 1; i < n && i < 8; i++) { char *sp = strstr(sym[i], "0x"); const char *nm = sp ? strchr(sp, ' ') : NULL; strlcat(e->where, nm ? nm + 1 : sym[i], sizeof(e->where)); strlcat(e->where, " < ", sizeof(e->where)); } free(sym); }
    }
    if (e) { e->live++; e->made++; }
    os_unfair_lock_unlock(&g_sites_lock);
    return h;
}
static void rn_site_leave(uint64_t h) {
    if (!h) return;
    os_unfair_lock_lock(&g_sites_lock);
    for (int i = 0; i < RN_NSITES; i++) if (g_sites[i].h == h) { g_sites[i].live--; break; }
    os_unfair_lock_unlock(&g_sites_lock);
}
static void rn_sites_report(void) {                    // the four sites with the most live objects
    os_unfair_lock_lock(&g_sites_lock);
    for (int k = 0; k < 4; k++) {
        int best = -1;
        for (int i = 0; i < RN_NSITES; i++) if (g_sites[i].h && g_sites[i].live > 0 && (best < 0 || g_sites[i].live > g_sites[best].live)) best = i;
        if (best < 0) break;
        rn_log("  alive %ld of %ld made [%c]: %s", g_sites[best].live, g_sites[best].made, g_sites[best].kind, g_sites[best].where);
        g_sites[best].live = -g_sites[best].live;       // mark as reported (restored below)
    }
    for (int i = 0; i < RN_NSITES; i++) if (g_sites[i].live < 0) g_sites[i].live = -g_sites[i].live;
    os_unfair_lock_unlock(&g_sites_lock);
}

static int rn_vk_format_of(MTLPixelFormat pf);
static rn_vk_image *rn_display_vram_peek(void *tex);
static void rn_srcdump(IOSurfaceRef surf, const uint8_t *src, size_t pitch, size_t w, size_t h);
static void rn_cmpdump(IOSurfaceRef surf, size_t w, size_t h, const uint8_t *gpu, unsigned seq);
static void rn_display_host_write_note(void *tex, const char *how);
static rn_vk *rn_vk_shared(void) {
    static rn_vk *vk;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        char err[200] = "";
        vk = rn_vk_open(NULL, err, sizeof(err));
        if (vk) {
            rn_log("RADV opened: %s", rn_vk_device_name(vk));
            if (getenv("RN_LATENCY_PROBE") || access("/Users/Shared/rednavi-latency", F_OK) == 0) { double mn, av, mx; rn_vk_latency_probe(vk, 200, &mn, &av, &mx); rn_log("empty submit and wait, 200 round trips: min %.0f us, average %.0f us, max %.0f us", mn, av, mx); 
            if (rn_diag) { double up, down; const int f = rn_vk_format_of(MTLPixelFormatBGRA8Unorm); rn_vk_bandwidth_probe(vk, f, 30, &up, &down); rn_log("1920x1080 BGRA copy, 30 each: host to card %.2f ms, card to host %.2f ms", up, down); } }
        }
        else rn_log("RADV not available: %s", err);
    });
    return vk;
}

// YES when this process is let in and RADV opens. A process that is not let in gets no device at all: with the native path a client sees an accelerator it cannot make a command queue on (WindowServer aborts and restarts forever).
BOOL rn_compute_usable(void) { return rn_gate() && rn_vk_shared() != NULL; }

#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wincomplete-implementation"

@class RedNaviMTLComputePipelineState;

@interface RedNaviMTLBuffer : NSObject {
@public
    rn_vk_buffer *b;
    id<MTLDevice> dev;
    NSUInteger len;
    NSUInteger options;
    NSString *label_;
    void *userPtr;                                   // newBufferWithBytesNoCopy: the caller's memory, copied in before and out after each command buffer
    uint64_t site_;
    void (^userDealloc)(void *, NSUInteger);
}
- (instancetype)initWithDevice:(id<MTLDevice>)d length:(NSUInteger)l options:(NSUInteger)o;
@end

@implementation RedNaviMTLBuffer
- (MTLResourceID)gpuResourceID { MTLResourceID r = {0}; return r; }
- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)token { return 0; }
+ (void)load { class_addProtocol(self, @protocol(MTLBuffer)); class_addProtocol(self, @protocol(MTLResource)); }
- (instancetype)initWithDevice:(id<MTLDevice>)d length:(NSUInteger)l options:(NSUInteger)o {
    if ((self = [super init])) {
        rn_vk *vk = rn_vk_shared();
        b = vk ? rn_vk_buffer_create(vk, l ? l : 1) : NULL;
        if (!b) { rn_log("buffer of %lu bytes: allocation failed", (unsigned long)l); return nil; }
        dev = d; len = l; options = o; site_ = rn_site_enter('B');
        memset(rn_vk_buffer_contents(b), 0, l ? l : 1);
        if ((rn_diag && getenv("RN_LOG_BUFFERS"))) rn_log("new buffer %p: %lu bytes, options 0x%lx, contents %p", self, (unsigned long)l, (unsigned long)o, rn_vk_buffer_contents(b));
    }
    return self;
}
- (void)dealloc {
    rn_site_leave(site_);
    if (userPtr) { @synchronized([RedNaviMTLBuffer class]) { [[RedNaviMTLBuffer noCopyBuffers] removeObject:self]; } if (userDealloc) userDealloc(userPtr, len); }
    if (b) rn_vk_buffer_destroy(rn_vk_shared(), b);
}
+ (NSHashTable *)noCopyBuffers { static NSHashTable *t; static dispatch_once_t o; dispatch_once(&o, ^{ t = [NSHashTable weakObjectsHashTable]; }); return t; }
// The buffers made over the caller's memory: what the caller wrote goes into the RADV buffer before a command buffer runs, what the GPU wrote comes back after it.
// Copies to and from memory the caller owns. The caller can unmap or truncate it while the command buffer is being committed (it was the crash of 2026-11-07: SIGSEGV and SIGBUS in
// memmove under -commitBody), so the copy goes through the kernel, which answers an error where memmove would fault.
static BOOL rn_copy_from_caller(void *dst, const void *src, size_t n) {
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)src, n, (vm_address_t)dst, &got) == KERN_SUCCESS && got == n;
}
static BOOL rn_copy_to_caller(void *dst, const void *src, size_t n) {
    return vm_write(mach_task_self(), (vm_address_t)dst, (vm_offset_t)src, (mach_msg_type_number_t)n) == KERN_SUCCESS;
}
// Only the buffers this command buffer uses are copied: in before its work, out after it, and out only if it succeeded.
+ (void)syncNoCopy:(NSSet *)used in:(BOOL)in {
    for (RedNaviMTLBuffer *x in used) {
        if (!x->userPtr || !x->b) continue;
        static unsigned said;
        BOOL ok = in ? rn_copy_from_caller(rn_vk_buffer_contents(x->b), x->userPtr, x->len) : rn_copy_to_caller(x->userPtr, rn_vk_buffer_contents(x->b), x->len);
        if (!ok && said++ < 20) rn_log("buffer over caller memory %p (%lu bytes): the memory cannot be %s, skipped", x->userPtr, (unsigned long)x->len, in ? "read" : "written");
    }
}
- (void *)contents { return userPtr ? userPtr : rn_vk_buffer_contents(b); }
- (NSUInteger)length { return len; }
- (id<MTLDevice>)device { return dev; }
- (MTLResourceOptions)resourceOptions { return options; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeUntracked; }
- (NSUInteger)allocatedSize { return len; }
- (void)didModifyRange:(NSRange)range {}
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
- (id<MTLHeap>)heap { return nil; }
- (NSUInteger)heapOffset { return 0; }
- (BOOL)isAliasable { return NO; }
- (void)makeAliasable {}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return MTLPurgeableStateNonVolatile; }
@end

// Textures (docs/M6_METAL_ON_RADV.md): 2D, one level, one sample, in RADV memory with a host copy that is kept in step at the borders of each dispatch (rn_vk_image). A texture made
// from an IOSurface is a copy too, made at each command buffer: RADV cannot import host memory.
static int rn_vk_format_of(MTLPixelFormat pf) {
    switch (pf) {
    case MTLPixelFormatRGBA8Unorm: return RN_VK_RGBA8;
    case MTLPixelFormatBGRA8Unorm: return RN_VK_BGRA8;
    case MTLPixelFormatRGBA8Unorm_sRGB: return RN_VK_RGBA8_SRGB;
    case MTLPixelFormatRGB10A2Unorm: return RN_VK_RGB10A2;
    case MTLPixelFormatBGR10A2Unorm: return RN_VK_BGR10A2;
    case MTLPixelFormatRG16Float: return RN_VK_RG16F;
    case MTLPixelFormatR16Unorm: return RN_VK_R16;
    case MTLPixelFormatRG16Unorm: return RN_VK_RG16;
    case MTLPixelFormatRGBA16Unorm: return RN_VK_RGBA16;
    case MTLPixelFormatBGRA8Unorm_sRGB: return RN_VK_BGRA8_SRGB;
    case MTLPixelFormatR8Unorm: return RN_VK_R8;
    case MTLPixelFormatA8Unorm: return RN_VK_A8;
    case MTLPixelFormatRG8Unorm: return RN_VK_RG8;
    case MTLPixelFormatR16Float: return RN_VK_R16F;
    case MTLPixelFormatRGBA16Float: return RN_VK_RGBA16F;
    case MTLPixelFormatR32Float: return RN_VK_R32F;
    case MTLPixelFormatRG32Float: return RN_VK_RG32F;
    case MTLPixelFormatRGBA32Float: return RN_VK_RGBA32F;
    case MTLPixelFormatR32Uint: return RN_VK_R32U;
    case MTLPixelFormatR32Sint: return RN_VK_R32I;
    case MTLPixelFormatRGBA8Uint: return RN_VK_RGBA8U;
    default: return 0;
    }
}

// Depth and stencil formats are images of the card only (rn_vk_depth). Depth24Unorm_Stencil8 and Stencil8 are kept in a 32 bit float depth with stencil, which holds more.
static int rn_depth_format_of(MTLPixelFormat pf) {
    switch (pf) {
    case MTLPixelFormatDepth16Unorm: return RN_VK_D16;
    case MTLPixelFormatDepth32Float: return RN_VK_D32;
    case MTLPixelFormatDepth32Float_Stencil8: case MTLPixelFormatDepth24Unorm_Stencil8: case MTLPixelFormatStencil8: return RN_VK_D32S8;
    default: return 0;
    }
}

@class RedNaviMTLBuffer;
@interface RedNaviMTLTexture : NSObject {
@public
    RedNaviMTLBuffer *srcBuf;                        // a texture made inside a buffer (client storage): the buffer's rows are copied into the host copy before each command buffer that uses it
    NSUInteger srcOff, srcBpr;
    uint8_t *srcRaw;                                 // or client memory (newTiledTextureWithBytesNoCopy:...): the first texel's address, read the same way
    void *srcBase; NSUInteger srcLen; void (^srcDealloc)(void *, NSUInteger);
    rn_vk_image *img;
    rn_vk_depth *dep;                                // a depth or stencil texture: no image, no host copy
    id<MTLDevice> dev;
    MTLPixelFormat pf;
    NSUInteger w, h, d;                              // d: depth of a 3D texture, else 1
    MTLTextureType ttype;                            // the type asked for; a 1D texture is a one-row image, a 1D array one row per slice (CoreDisplay's gamma tables are R32Float 1D arrays)
    NSUInteger alen;                                 // array length reported (1D arrays only)
    unsigned texel;
    MTLTextureUsage usage_;
    NSString *label_;
    IOSurfaceRef surf;
    BOOL counted_;
    uint64_t site_;
    uint32_t surfSeed;                            // the surface's seed when the image and the surface last agreed: an unchanged surface is not copied again
    BOOL displayScan;                             // one of CoreDisplay's scanout surfaces (IOSurfaceName "CoreDisplay Scanout"): its render passes go straight into the display's own memory
    BOOL zeroCopy;                                // the image is the IOSurface's own memory (rn_vk_image_from_host): no copy in either direction
    NSUInteger surfPlane;                         // the plane of a multi-planar surface this texture shows (0 for the others)                               // set for a texture made from an IOSurface: copied in and out at the borders of a command buffer
    RedNaviMTLTexture *parent;                       // a texture view: the texture it is a view of (it shares its image, memory and host copy)
    MTLTextureSwizzleChannels swz;
    atomic_int queued_;                           // command buffers that use this texture (a root, not a view), were committed and have not run yet
}
- (instancetype)initWithDevice:(id<MTLDevice>)d descriptor:(MTLTextureDescriptor *)desc;
- (instancetype)initWithDevice:(id<MTLDevice>)d descriptor:(MTLTextureDescriptor *)desc iosurface:(IOSurfaceRef)io plane:(NSUInteger)plane;
- (void)syncFromSurface;
- (void)syncFromBuffer;
- (void)syncToSurface;
- (RedNaviMTLTexture *)viewWithPixelFormat:(MTLPixelFormat)pf swizzle:(const MTLTextureSwizzleChannels *)sw;
@end
// A commit returns before its command buffer has run (the bodies run on one serial queue). Metal's rule is that a committed command buffer sees the textures as they were when it was
// committed, so a host access to a texture (replaceRegion, getBytes) waits until the command buffers that use it have run. Without the wait a client that draws with a texture, commits
// and writes the next picture into the same texture got the new pixels in the old frame. The wait ends when the card's work is done, before the completion handlers, and gives up after
// two seconds: a handler that needs a lock the waiting thread holds must not stop both for good.
static pthread_mutex_t g_texq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_texq_cv = PTHREAD_COND_INITIALIZER;
static void *g_commit_key = &g_commit_key;      // set on the commit queue
static void rn_tex_wait_idle(RedNaviMTLTexture *t) {
    RedNaviMTLTexture *r = t->parent ? t->parent : t;
    if (atomic_load(&r->queued_) <= 0 || dispatch_get_specific(g_commit_key) == g_commit_key) return;
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 2;
    int rc = 0;
    pthread_mutex_lock(&g_texq_mu);
    while (atomic_load(&r->queued_) > 0 && rc == 0) rc = pthread_cond_timedwait(&g_texq_cv, &g_texq_mu, &until);
    pthread_mutex_unlock(&g_texq_mu);
    static atomic_uint waits, late;
    const unsigned n = atomic_fetch_add(&waits, 1);
    if (rc) { if (atomic_fetch_add(&late, 1) < 20) rn_log("texture %lux%lu: a host access did not wait for the command buffers that use it (2 s passed)", (unsigned long)r->w, (unsigned long)r->h); }
    else if (n < 8 || n % 1000 == 0) rn_log("texture %lux%lu: host access waited for a committed command buffer (%u such waits so far)", (unsigned long)r->w, (unsigned long)r->h, n + 1);
}
// The rows of a texture's host copy: a 2D array keeps its slices one after the other, so a blit to slice s is one to rows s * h and on.
// A cube texture is kept as a 2D array of six faces, so that it can be made, filled face by face and read back (Maps' ground textures are cubes). Sampling it as a cube in a shader is not done yet.
#define RN_LAYERED(tt) ((tt) == MTLTextureType2DArray || (tt) == MTLTextureTypeCube)
static NSUInteger rn_tex_rows(RedNaviMTLTexture *t) { return RN_LAYERED(t->ttype) ? t->h * t->d : t->h; }
static BOOL rn_slice_rows(RedNaviMTLTexture *t, NSUInteger slice, MTLOrigin *o) {
    if (!t) return NO;
    if (RN_LAYERED(t->ttype) && slice < t->d) { o->y += slice * t->h; return YES; }
    if (t->alen > 1 && slice < t->alen) { o->y += slice; return YES; }
    return NO;
}
@implementation RedNaviMTLTexture
+ (void)load { class_addProtocol(self, @protocol(MTLTexture)); class_addProtocol(self, @protocol(MTLResource)); }
- (instancetype)initWithDevice:(id<MTLDevice>)d descriptor:(MTLTextureDescriptor *)desc {
    if (!(self = [super init])) return nil;
    const BOOL is3d = desc.textureType == MTLTextureType3D;
    const BOOL is2da = RN_LAYERED(desc.textureType);   // OpenGL's GL_TEXTURE_2D_ARRAY (kitty's glyph atlas): the slices are kept like the depth of a 3D texture
    const BOOL is1d = desc.textureType == MTLTextureType1D || desc.textureType == MTLTextureType1DArray;
    ttype = desc.textureType; alen = desc.textureType == MTLTextureType1DArray ? (desc.arrayLength ? desc.arrayLength : 1) : 1;
    if ((desc.textureType != MTLTextureType2D && !is3d && !is1d && !is2da) || (desc.arrayLength > 1 && desc.textureType != MTLTextureType1DArray && !is2da) || desc.sampleCount > 1 || (!is3d && desc.depth > 1)) {
        rn_log("texture: only plain 2D textures are supported (type %ld, array %lu, samples %lu)", (long)desc.textureType, (unsigned long)desc.arrayLength, (unsigned long)desc.sampleCount);
        return nil;
    }
    if (desc.mipmapLevelCount > 1) rn_log("texture: %lu mipmap levels asked, only level 0 exists", (unsigned long)desc.mipmapLevelCount);
    if (rn_depth_format_of(desc.pixelFormat)) {
        rn_vk *dvk = rn_vk_shared();
        dep = dvk && desc.textureType == MTLTextureType2D ? rn_vk_depth_create(dvk, rn_depth_format_of(desc.pixelFormat), (uint32_t)desc.width, (uint32_t)desc.height) : NULL;
        if (!dep) { rn_log("depth texture %lux%lu pixel format %lu: not supported (type %ld, %s)", (unsigned long)desc.width, (unsigned long)desc.height, (unsigned long)desc.pixelFormat, (long)desc.textureType, dvk ? "no depth image on the card" : "RADV is not available"); return nil; }
        dev = d; pf = desc.pixelFormat; w = desc.width; h = desc.height; self->d = 1; usage_ = desc.usage; texel = 4;
        site_ = rn_site_enter('T');
        return self;
    }
    const int f = rn_vk_format_of(desc.pixelFormat);
    rn_vk *vk = f ? rn_vk_shared() : NULL;
    const NSUInteger rows = is1d ? alen : desc.height;
    const NSUInteger slices = is3d ? desc.depth : desc.textureType == MTLTextureTypeCube ? 6 : is2da ? (desc.arrayLength ? desc.arrayLength : 1) : 1;
    img = vk ? rn_vk_image_create_layers(vk, f, (uint32_t)desc.width, (uint32_t)rows, (uint32_t)slices, is2da) : NULL;
    if (!img) { rn_log("texture %lux%lu pixel format %lu: not supported (%s)", (unsigned long)desc.width, (unsigned long)desc.height, (unsigned long)desc.pixelFormat, f ? (vk ? rn_vk_image_last_error() : "RADV is not available") : "no Vulkan format for it"); return nil; }
    dev = d; pf = desc.pixelFormat; w = desc.width; h = rows; self->d = slices; usage_ = desc.usage; texel = rn_vk_format_texel_bytes(f);
    site_ = rn_site_enter('T');
    counted_ = YES; atomic_fetch_add(&g_tex_live, 1); atomic_fetch_add(&g_tex_made, 1); atomic_fetch_add(&g_tex_bytes, (unsigned long)w * h * self->d * texel);
    return self;
}
- (void)dealloc { rn_site_leave(site_); if (srcDealloc) srcDealloc(srcBase, srcLen); if (counted_) { atomic_fetch_sub(&g_tex_live, 1); atomic_fetch_sub(&g_tex_bytes, (unsigned long)w * h * self->d * texel); } if (img) rn_vk_image_destroy(rn_vk_shared(), img); if (dep) rn_vk_depth_destroy(rn_vk_shared(), dep); if (surf) CFRelease(surf); }
// IOSurface-backed textures (no host memory import in the kext, so a copy): the surface's rows are copied into the host copy of the image before the command buffer runs and, for
// what the command buffer wrote, back afterwards. The surface's row pitch is its own; the host copy is packed.
- (instancetype)initWithDevice:(id<MTLDevice>)d descriptor:(MTLTextureDescriptor *)desc iosurface:(IOSurfaceRef)io plane:(NSUInteger)plane {
    if (!io) { rn_log("texture from an IOSurface: no surface"); return nil; }
    const size_t planes = IOSurfaceGetPlaneCount(io);
    if (plane >= (planes ? planes : 1)) { rn_log("texture from an IOSurface: plane %lu of a surface with %zu", (unsigned long)plane, planes); return nil; }
    if ((self = [self initWithDevice:d descriptor:desc])) {
        const size_t sw = planes ? IOSurfaceGetWidthOfPlane(io, plane) : IOSurfaceGetWidth(io), sh = planes ? IOSurfaceGetHeightOfPlane(io, plane) : IOSurfaceGetHeight(io);
        const size_t sb = planes ? IOSurfaceGetBytesPerElementOfPlane(io, plane) : IOSurfaceGetBytesPerElement(io);
        if (w > sw || h > sh || texel != sb) {
            rn_log("texture from an IOSurface: plane %lu is %zux%zu with %zu bytes per element, the texture %lux%lu with %u", (unsigned long)plane, sw, sh, sb, (unsigned long)w, (unsigned long)h, texel);
            return nil;
        }
        surfPlane = planes ? plane : 0;
        surf = (IOSurfaceRef)CFRetain(io);
        if (!planes) { CFTypeRef nm = IOSurfaceCopyValue(io, CFSTR("IOSurfaceName")); displayScan = nm && CFGetTypeID(nm) == CFStringGetTypeID() && CFStringCompare((CFStringRef)nm, CFSTR("CoreDisplay Scanout"), 0) == kCFCompareEqualTo; if (nm) CFRelease(nm); }
        // Zero copy when the surface is one linear plane the card can use as it is: the image is made over the surface's own pages (the kext wires them, RADV imports them as host memory).
        // RN_METAL_NO_ZEROCOPY=1 keeps the copies, for comparison.
        if (!planes && !getenv("RN_METAL_NO_ZEROCOPY") && access("/Users/Shared/rednavi-nozerocopy", F_OK) != 0 && w == sw && h == sh) {
            rn_vk *zvk = rn_vk_shared();
            const int zf = rn_vk_format_of(pf);
            void *zbase = IOSurfaceGetBaseAddress(io);
            rn_vk_image *z = zvk && zf && zbase ? rn_vk_image_from_host(zvk, zf, (uint32_t)w, (uint32_t)h, zbase, IOSurfaceGetBytesPerRow(io), IOSurfaceGetAllocSize(io)) : NULL;
            static unsigned said;
            if (z) { rn_vk_image_destroy(zvk, img); img = z; zeroCopy = YES; if (said++ < 6) rn_log("iosurface %u %lux%lu: zero copy", (unsigned)IOSurfaceGetID(io), (unsigned long)w, (unsigned long)h); }
            else if (w >= 256 && said++ < 6) rn_log("iosurface %u %lux%lu: copies (%s)", (unsigned)IOSurfaceGetID(io), (unsigned long)w, (unsigned long)h, rn_vk_image_last_error());
        }
        [self syncFromSurface];
    }
    return self;
}
// The host copy is written under the batch lock, like replaceRegion and syncFromBuffer: a batch that records the upload of this image on another thread must not read the rows half written (torn
// rows) or clear the dirty flag after the new rows are in but before the flag is set (a lost update that leaves the old picture). The lock comes before the IOSurface lock, as everywhere.
- (void)syncFromSurface {
    if (!surf || zeroCopy) return;
    rn_vk_host_lock();
    [self syncFromSurfaceLocked];
    rn_vk_host_unlock();
}
- (void)syncFromSurfaceLocked {
    if (!surf || zeroCopy) return;
    // The seed only changes for writes made under IOSurfaceLock; surfaces are also written by other means (WindowServer's own compositor, the GPU of other processes), so every use copies
    // the surface again. A seed shortcut for small surfaces left stale titlebars and small rectangles in the picture.
    { const IOReturn lr = IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
      if (lr != kIOReturnSuccess) { static unsigned said; if (said++ < 40) rn_log("iosurface %u plane %lu: lock failed (0x%x), the texture keeps its old pixels", (unsigned)IOSurfaceGetID(surf), (unsigned long)surfPlane, (unsigned)lr); return; } }
    const bool planar = IOSurfaceGetPlaneCount(surf) > 0;
    const uint8_t *src = planar ? IOSurfaceGetBaseAddressOfPlane(surf, surfPlane) : IOSurfaceGetBaseAddress(surf);
    const size_t pitch = planar ? IOSurfaceGetBytesPerRowOfPlane(surf, surfPlane) : IOSurfaceGetBytesPerRow(surf);
    uint8_t *dst = rn_vk_image_staging(img);
    {   // Experiment switch /Users/Shared/rednavi-yuvgrey: the planes of a planar surface are mid grey (luma and neutral chroma) instead of the surface's contents
        static int grey = -1;
        if (grey < 0) grey = access("/Users/Shared/rednavi-yuvgrey", F_OK) == 0;
        if (grey && planar) {
            IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
            if (dst[0] != 0x80) { memset(dst, 0x80, w * h * texel); rn_vk_image_host_wrote(img); }
            return;
        }
    }
    if (!planar && texel == 4 && w >= 256) rn_srcdump(surf, src, pitch, w, h);      // experiment: the surface's own bytes, to compare with what the card holds (only while a pass capture runs)
    BOOL changed = NO;      // rows that are the same are not written, and an image that did not change is not uploaded to the card again
    for (NSUInteger y = 0; y < h; y++) {
        uint8_t *d = dst + y * w * texel; const uint8_t *r = src + y * pitch;
        if (changed || memcmp(d, r, w * texel)) { memcpy(d, r, w * texel); changed = YES; }
    }
    IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
    surfSeed = IOSurfaceGetSeed(surf);
    if (changed) rn_vk_image_host_wrote(img);
    if ((w == 161 || w == 249) && w == h && texel == 4) {   // the window shadow textures: when their bytes change, and the seed (who writes them is not known; a hash that never changes after the first use means the
        static uint64_t seen[8]; static uint32_t seenId[8]; static unsigned nseen;   // notch in their outline was there from the start)
        uint64_t hsh = 1469598103934665603ull; for (size_t i = 0; i < (size_t)w * h * texel; i++) hsh = (hsh ^ dst[i]) * 1099511628211ull;
        const uint32_t sid = (uint32_t)IOSurfaceGetID(surf); unsigned k = 0; while (k < nseen && seenId[k] != sid) k++;
        if (k == nseen && nseen < 8) { seenId[nseen] = sid; seen[nseen++] = 0; }
        if (k < 8 && seen[k] != hsh) { rn_log("shadow texture surface %u %lux%lu: bytes %016llx (were %016llx), seed %u, rows %s", sid, (unsigned long)w, (unsigned long)h, (unsigned long long)hsh, (unsigned long long)seen[k], (unsigned)surfSeed, changed ? "rewritten" : "same as the host copy"); seen[k] = hsh; }
    }
}
- (void)syncToSurface {
    if (!surf || zeroCopy) return;
    const uint8_t *src = rn_vk_image_staging(img);      // before the IOSurface lock: it can take the batch lock, and syncFromSurface takes the batch lock first
    if (IOSurfaceLock(surf, 0, NULL) != kIOReturnSuccess) return;
    const bool planar = IOSurfaceGetPlaneCount(surf) > 0;
    uint8_t *dst = planar ? IOSurfaceGetBaseAddressOfPlane(surf, surfPlane) : IOSurfaceGetBaseAddress(surf);
    const size_t pitch = planar ? IOSurfaceGetBytesPerRowOfPlane(surf, surfPlane) : IOSurfaceGetBytesPerRow(surf);
    for (NSUInteger y = 0; y < h; y++) memcpy(dst + y * pitch, src + y * w * texel, w * texel);
    {   // measurement: how much of what the GPU wrote is not black (4096 sampled texels of a 4-byte format), for the first writes and then every 400th
        static unsigned n; unsigned k = n++;
        if (texel == 4 && w >= 256 && (k < 24 || k % 400 == 0)) {
            unsigned nz = 0; uint32_t v0 = 0;
            for (unsigned i = 0; i < 4096; i++) { size_t yy = ((size_t)i * 2654435761u) % h, xx = ((size_t)i * 40503u) % w; uint32_t px = *(const uint32_t *)(src + (yy * w + xx) * 4); if (px & 0x00ffffff) nz++; if (i == 0) v0 = px; }
            rn_log("syncToSurface #%u: surface %u %lux%lu, %u of 4096 sampled texels not black (first %08x)", k, (unsigned)IOSurfaceGetID(surf), (unsigned long)w, (unsigned long)h, nz, v0);
        }
    }
    IOSurfaceUnlock(surf, 0, NULL);
    surfSeed = IOSurfaceGetSeed(surf);
}
- (void)syncFromBuffer {
    if (!srcBuf && !srcRaw) return;
    rn_vk_host_lock();
    const uint8_t *src = srcBuf ? (const uint8_t *)[(id<MTLBuffer>)srcBuf contents] + srcOff : srcRaw;
    uint8_t *dst = rn_vk_image_staging(img);
    // Only what differs is written, and an image that did not change is not uploaded to the card again. The caller's memory is read through the kernel copy (it can vanish under us), a row at a time.
    BOOL changed = NO;
    // One kernel call for the whole image when it fits the scratch (the row loop below cost one mach message per row: Launchpad's folder animation spent most of a frame in vm_read_overwrite, 2026-11-21).
    // The scratch is shared and only used under the host lock taken above.
    static uint8_t *scratch; static size_t scratchCap;
    const size_t span = srcBpr * (h - 1) + w * texel;
    if (h > 1 && span <= (8u << 20)) {
        if (span > scratchCap) { free(scratch); scratchCap = span; scratch = malloc(scratchCap); }
        if (scratch && rn_copy_from_caller(scratch, src, span)) {
            for (NSUInteger y = 0; y < h; y++) {
                const uint8_t *s = scratch + y * srcBpr;
                uint8_t *d = dst + y * w * texel;
                if (memcmp(d, s, w * texel)) { memcpy(d, s, w * texel); changed = YES; }
            }
            if (changed) rn_vk_image_host_wrote(img);
            rn_vk_host_unlock();
            return;
        }
        // not readable as a whole (the last row may end the mapping, or it vanished): the row loop below skips what cannot be read
    }
    uint8_t rowbuf[16384];
    const BOOL small = w * texel <= sizeof(rowbuf);
    for (NSUInteger y = 0; y < h; y++) {
        uint8_t *d = dst + y * w * texel;
        BOOL ok;
        if (small && !changed) {
            ok = rn_copy_from_caller(rowbuf, src + y * srcBpr, w * texel);
            if (ok && memcmp(d, rowbuf, w * texel)) { memcpy(d, rowbuf, w * texel); changed = YES; }
        } else {
            ok = rn_copy_from_caller(d, src + y * srcBpr, w * texel);
            if (ok) changed = YES;
        }
        if (!ok) { static unsigned said; if (said++ < 20) rn_log("texture over caller memory %p: row %lu cannot be read, skipped", (const void *)src, (unsigned long)y); break; }
    }
    if (changed) rn_vk_image_host_wrote(img);
    rn_vk_host_unlock();
}
- (id<MTLDevice>)device { return dev; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
- (MTLResourceOptions)resourceOptions { return 0; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeUntracked; }
- (NSUInteger)allocatedSize { return w * h * d * texel; }
- (id<MTLHeap>)heap { return nil; }
- (NSUInteger)heapOffset { return 0; }
- (BOOL)isAliasable { return NO; }
- (void)makeAliasable {}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return MTLPurgeableStateNonVolatile; }
- (id<MTLResource>)rootResource { return nil; }
- (id<MTLTexture>)parentTexture { return (id)parent; }
- (NSUInteger)parentRelativeLevel { return 0; }
- (NSUInteger)parentRelativeSlice { return 0; }
- (id<MTLBuffer>)buffer { return (id)srcBuf; }
- (NSUInteger)bufferOffset { return srcOff; }
- (NSUInteger)bufferBytesPerRow { return srcBpr; }
- (IOSurfaceRef)iosurface { return surf; }
- (NSUInteger)iosurfacePlane { return surfPlane; }
// Texture views: another pixel format of the same texel size and/or swizzled components over the same storage. Only the whole of a 2D, one-level texture can be viewed.
- (RedNaviMTLTexture *)viewWithPixelFormat:(MTLPixelFormat)newPf swizzle:(const MTLTextureSwizzleChannels *)sw {
    const int f = rn_vk_format_of(newPf);
    rn_vk *vk = f ? rn_vk_shared() : NULL;
    if (!vk) { rn_log("texture view: pixel format %lu is not supported", (unsigned long)newPf); return nil; }
    unsigned comp[4], *cp = NULL;
    if (sw) {
        const MTLTextureSwizzle in[4] = {sw->red, sw->green, sw->blue, sw->alpha};
        static const unsigned map[6] = {1, 2, 3, 4, 5, 6};               // VkComponentSwizzle: ZERO ONE R G B A for Zero One Red Green Blue Alpha
        for (int i = 0; i < 4; i++) comp[i] = in[i] < 6 ? map[in[i]] : 0;
        cp = comp;
    }
    rn_vk_image *v = rn_vk_image_view(vk, parent ? parent->img : img, f, cp);
    if (!v) { rn_log("texture view: pixel format %lu does not fit the texture (texel size) or cannot be sampled", (unsigned long)newPf); return nil; }
    RedNaviMTLTexture *t = [[RedNaviMTLTexture alloc] init];
    t->img = v; t->dev = dev; t->pf = newPf; t->w = w; t->h = h; t->d = d; t->ttype = ttype; t->alen = alen; t->texel = texel; t->usage_ = usage_;
    t->parent = parent ? parent : self;
    t->zeroCopy = t->parent->zeroCopy;
    t->displayScan = t->parent->displayScan;
    t->surf = t->parent->surf ? (IOSurfaceRef)CFRetain(t->parent->surf) : NULL;
    t->swz = sw ? *sw : (MTLTextureSwizzleChannels){MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha};
    return t;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)newPf { return (id)[self viewWithPixelFormat:newPf swizzle:NULL]; }
// A view must cover the whole texture: all its slices, one level, the same type (a 2D array of one slice may also be viewed as 2D).
static BOOL rn_whole_view(RedNaviMTLTexture *t, MTLTextureType type, NSRange levels, NSRange slices) {
    const MTLTextureType own = t.textureType;
    const NSUInteger n = own == MTLTextureType2DArray ? t.arrayLength : own == MTLTextureTypeCube ? 6 : 1;
    if ((type == own || (type == MTLTextureType2D && own == MTLTextureType2DArray && n == 1)) && levels.location == 0 && levels.length == 1 && slices.location == 0 && slices.length == n) return YES;
    rn_log("texture view: only the whole of a texture of one level can be viewed (type %ld of %ld, slices %lu+%lu)", (long)type, (long)own, (unsigned long)slices.location, (unsigned long)slices.length);
    return NO;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)newPf textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices {
    if (!rn_whole_view(self, type, levels, slices)) return nil;
    return [self newTextureViewWithPixelFormat:newPf];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)newPf textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices swizzle:(MTLTextureSwizzleChannels)swizzle {
    if (!rn_whole_view(self, type, levels, slices)) return nil;
    return (id)[self viewWithPixelFormat:newPf swizzle:&swizzle];
}
- (MTLTextureSwizzleChannels)swizzle { return swz.red == swz.green && swz.green == swz.blue && swz.blue == swz.alpha && swz.red == 0 && !parent ? (MTLTextureSwizzleChannels){MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha} : swz; }
// Private: the swizzle packed one channel per byte, red in the low byte (identity 0x05040302). AppleMetalOpenGLRenderer compares it with the swizzle a GL texture wants before it
// makes a view; the generic answer 0 is no valid swizzle, and the renderer then gave up on the view ("unhandled ... diffSwizzleKey").
- (uint32_t)swizzleKey { const MTLTextureSwizzleChannels c = [self swizzle]; return (uint32_t)c.red | (uint32_t)c.green << 8 | (uint32_t)c.blue << 16 | (uint32_t)c.alpha << 24; }
- (MTLTextureCompressionType)compressionType { return MTLTextureCompressionTypeLossless; }
- (MTLResourceID)gpuResourceID { MTLResourceID r = {0}; return r; }
- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)token { return 0; }
- (id<MTLTexture>)newRemoteTextureViewForDevice:(id<MTLDevice>)device { rn_log("newRemoteTextureViewForDevice: not supported"); return nil; }
- (MTLSharedTextureHandle *)newSharedTextureHandle { rn_log("newSharedTextureHandle: not supported (an IOSurface-backed texture is shared through its IOSurface)"); return nil; }
- (id<MTLTexture>)remoteStorageTexture { return nil; }
- (MTLTextureType)textureType { return (alen && (ttype == MTLTextureType1D || ttype == MTLTextureType1DArray)) || RN_LAYERED(ttype) ? ttype : (d > 1 ? MTLTextureType3D : MTLTextureType2D); }
- (MTLPixelFormat)pixelFormat { return pf; }
- (NSUInteger)width { return w; }
- (NSUInteger)height { return (alen && (ttype == MTLTextureType1D || ttype == MTLTextureType1DArray)) ? 1 : h; }
- (NSUInteger)depth { return RN_LAYERED(ttype) ? 1 : d; }
- (NSUInteger)mipmapLevelCount { return 1; }
- (NSUInteger)sampleCount { return 1; }
- (NSUInteger)arrayLength { return ttype == MTLTextureType2DArray ? d : alen ? alen : 1; }
- (MTLTextureUsage)usage { return usage_; }
- (BOOL)isShareable { return NO; }
- (BOOL)isFramebufferOnly { return NO; }
- (BOOL)allowGPUOptimizedContents { return NO; }
- (BOOL)isSparse { return NO; }
- (NSUInteger)firstMipmapInTail { return 0; }
- (NSUInteger)tailSizeInBytes { return 0; }
- (BOOL)clampRegion:(MTLRegion)r {
    return r.origin.x + r.size.width <= w && r.origin.y + r.size.height <= h && r.origin.z + r.size.depth <= d;
}
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi {
    if (RN_LAYERED(ttype) && slice && !region.origin.z && region.size.depth == 1) { region.origin.z = slice; slice = 0; }   // a slice of a 2D array is stored like a depth slice
    if (level || slice || ![self clampRegion:region]) { rn_log("replaceRegion: level %lu slice %lu or region outside the texture ignored", (unsigned long)level, (unsigned long)slice); return; }
    rn_tex_wait_idle(self);
    rn_vk_host_lock();
    uint8_t *dst = rn_vk_image_staging(img);
    const size_t pitch = rn_vk_image_row_bytes(img);      // a zero-copy IOSurface image has the surface's row pitch
    if (!bpi) bpi = bpr * region.size.height;
    for (NSUInteger z = 0; z < region.size.depth; z++)
        for (NSUInteger y = 0; y < region.size.height; y++)
            memcpy(dst + ((region.origin.z + z) * h + region.origin.y + y) * pitch + region.origin.x * texel, (const uint8_t *)bytes + z * bpi + y * bpr, region.size.width * texel);
    rn_vk_image_host_wrote(img);
    rn_vk_host_unlock();
    if ((w == 161 || w == 249) && w == h) rn_log("shadow texture %lux%lu: replaceRegion (%lu,%lu %lux%lu) from %p", (unsigned long)w, (unsigned long)h, (unsigned long)region.origin.x, (unsigned long)region.origin.y, (unsigned long)region.size.width, (unsigned long)region.size.height, __builtin_return_address(0));
    rn_display_host_write_note((__bridge void *)self, "replaceRegion");
    [self syncToSurface];
}
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr {
    [self replaceRegion:region mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:bpr bytesPerImage:0];
}
- (void)getBytes:(void *)out bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice {
    if (RN_LAYERED(ttype) && slice && !region.origin.z && region.size.depth == 1) { region.origin.z = slice; slice = 0; }
    if (level || slice || ![self clampRegion:region]) { rn_log("getBytes: level %lu slice %lu or region outside the texture ignored", (unsigned long)level, (unsigned long)slice); return; }
    rn_tex_wait_idle(self);
    [self syncFromSurface];
    rn_vk_image *card = rn_display_vram_peek((__bridge void *)self);      // a scanout surface's pixels are in the card's copy, not in the texture's own image
    const uint8_t *src = rn_vk_image_staging(card ? card : img);
    const size_t pitch = rn_vk_image_row_bytes(card ? card : img);
    if (!bpi) bpi = bpr * region.size.height;
    for (NSUInteger z = 0; z < region.size.depth; z++)
        for (NSUInteger y = 0; y < region.size.height; y++)
            memcpy((uint8_t *)out + z * bpi + y * bpr, src + ((region.origin.z + z) * h + region.origin.y + y) * pitch + region.origin.x * texel, region.size.width * texel);
}
- (void)getBytes:(void *)out bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level {
    [self getBytes:out bytesPerRow:bpr bytesPerImage:0 fromRegion:region mipmapLevel:level slice:0];
}
@end

@interface RedNaviMTLSamplerState : NSObject {
@public
    rn_vk_sampler *s;
    id<MTLDevice> dev;
    NSString *label_;
}
@end
@implementation RedNaviMTLSamplerState
- (MTLResourceID)gpuResourceID { MTLResourceID r = {0}; return r; }
+ (void)load { class_addProtocol(self, @protocol(MTLSamplerState)); }
- (void)dealloc { if (s) rn_vk_sampler_destroy(rn_vk_shared(), s); }
- (id<MTLDevice>)device { return dev; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
@end

static uint32_t rn_address_of(MTLSamplerAddressMode m) {
    switch (m) {
    case MTLSamplerAddressModeClampToEdge: case MTLSamplerAddressModeMirrorClampToEdge: return 1;
    case MTLSamplerAddressModeRepeat: return 2;
    case MTLSamplerAddressModeMirrorRepeat: return 3;
    case MTLSamplerAddressModeClampToZero: return 0;
    default: return 4;
    }
}
static id rn_new_sampler_state(id<MTLDevice> dev, MTLSamplerDescriptor *d) {
    rn_vk *vk = rn_vk_shared();
    if (!vk) return nil;
    rn_vk_sampler_desc sd;
    memset(&sd, 0, sizeof(sd));
    sd.min_filter = d.minFilter == MTLSamplerMinMagFilterLinear;
    sd.mag_filter = d.magFilter == MTLSamplerMinMagFilterLinear;
    sd.mip_filter = d.mipFilter == MTLSamplerMipFilterLinear ? 2 : d.mipFilter == MTLSamplerMipFilterNearest ? 1 : 0;
    sd.address[0] = rn_address_of(d.sAddressMode); sd.address[1] = rn_address_of(d.tAddressMode); sd.address[2] = rn_address_of(d.rAddressMode);
    sd.pixel_coords = !d.normalizedCoordinates;
    sd.border = d.borderColor == MTLSamplerBorderColorOpaqueBlack ? 1 : d.borderColor == MTLSamplerBorderColorOpaqueWhite ? 2 : 0;
    sd.lod_min = d.lodMinClamp; sd.lod_max = d.lodMaxClamp;
    RedNaviMTLSamplerState *st = [RedNaviMTLSamplerState new];
    st->s = rn_vk_sampler_create(vk, &sd);
    st->dev = dev;
    if (!st->s) { rn_log("sampler state: RADV refused it"); return nil; }
    return st;
}

@interface RedNaviMTLComputePipelineState : NSObject {
@public
    rn_vk_compute *c;
    unsigned nbind;                  // resources of the pipeline, in the order of its rn_vk bindings
    unsigned metalIndex[RN_VK_MAX_BINDINGS];   // the Metal index each of them takes (buffer, texture or sampler index, by kind)
    unsigned kind[RN_VK_MAX_BINDINGS];         // RN_VK_BUFFER, RN_VK_SAMPLED_IMAGE, RN_VK_STORAGE_IMAGE, RN_VK_SAMPLER or RN_VK_STATIC_SAMPLER
    unsigned push;                   // bytes of push constants (the hand-made SPIR-V path; 0 for translated kernels)
    unsigned pushIndex;              // the Metal index that setBytes fills into the push constants (0xffffffff: none: setBytes data becomes a small buffer)
    BOOL spec;                       // the threadgroup size is specialized at dispatch (translated kernels)
    BOOL dyn;                        // translated kernel with the grid in push constants: run with rn_vk_dispatch_threads
    unsigned localSize[3];
    id<MTLDevice> dev;
    NSString *label_;
}
@end
@implementation RedNaviMTLComputePipelineState
- (MTLResourceID)gpuResourceID { MTLResourceID r = {0}; return r; }
+ (void)load { class_addProtocol(self, @protocol(MTLComputePipelineState)); }
- (void)dealloc { if (c) rn_vk_compute_destroy(rn_vk_shared(), c); }
- (id<MTLDevice>)device { return dev; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (NSUInteger)threadExecutionWidth { return 32; }
- (NSUInteger)staticThreadgroupMemoryLength { return 0; }
- (BOOL)supportsIndirectCommandBuffers { return NO; }
@end

// Binding slots an encoder keeps per stage, for buffers, textures and samplers alike: 31 is the buffer limit the device reports (maxVertexBuffers, maxFragmentBuffers). The table had 16 and
// dropped every binding above it without a word, so a shader that reads index 16 or more failed with "no buffer bound". Textures 31 to 127 (the device reports 128) are still dropped, now logged.
static BOOL rn_slot_ok(NSUInteger index, const char *what) {
    if (index < RN_MTL_SLOTS) return YES;
    static unsigned said;
    if (said++ < 8) rn_log("%s binding at index %lu dropped: the encoder keeps %d slots", what, (unsigned long)index, RN_MTL_SLOTS);
    return NO;
}

// The resources a Metal encoder has bound, turned into the rn_vk arguments of a pipeline's bindings (compute, vertex and fragment stages all use it). Returns an error text or nil.
// setBytes data becomes a small buffer that is kept in `temps` until the work is done.
static NSMutableArray *rn_batch_keep(void) { static NSMutableArray *k; static dispatch_once_t o; dispatch_once(&o, ^{ k = [NSMutableArray new]; }); return k; }
// A texture of zeros for a texture argument with nothing behind it, by the translator's dimension (rn_air_binding.dim): 3D 1 x 1 x 2, a 2D array of one slice, otherwise 2D 1 x 1.
// Made once each; they are never written.
static rn_vk_image *rn_null_image(unsigned dim) {
    static rn_vk_image *img[3];
    static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    const int k = dim == 2 ? 1 : dim == 9 ? 2 : 0;
    os_unfair_lock_lock(&lock);
    if (!img[k]) { rn_vk *vk = rn_vk_shared(); img[k] = vk ? rn_vk_image_create_layers(vk, RN_VK_RGBA8, 1, 1, k == 1 ? 2 : 1, k == 2) : NULL; }
    os_unfair_lock_unlock(&lock);
    return img[k];
}
static NSString *rn_bind_args(id<MTLDevice> dev, unsigned n, const unsigned *kind, const unsigned *mi, RedNaviMTLBuffer *const *bufs, const uint64_t *offs, NSData *const *bytesAt,
                              RedNaviMTLTexture *const *texs, RedNaviMTLSamplerState *const *samps, rn_vk_arg *av, NSMutableArray *temps) {
    for (unsigned i = 0; i < n; i++) {
        const unsigned m = mi[i];
        switch (kind[i] & 0xff) {
        case RN_VK_BUFFER:
            if (m < RN_MTL_SLOTS && bufs[m]) { av[i].buffer = bufs[m]->b; av[i].offset = offs[m]; break; }
            if (m < RN_MTL_SLOTS && bytesAt[m]) {
                RedNaviMTLBuffer *t = [[RedNaviMTLBuffer alloc] initWithDevice:dev length:bytesAt[m].length options:0];
                if (!t) return @"no memory for constant data";
                memcpy(rn_vk_buffer_contents(t->b), bytesAt[m].bytes, bytesAt[m].length);
                [temps addObject:t];
                av[i].buffer = t->b;
                break;
            }
            {
                // Nothing bound: Metal lets an app leave a buffer of the function unbound when the code does not reach it, and AppleMetalOpenGLRenderer does so with the uniform
                // buffer of a GL program that has no uniforms (2026-11-21). The translator still lists the argument, so it gets a buffer of zeros, made once per process.
                static RedNaviMTLBuffer *zeros;
                static dispatch_once_t once;
                dispatch_once(&once, ^{ zeros = [[RedNaviMTLBuffer alloc] initWithDevice:dev length:65536 options:0]; if (zeros) memset(rn_vk_buffer_contents(zeros->b), 0, 65536); });
                if (!zeros) return [NSString stringWithFormat:@"no buffer bound at index %u", m];
                av[i].buffer = zeros->b;
                av[i].offset = 0;
                break;
            }
        case RN_AIR_COLOR_INPUT: break;                      // framebuffer fetch: the render target, set by the pass (runRenderPass)
        case RN_AIR_NULL_TEXTURE: {
            // a placeholder the translator adds for a texture that is absent in this variant: a texture of zeros of the dimension the shader declares (2D 1 x 1, or 3D 1 x 1 x 2), made once
            if (!(av[i].image = rn_null_image(kind[i] >> 8))) return @"no memory for a placeholder texture";
            break;
        }
        case RN_VK_SAMPLED_IMAGE:
        case RN_VK_STORAGE_IMAGE:
            if (m < RN_MTL_SLOTS && texs[m]) { rn_vk_image *dv = rn_display_vram_peek((__bridge void *)texs[m]); av[i].image = dv ? dv : texs[m]->img; if (rn_diag && getenv("RN_EXP_NULL_F0") && m == 0 && (kind[i] & 0xff) == RN_VK_SAMPLED_IMAGE && texs[m]->w == 256) { static rn_vk_image *z; if (!z) z = rn_vk_image_create(rn_vk_shared(), RN_VK_BGRA8, 256, 256); av[i].image = z; } break; }
            // Nothing bound: AppleMetalOpenGLRenderer leaves a texture unit of its fixed-function shaders unbound when no GL texture is there (2026-11-21); Metal does not check it.
            // A sampled image gets a texture of zeros of the shader's dimension.
            if ((kind[i] & 0xff) == RN_VK_SAMPLED_IMAGE && (av[i].image = rn_null_image(kind[i] >> 8))) break;
            return [NSString stringWithFormat:@"no texture bound at index %u", m];
        case RN_VK_SAMPLER:
            if (m < RN_MTL_SLOTS && samps[m]) { av[i].sampler = samps[m]->s; break; }
            {   // nothing bound (an unbound GL texture unit, as for textures above): a nearest, clamp-to-edge sampler, made once
                static rn_vk_sampler *def;
                static dispatch_once_t once;
                dispatch_once(&once, ^{ rn_vk *vk = rn_vk_shared(); rn_vk_sampler_desc sd = {0, 0, 0, {1, 1, 1}, 0, 0, 0.0f, 1000.0f}; def = vk ? rn_vk_sampler_create(vk, &sd) : NULL; });
                if (def) { av[i].sampler = def; break; }
            }
            return [NSString stringWithFormat:@"no sampler state bound at index %u", m];
        default: break;
        }
    }
    return nil;
}

// One recorded dispatch.
@interface RNDispatch : NSObject {
@public
    RedNaviMTLComputePipelineState *pso;
    RedNaviMTLBuffer *bufs[RN_MTL_SLOTS];
    uint64_t offs[RN_MTL_SLOTS];
    NSData *bytesAt[RN_MTL_SLOTS];
    RedNaviMTLTexture *texs[RN_MTL_SLOTS];
    RedNaviMTLSamplerState *samps[RN_MTL_SLOTS];
    uint8_t push[128];
    uint32_t pushLen;
    MTLSize groups, threads, tpt;     // threads: the whole grid in threads
}
@end
@implementation RNDispatch
@end

// ---------------------------------------------------------------------------------------------------------------------------------------- render pipelines and passes

// A render pipeline: the translated modules and their bindings are kept, the Vulkan pipeline is made per primitive type (Vulkan fixes the topology in the pipeline, Metal passes it
// with the draw); the triangle list is made at once so that a module RADV refuses is found when the state is created.

// MTLVertexFormat to a VkFormat number (0: not supported). Small integer formats become the "scaled" Vulkan formats (the shader reads floats, as Metal converts them);
// 32-bit integers stay integers.
static unsigned rn_vk_vertex_format(MTLVertexFormat f) {
    switch (f) {
    case MTLVertexFormatUChar: return 11;           case MTLVertexFormatUChar2: return 18;          case MTLVertexFormatUChar3: return 25;          case MTLVertexFormatUChar4: return 39;
    case MTLVertexFormatChar: return 12;            case MTLVertexFormatChar2: return 19;           case MTLVertexFormatChar3: return 26;           case MTLVertexFormatChar4: return 40;
    case MTLVertexFormatUCharNormalized: return 9;  case MTLVertexFormatUChar2Normalized: return 16; case MTLVertexFormatUChar3Normalized: return 23; case MTLVertexFormatUChar4Normalized: return 37;
    case MTLVertexFormatCharNormalized: return 10;  case MTLVertexFormatChar2Normalized: return 17; case MTLVertexFormatChar3Normalized: return 24; case MTLVertexFormatChar4Normalized: return 38;
    case MTLVertexFormatUChar4Normalized_BGRA: return 44;
    case MTLVertexFormatUShort: return 72;          case MTLVertexFormatUShort2: return 79;         case MTLVertexFormatUShort3: return 86;         case MTLVertexFormatUShort4: return 93;
    case MTLVertexFormatShort: return 73;           case MTLVertexFormatShort2: return 80;          case MTLVertexFormatShort3: return 87;          case MTLVertexFormatShort4: return 94;
    case MTLVertexFormatUShortNormalized: return 70; case MTLVertexFormatUShort2Normalized: return 77; case MTLVertexFormatUShort3Normalized: return 84; case MTLVertexFormatUShort4Normalized: return 91;
    case MTLVertexFormatShortNormalized: return 71; case MTLVertexFormatShort2Normalized: return 78; case MTLVertexFormatShort3Normalized: return 85; case MTLVertexFormatShort4Normalized: return 92;
    case MTLVertexFormatHalf: return 76;            case MTLVertexFormatHalf2: return 83;           case MTLVertexFormatHalf3: return 90;           case MTLVertexFormatHalf4: return 97;
    case MTLVertexFormatFloat: return 100;          case MTLVertexFormatFloat2: return 103;         case MTLVertexFormatFloat3: return 106;         case MTLVertexFormatFloat4: return 109;
    case MTLVertexFormatInt: return 99;             case MTLVertexFormatInt2: return 102;           case MTLVertexFormatInt3: return 105;           case MTLVertexFormatInt4: return 108;
    case MTLVertexFormatUInt: return 98;            case MTLVertexFormatUInt2: return 101;          case MTLVertexFormatUInt3: return 104;          case MTLVertexFormatUInt4: return 107;
    default: return 0;
    }
}

// The numeric type of each vertex shader input by its Location, read from the SPIR-V: 0 float (or not found), 1 unsigned, 2 signed integer. Metal converts a vertex attribute to the
// type the shader declares (UChar into a uint input stays an integer, into a float input becomes a float); Vulkan takes the type from the format, so an integer input needs the
// UINT or SINT format where rn_vk_vertex_format gives the scaled one (Kitty's cell attributes through the OpenGL renderer are UChar and UInt into uint inputs).
static void rn_spv_input_types(NSData *spv, uint8_t types[16]) {
    memset(types, 0, 16);
    const uint32_t *w = spv.bytes; const size_t n = spv.length / 4;
    if (n < 5 || w[0] != 0x07230203) return;
    const uint32_t bound = w[3];
    if (!bound || bound > (1u << 22)) return;
    uint32_t *loc = calloc(bound, 4), *ptr = calloc(bound, 4), *kind = calloc(bound, 4);   // loc: Location + 1; ptr: pointee of an Input pointer type; kind: 1 uint, 2 int, 3 float, or a vector's component type + 0x100
    for (size_t i = 5; loc && ptr && kind && i < n; ) {
        const uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > n) break;
        if (op == 71 && wc >= 4 && w[i + 2] == 30 && w[i + 1] < bound) loc[w[i + 1]] = w[i + 3] + 1;                 // OpDecorate id Location n
        else if (op == 21 && wc >= 4 && w[i + 1] < bound) kind[w[i + 1]] = w[i + 3] ? 2 : 1;                         // OpTypeInt id width signedness
        else if (op == 22 && wc >= 3 && w[i + 1] < bound) kind[w[i + 1]] = 3;                                        // OpTypeFloat
        else if (op == 23 && wc >= 4 && w[i + 1] < bound && w[i + 2] < bound) kind[w[i + 1]] = 0x100 | (kind[w[i + 2]] & 0xff);   // OpTypeVector id component count
        else if (op == 32 && wc >= 4 && w[i + 2] == 1 && w[i + 1] < bound) ptr[w[i + 1]] = w[i + 3] + 1;             // OpTypePointer id Input type
        i += wc;
    }
    // OpVariable (result type, id, storage class Input 1) comes after the decorations and types, in a second pass
    for (size_t i = 5; loc && ptr && kind && i < n; ) {
        const uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > n) break;
        if (op == 59 && wc >= 4 && w[i + 3] == 1 && w[i + 1] < bound && w[i + 2] < bound && loc[w[i + 2]] && loc[w[i + 2]] <= 16 && ptr[w[i + 1]]) {
            const uint32_t t = ptr[w[i + 1]] - 1, k = t < bound ? kind[t] & 0xff : 0;
            types[loc[w[i + 2]] - 1] = k == 1 ? 1 : k == 2 ? 2 : 0;
        }
        i += wc;
    }
    free(loc); free(ptr); free(kind);
}
// The integer variant of a scaled VkFormat (USCALED + 2 is UINT, SSCALED + 2 is SINT, for 8- and 16-bit formats), or the format itself.
static unsigned rn_vk_vertex_format_int(unsigned vf) {
    switch (vf) {
    case 11: case 12: case 18: case 19: case 25: case 26: case 39: case 40: case 72: case 73: case 79: case 80: case 86: case 87: case 93: case 94: return vf + 2;
    default: return vf;
    }
}

// MTLBlendFactor / MTLBlendOperation / MTLColorWriteMask to Vulkan's values.
static int rn_vk_blend_factor(MTLBlendFactor f, unsigned *out) {
    switch (f) {
    case MTLBlendFactorZero: *out = 0; return 1;                       case MTLBlendFactorOne: *out = 1; return 1;
    case MTLBlendFactorSourceColor: *out = 2; return 1;                case MTLBlendFactorOneMinusSourceColor: *out = 3; return 1;
    case MTLBlendFactorDestinationColor: *out = 4; return 1;           case MTLBlendFactorOneMinusDestinationColor: *out = 5; return 1;
    case MTLBlendFactorSourceAlpha: *out = 6; return 1;                case MTLBlendFactorOneMinusSourceAlpha: *out = 7; return 1;
    case MTLBlendFactorDestinationAlpha: *out = 8; return 1;           case MTLBlendFactorOneMinusDestinationAlpha: *out = 9; return 1;
    case MTLBlendFactorBlendColor: *out = 10; return 1;                case MTLBlendFactorOneMinusBlendColor: *out = 11; return 1;
    case MTLBlendFactorBlendAlpha: *out = 12; return 1;                case MTLBlendFactorOneMinusBlendAlpha: *out = 13; return 1;
    case MTLBlendFactorSourceAlphaSaturated: *out = 14; return 1;
    default: return 0;
    }
}
static int rn_vk_blend_op(MTLBlendOperation o, unsigned *out) {
    switch (o) {
    case MTLBlendOperationAdd: *out = 0; return 1;                     case MTLBlendOperationSubtract: *out = 1; return 1;
    case MTLBlendOperationReverseSubtract: *out = 2; return 1;         case MTLBlendOperationMin: *out = 3; return 1;
    case MTLBlendOperationMax: *out = 4; return 1;
    default: return 0;
    }
}
static unsigned rn_vk_write_mask(MTLColorWriteMask m) {
    return ((m & MTLColorWriteMaskRed) ? 1u : 0) | ((m & MTLColorWriteMaskGreen) ? 2u : 0) | ((m & MTLColorWriteMaskBlue) ? 4u : 0) | ((m & MTLColorWriteMaskAlpha) ? 8u : 0);
}

@interface RedNaviMTLRenderPipelineState : NSObject {
@public
    NSData *spv[2];                                 // vertex, fragment
    unsigned n[2], kind[2][RN_VK_MAX_BINDINGS], mi[2][RN_VK_MAX_BINDINGS];
    rn_vk_binding binds[2][RN_VK_MAX_BINDINGS];
    rn_vk_sampler_desc sampl[2][RN_VK_MAX_BINDINGS];
    unsigned nt;
    int tfmt[8];
    int dfmt;                                        // RN_VK_D16, D32 or D32S8 when the pipeline has a depth or stencil attachment
    rn_vk_blend blend[8];
    NSString *names[2];                              // vertex and fragment function names (for logs)
    NSString *variant[2];                            // per stage: the function constants it was specialised with and the kinds of its bindings (pass dumps)
    unsigned nvattr, nvlayout;
    rn_vk_vattr vattr[16];
    rn_vk_vlayout vlayout[16];
    rn_vk_render *byKey[5 * 6];                     // by primitive type, cull mode and winding

    NSArray<NSDictionary *> *args[2];                // per stage: the arguments for the reflection (rn_render_reflection)

    id<MTLDevice> dev;
    NSString *label_;
}
- (rn_vk_render *)renderForPrimitive:(unsigned)prim cull:(unsigned)cull clockwise:(BOOL)cw;
@end
@implementation RedNaviMTLRenderPipelineState
+ (void)load { class_addProtocol(self, @protocol(MTLRenderPipelineState)); }
- (void)dealloc { for (int i = 0; i < 5 * 6; i++) if (byKey[i]) rn_vk_render_destroy(rn_vk_shared(), byKey[i]); }
- (id<MTLDevice>)device { return dev; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 0; }
- (BOOL)threadgroupSizeMatchesTileSize { return NO; }
- (NSUInteger)imageblockSampleLength { return 0; }
- (BOOL)supportsIndirectCommandBuffers { return NO; }
- (MTLResourceID)gpuResourceID { MTLResourceID r = {0}; return r; }
- (rn_vk_render *)renderForPrimitive:(unsigned)prim cull:(unsigned)cull clockwise:(BOOL)cw {
    if (prim > 4 || cull > 2) return NULL;
    const unsigned key = prim * 6 + cull * 2 + (cw ? 1 : 0);
    if (byKey[key]) return byKey[key];
    rn_vk *vk = rn_vk_shared();
    if (!vk) return NULL;
    rn_vk_render_desc d;
    memset(&d, 0, sizeof(d));
    d.vs = spv[0].bytes; d.vs_bytes = spv[0].length; d.fs = spv[1].bytes; d.fs_bytes = spv[1].length;
    d.nvbind = n[0]; d.vbinds = binds[0]; d.nfbind = n[1]; d.fbinds = binds[1];
    d.depth_format = dfmt; d.ntargets = nt; d.topology = prim; d.cull_mode = cull; d.front_clockwise = cw;
    for (unsigned i = 0; i < nt; i++) { d.target_formats[i] = tfmt[i]; d.blend[i] = blend[i]; }
    d.nvattr = nvattr; d.vattrs = vattr; d.nvlayout = nvlayout; d.vlayouts = vlayout;
    byKey[key] = rn_vk_render_create(vk, &d);
    return byKey[key];
}
@end

// One recorded draw: the pipeline and what was bound to each stage at that time.
@interface RNDraw : NSObject {
@public
    RedNaviMTLRenderPipelineState *pso;
    RedNaviMTLBuffer *bufs[2][RN_MTL_SLOTS];
    uint64_t offs[2][RN_MTL_SLOTS];
    NSData *bytesAt[2][RN_MTL_SLOTS];
    RedNaviMTLTexture *texs[2][RN_MTL_SLOTS];
    RedNaviMTLSamplerState *samps[2][RN_MTL_SLOTS];
    BOOL hasViewport, hasScissor, hasBlendColor;
    float blendColor[4];
    double viewport[6];
    uint32_t scissor[4];
    unsigned prim, vertexStart, vertexCount, instanceCount, baseInstance;
    unsigned cull;                                   // MTLCullMode
    BOOL clockwise;
    rn_vk_ds ds;                                     // depth stencil state at the time of the draw
    RedNaviMTLBuffer *indexBuf;                      // indexed draws: vertexCount is the index count
    uint64_t indexOffset;
    BOOL index32;
    int32_t baseVertex;
}
@end
@implementation RNDraw
@end

// A depth stencil state: the compare functions and stencil operations in Vulkan's numbers (they are Metal's). The stencil reference values are the encoder's, put in when a draw is recorded.
@interface RedNaviMTLDepthStencilState : NSObject {
@public
    rn_vk_ds ds;
    id<MTLDevice> dev;
    NSString *label_;
}
@end
@implementation RedNaviMTLDepthStencilState
+ (void)load { class_addProtocol(self, @protocol(MTLDepthStencilState)); }
- (id<MTLDevice>)device { return dev; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
@end

// Selectors that Metal and its clients send to the plug-in's objects and that the plug-in does not implement (private SPI of the Apple classes: -[MTLTexture protectionOptions] from
// QuartzCore's MetalContext, found with ca_renderer_test): an unrecognized selector throws, which ends the process, so they are answered with a zero of the right kind and logged. The
// kind comes from the Metal protocols (public and SPI) when one declares the selector, else it is an integer. Struct results are not handled (a zero in the integer registers).
static uintptr_t rn_stub_int(id self, SEL _cmd) { return 0; }
static float rn_stub_float(id self, SEL _cmd) { return 0; }
static double rn_stub_double(id self, SEL _cmd) { return 0; }
static const char *rn_selector_types(SEL sel) {
    static const char *const protos[] = {"MTLTexture", "MTLTextureSPI", "MTLResource", "MTLResourceSPI", "MTLBuffer", "MTLBufferSPI", "MTLCommandQueue", "MTLCommandQueueSPI", "MTLCommandBuffer",
        "MTLCommandBufferSPI", "MTLCommandEncoder", "MTLRenderCommandEncoder", "MTLRenderCommandEncoderSPI", "MTLComputeCommandEncoder", "MTLBlitCommandEncoder", "MTLLibrary", "MTLFunction",
        "MTLRenderPipelineState", "MTLComputePipelineState", "MTLSamplerState", "MTLDepthStencilState", "MTLSharedEvent", "MTLEvent", "MTLDrawable", "MTLHeap", "MTLHeapSPI"};
    for (unsigned i = 0; i < sizeof(protos) / sizeof(protos[0]); i++) {
        Protocol *p = objc_getProtocol(protos[i]);
        if (!p) continue;
        for (int req = 1; req >= 0; req--) {
            struct objc_method_description d = protocol_getMethodDescription(p, sel, req, YES);
            if (d.name && d.types) return d.types;
        }
    }
    return NULL;
}
static BOOL rn_resolve_default(id self, SEL _cmd, SEL sel) {
    // -originalObject belongs to Metal's tool and capture wrappers (as for the device in plugin.m): AppleMetalOpenGLRenderer asks respondsToSelector: and then talks to the answer, so
    // a stub answering nil sent its texture bindings to nil (2026-11-21). Not answering it makes the caller use the object itself.
    if (sel == sel_registerName("originalObject")) return NO;
    const char *types = rn_selector_types(sel);
    const char *r = types ? types : "Q16@0:8";
    while (*r == 'r' || *r == 'n' || *r == 'N' || *r == 'o' || *r == 'O' || *r == 'R' || *r == 'V') r++;
    IMP imp = (r[0] == 'f') ? (IMP)rn_stub_float : (r[0] == 'd') ? (IMP)rn_stub_double : (IMP)rn_stub_int;
    class_addMethod((Class)self, sel, imp, types ? types : "Q16@0:8");
    rn_log("no implementation of -[%s %s]: answering zero%s",class_getName((Class)self), sel_getName(sel), types ? "" : " (no protocol declares it)");
    return YES;
}
static void rn_add_default_resolver(Class c) { class_addMethod(object_getClass(c), @selector(resolveInstanceMethod:), (IMP)rn_resolve_default, "c@::"); }

// A render pass: its colour attachments with their load actions and the draws, run when the command buffer is committed.
@interface RNRenderPass : NSObject {
@public
    RedNaviMTLTexture *tex[8];
    BOOL clear[8], store[8];
    float clearColor[8][4];
    unsigned nt;
    RedNaviMTLTexture *depthTex;                     // depth and stencil attachment, if any
    BOOL clearDepth, clearStencil;
    float depthValue;
    uint32_t stencilValue;
    NSMutableArray<RNDraw *> *draws;
}
@end
@implementation RNRenderPass
@end

@class RedNaviMTLCommandBuffer;

@interface RedNaviMTLComputeCommandEncoder : NSObject {
    RedNaviMTLCommandBuffer *cb;
    RedNaviMTLComputePipelineState *pso;
    RedNaviMTLBuffer *bufs[RN_MTL_SLOTS];
    uint64_t offs[RN_MTL_SLOTS];
    NSData *bytesAt[RN_MTL_SLOTS];
    RedNaviMTLTexture *texs[RN_MTL_SLOTS];
    RedNaviMTLSamplerState *samps[RN_MTL_SLOTS];
    uint8_t push[128];
    uint32_t pushLen;
    NSString *label_;
    BOOL ended;
}
- (instancetype)initWithCommandBuffer:(RedNaviMTLCommandBuffer *)c;
@end


// ---------------------------------------------------------------------------------------------------------------------------------------- events
// MTLEvent / MTLSharedEvent (docs/M6_WINDOWSERVER_STUDY.md: SkyLight and QuartzCore order GPU work with them). The value is a 64-bit integer in a POSIX shared memory object, so that a
// second process that gets the event's handle sees the same value; waiting and listeners poll it. A command buffer runs synchronously at commit, so encodeSignalEvent:value: sets the
// value at its place in the order of the work and encodeWaitForEvent:value: blocks the thread that commits until the value is reached (10 s at most, then the command buffer fails).
@interface RedNaviSharedEventHandle : MTLSharedEventHandle <NSSecureCoding> {
@public
    NSString *shmName;
    NSString *label_;
}
@end
@implementation RedNaviSharedEventHandle
+ (BOOL)supportsSecureCoding { return YES; }
- (NSString *)label { return label_; }
- (void)encodeWithCoder:(NSCoder *)c { [c encodeObject:shmName forKey:@"shm"]; [c encodeObject:label_ forKey:@"label"]; }
- (instancetype)initWithCoder:(NSCoder *)c {
    if ((self = [super init])) { shmName = [c decodeObjectOfClass:[NSString class] forKey:@"shm"]; label_ = [c decodeObjectOfClass:[NSString class] forKey:@"label"]; }
    return self;
}
@end

@interface RedNaviMTLSharedEvent : NSObject {
@public
    id<MTLDevice> dev;
    NSString *label_;
    NSString *shmName;
    _Atomic uint64_t *val;
    BOOL owner;
}
- (instancetype)initWithDevice:(id<MTLDevice>)d shmName:(NSString *)name create:(BOOL)create;
- (BOOL)waitFor:(uint64_t)value timeoutMS:(uint64_t)ms;
@end
@implementation RedNaviMTLSharedEvent
+ (void)load { class_addProtocol(self, @protocol(MTLSharedEvent)); class_addProtocol(self, @protocol(MTLEvent)); }
- (instancetype)initWithDevice:(id<MTLDevice>)d shmName:(NSString *)name create:(BOOL)create {
    if (!(self = [super init])) return nil;
    static _Atomic unsigned counter;
    if (create) name = [NSString stringWithFormat:@"/rnev.%d.%u", getpid(), atomic_fetch_add(&counter, 1)];
    const int fd = shm_open(name.UTF8String, create ? (O_CREAT | O_EXCL | O_RDWR) : O_RDWR, 0600);
    if (fd < 0) { rn_log("shared event %s: shm_open failed (%d)", name.UTF8String, errno); return nil; }
    if (create && ftruncate(fd, 4096) != 0) { close(fd); shm_unlink(name.UTF8String); return nil; }
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { if (create) shm_unlink(name.UTF8String); return nil; }
    val = p; shmName = name; owner = create; dev = d;
    return self;
}
- (void)dealloc { if (val) munmap((void *)val, 4096); if (owner) shm_unlink(shmName.UTF8String); }
- (id<MTLDevice>)device { return dev; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
- (uint64_t)signaledValue { return atomic_load(val); }
- (void)setSignaledValue:(uint64_t)v { atomic_store(val, v); }
- (BOOL)waitFor:(uint64_t)value timeoutMS:(uint64_t)ms {
    const uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + ms * 1000000ull;
    while (atomic_load(val) < value) {
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline) return NO;
        usleep(200);
    }
    return YES;
}
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds { return [self waitFor:value timeoutMS:milliseconds]; }
- (void)notifyListener:(MTLSharedEventListener *)listener atValue:(uint64_t)value block:(MTLSharedEventNotificationBlock)block {
    dispatch_queue_t q = listener.dispatchQueue ?: dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);
    MTLSharedEventNotificationBlock b = [block copy];
    // a polling thread per notification; it ends when the value is reached or after a minute
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        if ([self waitFor:value timeoutMS:60000]) dispatch_async(q, ^{ b((id<MTLSharedEvent>)self, atomic_load(self->val)); });
    });
}
- (MTLSharedEventHandle *)newSharedEventHandle {
    RedNaviSharedEventHandle *h = [RedNaviSharedEventHandle new];
    h->shmName = shmName; h->label_ = label_;
    return h;
}
@end

// A record of a command buffer: signal an event or wait for it.
@interface RNEventOp : NSObject {
@public
    RedNaviMTLSharedEvent *event;
    uint64_t value;
    BOOL wait;
}
@end
@implementation RNEventOp
@end

@interface RedNaviMTLCommandBuffer : NSObject {
@public
    id<MTLCommandQueue> queue;
    id<MTLDevice> dev;
    NSMutableArray *records;                    // RNDispatch and RNRenderPass in the order they were encoded
    NSMutableArray *completed, *scheduled;
    NSMutableArray *presents;                   // drawables to present once the command buffer is done (CAMetalLayer)
    MTLCommandBufferStatus st;
    unsigned directPasses;                      // passes of this command buffer that rendered into the scanout
    size_t scanW, scanH;                        // size of the surface scanSrc belongs to
    rn_vk_image *scanSrc;                       // the card's copy of the surface the last such pass finished: copied into the scanout when the command buffer ends
    BOOL ran;                                   // the body has started
    BOOL scanCopied;                            // the scanout copy was recorded in the batch
    BOOL handlersRun;                           // the completed handlers have been called
    BOOL useCounted;                            // rednavi-usecount: the surfaces of queuedTex had their use count raised at the commit
    NSArray *queuedTex;                       // the textures counted as in use from the commit until the body has run (rn_tex_wait_idle)
    NSError *err;
    NSString *label_;
    dispatch_semaphore_t done;
    NSMutableDictionary *userDict_;             // MPS keeps its per-command-buffer state here
}
- (NSMutableDictionary *)userDictionary;
- (instancetype)initWithQueue:(id<MTLCommandQueue>)q device:(id<MTLDevice>)d;
- (void)runRenderPass:(RNRenderPass *)rp;
- (void)fail:(NSString *)why;
- (void)commitBody;
@end

@implementation RedNaviMTLComputeCommandEncoder
+ (void)load { class_addProtocol(self, @protocol(MTLComputeCommandEncoder)); class_addProtocol(self, @protocol(MTLCommandEncoder)); }
- (instancetype)initWithCommandBuffer:(RedNaviMTLCommandBuffer *)c { if ((self = [super init])) cb = c; return self; }
- (id<MTLDevice>)device { return cb->dev; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
- (MTLDispatchType)dispatchType { return MTLDispatchTypeSerial; }
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state { pso = (RedNaviMTLComputePipelineState *)state; }
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (!rn_slot_ok(index, "buffer")) return;
    bufs[index] = (RedNaviMTLBuffer *)buffer;
    offs[index] = offset;
    bytesAt[index] = nil;
}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained [])buffers offsets:(const NSUInteger [])offsets withRange:(NSRange)range {
    for (NSUInteger i = 0; i < range.length; i++) [self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}
// Constant data. A hand-made SPIR-V pipeline has push constants at one index; for a translated kernel every [[buffer(n)]] is a storage buffer, also the one the app fills with
// setBytes, so the bytes are kept and turned into a small buffer when the command buffer runs.
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    if (!rn_slot_ok(index, "buffer")) return;
    if (pso && index == pso->pushIndex) {
        if (length > sizeof(push)) length = sizeof(push);
        memcpy(push, bytes, length);
        pushLen = (uint32_t)length;
        return;
    }
    bytesAt[index] = [NSData dataWithBytes:bytes length:length];
    bufs[index] = nil;
}
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    if (rn_slot_ok(index, "texture")) texs[index] = (RedNaviMTLTexture *)texture;
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index { rn_log("setThreadgroupMemoryLength: not implemented, ignored (kernels with threadgroup memory arguments are refused by the translator)"); }
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained [])textures withRange:(NSRange)range {
    for (NSUInteger i = 0; i < range.length; i++) [self setTexture:textures[i] atIndex:range.location + i];
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    if (rn_slot_ok(index, "sampler")) samps[index] = (RedNaviMTLSamplerState *)sampler;
}
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])samplers withRange:(NSRange)range {
    for (NSUInteger i = 0; i < range.length; i++) [self setSamplerState:samplers[i] atIndex:range.location + i];
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler lodMinClamp:(float)lodMinClamp lodMaxClamp:(float)lodMaxClamp atIndex:(NSUInteger)index { [self setSamplerState:sampler atIndex:index]; }
- (void)record:(MTLSize)groups grid:(MTLSize)threads threads:(MTLSize)tpt {
    if (!pso) { rn_log("dispatch without a pipeline state"); return; }
    RNDispatch *d = [RNDispatch new];
    d->pso = pso;
    for (unsigned i = 0; i < RN_MTL_SLOTS; i++) { d->bufs[i] = bufs[i]; d->offs[i] = offs[i]; d->bytesAt[i] = bytesAt[i]; d->texs[i] = texs[i]; d->samps[i] = samps[i]; }
    memcpy(d->push, push, sizeof(push));
    d->pushLen = pushLen;
    d->groups = groups;
    d->threads = threads;
    d->tpt = tpt;
    [cb->records addObject:d];
}
- (void)dispatchThreadgroups:(MTLSize)tg threadsPerThreadgroup:(MTLSize)tpt {
    [self record:tg grid:MTLSizeMake(tg.width * tpt.width, tg.height * tpt.height, tg.depth * tpt.depth) threads:tpt];
}
- (void)dispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)tpt {
    NSUInteger w = tpt.width ? tpt.width : 1, h = tpt.height ? tpt.height : 1, dd = tpt.depth ? tpt.depth : 1;
    MTLSize g = {(threads.width + w - 1) / w, (threads.height + h - 1) / h, (threads.depth + dd - 1) / dd};
    [self record:g grid:threads threads:tpt];
}
- (void)endEncoding { ended = YES; }
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope {}
- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count {}
- (void)insertDebugSignpost:(NSString *)string {}
- (void)pushDebugGroup:(NSString *)string {}
- (void)popDebugGroup {}
@end


// ---------------------------------------------------------------------------------------------------------------------------------------- blit encoder
// Copies and fills are done on the CPU at commit, in order with the passes and dispatches: buffers are host memory and a texture's host copy is current after every pass (the
// render and compute paths copy what the GPU wrote back into it), so a blit between them is a memcpy and the destination texture is marked as written by the host (it is uploaded
// before its next use on the GPU). Textures are 2D with one level, so slices and levels other than 0 are errors.
@interface RNBlit : NSObject {
@public
    int kind;                                        // 0 buffer to texture, 1 texture to buffer, 2 texture to texture, 3 buffer to buffer, 4 fill
    RedNaviMTLBuffer *sb, *db;
    RedNaviMTLTexture *st, *dt;
    NSUInteger soff, doff, sbpr, dbpr, size;
    MTLOrigin so, dorg;
    MTLSize sz;
    uint8_t value;
}
@end
@implementation RNBlit
@end

static rn_vk_image *rn_display_vram_get(rn_vk *vk, RedNaviMTLTexture *rt, BOOL create);
static NSString *rn_run_blit(RNBlit *b) {
    // Render passes of the same command buffer are only recorded until the batch ends; a blit that reads or writes a buffer on the host runs now, so what was recorded before it must have run first.
    if (b->kind != 2) { rn_vk *bvk = rn_vk_shared(); if (bvk) rn_vk_batch_sync(bvk); }
    switch (b->kind) {
    case 0: case 1: {
        RedNaviMTLTexture *t = b->kind == 0 ? b->dt : b->st;
        RedNaviMTLBuffer *bf = b->kind == 0 ? b->sb : b->db;
        const MTLOrigin o = b->kind == 0 ? b->dorg : b->so;
        const NSUInteger bpr = b->kind == 0 ? b->sbpr : b->dbpr, off = b->kind == 0 ? b->soff : b->doff;
        if (!t || !bf) {
            static int said; if (said < 40) { said++; rn_log("blit kind %d failed: texture %p (alen %lu, %lux%lu fmt %lu type %ld), buffer %p, region %lux%lu at (%lu,%lu), bpr %lu", b->kind, (__bridge void *)t, t ? (unsigned long)t->alen : 0ul, t ? (unsigned long)t->w : 0ul, t ? (unsigned long)t->h : 0ul, t ? (unsigned long)t->pf : 0ul, t ? (long)t->ttype : -1l, (__bridge void *)bf, (unsigned long)b->sz.width, (unsigned long)b->sz.height, (unsigned long)o.x, (unsigned long)o.y, (unsigned long)bpr); }
            return @"blit: a texture or buffer is missing";
        }
        if (rn_diag && getenv("RN_LOG_ARRAYS") && t->ttype == MTLTextureType2DArray) rn_log("blit kind %d: 2D array %lux%lux%lu, region %lux%lux%lu at (%lu,%lu,%lu), buffer %p offset %lu bpr %lu, first bytes %08x", b->kind, (unsigned long)t->w, (unsigned long)t->h, (unsigned long)t->d, (unsigned long)b->sz.width, (unsigned long)b->sz.height, (unsigned long)b->sz.depth, (unsigned long)o.x, (unsigned long)o.y, (unsigned long)o.z, (__bridge void *)bf, (unsigned long)off, (unsigned long)bpr, *(const uint32_t *)((const uint8_t *)rn_vk_buffer_contents(bf->b) + off + 6 * bpr + 8));
        if (o.z || b->sz.depth > 1 || o.x + b->sz.width > t->w || o.y + b->sz.height > rn_tex_rows(t)) return @"blit: the region is outside the texture";
        if (!b->sz.width || !b->sz.height) return nil;
        if (bpr < b->sz.width * t->texel) return @"blit: bytesPerRow is smaller than a row of the region";
        if (off + (b->sz.height - 1) * bpr + b->sz.width * t->texel > bf->len) return @"blit: the region is outside the buffer";
        rn_vk_image *card = b->kind == 1 ? rn_display_vram_peek((__bridge void *)t) : NULL;      // reading a scanout surface: its pixels are in the card's copy
        uint8_t *tex = rn_vk_image_staging(card ? card : t->img), *buf = (uint8_t *)rn_vk_buffer_contents(bf->b) + off;
        const size_t tpitch = rn_vk_image_row_bytes(card ? card : t->img);
        for (NSUInteger y = 0; y < b->sz.height; y++) {
            uint8_t *tp = tex + (o.y + y) * tpitch + o.x * t->texel, *bp = buf + y * bpr;
            if (b->kind == 0) memcpy(tp, bp, b->sz.width * t->texel); else memcpy(bp, tp, b->sz.width * t->texel);
        }
        if (b->kind == 0) { rn_vk_image_host_wrote(t->img); rn_display_host_write_note((__bridge void *)t, "a blit from a buffer"); }
        return nil;
    }
    case 2: {
        RedNaviMTLTexture *s2 = b->st, *d2 = b->dt;
        if (!s2 || !d2) return @"blit: a texture is missing";
        if (s2->texel != d2->texel) return @"blit: the textures have different texel sizes";
        if (b->so.z || b->dorg.z || b->sz.depth > 1 || b->so.x + b->sz.width > s2->w || b->so.y + b->sz.height > rn_tex_rows(s2) || b->dorg.x + b->sz.width > d2->w || b->dorg.y + b->sz.height > rn_tex_rows(d2))
            return @"blit: the region is outside a texture";
        {   // A CoreDisplay scanout surface lives on the card (direct scanout renders into its VRAM copy): a copy into it, or out of it into a surface the CPU reads, is done there. A memmove on the
            // host copy would leave the picture the card draws on unchanged, and the pixels the blit meant to carry over stay as they were.
            RedNaviMTLTexture *sr = s2->parent ? s2->parent : s2, *dr = d2->parent ? d2->parent : d2;
            if ((dr->displayScan || sr->displayScan) && s2->pf == d2->pf) {
                rn_vk *vk = rn_vk_shared();
                rn_vk_image *si = sr->displayScan ? rn_display_vram_get(vk, sr, NO) : NULL, *di = dr->displayScan ? rn_display_vram_get(vk, dr, YES) : NULL;
                if (!si && sr->displayScan) si = rn_display_vram_get(vk, sr, YES);
                if (!si) si = sr->img;
                if (!di && (dr->zeroCopy)) di = dr->img;
                if (si && di && (dr->displayScan || dr->zeroCopy)) {
                    const int rc = rn_vk_image_copy_region(vk, si, di, (uint32_t)b->so.x, (uint32_t)b->so.y, (uint32_t)b->dorg.x, (uint32_t)b->dorg.y, (uint32_t)b->sz.width, (uint32_t)b->sz.height);
                    static unsigned said; if (said++ < 12) rn_log("blit between display surfaces on the card: %lux%lu from (%lu,%lu) to (%lu,%lu), result %d", (unsigned long)b->sz.width, (unsigned long)b->sz.height, (unsigned long)b->so.x, (unsigned long)b->so.y, (unsigned long)b->dorg.x, (unsigned long)b->dorg.y, rc);
                    if (rc == 0) return nil;
                }
            }
        }
        rn_vk_image *scard = rn_display_vram_peek((__bridge void *)s2);
        const uint8_t *sp = rn_vk_image_staging(scard ? scard : s2->img);
        uint8_t *dp = rn_vk_image_staging(d2->img);
        const size_t spitch = rn_vk_image_row_bytes(scard ? scard : s2->img), dpitch = rn_vk_image_row_bytes(d2->img);     // a zero-copy IOSurface image has the surface's row pitch
        for (NSUInteger y = 0; y < b->sz.height; y++) memmove(dp + (b->dorg.y + y) * dpitch + b->dorg.x * d2->texel, sp + (b->so.y + y) * spitch + b->so.x * s2->texel, b->sz.width * s2->texel);
        rn_vk_image_host_wrote(d2->img);
        rn_display_host_write_note((__bridge void *)d2, "a blit on the host");
        return nil;
    }
    case 3:
        if (!b->sb || !b->db || b->soff + b->size > b->sb->len || b->doff + b->size > b->db->len) return @"blit: a buffer range is outside its buffer";
        memmove((uint8_t *)rn_vk_buffer_contents(b->db->b) + b->doff, (uint8_t *)rn_vk_buffer_contents(b->sb->b) + b->soff, b->size);
        return nil;
    case 4:
        if (!b->db || b->doff + b->size > b->db->len) return @"fillBuffer: the range is outside the buffer";
        memset((uint8_t *)rn_vk_buffer_contents(b->db->b) + b->doff, b->value, b->size);
        return nil;
    }
    return @"blit: unknown operation";
}

@interface RedNaviMTLBlitCommandEncoder : NSObject {
    RedNaviMTLCommandBuffer *cb;
    NSString *label_;
    BOOL ended;
}
- (instancetype)initWithCommandBuffer:(RedNaviMTLCommandBuffer *)c;
@end
@implementation RedNaviMTLBlitCommandEncoder
+ (void)load { class_addProtocol(self, @protocol(MTLBlitCommandEncoder)); class_addProtocol(self, @protocol(MTLCommandEncoder)); }
- (instancetype)initWithCommandBuffer:(RedNaviMTLCommandBuffer *)c { if ((self = [super init])) cb = c; return self; }
- (id<MTLDevice>)device { return cb->dev; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
- (RNBlit *)add:(int)kind {
    RNBlit *b = [RNBlit new];
    b->kind = kind;
    [cb->records addObject:b];
    return b;
}
- (void)copyFromBuffer:(id<MTLBuffer>)buf sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)tex
      destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin {
    RNBlit *b = [self add:0];
    b->sb = (RedNaviMTLBuffer *)buf; b->soff = so; b->sbpr = bpr; b->dt = (RedNaviMTLTexture *)tex; b->dorg = origin; b->sz = size;
    if (level) { b->dt = nil; rn_log("blit buffer->texture: level %lu asked, slice %lu", (unsigned long)level, (unsigned long)slice); }                           // reported as an error at commit
    else if (slice) { if (!rn_slice_rows(b->dt, slice, &b->dorg)) b->dt = nil; }   // a slice of a 1D array is a row of the image, one of a 2D array h rows
}
- (void)copyFromBuffer:(id<MTLBuffer>)buf sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)tex
      destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin options:(MTLBlitOption)options {
    [self copyFromBuffer:buf sourceOffset:so sourceBytesPerRow:bpr sourceBytesPerImage:bpi sourceSize:size toTexture:tex destinationSlice:slice destinationLevel:level destinationOrigin:origin];
}
- (void)copyFromTexture:(id<MTLTexture>)tex sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)buf
      destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi {
    RNBlit *b = [self add:1];
    b->st = (RedNaviMTLTexture *)tex; b->so = origin; b->sz = size; b->db = (RedNaviMTLBuffer *)buf; b->doff = doff; b->dbpr = bpr;
    if (level) b->st = nil;
    else if (slice) { if (!rn_slice_rows(b->st, slice, &b->so)) b->st = nil; }
}
- (void)copyFromTexture:(id<MTLTexture>)tex sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)buf
      destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi options:(MTLBlitOption)options {
    [self copyFromTexture:tex sourceSlice:slice sourceLevel:level sourceOrigin:origin sourceSize:size toBuffer:buf destinationOffset:doff destinationBytesPerRow:bpr destinationBytesPerImage:bpi];
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst
       destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg {
    RNBlit *b = [self add:2];
    b->st = (RedNaviMTLTexture *)src; b->so = so; b->sz = size; b->dt = (RedNaviMTLTexture *)dst; b->dorg = dorg;
    if (sl || dl) b->dt = nil;
    else {
        if (ss && !rn_slice_rows(b->st, ss, &b->so)) b->dt = nil;
        if (ds && !rn_slice_rows(b->dt, ds, &b->dorg)) b->dt = nil;
    }
}
- (void)copyFromTexture:(id<MTLTexture>)src toTexture:(id<MTLTexture>)dst {
    [self copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(src.width, src.height, 1) toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl toTexture:(id<MTLTexture>)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl sliceCount:(NSUInteger)n levelCount:(NSUInteger)lc {
    if (ss || sl || ds || dl || n != 1 || lc != 1) { RNBlit *b = [self add:2]; b->st = nil; return; }
    [self copyFromTexture:src toTexture:dst];
}
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)dst destinationOffset:(NSUInteger)doff size:(NSUInteger)size {
    RNBlit *b = [self add:3];
    b->sb = (RedNaviMTLBuffer *)src; b->soff = so; b->db = (RedNaviMTLBuffer *)dst; b->doff = doff; b->size = size;
}
- (void)fillBuffer:(id<MTLBuffer>)buf range:(NSRange)range value:(uint8_t)value {
    RNBlit *b = [self add:4];
    b->db = (RedNaviMTLBuffer *)buf; b->doff = range.location; b->size = range.length; b->value = value;
}
// nothing to do: there are no mipmap levels, and host and device copies are kept in step by the command buffer
- (void)generateMipmapsForTexture:(id<MTLTexture>)texture {}
- (void)synchronizeResource:(id<MTLResource>)resource {}
- (void)synchronizeTexture:(id<MTLTexture>)texture slice:(NSUInteger)slice level:(NSUInteger)level {}
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)texture {}
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)texture slice:(NSUInteger)slice level:(NSUInteger)level {}
- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)texture {}
- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)texture slice:(NSUInteger)slice level:(NSUInteger)level {}
- (void)updateFence:(id<MTLFence>)fence {}
- (void)waitForFence:(id<MTLFence>)fence {}
- (void)endEncoding { ended = YES; }
- (void)insertDebugSignpost:(NSString *)string {}
- (void)pushDebugGroup:(NSString *)string {}
- (void)popDebugGroup {}
@end


@interface RedNaviMTLRenderCommandEncoder : NSObject {
    RedNaviMTLCommandBuffer *cb;
    RNRenderPass *pass;
    RedNaviMTLRenderPipelineState *pso;
    RedNaviMTLBuffer *bufs[2][RN_MTL_SLOTS];
    uint64_t offs[2][RN_MTL_SLOTS];
    NSData *bytesAt[2][RN_MTL_SLOTS];
    RedNaviMTLTexture *texs[2][RN_MTL_SLOTS];
    RedNaviMTLSamplerState *samps[2][RN_MTL_SLOTS];
    BOOL hasViewport, hasScissor, hasBlendColor;
    float blendColor[4];
    double viewport[6];
    uint32_t scissor[4];
    RedNaviMTLDepthStencilState *dss;                // the depth stencil state set, if any
    uint32_t stencilRef[2];                          // front and back reference values
    unsigned cullMode;                               // MTLCullMode, default none
    BOOL counterClockwise;                           // MTLWinding, default clockwise
    NSString *label_;
    BOOL ended;
}
- (instancetype)initWithCommandBuffer:(RedNaviMTLCommandBuffer *)c pass:(RNRenderPass *)p;
@end
@implementation RedNaviMTLRenderCommandEncoder
+ (void)load { class_addProtocol(self, @protocol(MTLRenderCommandEncoder)); class_addProtocol(self, @protocol(MTLCommandEncoder)); }
- (instancetype)initWithCommandBuffer:(RedNaviMTLCommandBuffer *)c pass:(RNRenderPass *)p { if ((self = [super init])) { cb = c; pass = p; } return self; }
- (id<MTLDevice>)device { return cb->dev; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state { pso = (RedNaviMTLRenderPipelineState *)state; }
- (void)bindBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o index:(NSUInteger)i stage:(int)st { if ((rn_diag && getenv("RN_LOG_BUFFERS"))) rn_log("trace: set%sBuffer %p off %lu idx %lu", st ? "Fragment" : "Vertex", b, (unsigned long)o, (unsigned long)i); if (rn_slot_ok(i, "buffer")) { bufs[st][i] = (RedNaviMTLBuffer *)b; offs[st][i] = o; bytesAt[st][i] = nil; } }
- (void)bindBytes:(const void *)bytes length:(NSUInteger)len index:(NSUInteger)i stage:(int)st { if (rn_slot_ok(i, "buffer")) { bytesAt[st][i] = [NSData dataWithBytes:bytes length:len]; bufs[st][i] = nil; } }
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index { [self bindBuffer:buffer offset:offset index:index stage:0]; }
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained [])buffers offsets:(const NSUInteger [])offsets withRange:(NSRange)range {
    for (NSUInteger i = 0; i < range.length; i++) [self bindBuffer:buffers[i] offset:offsets[i] index:range.location + i stage:0];
}
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index { if ((rn_diag && getenv("RN_LOG_BUFFERS"))) rn_log("trace: setVertexBufferOffset off %lu idx %lu", (unsigned long)offset, (unsigned long)index); if (index < RN_MTL_SLOTS) offs[0][index] = offset; }
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index { [self bindBytes:bytes length:length index:index stage:0]; }
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index { [self bindBuffer:buffer offset:offset index:index stage:1]; }
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained [])buffers offsets:(const NSUInteger [])offsets withRange:(NSRange)range {
    for (NSUInteger i = 0; i < range.length; i++) [self bindBuffer:buffers[i] offset:offsets[i] index:range.location + i stage:1];
}
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index { if ((rn_diag && getenv("RN_LOG_BUFFERS"))) rn_log("trace: setFragmentBufferOffset off %lu idx %lu", (unsigned long)offset, (unsigned long)index); if (index < RN_MTL_SLOTS) offs[1][index] = offset; }
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index { [self bindBytes:bytes length:length index:index stage:1]; }
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { if (rn_diag && getenv("RN_LOG_BUFFERS")) rn_log("trace: setVertexTexture %p (%s) idx %lu", t, t ? class_getName(object_getClass(t)) : "-", (unsigned long)i); if (rn_slot_ok(i, "texture")) texs[0][i] = (RedNaviMTLTexture *)t; }
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { if (rn_diag && getenv("RN_LOG_BUFFERS")) rn_log("trace: setFragmentTexture %p (%s) idx %lu", t, t ? class_getName(object_getClass(t)) : "-", (unsigned long)i); if (rn_slot_ok(i, "texture")) texs[1][i] = (RedNaviMTLTexture *)t; }
- (void)setVertexTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r { for (NSUInteger i = 0; i < r.length; i++) [self setVertexTexture:t[i] atIndex:r.location + i]; }
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r { for (NSUInteger i = 0; i < r.length; i++) [self setFragmentTexture:t[i] atIndex:r.location + i]; }
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { if (rn_slot_ok(i, "sampler")) samps[0][i] = (RedNaviMTLSamplerState *)s; }
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { if (rn_slot_ok(i, "sampler")) samps[1][i] = (RedNaviMTLSamplerState *)s; }
- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s withRange:(NSRange)r { for (NSUInteger i = 0; i < r.length; i++) [self setVertexSamplerState:s[i] atIndex:r.location + i]; }
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s withRange:(NSRange)r { for (NSUInteger i = 0; i < r.length; i++) [self setFragmentSamplerState:s[i] atIndex:r.location + i]; }
// The LOD clamps of the encoder (OpenGL's GL_TEXTURE_MIN_LOD/MAX_LOD) are not applied: the sampler's own clamps are used.
- (void)setVertexSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i { [self setVertexSamplerState:s atIndex:i]; }
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i { [self setFragmentSamplerState:s atIndex:i]; }
- (void)setViewport:(MTLViewport)v {
    hasViewport = YES;
    viewport[0] = v.originX; viewport[1] = v.originY; viewport[2] = v.width; viewport[3] = v.height; viewport[4] = v.znear; viewport[5] = v.zfar;
}
- (void)setScissorRect:(MTLScissorRect)r { hasScissor = YES; scissor[0] = (uint32_t)r.x; scissor[1] = (uint32_t)r.y; scissor[2] = (uint32_t)r.width; scissor[3] = (uint32_t)r.height; }
// The arrayed forms: one viewport and one scissor are kept (no viewport index in the shaders), so the first of each is used, as setViewport: / setScissorRect: would set it.
- (void)setViewports:(const MTLViewport [])v count:(NSUInteger)n { if (n) [self setViewport:v[0]]; if (n > 1) [self ignored:"viewports beyond the first"]; }
- (void)setScissorRects:(const MTLScissorRect [])r count:(NSUInteger)n { if (n) [self setScissorRect:r[0]]; if (n > 1) [self ignored:"scissor rects beyond the first"]; }
// state that is not implemented yet is logged once and ignored: there is no depth test
- (void)ignored:(const char *)what { if (!rn_diag) return; static NSMutableSet *seen; if (!seen) seen = [NSMutableSet new]; NSString *k = @(what); if (![seen containsObject:k]) { [seen addObject:k]; rn_log("render encoder: %s is not implemented, ignored", what); } }
- (void)setDepthClipMode:(MTLDepthClipMode)m { if (m != MTLDepthClipModeClip) [self ignored:"depth clip mode (clamp)"]; }
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)m offset:(NSUInteger)o { if (m != MTLVisibilityResultModeDisabled) [self ignored:"visibility result mode (occlusion queries)"]; }
- (void)textureBarrier { [self ignored:"textureBarrier"]; }
- (void)setCullMode:(MTLCullMode)m { cullMode = (unsigned)m; }
- (void)setFrontFacingWinding:(MTLWinding)w { counterClockwise = w == MTLWindingCounterClockwise; }
- (void)setTriangleFillMode:(MTLTriangleFillMode)m { if (m != MTLTriangleFillModeFill) [self ignored:"triangle fill mode"]; }
- (void)setDepthStencilState:(id<MTLDepthStencilState>)s { dss = (RedNaviMTLDepthStencilState *)s; }
- (void)setDepthBias:(float)d slopeScale:(float)s clamp:(float)c {}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a { hasBlendColor = YES; blendColor[0] = r; blendColor[1] = g; blendColor[2] = b; blendColor[3] = a; }
- (void)setStencilReferenceValue:(uint32_t)v { stencilRef[0] = stencilRef[1] = v; }
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b { stencilRef[0] = f; stencilRef[1] = b; }
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances baseInstance:(NSUInteger)base {
    if (!pso) { rn_log("draw without a render pipeline state"); return; }
    RNDraw *d = [RNDraw new];
    d->pso = pso;
    for (int s = 0; s < 2; s++) for (int i = 0; i < RN_MTL_SLOTS; i++) { d->bufs[s][i] = bufs[s][i]; d->offs[s][i] = offs[s][i]; d->bytesAt[s][i] = bytesAt[s][i]; d->texs[s][i] = texs[s][i]; d->samps[s][i] = samps[s][i]; }
    d->hasViewport = hasViewport; memcpy(d->viewport, viewport, sizeof(viewport));
    d->hasScissor = hasScissor; memcpy(d->scissor, scissor, sizeof(scissor));
    d->hasBlendColor = hasBlendColor; memcpy(d->blendColor, blendColor, sizeof(blendColor));
    d->prim = (unsigned)type; d->vertexStart = (unsigned)start; d->vertexCount = (unsigned)count; d->instanceCount = (unsigned)instances; d->baseInstance = (unsigned)base;
    d->cull = cullMode; d->clockwise = !counterClockwise;
    if (dss) { d->ds = dss->ds; d->ds.front.ref = stencilRef[0]; d->ds.back.ref = stencilRef[1]; }
    if ((rn_diag && getenv("RN_LOG_BUFFERS"))) {                                 // does anything rewrite the vertex data of the earlier draws while the rest is encoded?
        static NSMutableArray *snaps; if (!snaps) snaps = [NSMutableArray new];
        static unsigned drawNo;
        for (NSMutableDictionary *sn in snaps) {
            RedNaviMTLBuffer *sb = sn[@"buf"];
            if (!sb || !sb->b) continue;
            NSData *old = sn[@"data"]; const uint8_t *bp = (const uint8_t *)rn_vk_buffer_contents(sb->b) + [sn[@"off"] unsignedLongValue];
            if (memcmp(bp, old.bytes, old.length)) {
                rn_log("  REWRITTEN: draw #%u's vertex data (buffer %p offset %lu) changed before draw #%u was encoded", [sn[@"n"] unsignedIntValue], sb, [sn[@"off"] unsignedLongValue], drawNo);
                sn[@"data"] = [NSData dataWithBytes:bp length:old.length];
            }
        }
        if (d->bufs[0][1]) [snaps addObject:[@{@"buf": d->bufs[0][1], @"off": @(d->offs[0][1]), @"n": @(drawNo), @"data": [NSData dataWithBytes:(const uint8_t *)rn_vk_buffer_contents(d->bufs[0][1]->b) + d->offs[0][1] length:96]} mutableCopy]];
        drawNo++;
        char bl[400] = ""; int o2 = 0;
        for (int st = 0; st < 2; st++) for (int i = 0; i < RN_MTL_SLOTS; i++) { if (d->bufs[st][i] && o2 < 360) o2 += snprintf(bl + o2, sizeof(bl) - o2, " %c%d=%p+%lu", st ? 'f' : 'v', i, d->bufs[st][i], (unsigned long)d->offs[st][i]); else if (d->bytesAt[st][i] && o2 < 360) o2 += snprintf(bl + o2, sizeof(bl) - o2, " %c%d=bytes(%lu)", st ? 'f' : 'v', i, (unsigned long)d->bytesAt[st][i].length); }
        rn_log("  encode: draw #%u binds:%s", drawNo - 1, bl);
    }
    if ((rn_diag && getenv("RN_LOG_BUFFERS")) && d->bufs[0][1]) {                // the same bytes as at run time, to tell whether the arena is rewritten in between
        const uint8_t *bp = (const uint8_t *)rn_vk_buffer_contents(d->bufs[0][1]->b) + d->offs[0][1];
        char hx[100] = ""; for (int j = 0; j < 32; j++) snprintf(hx + 2 * j, 4, "%02x", bp[j]);
        rn_log("  encode: draw %lu of the pass, v buffer 1 %p offset %lu: %s", (unsigned long)pass->draws.count, d->bufs[0][1], (unsigned long)d->offs[0][1], hx);
    }
    [pass->draws addObject:d];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)offset
                instanceCount:(NSUInteger)instances baseVertex:(NSInteger)baseVertex baseInstance:(NSUInteger)base {
    if (!pso) { rn_log("indexed draw without a render pipeline state"); return; }
    [self drawPrimitives:type vertexStart:0 vertexCount:count instanceCount:instances baseInstance:base];
    RNDraw *d = pass->draws.lastObject;
    d->indexBuf = (RedNaviMTLBuffer *)indexBuffer; d->indexOffset = offset; d->index32 = indexType == MTLIndexTypeUInt32; d->baseVertex = (int32_t)baseVertex;
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)offset instanceCount:(NSUInteger)instances {
    [self drawIndexedPrimitives:type indexCount:count indexType:indexType indexBuffer:indexBuffer indexBufferOffset:offset instanceCount:instances baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)offset {
    [self drawIndexedPrimitives:type indexCount:count indexType:indexType indexBuffer:indexBuffer indexBufferOffset:offset instanceCount:1 baseVertex:0 baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances { [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances baseInstance:0]; }
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count { [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:1 baseInstance:0]; }
- (void)endEncoding { if (ended) return; ended = YES; [cb->records addObject:pass]; }
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope afterStages:(MTLRenderStages)a beforeStages:(MTLRenderStages)b {}
- (void)insertDebugSignpost:(NSString *)string {}
- (void)pushDebugGroup:(NSString *)string {}
- (void)popDebugGroup {}
@end

// Time spent in -commit (the work runs on the caller's thread): a line every five seconds, to see where a slow client waits.
// Where a commit spends its time (logged with the 5 s line): 0 whole render passes, 1 the Vulkan part of them, 2 waiting for the card, 3 copying surfaces and buffers in, 4 caller buffers in and out,
// 5 render pipelines made (translation and Vulkan), 6 the completed handlers. Ticks of mach_absolute_time.
extern uint64_t rn_vk_fence_ticks;
extern uint64_t rn_vk_bufstat[6];
extern uint64_t rn_vk_imgstat[3];
static _Atomic uint64_t g_ph[8];
static inline void rn_ph(int i, uint64_t t0) { atomic_fetch_add(&g_ph[i], mach_absolute_time() - t0); }
static _Atomic uint64_t g_frames, g_framegapmax, g_framelast, g_framelong, g_passes;   // frames that reached the scanout, the longest gap between two (ticks), gaps over 12 ms, render passes
// Measurement: a frame that comes from the same card copy as the one before it. CoreDisplay goes round its ring, so two in a row from one surface mean that one frame of the
// compositor was spread over two command buffers, and the first copy put an unfinished picture on the screen. Counted in all, and those less than 4 ms after the one before.
static _Atomic uint64_t g_framesame, g_framesamefast;
static _Atomic(void *) g_framesrc;
static void rn_frame_note(void *src) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    const uint64_t now = mach_absolute_time(), last = atomic_exchange(&g_framelast, now);
    const BOOL same = atomic_exchange(&g_framesrc, src) == src;
    atomic_fetch_add(&g_frames, 1);
    if (!last) return;
    const uint64_t gap = (now - last) * tb.numer / tb.denom;          // ns
    if (same) { atomic_fetch_add(&g_framesame, 1); if (gap < 4000000ull) atomic_fetch_add(&g_framesamefast, 1); }
    uint64_t m = atomic_load(&g_framegapmax);
    while (gap > m && !atomic_compare_exchange_weak(&g_framegapmax, &m, gap)) {}
    if (gap > 12000000ull) atomic_fetch_add(&g_framelong, 1);
}
static void rn_commit_stats(uint64_t ticks) {
    if (!rn_diag) return;                                // the statistics are diagnostics: only with /Users/Shared/rednavi-log
    static mach_timebase_info_data_t tb;
    static uint64_t sum, worst, count, since;
    static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    if (!tb.denom) mach_timebase_info(&tb);
    os_unfair_lock_lock(&lock);
    sum += ticks; count++; if (ticks > worst) worst = ticks;
    const uint64_t now = mach_absolute_time();
    if (!since) since = now;
    if ((now - since) * tb.numer / tb.denom > 5000000000ull) {
        { double ph[8]; for (int i = 0; i < 8; i++) ph[i] = (double)atomic_exchange(&g_ph[i], 0) * tb.numer / tb.denom / 1e6; ph[2] = (double)__atomic_exchange_n(&rn_vk_fence_ticks, 0, __ATOMIC_RELAXED) * tb.numer / tb.denom / 1e6;
          rn_log("commit time in ms: render passes %.0f (vulkan %.0f, card wait %.0f), surfaces in %.0f, caller buffers %.0f, pipelines %.0f, handlers %.0f", ph[0], ph[1], ph[2], ph[3], ph[4], ph[5], ph[6]); }
        { uint64_t bs[6]; for (int i = 0; i < 5; i++) bs[i] = __atomic_exchange_n(&rn_vk_bufstat[i], 0, __ATOMIC_RELAXED); bs[5] = rn_vk_bufstat[5];
          rn_log("buffer cache: %llu hits, %llu small misses, %llu big (largest %llu bytes), %llu kept, %llu dropped", bs[0], bs[1], bs[2], bs[5], bs[3], bs[4]); }
        { uint64_t is[3]; for (int i = 0; i < 3; i++) is[i] = __atomic_exchange_n(&rn_vk_imgstat[i], 0, __ATOMIC_RELAXED);
          rn_log("images: %llu reused from the pool, %llu made, %llu kept on free", is[0], is[1], is[2]); }
        rn_log("frames: %llu reached the scanout in 5 s, longest gap %.1f ms, %llu gaps over 12 ms, %llu render passes, %llu from the same surface as the frame before (%llu of them within 4 ms)", (unsigned long long)atomic_exchange(&g_frames, 0), (double)atomic_exchange(&g_framegapmax, 0) / 1e6, (unsigned long long)atomic_exchange(&g_framelong, 0), (unsigned long long)atomic_exchange(&g_passes, 0), (unsigned long long)atomic_exchange(&g_framesame, 0), (unsigned long long)atomic_exchange(&g_framesamefast, 0));
        rn_log("commit: %llu command buffers in 5 s, %.2f ms each on average, %.2f ms at worst; textures alive %ld (%lu MB), made so far %ld", count, (double)sum * tb.numer / tb.denom / 1e6 / count, (double)worst * tb.numer / tb.denom / 1e6, (long)g_tex_live, (unsigned long)g_tex_bytes >> 20, (long)g_tex_made);
        sum = worst = count = 0; since = now;
        rn_sites_report();
    }
    os_unfair_lock_unlock(&lock);
}

// Pass-by-pass capture (touch /Users/Shared/rednavi-passdump-arm: each touch makes one capture per process): every render pass of the next frame
// that has a target of 48 pixels or more on both sides is written as /private/var/tmp/rnpass/<n>_<w>x<h>.png (its first target as the card has it after the pass, read back through the host
// copy), with a line in index.txt (shaders of the first and last draw, formats, load and store, count of NaN and Inf texels of a 16-bit float target; those texels are drawn magenta and cyan).
// The next 80 passes, in the order they ran (a window that redraws, like Activity Monitor with its graphs, gives its own passes, then the final pass into the scanout).
static int g_pd_left; static unsigned g_pd_seq;
// The last source bytes rn_srcdump read, per IOSurface (RGBA, rows packed), for rn_cmpdump.
static struct { unsigned id; uint8_t *px; size_t w, h, pitch; BOOL compared; } g_src_cache[8]; static unsigned g_src_next;
static pthread_mutex_t g_src_lock = PTHREAD_MUTEX_INITIALIZER;   // commit bodies of several queues can reach the capture code at once
static float rn_half(uint16_t h) {
    const uint32_t e = (h >> 10) & 31, m = h & 1023; float f;
    if (!e) f = ldexpf((float)m, -24); else if (e == 31) f = m ? NAN : INFINITY; else f = ldexpf((float)(m | 1024), (int)e - 25);
    return (h & 0x8000) ? -f : f;
}
static char g_pd_dir[64];
// Called by rn_vk once the batch has run: the target of pass <seq> after draw <draw>, as <dir>/snap_<seq>_d<draw>.png.
static void rn_snap_write(unsigned seq, unsigned draw, uint32_t w, uint32_t h, const uint8_t *bgra) {
    if (!g_pd_dir[0]) return;
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    for (size_t i = 0; i < (size_t)w * h; i++) { px[i * 4] = bgra[i * 4 + 2]; px[i * 4 + 1] = bgra[i * 4 + 1]; px[i * 4 + 2] = bgra[i * 4]; px[i * 4 + 3] = 255; }
    if (w == h && w <= 256) {   // a shadow pass: the target with its alpha, as the card has it after this draw
        char rp_[128]; snprintf(rp_, sizeof(rp_), "%s/snap_%03u_d%03u_%ux%u.raw", g_pd_dir, seq, draw, w, h);
        FILE *rf = fopen(rp_, "wb"); if (rf) { fwrite(bgra, 4, (size_t)w * h, rf); fclose(rf); chmod(rp_, 0644); }
    }
    char path[128]; snprintf(path, sizeof(path), "%s/snap_%03u_d%03u.png", g_pd_dir, seq, draw);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, px, (size_t)w * h * 4, NULL);
    CGImageRef im = CGImageCreate(w, h, 8, 32, (size_t)w * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp, NULL, false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dst = im ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    if (dst) { CGImageDestinationAddImage(dst, im, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); chmod(path, 0644); }
    CFRelease(url); if (im) CGImageRelease(im); CGDataProviderRelease(dp); CGColorSpaceRelease(cs);
    free(px);
}
// A texture whose card copy differs from its host copy, as <dir>/tex_<pass>_d<draw>.png: host copy on the left, card copy on the right, eight times larger (one byte per texel as grey,
// two as red and green, four as colour on the top row and alpha as grey on the bottom row).
static void rn_tex_pair_png(unsigned seq, unsigned draw, uint32_t w, uint32_t h, unsigned texel, const uint8_t *hc, const uint8_t *cc) {
    if (!g_pd_dir[0] || !w || !h || (texel != 1 && texel != 2 && texel != 4)) return;
    const unsigned S = 8, G = 16, rows = texel == 4 ? 2 : 1;
    const size_t W = (size_t)w * S * 2 + G, H = (size_t)h * S * rows;
    uint8_t *px = calloc(W * H, 4);
    if (!px) return;
    for (int side = 0; side < 2; side++) {
        const uint8_t *src = side ? cc : hc;
        for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) {
            const uint8_t *t = src + (y * w + x) * texel; uint8_t c[4] = {0, 0, 0, 255}, a[4] = {0, 0, 0, 255};
            if (texel == 1) { c[0] = c[1] = c[2] = t[0]; }
            else if (texel == 2) { c[0] = t[0]; c[1] = t[1]; }
            else { c[0] = t[2]; c[1] = t[1]; c[2] = t[0]; a[0] = a[1] = a[2] = t[3]; }
            for (unsigned r = 0; r < rows; r++) for (unsigned dy = 0; dy < S; dy++) for (unsigned dx = 0; dx < S; dx++) {
                uint8_t *d = px + (((y + r * h) * S + dy) * W + (side ? w * S + G : 0) + x * S + dx) * 4;
                memcpy(d, r ? a : c, 4);
            }
        }
    }
    char path[128]; snprintf(path, sizeof(path), "%s/tex_%03u_d%03u.png", g_pd_dir, seq, draw);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, px, W * H * 4, NULL);
    CGImageRef im = CGImageCreate(W, H, 8, 32, W * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp, NULL, false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dst = im ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    if (dst) { CGImageDestinationAddImage(dst, im, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); chmod(path, 0644); }
    CFRelease(url); if (im) CGImageRelease(im); CGDataProviderRelease(dp); CGColorSpaceRelease(cs);
    free(px);
}
static int g_pd_small = 400;        // passes of small targets that may still be dumped (they do not count against g_pd_left)
static BOOL rn_passdump_on(void) {
    // /Users/Shared/rednavi-passdump-live: while the file exists the budgets are refilled whenever they run out, so a long gesture (dragging a window, 60 frames a second) is captured whole.
    // The captures are large (a few MB a frame): create the file, do the gesture for a few seconds, remove it.
    if (!rn_diag) return NO;                // a stat every 20 ms of passes otherwise
    static struct timespec lastLive; struct timespec nowt; clock_gettime(CLOCK_MONOTONIC, &nowt);
    if (g_pd_left <= 0 && (nowt.tv_sec - lastLive.tv_sec) * 1000 + (nowt.tv_nsec - lastLive.tv_nsec) / 1000000 >= 20) {
        lastLive = nowt;
        struct stat lst;
        if (stat("/Users/Shared/rednavi-passdump-live", &lst) == 0) {
            if (!g_pd_dir[0]) {
                mkdir("/private/var/tmp/rnpass", 0755); chmod("/private/var/tmp/rnpass", 0755);
                snprintf(g_pd_dir, sizeof(g_pd_dir), "/private/var/tmp/rnpass/%d", (int)getpid());
                mkdir(g_pd_dir, 0755); chmod(g_pd_dir, 0755);
                g_pd_seq = 0; rn_vk_snap_hook = rn_snap_write;
            }
            g_pd_left = 240; g_pd_small = 400;
        }
    }
    if (g_pd_left > 0) return YES;
    // A touch of the file arms one capture: the file's modification time is compared with the one this process last acted on (WindowServer runs as _windowserver and cannot remove
    // a file of the user in /Users/Shared). Each process writes into its own directory, /private/var/tmp/rnpass/<pid>.
    static time_t seen; struct stat st;
    if (stat("/Users/Shared/rednavi-passdump-arm", &st) != 0 || st.st_mtime == seen) return NO;
    seen = st.st_mtime;
    mkdir("/private/var/tmp/rnpass", 0755); chmod("/private/var/tmp/rnpass", 0755);
    snprintf(g_pd_dir, sizeof(g_pd_dir), "/private/var/tmp/rnpass/%d", (int)getpid());
    mkdir(g_pd_dir, 0755); chmod(g_pd_dir, 0755);
    g_pd_left = 240; g_pd_small = 400; g_pd_seq = 0;
    rn_vk_snap_hook = rn_snap_write;
    return YES;
}
static void rn_passdump(RNRenderPass *rp, rn_vk_image *img, NSUInteger nd) {
    RedNaviMTLTexture *t = rp->tex[0];
    if (!t || !img || t->w < 48 || t->h < 48) return;
    const uint8_t *src = rn_vk_image_staging(img);
    if (!src) return;
    const size_t n = t->w * t->h;
    const MTLPixelFormat pf = t->pf;
    uint8_t *px = malloc(n * 4);
    if (!px) return;
    unsigned long nan = 0, inf = 0; BOOL ok = YES;
    if (pf == MTLPixelFormatBGRA8Unorm || pf == MTLPixelFormatBGRA8Unorm_sRGB || pf == MTLPixelFormatRGBA8Unorm || pf == MTLPixelFormatRGBA8Unorm_sRGB) {
        const BOOL bgra = pf == MTLPixelFormatBGRA8Unorm || pf == MTLPixelFormatBGRA8Unorm_sRGB;
        for (size_t i = 0; i < n; i++) { px[4*i] = src[4*i + (bgra ? 2 : 0)]; px[4*i+1] = src[4*i+1]; px[4*i+2] = src[4*i + (bgra ? 0 : 2)]; px[4*i+3] = 255; }
    } else if (pf == MTLPixelFormatRGBA16Float) {
        const uint16_t *h = (const uint16_t *)src;
        for (size_t i = 0; i < n; i++) {
            float c[4]; for (int k = 0; k < 4; k++) c[k] = rn_half(h[4*i + k]);
            if (c[0] != c[0] || c[1] != c[1] || c[2] != c[2]) { nan++; px[4*i] = 255; px[4*i+1] = 0; px[4*i+2] = 255; px[4*i+3] = 255; continue; }
            if (isinf(c[0]) || isinf(c[1]) || isinf(c[2])) { inf++; px[4*i] = 0; px[4*i+1] = 255; px[4*i+2] = 255; px[4*i+3] = 255; continue; }
            for (int k = 0; k < 3; k++) { float v = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k]; px[4*i + k] = (uint8_t)(v * 255.f + .5f); }
            px[4*i+3] = 255;
        }
    } else if (pf == MTLPixelFormatR8Unorm) {
        for (size_t i = 0; i < n; i++) { px[4*i] = px[4*i+1] = px[4*i+2] = src[i]; px[4*i+3] = 255; }
    } else ok = NO;
    char path[96]; snprintf(path, sizeof(path), "%s/%03u_%lux%lu.png", g_pd_dir, g_pd_seq, (unsigned long)t->w, (unsigned long)t->h);
    if (ok) {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, px, n * 4, NULL);
        CGImageRef im = CGImageCreate(t->w, t->h, 8, 32, t->w * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp, NULL, false, kCGRenderingIntentDefault);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
        CGImageDestinationRef dst = im ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
        if (dst) { CGImageDestinationAddImage(dst, im, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); chmod(path, 0644); }
        CFRelease(url); if (im) CGImageRelease(im); CGDataProviderRelease(dp); CGColorSpaceRelease(cs);
    }
    if (t->w == t->h && t->w >= 93 && t->w <= 256 && (pf == MTLPixelFormatBGRA8Unorm || pf == MTLPixelFormatBGRA8Unorm_sRGB || pf == MTLPixelFormatRGBA8Unorm)) {   // the shadow shapes: the PNG drops the alpha, so the target as the card has it
        char rpth[112]; snprintf(rpth, sizeof(rpth), "%s/%03u_%lux%lu_target.raw", g_pd_dir, g_pd_seq, (unsigned long)t->w, (unsigned long)t->h);
        FILE *rf = fopen(rpth, "wb"); if (rf) { fwrite(src, 4, n, rf); fclose(rf); chmod(rpth, 0644); }
    }
    if (ok && t->surf && (pf == MTLPixelFormatBGRA8Unorm || pf == MTLPixelFormatBGRA8Unorm_sRGB)) rn_cmpdump(t->surf, t->w, t->h, px, g_pd_seq);
    free(px);
    RNDraw *d0 = nd ? rp->draws[0] : nil, *d1 = nd ? rp->draws[nd - 1] : nil;
    char ipath[96]; snprintf(ipath, sizeof(ipath), "%s/index.txt", g_pd_dir);
    FILE *f = fopen(ipath, "a");
    if (f) {
        fprintf(f, "%03u %lux%lu pf %lu %s: %lu draws, first %s + %s, last %s + %s, blend %d, targets %u, clear %d store %d, surf %u%s%s\n", g_pd_seq, (unsigned long)t->w, (unsigned long)t->h, (unsigned long)pf, ok ? "png" : "FORMAT NOT DUMPED", (unsigned long)nd,
                d0 && d0->pso && d0->pso->names[0] ? d0->pso->names[0].UTF8String : "-", d0 && d0->pso && d0->pso->names[1] ? d0->pso->names[1].UTF8String : "-",
                d1 && d1->pso && d1->pso->names[0] ? d1->pso->names[0].UTF8String : "-", d1 && d1->pso && d1->pso->names[1] ? d1->pso->names[1].UTF8String : "-",
                d0 && d0->pso ? d0->pso->blend[0].enabled : 0, rp->nt, rp->clear[0], rp->store[0], t->surf ? (unsigned)IOSurfaceGetID(t->surf) : 0u, nan ? " NaN" : "", inf ? " Inf" : "");
        if (nan || inf) fprintf(f, "    %lu NaN texels, %lu Inf texels\n", nan, inf);
        fclose(f); chmod(ipath, 0644);
    }
    {   // every draw of the pass: shaders, the rectangle it covers (viewport and scissor), blend, and the textures it samples (size, format, IOSurface), as <n>_draws.txt
        char dpath[112]; snprintf(dpath, sizeof(dpath), "%s/%03u_draws.txt", g_pd_dir, g_pd_seq);
        FILE *df = fopen(dpath, "w");
        if (df) {
            for (NSUInteger k = 0; k < nd; k++) {
                RNDraw *d = rp->draws[k];
                int x0 = 0, y0 = 0, x1 = (int)t->w, y1 = (int)t->h;
                if (d->hasViewport) { x0 = (int)d->viewport[0]; y0 = (int)d->viewport[1]; x1 = (int)(d->viewport[0] + d->viewport[2]); y1 = (int)(d->viewport[1] + d->viewport[3]); }
                if (d->hasScissor) { int a = (int)d->scissor[0], b = (int)d->scissor[1], c = a + (int)d->scissor[2], e = b + (int)d->scissor[3]; if (a > x0) x0 = a; if (b > y0) y0 = b; if (c < x1) x1 = c; if (e < y1) y1 = e; }
                fprintf(df, "%3lu %s + %s [%s ; %s] rect (%d,%d)-(%d,%d) blend %d %u verts x %u inst:", (unsigned long)k, d->pso && d->pso->names[0] ? d->pso->names[0].UTF8String : "-", d->pso && d->pso->names[1] ? d->pso->names[1].UTF8String : "-", d->pso && d->pso->variant[0] ? d->pso->variant[0].UTF8String : "-", d->pso && d->pso->variant[1] ? d->pso->variant[1].UTF8String : "-",
                        x0, y0, x1, y1, d->pso ? d->pso->blend[0].enabled : 0, (unsigned)d->vertexCount, (unsigned)d->instanceCount);
                for (int st = 0; st < 2; st++) for (int i = 0; i < RN_MTL_SLOTS; i++) {
                    RedNaviMTLTexture *tx = d->texs[st][i];
                    if (tx) fprintf(df, " %c%d=%lux%lu/pf%lu/surf%u%s", st ? 'f' : 'v', i, (unsigned long)tx->w, (unsigned long)tx->h, (unsigned long)tx->pf, tx->surf ? (unsigned)IOSurfaceGetID(tx->surf) : 0u, tx->img && rn_vk_image_is_host(tx->img) ? "H" : "");
                }
                fputc('\n', df);
                const BOOL shadowDraw = d->pso && ((d->pso->names[0] && [d->pso->names[0] containsString:@"Shadow"]) || (d->pso->names[1] && [d->pso->names[1] containsString:@"Shadow"]) || (t->w == t->h && (t->w == 93 || t->w == 137 || t->w == 161 || t->w == 174 || t->w == 249 || t->w == 256)));   // the window shadow: its draws, and every draw into a square target of shadow size
                if (d->pso && (shadowDraw || (d->pso->variant[1] && ([d->pso->variant[1] containsString:@"fc_image_function0=0a"] || [d->pso->variant[1] containsString:@"fc_image_function0=0b"] || [d->pso->variant[1] containsString:@"fc_image_function0=a"])))) {
                    // the analytic (border and disc) draws, whole, for an offline replay: <n>_d<k>.bin = u32 magic RND1, u32 vertices, u32 instances, u32 blend, i32 x0 y0 x1 y1, u32 target w h, u32 buffers,
                    // then per buffer u32 stage, u32 index, u32 length, the bytes (at most 32 KB), then per stage (vertex, fragment) u32 count and per binding u32 binding, kind, Metal index, then the vertex attributes and layouts; and the two SPIR-V modules of the pipeline as <n>_d<k>_v.spv and _f.spv
                    char bpath[128]; snprintf(bpath, sizeof(bpath), "%s/%03u_d%lu.bin", g_pd_dir, g_pd_seq, (unsigned long)k);
                    FILE *bf = fopen(bpath, "w");
                    if (bf) {
                        uint32_t nb = 0, hdr[12] = {0x31444e52u, (uint32_t)d->vertexCount, (uint32_t)d->instanceCount, (uint32_t)(d->pso->blend[0].enabled), (uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1, (uint32_t)t->w, (uint32_t)t->h, 0, 0};
                        for (int st = 0; st < 2; st++) for (int i = 0; i < RN_MTL_SLOTS; i++) if (d->bufs[st][i] || d->bytesAt[st][i]) nb++;
                        hdr[10] = nb; fwrite(hdr, 4, 12, bf);
                        for (int st = 0; st < 2; st++) for (int i = 0; i < RN_MTL_SLOTS; i++) {
                            const uint8_t *bp = NULL; NSUInteger len = 0;
                            if (d->bufs[st][i]) { bp = rn_vk_buffer_contents(d->bufs[st][i]->b) + d->offs[st][i]; len = d->bufs[st][i]->len - d->offs[st][i]; }
                            else if (d->bytesAt[st][i]) { bp = d->bytesAt[st][i].bytes; len = d->bytesAt[st][i].length; }
                            if (!bp) continue;
                            if (len > 32768) len = 32768;
                            uint32_t h3[3] = {(uint32_t)st, (uint32_t)i, (uint32_t)len}; fwrite(h3, 4, 3, bf); fwrite(bp, 1, len, bf);
                        }
                        for (int s2 = 0; s2 < 2; s2++) {    // the bindings of each stage: u32 count, then per binding u32 descriptor binding, kind (RN_VK_*), Metal index
                            uint32_t cnt = d->pso->n[s2]; fwrite(&cnt, 4, 1, bf);
                            for (unsigned q = 0; q < cnt; q++) { uint32_t h3[3] = {d->pso->binds[s2][q].binding, d->pso->binds[s2][q].kind, d->pso->mi[s2][q]}; fwrite(h3, 4, 3, bf); }
                        }
                        { uint32_t cnt = d->pso->nvattr; fwrite(&cnt, 4, 1, bf);       // the vertex input: u32 count, per attribute u32 location, binding, format (VkFormat), offset; then u32 count, per layout u32 binding, stride, per instance
                          for (unsigned q = 0; q < cnt; q++) { uint32_t h4[4] = {d->pso->vattr[q].location, d->pso->vattr[q].binding, d->pso->vattr[q].format, d->pso->vattr[q].offset}; fwrite(h4, 4, 4, bf); }
                          cnt = d->pso->nvlayout; fwrite(&cnt, 4, 1, bf);
                          for (unsigned q = 0; q < cnt; q++) { uint32_t h3[3] = {d->pso->vlayout[q].binding, d->pso->vlayout[q].stride, d->pso->vlayout[q].per_instance}; fwrite(h3, 4, 3, bf); } }
                        { double vp[6] = {d->hasViewport ? d->viewport[0] : 0, d->hasViewport ? d->viewport[1] : 0, d->hasViewport ? d->viewport[2] : (double)t->w, d->hasViewport ? d->viewport[3] : (double)t->h, d->hasViewport ? d->viewport[4] : 0, d->hasViewport ? d->viewport[5] : 1};
                          uint32_t sc[4] = {d->hasScissor ? (uint32_t)d->scissor[0] : 0, d->hasScissor ? (uint32_t)d->scissor[1] : 0, d->hasScissor ? (uint32_t)d->scissor[2] : (uint32_t)t->w, d->hasScissor ? (uint32_t)d->scissor[3] : (uint32_t)t->h};
                          const rn_vk_blend *bl = &d->pso->blend[0];
                          uint32_t st[12] = {(uint32_t)bl->enabled, bl->src_rgb, bl->dst_rgb, bl->op_rgb, bl->src_a, bl->dst_a, bl->op_a, bl->write_mask, (uint32_t)d->prim, (uint32_t)d->cull, (uint32_t)d->clockwise, 0};
                          fwrite(vp, 8, 6, bf); fwrite(sc, 4, 4, bf); fwrite(st, 4, 12, bf); }       // the state of the draw: viewport (6 doubles), scissor (4 u32), then blend enabled, src rgb, dst rgb, op rgb, src a, dst a, op a, write mask, primitive, cull, clockwise, 0
                        {   // the indexing: u32 indexed (0 or 1), u32 32-bit indices, i32 base vertex, u32 first vertex, u32 index bytes (at most 64 KB), the index bytes (vertices is then the index count)
                            uint32_t ix[5] = {d->indexBuf ? 1u : 0u, d->index32 ? 1u : 0u, (uint32_t)d->baseVertex, (uint32_t)d->vertexStart, 0};
                            NSUInteger ilen = d->indexBuf ? (NSUInteger)d->vertexCount * (d->index32 ? 4 : 2) : 0;
                            if (d->indexBuf && d->indexOffset + ilen > d->indexBuf->len) ilen = d->indexBuf->len > d->indexOffset ? d->indexBuf->len - d->indexOffset : 0;
                            if (ilen > 65536) ilen = 65536;
                            ix[4] = (uint32_t)ilen; fwrite(ix, 4, 5, bf);
                            if (ilen) fwrite(rn_vk_buffer_contents(d->indexBuf->b) + d->indexOffset, 1, ilen, bf);
                        }
                        fclose(bf); chmod(bpath, 0644);
                    }
                    for (int s2 = 0; s2 < 2; s2++) if (d->pso->spv[s2]) {
                        char sp[128]; snprintf(sp, sizeof(sp), "%s/%03u_d%lu_%c.spv", g_pd_dir, g_pd_seq, (unsigned long)k, s2 ? 'f' : 'v');
                        [d->pso->spv[s2] writeToFile:[NSString stringWithUTF8String:sp] atomically:NO]; chmod(sp, 0644);
                    }
                    // the textures the draw binds, as raw bytes (at most 1 MB each): <n>_d<k>_t<stage><index>_<w>x<h>_pf<format>.raw is the host copy the card is given (w * h * texel bytes) and, for a texture over an
                    // IOSurface, ..._surf.raw is the surface's own bytes (rows are the surface's pitch apart: the pitch is in the name). Compare the two to see whether the upload changed anything.
                    for (int st = 0; st < 2; st++) for (int i = 0; i < RN_MTL_SLOTS; i++) {
                        RedNaviMTLTexture *tx = d->texs[st][i];
                        if (!tx || !tx->img || tx->d > 1 || !shadowDraw) continue;   // the shadow draws only
                        const size_t nbt = (size_t)tx->w * tx->h * tx->texel;
                        if (!nbt || nbt > (1u << 20)) continue;
                        const uint8_t *hp = rn_vk_image_staging(tx->img);
                        char tp[192]; snprintf(tp, sizeof(tp), "%s/%03u_d%lu_t%c%d_%lux%lu_pf%lu.raw", g_pd_dir, g_pd_seq, (unsigned long)k, st ? 'f' : 'v', i, (unsigned long)tx->w, (unsigned long)tx->h, (unsigned long)tx->pf);
                        if (hp) { FILE *tf = fopen(tp, "w"); if (tf) { fwrite(hp, 1, nbt, tf); fclose(tf); chmod(tp, 0644); } }
                        if (tx->surf && IOSurfaceGetPlaneCount(tx->surf) == 0 && IOSurfaceLock(tx->surf, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess) {
                            const size_t pitch = IOSurfaceGetBytesPerRow(tx->surf), sh = IOSurfaceGetHeight(tx->surf);
                            snprintf(tp, sizeof(tp), "%s/%03u_d%lu_t%c%d_%lux%lu_pf%lu_pitch%lu_surf.raw", g_pd_dir, g_pd_seq, (unsigned long)k, st ? 'f' : 'v', i, (unsigned long)tx->w, (unsigned long)tx->h, (unsigned long)tx->pf, (unsigned long)pitch);
                            if (pitch * sh <= (1u << 20)) { FILE *tf = fopen(tp, "w"); if (tf) { fwrite(IOSurfaceGetBaseAddress(tx->surf), 1, pitch * sh, tf); fclose(tf); chmod(tp, 0644); } }
                            IOSurfaceUnlock(tx->surf, kIOSurfaceLockReadOnly, NULL);
                        }
                    }
                }
                if (k < 8 || d->vertexCount > 6) {      // what is bound at each buffer index of the first draws: setBytes data or a client buffer, the first 64 bytes as floats and the size
                    // the first six draws are dumped in full (the six vertices of a quad with their stride), the others as 16 floats
                    for (int st = 0; st < 2; st++) for (int i = 0; i < RN_MTL_SLOTS; i++) {
                        const uint8_t *bp = NULL; NSUInteger len = 0;
                        if (d->bufs[st][i]) { bp = rn_vk_buffer_contents(d->bufs[st][i]->b) + d->offs[st][i]; len = d->bufs[st][i]->len - d->offs[st][i]; }
                        else if (d->bytesAt[st][i]) { bp = d->bytesAt[st][i].bytes; len = d->bytesAt[st][i].length; }
                        if (!bp) continue;
                        fprintf(df, "      %c%d %s %lu bytes:", st ? 'f' : 'v', i, d->bufs[st][i] ? "buffer" : "bytes", (unsigned long)len);
                        for (NSUInteger j = 0; j < (k < 6 ? 96 : 16) && (j + 1) * 4 <= len; j++) { float fv; memcpy(&fv, bp + 4 * j, 4); fprintf(df, " %g", fv); }
                        fputc('\n', df);
                    }
                }
            }
            fclose(df); chmod(dpath, 0644);
        }
    }
    {   // textures the pass sampled, read back from the card and compared with the host copy they came from: <dir>/verify.txt (mismatches, and one summary line per pass)
        RedNaviMTLTexture *seen[96]; unsigned ns = 0, checked = 0, bad = 0, skipped = 0;
        char vpath[112]; snprintf(vpath, sizeof(vpath), "%s/verify.txt", g_pd_dir);
        FILE *vf = fopen(vpath, "a");
        for (NSUInteger k = 0; k < nd && vf; k++) {
            RNDraw *d = rp->draws[k];
            for (int st = 0; st < 2; st++) for (int i = 0; i < RN_MTL_SLOTS; i++) {
                RedNaviMTLTexture *tx = d->texs[st][i];
                if (!tx || !tx->img || tx == t) continue;
                BOOL dup = NO; for (unsigned q = 0; q < ns; q++) if (seen[q] == tx) dup = YES;
                if (dup || ns >= 96) continue;
                seen[ns++] = tx;
                size_t nd_ = 0, first = 0;
                uint8_t *hc = NULL, *cc = NULL;
                if (rn_vk_image_verify(rn_vk_shared(), tx->img, &nd_, &first, &hc, &cc)) { skipped++; continue; }
                checked++;
                if (nd_) { bad++; fprintf(vf, "pass %03u draw %lu %c%d %lux%lu pf%lu surf %u: %lu bytes differ, first at byte %lu\n", g_pd_seq, (unsigned long)k, st ? 'f' : 'v', i, (unsigned long)tx->w, (unsigned long)tx->h, (unsigned long)tx->pf, tx->surf ? (unsigned)IOSurfaceGetID(tx->surf) : 0u, (unsigned long)nd_, (unsigned long)first);
                    if (hc && cc) {   // where: per row the count of bytes that differ and the first and last column, then the first 32 differing bytes (host copy, card), and how the texture was made
                        fprintf(vf, "    made over %s; %u bytes per texel\n", tx->srcBuf ? "a buffer" : (tx->srcRaw ? "client memory" : "replaceRegion/blit"), tx->texel);
                        for (NSUInteger y = 0; y < tx->h; y++) { unsigned cnt = 0; long c0 = -1, c1 = -1; for (NSUInteger x = 0; x < tx->w * tx->texel; x++) if (hc[y * tx->w * tx->texel + x] != cc[y * tx->w * tx->texel + x]) { cnt++; if (c0 < 0) c0 = (long)x; c1 = (long)x; } if (cnt) fprintf(vf, "    row %lu: %u differ, columns %ld..%ld\n", (unsigned long)y, cnt, c0, c1); }
                        if (tx->srcBuf || tx->srcRaw) {   // who is behind: the caller's memory now, against the host copy and against the card's copy
                            const uint8_t *csrc = tx->srcBuf ? (const uint8_t *)[(id<MTLBuffer>)tx->srcBuf contents] + tx->srcOff : tx->srcRaw;
                            unsigned long okRows = 0, eqHost = 0, eqCard = 0; uint8_t row[4096];
                            for (NSUInteger y = 0; y < tx->h && tx->w * tx->texel <= sizeof(row); y++) {
                                if (!rn_copy_from_caller(row, csrc + y * tx->srcBpr, tx->w * tx->texel)) continue;
                                okRows++;
                                if (!memcmp(row, hc + y * tx->w * tx->texel, tx->w * tx->texel)) eqHost++;
                                if (!memcmp(row, cc + y * tx->w * tx->texel, tx->w * tx->texel)) eqCard++;
                            }
                            fprintf(vf, "    caller's memory now: %lu rows read, %lu equal the host copy, %lu equal the card's copy (of %lu rows)\n", okRows, eqHost, eqCard, (unsigned long)tx->h);
                        }
                        {   // the host copy when the upload was recorded and when it was submitted, against the card's copy and the host copy now
                            const uint8_t *rec, *sb, *af; const size_t nb = (size_t)tx->w * tx->h * tx->texel;
                            if (!rn_vk_image_hist(tx->img, &rec, &sb, &af) && af) fprintf(vf, "    card copy right after the upload: %s the host copy now, %s the card's copy now\n", !memcmp(af, hc, nb) ? "equals" : "differs from", !memcmp(af, cc, nb) ? "equals" : "differs from");
                            if (!rn_vk_image_hist(tx->img, &rec, &sb, &af))
                                fprintf(vf, "    at the last upload: recorded copy %s the host copy now and %s the card's copy; copy at submit %s the host copy now and %s the card's copy\n", !memcmp(rec, hc, nb) ? "equals" : "differs from", !memcmp(rec, cc, nb) ? "equals" : "differs from", !memcmp(sb, hc, nb) ? "equals" : "differs from", !memcmp(sb, cc, nb) ? "equals" : "differs from");
                            else fprintf(vf, "    no upload recorded during the capture for this image\n");
                        }
                        {   // does the card's copy equal the host copy of another texture of this pass (aliasing), or share its VkImage?
                            const size_t nb = (size_t)tx->w * tx->h * tx->texel;
                            for (unsigned q = 0; q < ns; q++) {
                                RedNaviMTLTexture *o = seen[q];
                                if (o == tx) continue;
                                if (o->img == tx->img) fprintf(vf, "    shares the image with another texture of this pass: %lux%lu pf%lu\n", (unsigned long)o->w, (unsigned long)o->h, (unsigned long)o->pf);
                                if ((size_t)o->w * o->h * o->texel == nb && rn_vk_image_staging(o->img) && !memcmp(rn_vk_image_staging(o->img), cc, nb)) fprintf(vf, "    card copy equals the host copy of another texture: %lux%lu pf%lu\n", (unsigned long)o->w, (unsigned long)o->h, (unsigned long)o->pf);
                            }
                        }
                        rn_tex_pair_png(g_pd_seq, (unsigned)k, (uint32_t)tx->w, (uint32_t)tx->h, tx->texel, hc, cc);
                        unsigned shown = 0; for (size_t q = 0; q < (size_t)tx->w * tx->h * tx->texel && shown < 32; q++) if (hc[q] != cc[q]) { fprintf(vf, "    byte %lu: host %02x card %02x\n", (unsigned long)q, hc[q], cc[q]); shown++; }
                    }
                }
                free(hc); free(cc);
            }
        }
        if (vf) { fprintf(vf, "pass %03u: %u textures, %u compared, %u differ, %u not compared\n", g_pd_seq, ns, checked, bad, skipped); fclose(vf); chmod(vpath, 0644); }
    }
    g_pd_seq++; g_pd_left--;
}
// The bytes of an IOSurface as the plug-in reads them in syncFromSurface (BGRA8 only), as <dir>/src_<surface id>_<n>.png, at most 60 per capture and only while a capture is running (g_pd_left).
// Pass <n> of the same surface is then compared with the last src_ file written before it: a glitch already in the source is not made by the card.
static void rn_srcdump(IOSurfaceRef surf, const uint8_t *src, size_t pitch, size_t w, size_t h) {
    static unsigned done, doneBig; static int dirpid;
    if (g_pd_left <= 0 || !g_pd_dir[0]) return;
    if (dirpid != (int)getpid()) { dirpid = (int)getpid(); done = 0; doneBig = 0; }
    const OSType fmt = IOSurfaceGetPixelFormat(surf);
    if (fmt != 'BGRA') return;
    uint8_t *px = malloc(w * h * 4);
    if (!px) return;
    for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) { const uint8_t *s = src + y * pitch + x * 4; uint8_t *d = px + (y * w + x) * 4; d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; }
    // The src_ PNG is written for at most 60 sources (and not for the big full-screen ones); the bytes are always kept for rn_cmpdump, so a window surface that is read late in a capture is still compared.
    // Window-sized sources (over 0.6 M pixels: an app window) have a budget of their own, 24 per process, so the ring surfaces of full-screen size cannot use up the 60.
    const BOOL big = w * h > 600000;
    if ((big ? doneBig < 24 : done < 60) && w * h <= 9000000) {
        char path[112]; snprintf(path, sizeof(path), "%s/src_%u_%02u_p%03u.png", g_pd_dir, (unsigned)IOSurfaceGetID(surf), big ? 100 + doneBig++ : done++, g_pd_seq);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, px, w * h * 4, NULL);
        CGImageRef im = CGImageCreate(w, h, 8, 32, w * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp, NULL, false, kCGRenderingIntentDefault);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
        CGImageDestinationRef dst = im ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
        if (dst) { CGImageDestinationAddImage(dst, im, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); chmod(path, 0644); }
        CFRelease(url); if (im) CGImageRelease(im); CGDataProviderRelease(dp); CGColorSpaceRelease(cs);
    }
    // keep the surface's bytes (RGBA, rows packed) for rn_cmpdump: the cache holds the last source of up to 8 surfaces
    {
        int slot = -1;
        pthread_mutex_lock(&g_src_lock);
        for (int i = 0; i < 8; i++) if (g_src_cache[i].id == (unsigned)IOSurfaceGetID(surf)) { slot = i; break; }
        if (slot < 0) { slot = (int)(g_src_next++ % 8); }
        free(g_src_cache[slot].px);
        g_src_cache[slot].px = px; g_src_cache[slot].id = (unsigned)IOSurfaceGetID(surf); g_src_cache[slot].w = w; g_src_cache[slot].h = h;
        g_src_cache[slot].pitch = pitch; g_src_cache[slot].compared = NO;
        pthread_mutex_unlock(&g_src_lock);
    }
}

// Side by side: the first render pass into a surface that rn_srcdump saw is compared with the bytes of that surface as the CPU had them just before (IOSurface locked read-only, rows read at
// IOSurfaceGetBytesPerRow, the padding of a row never looked at). Written as <dir>/cmp_<surface id>_p<pass>.png: source | card after the pass | red where they differ, and a line in
// <dir>/cmp.txt: texels that differ, the first and last row with a difference, how many rows differ over more than half of their width, and the longest run of such rows. A pass that loads the
// surface and draws two rectangles (what a window's first pass is) should leave most texels equal: horizontal dark bands then show as full-width rows of differences, either already
// in the source (the CPU side of the upload) or only after the pass (the card).
static void rn_cmpdump_body(IOSurfaceRef surf, size_t w, size_t h, const uint8_t *gpu, unsigned seq);
static void rn_cmpdump(IOSurfaceRef surf, size_t w, size_t h, const uint8_t *gpu, unsigned seq) {
    pthread_mutex_lock(&g_src_lock); rn_cmpdump_body(surf, w, h, gpu, seq); pthread_mutex_unlock(&g_src_lock);
}
static void rn_cmpdump_body(IOSurfaceRef surf, size_t w, size_t h, const uint8_t *gpu, unsigned seq) {
    const unsigned id = (unsigned)IOSurfaceGetID(surf);
    int slot = -1;
    for (int i = 0; i < 8; i++) if (g_src_cache[i].id == id && g_src_cache[i].px) { slot = i; break; }
    if (slot < 0 || g_src_cache[slot].compared || g_src_cache[slot].w != w || g_src_cache[slot].h != h) return;
    g_src_cache[slot].compared = YES;
    if (w * h > 1500000) return; // a full-screen pass rewrites the whole surface: the comparison says nothing there
    const uint8_t *sp = g_src_cache[slot].px;
    const BOOL three = w * 3 <= 16384;
    const size_t ow = w * (three ? 3 : 2);
    uint8_t *out = malloc(ow * h * 4);
    if (!out) return;
    unsigned long diff = 0, bandRows = 0, runLen = 0, maxRun = 0; long firstRow = -1, lastRow = -1;
    for (size_t y = 0; y < h; y++) {
        unsigned long rowDiff = 0;
        for (size_t x = 0; x < w; x++) {
            const uint8_t *a = sp + (y * w + x) * 4, *b = gpu + (y * w + x) * 4;
            uint8_t *o = out + (y * ow + x) * 4, *o2 = out + (y * ow + w + x) * 4;
            memcpy(o, a, 4); memcpy(o2, b, 4);
            const BOOL ne = a[0] != b[0] || a[1] != b[1] || a[2] != b[2];
            if (ne) rowDiff++;
            if (three) { uint8_t *o3 = out + (y * ow + 2 * w + x) * 4; o3[0] = ne ? 255 : 0; o3[1] = 0; o3[2] = 0; o3[3] = 255; }
        }
        diff += rowDiff;
        if (rowDiff) { if (firstRow < 0) firstRow = (long)y; lastRow = (long)y; }
        if (rowDiff * 2 > w) { bandRows++; runLen++; if (runLen > maxRun) maxRun = runLen; } else runLen = 0;
    }
    char path[128]; snprintf(path, sizeof(path), "%s/cmp_%u_p%03u.png", g_pd_dir, id, seq);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, out, ow * h * 4, NULL);
    CGImageRef im = CGImageCreate(ow, h, 8, 32, ow * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp, NULL, false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dst = im ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    if (dst) { CGImageDestinationAddImage(dst, im, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); chmod(path, 0644); }
    CFRelease(url); if (im) CGImageRelease(im); CGDataProviderRelease(dp); CGColorSpaceRelease(cs);
    free(out);
    char cpath[112]; snprintf(cpath, sizeof(cpath), "%s/cmp.txt", g_pd_dir);
    FILE *f = fopen(cpath, "a");
    if (f) {
        fprintf(f, "surf %u pass %03u %lux%lu (source pitch %lu bytes, packed %lu): %lu texels differ, rows %ld..%ld, %lu rows differ over more than half their width (longest run %lu)\n",
                id, seq, (unsigned long)w, (unsigned long)h, (unsigned long)g_src_cache[slot].pitch, (unsigned long)(w * 4), diff, firstRow, lastRow, bandRows, maxRun);
        fclose(f); chmod(cpath, 0644);
    }
}

// Trace aid (only with /Users/Shared/rednavi-trace): the picture a big render pass left in its IOSurface, as /private/var/tmp/rednavi-trace-<n>.png, at most one every three seconds.
static void rn_trace_dump(RedNaviMTLTexture *t, unsigned draws) {
    static uint64_t last; static unsigned seq;
    const uint64_t now = mach_absolute_time();
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    if (last && (now - last) * tb.numer / tb.denom < 3000000000ull) return;
    if (!t->surf || draws < 20 || t->pf != MTLPixelFormatBGRA8Unorm) return;
    last = now;
    if (IOSurfaceLock(t->surf, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) return;
    const size_t pitch = IOSurfaceGetBytesPerRow(t->surf);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef cg = CGBitmapContextCreate(IOSurfaceGetBaseAddress(t->surf), t->w, t->h, 8, pitch, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = cg ? CGBitmapContextCreateImage(cg) : NULL;
    if (img) {
        char path[128]; snprintf(path, sizeof(path), "/private/var/tmp/rednavi-trace-%u.png", seq++ % 8);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
        if (dst) { CGImageDestinationAddImage(dst, img, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); chmod(path, 0644); }
        CFRelease(url); CGImageRelease(img);
        rn_log("trace: wrote %s (%lux%lu, %u draws)", path, (unsigned long)t->w, (unsigned long)t->h, draws);
    }
    if (cg) CGContextRelease(cg);
    CGColorSpaceRelease(cs);
    IOSurfaceUnlock(t->surf, kIOSurfaceLockReadOnly, NULL);
}

// Direct scanout (doc: the PCIe link carries about 6.4 GB/s each way, measured 2026-11-08, and a compositor pass with its surfaces in system memory took 3 to 5 ms): the render
// passes into CoreDisplay's scanout surfaces are redirected to an image over the display's own VRAM surface (display surface 0, mapped by the bridge), so the GPU writes locally,
// and the accelerator's display pipe, told by RN_PRESENT_FRAME_DONE, does not copy the IOSurface into the scanout. File /Users/Shared/rednavi-nodirectscan turns it off.
static const uint8_t *g_scan_cpu; static uint32_t g_scan_pitch;
static rn_vk_image *rn_scanout_image(rn_vk *vk, size_t w, size_t h) {
    static rn_vk_image *img; static dispatch_once_t once; static size_t sw, sh;
    dispatch_once(&once, ^{
        if (access("/Users/Shared/rednavi-nodirectscan", F_OK) == 0) { rn_log("direct scanout: turned off by /Users/Shared/rednavi-nodirectscan"); return; }
        int (*scan)(uint32_t, uint32_t *, uint32_t *, uint32_t *, void **, size_t *) = rn_vk_symbol(vk, "rn_bridge_scanout");
        if (!scan) { rn_log("direct scanout: rn_bridge_scanout not found in the Vulkan driver"); return; }
        uint32_t bw = 0, bh = 0, bp = 0; void *cpu = NULL; size_t len = 0;
        int r = scan(0, &bw, &bh, &bp, &cpu, &len);
        const int fmt = rn_vk_format_of(MTLPixelFormatBGRA8Unorm);
        if (r || !cpu || !fmt || bp != bw * 4) { rn_log("direct scanout: scanout surface 0 not available (%d, pitch %u, width %u)", r, bp, bw); return; }
        g_scan_cpu = cpu; g_scan_pitch = bp;
        img = rn_vk_image_from_host(vk, fmt, bw, bh, cpu, bp, len);
        if (img) { sw = bw; sh = bh; rn_log("direct scanout: rendering CoreDisplay's passes into the %ux%u scanout surface", bw, bh); }
        else rn_log("direct scanout: the scanout surface cannot be used as an image");
    });
    return img && w == sw && h == sh ? img : NULL;
}
// The card's own copy of a CoreDisplay scanout surface (device memory), one per IOSurface and not per texture: CoreDisplay makes a new MTLTexture over the same ring surface
// for each frame and draws only the changed part into it, so the contents have to outlive the texture object.
// Glitch locator (with /Users/Shared/rednavi-scandump): the last passes into display surfaces, one record per draw (shaders, the rectangle it covers, what it samples), kept in a ring.
// When the dump of a finished frame has a block of pure green (what a texture of zeros looks like through a YCbCr conversion, or what uninitialised memory often is), the draws of the
// ring that cover that point are written to the log: either the draw that produced it is named, or nothing recent drew there (the pixels were never redrawn).
typedef struct { uint64_t seq; char vs[48], fs[48]; int x0, y0, x1, y1; unsigned ntex; unsigned tw[3], th[3], tpf[3], tsurf[3]; unsigned instances, vertices; int blend; } RNDrawRec;
static RNDrawRec g_drawring[512]; static unsigned g_drawhead; static uint64_t g_passseq;
static BOOL rn_locator_on(void) { static int on = -1; if (on < 0) on = access("/Users/Shared/rednavi-scandump", F_OK) == 0; return on; }
static void rn_locator_note(RNRenderPass *rp, RedNaviMTLTexture *rt) {
    if (!rn_locator_on() || !rt) return;
    const uint64_t seq = ++g_passseq;
    for (RNDraw *d in rp->draws) {
        RNDrawRec *r = &g_drawring[g_drawhead++ % 512];
        memset(r, 0, sizeof(*r));
        r->seq = seq;
        strlcpy(r->vs, d->pso && d->pso->names[0] ? d->pso->names[0].UTF8String : "-", sizeof(r->vs));
        strlcpy(r->fs, d->pso && d->pso->names[1] ? d->pso->names[1].UTF8String : "-", sizeof(r->fs));
        r->x0 = 0; r->y0 = 0; r->x1 = (int)rt->w; r->y1 = (int)rt->h;
        if (d->hasViewport) { r->x0 = (int)d->viewport[0]; r->y0 = (int)d->viewport[1]; r->x1 = (int)(d->viewport[0] + d->viewport[2]); r->y1 = (int)(d->viewport[1] + d->viewport[3]); }
        if (d->hasScissor) { int sx0 = (int)d->scissor[0], sy0 = (int)d->scissor[1], sx1 = sx0 + (int)d->scissor[2], sy1 = sy0 + (int)d->scissor[3]; if (sx0 > r->x0) r->x0 = sx0; if (sy0 > r->y0) r->y0 = sy0; if (sx1 < r->x1) r->x1 = sx1; if (sy1 < r->y1) r->y1 = sy1; }
        for (int i = 0; i < RN_MTL_SLOTS && r->ntex < 3; i++) { RedNaviMTLTexture *t = d->texs[1][i]; if (!t) continue; r->tw[r->ntex] = (unsigned)t->w; r->th[r->ntex] = (unsigned)t->h; r->tpf[r->ntex] = (unsigned)t->pf; r->tsurf[r->ntex] = t->surf ? (unsigned)IOSurfaceGetID(t->surf) : 0; r->ntex++; }
        r->instances = d->instanceCount; r->vertices = d->vertexCount; r->blend = d->pso ? d->pso->blend[0].enabled : 0;
    }
}
static void rn_locator_find(const uint8_t *px, size_t w, size_t h) {
    int found = 0;
    for (size_t y = 8; y + 8 < h && found < 3; y += 8)
        for (size_t x = 8; x + 8 < w && found < 3; x += 8) {
            int ok = 1;
            for (int dy = 0; dy < 8 && ok; dy += 7) for (int dx = 0; dx < 8 && ok; dx += 7) { const uint8_t *q = px + ((y + dy) * w + x + dx) * 4; ok = q[1] > 200 && q[2] < 90 && q[0] < 90; }
            if (!ok) continue;
            found++;
            rn_log("locator: green block at (%zu,%zu); draws covering it, newest first:", x, y);
            unsigned shown = 0;
            for (unsigned k = 0; k < 512 && shown < 6; k++) {
                RNDrawRec *r = &g_drawring[(g_drawhead - 1 - k) % 512];
                if (!r->seq) break;
                if ((int)x < r->x0 || (int)x >= r->x1 || (int)y < r->y0 || (int)y >= r->y1) continue;
                shown++;
                rn_log("locator:   pass %llu: %s + %s, rect (%d,%d)-(%d,%d), %u vertices x %u instances, blend %d, samples %u: %ux%u/%u surf %u, %ux%u/%u surf %u, %ux%u/%u surf %u", (unsigned long long)r->seq, r->vs, r->fs, r->x0, r->y0, r->x1, r->y1, r->vertices, r->instances, r->blend, r->ntex,
                       r->tw[0], r->th[0], r->tpf[0], r->tsurf[0], r->tw[1], r->th[1], r->tpf[1], r->tsurf[1], r->tw[2], r->th[2], r->tpf[2], r->tsurf[2]);
            }
            if (!shown) rn_log("locator:   no draw of the last %u covers it (never redrawn)", MIN(g_drawhead, 512u));
        }
}
// Trace aid (only with /Users/Shared/rednavi-scandump): the finished frame as the card rendered it (the card copy of the surface, read back with a GPU transfer into host memory;
// reading the display's own VRAM surface through the BAR takes seconds) as /private/var/tmp/rednavi-scan-<n>.png, one every two seconds, eight files in rotation.
static void rn_scan_dump(rn_vk_image *src, size_t w, size_t h) {
    static uint64_t last; static unsigned seq; static int on = -1;
    if (on < 0) on = access("/Users/Shared/rednavi-scandump", F_OK) == 0;
    if (!on || !src) return;
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    const uint64_t now = mach_absolute_time();
    if (last && (now - last) * tb.numer / tb.denom < 2000000000ull) return;
    last = now;
    const size_t pitch = w * 4;
    uint8_t *copy = malloc(pitch * h);
    if (!copy) return;
    memcpy(copy, rn_vk_image_staging(src), pitch * h);
    rn_locator_find(copy, w, h);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef cg = CGBitmapContextCreate(copy, w, h, 8, pitch, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = cg ? CGBitmapContextCreateImage(cg) : NULL;
    if (img) {
        char path[128]; snprintf(path, sizeof(path), "/private/var/tmp/rednavi-scan-%u.png", seq++ % 8);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
        if (dst) { CGImageDestinationAddImage(dst, img, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); chmod(path, 0644); }
        CFRelease(url); CGImageRelease(img);
    }
    if (cg) CGContextRelease(cg);
    CGColorSpaceRelease(cs);
    free(copy);
}
// A texture view of a scanout surface with another pixel format of the same texel size (an sRGB view of a BGRA8 surface is what colour managed drawing samples and renders through) is a view of the
// card's copy, not of the texture's own image, whose pixels are old. One view per (card copy, format), dropped with the copy.
typedef struct { rn_vk_image *base, *view; MTLPixelFormat pf; } RNVramView;
static RNVramView g_vviews[64]; static unsigned g_vviewn;
static void rn_vram_views_drop(rn_vk *vk, rn_vk_image *base) {
    for (unsigned i = 0; i < g_vviewn;) if (g_vviews[i].base == base) { rn_vk_image_destroy(vk, g_vviews[i].view); g_vviews[i] = g_vviews[--g_vviewn]; } else i++;
}
static rn_vk_image *rn_vram_as(rn_vk *vk, rn_vk_image *base, MTLPixelFormat pf) {
    for (unsigned i = 0; i < g_vviewn; i++) if (g_vviews[i].base == base && g_vviews[i].pf == pf) return g_vviews[i].view;
    const int f = rn_vk_format_of(pf);
    if (!f || g_vviewn >= 64) return NULL;
    rn_vk_image *v = rn_vk_image_view(vk, base, f, NULL);
    if (!v) return NULL;
    g_vviews[g_vviewn++] = (RNVramView){base, v, pf};
    return v;
}
static rn_vk_image *rn_display_vram_get(rn_vk *vk, RedNaviMTLTexture *rt, BOOL create) {
    static NSMutableDictionary<NSNumber *, NSValue *> *cache; static NSMutableArray<NSNumber *> *order;
    if (!rt->surf) return NULL;
    @synchronized ([RedNaviMTLTexture class]) {
        if (!cache) { cache = [NSMutableDictionary new]; order = [NSMutableArray new]; }
        NSNumber *key = @(((unsigned long long)IOSurfaceGetID(rt->surf) << 32) | ((rt->w & 0xFFFF) << 16) | (rt->h & 0xFFFF));
        NSValue *v = cache[key];
        if (v) {        // least recently used goes first: the ring surfaces in use stay at the end, and the copy dropped for a new surface is one nothing has touched for longest
            if (order.lastObject != key && ![order.lastObject isEqual:key]) { [order removeObject:key]; [order addObject:key]; }
            return (rn_vk_image *)v.pointerValue;
        }
        if (!create) return NULL;
        rn_vk_image *img = rn_vk_image_create(vk, rn_vk_format_of(rt->pf), (uint32_t)rt->w, (uint32_t)rt->h);
        if (!img) return NULL;
        if (order.count >= 16) { rn_vram_views_drop(vk, (rn_vk_image *)cache[order[0]].pointerValue); rn_vk_image_destroy(vk, (rn_vk_image *)cache[order[0]].pointerValue); [cache removeObjectForKey:order[0]]; [order removeObjectAtIndex:0]; }
        cache[key] = [NSValue valueWithPointer:img]; [order addObject:key];
        // A new ring surface starts with whatever the card's memory held, and CoreDisplay draws only what changed into it: the picture on the screen is its best starting point.
        rn_vk_image *shown = rn_scanout_image(vk, rt->w, rt->h);
        if (shown && rn_vk_image_copy(vk, shown, img) != 0) rn_log("direct scanout: the card copy for IOSurface %u could not be filled from the scanout", (unsigned)IOSurfaceGetID(rt->surf));
        {   // Experiment switch /Users/Shared/rednavi-magenta: a new ring surface starts as bright magenta instead of the picture on the screen, so that tiles CoreDisplay never draws show up.
            static int mag = -1;
            if (mag < 0) mag = access("/Users/Shared/rednavi-magenta", F_OK) == 0;
            if (mag) { rn_vk_target t = {.image = img, .clear = 1, .color = {1.f, 0.f, 1.f, 1.f}}; rn_vk_render_pass(vk, &t, 1, NULL, 0); }
        }
        rn_log("direct scanout: card copy for IOSurface %u (%lux%lu)", (unsigned)IOSurfaceGetID(rt->surf), (unsigned long)rt->w, (unsigned long)rt->h);
        return img;
    }
}
static rn_vk_image *rn_display_vram(rn_vk *vk, RedNaviMTLTexture *rt) { return rn_display_vram_get(vk, rt, YES); }
// A scanout surface that is sampled (CoreDisplay reads one ring surface while it draws into another) is read from the card's copy, which is where its pixels are now.
static rn_vk_image *rn_display_vram_peek(void *tp) {
    RedNaviMTLTexture *t = (__bridge RedNaviMTLTexture *)tp, *rt = t->parent ? t->parent : t;
    if (!rt->displayScan) return NULL;
    rn_vk *vk = rn_vk_shared();
    // With direct scanout a scanout surface's pixels are only ever on the card: its own image is never drawn into. A surface whose copy is not there (never drawn yet, or dropped
    // from the cache) is given one, filled from the picture on the screen; the texture's own image would be read as old pixels in exactly the rectangles a frame carries over.
    rn_vk_image *vr = rn_display_vram_get(vk, rt, rn_scanout_image(vk, rt->w, rt->h) != NULL);
    if (!vr || t->pf == rt->pf) return vr;
    if (t->texel != rt->texel || t->swz.red != MTLTextureSwizzleRed || t->swz.green != MTLTextureSwizzleGreen || t->swz.blue != MTLTextureSwizzleBlue || t->swz.alpha != MTLTextureSwizzleAlpha) return NULL;
    @synchronized ([RedNaviMTLTexture class]) { return rn_vram_as(vk, vr, t->pf); }
}
// A host write into a scanout surface (replaceRegion, a blit from a buffer, a blit between different pixel formats) lands in the texture's own image, which direct scanout never shows:
// the card copy keeps its old pixels there. Not handled; the log says whether a client ever does it, and how.
static void rn_display_host_write_note(void *tp, const char *how) {
    RedNaviMTLTexture *t = (__bridge RedNaviMTLTexture *)tp, *rt = t->parent ? t->parent : t;
    if (!rt->displayScan || !rn_scanout_image(rn_vk_shared(), rt->w, rt->h)) return;
    static atomic_uint said; const unsigned n = atomic_fetch_add(&said, 1);
    if (n < 20 || n % 1000 == 0) rn_log("direct scanout: %s writes the host image of scanout IOSurface %u, the card copy does not get it (%u such writes so far)", how, rt->surf ? (unsigned)IOSurfaceGetID(rt->surf) : 0u, n + 1);
}
static void rn_frame_done(void);
static void rn_scan_dump(rn_vk_image *src, size_t w, size_t h);
// A frame of the compositor can be spread over more than one command buffer into the same ring surface (measured: 40% of the copies came from the surface of the copy before, all
// within 4 ms), and a copy after the first of them shows an unfinished picture. The copy into the scanout therefore waits: it is made when a command buffer draws into another
// surface, or when nothing more was drawn into this one for 3 ms. /Users/Shared/rednavi-nodefer keeps the copy at the end of every command buffer.
static int rn_defer_on(void) { static int v = -1; if (v < 0) v = access("/Users/Shared/rednavi-nodefer", F_OK) != 0; return v; }
static os_unfair_lock g_defer_lock = OS_UNFAIR_LOCK_INIT;
static rn_vk_image *g_defer_src; static size_t g_defer_w, g_defer_h; static uint64_t g_defer_gen;
static void rn_defer_copy(rn_vk_image *src, size_t w, size_t h) {
    rn_vk *vk = rn_vk_shared();
    rn_vk_image *scan = rn_scanout_image(vk, w, h);
    if (scan && rn_vk_image_copy(vk, src, scan) == 0) { rn_frame_note(src); rn_scan_dump(src, w, h); }
}
static void rn_defer_frame(rn_vk_image *src, size_t w, size_t h) {
    static dispatch_queue_t q; static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("rednavi.scanout", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0)); });
    os_unfair_lock_lock(&g_defer_lock);
    rn_vk_image *old = g_defer_src != src ? g_defer_src : NULL; const size_t ow = g_defer_w, oh = g_defer_h;
    g_defer_src = src; g_defer_w = w; g_defer_h = h;
    const uint64_t gen = ++g_defer_gen;
    os_unfair_lock_unlock(&g_defer_lock);
    if (old) rn_defer_copy(old, ow, oh);         // the surface before this one is finished
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_MSEC), q, ^{
        os_unfair_lock_lock(&g_defer_lock);
        rn_vk_image *mine = g_defer_gen == gen ? g_defer_src : NULL; const size_t mw = g_defer_w, mh = g_defer_h;
        if (mine) g_defer_src = NULL;
        os_unfair_lock_unlock(&g_defer_lock);
        if (mine) rn_defer_copy(mine, mw, mh);
    });
}
static void rn_frame_done(void) {      // RN_PRESENT_FRAME_DONE through the bridge's present call (selector RN_UC_PRESENT)
    static int (*present)(uint32_t, int); static dispatch_once_t once;
    dispatch_once(&once, ^{ present = rn_vk_symbol(rn_vk_shared(), "rn_bridge_present"); });
    if (present) present(0xFFFF0001u, 0);
}

@implementation RedNaviMTLCommandBuffer
+ (void)load { class_addProtocol(self, @protocol(MTLCommandBuffer)); }
- (NSMutableDictionary *)userDictionary { @synchronized (self) { if (!userDict_) userDict_ = [NSMutableDictionary new]; return userDict_; } }
- (instancetype)initWithQueue:(id<MTLCommandQueue>)q device:(id<MTLDevice>)d {
    if ((self = [super init])) {
        queue = q; dev = d; records = [NSMutableArray new]; completed = [NSMutableArray new]; scheduled = [NSMutableArray new]; presents = [NSMutableArray new];
        st = MTLCommandBufferStatusNotEnqueued;
        done = dispatch_semaphore_create(0);
    }
    return self;
}
- (id<MTLDevice>)device { return dev; }
- (id<MTLCommandQueue>)commandQueue { return queue; }
- (BOOL)retainedReferences { return YES; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
- (MTLCommandBufferStatus)status { return st; }
- (NSError *)error { return err; }
- (CFTimeInterval)GPUStartTime { return 0; }
- (CFTimeInterval)GPUEndTime { return 0; }
- (CFTimeInterval)kernelStartTime { return 0; }
- (CFTimeInterval)kernelEndTime { return 0; }
- (void)enqueue { if (st == MTLCommandBufferStatusNotEnqueued) st = MTLCommandBufferStatusEnqueued; }
// A handler added after the commit has nothing left to wait for (the commit is synchronous): it runs now. The arrays are guarded because other threads add handlers while a commit walks them.
- (void)addScheduledHandler:(MTLCommandBufferHandler)block {
    BOOL now; @synchronized(scheduled) { now = st >= MTLCommandBufferStatusCommitted; if (!now) [scheduled addObject:[block copy]]; }
    if (now) block((id)self);
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)block {
    BOOL now; @synchronized(completed) { now = st == MTLCommandBufferStatusCompleted || (st == MTLCommandBufferStatusError && handlersRun); if (!now) [completed addObject:[block copy]]; }
    if (now) block((id)self);
}
- (void)waitUntilScheduled {}
// Private variants of commit that SkyLight and CoreAnimation use (SkyLight's MetalContext::Flush sends commitAndWaitUntilSubmitted, found with tools/ws_flush.sh): the commit here is synchronous,
// so the buffer is submitted and completed when it returns. Without them the message is unimplemented and WindowServer's composition never reaches the GPU.
- (void)commitAndWaitUntilSubmitted { [self commitAsync:YES]; }
- (void)commitAndWaitUntilScheduled { [self commitAsync:YES]; }
- (void)commitAndWaitUntilCompleted { [self commitAsync:YES]; }
// The first version runs everything at commit, so a drawable is presented at the end of commit.
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value {
    RNEventOp *o = [RNEventOp new]; o->event = (RedNaviMTLSharedEvent *)event; o->value = value; [records addObject:o];
}
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value {
    RNEventOp *o = [RNEventOp new]; o->event = (RedNaviMTLSharedEvent *)event; o->value = value; o->wait = YES; [records addObject:o];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable { if (drawable) [presents addObject:drawable]; }
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)presentationTime { [self presentDrawable:drawable]; }
- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)duration { [self presentDrawable:drawable]; }
- (void)pushDebugGroup:(NSString *)string {}
- (void)popDebugGroup {}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder { return (id)[[RedNaviMTLComputeCommandEncoder alloc] initWithCommandBuffer:self]; }
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)t { return [self computeCommandEncoder]; }
- (id<MTLBlitCommandEncoder>)blitCommandEncoder { return (id)[[RedNaviMTLBlitCommandEncoder alloc] initWithCommandBuffer:self]; }
- (id<MTLBlitCommandEncoder>)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)d { return [self blitCommandEncoder]; }
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
    if ((rn_diag && getenv("RN_LOG_BUFFERS"))) rn_log("render pass encoder on command buffer %p, thread %p", self, (void *)pthread_self());
    RNRenderPass *rp = [RNRenderPass new];
    rp->draws = [NSMutableArray new];
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *ca = d.colorAttachments[i];
        if (!ca.texture) break;
        if (![ca.texture isKindOfClass:[RedNaviMTLTexture class]]) { rn_log("renderCommandEncoder: attachment %lu is not a RedNavi texture", (unsigned long)i); return nil; }
        rp->tex[i] = (RedNaviMTLTexture *)ca.texture;
        rp->clear[i] = ca.loadAction == MTLLoadActionClear;
        rp->store[i] = ca.storeAction != MTLStoreActionDontCare;
        if (ca.storeAction == MTLStoreActionMultisampleResolve || ca.storeAction == MTLStoreActionStoreAndMultisampleResolve || ca.resolveTexture) rn_log("renderCommandEncoder: multisample resolve is not implemented, ignored");
        rp->clearColor[i][0] = (float)ca.clearColor.red; rp->clearColor[i][1] = (float)ca.clearColor.green; rp->clearColor[i][2] = (float)ca.clearColor.blue; rp->clearColor[i][3] = (float)ca.clearColor.alpha;
        rp->nt = (unsigned)i + 1;
    }
    id<MTLTexture> dtex = d.depthAttachment.texture ?: d.stencilAttachment.texture;
    if (dtex) {
        RedNaviMTLTexture *dt = (RedNaviMTLTexture *)dtex;
        if (![dtex isKindOfClass:[RedNaviMTLTexture class]] || !dt->dep) rn_log("renderCommandEncoder: the depth attachment is not a RedNavi depth texture, ignored");
        else {
            rp->depthTex = dt;
            rp->clearDepth = d.depthAttachment.texture && d.depthAttachment.loadAction == MTLLoadActionClear;
            rp->clearStencil = d.stencilAttachment.texture && d.stencilAttachment.loadAction == MTLLoadActionClear;
            rp->depthValue = (float)d.depthAttachment.clearDepth; rp->stencilValue = d.stencilAttachment.clearStencil;
        }
    }
    if (!rp->nt && rp->depthTex) {                    // depth only: the pipelines write no colour, into a scratch target of the depth's size (kept per size)
        static NSMutableDictionary<NSNumber *, RedNaviMTLTexture *> *scratch;
        static dispatch_once_t once; dispatch_once(&once, ^{ scratch = [NSMutableDictionary new]; });
        NSNumber *key = @((uint64_t)rp->depthTex->w << 32 | rp->depthTex->h);
        RedNaviMTLTexture *sc;
        @synchronized (scratch) {
            sc = scratch[key];
            if (!sc) {
                MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:rp->depthTex->w height:rp->depthTex->h mipmapped:NO];
                sd.usage = MTLTextureUsageRenderTarget;
                sc = [[RedNaviMTLTexture alloc] initWithDevice:dev descriptor:sd];
                if (sc) scratch[key] = sc;
            }
        }
        if (!sc) { rn_log("renderCommandEncoder: no scratch target for a depth-only pass %lux%lu", (unsigned long)rp->depthTex->w, (unsigned long)rp->depthTex->h); return nil; }
        rp->tex[0] = sc; rp->nt = 1;
    }
    if (!rp->nt) { rn_log("renderCommandEncoder: no colour or depth attachment"); return nil; }
    return (id)[[RedNaviMTLRenderCommandEncoder alloc] initWithCommandBuffer:self pass:rp];
}
- (void)runRenderPass:(RNRenderPass *)rp {
    rn_vk *vk = rn_vk_shared();
    NSUInteger nd = rp->draws.count;
    if (rn_diag && getenv("RN_MAX_DRAWS") && nd > (NSUInteger)atoi(getenv("RN_MAX_DRAWS"))) nd = (NSUInteger)atoi(getenv("RN_MAX_DRAWS"));   // debugging: only the first n draws of every pass
    rn_vk_target tg[8];
    BOOL direct = NO;
    for (unsigned i = 0; i < rp->nt; i++) { tg[i] = (rn_vk_target){rp->tex[i]->img, rp->clear[i], {rp->clearColor[i][0], rp->clearColor[i][1], rp->clearColor[i][2], rp->clearColor[i][3]}, !rp->store[i]};
        RedNaviMTLTexture *rt = rp->tex[i]->parent ? rp->tex[i]->parent : rp->tex[i];
        if (rt->displayScan && rp->tex[i]->texel == rt->texel && rn_scanout_image(vk, rt->w, rt->h)) {
            rn_vk_image *vr = rn_display_vram(vk, rt);
            if (vr && rp->tex[i]->pf != rt->pf) { @synchronized ([RedNaviMTLTexture class]) { vr = rn_vram_as(vk, vr, rp->tex[i]->pf); } }
            if (vr) { tg[i].image = vr; direct = YES; }
        } }
    if ((rn_diag && getenv("RN_LOG_DRAWS")))
        for (unsigned i = 0; i < rp->nt; i++) rn_log("pass: target %u %lux%lu/%lu %s %s clear (%.2f %.2f %.2f %.2f), %lu draws", i, (unsigned long)rp->tex[i]->w, (unsigned long)rp->tex[i]->h, (unsigned long)rp->tex[i]->pf, rp->tex[i]->parent ? "view" : "texture", rp->clear[i] ? "with" : "no", rp->clearColor[i][0], rp->clearColor[i][1], rp->clearColor[i][2], rp->clearColor[i][3], (unsigned long)nd);
    {   // /Users/Shared/rednavi-probe-display: what the render passes into display-sized surfaces are (which one CoreDisplay's final pass is), the first few of each kind
        static unsigned said; struct stat pst;
        if (said < 14 && rp->tex[0] && rp->tex[0]->surf && rp->tex[0]->w == 1920 && rp->tex[0]->h == 1080 && rn_diag && stat("/Users/Shared/rednavi-probe-display", &pst) == 0) {
            said++;
            CFDictionaryRef props = IOSurfaceCopyAllValues(rp->tex[0]->surf);
            NSString *pd = props ? [(__bridge NSDictionary *)props description] : @"-";
            rn_log("display-sized pass: IOSurface %u, %lu targets, %lu draws, first draw %s / %s, properties %s", (unsigned)IOSurfaceGetID(rp->tex[0]->surf), (unsigned long)rp->nt, (unsigned long)nd,
                   nd ? ((RNDraw *)rp->draws[0])->pso->names[0].UTF8String : "-", nd ? ((RNDraw *)rp->draws[0])->pso->names[1].UTF8String : "-", [[pd stringByReplacingOccurrencesOfString:@"\n" withString:@" "] UTF8String]);
            if (props) CFRelease(props);
        }
    }
    // /Users/Shared/rednavi-logdraws (looked at on every pass, so it can be made and removed while the client runs): the draws of the passes into display surfaces, with their buffers
    BOOL logPass = NO;
    if (direct) { static _Atomic unsigned budget; struct stat lst; if (rn_diag && stat("/Users/Shared/rednavi-logdraws", &lst) == 0) { if (atomic_fetch_add(&budget, 1) < 400) { logPass = YES; rn_log("logdraws: pass into IOSurface %u, %lu draws, clear %d", rp->tex[0]->surf ? (unsigned)IOSurfaceGetID(rp->tex[0]->surf) : (rp->tex[0]->parent && rp->tex[0]->parent->surf ? (unsigned)IOSurfaceGetID(rp->tex[0]->parent->surf) : 0u), (unsigned long)nd, rp->clear[0]); } } else atomic_store(&budget, 0); }
    const NSUInteger na = nd ? nd : 1;               // a pass without draws still runs: its clears are done
    rn_vk_draw *dr = calloc(na, sizeof(*dr));
    rn_vk_arg (*va)[RN_VK_MAX_BINDINGS] = calloc(na, sizeof(*va));
    rn_vk_arg (*fa)[RN_VK_MAX_BINDINGS] = calloc(na, sizeof(*fa));
    rn_vk_vbuf (*vbs)[16] = calloc(na, sizeof(*vbs));
    NSMutableArray *temps = rn_batch_keep();     // setBytes buffers stay alive until the batch has run (a freed one goes to the buffer cache and the next pass would overwrite it)
    NSString *bad = (dr && va && fa && vbs) ? nil : @"out of memory";
    for (NSUInteger k = 0; k < nd && !bad; k++) {
        RNDraw *d = rp->draws[k];
        RedNaviMTLRenderPipelineState *p = d->pso;
        if (logPass || (rn_diag && getenv("RN_LOG_DRAWS"))) {
            char tb[400] = ""; int o = 0;
            for (int st = 0; st < 2; st++) for (int i = 0; i < RN_MTL_SLOTS; i++) if (d->texs[st][i] && o < 380) o += snprintf(tb + o, sizeof(tb) - o, " %c%d=%lux%lu/%lu%s", st ? 'f' : 'v', i, (unsigned long)d->texs[st][i]->w, (unsigned long)d->texs[st][i]->h, (unsigned long)d->texs[st][i]->pf, d->texs[st][i] == rp->tex[0] || d->texs[st][i]->parent == rp->tex[0] || (rp->tex[0]->parent && d->texs[st][i] == rp->tex[0]->parent) ? "(=target)" : "");
            char vl[200] = ""; int vo = 0;
            for (unsigned q = 0; q < p->nvattr && vo < 180; q++) vo += snprintf(vl + vo, sizeof(vl) - vo, " a%u=b%u+%u/f%u", p->vattr[q].location, p->vattr[q].binding, p->vattr[q].offset, p->vattr[q].format);
            for (unsigned q = 0; q < p->nvlayout && vo < 180; q++) vo += snprintf(vl + vo, sizeof(vl) - vo, " b%u:%u%s", p->vlayout[q].binding, p->vlayout[q].stride, p->vlayout[q].per_instance ? "i" : "v");
            rn_log("draw %lu: base instance %u, vertex input:%s", (unsigned long)k, d->baseInstance, vl);
            if (getenv("RN_LOG_ARRAYS") && d->texs[0][0] && d->texs[0][0]->texel == 4) {      // debugging: the first texels of vertex texture 0
                const uint32_t *tv = (const uint32_t *)rn_vk_image_staging(d->texs[0][0]->img); char tx[300] = ""; int to = 0;
                for (unsigned q = 0; q < 24 && q < d->texs[0][0]->w * d->texs[0][0]->h && to < 280; q++) to += snprintf(tx + to, sizeof(tx) - to, " %x", tv[q]);
                rn_log("draw %lu: vertex texture 0 (%lux%lu type %ld):%s", (unsigned long)k, (unsigned long)d->texs[0][0]->w, (unsigned long)d->texs[0][0]->h, (long)d->texs[0][0]->ttype, tx);
            }
            rn_log("draw %lu: pso %p %s + %s, prim %u, %u vertices from %u x %u instances%s, blend %d, viewport %d (%g %g %g %g) scissor %d (%u %u %u %u) cull %d, textures:%s", (unsigned long)k, p, p->names[0].UTF8String, p->names[1].UTF8String, d->prim, d->vertexCount, d->vertexStart, d->instanceCount, d->indexBuf ? " (indexed)" : "", p->blend[0].enabled, d->hasViewport, d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3], d->hasScissor, (unsigned)d->scissor[0], (unsigned)d->scissor[1], (unsigned)d->scissor[2], (unsigned)d->scissor[3], (int)d->cull, tb);
        }
        if (logPass && d->indexBuf) {                                    // the indices of the draw, as the card will read them
            const uint8_t *ip = rn_vk_buffer_contents(d->indexBuf->b) + d->indexOffset;
            const unsigned isz = d->index32 ? 4 : 2;
            char ix[200] = ""; int io = 0;
            for (unsigned j = 0; j < d->vertexCount && j < 12 && io < 180; j++) { unsigned v = isz == 4 ? ((const uint32_t *)ip)[j] : ((const uint16_t *)ip)[j]; io += snprintf(ix + io, sizeof(ix) - io, " %u", v); }
            rn_log("  indices (%s, offset %llu, buffer %lu bytes):%s", d->index32 ? "uint32" : "uint16", (unsigned long long)d->indexOffset, (unsigned long)d->indexBuf->len, ix);
        }
        if (logPass || (rn_diag && getenv("RN_LOG_BUFFERS"))) {                       // debugging: what is bound at every buffer index, with the first bytes
            for (int st = 0; st < 2; st++) for (int i = 0; i < RN_MTL_SLOTS; i++) {
                const uint8_t *bp = NULL; NSUInteger len = 0;
                if (d->bufs[st][i]) { bp = rn_vk_buffer_contents(d->bufs[st][i]->b) + d->offs[st][i]; len = d->bufs[st][i]->len - d->offs[st][i]; }
                else if (d->bytesAt[st][i]) { bp = d->bytesAt[st][i].bytes; len = d->bytesAt[st][i].length; }
                if (!bp) continue;
                char hx[400] = ""; for (NSUInteger j = 0; j < len && j < 96; j++) snprintf(hx + 2 * j + j / 4, 4, "%02x ", bp[j]);
                char *sp; while ((sp = strchr(hx, ' '))) *sp = '_';
                rn_log("  %c buffer %d (offset %lu, %lu bytes left, %s): %s", st ? 'f' : 'v', i, (unsigned long)d->offs[st][i], (unsigned long)len, d->bufs[st][i] ? "buffer" : "setBytes", hx);
            }
        }
        rn_vk_render *r = [p renderForPrimitive:d->prim cull:d->cull clockwise:d->clockwise];
        if (!r) { bad = [NSString stringWithFormat:@"RADV has no pipeline for primitive type %u", d->prim]; break; }
        bad = rn_bind_args(dev, p->n[0], p->kind[0], p->mi[0], d->bufs[0], d->offs[0], d->bytesAt[0], d->texs[0], d->samps[0], va[k], temps);
        if (!bad) bad = rn_bind_args(dev, p->n[1], p->kind[1], p->mi[1], d->bufs[1], d->offs[1], d->bytesAt[1], d->texs[1], d->samps[1], fa[k], temps);
        for (unsigned i = 0; !bad && i < p->n[1]; i++)                    // [[color(n)]] inputs read the pass's render target n
            if ((p->kind[1][i] & 0xff) == RN_AIR_COLOR_INPUT) {
                const unsigned m = p->mi[1][i];
                if (m >= rp->nt) bad = [NSString stringWithFormat:@"the fragment function reads colour attachment %u, the pass has %u", m, rp->nt];
                else fa[k][i].image = tg[m].image;
            }
        for (unsigned i = 0; !bad && i < p->nvlayout; i++) {              // vertex buffers of the vertex descriptor, by buffer index
            const unsigned bi = p->vlayout[i].binding;
            if (d->bufs[0][bi]) { vbs[k][bi] = (rn_vk_vbuf){d->bufs[0][bi]->b, d->offs[0][bi]}; continue; }
            if (d->bytesAt[0][bi]) {
                RedNaviMTLBuffer *t = [[RedNaviMTLBuffer alloc] initWithDevice:dev length:d->bytesAt[0][bi].length options:0];
                if (!t) { bad = @"no memory for vertex data"; break; }
                memcpy(rn_vk_buffer_contents(t->b), d->bytesAt[0][bi].bytes, d->bytesAt[0][bi].length);
                [temps addObject:t];
                vbs[k][bi] = (rn_vk_vbuf){t->b, 0};
                continue;
            }
            bad = [NSString stringWithFormat:@"no vertex buffer bound at index %u", bi];
        }
        dr[k] = (rn_vk_draw){.pipe = r, .vbufs = vbs[k], .has_blend_color = d->hasBlendColor, .vargs = va[k], .fargs = fa[k], .has_viewport = d->hasViewport, .has_scissor = d->hasScissor, .first_vertex = d->vertexStart, .vertex_count = d->vertexCount,
                             .instance_count = d->instanceCount, .first_instance = d->baseInstance};
        if (d->indexBuf) { dr[k].index_buffer = d->indexBuf->b; dr[k].index_offset = d->indexOffset; dr[k].index32 = d->index32; dr[k].base_vertex = d->baseVertex; }
        dr[k].ds = d->ds;
        memcpy(dr[k].blend_color, d->blendColor, sizeof(dr[k].blend_color));
        memcpy(dr[k].viewport, d->viewport, sizeof(dr[k].viewport));
        memcpy(dr[k].scissor, d->scissor, sizeof(dr[k].scissor));
    }
    const uint64_t phv = mach_absolute_time();
    rn_vk_snap_small = g_pd_dir[0] && nd < 16 && tg[0].image && rp->tex[0] && rp->tex[0]->w == rp->tex[0]->h && (rp->tex[0]->w == 93 || rp->tex[0]->w == 137 || rp->tex[0]->w == 161 || rp->tex[0]->w == 174 || rp->tex[0]->w == 249 || rp->tex[0]->w == 256);
    rn_vk_snap_on = (g_pd_left > 0 && nd >= 16) || rn_vk_snap_small; rn_vk_snap_seq = g_pd_seq;   // pass capture running: snapshots of the target after some draws (rn_snap_write)
    rn_vk_depth_target dtv = {0};
    if (rp->depthTex) dtv = (rn_vk_depth_target){rp->depthTex->dep, rp->clearDepth, rp->depthValue, rp->clearStencil, rp->stencilValue};
    int rc = bad ? -1 : rn_vk_render_pass_ds(vk, tg, rp->nt, rp->depthTex ? &dtv : NULL, dr, (unsigned)nd);
    rn_vk_snap_on = 0; rn_vk_snap_small = 0;
    rn_ph(1, phv);
    if (!rc) rn_locator_note(rp, rp->tex[0]->parent ? rp->tex[0]->parent : rp->tex[0]);
    if (!bad && !rc && rp->nt) {
        BOOL dump = rn_passdump_on();
        const BOOL smallPass = rp->tex[0] && (size_t)rp->tex[0]->w * rp->tex[0]->h < 40000;      // icon, glyph and menu passes (64x64, 160x64, 352x64 ...) have a budget of their own: they used up the main one before a window was drawn, but a tab or hover layer can be one of them
        if (dump && smallPass && g_pd_small-- <= 0) dump = NO;
        // The window shadow is made once, early (the first blur passes ran before the 160 passes of a capture were over): a pass into a square target of shadow size is captured when
        // the capture is armed even after the budget is spent, up to 120 of them.
        static int shadowLeft = 200;
        if (!dump && g_pd_dir[0] && shadowLeft > 0 && rp->tex[0] && rp->tex[0]->w == rp->tex[0]->h && (rp->tex[0]->w == 93 || rp->tex[0]->w == 137 || rp->tex[0]->w == 161 || rp->tex[0]->w == 174 || rp->tex[0]->w == 249 || rp->tex[0]->w == 256)) { dump = YES; shadowLeft--; }
        if (dump) { rn_passdump(rp, tg[0].image, nd); if (smallPass) g_pd_left++; }      // a small pass is not counted against the main budget
    }
    if (direct && !rc) {         // a pass that only fills the surface with a colour (CoreDisplay clears a surface before it draws into it) is not a finished frame; any other is, and goes to the scanout
        static int noskip = -1;      // experiment switch /Users/Shared/rednavi-noframeskip: every pass into a scanout surface is a frame
        if (noskip < 0) noskip = access("/Users/Shared/rednavi-noframeskip", F_OK) == 0;
        BOOL onlyFills = nd > 0 && !noskip;     // every draw is looked at: a pass that starts with a fill and then draws content is a frame, and one left out stays off the screen until the next change
        for (NSUInteger k = 0; k < nd && onlyFills; k++) { RNDraw *dk = rp->draws[k]; onlyFills = dk->pso && dk->pso->names[1] && [dk->pso->names[1] hasPrefix:@"ColorFill"]; }
        if (!onlyFills) {
            RedNaviMTLTexture *rt = rp->tex[0]->parent ? rp->tex[0]->parent : rp->tex[0];
            scanSrc = rn_display_vram(vk, rt); scanW = rt->w; scanH = rt->h;      // the frame is complete when the command buffer is: one copy into the scanout at its end
            directPasses++;
        }
    }
    if (!bad && !rc && rn_diag && getenv("RN_DUMP_PASSES") && rp->nt && rp->tex[0]->texel == 4 && rp->store[0]) {   // debugging: every pass's first target as a PNG in the directory named by the variable
        static unsigned seq;
        RedNaviMTLTexture *t = rp->tex[0];
        rn_vk_batch_sync(vk);                                           // the pass is only recorded; its pixels reach the host copy when the batch has run
        const uint8_t *src = rn_vk_image_staging(t->img);
        const size_t n = t->w * t->h;
        uint8_t *px = malloc(n * 4);
        for (size_t i = 0; i < n; i++) {                                // to RGBA; BGRA8 formats (80, 81) are swapped
            const BOOL bgra = t->pf == MTLPixelFormatBGRA8Unorm || t->pf == MTLPixelFormatBGRA8Unorm_sRGB;
            px[4 * i] = src[4 * i + (bgra ? 2 : 0)]; px[4 * i + 1] = src[4 * i + 1]; px[4 * i + 2] = src[4 * i + (bgra ? 0 : 2)]; px[4 * i + 3] = src[4 * i + 3];
        }
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, px, n * 4, NULL);
        CGImageRef im = CGImageCreate(t->w, t->h, 8, 32, t->w * 4, cs, kCGImageAlphaLast | kCGBitmapByteOrderDefault, dp, NULL, false, kCGRenderingIntentDefault);
        NSString *path = [NSString stringWithFormat:@"%s/pass%02u_%lux%lu.png", getenv("RN_DUMP_PASSES"), seq++, (unsigned long)t->w, (unsigned long)t->h];
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, NULL);
        if (dst) { CGImageDestinationAddImage(dst, im, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); }
        CGImageRelease(im); CGDataProviderRelease(dp); CGColorSpaceRelease(cs); free(px);
    }
    free(dr); free(va); free(fa); free(vbs);
    if (bad) [self fail:bad];
    else if (rc == 4) [self fail:@"a buffer offset is not a multiple of the storage buffer offset alignment, or beyond the buffer"];
    else if (rc == 5) [self fail:@"the render pipeline does not match the attachments of the pass"];
    else if (rc == 6) [self fail:@"a resource of the render pipeline is missing or cannot be used as bound"];
    else if (rc) [self fail:[NSString stringWithFormat:@"render pass failed (%d)", rc]];
}
- (void)dealloc {
    static unsigned said;
    if (st < MTLCommandBufferStatusCommitted && records.count && said < 40) { said++; rn_log("command buffer released without commit: %lu records, status %ld, label %s", (unsigned long)records.count, (long)st, label_ ? label_.UTF8String : "-"); }
}
- (void)fail:(NSString *)why {
    rn_log("command buffer failed: %s", why.UTF8String);
    err = [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal userInfo:@{NSLocalizedDescriptionKey: why}];
    st = MTLCommandBufferStatusError;
}
// The first version runs the recorded dispatches synchronously on the calling thread.
// One commit at a time: the Vulkan queue and command pool are the process's single ones and are not thread safe, and the textures and buffers a commit copies are shared with the others.
// Recursive: a completion handler may commit another command buffer.
- (void)commit { [self commitAsync:NO]; }
// Private: AppleMetalOpenGLRenderer commits with it (it also keeps the buffer for the renderer's own bookkeeping, which needs nothing here).
- (void)commitAndHold { [self commitAsync:NO]; }
// Commit order is kept by one serial queue; the caller goes on while the card works. The completion handlers, the presents and the semaphore run at the end of the body, on the queue.
// A buffer that waits for an event runs on the caller's thread after the queue is drained: the signal may come from a buffer committed later by another thread, and the queue must not block on it.
- (void)commitAsync:(BOOL)wait {
    static dispatch_queue_t q; static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("rednavi.commit", DISPATCH_QUEUE_SERIAL); dispatch_queue_set_specific(q, g_commit_key, g_commit_key, NULL); });
    if (st >= MTLCommandBufferStatusCommitted) return;
    const BOOL onQueue = dispatch_get_specific(g_commit_key) == g_commit_key;
    BOOL waits = NO;
    for (id rec in records) if ([rec isKindOfClass:[RNEventOp class]] && ((RNEventOp *)rec)->wait) { waits = YES; break; }
    void (^run)(void) = ^{
        static NSRecursiveLock *lock; static dispatch_once_t o2;
        dispatch_once(&o2, ^{ lock = [NSRecursiveLock new]; });
        const uint64_t t0 = mach_absolute_time();
        [lock lock];
        [self commitBody];
        [lock unlock];
        rn_commit_stats(mach_absolute_time() - t0);
    };
    if (onQueue) { run(); return; }
    if (waits) { dispatch_sync(q, ^{}); run(); return; }
    {   // the textures are in use from now until the body has run: host accesses to them wait (rn_tex_wait_idle)
        NSMutableOrderedSet<RedNaviMTLTexture *> *used = [NSMutableOrderedSet orderedSet];
        [self eachTexture:^(RedNaviMTLTexture *t, BOOL written) { if (t) [used addObject:t->parent ? t->parent : t]; }];
        for (RedNaviMTLTexture *t in used) atomic_fetch_add(&t->queued_, 1);
        queuedTex = used.array;
        {   // An IOSurface counts as in use (IOSurfaceIsInUse) from the commit until the body has run, as it does for Apple's own GPU drivers, so that a client does not draw its next frame into a
            // surface that is still waiting to be read. Switch /Users/Shared/rednavi-nousecount turns it off.
            static int on = -1;
            if (on < 0) on = access("/Users/Shared/rednavi-nousecount", F_OK) != 0;
            if (on) for (RedNaviMTLTexture *t in queuedTex) if (t->surf) IOSurfaceIncrementUseCount(t->surf);
            useCounted = on;
        }
    }
    st = MTLCommandBufferStatusCommitted;       // visible at once; the body sets the final state
    dispatch_async(q, run);
    {   // Experiment switch /Users/Shared/rednavi-synccommit: every commit returns only when its body has run, so that nothing the client does after the commit (reusing a buffer, drawing into a surface)
        // can happen before the card has read what the commit referenced.
        static int sync = -1;
        if (sync < 0) sync = access("/Users/Shared/rednavi-synccommit", F_OK) == 0;
        if (sync) wait = YES;
    }
    if (wait) [self waitUntilCompleted];
}
// Every texture the records use (nil entries included), with whether the record writes it.
- (void)eachTexture:(void (^)(RedNaviMTLTexture *, BOOL))note {
    for (id rec in records) {
        if ([rec isKindOfClass:[RNRenderPass class]]) {
            RNRenderPass *rp = rec;
            for (unsigned i = 0; i < rp->nt; i++) note(rp->tex[i], rp->store[i]);
            for (RNDraw *dw in rp->draws) for (int s2 = 0; s2 < 2; s2++) for (int i = 0; i < RN_MTL_SLOTS; i++) note(dw->texs[s2][i], NO);
        } else if ([rec isKindOfClass:[RNEventOp class]]) {
            // no textures
        } else if ([rec isKindOfClass:[RNBlit class]]) {
            RNBlit *bl = rec;
            note(bl->st, NO);
            note(bl->dt, YES);
        } else {
            RNDispatch *dp = rec;
            for (int i = 0; i < RN_MTL_SLOTS; i++) note(dp->texs[i], dp->texs[i] && (dp->texs[i]->usage_ & MTLTextureUsageShaderWrite));
        }
    }
}
- (void)commitBody {
    if ((rn_diag && getenv("RN_LOG_BUFFERS"))) rn_log("commit: command buffer %p, %lu records, thread %p", self, (unsigned long)records.count, (void *)pthread_self());
    if (ran) return;
    ran = YES;
    {   static dispatch_once_t swonce;       // which experiment switches this process runs with, so that what is seen on the screen can be tied to a configuration
        dispatch_once(&swonce, ^{
            static const char *const sw[] = {"nodirectscan", "nozerocopy", "fullbarrier", "noframeskip", "nopool", "passsubmit", "yuvgrey", "nodefer", "dcc"};
            char line[160] = ""; char path[64];
            for (unsigned i = 0; i < sizeof(sw) / sizeof(*sw); i++) { snprintf(path, sizeof(path), "/Users/Shared/rednavi-%s", sw[i]); if (access(path, F_OK) == 0) { strlcat(line, " ", sizeof(line)); strlcat(line, sw[i], sizeof(line)); } }
            rn_log("experiment switches:%s; RADV_DEBUG=%s", line[0] ? line : " none", getenv("RADV_DEBUG") ? getenv("RADV_DEBUG") : "(unset)");
        });
    }
    st = MTLCommandBufferStatusCommitted;
    NSArray *schedHandlers; @synchronized(scheduled) { st = MTLCommandBufferStatusCommitted; schedHandlers = [scheduled copy]; [scheduled removeAllObjects]; }
    for (MTLCommandBufferHandler h in schedHandlers) h((id)self);
    rn_vk *vk = rn_vk_shared();
    if (!vk) [self fail:@"RADV is not available"];
    // IOSurface-backed textures: what the surface holds goes to the images first, what the command buffer wrote goes back to the surface afterwards
    NSMutableOrderedSet<RedNaviMTLTexture *> *surfUsed = [NSMutableOrderedSet orderedSet], *surfWritten = [NSMutableOrderedSet orderedSet];
    void (^note)(RedNaviMTLTexture *, BOOL) = ^(RedNaviMTLTexture *t, BOOL written) {
        if (t && t->parent) t = t->parent;       // a view shares its root's storage and host copy: the root is what is synchronised (a view has none of the buffer fields and not the plane)
        if (!t || (!t->surf && !t->srcBuf && !t->srcRaw)) return;
        [surfUsed addObject:t];
        if (written) [surfWritten addObject:t];
    };
    [self eachTexture:note];
    { const uint64_t ph3 = mach_absolute_time(); for (RedNaviMTLTexture *t in surfUsed) { [t syncFromSurface]; [t syncFromBuffer]; } rn_ph(3, ph3); }
    {   // /Users/Shared/rednavi-trace: what each command buffer draws (a client such as WindowServer has no environment to set)
        struct stat tst;
        if (rn_diag && stat("/Users/Shared/rednavi-trace", &tst) == 0)
            for (id rec in records) {
                if ([rec isKindOfClass:[RNRenderPass class]]) {
                    RNRenderPass *rp = rec;
                    for (unsigned i = 0; i < rp->nt; i++) {
                        RedNaviMTLTexture *t = rp->tex[i];
                        rn_log("trace: pass target %u: %lux%lu format %lu%s, %lu draws, %s%s, clear %.2f %.2f %.2f %.2f", i, (unsigned long)t->w, (unsigned long)t->h, (unsigned long)t->pf, t->surf ? " (IOSurface)" : "",
                               (unsigned long)rp->draws.count, rp->clear[i] ? "cleared" : "loaded", rp->store[i] ? ", stored" : "", rp->clearColor[i][0], rp->clearColor[i][1], rp->clearColor[i][2], rp->clearColor[i][3]);
                    }
                    static unsigned said;
                    if (said < 120) {   // what the draws sample: size, format, and how many of 4096 sampled texels of the host copy are not zero
                        said++;
                        for (RNDraw *dw in rp->draws) rn_log("trace:   draw %s / %s: %u vertices x %u instances", dw->pso && dw->pso->names[0] ? [dw->pso->names[0] UTF8String] : "-", dw->pso && dw->pso->names[1] ? [dw->pso->names[1] UTF8String] : "-", dw->vertexCount, dw->instanceCount);
                        for (RNDraw *dw in rp->draws) for (int st = 0; st < 2; st++) for (int ti = 0; ti < RN_MTL_SLOTS; ti++) {
                            RedNaviMTLTexture *t = dw->texs[st][ti];
                            if (!t) continue;
                            unsigned nz = 0; uint32_t v0 = 0;
                            const uint8_t *sp = t->img ? rn_vk_image_staging(t->img) : NULL;
                            if (sp && t->texel == 4 && t->w && t->h) for (unsigned i = 0; i < 4096; i++) { size_t yy = ((size_t)i * 2654435761u) % t->h, xx = ((size_t)i * 40503u) % t->w; uint32_t px = *(const uint32_t *)(sp + (yy * t->w + xx) * 4); if (px) nz++; if (i == 0) v0 = px; }
                            rn_log("trace:   draw samples %c%d: %lux%lu fmt %lu type %ld%s, %u of 4096 nonzero (first %08x)", st ? 'f' : 'v', ti, (unsigned long)t->w, (unsigned long)t->h, (unsigned long)t->pf, (long)t->ttype, t->surf ? " IOSurface" : "", nz, v0);
                        }
                    }
                } else if ([rec isKindOfClass:[RNBlit class]]) rn_log("trace: blit");
                else if ([rec isKindOfClass:[RNDispatch class]]) rn_log("trace: compute dispatch");
            }
    }
    if ((rn_diag && getenv("RN_LOG_BUFFERS")))
        for (id rec in records) if ([rec isKindOfClass:[RNRenderPass class]]) {
            RNRenderPass *rp = rec;
            for (RNDraw *dw in rp->draws) if (dw->bufs[0][1]) {
                const uint8_t *bp = (const uint8_t *)rn_vk_buffer_contents(dw->bufs[0][1]->b) + dw->offs[0][1];
                char hx[100] = ""; for (int j = 0; j < 32; j++) snprintf(hx + 2 * j, 4, "%02x", bp[j]);
                rn_log("  commit start: buffer %p offset %lu: %s", dw->bufs[0][1], (unsigned long)dw->offs[0][1], hx);
            }
        }
    NSMutableSet *noCopyUsed = [NSMutableSet set];
    {   // the buffers made over caller memory that this command buffer touches
        void (^nb)(RedNaviMTLBuffer *) = ^(RedNaviMTLBuffer *bf) { if (bf && bf->userPtr) [noCopyUsed addObject:bf]; };
        for (id rec in records) {
            if ([rec isKindOfClass:[RNRenderPass class]]) { for (RNDraw *dw in ((RNRenderPass *)rec)->draws) { for (int s2 = 0; s2 < 2; s2++) for (int i = 0; i < RN_MTL_SLOTS; i++) nb(dw->bufs[s2][i]); nb(dw->indexBuf); } }
            else if ([rec isKindOfClass:[RNDispatch class]]) { for (int i = 0; i < RN_MTL_SLOTS; i++) nb(((RNDispatch *)rec)->bufs[i]); }
            else if ([rec isKindOfClass:[RNBlit class]]) { nb(((RNBlit *)rec)->sb); nb(((RNBlit *)rec)->db); }
        }
    }
    { const uint64_t ph4i = mach_absolute_time(); [RedNaviMTLBuffer syncNoCopy:noCopyUsed in:YES]; rn_ph(4, ph4i); }
    if (vk) rn_vk_batch_begin(vk);
    for (id rec in records) {
        if (st == MTLCommandBufferStatusError) break;
        if ([rec isKindOfClass:[RNRenderPass class]]) { atomic_fetch_add(&g_passes, 1); const uint64_t ph0 = mach_absolute_time(); [self runRenderPass:rec]; rn_ph(0, ph0); continue; }
        if ([rec isKindOfClass:[RNBlit class]]) { NSString *why = rn_run_blit(rec); if (why) [self fail:why]; continue; }
        if ([rec isKindOfClass:[RNEventOp class]]) {
            RNEventOp *o = rec;
            if (![o->event isKindOfClass:[RedNaviMTLSharedEvent class]]) { [self fail:@"an event that is not from RedNavi"]; break; }
            // the batch ends first: a signal must not run ahead of the work before it, and a wait must not hold the card lock
            { const int br = vk ? rn_vk_batch_end(vk) : 0; if (br) [self fail:[NSString stringWithFormat:@"render passes failed (%d)", br]]; [rn_batch_keep() removeAllObjects]; }
            BOOL got = YES;
            if (!o->wait) { if (o->event.signaledValue < o->value) o->event.signaledValue = o->value; }
            else got = [o->event waitFor:o->value timeoutMS:10000];
            if (vk) rn_vk_batch_begin(vk);
            if (!got) [self fail:[NSString stringWithFormat:@"waiting for an event to reach %llu timed out after 10 s (it is at %llu)", o->value, o->event.signaledValue]];
            continue;
        }
        RNDispatch *d = rec;
        RedNaviMTLComputePipelineState *p = d->pso;
        rn_vk_arg av[RN_VK_MAX_BINDINGS];
        memset(av, 0, sizeof(av));
        NSMutableArray *temps = [NSMutableArray new];       // buffers made from setBytes data, alive until the dispatch is done
        NSString *bad = rn_bind_args(dev, p->nbind, p->kind, p->metalIndex, d->bufs, d->offs, d->bytesAt, d->texs, d->samps, av, temps);
        if (bad) { [self fail:bad]; break; }
        const uint32_t groups[3] = {(uint32_t)d->groups.width, (uint32_t)d->groups.height, (uint32_t)d->groups.depth};
        const uint32_t local[3] = {(uint32_t)d->tpt.width, (uint32_t)d->tpt.height, (uint32_t)d->tpt.depth};
        const uint32_t grid[3] = {(uint32_t)d->threads.width, (uint32_t)d->threads.height, (uint32_t)d->threads.depth};
        int rc = p->dyn ? rn_vk_dispatch_threads(vk, p->c, av, grid, local) : rn_vk_dispatch(vk, p->c, av, d->pushLen ? d->push : NULL, groups, local);
        if (rc == 4) [self fail:@"a buffer offset is not a multiple of the storage buffer offset alignment, or beyond the buffer"];
        else if (rc == 6) [self fail:@"a resource of the pipeline is missing or cannot be used as bound (a texture whose format cannot be a storage image?)"];
        else if (rc) [self fail:[NSString stringWithFormat:@"dispatch failed (%d)", rc]];
    }
    if (vk && scanSrc && st != MTLCommandBufferStatusError && !rn_defer_on()) {       // the scanout copy rides in the same submit as the passes
        rn_vk_image *scan = rn_scanout_image(vk, scanW, scanH);
        if (scan && rn_vk_batch_copy(vk, scanSrc, scan) == 0) scanCopied = YES;
    }
    if (vk) { const int br = rn_vk_batch_end(vk); if (br) [self fail:[NSString stringWithFormat:@"render passes failed (%d)", br]]; }
    [rn_batch_keep() removeAllObjects];
    if (st != MTLCommandBufferStatusError) for (RedNaviMTLTexture *t in surfWritten) [t syncToSurface];
    {   struct stat tst2;
        if (rn_diag && stat("/Users/Shared/rednavi-trace", &tst2) == 0)
            for (id rec in records) if ([rec isKindOfClass:[RNRenderPass class]]) { RNRenderPass *rp = rec; if (rp->nt && rp->tex[0]) rn_trace_dump(rp->tex[0], (unsigned)rp->draws.count); }
    }
    { const uint64_t ph4 = mach_absolute_time(); if (st != MTLCommandBufferStatusError) [RedNaviMTLBuffer syncNoCopy:noCopyUsed in:NO]; rn_ph(4, ph4); }
    if (scanSrc && st != MTLCommandBufferStatusError && rn_defer_on()) { rn_frame_done(); rn_defer_frame(scanSrc, scanW, scanH); }
    else if (scanSrc && st != MTLCommandBufferStatusError) {      // the frame reaches the scanout before the buffer is reported complete: a completion handler lets the client reuse the surface
        rn_vk_image *scan = scanCopied ? NULL : rn_scanout_image(rn_vk_shared(), scanW, scanH);
        if (scanCopied || (scan && rn_vk_image_copy(rn_vk_shared(), scanSrc, scan) == 0)) { rn_frame_done(); rn_frame_note(scanSrc); rn_scan_dump(scanSrc, scanW, scanH); }
        else { static unsigned said; if (said++ < 20) rn_log("direct scanout: the finished frame (%lux%lu) could not be copied into the scanout", (unsigned long)scanW, (unsigned long)scanH); }
    }
    if (queuedTex) {        // the card's work is done and the surfaces are written back: host accesses to the textures may go on
        pthread_mutex_lock(&g_texq_mu);
        for (RedNaviMTLTexture *t in queuedTex) { atomic_fetch_sub(&t->queued_, 1); if (useCounted && t->surf) IOSurfaceDecrementUseCount(t->surf); }
        pthread_cond_broadcast(&g_texq_cv);
        pthread_mutex_unlock(&g_texq_mu);
        queuedTex = nil;
    }
    if (st != MTLCommandBufferStatusError) st = MTLCommandBufferStatusCompleted;
    NSArray *doneHandlers; @synchronized(completed) { handlersRun = YES; doneHandlers = [completed copy]; [completed removeAllObjects]; }
    { const uint64_t ph6 = mach_absolute_time(); for (MTLCommandBufferHandler h in doneHandlers) h((id)self); rn_ph(6, ph6); }
    for (id<MTLDrawable> dr in presents) [dr present];
    dispatch_semaphore_signal(done);
}
- (void)waitUntilCompleted {
    if (st < MTLCommandBufferStatusCommitted) return;
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    dispatch_semaphore_signal(done);
}
@end


// Textures made inside a buffer ("client storage": CoreAnimation puts the pixels of a small image in a pooled MTLBuffer and makes a texture over them; without this it logged
// "nil client storage texture from buffer" thousands of times and the image was never drawn). The texture keeps the buffer and copies the rows out of it before each command buffer.
@implementation RedNaviMTLBuffer (RNTexture)
- (id<MTLTexture>)newTiledTextureWithDescriptor:(MTLTextureDescriptor *)desc offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bytesPerRow { return [self newTextureWithDescriptor:desc offset:offset bytesPerRow:bytesPerRow]; }
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)desc offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bytesPerRow {
    static int said;
    if (said < 12) { said++; rn_log("newTextureWithDescriptor:offset:bytesPerRow: on a buffer of %lu bytes: %lux%lu format %lu type %ld offset %lu bytes per row %lu", (unsigned long)len, (unsigned long)desc.width, (unsigned long)desc.height, (unsigned long)desc.pixelFormat, (long)desc.textureType, (unsigned long)offset, (unsigned long)bytesPerRow); }
    RedNaviMTLTexture *t = [[RedNaviMTLTexture alloc] initWithDevice:dev descriptor:desc];
    if (!t) return nil;
    if (offset + (t->h ? (t->h - 1) * bytesPerRow : 0) + t->w * t->texel > len || bytesPerRow < t->w * t->texel) {
        rn_log("texture over a buffer: %lux%lu with %u bytes per texel, offset %lu, bytes per row %lu does not fit a buffer of %lu bytes", (unsigned long)t->w, (unsigned long)t->h, t->texel, (unsigned long)offset, (unsigned long)bytesPerRow, (unsigned long)len);
        return nil;
    }
    t->srcBuf = self; t->srcOff = offset; t->srcBpr = bytesPerRow;
    [t syncFromBuffer];
    return t;
}
@end


// A texture over memory the caller owns (the SPI CoreAnimation uses for "client storage" images, newTiledTextureWithBytesNoCopy:...): the rows are copied out of the memory before each
// command buffer that uses the texture (the caller may change them between frames) and the caller's deallocator runs when the texture goes.
static RedNaviMTLTexture *rn_texture_over_bytes(id<MTLDevice> dev, MTLTextureDescriptor *desc, void *bytes, NSUInteger len, NSUInteger off, NSUInteger bpr, void (^dealloc)(void *, NSUInteger)) {
    static int said;
    if (said < 12) { said++; rn_log("texture over client memory: %lux%lu format %lu, %lu bytes, offset %lu, bytes per row %lu", (unsigned long)desc.width, (unsigned long)desc.height, (unsigned long)desc.pixelFormat, (unsigned long)len, (unsigned long)off, (unsigned long)bpr); }
    RedNaviMTLTexture *t = [[RedNaviMTLTexture alloc] initWithDevice:dev descriptor:desc];
    if (!t) return nil;
    if (!bytes || bpr < t->w * t->texel || off + (t->h ? (t->h - 1) * bpr : 0) + t->w * t->texel > len) {
        rn_log("texture over client memory: %lux%lu with %u bytes per texel does not fit %lu bytes at offset %lu, bytes per row %lu", (unsigned long)t->w, (unsigned long)t->h, t->texel, (unsigned long)len, (unsigned long)off, (unsigned long)bpr);
        return nil;
    }
    t->srcRaw = (uint8_t *)bytes + off; t->srcBase = bytes; t->srcLen = len; t->srcBpr = bpr;
    if (dealloc) t->srcDealloc = [dealloc copy];
    [t syncFromBuffer];
    return t;
}

@interface RedNaviMTLCommandQueue : NSObject {
    id<MTLDevice> dev;
    NSString *label_;
}
- (instancetype)initWithDevice:(id<MTLDevice>)d;
@end
@implementation RedNaviMTLCommandQueue
+ (void)load { class_addProtocol(self, @protocol(MTLCommandQueue)); }
- (instancetype)initWithDevice:(id<MTLDevice>)d { if ((self = [super init])) dev = d; return self; }
- (id<MTLDevice>)device { return dev; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
- (id<MTLCommandBuffer>)commandBuffer { return (id)[[RedNaviMTLCommandBuffer alloc] initWithQueue:(id)self device:dev]; }
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [self commandBuffer]; }
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)descriptor { return [self commandBuffer]; }
- (void)insertDebugCaptureBoundary {}
@end


// ---------------------------------------------------------------------------------------------------------------------------------------- libraries and functions


// Function constants (docs/M6_WINDOWSERVER_STUDY.md: QuartzCore builds its shaders from one source with them). MTLFunctionConstantValues has no public way to read back what was set;
// its internal class has newIndexedConstantArray and newNamedConstantArray (MTLIndexedConstantValue: index, dataType, data; MTLNamedConstantValue: the same with name). They are private,
// so they are used only if the object answers them, and a macOS without them makes function specialization fail with a message instead of ignoring the values.
static unsigned rn_data_type_size(MTLDataType t) {
    switch (t) {
    case MTLDataTypeBool: case MTLDataTypeChar: case MTLDataTypeUChar: return 1;
    case MTLDataTypeBool2: case MTLDataTypeChar2: case MTLDataTypeUChar2: case MTLDataTypeShort: case MTLDataTypeUShort: case MTLDataTypeHalf: return 2;
    case MTLDataTypeBool3: case MTLDataTypeChar3: case MTLDataTypeUChar3: return 3;
    case MTLDataTypeBool4: case MTLDataTypeChar4: case MTLDataTypeUChar4: case MTLDataTypeShort2: case MTLDataTypeUShort2: case MTLDataTypeHalf2: case MTLDataTypeInt: case MTLDataTypeUInt: case MTLDataTypeFloat: return 4;
    case MTLDataTypeShort3: case MTLDataTypeUShort3: case MTLDataTypeHalf3: return 6;
    case MTLDataTypeShort4: case MTLDataTypeUShort4: case MTLDataTypeHalf4: case MTLDataTypeInt2: case MTLDataTypeUInt2: case MTLDataTypeFloat2: return 8;
    case MTLDataTypeInt3: case MTLDataTypeUInt3: case MTLDataTypeFloat3: return 12;
    case MTLDataTypeInt4: case MTLDataTypeUInt4: case MTLDataTypeFloat4: return 16;
    default: return 0;
    }
}
static NSArray<NSDictionary *> *rn_capture_constants(MTLFunctionConstantValues *values, NSString **why) {
    if (!values) return nil;
    SEL si = sel_registerName("newIndexedConstantArray"), sn = sel_registerName("newNamedConstantArray");
    if (![values respondsToSelector:si] || ![values respondsToSelector:sn]) { *why = @"cannot read the function constant values on this macOS (private accessors missing)"; return nil; }
    NSMutableArray *out = [NSMutableArray array];
    NSArray *idx = ((id (*)(id, SEL))objc_msgSend)(values, si), *named = ((id (*)(id, SEL))objc_msgSend)(values, sn);
    for (int k = 0; k < 2; k++) for (id v in (k ? named : idx)) {
        if (![v respondsToSelector:@selector(dataType)] || ![v respondsToSelector:@selector(data)]) { *why = @"unexpected function constant value object"; return nil; }
        const MTLDataType t = ((MTLDataType (*)(id, SEL))objc_msgSend)(v, @selector(dataType));
        const unsigned n = rn_data_type_size(t);
        const void *p = ((const void *(*)(id, SEL))objc_msgSend)(v, @selector(data));
        if (!n || !p) { *why = [NSString stringWithFormat:@"function constant with data type %lu is not supported", (unsigned long)t]; return nil; }
        NSData *d = [NSData dataWithBytes:p length:n];
        if (k) [out addObject:@{@"name": ((NSString *(*)(id, SEL))objc_msgSend)(v, @selector(name)), @"data": d}];
        else [out addObject:@{@"index": @(((NSUInteger (*)(id, SEL))objc_msgSend)(v, @selector(index))), @"data": d}];
    }
    return out;
}
// The C array rn_air_translate_stage_fc takes, for a function's constants (the strings stay owned by `fcs`).
static unsigned rn_fc_array(NSArray<NSDictionary *> *fcs, rn_air_const *out, unsigned max) {
    unsigned n = 0;
    for (NSDictionary *d in fcs) {
        if (n >= max) break;
        NSData *data = d[@"data"];
        out[n++] = (rn_air_const){d[@"index"] ? (int32_t)[d[@"index"] intValue] : -1, d[@"name"] ? [d[@"name"] UTF8String] : NULL, (uint32_t)data.length, data.bytes};
    }
    return n;
}

@interface RedNaviMTLFunction : NSObject {
@public
    NSString *name;
    MTLFunctionType type;
    NSData *bitcode;
    id<MTLDevice> dev;
    NSString *label_;
    NSArray<NSDictionary *> *fcs;                    // function constants: @"index" (NSNumber) or @"name", and @"data" (NSData)
    NSArray<NSDictionary *> *args;                   // OpenGL functions: the buffers, textures and samplers of the AIR metadata (rn_gl_args), for the pipeline reflection
}
@end
@implementation RedNaviMTLFunction
+ (void)load { class_addProtocol(self, @protocol(MTLFunction)); }
- (id<MTLDevice>)device { return dev; }
- (NSString *)name { return name; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
- (MTLFunctionType)functionType { return type; }
- (MTLPatchType)patchType { return MTLPatchTypeNone; }
- (NSInteger)patchControlPointCount { return -1; }
- (NSArray *)vertexAttributes { return nil; }
- (NSArray *)stageInputAttributes { return nil; }
- (NSDictionary<NSString *, MTLFunctionConstant *> *)functionConstantsDictionary { return @{}; }
- (MTLFunctionOptions)options { return MTLFunctionOptionNone; }
@end

@interface RedNaviMTLLibrary : NSObject {
@public
    id<MTLDevice> dev;
    NSData *data;
    NSMutableDictionary<NSString *, RedNaviMTLFunction *> *funcs;
    NSArray<NSString *> *names;
    NSString *label_;
}
- (instancetype)initWithDevice:(id<MTLDevice>)d data:(NSData *)blob error:(NSError **)error;
@end
@implementation RedNaviMTLLibrary
+ (void)load { class_addProtocol(self, @protocol(MTLLibrary)); }
- (instancetype)initWithDevice:(id<MTLDevice>)d data:(NSData *)blob error:(NSError **)error {
    if (error) *error = nil;                         // callers (CoreDisplay) leave it uninitialised and read it after a success
    if (!(self = [super init])) return nil;
    // MPSImage's library has 1837 functions (MPS asks for them by name and asserts when one is missing): the list lives on the heap and is big enough for any system library
    enum { RN_MAX_LIB_FUNCS = 8192 };
    NSMutableData *fnStore = [NSMutableData dataWithLength:sizeof(rn_mtl_func) * RN_MAX_LIB_FUNCS];
    rn_mtl_func *fn = (rn_mtl_func *)fnStore.mutableBytes;
    unsigned cnt = 0;
    // A fat file (QuartzCore's default.metallib: one slice of AIR and the rest GPU binaries for other targets) holds the AIR in the slice that parses
    if (blob.length > 8 && memcmp(blob.bytes, "\xca\xfe\xba\xbe", 4) == 0) {
        const uint8_t *b = blob.bytes;
        const unsigned nslices = (b[4] << 24) | (b[5] << 16) | (b[6] << 8) | b[7];
        for (unsigned i = 0; i < nslices && i < 32 && 8 + 20 * (i + 1) <= blob.length; i++) {
            const uint8_t *e = b + 8 + 20 * i;
            const unsigned long off = ((unsigned long)e[8] << 24) | (e[9] << 16) | (e[10] << 8) | e[11], sz = ((unsigned long)e[12] << 24) | (e[13] << 16) | (e[14] << 8) | e[15];
            if (off + sz > blob.length || sz < 8) continue;
            NSData *slice = [blob subdataWithRange:NSMakeRange(off, sz)];
            unsigned c2 = 0;
            if (rn_metallib_parse(slice.bytes, slice.length, fn, RN_MAX_LIB_FUNCS, &c2) == 0) { blob = slice; rn_log("library: fat file, using slice %u (%lu bytes)", i, sz); break; }
        }
    }
    int rc = rn_metallib_parse(blob.bytes, blob.length, fn, RN_MAX_LIB_FUNCS, &cnt);
    if (rc) {
        rn_log("newLibraryWithData: not a metallib this reader understands (code %d)", rc);
        if (error) *error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorUnsupported userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"unreadable metallib (%d)", rc]}];
        return nil;
    }
    dev = d; data = blob; funcs = [NSMutableDictionary new];
    NSMutableArray *nm = [NSMutableArray new];
    if (cnt > RN_MAX_LIB_FUNCS) { rn_log("library: %u functions, only the first %d are kept", cnt, RN_MAX_LIB_FUNCS); cnt = RN_MAX_LIB_FUNCS; }
    for (unsigned i = 0; i < cnt; i++) {
        RedNaviMTLFunction *f = [RedNaviMTLFunction new];
        f->name = @(fn[i].name);
        f->type = (MTLFunctionType)(fn[i].type + 1);       // the container has 0 vertex, 1 fragment, 2 kernel
        f->bitcode = [blob subdataWithRange:NSMakeRange(fn[i].bitcode_off, fn[i].bitcode_size)];
        f->dev = d;
        funcs[f->name] = f;
        [nm addObject:f->name];
    }
    names = nm;
    rn_log("library: %u functions", cnt);
    return self;
}
- (id<MTLDevice>)device { return dev; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
- (NSArray<NSString *> *)functionNames { return names; }
- (MTLLibraryType)type { return MTLLibraryTypeExecutable; }
- (NSString *)installName { return nil; }
- (id<MTLFunction>)newFunctionWithName:(NSString *)functionName {
    RedNaviMTLFunction *f = funcs[functionName];
    rn_log("function requested: %s%s", functionName.UTF8String, f ? "" : " (not in the library)");
    if (!f) return nil;
    RedNaviMTLFunction *c = [RedNaviMTLFunction new];
    c->name = f->name; c->type = f->type; c->bitcode = f->bitcode; c->dev = f->dev;
    return (id)c;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)functionName constantValues:(MTLFunctionConstantValues *)constantValues error:(NSError **)error {
    if (error) *error = nil;                         // CoreDisplay leaves it uninitialised and tests it after a success: a stale value crashed WindowServer
    RedNaviMTLFunction *c = (RedNaviMTLFunction *)[self newFunctionWithName:functionName];
    if (!c) { if (error) *error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorFunctionNotFound userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"Function %@ was not found in the library", functionName]}]; return nil; }
    NSString *why = nil;
    c->fcs = rn_capture_constants(constantValues, &why);
    if (getenv("RN_LOG_CONSTANTS")) for (NSDictionary *d in c->fcs) {
        const uint8_t *p = [d[@"data"] bytes]; NSUInteger n = [d[@"data"] length]; char hex[40] = ""; for (NSUInteger i = 0; i < n && i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", p[i]);
        rn_log("  constant %s = %s", d[@"index"] ? [[d[@"index"] description] UTF8String] : [d[@"name"] UTF8String], hex);
    }
    if (why) {
        rn_log("function constants of %s: %s", functionName.UTF8String, why.UTF8String);
        if (error) *error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorCompileFailure userInfo:@{NSLocalizedDescriptionKey: why}];
        return nil;
    }
    return (id)c;
}
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor error:(NSError **)error {
    if (error) *error = nil;
    if (descriptor.binaryArchives.count || descriptor.options) rn_log("newFunctionWithDescriptor: binary archives and options are ignored");
    if (descriptor.specializedName) rn_log("newFunctionWithDescriptor: specializedName is ignored");
    return [self newFunctionWithName:descriptor.name constantValues:descriptor.constantValues error:error];
}
- (void)newFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor completionHandler:(void (^)(id<MTLFunction>, NSError *))completionHandler {
    NSError *e = nil;
    id<MTLFunction> f = [self newFunctionWithDescriptor:descriptor error:&e];
    completionHandler(f, e);
}
- (void)newFunctionWithName:(NSString *)functionName constantValues:(MTLFunctionConstantValues *)constantValues completionHandler:(void (^)(id<MTLFunction>, NSError *))completionHandler {
    NSError *e = nil;
    id<MTLFunction> f = [self newFunctionWithName:functionName constantValues:constantValues error:&e];
    completionHandler(f, e);
}
@end

// A compute pipeline state from a function of a library: the function's bitcode goes to the translator (rn_air.h), the SPIR-V to RADV.
static id rn_pso_from_function(id<MTLDevice> dev, id fobj, NSError **error) {
    if (error) *error = nil;
    RedNaviMTLFunction *f = (RedNaviMTLFunction *)fobj;
    NSString *why = nil;
    if (![f isKindOfClass:[RedNaviMTLFunction class]]) why = @"the function is not from a RedNavi library";
    else if (f->type != MTLFunctionTypeKernel) why = @"only kernel functions are supported";
    rn_vk *vk = why ? NULL : rn_vk_shared();
    if (!why && !vk) why = @"RADV is not available";
    if (!why) {
        char err[300] = "";
        uint32_t *spv = NULL;
        size_t spvBytes = 0;
        rn_air_kernel_info info;
        memset(&info, 0, sizeof(info));
        rn_air_const fcc[64];
    const unsigned nfc = rn_fc_array(f->fcs, fcc, 64);
    if (rn_air_translate_stage_fc(NULL, RN_AIR_KERNEL, f->bitcode.bytes, f->bitcode.length, f->name.UTF8String, fcc, nfc, &info, &spv, &spvBytes, err, sizeof(err))) {
            why = [NSString stringWithUTF8String:err];
        } else {
            rn_vk_binding binds[RN_VK_MAX_BINDINGS];
            rn_vk_sampler_desc sds[RN_VK_MAX_BINDINGS];
            unsigned n = info.binding_count < RN_VK_MAX_BINDINGS ? info.binding_count : RN_VK_MAX_BINDINGS;
            for (unsigned i = 0; i < n; i++) {
                const rn_air_binding *ab = &info.bindings[i];
                binds[i] = (rn_vk_binding){ab->binding, ab->writable, ab->kind, NULL};
                if (ab->kind == RN_AIR_STATIC_SAMPLER) {
                    // a pixel-coordinate constexpr sampler: the translator divides the coordinate by the texture size or fetches the texels itself, so the VkSampler is a normalized one
                    sds[i] = (rn_vk_sampler_desc){ab->min_filter, ab->mag_filter, ab->mip_filter, {ab->address[0], ab->address[1], ab->address[2]}, 0, ab->border, ab->lod_min, ab->lod_max};
                    binds[i].sampler = &sds[i];
                }
            }
            rn_vk_compute *c = n ? rn_vk_compute_create(vk, spv, spvBytes, n, binds, info.dispatch_push_bytes, 2) : NULL;
            rn_air_free(spv, spvBytes);
            if (!c) why = n ? @"RADV refused the translated module" : @"the kernel uses no resources";
            else {
                RedNaviMTLComputePipelineState *p = [RedNaviMTLComputePipelineState new];
                p->c = c; p->nbind = n; p->push = 0; p->pushIndex = 0xffffffffu; p->spec = YES; p->dyn = YES; p->dev = dev;
                for (unsigned i = 0; i < n; i++) { p->metalIndex[i] = info.bindings[i].metal_index; p->kind[i] = info.bindings[i].kind; }
                for (int i = 0; i < 3; i++) p->localSize[i] = info.local_size[i];
                return p;
            }
        }
    }
    rn_log("compute pipeline for %s failed: %s", [f isKindOfClass:[RedNaviMTLFunction class]] ? f->name.UTF8String : "?", why.UTF8String);
    if (error) *error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorInternal userInfo:@{NSLocalizedDescriptionKey: why}];
    return nil;
}

// The pipeline reflection (MTLRenderPipelineReflection): its vertexArguments and fragmentArguments, one RNArgument (an MTLArgument and MTLBinding look-alike) per buffer, texture and
// sampler of the stage. AppleMetalOpenGLRenderer asks for it and binds only the arguments it lists.
@interface RNArgument : NSObject { @public NSDictionary *d; NSString *name_; }
@end
@implementation RNArgument
+ (void)initialize { if (self == [RNArgument class]) rn_add_default_resolver(self); }
- (NSString *)name { return name_; }
- (MTLArgumentType)type { return (MTLArgumentType)[d[@"type"] unsignedIntegerValue]; }
- (NSUInteger)index { return [d[@"index"] unsignedIntegerValue]; }
- (BOOL)isActive { return YES; }
- (BOOL)isUsed { return YES; }
- (BOOL)isArgument { return NO; }
- (MTLBindingAccess)access { return [d[@"writable"] boolValue] ? MTLBindingAccessReadWrite : MTLBindingAccessReadOnly; }
- (NSUInteger)bufferAlignment { return 16; }
- (NSUInteger)bufferDataSize { return [d[@"size"] unsignedIntegerValue]; }
- (MTLDataType)bufferDataType { return self.type == MTLArgumentTypeBuffer ? MTLDataTypeStruct : MTLDataTypeNone; }
- (MTLStructType *)bufferStructType { return nil; }
- (MTLPointerType *)bufferPointerType { return nil; }
- (MTLTextureType)textureType { return (MTLTextureType)[d[@"textureType"] unsignedIntegerValue]; }
- (MTLDataType)textureDataType { return self.type == MTLArgumentTypeTexture ? (MTLDataType)[d[@"dataType"] unsignedIntegerValue] : MTLDataTypeNone; }
- (BOOL)isDepthTexture { return [d[@"depth"] boolValue]; }
- (NSUInteger)arrayLength { return 1; }
- (NSUInteger)threadgroupMemoryAlignment { return 0; }
- (NSUInteger)threadgroupMemoryDataSize { return 0; }
@end
@interface RNRenderReflection : NSObject { @public NSArray *stage[2]; }
@end
@implementation RNRenderReflection
+ (void)initialize { if (self == [RNRenderReflection class]) rn_add_default_resolver(self); }
- (NSArray *)vertexArguments { return stage[0]; }
- (NSArray *)fragmentArguments { return stage[1]; }
- (NSArray *)vertexBindings { return stage[0]; }
- (NSArray *)fragmentBindings { return stage[1]; }
- (NSArray *)tileArguments { return @[]; }
- (NSArray *)tileBindings { return @[]; }
@end
static id rn_render_reflection(id pso) {
    if (![pso isKindOfClass:[RedNaviMTLRenderPipelineState class]]) return nil;
    RedNaviMTLRenderPipelineState *p = pso;
    RNRenderReflection *r = [RNRenderReflection new];
    for (int s = 0; s < 2; s++) {
        NSMutableArray *a = [NSMutableArray array];
        for (NSDictionary *d in p->args[s]) {
            RNArgument *x = [RNArgument new];
            x->d = d;
            const unsigned t = [d[@"type"] unsignedIntValue], idx = [d[@"index"] unsignedIntValue];
            if ([d[@"gl"] boolValue]) {
                // The names AppleMetalOpenGLRenderer's processReflection looks for (2026-11-21, read from its binary): a stage prefix (vs_, fs_), then tex_<n> and smp_<n> for the
                // sampler of GL texture slot n (the AIR index), uniform_buffer for the program's default uniform block (AIR buffer 0) and ubo_<k> for uniform block k, which the
                // GL code generator puts at AIR buffer 18 + k (seen with Kitty: block 0 ColorTable at 18, block 1 CellRenderData at 19). Other buffers keep a plain name.
                const char *pf = s ? "fs_" : "vs_";
                if (t == MTLArgumentTypeTexture) x->name_ = [NSString stringWithFormat:@"%stex_%u", pf, idx];
                else if (t == MTLArgumentTypeSampler) x->name_ = [NSString stringWithFormat:@"%ssmp_%u", pf, idx];
                else if (idx == 0) x->name_ = [NSString stringWithFormat:@"%suniform_buffer", pf];
                else if (idx >= 18) x->name_ = [NSString stringWithFormat:@"%subo_%u", pf, idx - 18];
            }
            if (!x->name_) x->name_ = [NSString stringWithFormat:@"%s%u", t == MTLArgumentTypeBuffer ? "buffer" : t == MTLArgumentTypeTexture ? "texture" : "sampler", idx];
            [a addObject:x];
        }
        r->stage[s] = a;
    }
    return r;
}

// A render pipeline state from a descriptor: both functions go to the translator (vertex module in set 0, fragment module in set 1) and the Vulkan pipeline for triangles is built.
static id rn_render_pso_from_descriptor_body(id<MTLDevice> dev, MTLRenderPipelineDescriptor *desc, NSError **error);
static id rn_render_pso_from_descriptor(id<MTLDevice> dev, MTLRenderPipelineDescriptor *desc, NSError **error) {
    const uint64_t ph5 = mach_absolute_time();
    id r = rn_render_pso_from_descriptor_body(dev, desc, error);
    rn_ph(5, ph5);
    return r;
}
// OpenGL (2026-11-21). AppleMetalOpenGLRenderer makes its functions with -newFunctionWithGLCoreIR:inputsDescription:functionType:, which the base class answers with an
// _MTLFunctionInternal whose bitcodeData (bitcodeType 1) is not LLVM bitcode but the request the GL front end left: a uint32 kind (1 vertex state, 2 fragment state, 3 a GLSL
// program, 4 an ARB program) and its data. Apple's own libGLVMPlugin turns it into an AIR module (oglCodeGenServiceGenerateIR, the entry MTLCompiler uses), Apple's LLVM writes it
// as bitcode, and the function goes on as one of our own: entry "#main", no function constants. Results are kept by request, since the renderer asks for the same functions again
// with every change of pipeline state.
// The LLVM C API of libLLVMContainer that reads the AIR metadata of a module (rn_gl_args).
static struct {
    unsigned (*nmd_count)(void *, const char *);
    void (*nmd_ops)(void *, const char *, void **);
    unsigned (*md_count)(void *);
    void (*md_ops)(void *, void **);
    const char *(*md_string)(void *, unsigned *);
    void *(*is_int)(void *);
    unsigned long long (*int_value)(void *);
} rn_llvm_md;
// The arguments of an OpenGL function as the AIR metadata lists them (the third operand of the air.vertex or air.fragment node): one dictionary per air.buffer, air.texture
// and air.sampler with @"type" (MTLArgumentType), @"index" (air.location_index), @"size" (air.buffer_size) and for textures @"textureType" and @"dataType", from air.arg_type_name.
// AppleMetalOpenGLRenderer binds only what the pipeline reflection lists (2026-11-21: without it no uniform buffer and no texture reached a draw).
static NSArray<NSDictionary *> *rn_gl_args(void *mod) {
    if (!rn_llvm_md.nmd_count || !rn_llvm_md.nmd_ops || !rn_llvm_md.md_count || !rn_llvm_md.md_ops || !rn_llvm_md.md_string || !rn_llvm_md.is_int || !rn_llvm_md.int_value) return nil;
    const char *stage = rn_llvm_md.nmd_count(mod, "air.vertex") ? "air.vertex" : rn_llvm_md.nmd_count(mod, "air.fragment") ? "air.fragment" : NULL;
    if (!stage) return nil;
    void *fnode = NULL;
    rn_llvm_md.nmd_ops(mod, stage, &fnode);
    if (!fnode || rn_llvm_md.md_count(fnode) < 3) return nil;
    void *fops[3] = {0};
    rn_llvm_md.md_ops(fnode, fops);
    if (!fops[2]) return nil;
    const unsigned na = rn_llvm_md.md_count(fops[2]);
    void **aops = calloc(na ? na : 1, sizeof(void *));
    rn_llvm_md.md_ops(fops[2], aops);
    NSMutableArray<NSDictionary *> *out = [NSMutableArray array];
    for (unsigned a = 0; a < na; a++) {
        const unsigned no = aops[a] ? rn_llvm_md.md_count(aops[a]) : 0;
        if (!no || no > 64) continue;
        void *ops[64] = {0};
        rn_llvm_md.md_ops(aops[a], ops);
        int type = -1;
        unsigned long long index = 0, size = 0;
        NSString *tname = nil;
        for (unsigned o = 0; o < no; o++) {
            unsigned len = 0;
            const char *str = ops[o] ? rn_llvm_md.md_string(ops[o], &len) : NULL;
            if (!str) continue;
            NSString *k = [[NSString alloc] initWithBytes:str length:len encoding:NSUTF8StringEncoding];
            void *next = o + 1 < no ? ops[o + 1] : NULL;
            const BOOL nextInt = next && rn_llvm_md.is_int(next);
            if ([k isEqualToString:@"air.buffer"]) type = MTLArgumentTypeBuffer;
            else if ([k isEqualToString:@"air.texture"]) type = MTLArgumentTypeTexture;
            else if ([k isEqualToString:@"air.sampler"]) type = MTLArgumentTypeSampler;
            else if ([k isEqualToString:@"air.location_index"] && nextInt) index = rn_llvm_md.int_value(next);
            else if ([k isEqualToString:@"air.buffer_size"] && nextInt) size = rn_llvm_md.int_value(next);
            else if ([k isEqualToString:@"air.arg_type_name"] && next) { unsigned l2 = 0; const char *t = rn_llvm_md.md_string(next, &l2); if (t) tname = [[NSString alloc] initWithBytes:t length:l2 encoding:NSUTF8StringEncoding]; }
        }
        if (type < 0) continue;
        NSMutableDictionary *d = [@{@"type": @(type), @"index": @(index), @"size": @(size), @"gl": @YES} mutableCopy];
        if (type == MTLArgumentTypeTexture) {
            MTLTextureType tt = MTLTextureType2D;
            if ([tname hasPrefix:@"texture2d_array"]) tt = MTLTextureType2DArray;
            else if ([tname hasPrefix:@"texture2d_ms"]) tt = MTLTextureType2DMultisample;
            else if ([tname hasPrefix:@"texture1d_array"]) tt = MTLTextureType1DArray;
            else if ([tname hasPrefix:@"texture1d"]) tt = MTLTextureType1D;
            else if ([tname hasPrefix:@"texture3d"]) tt = MTLTextureType3D;
            else if ([tname hasPrefix:@"texturecube_array"]) tt = MTLTextureTypeCubeArray;
            else if ([tname hasPrefix:@"texturecube"]) tt = MTLTextureTypeCube;
            else if ([tname hasPrefix:@"texture_buffer"]) tt = MTLTextureTypeTextureBuffer;
            else if ([tname hasPrefix:@"depth2d_array"]) { tt = MTLTextureType2DArray; d[@"depth"] = @YES; }
            else if ([tname hasPrefix:@"depthcube"]) { tt = MTLTextureTypeCube; d[@"depth"] = @YES; }
            else if ([tname hasPrefix:@"depth2d"]) { d[@"depth"] = @YES; }
            MTLDataType dt = MTLDataTypeFloat;
            if ([tname containsString:@"<uint"]) dt = MTLDataTypeUInt;
            else if ([tname containsString:@"<int"]) dt = MTLDataTypeInt;
            else if ([tname containsString:@"<half"]) dt = MTLDataTypeHalf;
            d[@"textureType"] = @(tt);
            d[@"dataType"] = @(dt);
        }
        [out addObject:d];
    }
    free(aops);
    return out;
}

static RedNaviMTLFunction *rn_gl_function(id f, id<MTLDevice> dev, NSString **why) {
    static void *(*ctx_create)(void), (*ctx_dispose)(void *), (*mod_dispose)(void *);
    static int (*to_fd)(void *, int, int, int);
    static void *(*gen)(void *, const void *, uint32_t);
    static dispatch_once_t once;
    static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    static NSMutableDictionary<NSData *, NSArray *> *cache;     // request -> @[bitcode, arguments]
    dispatch_once(&once, ^{
        void *gl = dlopen("/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGLVMPlugin.dylib", RTLD_NOW | RTLD_LOCAL);
        void *ll = dlopen("/System/Library/PrivateFrameworks/GPUCompiler.framework/Versions/A/Libraries/libLLVMContainer.dylib", RTLD_NOW | RTLD_LOCAL);
        if (gl && ll) {
            ctx_create = dlsym(ll, "LLVMContextCreate"); ctx_dispose = dlsym(ll, "LLVMContextDispose"); mod_dispose = dlsym(ll, "LLVMDisposeModule");
            to_fd = dlsym(ll, "LLVMWriteBitcodeToFD"); gen = dlsym(gl, "oglCodeGenServiceGenerateIR");
            rn_llvm_md.nmd_count = dlsym(ll, "LLVMGetNamedMetadataNumOperands"); rn_llvm_md.nmd_ops = dlsym(ll, "LLVMGetNamedMetadataOperands");
            rn_llvm_md.md_count = dlsym(ll, "LLVMGetMDNodeNumOperands"); rn_llvm_md.md_ops = dlsym(ll, "LLVMGetMDNodeOperands");
            rn_llvm_md.md_string = dlsym(ll, "LLVMGetMDString"); rn_llvm_md.is_int = dlsym(ll, "LLVMIsAConstantInt"); rn_llvm_md.int_value = dlsym(ll, "LLVMConstIntGetZExtValue");
        }
        cache = [NSMutableDictionary new];
    });
    if (![f respondsToSelector:@selector(bitcodeData)] || ![f respondsToSelector:@selector(bitcodeType)] ||
        ((NSUInteger (*)(id, SEL))objc_msgSend)(f, @selector(bitcodeType)) != 1) { *why = [NSString stringWithFormat:@"the function is not from a RedNavi library (%s)", class_getName(object_getClass(f))]; return nil; }
    if (!ctx_create || !ctx_dispose || !mod_dispose || !to_fd || !gen) { *why = @"OpenGL function: libGLVMPlugin or libLLVMContainer is not usable"; return nil; }
    id bd = [f performSelector:@selector(bitcodeData)];
    NSData *req = [bd isKindOfClass:[NSData class]] ? bd : nil;
    if (!req && bd) { const void *p0; size_t len; dispatch_data_t m = dispatch_data_create_map((dispatch_data_t)bd, &p0, &len); req = [NSData dataWithBytes:p0 length:len]; (void)m; }
    if (req.length < 4) { *why = @"OpenGL function without a request"; return nil; }
    os_unfair_lock_lock(&lock);
    NSArray *hit = cache[req];
    os_unfair_lock_unlock(&lock);
    NSData *bc = hit[0];
    NSArray<NSDictionary *> *args = hit.count > 1 ? hit[1] : nil;
    if (!bc) {
        char path[PATH_MAX];
        if (!confstr(_CS_DARWIN_USER_TEMP_DIR, path, sizeof(path))) strlcpy(path, "/tmp/", sizeof(path));
        strlcat(path, "rn_gl_XXXXXX", sizeof(path));
        const int fd = mkstemp(path);
        if (fd < 0) { *why = @"OpenGL function: no temporary file"; return nil; }
        unlink(path);
        void *ctx = ctx_create();
        void *mod = gen(ctx, req.bytes, (uint32_t)req.length);
        const int werr = mod ? to_fd(mod, fd, 0, 0) : -1;
        args = mod ? rn_gl_args(mod) : nil;
        if (mod) mod_dispose(mod);
        ctx_dispose(ctx);
        const off_t n = lseek(fd, 0, SEEK_END);
        NSMutableData *d = (!werr && n > 0) ? [NSMutableData dataWithLength:(NSUInteger)n] : nil;
        if (d && pread(fd, d.mutableBytes, (size_t)n, 0) != n) d = nil;
        close(fd);
        if (!d) { *why = [NSString stringWithFormat:@"OpenGL function: the GL code generator failed (request kind %u, %lu bytes)", *(const uint32_t *)req.bytes, (unsigned long)req.length]; return nil; }
        bc = d;
        os_unfair_lock_lock(&lock);
        cache[req] = args ? @[bc, args] : @[bc];
        os_unfair_lock_unlock(&lock);
        NSMutableString *al = [NSMutableString string];
        for (NSDictionary *a in args) [al appendFormat:@" %c%@%@", "b?ts"[[a[@"type"] intValue] & 3], a[@"index"], [a[@"size"] unsignedLongLongValue] ? [NSString stringWithFormat:@"(%@)", a[@"size"]] : @""];
        rn_log("OpenGL function: request kind %u, %lu bytes, AIR %lu bytes, arguments:%s", *(const uint32_t *)req.bytes, (unsigned long)req.length, (unsigned long)bc.length, args ? al.UTF8String : " (metadata not readable)");
        const char *dump = rn_diag ? getenv("RN_DUMP_AIR") : NULL;
        if (dump) {
            static int seq; [bc writeToFile:[NSString stringWithFormat:@"%s/gl_%02d_kind%u.air", dump, seq, *(const uint32_t *)req.bytes] atomically:NO];
            [req writeToFile:[NSString stringWithFormat:@"%s/gl_%02d_kind%u.req", dump, seq++, *(const uint32_t *)req.bytes] atomically:NO];
            Ivar fdv = class_getInstanceVariable(object_getClass(f), "_functionData");
            if (fdv) {                                       // MTLFunctionData: functionInputs at +112 and airScript at +152 (dispatch data)
                const char *base = (const char *)(__bridge void *)f + ivar_getOffset(fdv);
                const char *nm[2] = {"inputs", "airscript"}; const size_t off[2] = {112, 152};
                for (int k = 0; k < 2; k++) {
                    id o = (__bridge id)*(void * const *)(base + off[k]);
                    if (!o) { rn_log("GL function %s: nil", nm[k]); continue; }
                    const void *p0; size_t len; dispatch_data_t m = dispatch_data_create_map((dispatch_data_t)o, &p0, &len);
                    [[NSData dataWithBytes:p0 length:len] writeToFile:[NSString stringWithFormat:@"%s/gl_%02d_%s.bin", dump, seq - 1, nm[k]] atomically:NO];
                    rn_log("GL function %s: %lu bytes", nm[k], (unsigned long)len); (void)m;
                }
            }
            for (Class c = object_getClass(f); c && c != [NSObject class]; c = class_getSuperclass(c)) {
                unsigned ni = 0; Ivar *iv = class_copyIvarList(c, &ni);
                for (unsigned i = 0; i < ni; i++) {
                    const char *t = ivar_getTypeEncoding(iv[i]);
                    NSString *v = @"";
                    if (t && t[0] == '@') { id o = object_getIvar(f, iv[i]); v = o ? [[o description] substringToIndex:MIN((NSUInteger)300, [[o description] length])] : @"nil"; }
                    else if (t && (t[0] == 'Q' || t[0] == 'q' || t[0] == 'I' || t[0] == 'i')) v = [NSString stringWithFormat:@"%lld", (long long)*(int64_t *)((char *)(__bridge void *)f + ivar_getOffset(iv[i])) & (t[0] == 'I' || t[0] == 'i' ? 0xffffffffLL : -1LL)];
                    rn_log("GL function ivar %s.%s (%s): %s", class_getName(c), ivar_getName(iv[i]), t, v.UTF8String);
                }
                free(iv);
            }
        }
    }
    RedNaviMTLFunction *w = [RedNaviMTLFunction new];
    w->name = @"#main";
    w->type = [(id<MTLFunction>)f functionType];
    w->bitcode = bc;
    w->args = args;
    w->dev = dev;
    return w;
}
static id rn_render_pso_from_descriptor_body(id<MTLDevice> dev, MTLRenderPipelineDescriptor *desc, NSError **error) {
    if (error) *error = nil;
    NSString *why = nil;
    RedNaviMTLFunction *fn[2] = {(RedNaviMTLFunction *)desc.vertexFunction, (RedNaviMTLFunction *)desc.fragmentFunction};
    rn_vk *vk = rn_vk_shared();
    RedNaviMTLRenderPipelineState *p = [RedNaviMTLRenderPipelineState new];
    rn_log("render pipeline asked: vertex %s, fragment %s", fn[0] ? [fn[0].name UTF8String] : "-", fn[1] ? [fn[1].name UTF8String] : "-");
    if (!vk) why = @"RADV is not available";
    else if (!fn[0] || !fn[1]) why = @"a render pipeline needs a vertex and a fragment function";
    else for (int s = 0; s < 2 && !why; s++) {
        if (![fn[s] isKindOfClass:[RedNaviMTLFunction class]] && !(fn[s] = rn_gl_function(fn[s], dev, &why))) break;
        if (fn[s]->type != (s ? MTLFunctionTypeFragment : MTLFunctionTypeVertex)) { why = @"the function has the wrong type for its stage"; break; }
        char err[300] = "";
        uint32_t *spv = NULL;
        size_t spvBytes = 0;
        rn_air_kernel_info info;
        memset(&info, 0, sizeof(info));
        rn_air_const fcc[64];
        const unsigned nfc = rn_fc_array(fn[s]->fcs, fcc, 64);
        if (getenv("RN_DUMP_SPV")) {                         // debugging: the specialized LLVM module next to the SPIR-V (the translator writes it where RN_AIR_DEBUG_LL says)
            static unsigned llSeq;
            setenv("RN_AIR_DEBUG_LL", [NSString stringWithFormat:@"%s/%02u_%s_%s.ll", getenv("RN_DUMP_SPV"), llSeq++, s ? "frag" : "vert", fn[s]->name.UTF8String].UTF8String, 1);
        }
        if (rn_air_translate_stage_fc(NULL, s ? RN_AIR_FRAGMENT : RN_AIR_VERTEX, fn[s]->bitcode.bytes, fn[s]->bitcode.length, fn[s]->name.UTF8String, fcc, nfc, &info, &spv, &spvBytes, err, sizeof(err))) {
            why = [NSString stringWithFormat:@"%s function %s: %s", s ? "fragment" : "vertex", fn[s]->name.UTF8String, err];
            break;
        }
        p->names[s] = fn[s]->name;
        if (s && access("/Users/Shared/rednavi-finederiv", F_OK) == 0) {   // experiment: the derivatives of a fragment stage are asked for in the fine variant (the plain ones leave the choice to the GPU, which may give one value per 2x2 quad)
            uint32_t *w = (uint32_t *)spv; const size_t nw = spvBytes / 4;
            for (size_t i = 5; i < nw;) {
                const uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
                if (!wc || i + wc > nw) break;
                if (op >= 207 && op <= 209) w[i] = (w[i] & 0xffff0000u) | (op + 3);   // OpDPdx, OpDPdy, OpFwidth -> the Fine ones
                i += wc;
            }
        }
        p->spv[s] = [NSData dataWithBytes:spv length:spvBytes];
        if (getenv("RN_SPV_OVERRIDE_DIR") || getenv("RN_DUMP_SPV")) {              // debugging: a module is known by the FNV-1a hash of its SPIR-V; a file <hash>.spv in the override directory replaces it
            uint32_t h = 2166136261u; for (size_t b = 0; b < spvBytes; b++) h = (h ^ ((const uint8_t *)spv)[b]) * 16777619u;
            if (getenv("RN_DUMP_SPV")) rn_log("spv hash %08x: %s %s (%lu bytes)", h, s ? "fragment" : "vertex", fn[s]->name.UTF8String, (unsigned long)spvBytes);
            if (getenv("RN_SPV_OVERRIDE_DIR")) {
                NSData *o = [NSData dataWithContentsOfFile:[NSString stringWithFormat:@"%s/%08x.spv", getenv("RN_SPV_OVERRIDE_DIR"), h]];
                if (o) { p->spv[s] = o; rn_log("spv hash %08x replaced by the override file (%lu bytes)", h, (unsigned long)o.length); }
            }
        }
        if (getenv("RN_DUMP_SPV")) {                         // debugging: the SPIR-V of every translated stage, to read with spirv-dis
            static unsigned spvSeq;
            NSString *f = [NSString stringWithFormat:@"%s/%02u_%s_%s.spv", getenv("RN_DUMP_SPV"), spvSeq++, s ? "frag" : "vert", fn[s]->name.UTF8String];
            [p->spv[s] writeToFile:f atomically:NO];
        }
        rn_air_free(spv, spvBytes);
        unsigned n = info.binding_count < RN_VK_MAX_BINDINGS ? info.binding_count : RN_VK_MAX_BINDINGS;
        p->n[s] = n;
        {
            NSMutableString *vs = [NSMutableString stringWithFormat:@"fc%u", nfc];
            for (unsigned c = 0; c < nfc; c++) {
                if (fcc[c].index >= 0) [vs appendFormat:@" %d=", fcc[c].index]; else [vs appendFormat:@" %s=", fcc[c].name];
                for (uint32_t b = 0; b < fcc[c].size && b < 8; b++) [vs appendFormat:@"%02x", ((const uint8_t *)fcc[c].data)[b]];
            }
            unsigned nk[8] = {0};
            for (unsigned b = 0; b < n; b++) if (info.bindings[b].kind < 8) nk[info.bindings[b].kind]++;
            [vs appendFormat:@" | buf%u tex%u stor%u smp%u static%u null%u in%u", nk[0], nk[1], nk[2], nk[3], nk[4], nk[5], nk[6]];
            for (unsigned b = 0; b < n; b++) {   // the constexpr samplers of the shader: index, filters (min/mag/mip), address modes, unnormalized coordinates
                const rn_air_binding *sb = &info.bindings[b];
                if (sb->kind == RN_AIR_STATIC_SAMPLER) [vs appendFormat:@" ss%u=f%u%u%u,a%u%u%u,px%u", sb->metal_index, sb->min_filter, sb->mag_filter, sb->mip_filter, sb->address[0], sb->address[1], sb->address[2], sb->pixel_coords];
            }
            p->variant[s] = vs;
        }
        for (unsigned i = 0; i < n; i++) {
            const rn_air_binding *ab = &info.bindings[i];
            p->binds[s][i] = (rn_vk_binding){ab->binding, ab->writable, ab->kind == RN_AIR_NULL_TEXTURE ? RN_VK_SAMPLED_IMAGE : ab->kind, NULL};
            p->kind[s][i] = (ab->kind == RN_AIR_NULL_TEXTURE ? RN_AIR_NULL_TEXTURE : ab->kind) | (ab->dim << 8);   // the dimension of a texture rides in the upper bits (a placeholder needs it)
            p->mi[s][i] = ab->metal_index;
            if (ab->kind == RN_AIR_STATIC_SAMPLER) {
                p->sampl[s][i] = (rn_vk_sampler_desc){ab->min_filter, ab->mag_filter, ab->mip_filter, {ab->address[0], ab->address[1], ab->address[2]}, 0, ab->border, ab->lod_min, ab->lod_max};
                p->binds[s][i].sampler = &p->sampl[s][i];
            }
        }
        if (fn[s]->args) p->args[s] = fn[s]->args;
        else {                                               // a function of a Metal library: the reflection from the translator's bindings (no buffer sizes)
            static const MTLTextureType tt[8][2] = {{MTLTextureType1D, MTLTextureType1DArray}, {MTLTextureType2D, MTLTextureType2DArray}, {MTLTextureType3D, MTLTextureType3D},
                                                    {MTLTextureTypeCube, MTLTextureTypeCubeArray}, {MTLTextureTypeTextureBuffer, MTLTextureTypeTextureBuffer}};
            NSMutableArray<NSDictionary *> *a = [NSMutableArray array];
            for (unsigned i = 0; i < n; i++) {
                const rn_air_binding *ab = &info.bindings[i];
                if (ab->kind == RN_AIR_BUFFER) [a addObject:@{@"type": @(MTLArgumentTypeBuffer), @"index": @(ab->metal_index), @"size": @0, @"writable": @(ab->writable)}];
                else if (ab->kind == RN_AIR_SAMPLER) [a addObject:@{@"type": @(MTLArgumentTypeSampler), @"index": @(ab->metal_index)}];
                else if (ab->kind == RN_AIR_TEXTURE || ab->kind == RN_AIR_STORAGE_IMAGE || ab->kind == RN_AIR_NULL_TEXTURE)
                    [a addObject:@{@"type": @(MTLArgumentTypeTexture), @"index": @(ab->metal_index), @"textureType": @(tt[ab->dim & 7][(ab->dim >> 3) & 1]), @"dataType": @(MTLDataTypeFloat), @"writable": @(ab->kind == RN_AIR_STORAGE_IMAGE)}];
            }
            p->args[s] = a;
        }
    }
    if (!why) {
        for (NSUInteger i = 0; i < 8; i++) {
            const MTLPixelFormat pf = desc.colorAttachments[i].pixelFormat;
            if (pf == MTLPixelFormatInvalid) continue;
            const int f = rn_vk_format_of(pf);
            if (!f) { why = [NSString stringWithFormat:@"colour attachment %lu: pixel format %lu is not supported", (unsigned long)i, (unsigned long)pf]; break; }
            if (i != p->nt) { why = @"colour attachments must be contiguous from 0"; break; }
            MTLRenderPipelineColorAttachmentDescriptor *ca = desc.colorAttachments[i];
            rn_vk_blend *bl = &p->blend[p->nt];
            bl->write_mask = rn_vk_write_mask(ca.writeMask);
            if (ca.blendingEnabled) {
                bl->enabled = 1;
                if (!rn_vk_blend_factor(ca.sourceRGBBlendFactor, &bl->src_rgb) || !rn_vk_blend_factor(ca.destinationRGBBlendFactor, &bl->dst_rgb) ||
                    !rn_vk_blend_factor(ca.sourceAlphaBlendFactor, &bl->src_a) || !rn_vk_blend_factor(ca.destinationAlphaBlendFactor, &bl->dst_a) ||
                    !rn_vk_blend_op(ca.rgbBlendOperation, &bl->op_rgb) || !rn_vk_blend_op(ca.alphaBlendOperation, &bl->op_a)) { why = @"a blend factor or operation is not supported"; break; }
            } else {
                bl->src_rgb = bl->src_a = 1;                       // Vulkan wants valid values even when blending is off
            }
            p->tfmt[p->nt++] = f;
        }
        // The vertex descriptor of [[stage_in]] vertex functions: layouts[i] is vertex buffer index i, attribute n is the shader's [[attribute(n)]].
        MTLVertexDescriptor *vd = desc.vertexDescriptor;
        uint8_t intype[16] = {0};
        if (vd) rn_spv_input_types(p->spv[0], intype);
        for (NSUInteger i = 0; vd && !why && i < 16; i++) {
            MTLVertexAttributeDescriptor *a = vd.attributes[i];
            if (a.format == MTLVertexFormatInvalid) continue;
            const unsigned vf = intype[i] ? rn_vk_vertex_format_int(rn_vk_vertex_format(a.format)) : rn_vk_vertex_format(a.format);
            if (!vf) { why = [NSString stringWithFormat:@"vertex attribute %lu: format %lu is not supported", (unsigned long)i, (unsigned long)a.format]; break; }
            if (a.bufferIndex >= 16) { why = @"a vertex buffer index is 16 or more"; break; }
            p->vattr[p->nvattr++] = (rn_vk_vattr){(unsigned)i, (unsigned)a.bufferIndex, vf, (unsigned)a.offset};
        }
        for (NSUInteger i = 0; vd && !why && i < 16; i++) {
            MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
            BOOL used = NO;
            for (unsigned k = 0; k < p->nvattr; k++) if (p->vattr[k].binding == i) used = YES;
            if (!used) continue;
            // Constant: every vertex reads the first element (OpenGL's current value of an attribute array that is not enabled), which is a per-vertex binding with stride 0 in Vulkan.
            const BOOL constant = l.stepFunction == MTLVertexStepFunctionConstant;
            if (l.stepFunction != MTLVertexStepFunctionPerVertex && l.stepFunction != MTLVertexStepFunctionPerInstance && !constant) { why = @"this vertex step function is not supported"; break; }
            if (l.stepFunction == MTLVertexStepFunctionPerInstance && l.stepRate != 1) { why = @"a vertex step rate other than 1 is not supported"; break; }
            if (l.stride == 0 && !constant) { why = @"a vertex buffer layout with stride 0 is not supported"; break; }
            p->vlayout[p->nvlayout++] = (rn_vk_vlayout){(unsigned)i, constant ? 0u : (unsigned)l.stride, l.stepFunction == MTLVertexStepFunctionPerInstance};
        }
        p->dfmt = rn_depth_format_of(desc.depthAttachmentPixelFormat) ?: rn_depth_format_of(desc.stencilAttachmentPixelFormat);
        if (!p->nt && p->dfmt) {                                   // a pass with only depth: one colour target that nothing is written to (the pass supplies a scratch image of the depth's size)
            p->tfmt[0] = RN_VK_RGBA8; p->blend[0] = (rn_vk_blend){.src_rgb = 1, .src_a = 1, .write_mask = 0}; p->nt = 1;
        }
        if (!why && !p->nt) why = @"a render pipeline needs a colour attachment";
        if (!why && desc.rasterSampleCount > 1) why = @"multisampling is not supported";
        if (!why && ![p renderForPrimitive:3 cull:0 clockwise:YES]) why = @"RADV refused the pipeline";
    }
    if (why) {
        rn_log("render pipeline failed: %s", why.UTF8String);
        if (error) *error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorInternal userInfo:@{NSLocalizedDescriptionKey: why}];
        return nil;
    }
    p->dev = dev;
    rn_log("render pipeline %p made: vertex %lu bytes of SPIR-V (%u bindings), fragment %lu (%u bindings), %u vertex attributes", p, (unsigned long)p->spv[0].length, p->n[0], (unsigned long)p->spv[1].length, p->n[1], p->nvattr);
    return p;
}

// Test hook for tools/mtl_library_test.m: a library from data without a device.
id rn_test_library(NSData *blob, NSError **error) { return [[RedNaviMTLLibrary alloc] initWithDevice:nil data:blob error:error]; }

// ---------------------------------------------------------------------------------------------------------------------------------------- installation on the device class

typedef uintptr_t (*rn_imp_len_opt)(id, SEL, NSUInteger, NSUInteger);
typedef uintptr_t (*imp_fn_err1)(id, SEL, id);
typedef uintptr_t (*rn_imp_bytes)(id, SEL, const void *, NSUInteger, NSUInteger);

// The bytes a texture takes in a heap (heapTextureSizeAndAlignWithDescriptor: of the device and the heap's own count): texel size x texels of every level and slice, a third more
// for the mip chain, rounded up to 4 KB. The base class would answer from the tables of a vendor driver, which this device does not have.
static MTLSizeAndAlign rn_heap_texture_size(MTLTextureDescriptor *desc) {
    unsigned texel = rn_vk_format_texel_bytes(rn_vk_format_of(desc.pixelFormat));
    if (!texel) texel = 16;
    NSUInteger slices = MAX(desc.arrayLength, 1) * (desc.textureType == MTLTextureTypeCube || desc.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    NSUInteger bytes = (NSUInteger)texel * MAX(desc.width, 1) * MAX(desc.height, 1) * MAX(desc.depth, 1) * slices;
    if (desc.mipmapLevelCount > 1) bytes += bytes / 3;
    return (MTLSizeAndAlign){(bytes + 4095) & ~(NSUInteger)4095, 4096};
}

// Heaps (newHeapWithDescriptor:): no sub-allocation yet. A resource made from a heap is a resource of the device, allocated on its own; the heap counts the bytes of the resources
// made from it that are still alive (a lease object rides on each resource and gives its bytes back when the resource goes away), so that size, usedSize and the refusal of an
// allocation that does not fit behave as a heap's do. heapOffset stays 0, and placement heaps (resources at an offset the app chooses) are not supported.
@class RedNaviMTLHeap;
@interface RNHeapLease : NSObject { @public RedNaviMTLHeap *__weak heap; NSUInteger bytes; }
@end
@interface RedNaviMTLHeap : NSObject <MTLHeap> {
@public
    id<MTLDevice> dev_;
    MTLHeapDescriptor *desc_;
    _Atomic NSUInteger used_;
    NSString *label_;
}
- (instancetype)initWithDevice:(id<MTLDevice>)d descriptor:(MTLHeapDescriptor *)desc;
@end
@implementation RNHeapLease
- (void)dealloc { RedNaviMTLHeap *h = heap; if (h) atomic_fetch_sub(&h->used_, bytes); }
@end
static char rn_heap_lease_key;
@implementation RedNaviMTLHeap
+ (void)load { class_addProtocol(self, @protocol(MTLHeap)); }
- (instancetype)initWithDevice:(id<MTLDevice>)d descriptor:(MTLHeapDescriptor *)desc {
    if (!(self = [super init])) return nil;
    dev_ = d;
    desc_ = [desc copy];
    rn_log("heap: %lu bytes, storage %lu, type %ld%s", (unsigned long)desc.size, (unsigned long)desc.storageMode, (long)desc.type,
           desc.type == MTLHeapTypePlacement ? " (placement: resources at an offset are not supported)" : "");
    return self;
}
// Takes the bytes from the heap if they fit; NO when they do not.
- (BOOL)reserve:(NSUInteger)need {
    NSUInteger cur = atomic_load(&used_);
    do { if (cur + need > desc_.size) return NO; } while (!atomic_compare_exchange_weak(&used_, &cur, cur + need));
    return YES;
}
- (void)lease:(id)resource bytes:(NSUInteger)need {
    if (!resource) { atomic_fetch_sub(&used_, need); return; }
    RNHeapLease *l = [RNHeapLease new];
    l->heap = self; l->bytes = need;
    objc_setAssociatedObject(resource, &rn_heap_lease_key, l, OBJC_ASSOCIATION_RETAIN);
}
- (id<MTLDevice>)device { return dev_; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)label { label_ = [label copy]; }
- (MTLStorageMode)storageMode { return desc_.storageMode; }
- (MTLCPUCacheMode)cpuCacheMode { return desc_.cpuCacheMode; }
- (MTLHazardTrackingMode)hazardTrackingMode { return desc_.hazardTrackingMode; }
- (MTLResourceOptions)resourceOptions { return desc_.resourceOptions; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return MTLPurgeableStateNonVolatile; }
- (MTLHeapType)type { return desc_.type; }
- (NSUInteger)size { return desc_.size; }
- (NSUInteger)usedSize { return atomic_load(&used_); }
- (NSUInteger)currentAllocatedSize { return desc_.size; }
- (NSUInteger)allocatedSize { return desc_.size; }
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment {
    NSUInteger a = alignment > 1 ? alignment : 1;
    NSUInteger start = (atomic_load(&used_) + a - 1) / a * a;
    return start < desc_.size ? desc_.size - start : 0;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    NSUInteger need = (length + 255) & ~(NSUInteger)255;
    if (![self reserve:need]) return nil;
    id<MTLBuffer> b = [dev_ newBufferWithLength:length options:options];
    [self lease:b bytes:need];
    return b;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)desc {
    NSUInteger need = rn_heap_texture_size(desc).size;
    if (![self reserve:need]) return nil;
    id<MTLTexture> t = [dev_ newTextureWithDescriptor:desc];
    [self lease:t bytes:need];
    return t;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset {
    rn_log("heap: a buffer at offset %lu of a placement heap is not supported", (unsigned long)offset);
    return nil;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)desc offset:(NSUInteger)offset {
    rn_log("heap: a texture at offset %lu of a placement heap is not supported", (unsigned long)offset);
    return nil;
}
@end

void rn_compute_install(Class k, Class sup) {
    {
        const char *classes[] = {"RedNaviMTLBuffer", "RedNaviMTLTexture", "RedNaviMTLSamplerState", "RedNaviMTLComputePipelineState", "RedNaviMTLRenderPipelineState", "RedNaviMTLComputeCommandEncoder",
                                 "RedNaviMTLRenderCommandEncoder", "RedNaviMTLBlitCommandEncoder", "RedNaviMTLCommandBuffer", "RedNaviMTLCommandQueue", "RedNaviMTLFunction", "RedNaviMTLLibrary",
                                 "RedNaviMTLDepthStencilState", "RedNaviMTLSharedEvent", "RedNaviMTLHeap"};
        for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) { Class c = objc_getClass(classes[i]); if (c) rn_add_default_resolver(c); }
    }
    // Command queues: every newCommandQueue* method of the superclass chain (the base class would call them in a loop, see M6_ACCELERATOR_STUDY.md section 10).
    for (Class c = sup; c && c != [NSObject class]; c = class_getSuperclass(c)) {
        unsigned n = 0;
        Method *list = class_copyMethodList(c, &n);
        for (unsigned i = 0; i < n; i++) {
            SEL sel = method_getName(list[i]);
            if (strncmp(sel_getName(sel), "newCommandQueue", 15) != 0) continue;
            const char *enc = method_getTypeEncoding(list[i]);
            // one block for the three shapes: all take at most one argument in a register that is ignored
            IMP imp = imp_implementationWithBlock(^uintptr_t(id self) {
                if (!rn_gate()) { rn_log("-%s: RN_METAL_COMPUTE is not set, answering nil", sel_getName(sel)); return 0; }
                return (uintptr_t)CFBridgingRetain([[RedNaviMTLCommandQueue alloc] initWithDevice:(id<MTLDevice>)self]);
            });
            if (class_addMethod(k, sel, imp, enc)) rn_log("installed -%s (%s) of %s", sel_getName(sel), enc ? enc : "?", class_getName(c));
        }
        free(list);
    }
    // The device's name. The base class answers "Apple" (for name and vendorName), and AppleMetalOpenGLRenderer (GLDDeviceRec::initWithDisplayMask) only accepts a device whose
    // name contains AMD or ATI, NVIDIA or Intel: anything else is "Unknown GPU!!" and an assertion, so no OpenGL context (2026-11-21). The RADV device name contains AMD.
    // WindowServer keeps the base class's answers for now: whether QuartzCore picks vendor-specific paths by name is not known, and the compositor works as it is.
    {
        extern const char *getprogname(void);
        if (strcmp(getprogname(), "WindowServer") != 0) {
            SEL sn = sel_registerName("name"), sv = sel_registerName("vendorName");
            Method mn = class_getInstanceMethod(sup, sn), mv = class_getInstanceMethod(sup, sv);
            IMP on = mn ? method_getImplementation(mn) : NULL, ov = mv ? method_getImplementation(mv) : NULL;
            class_addMethod(k, sn, imp_implementationWithBlock(^id(id self) {
                rn_vk *vk = rn_gate() ? rn_vk_shared() : NULL;
                return vk ? [NSString stringWithUTF8String:rn_vk_device_name(vk)] : (on ? ((id(*)(id, SEL))on)(self, sn) : nil);
            }), "@16@0:8");
            class_addMethod(k, sv, imp_implementationWithBlock(^id(id self) {
                return rn_gate() && rn_vk_shared() ? @"AMD" : (ov ? ((id(*)(id, SEL))ov)(self, sv) : nil);
            }), "@16@0:8");
            // Queries the GL renderer makes while it creates its device. The generic stub of plugin.m cannot answer a struct (MTLSize comes back through a hidden pointer, which the
            // stub took for self and crashed on). One sample per pixel only: the render encoder has no multisampling.
            class_addMethod(k, sel_registerName("maxThreadsPerThreadgroup"), imp_implementationWithBlock(^MTLSize(id self) { return MTLSizeMake(1024, 1024, 1024); }), "{?=QQQ}16@0:8");
            class_addMethod(k, sel_registerName("supportsSampleCount:"), imp_implementationWithBlock(^BOOL(id self, NSUInteger n) { return n == 1; }), "c24@0:8Q16");
            class_addMethod(k, sel_registerName("gpuCoreCount"), imp_implementationWithBlock(^NSUInteger(id self) { return 60; }), "Q16@0:8");   // compute units of the RX 7800 XT
        }
    }
    // Buffers
    SEL s1 = sel_registerName("newBufferWithLength:options:");
    Method m1 = class_getInstanceMethod(sup, s1);
    rn_imp_len_opt o1 = m1 ? (rn_imp_len_opt)method_getImplementation(m1) : NULL;
    class_addMethod(k, s1, imp_implementationWithBlock(^uintptr_t(id self, NSUInteger length, NSUInteger options) {
        if (!rn_gate()) return o1 ? o1(self, s1, length, options) : 0;
        return (uintptr_t)CFBridgingRetain([[RedNaviMTLBuffer alloc] initWithDevice:(id<MTLDevice>)self length:length options:options]);
    }), m1 ? method_getTypeEncoding(m1) : "@32@0:8Q16Q24");
    SEL s2 = sel_registerName("newBufferWithBytes:length:options:");
    Method m2 = class_getInstanceMethod(sup, s2);
    rn_imp_bytes o2 = m2 ? (rn_imp_bytes)method_getImplementation(m2) : NULL;
    class_addMethod(k, s2, imp_implementationWithBlock(^uintptr_t(id self, const void *bytes, NSUInteger length, NSUInteger options) {
        if (!rn_gate()) return o2 ? o2(self, s2, bytes, length, options) : 0;
        RedNaviMTLBuffer *b = [[RedNaviMTLBuffer alloc] initWithDevice:(id<MTLDevice>)self length:length options:options];
        if (b && bytes) memcpy(rn_vk_buffer_contents(b->b), bytes, length);
        return (uintptr_t)CFBridgingRetain(b);
    }), m2 ? method_getTypeEncoding(m2) : "@40@0:8r^v16Q24Q32");
    SEL s2b = sel_registerName("newBufferWithBytesNoCopy:length:options:deallocator:");
    Method m2b = class_getInstanceMethod(sup, s2b);
    typedef uintptr_t (*rn_imp_nocopy)(id, SEL, void *, NSUInteger, NSUInteger, id);
    rn_imp_nocopy o2b = m2b ? (rn_imp_nocopy)method_getImplementation(m2b) : NULL;
    class_addMethod(k, s2b, imp_implementationWithBlock(^uintptr_t(id self, void *bytes, NSUInteger length, NSUInteger options, void (^dealloc)(void *, NSUInteger)) {
        if (!rn_gate()) return o2b ? o2b(self, s2b, bytes, length, options, dealloc) : 0;
        RedNaviMTLBuffer *b = [[RedNaviMTLBuffer alloc] initWithDevice:(id<MTLDevice>)self length:length options:options];
        if (!b) return 0;
        b->userPtr = bytes; b->userDealloc = [dealloc copy];
        memcpy(rn_vk_buffer_contents(b->b), bytes, length);
        @synchronized([RedNaviMTLBuffer class]) { [[RedNaviMTLBuffer noCopyBuffers] addObject:b]; }
        rn_log("buffer over caller memory: %lu bytes at %p", (unsigned long)length, bytes);
        return (uintptr_t)CFBridgingRetain(b);
    }), m2b ? method_getTypeEncoding(m2b) : "@48@0:8^v16Q24Q32@?40");
    // Textures and sampler states
    SEL s7 = sel_registerName("newTextureWithDescriptor:");
    Method m7 = class_getInstanceMethod(sup, s7);
    imp_fn_err1 o7 = m7 ? (imp_fn_err1)method_getImplementation(m7) : NULL;
    class_addMethod(k, s7, imp_implementationWithBlock(^uintptr_t(id self, MTLTextureDescriptor *desc) {
        if (!rn_gate()) return o7 ? o7(self, s7, desc) : 0;
        return (uintptr_t)CFBridgingRetain([[RedNaviMTLTexture alloc] initWithDevice:(id<MTLDevice>)self descriptor:desc]);
    }), m7 ? method_getTypeEncoding(m7) : "@24@0:8@16");
    // Heaps (see RedNaviMTLHeap)
    SEL sh = sel_registerName("newHeapWithDescriptor:");
    Method mh = class_getInstanceMethod(sup, sh);
    typedef uintptr_t (*imp_heap)(id, SEL, id);
    imp_heap oh = mh ? (imp_heap)method_getImplementation(mh) : NULL;
    class_addMethod(k, sh, imp_implementationWithBlock(^uintptr_t(id self, MTLHeapDescriptor *desc) {
        if (!rn_gate()) return oh ? oh(self, sh, desc) : 0;
        return (uintptr_t)CFBridgingRetain([[RedNaviMTLHeap alloc] initWithDevice:(id<MTLDevice>)self descriptor:desc]);
    }), mh ? method_getTypeEncoding(mh) : "@24@0:8@16");
    // Events (the first one of this process's plug-in creates a shared memory object per event)
    SEL se1 = sel_registerName("newSharedEvent"), se2 = sel_registerName("newEvent"), se3 = sel_registerName("newSharedEventWithHandle:");
    Method me1 = class_getInstanceMethod(sup, se1), me2 = class_getInstanceMethod(sup, se2), me3 = class_getInstanceMethod(sup, se3);
    typedef uintptr_t (*imp_noarg)(id, SEL);
    typedef uintptr_t (*imp_onearg)(id, SEL, id);
    imp_noarg oe1 = me1 ? (imp_noarg)method_getImplementation(me1) : NULL, oe2 = me2 ? (imp_noarg)method_getImplementation(me2) : NULL;
    imp_onearg oe3 = me3 ? (imp_onearg)method_getImplementation(me3) : NULL;
    class_addMethod(k, se1, imp_implementationWithBlock(^uintptr_t(id self) {
        if (!rn_gate()) return oe1 ? oe1(self, se1) : 0;
        return (uintptr_t)CFBridgingRetain([[RedNaviMTLSharedEvent alloc] initWithDevice:(id<MTLDevice>)self shmName:nil create:YES]);
    }), me1 ? method_getTypeEncoding(me1) : "@16@0:8");
    class_addMethod(k, se2, imp_implementationWithBlock(^uintptr_t(id self) {
        if (!rn_gate()) return oe2 ? oe2(self, se2) : 0;
        return (uintptr_t)CFBridgingRetain([[RedNaviMTLSharedEvent alloc] initWithDevice:(id<MTLDevice>)self shmName:nil create:YES]);
    }), me2 ? method_getTypeEncoding(me2) : "@16@0:8");
    class_addMethod(k, se3, imp_implementationWithBlock(^uintptr_t(id self, id handle) {
        if (!rn_gate() || ![handle isKindOfClass:[RedNaviSharedEventHandle class]]) return oe3 ? oe3(self, se3, handle) : 0;
        return (uintptr_t)CFBridgingRetain([[RedNaviMTLSharedEvent alloc] initWithDevice:(id<MTLDevice>)self shmName:((RedNaviSharedEventHandle *)handle)->shmName create:NO]);
    }), me3 ? method_getTypeEncoding(me3) : "@24@0:8@16");
    SEL s7b = sel_registerName("newTextureWithDescriptor:iosurface:plane:");
    Method m7b = class_getInstanceMethod(sup, s7b);
    typedef uintptr_t (*imp_tex_io)(id, SEL, id, IOSurfaceRef, NSUInteger);
    imp_tex_io o7b = m7b ? (imp_tex_io)method_getImplementation(m7b) : NULL;
    class_addMethod(k, s7b, imp_implementationWithBlock(^uintptr_t(id self, MTLTextureDescriptor *desc, IOSurfaceRef io, NSUInteger plane) {
        if (!rn_gate()) return o7b ? o7b(self, s7b, desc, io, plane) : 0;
        id t = [[RedNaviMTLTexture alloc] initWithDevice:(id<MTLDevice>)self descriptor:desc iosurface:io plane:plane];
        // WindowServer builds the destination of the display compositor from a texture over the display's IOSurface (WSCompositeDestinationCreateWithMetalTexture); the large ones are logged.
        static int n;
        if (io && IOSurfaceGetWidth(io) >= 1024 && n < 400) {
            n++;
            rn_log("iosurface texture #%d: surface %u %zux%zu fmt 0x%x bpr %zu plane %lu, desc fmt %lu usage 0x%lx storage %lu type %lu -> %s", n, (unsigned)IOSurfaceGetID(io), IOSurfaceGetWidth(io), IOSurfaceGetHeight(io),
                   (unsigned)IOSurfaceGetPixelFormat(io), IOSurfaceGetBytesPerRow(io), (unsigned long)plane, (unsigned long)desc.pixelFormat, (unsigned long)desc.usage, (unsigned long)desc.storageMode, (unsigned long)desc.textureType, t ? "texture" : "nil");
        }
        return (uintptr_t)CFBridgingRetain(t);
    }), m7b ? method_getTypeEncoding(m7b) : "@40@0:8@16^{__IOSurface=}24Q32");
    // Textures over the caller's memory (private Metal SPI that CoreAnimation tries for client storage images; Apple's answer on this device was nil, so the images were never drawn)
    SEL s9a = sel_registerName("newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:");
    Method m9a = class_getInstanceMethod(sup, s9a);
    typedef uintptr_t (*imp_tile_a)(id, SEL, void *, NSUInteger, id, NSUInteger, NSUInteger);
    imp_tile_a o9a = m9a ? (imp_tile_a)method_getImplementation(m9a) : NULL;
    class_addMethod(k, s9a, imp_implementationWithBlock(^uintptr_t(id self, void *bytes, NSUInteger len, MTLTextureDescriptor *desc, NSUInteger off, NSUInteger bpr) {
        if (!rn_gate()) return o9a ? o9a(self, s9a, bytes, len, desc, off, bpr) : 0;
        return (uintptr_t)CFBridgingRetain(rn_texture_over_bytes((id<MTLDevice>)self, desc, bytes, len, off, bpr, nil));
    }), m9a ? method_getTypeEncoding(m9a) : "@56@0:8^v16Q24@32Q40Q48");
    SEL s9b = sel_registerName("newTiledTextureWithBytesNoCopy:length:deallocator:descriptor:offset:bytesPerRow:");
    Method m9b = class_getInstanceMethod(sup, s9b);
    typedef uintptr_t (*imp_tile_b)(id, SEL, void *, NSUInteger, id, id, NSUInteger, NSUInteger);
    imp_tile_b o9b = m9b ? (imp_tile_b)method_getImplementation(m9b) : NULL;
    class_addMethod(k, s9b, imp_implementationWithBlock(^uintptr_t(id self, void *bytes, NSUInteger len, void (^dealloc)(void *, NSUInteger), MTLTextureDescriptor *desc, NSUInteger off, NSUInteger bpr) {
        if (!rn_gate()) return o9b ? o9b(self, s9b, bytes, len, dealloc, desc, off, bpr) : 0;
        return (uintptr_t)CFBridgingRetain(rn_texture_over_bytes((id<MTLDevice>)self, desc, bytes, len, off, bpr, dealloc));
    }), m9b ? method_getTypeEncoding(m9b) : "@64@0:8^v16Q24@?32@40Q48Q56");
    SEL s8 = sel_registerName("newSamplerStateWithDescriptor:");
    Method m8 = class_getInstanceMethod(sup, s8);
    imp_fn_err1 o8 = m8 ? (imp_fn_err1)method_getImplementation(m8) : NULL;
    class_addMethod(k, s8, imp_implementationWithBlock(^uintptr_t(id self, MTLSamplerDescriptor *desc) {
        if (!rn_gate()) return o8 ? o8(self, s8, desc) : 0;
        return (uintptr_t)CFBridgingRetain(rn_new_sampler_state((id<MTLDevice>)self, desc));
    }), m8 ? method_getTypeEncoding(m8) : "@24@0:8@16");
    // Render pipeline states. Outside the gate everything goes to the superclass.
    typedef uintptr_t (*imp_desc_err)(id, SEL, id, NSError **);
    typedef uintptr_t (*imp_desc_opt_refl_err)(id, SEL, id, NSUInteger, void *, NSError **);
    typedef void (*imp_desc_handler)(id, SEL, id, id);
    typedef void (*imp_desc_opt_handler)(id, SEL, id, NSUInteger, id);
    SEL r1 = sel_registerName("newRenderPipelineStateWithDescriptor:error:");
    Method rm1 = class_getInstanceMethod(sup, r1);
    imp_desc_err ro1 = rm1 ? (imp_desc_err)method_getImplementation(rm1) : NULL;
    class_addMethod(k, r1, imp_implementationWithBlock(^uintptr_t(id self, id desc, NSError **error) {
        if (!rn_gate()) return ro1 ? ro1(self, r1, desc, error) : 0;
        return (uintptr_t)CFBridgingRetain(rn_render_pso_from_descriptor((id<MTLDevice>)self, desc, error));
    }), rm1 ? method_getTypeEncoding(rm1) : "@32@0:8@16^@24");
    SEL r2 = sel_registerName("newRenderPipelineStateWithDescriptor:options:reflection:error:");
    Method rm2 = class_getInstanceMethod(sup, r2);
    imp_desc_opt_refl_err ro2 = rm2 ? (imp_desc_opt_refl_err)method_getImplementation(rm2) : NULL;
    class_addMethod(k, r2, imp_implementationWithBlock(^uintptr_t(id self, id desc, NSUInteger options, void *reflection, NSError **error) {
        if (!rn_gate()) return ro2 ? ro2(self, r2, desc, options, reflection, error) : 0;
        id p = rn_render_pso_from_descriptor((id<MTLDevice>)self, desc, error);
        if (reflection) *(id __autoreleasing *)reflection = p ? rn_render_reflection(p) : nil;
        if (rn_diag && reflection) rn_log("render pipeline %p: reflection asked, options 0x%lx", (__bridge void *)p, (unsigned long)options);
        return (uintptr_t)CFBridgingRetain(p);
    }), rm2 ? method_getTypeEncoding(rm2) : "@48@0:8@16Q24^@32^@40");
    SEL r3 = sel_registerName("newRenderPipelineStateWithDescriptor:completionHandler:");
    Method rm3 = class_getInstanceMethod(sup, r3);
    imp_desc_handler ro3 = rm3 ? (imp_desc_handler)method_getImplementation(rm3) : NULL;
    class_addMethod(k, r3, imp_implementationWithBlock(^void(id self, id desc, void (^handler)(id, NSError *)) {
        if (!rn_gate()) { if (ro3) ro3(self, r3, desc, handler); return; }
        NSError *error = nil;
        id p = rn_render_pso_from_descriptor((id<MTLDevice>)self, desc, &error);
        handler(p, error);
    }), rm3 ? method_getTypeEncoding(rm3) : "v32@0:8@16@?24");
    SEL r4 = sel_registerName("newRenderPipelineStateWithDescriptor:options:completionHandler:");
    Method rm4 = class_getInstanceMethod(sup, r4);
    imp_desc_opt_handler ro4 = rm4 ? (imp_desc_opt_handler)method_getImplementation(rm4) : NULL;
    class_addMethod(k, r4, imp_implementationWithBlock(^void(id self, id desc, NSUInteger options, void (^handler)(id, id, NSError *)) {
        if (!rn_gate()) { if (ro4) ro4(self, r4, desc, options, (id)handler); return; }
        NSError *error = nil;
        id p = rn_render_pso_from_descriptor((id<MTLDevice>)self, desc, &error);
        handler(p, p ? rn_render_reflection(p) : nil, error);
    }), rm4 ? method_getTypeEncoding(rm4) : "v40@0:8@16Q24@?32");
    // The private shader entry
    class_addMethod(k, sel_registerName("rn_newComputePipelineWithSPIRV:bufferCount:pushBytes:"), imp_implementationWithBlock(^id(id self, NSData *spirv, NSUInteger nbuf, NSUInteger push) {
        rn_vk *vk = rn_gate() ? rn_vk_shared() : NULL;
        if (!vk) return nil;
        if (nbuf == 0 || nbuf > 16) return nil;
        rn_vk_binding binds[16];
        for (unsigned i = 0; i < nbuf; i++) binds[i] = (rn_vk_binding){(uint32_t)i, 1, RN_VK_BUFFER, NULL};
        rn_vk_compute *c = rn_vk_compute_create(vk, spirv.bytes, spirv.length, (unsigned)nbuf, binds, (unsigned)push, 0);
        if (!c) { rn_log("compute pipeline from SPIR-V failed"); return nil; }
        RedNaviMTLComputePipelineState *p = [RedNaviMTLComputePipelineState new];
        p->c = c; p->nbind = (unsigned)nbuf; p->push = (unsigned)push; p->pushIndex = (unsigned)nbuf; p->spec = NO; p->dev = (id<MTLDevice>)self;
        for (unsigned i = 0; i < nbuf; i++) { p->metalIndex[i] = i; p->kind[i] = RN_VK_BUFFER; }
        return p;
    }), "@40@0:8@16Q24Q32");

    // Libraries and compute pipeline states built from them. Outside the gate everything goes to the superclass.
    typedef uintptr_t (*imp_data_err)(id, SEL, id, NSError **);
    typedef uintptr_t (*imp_fn_err)(id, SEL, id, NSError **);
    typedef uintptr_t (*imp_fn_opt_refl_err)(id, SEL, id, NSUInteger, void *, NSError **);
    typedef uintptr_t (*imp_fn_handler)(id, SEL, id, id);
    SEL s3 = sel_registerName("newLibraryWithData:error:");
    Method m3 = class_getInstanceMethod(sup, s3);
    imp_data_err o3 = m3 ? (imp_data_err)method_getImplementation(m3) : NULL;
    class_addMethod(k, s3, imp_implementationWithBlock(^uintptr_t(id self, id data, NSError **error) {
        if (!rn_gate()) return o3 ? o3(self, s3, data, error) : 0;
        const void *buf = NULL;
        size_t size = 0;
        dispatch_data_t map = dispatch_data_create_map((dispatch_data_t)data, &buf, &size);
        NSData *blob = [NSData dataWithBytes:buf length:size];
        (void)map;
        return (uintptr_t)CFBridgingRetain([[RedNaviMTLLibrary alloc] initWithDevice:(id<MTLDevice>)self data:blob error:error]);
    }), m3 ? method_getTypeEncoding(m3) : "@32@0:8@16^@24");
    {
        SEL sds = sel_registerName("newDepthStencilStateWithDescriptor:");
        Method mds = class_getInstanceMethod(sup, sds);
        typedef uintptr_t (*imp_ds)(id, SEL, id);
        imp_ds ods = mds ? (imp_ds)method_getImplementation(mds) : NULL;
        class_addMethod(k, sds, imp_implementationWithBlock(^uintptr_t(id self, id desc) {
            if (!rn_gate()) return ods ? ods(self, sds, desc) : 0;
            RedNaviMTLDepthStencilState *st = [RedNaviMTLDepthStencilState new];
            st->dev = (id<MTLDevice>)self;
            MTLDepthStencilDescriptor *dd = (MTLDepthStencilDescriptor *)desc;
            if (dd) {
                st->ds.depth_test = 1;                                 // Metal always tests; Always passes
                st->ds.depth_write = dd.depthWriteEnabled; st->ds.depth_cmp = (unsigned)dd.depthCompareFunction;
                MTLStencilDescriptor *sf[2] = {dd.frontFaceStencil, dd.backFaceStencil};
                rn_vk_stencil_face *df[2] = {&st->ds.front, &st->ds.back};
                for (int f = 0; f < 2; f++) {
                    if (!sf[f]) { *df[f] = (rn_vk_stencil_face){0, 0, 0, 7, 0xffffffffu, 0xffffffffu, 0}; continue; }
                    *df[f] = (rn_vk_stencil_face){(unsigned)sf[f].stencilFailureOperation, (unsigned)sf[f].depthStencilPassOperation, (unsigned)sf[f].depthFailureOperation, (unsigned)sf[f].stencilCompareFunction, sf[f].readMask, sf[f].writeMask, 0};
                    if (sf[f].stencilCompareFunction != MTLCompareFunctionAlways || sf[f].stencilFailureOperation != MTLStencilOperationKeep || sf[f].depthFailureOperation != MTLStencilOperationKeep || sf[f].depthStencilPassOperation != MTLStencilOperationKeep) st->ds.stencil_test = 1;
                }
            }
            return (uintptr_t)CFBridgingRetain(st);
        }), mds ? method_getTypeEncoding(mds) : "@24@0:8@16");
    }
    // Stitched libraries (CoreImage compiles a whole filter graph into one compute pipeline with them; iconservicesagent and mediaanalysisd crashed on the first one): the base class reads
    // the private data of the functions it is given, which a function of this plug-in does not have, and dereferenced a null script. The plug-in cannot stitch AIR functions yet, so the
    // call fails with an error, which CoreImage handles.
    {
        SEL ss = sel_registerName("newLibraryWithStitchedDescriptor:error:"), sa = sel_registerName("newLibraryWithStitchedDescriptor:completionHandler:");
        Method ms = class_getInstanceMethod(sup, ss), ma = class_getInstanceMethod(sup, sa);
        typedef uintptr_t (*imp_stitch)(id, SEL, id, NSError **);
        typedef void (*imp_stitch_async)(id, SEL, id, id);
        imp_stitch os = ms ? (imp_stitch)method_getImplementation(ms) : NULL;
        imp_stitch_async oa = ma ? (imp_stitch_async)method_getImplementation(ma) : NULL;
        static NSError *(^unsupported)(void) = ^NSError *{
            return [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorUnsupported userInfo:@{NSLocalizedDescriptionKey: @"stitched libraries are not supported by the RedNavi Metal plug-in"}];
        };
        class_addMethod(k, ss, imp_implementationWithBlock(^uintptr_t(id self, id desc, NSError **error) {
            if (!rn_gate()) return os ? os(self, ss, desc, error) : 0;
            rn_log("newLibraryWithStitchedDescriptor: not supported, answering an error");
            if (error) *error = unsupported();
            return 0;
        }), ms ? method_getTypeEncoding(ms) : "@32@0:8@16^@24");
        class_addMethod(k, sa, imp_implementationWithBlock(^(id self, id desc, id handler) {
            if (!rn_gate()) { if (oa) oa(self, sa, desc, handler); return; }
            rn_log("newLibraryWithStitchedDescriptor (asynchronous): not supported, answering an error");
            NSError *e = unsupported();
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ ((void (^)(id, NSError *))handler)(nil, e); });
        }), ma ? method_getTypeEncoding(ma) : "v32@0:8@16@?24");
    }
    // Capabilities. The base class answers these from tables its own driver fills in, which a device made by this plug-in does not have (calling -supportsFamily: ended in a call through
    // a null pointer inside QuartzCore's Metal context setup), so they are answered here: what a discrete RDNA 3 card on macOS supports as far as the plug-in and the translator go (nothing here is read from the card; checked on the 7800 XT only). The Apple
    // GPU families, tile shaders, memoryless targets, raster order groups, SIMD-group operations and YCbCr formats are NO; native FP16 is NO so that the float variants of shaders are asked for.
    {
        SEL sfam = sel_registerName("supportsFamily:"), sfs = sel_registerName("supportsFeatureSet:");
        Method mfam = class_getInstanceMethod(sup, sfam), mfs = class_getInstanceMethod(sup, sfs);
        typedef BOOL (*imp_bool_int)(id, SEL, NSInteger);
        imp_bool_int ofam = mfam ? (imp_bool_int)method_getImplementation(mfam) : NULL, ofs = mfs ? (imp_bool_int)method_getImplementation(mfs) : NULL;
        class_addMethod(k, sfam, imp_implementationWithBlock(^BOOL(id self, NSInteger family) {
            if (!rn_gate()) return ofam ? ofam(self, sfam, family) : NO;
            // MTLGPUFamilyMac1 2001, Mac2 2002, Common1..3 3001..3003, Metal3 5001
            const BOOL yes = family == 2001 || family == 2002 || (family >= 3001 && family <= 3003) || family == 5001;
            rn_log("supportsFamily: %ld -> %d", (long)family, yes);
            return yes;
        }), mfam ? method_getTypeEncoding(mfam) : "B24@0:8q16");
        class_addMethod(k, sfs, imp_implementationWithBlock(^BOOL(id self, NSInteger set) {
            if (!rn_gate()) return ofs ? ofs(self, sfs, set) : NO;
            const BOOL yes = set >= 10000 && set <= 10005 && set != 10002;     // as the real Intel GPU answers: macOS GPUFamily1 v1, v2, v4, GPUFamily2 v1, v2 (10002 is not one of them)
            rn_log("supportsFeatureSet: %ld -> %d", (long)set, yes);
            return yes;
        }), mfs ? method_getTypeEncoding(mfs) : "B24@0:8Q16");
        static const struct { const char *name; BOOL value; } caps[] = {
            {"supportsBufferlessClientStorageTexture", YES}, {"isHeadless", NO},   /* WindowServer composes the display with Metal only if some device is not headless (WSSystemCanCompositeWithMetal asks isHeadless of every device); this one drives a display */ {"supportsExtendedXR10Formats", NO}, {"supportsMemorylessRenderTargets", NO}, {"supportsNativeHardwareFP16", NO},
            {"supportsRasterOrderGroups", NO}, {"supportsSIMDGroup", NO}, {"supportsSIMDReduction", NO}, {"supportsSIMDShuffleAndFill", NO}, {"supportsTileShaders", NO},
            {"supportsUnalignedVertexFetch", YES},   /* Vulkan has no stride or offset alignment rule of 4 bytes for vertex buffers (RADV fetches 1-byte strides); with NO the OpenGL renderer repacks the attributes per vertex and splits instanced draws into one draw per instance with instance_id 0 (Kitty's cells, 2026-11-21) */
            {"isFramebufferReadSupported", YES}, {"supportsProgrammableBlending", YES}, {"supportsYCBCRFormats12", NO}, {"supportsYCBCRPackedFormats12", NO}, {"supportsExpectedWakeupBeforeCommitDeadline", NO},
            // what a real device answers and RADV on Navi 32 can do too (tools/mtl_limits.m lists them): QuartzCore chooses data formats and shader variants from these
            {"supports32BitFloatFiltering", YES}, {"supportsFloat16InfNanFiltering", YES}, {"supportsTextureSwizzle", YES}, {"supportsQueryTextureLOD", YES}, {"supportsInt64", YES},
            {"supportsBaseVertexInstanceDrawing", YES}, {"supportsFragmentBufferWrites", YES}, {"supportsRenderTextureWrites", YES}, {"supportsExtendedVertexFormats", YES},
            {"supportsInvariantVertexPosition", YES}, {"supportsNonUniformThreadgroupSize", YES}, {"supportsShaderMinLODClamp", YES}, {"supportsSamplerReductionMode", YES},
            {"supportsSamplerCompareFunction", YES}, {"supportsMirrorClampToEdgeSamplerMode", YES}, {"supportsBlackOrWhiteSamplerBorderColors", YES}, {"supportsFastMathInfNaNPropagation", YES},
            {"supportsFixedLinePointFillDepthGradient", YES}, {"supportsRelaxedTextureViewRequirements", YES}, {"supportsTextureOutOfBoundsReads", YES}, {"supportsLinearTextureFromSharedBuffer", YES},
            {"supportsPullModelInterpolation", YES}, {"supportsSharedTextureHandles", YES}, {"supportsDynamicAttributeStride", YES}, {"supportsCountingOcclusionQuery", YES}};
        typedef BOOL (*imp_bool)(id, SEL);
        for (unsigned i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
            SEL sc = sel_registerName(caps[i].name);
            Method mc = class_getInstanceMethod(sup, sc);
            imp_bool oc = mc ? (imp_bool)method_getImplementation(mc) : NULL;
            const BOOL v = caps[i].value;
            const char *nm = caps[i].name;
            class_addMethod(k, sc, imp_implementationWithBlock(^BOOL(id self) {
                if (!rn_gate()) return oc ? oc(self, sc) : NO;
                rn_log("%s -> %d", nm, v);
                return v;
            }), mc ? method_getTypeEncoding(mc) : "B16@0:8");
        }
    }
    // Limits: the base class reads them from tables the vendor driver fills in, so a device made by this plug-in answers 0 (a 128 x 128 texture descriptor failed validation as "greater than
    // the maximum allowed size of 0" inside QuartzCore's context setup). The values are fixed, not read from the card: what is known of a macOS AMD device and of RADV on the 7800 XT (texture sizes and binding counts, alignments of its memory);
    // they are not checked on any other card (RADV's own limits on a Navi 33 are to be compared, docs/NAVI33_LADDER.md). The type of each answer is the type the base class method returns.
    {
        static const struct { const char *name; double value; } limits[] = {
            {"maxTextureWidth1D", 16384}, {"maxTextureWidth2D", 16384}, {"maxTextureHeight2D", 16384}, {"maxTextureWidth3D", 2048}, {"maxTextureHeight3D", 2048}, {"maxTextureDepth3D", 2048},
            {"maxTextureDimensionCube", 16384}, {"maxTextureLayers", 2048}, {"maxTextureBufferWidth", 134217728}, {"maxColorAttachments", 8}, {"maxVertexBuffers", 31}, {"maxFragmentBuffers", 31},
            {"maxComputeBuffers", 31}, {"maxTileBuffers", 31}, {"maxFragmentTextures", 128}, {"maxComputeTextures", 128}, {"maxTileTextures", 128}, {"maxFragmentSamplers", 16},
            {"maxComputeSamplers", 16}, {"maxTileSamplers", 16}, {"maxVertexAttributes", 31}, {"maxComputeAttributes", 31}, {"maxInterpolants", 32}, {"maxInterpolatedComponents", 124},
            {"maxTotalComputeThreadsPerThreadgroup", 1024}, {"maxComputeThreadgroupMemory", 32768}, {"maxConstantBufferArguments", 31}, {"maxFunctionConstantIndices", 65536},
            {"maxLineWidth", 1}, {"maxPointSize", 511}, {"maxBufferLength", 2147483648.0}, {"maxFramebufferStorageBits", 256}, {"maxVertexInlineDataSize", 4096}, {"maxFragmentInlineDataSize", 4096},
            {"maxComputeInlineDataSize", 4096}, {"maxTileInlineDataSize", 4096}, {"maxCustomSamplePositions", 16}, {"maxTessellationFactor", 64},
            {"linearTextureAlignmentBytes", 256}, {"deviceLinearTextureAlignmentBytes", 256}, {"deviceLinearReadOnlyTextureAlignmentBytes", 256}, {"linearTextureArrayAlignmentBytes", 0},
            {"linearTextureArrayAlignmentSlice", 0}, {"iosurfaceTextureAlignmentBytes", 16}, {"iosurfaceReadOnlyTextureAlignmentBytes", 16},
            // the same ones on a real device (tools/mtl_limits.m): QuartzCore lays its vertex/constant arena out with the constant buffer alignment
            {"minConstantBufferAlignmentBytes", 32}, {"minBufferNoCopyAlignmentBytes", 4096}, {"maxComputeThreadgroupMemoryAlignmentBytes", 16}};
        for (unsigned i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
            SEL sl = sel_registerName(limits[i].name);
            Method ml = class_getInstanceMethod(sup, sl);
            if (!ml) continue;
            char rt[8]; method_getReturnType(ml, rt, sizeof(rt));
            typedef uintptr_t (*imp_u)(id, SEL);
            imp_u ol = (imp_u)method_getImplementation(ml);
            const double v = limits[i].value;
            const char *enc = method_getTypeEncoding(ml);
            switch (rt[0]) {
            case 'Q': case 'q': case 'L': case 'l':
                class_addMethod(k, sl, imp_implementationWithBlock(^uint64_t(id self) { return rn_gate() ? (uint64_t)v : (uint64_t)ol(self, sl); }), enc); break;
            case 'I': case 'i':
                class_addMethod(k, sl, imp_implementationWithBlock(^uint32_t(id self) { return rn_gate() ? (uint32_t)v : (uint32_t)ol(self, sl); }), enc); break;
            case 'f':
                class_addMethod(k, sl, imp_implementationWithBlock(^float(id self) { return (float)v; }), enc); break;
            case 'd':
                class_addMethod(k, sl, imp_implementationWithBlock(^double(id self) { return v; }), enc); break;
            default: rn_log("limit %s: return type %c not handled", limits[i].name, rt[0]); break;
            }
        }
        SEL shb = sel_registerName("heapBufferSizeAndAlignWithLength:options:");
        Method mhb = class_getInstanceMethod(sup, shb);
        typedef MTLSizeAndAlign (*imp_hb)(id, SEL, NSUInteger, NSUInteger);
        imp_hb ohb = mhb ? (imp_hb)method_getImplementation(mhb) : NULL;
        class_addMethod(k, shb, imp_implementationWithBlock(^MTLSizeAndAlign(id self, NSUInteger len, NSUInteger opt) {
            if (!rn_gate() && ohb) return ohb(self, shb, len, opt);
            return (MTLSizeAndAlign){len, 32};                      // as a real device answers
        }), mhb ? method_getTypeEncoding(mhb) : "{?=QQ}32@0:8Q16Q24");
        SEL sht = sel_registerName("heapTextureSizeAndAlignWithDescriptor:");
        Method mht = class_getInstanceMethod(sup, sht);
        typedef MTLSizeAndAlign (*imp_ht)(id, SEL, id);
        imp_ht oht = mht ? (imp_ht)method_getImplementation(mht) : NULL;
        class_addMethod(k, sht, imp_implementationWithBlock(^MTLSizeAndAlign(id self, MTLTextureDescriptor *desc) {
            if (!rn_gate() && oht) return oht(self, sht, desc);
            return rn_heap_texture_size(desc);
        }), mht ? method_getTypeEncoding(mht) : "{?=QQ}24@0:8@16");
        SEL smin = sel_registerName("minimumLinearTextureAlignmentForPixelFormat:");
        Method mmin = class_getInstanceMethod(sup, smin);
        typedef NSUInteger (*imp_align)(id, SEL, NSUInteger);
        imp_align omin = mmin ? (imp_align)method_getImplementation(mmin) : NULL;
        class_addMethod(k, smin, imp_implementationWithBlock(^NSUInteger(id self, NSUInteger pf) { return rn_gate() ? 256 : (omin ? omin(self, smin, pf) : 0); }), mmin ? method_getTypeEncoding(mmin) : "Q24@0:8Q16");
    }
    // Libraries from files: the URL or path of a metallib, the default library of the main bundle or another one (default.metallib in its resources). Outside the gate: the superclass.
    {
        typedef uintptr_t (*imp_url_err)(id, SEL, id, NSError **);
        typedef uintptr_t (*imp_noerr)(id, SEL);
        static uintptr_t (^load_file)(id, NSString *, NSError **) = ^uintptr_t(id self, NSString *path, NSError **error) {
            if (error) *error = nil;
            NSData *blob = path ? [NSData dataWithContentsOfFile:path] : nil;
            if (!blob) {
                rn_log("library from %s: cannot read the file", path.UTF8String);
                if (error) *error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorUnsupported userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"cannot read %@", path]}];
                return 0;
            }
            rn_log("library from %s (%lu bytes)", path.UTF8String, (unsigned long)blob.length);
            return (uintptr_t)CFBridgingRetain([[RedNaviMTLLibrary alloc] initWithDevice:(id<MTLDevice>)self data:blob error:error]);
        };
        SEL su = sel_registerName("newLibraryWithURL:error:"), sf = sel_registerName("newLibraryWithFile:error:"), sd = sel_registerName("newDefaultLibrary"), sb = sel_registerName("newDefaultLibraryWithBundle:error:");
        Method mu = class_getInstanceMethod(sup, su), mf = class_getInstanceMethod(sup, sf), md = class_getInstanceMethod(sup, sd), mb = class_getInstanceMethod(sup, sb);
        imp_url_err ou = mu ? (imp_url_err)method_getImplementation(mu) : NULL, of = mf ? (imp_url_err)method_getImplementation(mf) : NULL, ob = mb ? (imp_url_err)method_getImplementation(mb) : NULL;
        imp_noerr od = md ? (imp_noerr)method_getImplementation(md) : NULL;
        class_addMethod(k, su, imp_implementationWithBlock(^uintptr_t(id self, NSURL *url, NSError **error) {
            if (!rn_gate()) return ou ? ou(self, su, url, error) : 0;
            return load_file(self, url.path, error);
        }), mu ? method_getTypeEncoding(mu) : "@32@0:8@16^@24");
        class_addMethod(k, sf, imp_implementationWithBlock(^uintptr_t(id self, NSString *path, NSError **error) {
            if (!rn_gate()) return of ? of(self, sf, path, error) : 0;
            return load_file(self, path, error);
        }), mf ? method_getTypeEncoding(mf) : "@32@0:8@16^@24");
        class_addMethod(k, sb, imp_implementationWithBlock(^uintptr_t(id self, NSBundle *bundle, NSError **error) {
            if (!rn_gate()) return ob ? ob(self, sb, bundle, error) : 0;
            return load_file(self, [bundle pathForResource:@"default" ofType:@"metallib"], error);
        }), mb ? method_getTypeEncoding(mb) : "@32@0:8@16^@24");
        class_addMethod(k, sd, imp_implementationWithBlock(^uintptr_t(id self) {
            if (!rn_gate()) return od ? od(self, sd) : 0;
            NSError *e = nil;
            return load_file(self, [[NSBundle mainBundle] pathForResource:@"default" ofType:@"metallib"], &e);
        }), md ? method_getTypeEncoding(md) : "@16@0:8");
    }
    SEL s4 = sel_registerName("newComputePipelineStateWithFunction:error:");
    Method m4 = class_getInstanceMethod(sup, s4);
    imp_fn_err o4 = m4 ? (imp_fn_err)method_getImplementation(m4) : NULL;
    class_addMethod(k, s4, imp_implementationWithBlock(^uintptr_t(id self, id fn, NSError **error) {
        if (!rn_gate()) return o4 ? o4(self, s4, fn, error) : 0;
        return (uintptr_t)CFBridgingRetain(rn_pso_from_function((id<MTLDevice>)self, fn, error));
    }), m4 ? method_getTypeEncoding(m4) : "@32@0:8@16^@24");
    SEL s5 = sel_registerName("newComputePipelineStateWithFunction:options:reflection:error:");
    Method m5 = class_getInstanceMethod(sup, s5);
    imp_fn_opt_refl_err o5 = m5 ? (imp_fn_opt_refl_err)method_getImplementation(m5) : NULL;
    class_addMethod(k, s5, imp_implementationWithBlock(^uintptr_t(id self, id fn, NSUInteger options, void *reflection, NSError **error) {
        if (!rn_gate()) return o5 ? o5(self, s5, fn, options, reflection, error) : 0;
        return (uintptr_t)CFBridgingRetain(rn_pso_from_function((id<MTLDevice>)self, fn, error));
    }), m5 ? method_getTypeEncoding(m5) : "@48@0:8@16Q24^@32^@40");
    SEL s6 = sel_registerName("newComputePipelineStateWithFunction:completionHandler:");
    Method m6 = class_getInstanceMethod(sup, s6);
    imp_fn_handler o6 = m6 ? (imp_fn_handler)method_getImplementation(m6) : NULL;
    class_addMethod(k, s6, imp_implementationWithBlock(^void(id self, id fn, void (^handler)(id, NSError *)) {
        if (!rn_gate()) { if (o6) o6(self, s6, fn, handler); return; }
        NSError *error = nil;
        id p = rn_pso_from_function((id<MTLDevice>)self, fn, &error);
        handler(p, error);
    }), m6 ? method_getTypeEncoding(m6) : "v32@0:8@16@?24");
    // MPS (WindowServer's blur) and others build compute pipelines from a descriptor, or with options and a handler
    typedef void (*imp_fn_opt_handler)(id, SEL, id, NSUInteger, id);
    SEL cs7 = sel_registerName("newComputePipelineStateWithFunction:options:completionHandler:");
    Method cm7 = class_getInstanceMethod(sup, cs7);
    imp_fn_opt_handler o7p = cm7 ? (imp_fn_opt_handler)method_getImplementation(cm7) : NULL;
    class_addMethod(k, cs7, imp_implementationWithBlock(^void(id self, id fn, NSUInteger options, void (^handler)(id, id, NSError *)) {
        if (!rn_gate()) { if (o7p) o7p(self, cs7, fn, options, (id)handler); return; }
        NSError *error = nil;
        id p = rn_pso_from_function((id<MTLDevice>)self, fn, &error);
        handler(p, nil, error);
    }), cm7 ? method_getTypeEncoding(cm7) : "v40@0:8@16Q24@?32");
    SEL cs8 = sel_registerName("newComputePipelineStateWithDescriptor:options:reflection:error:");
    Method cm8 = class_getInstanceMethod(sup, cs8);
    imp_fn_opt_refl_err o8p = cm8 ? (imp_fn_opt_refl_err)method_getImplementation(cm8) : NULL;
    class_addMethod(k, cs8, imp_implementationWithBlock(^uintptr_t(id self, MTLComputePipelineDescriptor *desc, NSUInteger options, void *reflection, NSError **error) {
        if (!rn_gate()) return o8p ? o8p(self, cs8, (id)desc, options, reflection, error) : 0;
        return (uintptr_t)CFBridgingRetain(rn_pso_from_function((id<MTLDevice>)self, desc.computeFunction, error));
    }), cm8 ? method_getTypeEncoding(cm8) : "@48@0:8@16Q24^@32^@40");
    SEL cs9 = sel_registerName("newComputePipelineStateWithDescriptor:options:completionHandler:");
    Method cm9 = class_getInstanceMethod(sup, cs9);
    imp_fn_opt_handler o9p = cm9 ? (imp_fn_opt_handler)method_getImplementation(cm9) : NULL;
    class_addMethod(k, cs9, imp_implementationWithBlock(^void(id self, MTLComputePipelineDescriptor *desc, NSUInteger options, void (^handler)(id, id, NSError *)) {
        if (!rn_gate()) { if (o9p) o9p(self, cs9, (id)desc, options, (id)handler); return; }
        NSError *error = nil;
        id p = rn_pso_from_function((id<MTLDevice>)self, desc.computeFunction, &error);
        handler(p, nil, error);
    }), cm9 ? method_getTypeEncoding(cm9) : "v40@0:8@16Q24@?32");
}
