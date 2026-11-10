// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Many small textures on the RedNavi device until one is refused (WindowServer lost them after ~150): prints how many, and how many at each step. Same arguments as ci_test.
// (derived from mps_test.m) MPS on the RedNavi device: a Gaussian blur through MetalPerformanceShaders, as WindowServer does for desktop window effects. Same arguments as ci_test.
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
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:80 height:64 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        int n = argc > 2 ? atoi(argv[2]) : 2000;
        NSMutableArray *keep = [NSMutableArray new];
        int fail = 0;
        for (int i = 0; i < n; i++) {
            id<MTLTexture> t = [dev newTextureWithDescriptor:td];
            if (!t) { if (!fail) printf("first failure at texture %d\n", i); fail++; continue; }
            [keep addObject:t];
        }
        printf("made %lu of %d, %d failed\n", (unsigned long)keep.count, n, fail);
        [keep removeAllObjects];
        int ok2 = 0;
        for (int i = 0; i < 200; i++) { id<MTLTexture> t = [dev newTextureWithDescriptor:td]; if (t) ok2++; }
        printf("after freeing all: %d of 200 made\n", ok2);
    }
    return 0;
}
