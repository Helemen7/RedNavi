// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// A real Cocoa window with a CAMetalLayer, drawn with the RedNavi Metal plug-in on RADV: a triangle that turns and changes colour, 60 times a second. Everything the layer does with
// the device (drawable textures made from IOSurfaces, presentDrawable:) goes through RedNaviMTL/compute.m. Run it inside the desktop's GUI session (open -n test_window.app --env ...),
// never with RN_METAL_COMPUTE set outside of this one process: only processes that set it get the plug-in's queues, so WindowServer keeps its own rendering.
// usage: test_window.app/Contents/MacOS/test_window <RedNaviMTLDriver.bundle or "system"> <vk_tri.metallib> [seconds]
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>
#include <IOSurface/IOSurface.h>
#include <math.h>
#include <IOKit/IOKitLib.h>
#import <objc/message.h>

@interface TestView : NSView
@end
@implementation TestView
- (BOOL)wantsUpdateLayer { return YES; }
@end

@interface Delegate : NSObject <NSApplicationDelegate> {
@public
    CAMetalLayer *layer;
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> pso;
    unsigned frame, reported;
    NSTimeInterval start, lastReport, until;
    double tNext, tEncode, tCommit;                  // seconds spent in nextDrawable, encoding, and commit + wait + present, since the last report
}
@end
@implementation Delegate
- (void)draw:(NSTimer *)t {
    @autoreleasepool {
        const NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
        if (now > until) { printf("done after %u frames\n", frame); fflush(stdout); [NSApp terminate:nil]; return; }
        const NSTimeInterval t0 = [NSDate timeIntervalSinceReferenceDate];
        id<CAMetalDrawable> dr = [layer nextDrawable];
        const NSTimeInterval t1 = [NSDate timeIntervalSinceReferenceDate];
        if (!dr) { if (frame < 3 || frame % 60 == 0) printf("frame %u: nextDrawable returned nil\n", frame); fflush(stdout); frame++; return; }
        if (frame == 0) {
            id<MTLTexture> tx = dr.texture;
            printf("first drawable: texture class %s, %lux%lu, pixel format %lu, usage %lu, iosurface %p\n", class_getName(object_getClass(tx)), (unsigned long)tx.width, (unsigned long)tx.height,
                   (unsigned long)tx.pixelFormat, (unsigned long)tx.usage, tx.iosurface);
            fflush(stdout);
        }
        const float a = (float)(now - start) * 1.5f, c = 0.5f + 0.5f * sinf((float)(now - start));
        float pos[3][4], col[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
        for (int i = 0; i < 3; i++) {
            const float th = a + i * 2.0943951f;
            pos[i][0] = 0.7f * cosf(th); pos[i][1] = 0.7f * sinf(th); pos[i][2] = 0; pos[i][3] = 1;
        }
        const float tint[4] = {1, 0.5f + 0.5f * c, 1 - 0.5f * c, 1};
        MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = dr.texture;
        rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.15, 1.0);
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
        if (!enc) { printf("frame %u: no render encoder\n", frame); fflush(stdout); frame++; return; }
        [enc setRenderPipelineState:pso];
        [enc setVertexBytes:pos length:sizeof(pos) atIndex:0];
        [enc setVertexBytes:col length:sizeof(col) atIndex:1];
        [enc setFragmentBytes:tint length:sizeof(tint) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cb presentDrawable:dr];
        const NSTimeInterval t2 = [NSDate timeIntervalSinceReferenceDate];
        [cb commit];
        [cb waitUntilCompleted];
        const NSTimeInterval t3 = [NSDate timeIntervalSinceReferenceDate];
        tNext += t1 - t0; tEncode += t2 - t1; tCommit += t3 - t2;
        if (cb.status != MTLCommandBufferStatusCompleted && frame < 5) printf("frame %u: command buffer status %ld, %s\n", frame, (long)cb.status, cb.error.localizedDescription.UTF8String);
        frame++;
        if (now - lastReport >= 2.0) {
            const unsigned n = frame - reported;
            printf("%u frames, %.1f fps; per frame: nextDrawable %.1f ms, encode %.1f ms, commit+present %.1f ms\n", n, n / (now - lastReport), 1000 * tNext / n, 1000 * tEncode / n, 1000 * tCommit / n);
            tNext = tEncode = tCommit = 0;
            fflush(stdout);
            reported = frame; lastReport = now;
        }
    }
}
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    NSRect r = NSMakeRect(200, 200, 400, 400);
    NSWindow *w = [[NSWindow alloc] initWithContentRect:r styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    w.title = @"RedNavi Metal on RADV";
    TestView *v = [[TestView alloc] initWithFrame:NSMakeRect(0, 0, 400, 400)];
    layer = [CAMetalLayer layer];
    layer.device = dev;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.contentsScale = w.backingScaleFactor;
    layer.drawableSize = CGSizeMake(400 * w.backingScaleFactor, 400 * w.backingScaleFactor);
    printf("layer device %s (%s), scale %.1f, drawable size %.0fx%.0f\n", [layer.device.name UTF8String], class_getName(object_getClass(layer.device)), w.backingScaleFactor, layer.drawableSize.width, layer.drawableSize.height);
    v.wantsLayer = YES;
    v.layer = layer;
    w.contentView = v;
    [w makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    start = lastReport = [NSDate timeIntervalSinceReferenceDate];
    until = start + (getenv("RN_WINDOW_SECONDS") ? atof(getenv("RN_WINDOW_SECONDS")) : 20.0);
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 target:self selector:@selector(draw:) userInfo:nil repeats:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        const char *bundlePath = getenv("RN_WINDOW_BUNDLE"), *lib = getenv("RN_WINDOW_LIB");
        if (!bundlePath || !lib) { printf("RN_WINDOW_BUNDLE and RN_WINDOW_LIB must be set\n"); return 2; }
        printf("RN_METAL_COMPUTE=%s\n", getenv("RN_METAL_COMPUTE") ? getenv("RN_METAL_COMPUTE") : "(unset)");
        id<MTLDevice> dev = nil;
        if (strcmp(bundlePath, "system") == 0) {
            dev = MTLCreateSystemDefaultDevice();
            NSArray *all = MTLCopyAllDevices();
            printf("system default device %p, %lu devices\n", (__bridge void *)dev, (unsigned long)all.count);
        } else {
            // Like tools/mtl_init_harness: the bundle from a folder, the device made from the accelerator in the registry, without Metal enumerating devices (which would load the installed bundle)
            NSBundle *b = [NSBundle bundleWithPath:@(bundlePath)];
            NSError *e = nil;
            printf("bundle load: %d\n", [b loadAndReturnError:&e]);
            Class k = objc_getClass("RedNaviMTLDevice");
            io_service_t svc = 0; io_iterator_t it;
            if (k && IORegistryCreateIterator(kIOMainPortDefault, kIOServicePlane, kIORegistryIterateRecursively, &it) == KERN_SUCCESS) {
                io_object_t o;
                while (!svc && (o = IOIteratorNext(it))) { if (IOObjectConformsTo(o, "IOAccelerator")) svc = o; else IOObjectRelease(o); }
            }
            if (!k || !svc) { printf("no RedNaviMTLDevice class or accelerator\n"); return 3; }
            id d = ((id (*)(id, SEL))objc_msgSend)((id)k, sel_registerName("alloc"));
            dev = ((id (*)(id, SEL, unsigned))objc_msgSend)(d, sel_registerName("initWithAcceleratorPort:"), svc);
        }
        if (!dev) { printf("no device\n"); return 4; }
        printf("device %s, class %s\n", [dev.name UTF8String], class_getName(object_getClass(dev)));
        NSData *blob = [NSData dataWithContentsOfFile:@(lib)];
        dispatch_data_t dd = dispatch_data_create(blob.bytes, blob.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *e = nil;
        id<MTLLibrary> library = [dev newLibraryWithData:dd error:&e];
        if (!library) { printf("no library: %s\n", e.localizedDescription.UTF8String); return 5; }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [library newFunctionWithName:@"tri_vertex"];
        pd.fragmentFunction = [library newFunctionWithName:@"tri_fragment"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!pso) { printf("no pipeline: %s\n", e.localizedDescription.UTF8String); return 6; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        if (!q) { printf("no command queue\n"); return 7; }
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        Delegate *del = [Delegate new];
        del->dev = dev; del->queue = q; del->pso = pso;
        app.delegate = del;
        [app run];
    }
    return 0;
}
