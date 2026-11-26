// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// MPS on the RedNavi device: a Gaussian blur through MetalPerformanceShaders, as WindowServer does for desktop window effects. Same arguments as ci_test.
// (derived from ci_test.m) CoreImage on the RedNavi device: a CIContext made on the plug-in's MTLDevice renders a filtered image (what iconservicesagent, mediaanalysisd and others do all day). CoreImage builds
// Metal programs (stitched libraries, compute pipelines) at the first render; before the plug-in answered those it crashed inside Metal. Prints the pixel it got; the log of the plug-in
// shows what CoreImage asked for. usage: ci_test <RedNaviMTLDriver.bundle|system>   (RN_METAL_COMPUTE=1 or the files of docs/TEST_MACHINES.md, RN_VK_DIR=..., DYLD_LIBRARY_PATH=...)
#import <Cocoa/Cocoa.h>
#import <CoreImage/CoreImage.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <IOKit/IOKitLib.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

static void on_crash(int sig, siginfo_t *si, void *uc) {
    char b[128]; int n = snprintf(b, sizeof(b), "\nSIGNAL %d at address %p\n", sig, si->si_addr); write(2, b, n);
    void *bt[30]; int nb = backtrace(bt, 30); backtrace_symbols_fd(bt, nb, 2); _exit(139);
}
int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        static char alt[65536]; stack_t ss = {alt, sizeof(alt), 0}; sigaltstack(&ss, NULL);
        struct sigaction sa = {0}; sa.sa_sigaction = on_crash; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
        id<MTLDevice> dev = nil;
        if (argc < 2 || !strcmp(argv[1], "system")) dev = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *b = [NSBundle bundleWithPath:@(argv[1])];
            if (![b load]) { puts("bundle load failed"); return 3; }
            Class k = objc_getClass("RedNaviMTLDevice");
            io_service_t svc = 0; io_iterator_t it; io_object_t o;
            if (k && IORegistryCreateIterator(kIOMainPortDefault, kIOServicePlane, kIORegistryIterateRecursively, &it) == KERN_SUCCESS)
                while (!svc && (o = IOIteratorNext(it))) { if (IOObjectConformsTo(o, "IOAccelerator")) svc = o; else IOObjectRelease(o); }
            if (!k || !svc) { puts("no RedNaviMTLDevice or accelerator"); return 3; }
            id d = ((id (*)(id, SEL))objc_msgSend)((id)k, sel_registerName("alloc"));
            dev = ((id (*)(id, SEL, unsigned))objc_msgSend)(d, sel_registerName("initWithAcceleratorPort:"), svc);
        }
        if (!dev) { puts("no device"); return 4; }
        printf("device %s (%s)\n", dev.name.UTF8String, class_getName(object_getClass(dev)));
        id<MTLCommandQueue> q = [dev newCommandQueue];
        printf("queue %p\n", q);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModeManaged;
        id<MTLTexture> a = [dev newTextureWithDescriptor:td];
        id<MTLTexture> b2 = [dev newTextureWithDescriptor:td];
        printf("textures %p %p\n", a, b2);
        // the source: 8-pixel squares of white and blue, so the blur has edges to soften
        static uint8_t src[64 * 64 * 4], dst[64 * 64 * 4];
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
            uint8_t *p = src + (y * 64 + x) * 4; int w = ((x >> 3) ^ (y >> 3)) & 1;
            p[0] = 255; p[1] = w ? 255 : 0; p[2] = w ? 255 : 0; p[3] = 255;   // BGRA
        }
        [a replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:src bytesPerRow:64 * 4];
        MPSImageGaussianBlur *blur = [[MPSImageGaussianBlur alloc] initWithDevice:dev sigma:4.0f];
        printf("blur %p\n", blur);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        [blur encodeToCommandBuffer:cb sourceTexture:a destinationTexture:b2];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl synchronizeResource:b2]; [bl endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        printf("status %ld\n", (long)cb.status);
        [b2 getBytes:dst bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
        // compare these with the same program on a Mac with a supported GPU: ./mps_test system
        const int at[][2] = {{4, 4}, {8, 4}, {7, 7}, {12, 3}, {32, 32}, {36, 33}};
        for (unsigned i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
            const uint8_t *p = dst + (at[i][1] * 64 + at[i][0]) * 4;
            printf("pixel (%d,%d) BGRA = %u %u %u %u\n", at[i][0], at[i][1], p[0], p[1], p[2], p[3]);
        }
        // A reference on the CPU: a separable Gaussian of sigma 4 with zero outside the texture (MPSImageEdgeModeZero, MPS's default) (MPS's kernel differs a little, so a texel counts as wrong only when it is more than 6 away).
        // The rows with wrong texels are listed: a whole picture that is off points at the filter, scattered rows or bands at how the work is split up.
        {
            const int R = 12; float k[2 * 12 + 1], ks = 0;
            for (int i = -R; i <= R; i++) { k[i + R] = expf(-(float)(i * i) / (2 * 16.0f)); ks += k[i + R]; }
            static float tmp[64 * 64 * 4];
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) for (int c = 0; c < 4; c++) {
                float s = 0; for (int i = -R; i <= R; i++) { int xx = x + i; if (xx >= 0 && xx < 64) s += k[i + R] * src[(y * 64 + xx) * 4 + c]; }
                tmp[(y * 64 + x) * 4 + c] = s / ks;
            }
            int bad = 0, rowsBad = 0;
            for (int y = 0; y < 64; y++) {
                int rb = 0, worst = 0;
                for (int x = 0; x < 64; x++) for (int c = 0; c < 4; c++) {
                    float s = 0; for (int i = -R; i <= R; i++) { int yy = y + i; if (yy >= 0 && yy < 64) s += k[i + R] * tmp[(yy * 64 + x) * 4 + c]; }
                    const int d = abs((int)lroundf(s / ks) - (int)dst[(y * 64 + x) * 4 + c]);
                    if (d > 6) { rb++; if (d > worst) worst = d; }
                }
                if (rb) { rowsBad++; if (rowsBad <= 16) printf("row %d: %d values wrong, worst by %d\n", y, rb, worst); }
                bad += rb;
            }
            printf("Gaussian blur against the CPU: %d values wrong in %d rows: %s\n", bad, rowsBad, bad ? "FAILED" : "ok");
            return bad ? 1 : 0;
        }
    }
    return 0;
}
