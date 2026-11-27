// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors
//
// Managed buffers above the small-buffer limit: the bytes given at creation, didModifyRange (whole and partial), a kernel, blits, synchronizeResource, two command buffers in flight.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
static int bad;
static void check(const char *what, const uint32_t *p, size_t n, uint32_t (^want)(size_t)) {
    for (size_t i = 0; i < n; i++) if (p[i] != want(i)) { printf("FAIL %s: [%zu] = %u, want %u\n", what, i, p[i], want(i)); bad++; return; }
    printf("ok   %s\n", what);
}
int main(void) {
    @autoreleasepool {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e = nil;
        id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void add1(device const uint *a [[buffer(0)]], device uint *b [[buffer(1)]], uint i [[thread_position_in_grid]]) { b[i] = a[i] + 1; }" options:nil error:&e];
        id<MTLComputePipelineState> p = lib ? [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"add1"] error:&e] : nil;
        if (!p) { printf("FAIL pipeline: %s\n", e.description.UTF8String); return 1; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        const size_t n = 20u << 20, bytes = n * 4;                  // 80 MB: more than one 32 MB chunk
        uint32_t *init = malloc(bytes); for (size_t i = 0; i < n; i++) init[i] = (uint32_t)i;
        id<MTLBuffer> a = [d newBufferWithBytes:init length:bytes options:MTLResourceStorageModeManaged];
        id<MTLBuffer> b = [d newBufferWithLength:bytes options:MTLResourceStorageModeManaged];
        id<MTLBuffer> c = [d newBufferWithLength:bytes options:MTLResourceStorageModeManaged];
        id<MTLBuffer> s = [d newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        check("new buffer is zero", c.contents, n, ^uint32_t(size_t i) { return 0; });
        check("contents of newBufferWithBytes", a.contents, n, ^uint32_t(size_t i) { return (uint32_t)i; });
        void (^run)(id<MTLCommandBuffer>, id<MTLBuffer>, id<MTLBuffer>) = ^(id<MTLCommandBuffer> cb, id<MTLBuffer> in, id<MTLBuffer> out) {
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder]; [ce setComputePipelineState:p]; [ce setBuffer:in offset:0 atIndex:0]; [ce setBuffer:out offset:0 atIndex:1];
            [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [ce endEncoding];
        };
        id<MTLCommandBuffer> cb = [q commandBuffer]; run(cb, a, b);
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl synchronizeResource:b]; [bl copyFromBuffer:b sourceOffset:0 toBuffer:s destinationOffset:0 size:bytes]; [bl endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        if (cb.error) { printf("FAIL command buffer: %s\n", cb.error.description.UTF8String); bad++; }
        const uint32_t *bp = b.contents;
        check("kernel, synchronizeResource (first contents after)", bp, n, ^uint32_t(size_t i) { return (uint32_t)i + 1; });
        check("blit to a Shared buffer", s.contents, n, ^uint32_t(size_t i) { return (uint32_t)i + 1; });
        uint32_t *ap = a.contents; for (size_t i = 1000; i < 2000; i++) ap[i] = 7; [a didModifyRange:NSMakeRange(4000, 4000)];
        id<MTLCommandBuffer> c1 = [q commandBuffer]; run(c1, a, b);
        bl = [c1 blitCommandEncoder]; [bl synchronizeResource:b]; [bl endEncoding]; [c1 commit];
        id<MTLCommandBuffer> c2 = [q commandBuffer]; run(c2, b, c);
        bl = [c2 blitCommandEncoder]; [bl synchronizeResource:c]; [bl endEncoding]; [c2 commit]; [c2 waitUntilCompleted];
        if (c1.error || c2.error) { printf("FAIL command buffers in flight\n"); bad++; }
        check("partial didModifyRange, cached pointer", bp, n, ^uint32_t(size_t i) { return i >= 1000 && i < 2000 ? 8 : (uint32_t)i + 1; });
        check("second command buffer reads the first", c.contents, n, ^uint32_t(size_t i) { return i >= 1000 && i < 2000 ? 9 : (uint32_t)i + 2; });
        id<MTLCommandBuffer> c3 = [q commandBuffer]; bl = [c3 blitCommandEncoder]; [bl fillBuffer:c range:NSMakeRange(0, bytes) value:0x11]; [bl synchronizeResource:c]; [bl endEncoding]; [c3 commit]; [c3 waitUntilCompleted];
        check("fill, synchronizeResource", c.contents, n, ^uint32_t(size_t i) { return 0x11111111u; });
        printf("%s\n", bad ? "FAILED" : "PASS");
    }
    return bad != 0;
}
