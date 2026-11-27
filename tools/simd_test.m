// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors
//
// SIMD-group functions on the default Metal device, checked against the values Metal defines for a 32-thread SIMD group: simd_lane_id, threads_per_simdgroup,
// simdgroup_index_in_threadgroup, simd_sum, simd_shuffle_xor, simd_prefix_exclusive_sum and simd_max.
// Build: clang -fobjc-arc -framework Metal -framework Foundation tools/simd_test.m -o simd_test
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static NSString *const src = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"kernel void k(device uint *o [[buffer(0)]], uint tid [[thread_position_in_grid]], uint lane [[thread_index_in_simdgroup]],\n"
"              uint width [[threads_per_simdgroup]], uint sg [[simdgroup_index_in_threadgroup]]) {\n"
"    uint v = tid + 1;\n"
"    o[tid * 7 + 0] = lane;\n"
"    o[tid * 7 + 1] = width;\n"
"    o[tid * 7 + 2] = sg;\n"
"    o[tid * 7 + 3] = simd_sum(v);\n"
"    o[tid * 7 + 4] = simd_shuffle_xor(v, 5u);\n"
"    o[tid * 7 + 5] = simd_prefix_exclusive_sum(v);\n"
"    o[tid * 7 + 6] = simd_max(v);\n"
"}\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        printf("device %s\n", dev.name.UTF8String);
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
        if (!lib) { printf("library: %s\n", err.description.UTF8String); return 1; }
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&err];
        if (!ps) { printf("pipeline: %s\n", err.description.UTF8String); return 1; }
        printf("threadExecutionWidth %lu\n", (unsigned long)ps.threadExecutionWidth);
        const uint32_t n = 256, tg = 128;
        id<MTLBuffer> b = [dev newBufferWithLength:n * 7 * 4 options:MTLResourceStorageModeShared];
        memset(b.contents, 0xEE, n * 7 * 4);
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:ps];
        [e setBuffer:b offset:0 atIndex:0];
        [e dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const uint32_t *o = b.contents;
        static const char *const names[] = {"lane", "width", "simdgroup", "simd_sum", "shuffle_xor", "prefix_sum", "simd_max"};
        uint32_t bad[7] = {0};
        for (uint32_t t = 0; t < n; t++) {
            const uint32_t lane = t % 32, base = t - lane;
            uint32_t sum = 0, pre = 0;
            for (uint32_t i = 0; i < 32; i++) { sum += base + i + 1; if (i < lane) pre += base + i + 1; }
            const uint32_t want[7] = {lane, 32, (t % tg) / 32, sum, (base + (lane ^ 5u)) + 1, pre, base + 32};
            for (int k = 0; k < 7; k++) if (o[t * 7 + k] != want[k]) { if (bad[k]++ < 3) printf("thread %u %s: got %u want %u\n", t, names[k], o[t * 7 + k], want[k]); }
        }
        int fails = 0;
        for (int k = 0; k < 7; k++) { printf("%-12s %s (%u wrong)\n", names[k], bad[k] ? "WRONG" : "ok", bad[k]); fails += bad[k] != 0; }
        return fails ? 1 : 0;
    }
}
