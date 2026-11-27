// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors
//
// Textures made inside buffers and buffers of an automatic heap: contents seen by the CPU and by kernels.
// clang -fobjc-arc -framework Metal -framework Foundation tools/buftex_test.m -o buftex_test
#import <Metal/Metal.h>
static int bad;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) bad++; }
enum { W = 67, H = 41, BPR = 512 };
static void fill(uint8_t *p, int seed) { for (int y = 0; y < H; y++) for (int x = 0; x < BPR; x++) p[y * BPR + x] = (uint8_t)(x * 3 + y * 7 + seed); }
int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void rd(texture2d<float, access::read> t [[texture(0)]], device uint *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) { o[p.y * 67 + p.x] = pack_float_to_unorm4x8(t.read(p)); }\n"
        "kernel void wr(texture2d<float, access::write> t [[texture(0)]], uint2 p [[thread_position_in_grid]]) { t.write(float4(p.x / 255.0, p.y / 255.0, 0.2, 1.0), p); }\n"
        options:nil error:&e];
    if (!lib) { printf("FAIL library: %s\n", e.description.UTF8String); return 1; }
    id<MTLComputePipelineState> rd = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rd"] error:&e], wr = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"wr"] error:&e];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLBuffer> out = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    void (^run)(id<MTLComputePipelineState>, id<MTLTexture>) = ^(id<MTLComputePipelineState> p, id<MTLTexture> t) {
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
        [c setComputePipelineState:p]; [c setTexture:t atIndex:0]; if (p == rd) [c setBuffer:out offset:0 atIndex:0];
        [c dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; [c endEncoding]; [cb commit]; [cb waitUntilCompleted];
    };
    int (^same)(const uint8_t *, const uint8_t *) = ^int(const uint8_t *a, const uint8_t *b) { for (int y = 0; y < H; y++) if (memcmp(a + y * BPR, b + y * W * 4, W * 4)) return 0; return 1; };
    static uint8_t got[H * BPR], keep[H * BPR];
    for (int round = 0; round < 3; round++) {       // later rounds can reuse the images of earlier ones
        @autoreleasepool {
            id<MTLBuffer> b = [dev newBufferWithLength:H * BPR options:MTLResourceStorageModeShared];
            if (round < 2) fill(b.contents, round + 1);             // the last round: all zeros
            id<MTLTexture> t = [b newTextureWithDescriptor:d offset:0 bytesPerRow:BPR];
            [t getBytes:got bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
            check(same(b.contents, got), "getBytes before any GPU use");
            id<MTLTexture> t2 = [b newTextureWithDescriptor:d offset:0 bytesPerRow:BPR];
            run(rd, t2);
            check(same(b.contents, out.contents), "kernel reads a new texture");
            if (round < 2) fill(b.contents, round + 9);
            run(rd, t2);
            check(same(b.contents, out.contents), "kernel reads after the buffer changed");
            id<MTLTexture> t3 = [b newTextureWithDescriptor:d offset:0 bytesPerRow:BPR];
            memcpy(keep, b.contents, H * BPR);
            uint8_t patch[5 * 7 * 4]; memset(patch, 0xab, sizeof(patch));
            [t3 replaceRegion:MTLRegionMake2D(3, 4, 7, 5) mipmapLevel:0 withBytes:patch bytesPerRow:7 * 4];
            for (int y = 0; y < 5; y++) memset(keep + (y + 4) * BPR + 12, 0xab, 28);
            check(!memcmp(keep, b.contents, H * BPR), "partial replaceRegion on a new texture keeps the rest of the buffer");
            run(rd, t3);
            check(same(b.contents, out.contents), "kernel reads after replaceRegion");
            id<MTLTexture> t4 = [b newTextureWithDescriptor:d offset:0 bytesPerRow:BPR];
            memcpy(keep, b.contents, H * BPR);
            run(wr, t4);
            const uint8_t *p = b.contents; int ok = 1;
            for (int y = 0; y < H; y++) { for (int x = 0; x < W; x++) ok &= p[y * BPR + x * 4] == x && p[y * BPR + x * 4 + 1] == y; ok &= !memcmp(p + y * BPR + W * 4, keep + y * BPR + W * 4, BPR - W * 4); }
            check(ok, "kernel writes a new texture, gaps kept");
        }
    }
    if ([dev.name containsString:@"RADV"]) {      // a Shared heap: not allowed by Apple's drivers on an Intel Mac
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new]; hd.size = 64 << 20; hd.storageMode = MTLStorageModeShared;
    id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
    NSMutableDictionary<NSNumber *, id<MTLBuffer>> *live = [NSMutableDictionary dictionary];
    srand(1); int ok = heap != nil, made = 0;
    for (int i = 0; ok && i < 60000; i++) {
        @autoreleasepool {
            NSNumber *k = @(rand() % 3000);
            id<MTLBuffer> old = live[k];
            if (old) {
                const uint32_t *p = old.contents;
                for (NSUInteger j = 0; j < old.length / 4; j++) if (p[j] != k.unsignedIntValue) { ok = 0; break; }
                if (rand() % 4 == 0) [old makeAliasable];
                [live removeObjectForKey:k]; old = nil;
            } else {
                id<MTLBuffer> b = [heap newBufferWithLength:4 * (1 + rand() % 2000) options:MTLResourceStorageModeShared];
                if (b) { uint32_t *p = b.contents; for (NSUInteger j = 0; j < b.length / 4; j++) p[j] = k.unsignedIntValue; live[k] = b; made++; }
            }
        }
    }
    printf("heap: %d buffers made, %lu alive, used %lu\n", made, (unsigned long)live.count, (unsigned long)heap.usedSize);
    check(ok && made > 20000, "heap buffers do not overlap");
    }
    printf("%s (%d failed)\n", bad ? "FAILED" : "ALL PASS", bad);
    return bad != 0;
} }
