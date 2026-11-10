// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// CoreImage on the RedNavi device: a CIContext made on the plug-in's MTLDevice renders a filtered image (what iconservicesagent, mediaanalysisd and others do all day). CoreImage builds
// Metal programs (stitched libraries, compute pipelines) at the first render; before the plug-in answered those it crashed inside Metal. Prints the pixel it got; the log of the plug-in
// shows what CoreImage asked for. usage: ci_test <RedNaviMTLDriver.bundle|system>   (RN_METAL_COMPUTE=1 or the files of docs/TEST_MACHINES.md, RN_VK_DIR=..., DYLD_LIBRARY_PATH=...)
#import <Cocoa/Cocoa.h>
#import <CoreImage/CoreImage.h>
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
        CIContext *ctx = [CIContext contextWithMTLDevice:dev options:@{kCIContextWorkingColorSpace: (__bridge id)CGColorSpaceCreateWithName(kCGColorSpaceSRGB)}];
        CIImage *src = [[CIImage imageWithColor:[CIColor colorWithRed:1 green:0.5 blue:0 alpha:1]] imageByCroppingToRect:CGRectMake(0, 0, 64, 64)];
        const char *which = getenv("RN_CI_FILTER") ? getenv("RN_CI_FILTER") : "CIColorInvert";   // RN_CI_FILTER=CIGaussianBlur uses MetalPerformanceShaders
        CIFilter *f = [CIFilter filterWithName:@(which)];
        [f setValue:src forKey:kCIInputImageKey];
        if ([f.inputKeys containsObject:kCIInputRadiusKey]) [f setValue:@4 forKey:kCIInputRadiusKey];
        CIImage *out = f.outputImage;
        // several filters in a row: CoreImage compiles them as one program (a DAG of stitched functions), which is what the system's agents trigger
        CIFilter *cc = [CIFilter filterWithName:@"CIColorControls" withInputParameters:@{kCIInputImageKey: out, kCIInputSaturationKey: @1.4, kCIInputBrightnessKey: @0.1}];
        CIFilter *vg = [CIFilter filterWithName:@"CIVignette" withInputParameters:@{kCIInputImageKey: cc.outputImage, kCIInputIntensityKey: @0.5, kCIInputRadiusKey: @2}];
        out = [vg.outputImage imageByCroppingToRect:CGRectMake(0, 0, 64, 64)];
        uint8_t px[64 * 64 * 4] = {0};
        [ctx render:out toBitmap:px rowBytes:64 * 4 bounds:CGRectMake(0, 0, 64, 64) format:kCIFormatRGBA8 colorSpace:CGColorSpaceCreateWithName(kCGColorSpaceSRGB)];
        printf("filter %s: pixel (32,32) = %u %u %u %u\n", which, px[(32 * 64 + 32) * 4], px[(32 * 64 + 32) * 4 + 1], px[(32 * 64 + 32) * 4 + 2], px[(32 * 64 + 32) * 4 + 3]);
    }
    return 0;
}
