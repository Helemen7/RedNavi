// QuartzCore's own Metal renderer on the RedNavi device, without WindowServer: a CARenderer draws a small layer tree (colours, rounded corners, opacity, a gradient, a shadow, an image,
// text) into a Metal texture. QuartzCore asks the device for its library (default.metallib), functions with function constants and pipelines, so the plug-in log
// (/tmp/rednavi-metal-<uid>.log) shows what it needs; the picture is written as a PNG. Environment of QuartzCore (for instance CA_DISABLE_INPLACE_SHADERS=1) can be set when running it.
// usage: ca_renderer_test <RedNaviMTLDriver.bundle|system> <out.png>   (RN_METAL_COMPUTE=1, RN_VK_DIR=..., DYLD_LIBRARY_PATH=... as for the harness)
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <IOKit/IOKitLib.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

extern void instrumentObjcMessageSends(BOOL);
static void on_crash(int sig, siginfo_t *si, void *uc) {
    char b[128];
    int n = snprintf(b, sizeof(b), "\nSIGNAL %d at address %p\n", sig, si->si_addr);
    write(2, b, n);
    void *bt[40]; int nb = backtrace(bt, 40); backtrace_symbols_fd(bt, nb, 2);
    _exit(139);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        static char alt[65536]; stack_t ss = {alt, sizeof(alt), 0}; sigaltstack(&ss, NULL);
        struct sigaction sa = {0}; sa.sa_sigaction = on_crash; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
        if (argc < 3) { puts("usage: ca_renderer_test <bundle|system> <out.png>"); return 2; }
        id<MTLDevice> dev = nil;
        if (strcmp(argv[1], "system") == 0) dev = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *b = [NSBundle bundleWithPath:@(argv[1])];
            NSError *e = nil;
            printf("bundle load: %d\n", [b loadAndReturnError:&e]);
            Class k = objc_getClass("RedNaviMTLDevice");
            io_service_t svc = 0; io_iterator_t it;
            if (k && IORegistryCreateIterator(kIOMainPortDefault, kIOServicePlane, kIORegistryIterateRecursively, &it) == KERN_SUCCESS) {
                io_object_t o;
                while (!svc && (o = IOIteratorNext(it))) { if (IOObjectConformsTo(o, "IOAccelerator")) svc = o; else IOObjectRelease(o); }
            }
            if (!k || !svc) { puts("no RedNaviMTLDevice class or accelerator"); return 3; }
            id d = ((id (*)(id, SEL))objc_msgSend)((id)k, sel_registerName("alloc"));
            dev = ((id (*)(id, SEL, unsigned))objc_msgSend)(d, sel_registerName("initWithAcceleratorPort:"), svc);
        }
        if (!dev) { puts("no device"); return 4; }
        printf("device %s (%s)\n", dev.name.UTF8String, class_getName(object_getClass(dev)));
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const int W = 256, H = 256;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        if (!q || !tex) { puts("no queue or texture"); return 5; }

        CALayer *root = [CALayer layer];
        root.frame = CGRectMake(0, 0, W, H);
        root.backgroundColor = CGColorCreateGenericRGB(0.1, 0.1, 0.3, 1);
        CALayer *a = [CALayer layer];
        a.frame = CGRectMake(20, 140, 100, 90); a.backgroundColor = CGColorCreateGenericRGB(1, 0.2, 0.2, 1); a.cornerRadius = 14; a.opacity = 0.8f;
        const char *only = getenv("RN_ONLY");           // RN_ONLY=a (rounded rect), g (gradient), s (shadow square), i (image): draw only those layers
        #define WANT(c) (!only || strchr(only, c))
        if (WANT('a')) [root addSublayer:a];
        CAGradientLayer *g = [CAGradientLayer layer];
        g.frame = CGRectMake(136, 140, 100, 90);
        g.colors = @[(__bridge_transfer id)CGColorCreateGenericRGB(0, 1, 0, 1), (__bridge_transfer id)CGColorCreateGenericRGB(1, 1, 0, 1)];
        if (WANT('g')) [root addSublayer:g];
        CALayer *s = [CALayer layer];
        s.frame = CGRectMake(40, 30, 80, 70); s.backgroundColor = CGColorCreateGenericRGB(1, 1, 1, 1);
        s.shadowOpacity = 0.7f; s.shadowRadius = 8; s.shadowOffset = CGSizeMake(0, -4); s.shadowColor = CGColorCreateGenericRGB(0, 0, 0, 1);
        if (WANT('s')) [root addSublayer:s];
        NSImage *img = [NSImage imageWithSize:NSMakeSize(64, 64) flipped:NO drawingHandler:^BOOL(NSRect r) {
            [[NSColor orangeColor] setFill]; NSRectFill(r); [[NSColor blueColor] setFill]; NSRectFill(NSMakeRect(8, 8, 24, 24)); return YES; }];
        CALayer *im = [CALayer layer];
        im.frame = CGRectMake(150, 30, 64, 64);
        im.contents = (__bridge id)[img CGImageForProposedRect:NULL context:nil hints:nil];
        if (WANT('i')) [root addSublayer:im];

        if (getenv("RN_TRACE_MSGS")) instrumentObjcMessageSends(YES);     // /tmp/msgSends-<pid>: every Objective-C message from here on
        CARenderer *r = [CARenderer rendererWithMTLTexture:tex options:@{kCARendererMetalCommandQueue: q}];
        if (!r) { puts("no CARenderer"); return 6; }
        r.layer = root;
        r.bounds = CGRectMake(0, 0, W, H);
        [CATransaction flush];
        for (int frame = 0; frame < 3; frame++) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
            [r beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
            [r addUpdateRect:r.bounds];
            [r render];
            [r endFrame];
            printf("frame %d rendered\n", frame);
        }
        // wait for the GPU work QuartzCore queued on our queue
        id<MTLCommandBuffer> cb = [q commandBuffer];
        [cb commit]; [cb waitUntilCompleted];
        uint8_t *px = malloc(W * H * 4);
        [tex getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
        // BGRA to RGBA for the PNG
        unsigned distinct = 0; uint32_t seen[64]; unsigned nonbg = 0;
        for (int i = 0; i < W * H; i++) {
            uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t;
            uint32_t v = *(uint32_t *)(px + i * 4);
            if (v != 0xff4d1a1a && v != 0) nonbg++;
            unsigned k = 0; while (k < distinct && seen[k] != v) k++;
            if (k == distinct && distinct < 64) seen[distinct++] = v;
        }
        printf("picture: %u distinct colours (up to 64 counted), %u pixels differ from the background and from zero; pixel (30,200) = %u %u %u %u\n", distinct, nonbg, px[(56 * W + 30) * 4], px[(56 * W + 30) * 4 + 1], px[(56 * W + 30) * 4 + 2], px[(56 * W + 30) * 4 + 3]);
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:&px pixelsWide:W pixelsHigh:H bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:W * 4 bitsPerPixel:32];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[2]) atomically:YES];
        printf("wrote %s\n", argv[2]);
    }
    return 0;
}
