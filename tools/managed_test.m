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
int main(int argc, const char **argv) {
    const int shared = argc > 1 && !strcmp(argv[1], "shared");     // for a driver with one memory for CPU and card: unannounced writes reach the card there
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
        // Two announced ranges with a gap the card wrote and the CPU never read back: the gap must keep the card's bytes.
        {   id<MTLCommandBuffer> g1 = [q commandBuffer]; bl = [g1 blitCommandEncoder]; [bl fillBuffer:b range:NSMakeRange(0, bytes) value:0x22]; [bl endEncoding]; [g1 commit]; [g1 waitUntilCompleted];   // the card's b, not synchronized: the CPU's copy holds other bytes
            uint32_t *w = b.contents;
            for (size_t i = 0; i < 1000; i++) w[i] = 500; for (size_t i = n - 1000; i < n; i++) w[i] = 600;
            for (size_t i = 5000; i < 6000; i++) w[i] = 12345;                                                                // never announced: a driver with one memory for both shows it, one with two does not
            [b didModifyRange:NSMakeRange(0, 4000)]; [b didModifyRange:NSMakeRange(bytes - 4000, 4000)];
            [b didModifyRange:NSMakeRange(2000, 2000)];                                                                       // overlaps the first
            id<MTLCommandBuffer> g2 = [q commandBuffer]; bl = [g2 blitCommandEncoder]; [bl copyFromBuffer:b sourceOffset:0 toBuffer:s destinationOffset:0 size:bytes]; [bl endEncoding]; [g2 commit]; [g2 waitUntilCompleted];
            const uint32_t *got = s.contents;
            check("gap between announced ranges keeps the card's bytes", got, n, ^uint32_t(size_t i) { return i < 1000 ? 500 : i >= n - 1000 ? 600 : shared && i >= 5000 && i < 6000 && got[i] == 12345 ? 12345 : 0x22222222u; });
        }
        // Many small ranges, in no order, and one announced twice.
        {   uint32_t *w = a.contents; for (size_t i = 0; i < n; i++) w[i] = (uint32_t)i * 3;
            for (size_t k = 0; k < 2000; k++) { size_t j = (k * 1237) % 2000; [a didModifyRange:NSMakeRange(j * 8192, 4096)]; [a didModifyRange:NSMakeRange(j * 8192 + 4096, 4096)]; }
            [a didModifyRange:NSMakeRange(0, bytes)]; [a didModifyRange:NSMakeRange(16, 16)];
            id<MTLCommandBuffer> m1 = [q commandBuffer]; run(m1, a, c); bl = [m1 blitCommandEncoder]; [bl synchronizeResource:c]; [bl endEncoding]; [m1 commit]; [m1 waitUntilCompleted];
            check("many ranges, then the whole buffer (more than one chunk)", c.contents, n, ^uint32_t(size_t i) { return (uint32_t)i * 3 + 1; });
        }
        // A texture over a Managed buffer: announced bytes reach a kernel that reads the texture.
        {   id<MTLLibrary> tl = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void rd(texture2d<uint, access::read> t [[texture(0)]], device uint *o [[buffer(0)]], uint2 g [[thread_position_in_grid]]) { o[g.y * 1024 + g.x] = t.read(g).r; }" options:nil error:&e];
            id<MTLComputePipelineState> rp = [d newComputePipelineStateWithFunction:[tl newFunctionWithName:@"rd"] error:&e];
            id<MTLBuffer> m = [d newBufferWithLength:4u << 20 options:MTLResourceStorageModeManaged];
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:1024 height:1024 mipmapped:NO]; td.storageMode = MTLStorageModeManaged;
            id<MTLTexture> tx = [m newTextureWithDescriptor:td offset:0 bytesPerRow:4096];
            for (uint32_t round = 1; round <= 2; round++) {
                uint32_t *w = m.contents; for (size_t i = 0; i < 1u << 20; i++) w[i] = (uint32_t)i * round; [m didModifyRange:NSMakeRange(0, 4u << 20)];
                id<MTLCommandBuffer> t1 = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [t1 computeCommandEncoder]; [ce setComputePipelineState:rp]; [ce setTexture:tx atIndex:0]; [ce setBuffer:s offset:0 atIndex:0];
                [ce dispatchThreads:MTLSizeMake(1024, 1024, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; [ce endEncoding]; [t1 commit]; [t1 waitUntilCompleted];
                check("texture over a Managed buffer", s.contents, 1u << 20, ^uint32_t(size_t i) { return (uint32_t)i * round; });
            }
        }
        // Buffers released with ranges still to be copied, and right after commit.
        for (int k = 0; k < 50; k++) @autoreleasepool {
            id<MTLBuffer> m = [d newBufferWithLength:4u << 20 options:MTLResourceStorageModeManaged];
            memset(m.contents, k, 4u << 20); [m didModifyRange:NSMakeRange(0, 4u << 20)];
            if (k & 1) { id<MTLCommandBuffer> r1 = [q commandBuffer]; bl = [r1 blitCommandEncoder]; [bl copyFromBuffer:m sourceOffset:0 toBuffer:s destinationOffset:0 size:4u << 20]; [bl endEncoding]; [r1 commit]; }
        }
        {   id<MTLCommandBuffer> r2 = [q commandBuffer]; [r2 commit]; [r2 waitUntilCompleted];
            check("released with ranges pending", s.contents, 1u << 20, ^uint32_t(size_t i) { return 0x31313131u; });
        }
        printf("%s\n", bad ? "FAILED" : "PASS");
    }
    return bad != 0;
}
