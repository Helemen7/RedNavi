// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors
//
// Separates the card's arithmetic rate from its memory rates: a kernel of fused multiply-adds with no memory traffic, a copy kernel between buffers of each storage mode, and a
// device atomic add on each. Times are wall clock around commit and waitUntilCompleted, over work long enough that the submit does not count.
// Build: clang -fobjc-arc -framework Foundation -framework Metal tools/gpu_bench.m -o gpu_bench   (on the RedNavi device: RN_METAL_COMPUTE=1 RN_METAL_SRCLIB=1)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>

static const char *src =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"kernel void k_alu(device float *out [[buffer(0)]], constant uint &n [[buffer(1)]], uint gid [[thread_position_in_grid]]) {\n"
"  float4 a = float4(gid & 255) * 1e-3f + float4(0.1f, 0.2f, 0.3f, 0.4f), b = a + 0.5f;\n"
"  for (uint i = 0; i < n; i++) { a = fma(a, float4(0.999f), b); b = fma(b, float4(0.998f), a * 1e-4f); }\n"
"  out[gid] = dot(a + b, float4(1.0f)); }\n"
"kernel void k_copy(device const uint4 *in [[buffer(0)]], device uint4 *out [[buffer(1)]], uint gid [[thread_position_in_grid]]) { out[gid] = in[gid]; }\n"
"kernel void k_add(device atomic_uint *a [[buffer(0)]], uint gid [[thread_position_in_grid]]) { atomic_fetch_add_explicit(&a[gid & 255], 1, memory_order_relaxed); }\n";

static double seconds(uint64_t t) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return (double)t * tb.numer / tb.denom / 1e9; }

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        printf("device: %s\n", dev.name.UTF8String);
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:nil error:&err];
        if (!lib) { printf("library: %s\n", err.description.UTF8String); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLComputePipelineState> (^pso)(NSString *) = ^(NSString *n) {
            NSError *e = nil; id<MTLComputePipelineState> p = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:n] error:&e];
            if (!p) { printf("pipeline %s: %s\n", n.UTF8String, e.description.UTF8String); exit(1); }
            return p;
        };
        double (^run)(id<MTLComputePipelineState>, NSArray *, NSUInteger, uint32_t, int) = ^double(id<MTLComputePipelineState> p, NSArray *bufs, NSUInteger threads, uint32_t arg, int times) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
            [c setComputePipelineState:p];
            for (NSUInteger i = 0; i < bufs.count; i++) [c setBuffer:bufs[i] offset:0 atIndex:i];
            if (arg) [c setBytes:&arg length:4 atIndex:bufs.count];
            for (int i = 0; i < times; i++) [c dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [c endEncoding];
            const uint64_t t0 = mach_absolute_time();
            [cb commit]; [cb waitUntilCompleted];
            const double s = seconds(mach_absolute_time() - t0);
            if (cb.status != MTLCommandBufferStatusCompleted) printf("  command buffer failed: %s\n", cb.error.description.UTF8String);
            return s;
        };
        const NSUInteger T = 1 << 20; const uint32_t iter = 8192;
        id<MTLBuffer> out = [dev newBufferWithLength:T * 4 options:MTLResourceStorageModePrivate];
        id<MTLComputePipelineState> alu = pso(@"k_alu"), copy = pso(@"k_copy"), add = pso(@"k_add");
        run(alu, @[out], T, 16, 1);
        for (int r = 0; r < 3; r++) { const double s = run(alu, @[out], T, iter, 4); printf("alu: %.1f ms, %.2f TFLOP/s\n", s * 1e3, 4.0 * T * iter * 16 / s / 1e12); }
        const NSUInteger bytes = 64u << 20;
        const char *names[3] = {"private", "managed", "shared"};
        const MTLResourceOptions opts[3] = {MTLResourceStorageModePrivate, MTLResourceStorageModeManaged, MTLResourceStorageModeShared};
        id<MTLBuffer> a[3], b[3];
        for (int m = 0; m < 3; m++) { a[m] = [dev newBufferWithLength:bytes options:opts[m]]; b[m] = [dev newBufferWithLength:bytes options:opts[m]]; }
        for (int from = 0; from < 3; from++)
            for (int to = 0; to < 3; to++) {
                run(copy, @[a[from], b[to]], bytes / 16, 0, 1);
                const double s = run(copy, @[a[from], b[to]], bytes / 16, 0, 8);
                printf("copy %s to %s: %.1f GB/s (read plus write)\n", names[from], names[to], 2.0 * bytes * 8 / s / 1e9);
            }
        for (int m = 0; m < 3; m++) {
            id<MTLBuffer> c = [dev newBufferWithLength:1024 options:opts[m]];
            const double s = run(add, @[c], T, 0, 4);
            printf("atomic add, %s: %.1f million/s\n", names[m], 4.0 * T / s / 1e6);
        }
    }
    return 0;
}
