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
#import <Foundation/Foundation.h>
#pragma clang diagnostic ignored "-Wunguarded-availability-new"   // protocol methods newer than the deployment target are answered when Metal asks
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <IOSurface/IOSurfaceRef.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdlib.h>
#include "rn_vk.h"
#include "rn_air.h"
#include "metallib.h"

extern void rn_log(const char *fmt, ...);

static BOOL rn_gate(void) { return getenv("RN_METAL_COMPUTE") != NULL; }

static rn_vk *rn_vk_shared(void) {
    static rn_vk *vk;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        char err[200] = "";
        vk = rn_vk_open(NULL, err, sizeof(err));
        if (vk) rn_log("RADV opened: %s", rn_vk_device_name(vk));
        else rn_log("RADV not available: %s", err);
    });
    return vk;
}

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
        dev = d; len = l; options = o;
        memset(rn_vk_buffer_contents(b), 0, l ? l : 1);
    }
    return self;
}
- (void)dealloc { if (b) rn_vk_buffer_destroy(rn_vk_shared(), b); }
- (void *)contents { return rn_vk_buffer_contents(b); }
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

@interface RedNaviMTLTexture : NSObject {
@public
    rn_vk_image *img;
    id<MTLDevice> dev;
    MTLPixelFormat pf;
    NSUInteger w, h;
    unsigned texel;
    MTLTextureUsage usage_;
    NSString *label_;
    IOSurfaceRef surf;                               // set for a texture made from an IOSurface: copied in and out at the borders of a command buffer
    RedNaviMTLTexture *parent;                       // a texture view: the texture it is a view of (it shares its image, memory and host copy)
    MTLTextureSwizzleChannels swz;
}
- (instancetype)initWithDevice:(id<MTLDevice>)d descriptor:(MTLTextureDescriptor *)desc;
- (instancetype)initWithDevice:(id<MTLDevice>)d descriptor:(MTLTextureDescriptor *)desc iosurface:(IOSurfaceRef)io plane:(NSUInteger)plane;
- (void)syncFromSurface;
- (void)syncToSurface;
- (RedNaviMTLTexture *)viewWithPixelFormat:(MTLPixelFormat)pf swizzle:(const MTLTextureSwizzleChannels *)sw;
@end
@implementation RedNaviMTLTexture
+ (void)load { class_addProtocol(self, @protocol(MTLTexture)); class_addProtocol(self, @protocol(MTLResource)); }
- (instancetype)initWithDevice:(id<MTLDevice>)d descriptor:(MTLTextureDescriptor *)desc {
    if (!(self = [super init])) return nil;
    if (desc.textureType != MTLTextureType2D || desc.arrayLength > 1 || desc.sampleCount > 1 || desc.depth > 1) {
        rn_log("texture: only plain 2D textures are supported (type %ld, array %lu, samples %lu)", (long)desc.textureType, (unsigned long)desc.arrayLength, (unsigned long)desc.sampleCount);
        return nil;
    }
    if (desc.mipmapLevelCount > 1) rn_log("texture: %lu mipmap levels asked, only level 0 exists", (unsigned long)desc.mipmapLevelCount);
    const int f = rn_vk_format_of(desc.pixelFormat);
    rn_vk *vk = f ? rn_vk_shared() : NULL;
    img = vk ? rn_vk_image_create(vk, f, (uint32_t)desc.width, (uint32_t)desc.height) : NULL;
    if (!img) { rn_log("texture %lux%lu pixel format %lu: not supported", (unsigned long)desc.width, (unsigned long)desc.height, (unsigned long)desc.pixelFormat); return nil; }
    dev = d; pf = desc.pixelFormat; w = desc.width; h = desc.height; usage_ = desc.usage; texel = rn_vk_format_texel_bytes(f);
    return self;
}
- (void)dealloc { if (img) rn_vk_image_destroy(rn_vk_shared(), img); if (surf) CFRelease(surf); }
// IOSurface-backed textures (no host memory import in the kext, so a copy): the surface's rows are copied into the host copy of the image before the command buffer runs and, for
// what the command buffer wrote, back afterwards. The surface's row pitch is its own; the host copy is packed.
- (instancetype)initWithDevice:(id<MTLDevice>)d descriptor:(MTLTextureDescriptor *)desc iosurface:(IOSurfaceRef)io plane:(NSUInteger)plane {
    if (!io || plane != 0) { rn_log("texture from an IOSurface: %s", io ? "only plane 0 is supported" : "no surface"); return nil; }
    if (IOSurfaceGetPlaneCount(io) > 1) { rn_log("texture from an IOSurface: multi-planar surfaces are not supported"); return nil; }
    if ((self = [self initWithDevice:d descriptor:desc])) {
        if (w > IOSurfaceGetWidth(io) || h > IOSurfaceGetHeight(io) || texel != IOSurfaceGetBytesPerElement(io)) {
            rn_log("texture from an IOSurface: the surface is %zux%zu with %zu bytes per element, the texture %lux%lu with %u", IOSurfaceGetWidth(io), IOSurfaceGetHeight(io), IOSurfaceGetBytesPerElement(io), (unsigned long)w, (unsigned long)h, texel);
            return nil;
        }
        surf = (IOSurfaceRef)CFRetain(io);
        [self syncFromSurface];
    }
    return self;
}
- (void)syncFromSurface {
    if (!surf || IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) return;
    const uint8_t *src = IOSurfaceGetBaseAddress(surf);
    const size_t pitch = IOSurfaceGetBytesPerRow(surf);
    uint8_t *dst = rn_vk_image_staging(img);
    for (NSUInteger y = 0; y < h; y++) memcpy(dst + y * w * texel, src + y * pitch, w * texel);
    IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
    rn_vk_image_host_wrote(img);
}
- (void)syncToSurface {
    if (!surf || IOSurfaceLock(surf, 0, NULL) != kIOReturnSuccess) return;
    uint8_t *dst = IOSurfaceGetBaseAddress(surf);
    const size_t pitch = IOSurfaceGetBytesPerRow(surf);
    const uint8_t *src = rn_vk_image_staging(img);
    for (NSUInteger y = 0; y < h; y++) memcpy(dst + y * pitch, src + y * w * texel, w * texel);
    IOSurfaceUnlock(surf, 0, NULL);
}
- (id<MTLDevice>)device { return dev; }
- (NSString *)label { return label_; }
- (void)setLabel:(NSString *)l { label_ = [l copy]; }
- (MTLResourceOptions)resourceOptions { return 0; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeUntracked; }
- (NSUInteger)allocatedSize { return w * h * texel; }
- (id<MTLHeap>)heap { return nil; }
- (NSUInteger)heapOffset { return 0; }
- (BOOL)isAliasable { return NO; }
- (void)makeAliasable {}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return MTLPurgeableStateNonVolatile; }
- (id<MTLResource>)rootResource { return nil; }
- (id<MTLTexture>)parentTexture { return (id)parent; }
- (NSUInteger)parentRelativeLevel { return 0; }
- (NSUInteger)parentRelativeSlice { return 0; }
- (id<MTLBuffer>)buffer { return nil; }
- (NSUInteger)bufferOffset { return 0; }
- (NSUInteger)bufferBytesPerRow { return 0; }
- (IOSurfaceRef)iosurface { return surf; }
- (NSUInteger)iosurfacePlane { return 0; }
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
    t->img = v; t->dev = dev; t->pf = newPf; t->w = w; t->h = h; t->texel = texel; t->usage_ = usage_;
    t->parent = parent ? parent : self;
    t->surf = t->parent->surf ? (IOSurfaceRef)CFRetain(t->parent->surf) : NULL;
    t->swz = sw ? *sw : (MTLTextureSwizzleChannels){MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha};
    return t;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)newPf { return (id)[self viewWithPixelFormat:newPf swizzle:NULL]; }
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)newPf textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices {
    if (type != MTLTextureType2D || levels.location != 0 || levels.length != 1 || slices.location != 0 || slices.length != 1) { rn_log("texture view: only the whole of a 2D texture of one level can be viewed"); return nil; }
    return [self newTextureViewWithPixelFormat:newPf];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)newPf textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices swizzle:(MTLTextureSwizzleChannels)swizzle {
    if (type != MTLTextureType2D || levels.location != 0 || levels.length != 1 || slices.location != 0 || slices.length != 1) { rn_log("texture view: only the whole of a 2D texture of one level can be viewed"); return nil; }
    return (id)[self viewWithPixelFormat:newPf swizzle:&swizzle];
}
- (MTLTextureSwizzleChannels)swizzle { return swz.red == swz.green && swz.green == swz.blue && swz.blue == swz.alpha && swz.red == 0 && !parent ? (MTLTextureSwizzleChannels){MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha} : swz; }
- (MTLTextureCompressionType)compressionType { return MTLTextureCompressionTypeLossless; }
- (MTLResourceID)gpuResourceID { MTLResourceID r = {0}; return r; }
- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)token { return 0; }
- (id<MTLTexture>)newRemoteTextureViewForDevice:(id<MTLDevice>)device { rn_log("newRemoteTextureViewForDevice: not supported"); return nil; }
- (MTLSharedTextureHandle *)newSharedTextureHandle { rn_log("newSharedTextureHandle: not supported (an IOSurface-backed texture is shared through its IOSurface)"); return nil; }
- (id<MTLTexture>)remoteStorageTexture { return nil; }
- (MTLTextureType)textureType { return MTLTextureType2D; }
- (MTLPixelFormat)pixelFormat { return pf; }
- (NSUInteger)width { return w; }
- (NSUInteger)height { return h; }
- (NSUInteger)depth { return 1; }
- (NSUInteger)mipmapLevelCount { return 1; }
- (NSUInteger)sampleCount { return 1; }
- (NSUInteger)arrayLength { return 1; }
- (MTLTextureUsage)usage { return usage_; }
- (BOOL)isShareable { return NO; }
- (BOOL)isFramebufferOnly { return NO; }
- (BOOL)allowGPUOptimizedContents { return NO; }
- (BOOL)isSparse { return NO; }
- (NSUInteger)firstMipmapInTail { return 0; }
- (NSUInteger)tailSizeInBytes { return 0; }
- (BOOL)clampRegion:(MTLRegion)r {
    return r.origin.x + r.size.width <= w && r.origin.y + r.size.height <= h && r.origin.z == 0 && r.size.depth <= 1;
}
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi {
    if (level || slice || ![self clampRegion:region]) { rn_log("replaceRegion: level %lu slice %lu or region outside the texture ignored", (unsigned long)level, (unsigned long)slice); return; }
    uint8_t *dst = rn_vk_image_staging(img);
    for (NSUInteger y = 0; y < region.size.height; y++)
        memcpy(dst + ((region.origin.y + y) * w + region.origin.x) * texel, (const uint8_t *)bytes + y * bpr, region.size.width * texel);
    rn_vk_image_host_wrote(img);
    [self syncToSurface];
}
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr {
    [self replaceRegion:region mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:bpr bytesPerImage:0];
}
- (void)getBytes:(void *)out bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice {
    if (level || slice || ![self clampRegion:region]) { rn_log("getBytes: level %lu slice %lu or region outside the texture ignored", (unsigned long)level, (unsigned long)slice); return; }
    [self syncFromSurface];
    const uint8_t *src = rn_vk_image_staging(img);
    for (NSUInteger y = 0; y < region.size.height; y++)
        memcpy((uint8_t *)out + y * bpr, src + ((region.origin.y + y) * w + region.origin.x) * texel, region.size.width * texel);
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

// The resources a Metal encoder has bound, turned into the rn_vk arguments of a pipeline's bindings (compute, vertex and fragment stages all use it). Returns an error text or nil.
// setBytes data becomes a small buffer that is kept in `temps` until the work is done.
static NSString *rn_bind_args(id<MTLDevice> dev, unsigned n, const unsigned *kind, const unsigned *mi, RedNaviMTLBuffer *const *bufs, const uint64_t *offs, NSData *const *bytesAt,
                              RedNaviMTLTexture *const *texs, RedNaviMTLSamplerState *const *samps, rn_vk_arg *av, NSMutableArray *temps) {
    for (unsigned i = 0; i < n; i++) {
        const unsigned m = mi[i];
        switch (kind[i]) {
        case RN_VK_BUFFER:
            if (m < 16 && bufs[m]) { av[i].buffer = bufs[m]->b; av[i].offset = offs[m]; break; }
            if (m < 16 && bytesAt[m]) {
                RedNaviMTLBuffer *t = [[RedNaviMTLBuffer alloc] initWithDevice:dev length:bytesAt[m].length options:0];
                if (!t) return @"no memory for constant data";
                memcpy(rn_vk_buffer_contents(t->b), bytesAt[m].bytes, bytesAt[m].length);
                [temps addObject:t];
                av[i].buffer = t->b;
                break;
            }
            return [NSString stringWithFormat:@"no buffer bound at index %u", m];
        case RN_AIR_NULL_TEXTURE: {
            // a placeholder the translator adds for a texture that is absent in this variant: a 1 x 1 texture of zeros (made once)
            static rn_vk_image *nullImage;
            if (!nullImage) { rn_vk *vk = rn_vk_shared(); nullImage = vk ? rn_vk_image_create(vk, RN_VK_RGBA8, 1, 1) : NULL; }
            if (!nullImage) return @"no memory for a placeholder texture";
            av[i].image = nullImage;
            break;
        }
        case RN_VK_SAMPLED_IMAGE:
        case RN_VK_STORAGE_IMAGE:
            if (m < 16 && texs[m]) { av[i].image = texs[m]->img; break; }
            return [NSString stringWithFormat:@"no texture bound at index %u", m];
        case RN_VK_SAMPLER:
            if (m < 16 && samps[m]) { av[i].sampler = samps[m]->s; break; }
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
    RedNaviMTLBuffer *bufs[16];
    uint64_t offs[16];
    NSData *bytesAt[16];
    RedNaviMTLTexture *texs[16];
    RedNaviMTLSamplerState *samps[16];
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
    rn_vk_blend blend[8];
    unsigned nvattr, nvlayout;
    rn_vk_vattr vattr[16];
    rn_vk_vlayout vlayout[16];
    rn_vk_render *byKey[5 * 6];                     // by primitive type, cull mode and winding

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
    d.ntargets = nt; d.topology = prim; d.cull_mode = cull; d.front_clockwise = cw;
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
    RedNaviMTLBuffer *bufs[2][16];
    uint64_t offs[2][16];
    NSData *bytesAt[2][16];
    RedNaviMTLTexture *texs[2][16];
    RedNaviMTLSamplerState *samps[2][16];
    BOOL hasViewport, hasScissor, hasBlendColor;
    float blendColor[4];
    double viewport[6];
    uint32_t scissor[4];
    unsigned prim, vertexStart, vertexCount, instanceCount, baseInstance;
    unsigned cull;                                   // MTLCullMode
    BOOL clockwise;
    RedNaviMTLBuffer *indexBuf;                      // indexed draws: vertexCount is the index count
    uint64_t indexOffset;
    BOOL index32;
    int32_t baseVertex;
}
@end
@implementation RNDraw
@end

// Depth/stencil states exist so that apps get an object back; the render encoder does not test depth or stencil yet (setDepthStencilState: logs and ignores it).
@interface RedNaviMTLDepthStencilState : NSObject {
@public
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
        "MTLRenderPipelineState", "MTLComputePipelineState", "MTLSamplerState", "MTLDepthStencilState", "MTLSharedEvent", "MTLEvent", "MTLDrawable"};
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
    const char *types = rn_selector_types(sel);
    const char *r = types ? types : "Q16@0:8";
    while (*r == 'r' || *r == 'n' || *r == 'N' || *r == 'o' || *r == 'O' || *r == 'R' || *r == 'V') r++;
    IMP imp = (r[0] == 'f') ? (IMP)rn_stub_float : (r[0] == 'd') ? (IMP)rn_stub_double : (IMP)rn_stub_int;
    class_addMethod((Class)self, sel, imp, types ? types : "Q16@0:8");
    rn_log("unimplemented -[%s %s]: answering zero%s", class_getName((Class)self), sel_getName(sel), types ? "" : " (no protocol declares it)");
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
    NSMutableArray<RNDraw *> *draws;
}
@end
@implementation RNRenderPass
@end

@class RedNaviMTLCommandBuffer;

@interface RedNaviMTLComputeCommandEncoder : NSObject {
    RedNaviMTLCommandBuffer *cb;
    RedNaviMTLComputePipelineState *pso;
    RedNaviMTLBuffer *bufs[16];
    uint64_t offs[16];
    NSData *bytesAt[16];
    RedNaviMTLTexture *texs[16];
    RedNaviMTLSamplerState *samps[16];
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
    NSError *err;
    NSString *label_;
    dispatch_semaphore_t done;
}
- (instancetype)initWithQueue:(id<MTLCommandQueue>)q device:(id<MTLDevice>)d;
- (void)runRenderPass:(RNRenderPass *)rp;
- (void)fail:(NSString *)why;
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
    if (index >= 16) return;
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
    if (index >= 16) return;
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
    if (index < 16) texs[index] = (RedNaviMTLTexture *)texture;
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index { rn_log("setThreadgroupMemoryLength: not implemented, ignored (kernels with threadgroup memory arguments are refused by the translator)"); }
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained [])textures withRange:(NSRange)range {
    for (NSUInteger i = 0; i < range.length; i++) [self setTexture:textures[i] atIndex:range.location + i];
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    if (index < 16) samps[index] = (RedNaviMTLSamplerState *)sampler;
}
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])samplers withRange:(NSRange)range {
    for (NSUInteger i = 0; i < range.length; i++) [self setSamplerState:samplers[i] atIndex:range.location + i];
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler lodMinClamp:(float)lodMinClamp lodMaxClamp:(float)lodMaxClamp atIndex:(NSUInteger)index { [self setSamplerState:sampler atIndex:index]; }
- (void)record:(MTLSize)groups grid:(MTLSize)threads threads:(MTLSize)tpt {
    if (!pso) { rn_log("dispatch without a pipeline state"); return; }
    RNDispatch *d = [RNDispatch new];
    d->pso = pso;
    for (unsigned i = 0; i < 16; i++) { d->bufs[i] = bufs[i]; d->offs[i] = offs[i]; d->bytesAt[i] = bytesAt[i]; d->texs[i] = texs[i]; d->samps[i] = samps[i]; }
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

static NSString *rn_run_blit(RNBlit *b) {
    switch (b->kind) {
    case 0: case 1: {
        RedNaviMTLTexture *t = b->kind == 0 ? b->dt : b->st;
        RedNaviMTLBuffer *bf = b->kind == 0 ? b->sb : b->db;
        const MTLOrigin o = b->kind == 0 ? b->dorg : b->so;
        const NSUInteger bpr = b->kind == 0 ? b->sbpr : b->dbpr, off = b->kind == 0 ? b->soff : b->doff;
        if (!t || !bf) return @"blit: a texture or buffer is missing";
        if (o.z || b->sz.depth > 1 || o.x + b->sz.width > t->w || o.y + b->sz.height > t->h) return @"blit: the region is outside the texture";
        if (!b->sz.width || !b->sz.height) return nil;
        if (bpr < b->sz.width * t->texel) return @"blit: bytesPerRow is smaller than a row of the region";
        if (off + (b->sz.height - 1) * bpr + b->sz.width * t->texel > bf->len) return @"blit: the region is outside the buffer";
        uint8_t *tex = rn_vk_image_staging(t->img), *buf = (uint8_t *)rn_vk_buffer_contents(bf->b) + off;
        for (NSUInteger y = 0; y < b->sz.height; y++) {
            uint8_t *tp = tex + ((o.y + y) * t->w + o.x) * t->texel, *bp = buf + y * bpr;
            if (b->kind == 0) memcpy(tp, bp, b->sz.width * t->texel); else memcpy(bp, tp, b->sz.width * t->texel);
        }
        if (b->kind == 0) rn_vk_image_host_wrote(t->img);
        return nil;
    }
    case 2: {
        RedNaviMTLTexture *s2 = b->st, *d2 = b->dt;
        if (!s2 || !d2) return @"blit: a texture is missing";
        if (s2->texel != d2->texel) return @"blit: the textures have different texel sizes";
        if (b->so.z || b->dorg.z || b->sz.depth > 1 || b->so.x + b->sz.width > s2->w || b->so.y + b->sz.height > s2->h || b->dorg.x + b->sz.width > d2->w || b->dorg.y + b->sz.height > d2->h)
            return @"blit: the region is outside a texture";
        const uint8_t *sp = rn_vk_image_staging(s2->img);
        uint8_t *dp = rn_vk_image_staging(d2->img);
        for (NSUInteger y = 0; y < b->sz.height; y++) memmove(dp + ((b->dorg.y + y) * d2->w + b->dorg.x) * d2->texel, sp + ((b->so.y + y) * s2->w + b->so.x) * s2->texel, b->sz.width * s2->texel);
        rn_vk_image_host_wrote(d2->img);
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
    if (slice || level) b->dt = nil;                  // reported as an error at commit
}
- (void)copyFromBuffer:(id<MTLBuffer>)buf sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)tex
      destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin options:(MTLBlitOption)options {
    [self copyFromBuffer:buf sourceOffset:so sourceBytesPerRow:bpr sourceBytesPerImage:bpi sourceSize:size toTexture:tex destinationSlice:slice destinationLevel:level destinationOrigin:origin];
}
- (void)copyFromTexture:(id<MTLTexture>)tex sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)buf
      destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi {
    RNBlit *b = [self add:1];
    b->st = (RedNaviMTLTexture *)tex; b->so = origin; b->sz = size; b->db = (RedNaviMTLBuffer *)buf; b->doff = doff; b->dbpr = bpr;
    if (slice || level) b->st = nil;
}
- (void)copyFromTexture:(id<MTLTexture>)tex sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)buf
      destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi options:(MTLBlitOption)options {
    [self copyFromTexture:tex sourceSlice:slice sourceLevel:level sourceOrigin:origin sourceSize:size toBuffer:buf destinationOffset:doff destinationBytesPerRow:bpr destinationBytesPerImage:bpi];
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst
       destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg {
    RNBlit *b = [self add:2];
    b->st = (RedNaviMTLTexture *)src; b->so = so; b->sz = size; b->dt = (RedNaviMTLTexture *)dst; b->dorg = dorg;
    if (ss || sl || ds || dl) b->dt = nil;
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
    RedNaviMTLBuffer *bufs[2][16];
    uint64_t offs[2][16];
    NSData *bytesAt[2][16];
    RedNaviMTLTexture *texs[2][16];
    RedNaviMTLSamplerState *samps[2][16];
    BOOL hasViewport, hasScissor, hasBlendColor;
    float blendColor[4];
    double viewport[6];
    uint32_t scissor[4];
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
- (void)bindBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o index:(NSUInteger)i stage:(int)st { if (i < 16) { bufs[st][i] = (RedNaviMTLBuffer *)b; offs[st][i] = o; bytesAt[st][i] = nil; } }
- (void)bindBytes:(const void *)bytes length:(NSUInteger)len index:(NSUInteger)i stage:(int)st { if (i < 16) { bytesAt[st][i] = [NSData dataWithBytes:bytes length:len]; bufs[st][i] = nil; } }
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index { [self bindBuffer:buffer offset:offset index:index stage:0]; }
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained [])buffers offsets:(const NSUInteger [])offsets withRange:(NSRange)range {
    for (NSUInteger i = 0; i < range.length; i++) [self bindBuffer:buffers[i] offset:offsets[i] index:range.location + i stage:0];
}
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index { if (index < 16) offs[0][index] = offset; }
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index { [self bindBytes:bytes length:length index:index stage:0]; }
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index { [self bindBuffer:buffer offset:offset index:index stage:1]; }
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained [])buffers offsets:(const NSUInteger [])offsets withRange:(NSRange)range {
    for (NSUInteger i = 0; i < range.length; i++) [self bindBuffer:buffers[i] offset:offsets[i] index:range.location + i stage:1];
}
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index { if (index < 16) offs[1][index] = offset; }
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index { [self bindBytes:bytes length:length index:index stage:1]; }
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { if (i < 16) texs[0][i] = (RedNaviMTLTexture *)t; }
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { if (i < 16) texs[1][i] = (RedNaviMTLTexture *)t; }
- (void)setVertexTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r { for (NSUInteger i = 0; i < r.length; i++) [self setVertexTexture:t[i] atIndex:r.location + i]; }
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r { for (NSUInteger i = 0; i < r.length; i++) [self setFragmentTexture:t[i] atIndex:r.location + i]; }
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { if (i < 16) samps[0][i] = (RedNaviMTLSamplerState *)s; }
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { if (i < 16) samps[1][i] = (RedNaviMTLSamplerState *)s; }
- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s withRange:(NSRange)r { for (NSUInteger i = 0; i < r.length; i++) [self setVertexSamplerState:s[i] atIndex:r.location + i]; }
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s withRange:(NSRange)r { for (NSUInteger i = 0; i < r.length; i++) [self setFragmentSamplerState:s[i] atIndex:r.location + i]; }
- (void)setViewport:(MTLViewport)v {
    hasViewport = YES;
    viewport[0] = v.originX; viewport[1] = v.originY; viewport[2] = v.width; viewport[3] = v.height; viewport[4] = v.znear; viewport[5] = v.zfar;
}
- (void)setScissorRect:(MTLScissorRect)r { hasScissor = YES; scissor[0] = (uint32_t)r.x; scissor[1] = (uint32_t)r.y; scissor[2] = (uint32_t)r.width; scissor[3] = (uint32_t)r.height; }
// state that is not implemented yet is logged once and ignored: there is no depth test
- (void)ignored:(const char *)what { static NSMutableSet *seen; if (!seen) seen = [NSMutableSet new]; NSString *k = @(what); if (![seen containsObject:k]) { [seen addObject:k]; rn_log("render encoder: %s is not implemented, ignored", what); } }
- (void)setDepthClipMode:(MTLDepthClipMode)m { if (m != MTLDepthClipModeClip) [self ignored:"depth clip mode (clamp)"]; }
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)m offset:(NSUInteger)o { if (m != MTLVisibilityResultModeDisabled) [self ignored:"visibility result mode (occlusion queries)"]; }
- (void)textureBarrier { [self ignored:"textureBarrier"]; }
- (void)setCullMode:(MTLCullMode)m { cullMode = (unsigned)m; }
- (void)setFrontFacingWinding:(MTLWinding)w { counterClockwise = w == MTLWindingCounterClockwise; }
- (void)setTriangleFillMode:(MTLTriangleFillMode)m { if (m != MTLTriangleFillModeFill) [self ignored:"triangle fill mode"]; }
- (void)setDepthStencilState:(id<MTLDepthStencilState>)s { [self ignored:"depth stencil state"]; }
- (void)setDepthBias:(float)d slopeScale:(float)s clamp:(float)c {}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a { hasBlendColor = YES; blendColor[0] = r; blendColor[1] = g; blendColor[2] = b; blendColor[3] = a; }
- (void)setStencilReferenceValue:(uint32_t)v {}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances baseInstance:(NSUInteger)base {
    if (!pso) { rn_log("draw without a render pipeline state"); return; }
    RNDraw *d = [RNDraw new];
    d->pso = pso;
    for (int s = 0; s < 2; s++) for (int i = 0; i < 16; i++) { d->bufs[s][i] = bufs[s][i]; d->offs[s][i] = offs[s][i]; d->bytesAt[s][i] = bytesAt[s][i]; d->texs[s][i] = texs[s][i]; d->samps[s][i] = samps[s][i]; }
    d->hasViewport = hasViewport; memcpy(d->viewport, viewport, sizeof(viewport));
    d->hasScissor = hasScissor; memcpy(d->scissor, scissor, sizeof(scissor));
    d->hasBlendColor = hasBlendColor; memcpy(d->blendColor, blendColor, sizeof(blendColor));
    d->prim = (unsigned)type; d->vertexStart = (unsigned)start; d->vertexCount = (unsigned)count; d->instanceCount = (unsigned)instances; d->baseInstance = (unsigned)base;
    d->cull = cullMode; d->clockwise = !counterClockwise;
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

@implementation RedNaviMTLCommandBuffer
+ (void)load { class_addProtocol(self, @protocol(MTLCommandBuffer)); }
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
- (void)addScheduledHandler:(MTLCommandBufferHandler)block { [scheduled addObject:[block copy]]; }
- (void)addCompletedHandler:(MTLCommandBufferHandler)block { [completed addObject:[block copy]]; }
- (void)waitUntilScheduled {}
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
    if (!rp->nt) { rn_log("renderCommandEncoder: no colour attachment (depth-only passes are not implemented)"); return nil; }
    if (d.depthAttachment.texture || d.stencilAttachment.texture) rn_log("renderCommandEncoder: depth and stencil attachments are not implemented, ignored");
    return (id)[[RedNaviMTLRenderCommandEncoder alloc] initWithCommandBuffer:self pass:rp];
}
- (void)runRenderPass:(RNRenderPass *)rp {
    rn_vk *vk = rn_vk_shared();
    const NSUInteger nd = rp->draws.count;
    rn_vk_target tg[8];
    for (unsigned i = 0; i < rp->nt; i++) { tg[i] = (rn_vk_target){rp->tex[i]->img, rp->clear[i], {rp->clearColor[i][0], rp->clearColor[i][1], rp->clearColor[i][2], rp->clearColor[i][3]}, !rp->store[i]}; }
    const NSUInteger na = nd ? nd : 1;               // a pass without draws still runs: its clears are done
    rn_vk_draw *dr = calloc(na, sizeof(*dr));
    rn_vk_arg (*va)[RN_VK_MAX_BINDINGS] = calloc(na, sizeof(*va));
    rn_vk_arg (*fa)[RN_VK_MAX_BINDINGS] = calloc(na, sizeof(*fa));
    rn_vk_vbuf (*vbs)[16] = calloc(na, sizeof(*vbs));
    NSMutableArray *temps = [NSMutableArray new];
    NSString *bad = (dr && va && fa && vbs) ? nil : @"out of memory";
    for (NSUInteger k = 0; k < nd && !bad; k++) {
        RNDraw *d = rp->draws[k];
        RedNaviMTLRenderPipelineState *p = d->pso;
        rn_vk_render *r = [p renderForPrimitive:d->prim cull:d->cull clockwise:d->clockwise];
        if (!r) { bad = [NSString stringWithFormat:@"RADV has no pipeline for primitive type %u", d->prim]; break; }
        bad = rn_bind_args(dev, p->n[0], p->kind[0], p->mi[0], d->bufs[0], d->offs[0], d->bytesAt[0], d->texs[0], d->samps[0], va[k], temps);
        if (!bad) bad = rn_bind_args(dev, p->n[1], p->kind[1], p->mi[1], d->bufs[1], d->offs[1], d->bytesAt[1], d->texs[1], d->samps[1], fa[k], temps);
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
        memcpy(dr[k].blend_color, d->blendColor, sizeof(dr[k].blend_color));
        memcpy(dr[k].viewport, d->viewport, sizeof(dr[k].viewport));
        memcpy(dr[k].scissor, d->scissor, sizeof(dr[k].scissor));
    }
    int rc = bad ? -1 : rn_vk_render_pass(vk, tg, rp->nt, dr, (unsigned)nd);
    free(dr); free(va); free(fa); free(vbs);
    if (bad) [self fail:bad];
    else if (rc == 4) [self fail:@"a buffer offset is not a multiple of the storage buffer offset alignment, or beyond the buffer"];
    else if (rc == 5) [self fail:@"the render pipeline does not match the attachments of the pass"];
    else if (rc == 6) [self fail:@"a resource of the render pipeline is missing or cannot be used as bound"];
    else if (rc) [self fail:[NSString stringWithFormat:@"render pass failed (%d)", rc]];
}
- (void)fail:(NSString *)why {
    rn_log("command buffer failed: %s", why.UTF8String);
    err = [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal userInfo:@{NSLocalizedDescriptionKey: why}];
    st = MTLCommandBufferStatusError;
}
// The first version runs the recorded dispatches synchronously on the calling thread.
- (void)commit {
    if (st >= MTLCommandBufferStatusCommitted) return;
    st = MTLCommandBufferStatusCommitted;
    for (MTLCommandBufferHandler h in scheduled) h((id)self);
    rn_vk *vk = rn_vk_shared();
    if (!vk) [self fail:@"RADV is not available"];
    // IOSurface-backed textures: what the surface holds goes to the images first, what the command buffer wrote goes back to the surface afterwards
    NSMutableOrderedSet<RedNaviMTLTexture *> *surfUsed = [NSMutableOrderedSet orderedSet], *surfWritten = [NSMutableOrderedSet orderedSet];
    void (^note)(RedNaviMTLTexture *, BOOL) = ^(RedNaviMTLTexture *t, BOOL written) {
        if (!t || !t->surf) return;
        [surfUsed addObject:t];
        if (written) [surfWritten addObject:t];
    };
    for (id rec in records) {
        if ([rec isKindOfClass:[RNRenderPass class]]) {
            RNRenderPass *rp = rec;
            for (unsigned i = 0; i < rp->nt; i++) note(rp->tex[i], rp->store[i]);
            for (RNDraw *dw in rp->draws) for (int s2 = 0; s2 < 2; s2++) for (int i = 0; i < 16; i++) note(dw->texs[s2][i], NO);
        } else if ([rec isKindOfClass:[RNEventOp class]]) {
            // no textures
        } else if ([rec isKindOfClass:[RNBlit class]]) {
            RNBlit *bl = rec;
            note(bl->st, NO);
            note(bl->dt, YES);
        } else {
            RNDispatch *dp = rec;
            for (int i = 0; i < 16; i++) note(dp->texs[i], dp->texs[i] && (dp->texs[i]->usage_ & MTLTextureUsageShaderWrite));
        }
    }
    for (RedNaviMTLTexture *t in surfUsed) [t syncFromSurface];
    for (id rec in records) {
        if (st == MTLCommandBufferStatusError) break;
        if ([rec isKindOfClass:[RNRenderPass class]]) { [self runRenderPass:rec]; continue; }
        if ([rec isKindOfClass:[RNBlit class]]) { NSString *why = rn_run_blit(rec); if (why) [self fail:why]; continue; }
        if ([rec isKindOfClass:[RNEventOp class]]) {
            RNEventOp *o = rec;
            if (![o->event isKindOfClass:[RedNaviMTLSharedEvent class]]) { [self fail:@"an event that is not from RedNavi"]; break; }
            if (!o->wait) { if (o->event.signaledValue < o->value) o->event.signaledValue = o->value; }
            else if (![o->event waitFor:o->value timeoutMS:10000]) [self fail:[NSString stringWithFormat:@"waiting for an event to reach %llu timed out after 10 s (it is at %llu)", o->value, o->event.signaledValue]];
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
    if (st != MTLCommandBufferStatusError) for (RedNaviMTLTexture *t in surfWritten) [t syncToSurface];
    if (st != MTLCommandBufferStatusError) st = MTLCommandBufferStatusCompleted;
    for (MTLCommandBufferHandler h in completed) h((id)self);
    for (id<MTLDrawable> dr in presents) [dr present];
    dispatch_semaphore_signal(done);
}
- (void)waitUntilCompleted {
    if (st < MTLCommandBufferStatusCommitted) return;
    dispatch_semaphore_wait(done, DISPATCH_TIME_NOW);
}
@end

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
    if (!(self = [super init])) return nil;
    rn_mtl_func fn[512];
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
            if (rn_metallib_parse(slice.bytes, slice.length, fn, 512, &c2) == 0) { blob = slice; rn_log("library: fat file, using slice %u (%lu bytes)", i, sz); break; }
        }
    }
    int rc = rn_metallib_parse(blob.bytes, blob.length, fn, 512, &cnt);
    if (rc) {
        rn_log("newLibraryWithData: not a metallib this reader understands (code %d)", rc);
        if (error) *error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorUnsupported userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"unreadable metallib (%d)", rc]}];
        return nil;
    }
    dev = d; data = blob; funcs = [NSMutableDictionary new];
    NSMutableArray *nm = [NSMutableArray new];
    for (unsigned i = 0; i < cnt && i < 512; i++) {
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
                    sds[i] = (rn_vk_sampler_desc){ab->min_filter, ab->mag_filter, ab->mip_filter, {ab->address[0], ab->address[1], ab->address[2]}, ab->pixel_coords, ab->border, ab->lod_min, ab->lod_max};
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

// A render pipeline state from a descriptor: both functions go to the translator (vertex module in set 0, fragment module in set 1) and the Vulkan pipeline for triangles is built.
static id rn_render_pso_from_descriptor(id<MTLDevice> dev, MTLRenderPipelineDescriptor *desc, NSError **error) {
    NSString *why = nil;
    RedNaviMTLFunction *fn[2] = {(RedNaviMTLFunction *)desc.vertexFunction, (RedNaviMTLFunction *)desc.fragmentFunction};
    rn_vk *vk = rn_vk_shared();
    RedNaviMTLRenderPipelineState *p = [RedNaviMTLRenderPipelineState new];
    if (!vk) why = @"RADV is not available";
    else if (!fn[0] || !fn[1]) why = @"a render pipeline needs a vertex and a fragment function";
    else for (int s = 0; s < 2 && !why; s++) {
        if (![fn[s] isKindOfClass:[RedNaviMTLFunction class]]) { why = @"the function is not from a RedNavi library"; break; }
        if (fn[s]->type != (s ? MTLFunctionTypeFragment : MTLFunctionTypeVertex)) { why = @"the function has the wrong type for its stage"; break; }
        char err[300] = "";
        uint32_t *spv = NULL;
        size_t spvBytes = 0;
        rn_air_kernel_info info;
        memset(&info, 0, sizeof(info));
        rn_air_const fcc[64];
        const unsigned nfc = rn_fc_array(fn[s]->fcs, fcc, 64);
        if (rn_air_translate_stage_fc(NULL, s ? RN_AIR_FRAGMENT : RN_AIR_VERTEX, fn[s]->bitcode.bytes, fn[s]->bitcode.length, fn[s]->name.UTF8String, fcc, nfc, &info, &spv, &spvBytes, err, sizeof(err))) {
            why = [NSString stringWithFormat:@"%s function %s: %s", s ? "fragment" : "vertex", fn[s]->name.UTF8String, err];
            break;
        }
        p->spv[s] = [NSData dataWithBytes:spv length:spvBytes];
        rn_air_free(spv, spvBytes);
        unsigned n = info.binding_count < RN_VK_MAX_BINDINGS ? info.binding_count : RN_VK_MAX_BINDINGS;
        p->n[s] = n;
        for (unsigned i = 0; i < n; i++) {
            const rn_air_binding *ab = &info.bindings[i];
            p->binds[s][i] = (rn_vk_binding){ab->binding, ab->writable, ab->kind == RN_AIR_NULL_TEXTURE ? RN_VK_SAMPLED_IMAGE : ab->kind, NULL};
            p->kind[s][i] = ab->kind;
            p->mi[s][i] = ab->metal_index;
            if (ab->kind == RN_AIR_STATIC_SAMPLER) {
                p->sampl[s][i] = (rn_vk_sampler_desc){ab->min_filter, ab->mag_filter, ab->mip_filter, {ab->address[0], ab->address[1], ab->address[2]}, ab->pixel_coords, ab->border, ab->lod_min, ab->lod_max};
                p->binds[s][i].sampler = &p->sampl[s][i];
            }
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
        for (NSUInteger i = 0; vd && !why && i < 16; i++) {
            MTLVertexAttributeDescriptor *a = vd.attributes[i];
            if (a.format == MTLVertexFormatInvalid) continue;
            const unsigned vf = rn_vk_vertex_format(a.format);
            if (!vf) { why = [NSString stringWithFormat:@"vertex attribute %lu: format %lu is not supported", (unsigned long)i, (unsigned long)a.format]; break; }
            if (a.bufferIndex >= 16) { why = @"a vertex buffer index is 16 or more"; break; }
            p->vattr[p->nvattr++] = (rn_vk_vattr){(unsigned)i, (unsigned)a.bufferIndex, vf, (unsigned)a.offset};
        }
        for (NSUInteger i = 0; vd && !why && i < 16; i++) {
            MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
            BOOL used = NO;
            for (unsigned k = 0; k < p->nvattr; k++) if (p->vattr[k].binding == i) used = YES;
            if (!used) continue;
            if (l.stepFunction != MTLVertexStepFunctionPerVertex && l.stepFunction != MTLVertexStepFunctionPerInstance) { why = @"this vertex step function is not supported"; break; }
            if (l.stepFunction == MTLVertexStepFunctionPerInstance && l.stepRate != 1) { why = @"a vertex step rate other than 1 is not supported"; break; }
            if (l.stride == 0) { why = @"a vertex buffer layout with stride 0 is not supported"; break; }
            p->vlayout[p->nvlayout++] = (rn_vk_vlayout){(unsigned)i, (unsigned)l.stride, l.stepFunction == MTLVertexStepFunctionPerInstance};
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
    return p;
}

// Test hook for tools/mtl_library_test.m: a library from data without a device.
id rn_test_library(NSData *blob, NSError **error) { return [[RedNaviMTLLibrary alloc] initWithDevice:nil data:blob error:error]; }

// ---------------------------------------------------------------------------------------------------------------------------------------- installation on the device class

typedef uintptr_t (*rn_imp_len_opt)(id, SEL, NSUInteger, NSUInteger);
typedef uintptr_t (*imp_fn_err1)(id, SEL, id);
typedef uintptr_t (*rn_imp_bytes)(id, SEL, const void *, NSUInteger, NSUInteger);

void rn_compute_install(Class k, Class sup) {
    {
        const char *classes[] = {"RedNaviMTLBuffer", "RedNaviMTLTexture", "RedNaviMTLSamplerState", "RedNaviMTLComputePipelineState", "RedNaviMTLRenderPipelineState", "RedNaviMTLComputeCommandEncoder",
                                 "RedNaviMTLRenderCommandEncoder", "RedNaviMTLBlitCommandEncoder", "RedNaviMTLCommandBuffer", "RedNaviMTLCommandQueue", "RedNaviMTLFunction", "RedNaviMTLLibrary",
                                 "RedNaviMTLDepthStencilState", "RedNaviMTLSharedEvent"};
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
    // Textures and sampler states
    SEL s7 = sel_registerName("newTextureWithDescriptor:");
    Method m7 = class_getInstanceMethod(sup, s7);
    imp_fn_err1 o7 = m7 ? (imp_fn_err1)method_getImplementation(m7) : NULL;
    class_addMethod(k, s7, imp_implementationWithBlock(^uintptr_t(id self, MTLTextureDescriptor *desc) {
        if (!rn_gate()) return o7 ? o7(self, s7, desc) : 0;
        return (uintptr_t)CFBridgingRetain([[RedNaviMTLTexture alloc] initWithDevice:(id<MTLDevice>)self descriptor:desc]);
    }), m7 ? method_getTypeEncoding(m7) : "@24@0:8@16");
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
        return (uintptr_t)CFBridgingRetain([[RedNaviMTLTexture alloc] initWithDevice:(id<MTLDevice>)self descriptor:desc iosurface:io plane:plane]);
    }), m7b ? method_getTypeEncoding(m7b) : "@40@0:8@16^{__IOSurface=}24Q32");
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
        return (uintptr_t)CFBridgingRetain(rn_render_pso_from_descriptor((id<MTLDevice>)self, desc, error));
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
        handler(p, nil, error);
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
            return (uintptr_t)CFBridgingRetain(st);
        }), mds ? method_getTypeEncoding(mds) : "@24@0:8@16");
    }
    // Capabilities. The base class answers these from tables its own driver fills in, which a device made by this plug-in does not have (calling -supportsFamily: ended in a call through
    // a null pointer inside QuartzCore's Metal context setup), so they are answered here: what a discrete Navi 32 on macOS supports as far as the plug-in and the translator go. The Apple
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
            const BOOL yes = set >= 10000 && set <= 10005;                    // macOS GPUFamily1 v1 to v4, GPUFamily2 v1
            rn_log("supportsFeatureSet: %ld -> %d", (long)set, yes);
            return yes;
        }), mfs ? method_getTypeEncoding(mfs) : "B24@0:8Q16");
        static const struct { const char *name; BOOL value; } caps[] = {
            {"supportsBufferlessClientStorageTexture", YES}, {"supportsExtendedXR10Formats", YES}, {"supportsMemorylessRenderTargets", NO}, {"supportsNativeHardwareFP16", NO},
            {"supportsRasterOrderGroups", NO}, {"supportsSIMDGroup", NO}, {"supportsSIMDReduction", NO}, {"supportsSIMDShuffleAndFill", NO}, {"supportsTileShaders", NO},
            {"isFramebufferReadSupported", NO}, {"supportsYCBCRFormats12", NO}, {"supportsYCBCRPackedFormats12", NO}, {"supportsExpectedWakeupBeforeCommitDeadline", NO}};
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
    // the maximum allowed size of 0" inside QuartzCore's context setup). Values are those of a Navi 32 as far as they are known (texture sizes and binding counts of Vulkan/RADV on this
    // card, alignments of its memory); the type of each answer is the type the base class method returns.
    {
        static const struct { const char *name; double value; } limits[] = {
            {"maxTextureWidth1D", 16384}, {"maxTextureWidth2D", 16384}, {"maxTextureHeight2D", 16384}, {"maxTextureWidth3D", 2048}, {"maxTextureHeight3D", 2048}, {"maxTextureDepth3D", 2048},
            {"maxTextureDimensionCube", 16384}, {"maxTextureLayers", 2048}, {"maxTextureBufferWidth", 134217728}, {"maxColorAttachments", 8}, {"maxVertexBuffers", 31}, {"maxFragmentBuffers", 31},
            {"maxComputeBuffers", 31}, {"maxTileBuffers", 31}, {"maxFragmentTextures", 128}, {"maxComputeTextures", 128}, {"maxTileTextures", 128}, {"maxFragmentSamplers", 16},
            {"maxComputeSamplers", 16}, {"maxTileSamplers", 16}, {"maxVertexAttributes", 31}, {"maxComputeAttributes", 31}, {"maxInterpolants", 32}, {"maxInterpolatedComponents", 124},
            {"maxTotalComputeThreadsPerThreadgroup", 1024}, {"maxComputeThreadgroupMemory", 32768}, {"maxConstantBufferArguments", 31}, {"maxFunctionConstantIndices", 65536},
            {"maxLineWidth", 1}, {"maxPointSize", 511}, {"maxBufferLength", 4294967296.0}, {"maxFramebufferStorageBits", 256}, {"maxVertexInlineDataSize", 4096}, {"maxFragmentInlineDataSize", 4096},
            {"maxComputeInlineDataSize", 4096}, {"maxTileInlineDataSize", 4096}, {"maxCustomSamplePositions", 16}, {"maxTessellationFactor", 64},
            {"linearTextureAlignmentBytes", 256}, {"deviceLinearTextureAlignmentBytes", 256}, {"deviceLinearReadOnlyTextureAlignmentBytes", 256}, {"linearTextureArrayAlignmentBytes", 256},
            {"linearTextureArrayAlignmentSlice", 256}, {"iosurfaceTextureAlignmentBytes", 256}, {"iosurfaceReadOnlyTextureAlignmentBytes", 256}};
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
}
