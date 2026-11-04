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
#import <Metal/Metal.h>
#import <objc/runtime.h>
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

@interface RedNaviMTLComputePipelineState : NSObject {
@public
    rn_vk_compute *c;
    unsigned nbuf, push;
    unsigned pushIndex;              // the Metal buffer index that setBytes fills (0xffffffff: none)
    unsigned localSize[3];
    id<MTLDevice> dev;
    NSString *label_;
}
@end
@implementation RedNaviMTLComputePipelineState
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

// One recorded dispatch.
@interface RNDispatch : NSObject {
@public
    RedNaviMTLComputePipelineState *pso;
    RedNaviMTLBuffer *bufs[16];
    uint64_t offs[16];
    uint8_t push[128];
    uint32_t pushLen;
    MTLSize groups;
}
@end
@implementation RNDispatch
@end

@class RedNaviMTLCommandBuffer;

@interface RedNaviMTLComputeCommandEncoder : NSObject {
    RedNaviMTLCommandBuffer *cb;
    RedNaviMTLComputePipelineState *pso;
    RedNaviMTLBuffer *bufs[16];
    uint64_t offs[16];
    uint8_t push[128];
    uint32_t pushLen;
    NSString *label_;
    BOOL ended;
}
- (instancetype)initWithCommandBuffer:(RedNaviMTLCommandBuffer *)c;
@end

@interface RedNaviMTLCommandBuffer : NSObject {
@public
    id<MTLCommandQueue> queue;
    id<MTLDevice> dev;
    NSMutableArray<RNDispatch *> *records;
    NSMutableArray *completed, *scheduled;
    MTLCommandBufferStatus st;
    NSError *err;
    NSString *label_;
    dispatch_semaphore_t done;
}
- (instancetype)initWithQueue:(id<MTLCommandQueue>)q device:(id<MTLDevice>)d;
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
}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained [])buffers offsets:(const NSUInteger [])offsets withRange:(NSRange)range {
    for (NSUInteger i = 0; i < range.length; i++) [self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    if (!pso || index != pso->pushIndex) { rn_log("setBytes at index %lu: the pipeline takes constant data at index %d only", (unsigned long)index, pso ? (int)pso->pushIndex : -1); return; }
    if (length > sizeof(push)) length = sizeof(push);
    memcpy(push, bytes, length);
    pushLen = (uint32_t)length;
}
- (void)record:(MTLSize)groups {
    if (!pso) { rn_log("dispatch without a pipeline state"); return; }
    RNDispatch *d = [RNDispatch new];
    d->pso = pso;
    for (unsigned i = 0; i < 16; i++) { d->bufs[i] = bufs[i]; d->offs[i] = offs[i]; }
    memcpy(d->push, push, sizeof(push));
    d->pushLen = pushLen;
    d->groups = groups;
    [cb->records addObject:d];
}
- (void)dispatchThreadgroups:(MTLSize)tg threadsPerThreadgroup:(MTLSize)tpt { [self record:tg]; }
- (void)dispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)tpt {
    MTLSize g = {(threads.width + tpt.width - 1) / (tpt.width ? tpt.width : 1), (threads.height + tpt.height - 1) / (tpt.height ? tpt.height : 1), (threads.depth + tpt.depth - 1) / (tpt.depth ? tpt.depth : 1)};
    [self record:g];
}
- (void)endEncoding { ended = YES; }
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope {}
- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained [])resources count:(NSUInteger)count {}
- (void)insertDebugSignpost:(NSString *)string {}
- (void)pushDebugGroup:(NSString *)string {}
- (void)popDebugGroup {}
@end

@implementation RedNaviMTLCommandBuffer
+ (void)load { class_addProtocol(self, @protocol(MTLCommandBuffer)); }
- (instancetype)initWithQueue:(id<MTLCommandQueue>)q device:(id<MTLDevice>)d {
    if ((self = [super init])) {
        queue = q; dev = d; records = [NSMutableArray new]; completed = [NSMutableArray new]; scheduled = [NSMutableArray new];
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
- (void)pushDebugGroup:(NSString *)string {}
- (void)popDebugGroup {}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder { return (id)[[RedNaviMTLComputeCommandEncoder alloc] initWithCommandBuffer:self]; }
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)t { return [self computeCommandEncoder]; }
- (id<MTLBlitCommandEncoder>)blitCommandEncoder { rn_log("blitCommandEncoder: not implemented, nil"); return nil; }
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d { rn_log("renderCommandEncoder: not implemented, nil"); return nil; }
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
    for (RNDispatch *d in records) {
        if (st == MTLCommandBufferStatusError) break;
        rn_vk_buffer *bv[16];
        uint64_t ov[16];
        for (unsigned i = 0; i < d->pso->nbuf; i++) {
            if (!d->bufs[i]) { [self fail:[NSString stringWithFormat:@"no buffer bound at index %u", i]]; break; }
            bv[i] = d->bufs[i]->b;
            ov[i] = d->offs[i];
        }
        if (st == MTLCommandBufferStatusError) break;
        int rc = rn_vk_dispatch(vk, d->pso->c, bv, ov, d->pushLen ? d->push : NULL, (uint32_t)d->groups.width, (uint32_t)d->groups.height, (uint32_t)d->groups.depth);
        if (rc == 4) [self fail:@"a buffer offset is not a multiple of the storage buffer offset alignment, or beyond the buffer"];
        else if (rc) [self fail:[NSString stringWithFormat:@"dispatch failed (%d)", rc]];
    }
    if (st != MTLCommandBufferStatusError) st = MTLCommandBufferStatusCompleted;
    for (MTLCommandBufferHandler h in completed) h((id)self);
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

@interface RedNaviMTLFunction : NSObject {
@public
    NSString *name;
    MTLFunctionType type;
    NSData *bitcode;
    id<MTLDevice> dev;
    NSString *label_;
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
    if (!f) return nil;
    RedNaviMTLFunction *c = [RedNaviMTLFunction new];
    c->name = f->name; c->type = f->type; c->bitcode = f->bitcode; c->dev = f->dev;
    return (id)c;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)functionName constantValues:(MTLFunctionConstantValues *)constantValues error:(NSError **)error { return [self newFunctionWithName:functionName]; }
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
        rn_air_kernel_info info = {0, 0, 0xffffffffu, {1, 1, 1}};
        if (rn_air_translate(NULL, f->bitcode.bytes, f->bitcode.length, f->name.UTF8String, &info, &spv, &spvBytes, err, sizeof(err))) {
            why = [NSString stringWithUTF8String:err];
        } else {
            rn_vk_compute *c = rn_vk_compute_create(vk, spv, spvBytes, info.buffer_count, info.push_bytes);
            rn_air_free(spv);
            if (!c) why = @"RADV refused the translated module";
            else {
                RedNaviMTLComputePipelineState *p = [RedNaviMTLComputePipelineState new];
                p->c = c; p->nbuf = info.buffer_count; p->push = info.push_bytes; p->pushIndex = info.push_index; p->dev = dev;
                for (int i = 0; i < 3; i++) p->localSize[i] = info.local_size[i];
                return p;
            }
        }
    }
    rn_log("compute pipeline for %s failed: %s", [f isKindOfClass:[RedNaviMTLFunction class]] ? f->name.UTF8String : "?", why.UTF8String);
    if (error) *error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorInternal userInfo:@{NSLocalizedDescriptionKey: why}];
    return nil;
}

// Test hook for tools/mtl_library_test.m: a library from data without a device.
id rn_test_library(NSData *blob, NSError **error) { return [[RedNaviMTLLibrary alloc] initWithDevice:nil data:blob error:error]; }

// ---------------------------------------------------------------------------------------------------------------------------------------- installation on the device class

typedef uintptr_t (*rn_imp_len_opt)(id, SEL, NSUInteger, NSUInteger);
typedef uintptr_t (*rn_imp_bytes)(id, SEL, const void *, NSUInteger, NSUInteger);

void rn_compute_install(Class k, Class sup) {
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
    // The private shader entry
    class_addMethod(k, sel_registerName("rn_newComputePipelineWithSPIRV:bufferCount:pushBytes:"), imp_implementationWithBlock(^id(id self, NSData *spirv, NSUInteger nbuf, NSUInteger push) {
        rn_vk *vk = rn_gate() ? rn_vk_shared() : NULL;
        if (!vk) return nil;
        rn_vk_compute *c = rn_vk_compute_create(vk, spirv.bytes, spirv.length, (unsigned)nbuf, (unsigned)push);
        if (!c) { rn_log("compute pipeline from SPIR-V failed"); return nil; }
        RedNaviMTLComputePipelineState *p = [RedNaviMTLComputePipelineState new];
        p->c = c; p->nbuf = (unsigned)nbuf; p->push = (unsigned)push; p->pushIndex = (unsigned)nbuf; p->dev = (id<MTLDevice>)self;
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
