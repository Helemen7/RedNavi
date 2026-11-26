// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors
//
// Self-checking Metal test of the features Geekbench's failing GPU workloads share: device and threadgroup atomics (append counters,
// histograms, min/max), counters cleared with a blit fill in private and managed buffers, and round/sin/cos. The kernels are Metal source,
// compiled with newLibraryWithSource: like Geekbench does (on the RedNavi device this needs RN_METAL_SRCLIB=1).
// Build: clang -fobjc-arc -framework Foundation -framework Metal tools/atomic_test.m -o atomic_test
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>

static const char *src =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"kernel void k_append(device atomic_uint *counter [[buffer(0)]], device uint *out [[buffer(1)]], constant uint &n [[buffer(2)]], uint gid [[thread_position_in_grid]]) {\n"
"  if (gid >= n || (gid & 1)) return;\n"
"  uint slot = atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);\n"
"  out[slot] = gid;\n"
"}\n"
"kernel void k_hist(device const uchar *in [[buffer(0)]], device atomic_uint *bins [[buffer(1)]], constant uint &n [[buffer(2)]], uint gid [[thread_position_in_grid]]) {\n"
"  if (gid < n) atomic_fetch_add_explicit(&bins[in[gid]], 1, memory_order_relaxed);\n"
"}\n"
"kernel void k_hist_local(device const uchar *in [[buffer(0)]], device atomic_int *bins [[buffer(1)]], constant uint &n [[buffer(2)]],\n"
"                         uint gid [[thread_position_in_grid]], uint lid [[thread_position_in_threadgroup]], uint tgs [[threads_per_threadgroup]]) {\n"
"  threadgroup atomic_int local[256];\n"
"  for (uint i = lid; i < 256; i += tgs) atomic_store_explicit(&local[i], 0, memory_order_relaxed);\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  if (gid < n) atomic_fetch_add_explicit(&local[in[gid]], 1, memory_order_relaxed);\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  for (uint i = lid; i < 256; i += tgs) atomic_fetch_add_explicit(&bins[i], atomic_load_explicit(&local[i], memory_order_relaxed), memory_order_relaxed);\n"
"}\n"
"kernel void k_minmax(device const uint *in [[buffer(0)]], device atomic_uint *mm [[buffer(1)]], device atomic_int *smax [[buffer(2)]], constant uint &n [[buffer(3)]],\n"
"                     uint gid [[thread_position_in_grid]], uint lid [[thread_position_in_threadgroup]]) {\n"
"  threadgroup atomic_int tmax;\n"
"  if (lid == 0) atomic_store_explicit(&tmax, -2147483647, memory_order_relaxed);\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  if (gid < n) {\n"
"    atomic_fetch_min_explicit(&mm[0], in[gid], memory_order_relaxed);\n"
"    atomic_fetch_max_explicit(&mm[1], in[gid], memory_order_relaxed);\n"
"    atomic_fetch_max_explicit(&tmax, int(in[gid]) - 500000, memory_order_relaxed);\n"
"  }\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  if (lid == 0) atomic_fetch_max_explicit(smax, atomic_load_explicit(&tmax, memory_order_relaxed), memory_order_relaxed);\n"
"}\n"
"kernel void k_copy(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]], device atomic_uint *counter [[buffer(2)]], uint gid [[thread_position_in_grid]]) {\n"
"  out[gid] = in[gid] + 1;\n"
"  atomic_fetch_add_explicit(counter, in[gid] & 1, memory_order_relaxed);\n"
"}\n"
"kernel void k_lds(device uint *out [[buffer(0)]], device atomic_uint *cnt [[buffer(1)]], uint gid [[thread_position_in_grid]], uint lid [[thread_position_in_threadgroup]]) {\n"
"  threadgroup atomic_uint c;\n"
"  if (lid == 0) atomic_store_explicit(&c, 0, memory_order_relaxed);\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  out[gid] = atomic_fetch_add_explicit(&c, 1, memory_order_relaxed);\n"
"  atomic_exchange_explicit(&cnt[1], 77u, memory_order_relaxed);\n"
"  uint e = 5; atomic_compare_exchange_weak_explicit(&cnt[2], &e, 99u, memory_order_relaxed, memory_order_relaxed);\n"
"  atomic_fetch_add_explicit(&cnt[0], 1, memory_order_relaxed);\n"
"}\n"
"kernel void k_math(device const float *in [[buffer(0)]], device float *out [[buffer(1)]], uint gid [[thread_position_in_grid]]) {\n"
"  float x = in[gid];\n"
"  out[gid * 4 + 0] = round(x);\n"
"  out[gid * 4 + 1] = sin(x);\n"
"  out[gid * 4 + 2] = cos(x);\n"
"  out[gid * 4 + 3] = float(int(round(x * 3.0f)));\n"
"}\n"
"kernel void k_clear(device uint *a [[buffer(0)]], uint gid [[thread_position_in_grid]]) { a[gid] = 0; }\n"
"kernel void k_bump(device atomic_uint *a [[buffer(0)]], uint gid [[thread_position_in_grid]]) {\n"
"  atomic_fetch_add_explicit(&a[gid & 255], 1, memory_order_relaxed);\n"
"  atomic_fetch_max_explicit(&a[256 + (gid & 15)], gid, memory_order_relaxed);\n"
"}\n"
"kernel void k_read(device const uint *a [[buffer(0)]], device uint *o [[buffer(1)]], uint gid [[thread_position_in_grid]]) { o[gid] = a[gid]; }\n"
"kernel void k_bh(device const uchar4 *src [[buffer(0)]], device atomic_int *hist [[buffer(1)]], constant int &w [[buffer(2)]], constant int &h [[buffer(3)]],\n"
"                 threadgroup atomic_int *lh [[threadgroup(0)]], uint2 gid [[thread_position_in_grid]], uint2 lid [[thread_position_in_threadgroup]], uint2 bs [[threads_per_threadgroup]]) {\n"
"  int n = int(bs.x * bs.y), t = int(bs.x * lid.y + lid.x);\n"
"  for (int i = t; i < 768; i += n) atomic_store_explicit(&lh[i], 0, memory_order_relaxed);\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  if (int(gid.x) < w && int(gid.y) < h) { uchar4 v = src[gid.y * w + gid.x];\n"
"    atomic_fetch_add_explicit(&lh[v.x], 1, memory_order_relaxed); atomic_fetch_add_explicit(&lh[256 + v.y], 1, memory_order_relaxed); atomic_fetch_add_explicit(&lh[512 + v.z], 1, memory_order_relaxed); }\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  for (int i = t; i < 768; i += n) atomic_fetch_add_explicit(&hist[i], atomic_load_explicit(&lh[i], memory_order_relaxed), memory_order_relaxed);\n"
"}\n";

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

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
        const uint32_t N = 1 << 20;
        srand(7);
        uint8_t *bytes = malloc(N); for (uint32_t i = 0; i < N; i++) bytes[i] = (uint8_t)(rand() % 251);
        uint32_t *words = malloc(N * 4); for (uint32_t i = 0; i < N; i++) words[i] = 1000 + (uint32_t)(rand() % 1000000);

        // 1. append counter in a private buffer cleared with a blit fill, read back through a blit copy
        for (int mode = 0; mode < 3; mode++) {
            MTLResourceOptions o = mode == 0 ? MTLResourceStorageModePrivate : mode == 1 ? MTLResourceStorageModeManaged : MTLResourceStorageModeShared;
            id<MTLBuffer> counter = [dev newBufferWithLength:4 options:o];
            id<MTLBuffer> out = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> rb = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
            memset(out.contents, 0xff, N * 4);
            if (mode == 2) *(uint32_t *)counter.contents = 12345;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder]; [b fillBuffer:counter range:NSMakeRange(0, 4) value:0]; [b endEncoding];
            id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
            [c setComputePipelineState:pso(@"k_append")];
            [c setBuffer:counter offset:0 atIndex:0]; [c setBuffer:out offset:0 atIndex:1]; [c setBytes:&N length:4 atIndex:2];
            [c dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)]; [c endEncoding];
            b = [cb blitCommandEncoder]; [b copyFromBuffer:counter sourceOffset:0 toBuffer:rb destinationOffset:0 size:4];
            if (mode == 1) [b synchronizeResource:counter];
            [b endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            uint32_t got = *(uint32_t *)rb.contents;
            CHECK(got == N / 2, "append (%s): counter %u, expected %u", mode == 0 ? "private" : mode == 1 ? "managed" : "shared", got, N / 2);
            if (mode != 0) { uint32_t direct = *(uint32_t *)counter.contents; CHECK(direct == N / 2, "append (%s): counter read from contents %u, expected %u", mode == 1 ? "managed" : "shared", direct, N / 2); }
            uint8_t *seen = calloc(N, 1); int bad = 0; const uint32_t *ov = out.contents;
            for (uint32_t i = 0; i < N / 2; i++) { uint32_t v = ov[i]; if (v >= N || (v & 1) || seen[v]++) bad++; }
            CHECK(bad == 0, "append (%s): %d bad slots", mode == 0 ? "private" : mode == 1 ? "managed" : "shared", bad);
            free(seen);
        }

        // 2. histogram, device atomics and threadgroup atomics
        uint32_t ref[256] = {0}; for (uint32_t i = 0; i < N; i++) ref[bytes[i]]++;
        id<MTLBuffer> in8 = [dev newBufferWithBytes:bytes length:N options:MTLResourceStorageModeShared];
        for (int k = 0; k < 2; k++) {
            id<MTLBuffer> bins = [dev newBufferWithLength:1024 options:MTLResourceStorageModeShared];
            memset(bins.contents, 0, 1024);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
            [c setComputePipelineState:pso(k ? @"k_hist_local" : @"k_hist")];
            [c setBuffer:in8 offset:0 atIndex:0]; [c setBuffer:bins offset:0 atIndex:1]; [c setBytes:&N length:4 atIndex:2];
            [c dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)]; [c endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            int bad = 0; const uint32_t *bv = bins.contents; for (int i = 0; i < 256; i++) if (bv[i] != ref[i]) { if (bad < 3) printf("  bin %d: %u, expected %u\n", i, bv[i], ref[i]); bad++; }
            CHECK(bad == 0, "%s: %d wrong bins", k ? "threadgroup histogram" : "device histogram", bad);
        }

        // 3. min/max, unsigned device atomics, signed threadgroup and device max
        {
            uint32_t mn = UINT32_MAX, mx = 0; for (uint32_t i = 0; i < N; i++) { if (words[i] < mn) mn = words[i]; if (words[i] > mx) mx = words[i]; }
            id<MTLBuffer> in32 = [dev newBufferWithBytes:words length:N * 4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> mm = [dev newBufferWithLength:8 options:MTLResourceStorageModeShared];
            id<MTLBuffer> smax = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
            ((uint32_t *)mm.contents)[0] = UINT32_MAX; ((uint32_t *)mm.contents)[1] = 0; *(int32_t *)smax.contents = INT32_MIN + 1;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
            [c setComputePipelineState:pso(@"k_minmax")];
            [c setBuffer:in32 offset:0 atIndex:0]; [c setBuffer:mm offset:0 atIndex:1]; [c setBuffer:smax offset:0 atIndex:2]; [c setBytes:&N length:4 atIndex:3];
            [c dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)]; [c endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            const uint32_t *m = mm.contents;
            CHECK(m[0] == mn && m[1] == mx, "unsigned min/max: %u %u, expected %u %u", m[0], m[1], mn, mx);
            CHECK(*(int32_t *)smax.contents == (int32_t)mx - 500000, "signed max: %d, expected %d", *(int32_t *)smax.contents, (int32_t)mx - 500000);
        }

        // 4. round, sin, cos
        {
            const uint32_t M = 4096; float *x = malloc(M * 4);
            for (uint32_t i = 0; i < M; i++) x[i] = ((float)i - M / 2) * 0.25f + (i % 7 == 0 ? 0.5f : 0.0f) - (i % 11 == 0 ? 0.5f : 0.0f);
            id<MTLBuffer> in = [dev newBufferWithBytes:x length:M * 4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> out = [dev newBufferWithLength:M * 16 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
            [c setComputePipelineState:pso(@"k_math")];
            [c setBuffer:in offset:0 atIndex:0]; [c setBuffer:out offset:0 atIndex:1];
            [c dispatchThreads:MTLSizeMake(M, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [c endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            const float *o = out.contents; int br = 0, bs = 0, bi = 0;
            for (uint32_t i = 0; i < M; i++) {
                if (o[i * 4] != roundf(x[i])) { if (br < 3) printf("  round(%g) = %g\n", x[i], o[i * 4]); br++; }
                if (fabsf(o[i * 4 + 1] - sinf(x[i])) > 1e-3f || fabsf(o[i * 4 + 2] - cosf(x[i])) > 1e-3f) { if (bs < 3) printf("  sin/cos(%g) = %g %g\n", x[i], o[i * 4 + 1], o[i * 4 + 2]); bs++; }
                if (o[i * 4 + 3] != (float)(int)roundf(x[i] * 3.0f)) { if (bi < 3) printf("  int(round(%g)) = %g\n", x[i] * 3.0f, o[i * 4 + 3]); bi++; }
            }
            CHECK(br == 0, "round: %d wrong", br);
            CHECK(bs == 0, "sin/cos: %d off by more than 1e-3", bs);
            CHECK(bi == 0, "int(round): %d wrong", bi);
        }
        // 0. threadgroup atomics with the old values stored plainly; device exchange, compare-exchange, add with the result unused
        {
            const uint32_t M = 4096;
            id<MTLBuffer> out = [dev newBufferWithLength:M * 4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> cnt = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
            uint32_t *cv = cnt.contents; cv[0] = 0; cv[1] = 0; cv[2] = 5;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
            [c setComputePipelineState:pso(@"k_lds")];
            [c setBuffer:out offset:0 atIndex:0]; [c setBuffer:cnt offset:0 atIndex:1];
            [c dispatchThreads:MTLSizeMake(M, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)]; [c endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            const uint32_t *ov = out.contents; int bad = 0;
            for (uint32_t g = 0; g < M / 256; g++) { uint8_t seen[256] = {0}; for (uint32_t i = 0; i < 256; i++) { uint32_t v = ov[g * 256 + i]; if (v >= 256 || seen[v]++) bad++; } }
            if (bad) printf("  threadgroup old values: %u %u %u %u ... %u\n", ov[0], ov[1], ov[2], ov[3], ov[255]);
            CHECK(bad == 0, "threadgroup atomic old values: %d wrong", bad);
            CHECK(cv[0] == M, "device add, result unused: %u, expected %u", cv[0], M);
            CHECK(cv[1] == 77, "device exchange: %u, expected 77", cv[1]);
            CHECK(cv[2] == 99, "device compare-exchange: %u, expected 99", cv[2]);
        }
        // 5. reuse: the CPU rewrites the same shared buffers between command buffers (a counter it resets, input it changes); the GPU must see the new data each time
        {
            const uint32_t M = 65536;
            id<MTLBuffer> in = [dev newBufferWithLength:M * 4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> out = [dev newBufferWithLength:M * 4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> counter = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
            id<MTLComputePipelineState> p = pso(@"k_copy");
            int bad = 0;
            for (uint32_t round = 0; round < 8; round++) {
                uint32_t *iv = in.contents, odd = 0;
                for (uint32_t i = 0; i < M; i++) { iv[i] = i * 7 + round * 1000003u; odd += iv[i] & 1; }
                *(uint32_t *)counter.contents = round;
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
                [c setComputePipelineState:p];
                [c setBuffer:in offset:0 atIndex:0]; [c setBuffer:out offset:0 atIndex:1]; [c setBuffer:counter offset:0 atIndex:2];
                [c dispatchThreads:MTLSizeMake(M, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)]; [c endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                const uint32_t *ov = out.contents; int wrong = 0;
                for (uint32_t i = 0; i < M; i++) if (ov[i] != iv[i] + 1) wrong++;
                if (wrong) { if (bad < 3) printf("  round %u: %d stale outputs\n", round, wrong); bad++; }
                if (*(uint32_t *)counter.contents != round + odd) { if (bad < 3) printf("  round %u: counter %u, expected %u\n", round, *(uint32_t *)counter.contents, round + odd); bad++; }
            }
            CHECK(bad == 0, "reuse: %d rounds wrong", bad);
        }
        // 6. GPU-only chain in one command buffer: a kernel clears with plain stores, the next does device atomics, a third reads with plain loads
        for (int mode = 0; mode < 2; mode++) {
            const uint32_t M = 65536;
            id<MTLBuffer> a = [dev newBufferWithLength:272 * 4 options:mode ? MTLResourceStorageModePrivate : MTLResourceStorageModeShared];
            id<MTLBuffer> o = [dev newBufferWithLength:272 * 4 options:MTLResourceStorageModeShared];
            memset(o.contents, 0xee, 272 * 4);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
            [c setComputePipelineState:pso(@"k_clear")]; [c setBuffer:a offset:0 atIndex:0];
            [c dispatchThreads:MTLSizeMake(272, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)];
            [c memoryBarrierWithScope:MTLBarrierScopeBuffers];
            [c setComputePipelineState:pso(@"k_bump")];
            [c dispatchThreads:MTLSizeMake(M, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [c memoryBarrierWithScope:MTLBarrierScopeBuffers];
            [c setComputePipelineState:pso(@"k_read")]; [c setBuffer:o offset:0 atIndex:1];
            [c dispatchThreads:MTLSizeMake(272, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)]; [c endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            const uint32_t *ov = o.contents; int bad = 0;
            for (int i = 0; i < 256; i++) if (ov[i] != M / 256) { if (bad < 3) printf("  chain %d: bin %d = %u\n", mode, i, ov[i]); bad++; }
            for (int i = 0; i < 16; i++) if (ov[256 + i] != M - 16 + (uint32_t)i) { if (bad < 3) printf("  chain %d: max %d = %u\n", mode, i, ov[256 + i]); bad++; }
            CHECK(bad == 0, "GPU-only chain (%s): %d wrong", mode ? "private" : "shared", bad);
        }
        // 7. the shape of a RAW-processing histogram kernel: threadgroup histogram, then 768 device adds per threadgroup; timed
        for (int run = 0; run < 4; run++) {
            const int sz = run < 3 ? 512 << run : 2048, priv = run == 3;   // the last run keeps the histogram in a private buffer (VRAM)
            const uint32_t P = (uint32_t)(sz * sz);
            id<MTLBuffer> src4 = [dev newBufferWithLength:P * 4 options:MTLResourceStorageModeShared];
            uint8_t *sp = src4.contents; for (uint32_t i = 0; i < P * 4; i++) sp[i] = (uint8_t)(i * 2654435761u >> 24);
            id<MTLBuffer> hist = [dev newBufferWithLength:768 * 4 options:priv ? MTLResourceStorageModePrivate : MTLResourceStorageModeShared];
            id<MTLBuffer> hrb = priv ? [dev newBufferWithLength:768 * 4 options:MTLResourceStorageModeShared] : hist;
            if (!priv) memset(hist.contents, 0, 768 * 4);
            int w = sz, h = sz;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            if (priv) { id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl fillBuffer:hist range:NSMakeRange(0, 768 * 4) value:0]; [bl endEncoding]; }
            id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
            [c setComputePipelineState:pso(@"k_bh")];
            [c setBuffer:src4 offset:0 atIndex:0]; [c setBuffer:hist offset:0 atIndex:1];
            [c setBytes:&w length:4 atIndex:2]; [c setBytes:&h length:4 atIndex:3];
            [c setThreadgroupMemoryLength:768 * 4 atIndex:0];
            [c dispatchThreads:MTLSizeMake(sz, sz, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)]; [c endEncoding];
            if (priv) { id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl copyFromBuffer:hist sourceOffset:0 toBuffer:hrb destinationOffset:0 size:768 * 4]; [bl endEncoding]; }
            NSDate *t0 = [NSDate date];
            [cb commit]; [cb waitUntilCompleted];
            double ms = -[t0 timeIntervalSinceNow] * 1000.0;
            const int32_t *hv = hrb.contents; long long tot = 0; for (int i = 0; i < 768; i++) tot += hv[i];
            long long ch[3] = {0, 0, 0}; for (int i = 0; i < 768; i++) ch[i / 256] += hv[i];
            if (tot != 3LL * P) printf("  channel sums %lld %lld %lld (each expected %u)\n", ch[0], ch[1], ch[2], P);
            printf("  histogram %dx%d%s: %.1f ms, %u device adds, total %lld (expected %u), status %ld\n", sz, sz, priv ? " (private)" : "", ms, P / 256 * 768, tot, 3 * P, (long)cb.status);
            CHECK(tot == 3LL * P, "histogram %dx%d: total %lld, expected %u", sz, sz, tot, 3 * P);
        }
        printf(fails ? "%d checks failed\n" : "all checks passed\n", fails);
    }
    return fails != 0;
}
