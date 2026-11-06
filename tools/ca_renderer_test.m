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


// RN_TRACE_ENC=1 (with `system`): logs the calls QuartzCore makes on the real device's render encoder, with the first bytes of the vertex data at the time of each draw and again at commit,
// to compare with what the plug-in sees (is data rewritten after the draw that uses it was encoded?).
static id g_vb[16], g_fb[16]; static NSUInteger g_vo[16], g_fo[16];
static NSMutableArray *g_draws;
static void rn_hook(Class c, SEL sel, id blockMaker) { Method m = class_getInstanceMethod(c, sel); if (m) method_setImplementation(m, imp_implementationWithBlock(blockMaker)); }
static void trace_install(id<MTLDevice> dev, id<MTLCommandQueue> q) {
    g_draws = [NSMutableArray new];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:8 height:8 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rd = [MTLRenderPassDescriptor renderPassDescriptor];
    rd.colorAttachments[0].texture = t; rd.colorAttachments[0].loadAction = MTLLoadActionClear; rd.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rd];
    Class ec = object_getClass(e), cc = object_getClass(cb), dc = object_getClass(dev);
    [e endEncoding];
    printf("trace: encoder class %s, command buffer class %s, device class %s\n", class_getName(ec), class_getName(cc), class_getName(dc));
    for (Class k = ec; k && k != [NSObject class]; k = class_getSuperclass(k)) printf("trace:   encoder superclass %s\n", class_getName(k));
    #define HEX(p, n, out) do { char *o_ = out; for (int j_ = 0; j_ < n; j_++) snprintf(o_ + 2 * j_, 3, "%02x", ((const uint8_t *)(p))[j_]); } while (0)
    __block IMP oSVB = method_getImplementation(class_getInstanceMethod(ec, @selector(setVertexBuffer:offset:atIndex:)));
    rn_hook(ec, @selector(setVertexBuffer:offset:atIndex:), ^(id self, id buf, NSUInteger off, NSUInteger idx) {
        if (idx < 16) { g_vb[idx] = buf; g_vo[idx] = off; }
        printf("trace: setVertexBuffer %p (len %lu) off %lu idx %lu\n", buf, (unsigned long)[(id<MTLBuffer>)buf length], (unsigned long)off, (unsigned long)idx);
        ((void (*)(id, SEL, id, NSUInteger, NSUInteger))oSVB)(self, @selector(setVertexBuffer:offset:atIndex:), buf, off, idx);
    });
    __block IMP oSVO = method_getImplementation(class_getInstanceMethod(ec, @selector(setVertexBufferOffset:atIndex:)));
    rn_hook(ec, @selector(setVertexBufferOffset:atIndex:), ^(id self, NSUInteger off, NSUInteger idx) {
        if (idx < 16) g_vo[idx] = off;
        printf("trace: setVertexBufferOffset off %lu idx %lu\n", (unsigned long)off, (unsigned long)idx);
        ((void (*)(id, SEL, NSUInteger, NSUInteger))oSVO)(self, @selector(setVertexBufferOffset:atIndex:), off, idx);
    });
    __block IMP oSFB = method_getImplementation(class_getInstanceMethod(ec, @selector(setFragmentBuffer:offset:atIndex:)));
    rn_hook(ec, @selector(setFragmentBuffer:offset:atIndex:), ^(id self, id buf, NSUInteger off, NSUInteger idx) {
        if (idx < 16) { g_fb[idx] = buf; g_fo[idx] = off; }
        printf("trace: setFragmentBuffer %p off %lu idx %lu\n", buf, (unsigned long)off, (unsigned long)idx);
        ((void (*)(id, SEL, id, NSUInteger, NSUInteger))oSFB)(self, @selector(setFragmentBuffer:offset:atIndex:), buf, off, idx);
    });
    __block IMP oDI = method_getImplementation(class_getInstanceMethod(ec, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:)));
    rn_hook(ec, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:), ^(id self, MTLPrimitiveType ty, NSUInteger n, MTLIndexType it, id ib, NSUInteger io) {
        char hx[100] = ""; if (g_vb[1]) HEX((const uint8_t *)[(id<MTLBuffer>)g_vb[1] contents] + g_vo[1], 32, hx);
        printf("trace: drawIndexed count %lu, v1 %p+%lu: %s\n", (unsigned long)n, g_vb[1], (unsigned long)g_vo[1], hx);
        if (g_vb[0]) for (int i = 0; i < 10; i++) if (g_fb[i]) { char fx[200] = ""; HEX((const uint8_t *)[(id<MTLBuffer>)g_fb[i] contents] + g_fo[i], 64, fx); printf("trace:   f%d+%lu: %s\n", i, (unsigned long)g_fo[i], fx); }
        if (g_vb[1]) [g_draws addObject:@[g_vb[1], @(g_vo[1]), [NSData dataWithBytes:(const uint8_t *)[(id<MTLBuffer>)g_vb[1] contents] + g_vo[1] length:32], @(g_draws.count)]];
        ((void (*)(id, SEL, MTLPrimitiveType, NSUInteger, MTLIndexType, id, NSUInteger))oDI)(self, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:), ty, n, it, ib, io);
    });
    __block IMP oEE = method_getImplementation(class_getInstanceMethod(ec, @selector(endEncoding)));
    rn_hook(ec, @selector(endEncoding), ^(id self) { printf("trace: endEncoding\n"); ((void (*)(id, SEL))oEE)(self, @selector(endEncoding)); });
    __block IMP oNB = method_getImplementation(class_getInstanceMethod(dc, @selector(newBufferWithLength:options:)));
    rn_hook(dc, @selector(newBufferWithLength:options:), ^id(id self, NSUInteger len, NSUInteger opt) {
        id b = ((id (*)(id, SEL, NSUInteger, NSUInteger))oNB)(self, @selector(newBufferWithLength:options:), len, opt);
        printf("trace: newBuffer %p len %lu options 0x%lx contents %p\n", b, (unsigned long)len, (unsigned long)opt, [(id<MTLBuffer>)b contents]);
        return b;
    });
    __block IMP oCM = method_getImplementation(class_getInstanceMethod(cc, @selector(commit)));
    rn_hook(cc, @selector(commit), ^(id self) {
        printf("trace: commit %p\n", self);
        for (NSArray *d in g_draws) {
            NSData *now = [NSData dataWithBytes:(const uint8_t *)[(id<MTLBuffer>)d[0] contents] + [d[1] unsignedLongValue] length:32];
            if (![now isEqualToData:d[2]]) printf("trace:   draw #%lu's vertex data changed between its encoding and the commit\n", [d[3] unsignedLongValue]);
        }
        [g_draws removeAllObjects];
        ((void (*)(id, SEL))oCM)(self, @selector(commit));
    });
    {   // function constants QuartzCore sets, by name or index, with the value bytes
        Class fcc = [MTLFunctionConstantValues class];
        __block IMP oN = method_getImplementation(class_getInstanceMethod(fcc, @selector(setConstantValue:type:withName:)));
        rn_hook(fcc, @selector(setConstantValue:type:withName:), ^(id self, const void *v, MTLDataType ty, NSString *nm) {
            unsigned char b[4] = {0}; memcpy(b, v, ty == MTLDataTypeBool || ty == MTLDataTypeUChar || ty == MTLDataTypeUShort ? (ty == MTLDataTypeUShort ? 2 : 1) : 4);
            printf("trace: fc %p %s type %lu = %02x%02x%02x%02x\n", self, nm.UTF8String, (unsigned long)ty, b[0], b[1], b[2], b[3]);
            ((void (*)(id, SEL, const void *, MTLDataType, NSString *))oN)(self, @selector(setConstantValue:type:withName:), v, ty, nm);
        });
        __block IMP oI = method_getImplementation(class_getInstanceMethod(fcc, @selector(setConstantValue:type:atIndex:)));
        rn_hook(fcc, @selector(setConstantValue:type:atIndex:), ^(id self, const void *v, MTLDataType ty, NSUInteger idx) {
            unsigned char b[4] = {0}; memcpy(b, v, ty == MTLDataTypeBool || ty == MTLDataTypeUChar ? 1 : ty == MTLDataTypeUShort ? 2 : 4);
            printf("trace: fc %p idx %lu type %lu = %02x%02x%02x%02x\n", self, (unsigned long)idx, (unsigned long)ty, b[0], b[1], b[2], b[3]);
            ((void (*)(id, SEL, const void *, MTLDataType, NSUInteger))oI)(self, @selector(setConstantValue:type:atIndex:), v, ty, idx);
        });
        __block IMP oV = method_getImplementation(class_getInstanceMethod(fcc, @selector(setConstantValues:type:withRange:)));
        rn_hook(fcc, @selector(setConstantValues:type:withRange:), ^(id self, const void *v, MTLDataType ty, NSRange r) {
            printf("trace: fc %p range %lu+%lu type %lu\n", self, (unsigned long)r.location, (unsigned long)r.length, (unsigned long)ty);
            ((void (*)(id, SEL, const void *, MTLDataType, NSRange))oV)(self, @selector(setConstantValues:type:withRange:), v, ty, r);
        });
    }
    {   // the function constants of every function QuartzCore makes (private accessors, as the plug-in reads them)
        NSError *le = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@"/System/Library/Frameworks/QuartzCore.framework/Resources/default.metallib"] error:&le];
        Class lc = object_getClass(lib);
        printf("trace: library class %s\n", lib ? class_getName(lc) : "(none)");
        void (^dump)(NSString *, MTLFunctionConstantValues *) = ^(NSString *name, MTLFunctionConstantValues *vals) {
            if (!vals) { printf("trace: fn %s (no constants)\n", name.UTF8String); return; }
            NSArray *idx = ((id (*)(id, SEL))objc_msgSend)(vals, sel_registerName("newIndexedConstantArray")), *named = ((id (*)(id, SEL))objc_msgSend)(vals, sel_registerName("newNamedConstantArray"));
            printf("trace: fn %s:", name.UTF8String);
            for (id v in named) { const unsigned char *d = ((const unsigned char *(*)(id, SEL))objc_msgSend)(v, @selector(data)); if (d[0] || d[1]) printf(" %s=%02x%02x", [((NSString *(*)(id, SEL))objc_msgSend)(v, @selector(name)) UTF8String], d[0], d[1]); }
            for (id v in idx) { const unsigned char *d = ((const unsigned char *(*)(id, SEL))objc_msgSend)(v, @selector(data)); if (d[0] || d[1]) printf(" #%lu=%02x%02x", ((NSUInteger (*)(id, SEL))objc_msgSend)(v, @selector(index)), d[0], d[1]); }
            printf("\n");
        };
        if (lib) {
            __block IMP oD = method_getImplementation(class_getInstanceMethod(lc, @selector(newFunctionWithDescriptor:error:)));
            rn_hook(lc, @selector(newFunctionWithDescriptor:error:), ^id(id self, MTLFunctionDescriptor *d, NSError **e) { dump(d.name, d.constantValues); return ((id (*)(id, SEL, id, NSError **))oD)(self, @selector(newFunctionWithDescriptor:error:), d, e); });
            __block IMP oC = method_getImplementation(class_getInstanceMethod(lc, @selector(newFunctionWithName:constantValues:error:)));
            rn_hook(lc, @selector(newFunctionWithName:constantValues:error:), ^id(id self, NSString *n, MTLFunctionConstantValues *c, NSError **e) { dump(n, c); return ((id (*)(id, SEL, id, id, NSError **))oC)(self, @selector(newFunctionWithName:constantValues:error:), n, c, e); });
        }
    }
    printf("trace: hooks installed\n");
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
        if (getenv("RN_TRACE_ENC")) trace_install(dev, q);
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
