// Runs what Metal does for one accelerator in a single process, without registering anything: loads the plug-in bundle, finds the accelerator in the registry (also an
// unregistered one), and sends the plug-in class -initWithAcceleratorPort:. A SIGSEGV handler prints the fault address, the registers and the regions around it, so a
// crash of the framework's user-space code shows where it reads (docs/M6_ACCELERATOR_STUDY.md, section 9).
// usage: mtl_init_harness [bundle]   (default /Library/GPUBundles/RedNaviMTLDriver.bundle)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <IOSurface/IOSurface.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <dlfcn.h>
#include "../mesa-bridge/test/vk_dispatch_spv.h"
#include "../mesa-bridge/test/vk_copy2_spv.h"
#include <signal.h>
#include <execinfo.h>
#include <sys/ucontext.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void handler(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    _STRUCT_X86_THREAD_STATE64 *s = &uc->uc_mcontext->__ss;
    char b[2048];
    int n = snprintf(b, sizeof(b), "\nSIGNAL %d at address %p\nrip %llx rsp %llx rbp %llx\nrax %llx rbx %llx rcx %llx rdx %llx\nrsi %llx rdi %llx r8 %llx r9 %llx\nr10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx\n",
                     sig, si->si_addr, s->__rip, s->__rsp, s->__rbp, s->__rax, s->__rbx, s->__rcx, s->__rdx, s->__rsi, s->__rdi, s->__r8, s->__r9, s->__r10, s->__r11, s->__r12, s->__r13, s->__r14, s->__r15);
    write(2, b, n);
    { void *bt[40]; int nb = backtrace(bt, 40); backtrace_symbols_fd(bt, nb, 2); }
    // regions near the fault address
    mach_vm_address_t a = (mach_vm_address_t)si->si_addr > 0x40000 ? (mach_vm_address_t)si->si_addr - 0x40000 : 0;
    for (int i = 0; i < 12; i++) {
        mach_vm_size_t sz = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
        if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) break;
        n = snprintf(b, sizeof(b), "region %llx-%llx prot %d/%d share %d\n", a, a + sz, info.protection, info.max_protection, info.shared);
        write(2, b, n);
        a += sz;
        if (a > (mach_vm_address_t)si->si_addr + 0x10000) break;
    }
    _exit(139);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        struct sigaction sa = {0};
        sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        static char altstack[65536]; stack_t ss = { altstack, sizeof(altstack), 0 }; sigaltstack(&ss, NULL);
        sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
        NSString *path = argc > 1 ? @(argv[1]) : @"/Library/GPUBundles/RedNaviMTLDriver.bundle";
        NSBundle *b = [NSBundle bundleWithPath:path];
        NSError *e = nil;
        printf("bundle load: %d\n", [b loadAndReturnError:&e]);
        if (getenv("RN_VK_SELFTEST")) {
            // RADV inside this process, with Metal loaded: the bundle's rn_vk_selftest (RedNaviMTL/rn_vk.c)
            NSString *exe = [b executablePath];
            void *h = dlopen(exe.UTF8String, RTLD_NOLOAD | RTLD_NOW);
            int (*st)(const char *) = h ? dlsym(h, "rn_vk_selftest") : NULL;
            printf("rn_vk_selftest %s\n", st ? "found" : "missing");
            if (st) { int rc = st(getenv("RN_VK_DIR")); printf("rn_vk_selftest -> %d\n", rc); }
            return 0;
        }
        Class k = objc_getClass("RedNaviMTLDevice");
        printf("class: %s\n", k ? class_getName(k) : "nil");
        if (!k) return 1;
        io_service_t s = 0; io_iterator_t it;
        if (IORegistryCreateIterator(kIOMainPortDefault, kIOServicePlane, kIORegistryIterateRecursively, &it) == KERN_SUCCESS) {
            io_object_t o;
            while (!s && (o = IOIteratorNext(it))) { if (IOObjectConformsTo(o, "IOAccelerator")) s = o; else IOObjectRelease(o); }
        }
        io_name_t cls = "?"; IOObjectGetClass(s, cls);
        printf("accelerator port %u class %s\n", s, cls);
        fflush(stdout);
        id dev = ((id (*)(id, SEL))objc_msgSend)((id)k, sel_registerName("alloc"));
        id r = ((id (*)(id, SEL, unsigned))objc_msgSend)(dev, sel_registerName("initWithAcceleratorPort:"), s);
        printf("initWithAcceleratorPort: -> %p\n", (__bridge void *)r);
        if (r) {
            printf("class %s, conforms to MTLDevice: %d\n", class_getName(object_getClass(r)), [r conformsToProtocol:@protocol(MTLDevice)]);
            printf("name: %s\n", [[r name] UTF8String]);
            if (getenv("RN_TRI_TEST")) {
                // A render pipeline from a .metallib (mesa-bridge/test/vk_tri.metal) through the official API: a 40 x 24 RGBA8 target cleared to grey, one triangle in the top-left
                // quadrant of Metal's clip space (x from -1 to 0, y from 0 to 1: pixels x < 20, y < 12, a different colour at each corner). The asserts catch a flipped image (the bottom-left
                // must stay grey), varyings that do not meet (the colours) and anything drawn outside the triangle's quadrant.
                id<MTLDevice> d = (id<MTLDevice>)r;
                NSData *blob = [NSData dataWithContentsOfFile:@(getenv("RN_TRI_TEST"))];
                dispatch_data_t dd = dispatch_data_create(blob.bytes, blob.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
                NSError *e = nil;
                id<MTLLibrary> lib = [d newLibraryWithData:dd error:&e];
                if (!lib) { printf("no library\n"); return 50; }
                MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
                pd.vertexFunction = [lib newFunctionWithName:@"tri_vertex"];
                pd.fragmentFunction = [lib newFunctionWithName:@"tri_fragment"];
                pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                id<MTLRenderPipelineState> rps = [d newRenderPipelineStateWithDescriptor:pd error:&e];
                printf("render pipeline %p, error: %s\n", (__bridge void *)rps, e ? e.localizedDescription.UTF8String : "none");
                if (!rps) return 51;
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:40 height:24 mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                id<MTLTexture> target = [d newTextureWithDescriptor:td];
                if (!target) { printf("no target texture\n"); return 52; }
                const float pos[3][4] = {{-1, 1, 0, 1}, {0, 1, 0, 1}, {-1, 0, 0, 1}};
                const float col[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
                const float tint[4] = {1, 1, 1, 1};
                MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
                rpd.colorAttachments[0].texture = target;
                rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
                rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
                rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.2, 0.2, 1.0);
                id<MTLCommandQueue> q = [d newCommandQueue];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
                if (!enc) { printf("no render encoder\n"); return 53; }
                [enc setRenderPipelineState:rps];
                [enc setVertexBytes:pos length:sizeof(pos) atIndex:0];
                [enc setVertexBytes:col length:sizeof(col) atIndex:1];
                [enc setFragmentBytes:tint length:sizeof(tint) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                uint8_t px[40 * 24 * 4];
                [target getBytes:px bytesPerRow:40 * 4 fromRegion:MTLRegionMake2D(0, 0, 40, 24) mipmapLevel:0];
                #define PX(x, y) (px + ((y) * 40 + (x)) * 4)
                int bad = 0, inTri = 0;
                for (int y = 0; y < 24; y++) for (int x = 0; x < 40; x++) {
                    const uint8_t *p = PX(x, y);
                    const int grey = p[0] == 51 && p[1] == 51 && p[2] == 51 && p[3] == 255;
                    if (!grey) inTri++;
                    if (!grey && (x >= 20 || y >= 12)) { if (bad < 5) printf("  drawn outside the quadrant: (%d,%d) = %u %u %u %u\n", x, y, p[0], p[1], p[2], p[3]); bad++; }
                }
                const uint8_t *a = PX(2, 1), *b = PX(14, 1), *c = PX(2, 9), *far = PX(39, 23), *bl = PX(2, 22);
                printf("status %ld, error %s; %d pixels differ from the clear colour (a half of the 20 x 12 quadrant is about 120)\n", (long)cb.status, cb.error ? cb.error.localizedDescription.UTF8String : "none", inTri);
                printf("near the red corner (2,1): %u %u %u; near green (14,1): %u %u %u; near blue (2,9): %u %u %u; bottom-right: %u %u %u; bottom-left: %u %u %u\n",
                       a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2], far[0], far[1], far[2], bl[0], bl[1], bl[2]);
                int fails = bad;
                if (!(a[0] > 180 && a[1] < 70 && a[2] < 70)) { printf("  FAIL: the red corner is not red\n"); fails++; }
                if (!(b[1] > 140 && b[0] < 110 && b[2] < 110)) { printf("  FAIL: the green corner is not green\n"); fails++; }
                if (!(c[2] > 140 && c[0] < 110 && c[1] < 110)) { printf("  FAIL: the blue corner is not blue\n"); fails++; }
                if (!(far[0] == 51 && far[1] == 51 && far[2] == 51)) { printf("  FAIL: the bottom-right is not the clear colour\n"); fails++; }
                if (!(bl[0] == 51 && bl[1] == 51 && bl[2] == 51)) { printf("  FAIL: the bottom-left is not the clear colour (image flipped?)\n"); fails++; }
                if (inTri < 100 || inTri > 140) { printf("  FAIL: the triangle covers %d pixels, not about 120\n", inTri); fails++; }
                printf("render encoder through Metal on RADV: %s\n", fails ? "FAILED" : "all checks passed");
                if (!fails && getenv("RN_PRESENT_SECONDS")) {
                    // To the screen: the same triangle, at the display's size, in a Metal texture; the pixels go to display surface 1 (rn_bridge_scanout, XRGB8888 = B G R X
                    // in memory) and rn_bridge_present flips to it. Surface 0 stays the desktop's and comes back at the end.
                    const char *dir = getenv("RN_VK_DIR");
                    char lp[1024]; snprintf(lp, sizeof(lp), "%s/libvulkan_radeon.dylib", dir ? dir : ".");
                    void *lib2 = dlopen(lp, RTLD_LAZY);
                    int (*scanout)(uint32_t, uint32_t *, uint32_t *, uint32_t *, void **, size_t *) = lib2 ? dlsym(lib2, "rn_bridge_scanout") : NULL;
                    int (*present)(uint32_t, int) = lib2 ? dlsym(lib2, "rn_bridge_present") : NULL;
                    uint32_t W = 0, H = 0, pitch = 0; void *scan = NULL; size_t slen = 0;
                    if (!scanout || !present || scanout(1, &W, &H, &pitch, &scan, &slen)) { printf("no display surface (needs rednavi_fb=1 and the bridge)\n"); return 60; }
                    printf("display surface 1: %ux%u, pitch %u\n", W, H, pitch);
                    MTLTextureDescriptor *bt = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
                    bt.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                    id<MTLTexture> big = [d newTextureWithDescriptor:bt];
                    if (!big) { printf("no display-size texture\n"); return 61; }
                    // a full-screen triangle (x from -1 to 1, y from -1 to 1 on the lower-left half), a dark blue background
                    const float bpos[3][4] = {{-0.8f, -0.8f, 0, 1}, {0.8f, -0.8f, 0, 1}, {0, 0.8f, 0, 1}};
                    rpd.colorAttachments[0].texture = big;
                    rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.05, 0.2, 1.0);
                    cb = [q commandBuffer];
                    enc = [cb renderCommandEncoderWithDescriptor:rpd];
                    [enc setRenderPipelineState:rps];
                    [enc setVertexBytes:bpos length:sizeof(bpos) atIndex:0];
                    [enc setVertexBytes:col length:sizeof(col) atIndex:1];
                    [enc setFragmentBytes:tint length:sizeof(tint) atIndex:0];
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [enc endEncoding];
                    [cb commit]; [cb waitUntilCompleted];
                    uint8_t *row = malloc((size_t)W * 4);
                    for (uint32_t y = 0; y < H; y++) {
                        [big getBytes:row bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, y, W, 1) mipmapLevel:0];
                        uint8_t *dst = (uint8_t *)scan + (size_t)y * pitch;
                        for (uint32_t x = 0; x < W; x++) { dst[x*4] = row[x*4+2]; dst[x*4+1] = row[x*4+1]; dst[x*4+2] = row[x*4]; dst[x*4+3] = 255; }
                    }
                    free(row);
                    int pr = present(1, 1);
                    printf("rn_bridge_present(1) -> %d; triangle on the screen for %s s\n", pr, getenv("RN_PRESENT_SECONDS"));
                    fflush(stdout);
                    sleep((unsigned)atoi(getenv("RN_PRESENT_SECONDS")));
                    pr = present(0, 1);
                    printf("rn_bridge_present(0) -> %d (desktop back)\n", pr);
                }
                return fails ? 1 : 0;
            }
            if (getenv("RN_QUAD_TEST") || getenv("RN_IOSURFACE_TEST")) {
                // [[stage_in]] with a vertex descriptor, a texture sampled in the fragment function with a runtime sampler state, and alpha blending (mesa-bridge/test/vk_quad.metal).
                // Pass 1 on a 32 x 32 target: a quad over the whole target with a 2 x 2 texture (top-left red, top-right green, bottom-left blue, bottom-right white), blending off;
                // then a small square inside the bottom-right quadrant with its colour (0.5, 0, 0, 0.5) and the blend state One / OneMinusSourceAlpha (premultiplied): over white it
                // gives (1, 0.5, 0.5, 1). The vertices of the quad come from an MTLBuffer, those of the square from setVertexBytes.
                id<MTLDevice> d = (id<MTLDevice>)r;
                NSData *blob = [NSData dataWithContentsOfFile:@(getenv("RN_QUAD_TEST") ? getenv("RN_QUAD_TEST") : getenv("RN_IOSURFACE_TEST"))];
                dispatch_data_t dd = dispatch_data_create(blob.bytes, blob.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
                NSError *e = nil;
                id<MTLLibrary> lib = [d newLibraryWithData:dd error:&e];
                if (!lib) { printf("no library\n"); return 70; }
                MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
                vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0;  vd.attributes[0].bufferIndex = 0;
                vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8;  vd.attributes[1].bufferIndex = 0;
                vd.attributes[2].format = MTLVertexFormatFloat4; vd.attributes[2].offset = 16; vd.attributes[2].bufferIndex = 0;
                vd.layouts[0].stride = 32;
                id<MTLRenderPipelineState> rps[2];
                for (int blend = 0; blend < 2; blend++) {
                    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
                    pd.vertexFunction = [lib newFunctionWithName:@"quad_vertex"];
                    pd.fragmentFunction = [lib newFunctionWithName:@"quad_fragment"];
                    pd.vertexDescriptor = vd;
                    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                    if (blend) {
                        pd.colorAttachments[0].blendingEnabled = YES;
                        pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
                        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                        pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
                        pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    }
                    e = nil;
                    rps[blend] = [d newRenderPipelineStateWithDescriptor:pd error:&e];
                    printf("pipeline (blending %s): %p, error: %s\n", blend ? "on" : "off", (__bridge void *)rps[blend], e ? e.localizedDescription.UTF8String : "none");
                    if (!rps[blend]) return 71;
                }
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];
                td.usage = MTLTextureUsageShaderRead;
                // RN_IOSURFACE_TEST: the sampled 2 x 2 texture and the 32 x 32 target are both IOSurfaces (pitch padded to 256 bytes), the texels are written into the first one through its
                // base address and the result is read from the second one's base address, with no getBytes: the copies at the command buffer's borders are what is under test.
                const int io = getenv("RN_IOSURFACE_TEST") != NULL;
                IOSurfaceRef surfTex = NULL, surfTarget = NULL;
                const uint8_t texels[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};
                id<MTLTexture> tex = nil;
                if (io) {
                    surfTex = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth: @2, (id)kIOSurfaceHeight: @2, (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @256, (id)kIOSurfacePixelFormat: @('RGBA')});
                    surfTarget = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth: @32, (id)kIOSurfaceHeight: @32, (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @256, (id)kIOSurfacePixelFormat: @('RGBA')});
                    if (!surfTex || !surfTarget) { printf("IOSurfaceCreate failed\n"); return 73; }
                    printf("IOSurfaces: pitch %zu and %zu\n", IOSurfaceGetBytesPerRow(surfTex), IOSurfaceGetBytesPerRow(surfTarget));
                    IOSurfaceLock(surfTex, 0, NULL);
                    for (int y = 0; y < 2; y++) memcpy((uint8_t *)IOSurfaceGetBaseAddress(surfTex) + y * IOSurfaceGetBytesPerRow(surfTex), texels[y * 2], 8);
                    IOSurfaceUnlock(surfTex, 0, NULL);
                    IOSurfaceLock(surfTarget, 0, NULL);
                    memset(IOSurfaceGetBaseAddress(surfTarget), 0x7f, IOSurfaceGetAllocSize(surfTarget));      // so a missing write-back shows
                    IOSurfaceUnlock(surfTarget, 0, NULL);
                    tex = [d newTextureWithDescriptor:td iosurface:surfTex plane:0];
                } else {
                    tex = [d newTextureWithDescriptor:td];
                    [tex replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 withBytes:texels bytesPerRow:8];
                }
                MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
                sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
                sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
                id<MTLSamplerState> smp = [d newSamplerStateWithDescriptor:sd];
                MTLTextureDescriptor *tt = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:32 mipmapped:NO];
                tt.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                id<MTLTexture> target = io ? [d newTextureWithDescriptor:tt iosurface:surfTarget plane:0] : [d newTextureWithDescriptor:tt];
                if (!tex || !smp || !target) { printf("no texture or sampler\n"); return 72; }
                typedef struct { float pos[2], uv[2], color[4]; } V;
                const V quad[6] = {{{-1, 1}, {0, 0}, {1, 1, 1, 1}}, {{-1, -1}, {0, 1}, {1, 1, 1, 1}}, {{1, 1}, {1, 0}, {1, 1, 1, 1}},
                                   {{1, 1}, {1, 0}, {1, 1, 1, 1}}, {{-1, -1}, {0, 1}, {1, 1, 1, 1}}, {{1, -1}, {1, 1}, {1, 1, 1, 1}}};
                const float sc[4] = {0.5f, 0, 0, 0.5f};
                const V sq[6] = {{{0.25f, -0.25f}, {0.9f, 0.9f}, {0.5f, 0, 0, 0.5f}}, {{0.25f, -0.75f}, {0.9f, 0.9f}, {0.5f, 0, 0, 0.5f}}, {{0.75f, -0.25f}, {0.9f, 0.9f}, {0.5f, 0, 0, 0.5f}},
                                 {{0.75f, -0.25f}, {0.9f, 0.9f}, {0.5f, 0, 0, 0.5f}}, {{0.25f, -0.75f}, {0.9f, 0.9f}, {0.5f, 0, 0, 0.5f}}, {{0.75f, -0.75f}, {0.9f, 0.9f}, {0.5f, 0, 0, 0.5f}}};
                (void)sc;
                id<MTLBuffer> vb = [d newBufferWithBytes:quad length:sizeof(quad) options:MTLResourceStorageModeShared];
                MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
                rpd.colorAttachments[0].texture = target;
                rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
                rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
                rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                id<MTLCommandQueue> q = [d newCommandQueue];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
                [enc setFragmentTexture:tex atIndex:0];
                [enc setFragmentSamplerState:smp atIndex:0];
                [enc setRenderPipelineState:rps[0]];
                [enc setVertexBuffer:vb offset:0 atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                [enc setRenderPipelineState:rps[1]];
                [enc setVertexBytes:sq length:sizeof(sq) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                [enc endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                uint8_t px[32 * 32 * 4];
                if (io) {
                    IOSurfaceLock(surfTarget, kIOSurfaceLockReadOnly, NULL);
                    for (int y = 0; y < 32; y++) memcpy(px + y * 128, (uint8_t *)IOSurfaceGetBaseAddress(surfTarget) + y * IOSurfaceGetBytesPerRow(surfTarget), 128);
                    IOSurfaceUnlock(surfTarget, kIOSurfaceLockReadOnly, NULL);
                } else [target getBytes:px bytesPerRow:32 * 4 fromRegion:MTLRegionMake2D(0, 0, 32, 32) mipmapLevel:0];
                struct { int x, y; int want[4]; const char *what; } probes[] = {
                    {8, 8, {255, 0, 0, 255}, "top-left texel (red)"}, {24, 8, {0, 255, 0, 255}, "top-right texel (green)"}, {8, 24, {0, 0, 255, 255}, "bottom-left texel (blue)"},
                    {30, 30, {255, 255, 255, 255}, "bottom-right texel (white), outside the square"}, {18, 24, {255, 255, 255, 255}, "left of the square"}, {24, 18, {255, 255, 255, 255}, "above the square"},
                    {24, 24, {255, 128, 128, 255}, "inside the square: (0.5,0,0,0.5) blended over white"}};
                int fails = 0;
                for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
                    const uint8_t *p = px + (probes[i].y * 32 + probes[i].x) * 4;
                    int ok = 1;
                    for (int c = 0; c < 4; c++) if (abs((int)p[c] - probes[i].want[c]) > 3) ok = 0;
                    printf("  (%d,%d) %s: %u %u %u %u %s\n", probes[i].x, probes[i].y, probes[i].what, p[0], p[1], p[2], p[3], ok ? "ok" : "FAIL");
                    if (!ok) fails++;
                }
                printf("status %ld, error %s\n", (long)cb.status, cb.error ? cb.error.localizedDescription.UTF8String : "none");
                if (cb.status != MTLCommandBufferStatusCompleted) fails++;
                printf("stage_in + texture + blending through Metal on RADV%s: %s\n", io ? " (IOSurface texture and target)" : "", fails ? "FAILED" : "all checks passed");
                return fails ? 1 : 0;
            }
            if (getenv("RN_BLIT_TEST")) {
                // The blit encoder through the official API: buffer to texture (row pitch 80 bytes for a 64-byte row), texture to texture (a sub-rectangle at an offset), texture to
                // buffer (pitch 96, offset 32), fillBuffer, buffer to buffer, and a copy into an IOSurface-backed texture (pitch 256) checked through the surface's base address.
                id<MTLDevice> d = (id<MTLDevice>)r;
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:8 mipmapped:NO];
                td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                id<MTLTexture> A = [d newTextureWithDescriptor:td], B = [d newTextureWithDescriptor:td];
                uint8_t zeros[16 * 8 * 4] = {0};
                [B replaceRegion:MTLRegionMake2D(0, 0, 16, 8) mipmapLevel:0 withBytes:zeros bytesPerRow:64];
                uint8_t srcData[80 * 8], expA[16 * 8 * 4], expB[16 * 8 * 4];
                for (int i = 0; i < 80 * 8; i++) srcData[i] = (uint8_t)(i * 7 + 3);
                for (int y = 0; y < 8; y++) memcpy(expA + y * 64, srcData + y * 80, 64);
                memset(expB, 0, sizeof(expB));
                for (int y = 0; y < 4; y++) memcpy(expB + ((3 + y) * 16 + 5) * 4, expA + ((1 + y) * 16 + 2) * 4, 8 * 4);
                id<MTLBuffer> sb = [d newBufferWithBytes:srcData length:sizeof(srcData) options:MTLResourceStorageModeShared];
                id<MTLBuffer> out = [d newBufferWithLength:96 * 8 + 32 options:MTLResourceStorageModeShared];
                id<MTLBuffer> f1 = [d newBufferWithLength:64 options:MTLResourceStorageModeShared], f2 = [d newBufferWithLength:64 options:MTLResourceStorageModeShared];
                memset(out.contents, 0x55, out.length); memset(f1.contents, 0x11, 64); memset(f2.contents, 0x22, 64);
                IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth: @16, (id)kIOSurfaceHeight: @8, (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @256, (id)kIOSurfacePixelFormat: @('RGBA')});
                IOSurfaceLock(surf, 0, NULL); memset(IOSurfaceGetBaseAddress(surf), 0x7f, IOSurfaceGetAllocSize(surf)); IOSurfaceUnlock(surf, 0, NULL);
                id<MTLTexture> S = surf ? [d newTextureWithDescriptor:td iosurface:surf plane:0] : nil;
                if (!A || !B || !sb || !out || !S) { printf("no resources\n"); return 80; }
                id<MTLCommandQueue> q = [d newCommandQueue];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                if (!be) { printf("no blit encoder\n"); return 81; }
                [be copyFromBuffer:sb sourceOffset:0 sourceBytesPerRow:80 sourceBytesPerImage:80 * 8 sourceSize:MTLSizeMake(16, 8, 1) toTexture:A destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                [be copyFromTexture:A sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(2, 1, 0) sourceSize:MTLSizeMake(8, 4, 1) toTexture:B destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(5, 3, 0)];
                [be copyFromTexture:B sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(16, 8, 1) toBuffer:out destinationOffset:32 destinationBytesPerRow:96 destinationBytesPerImage:96 * 8];
                [be fillBuffer:f1 range:NSMakeRange(8, 32) value:0xAB];
                [be copyFromBuffer:f1 sourceOffset:4 toBuffer:f2 destinationOffset:8 size:16];
                [be copyFromTexture:A toTexture:S];
                [be endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                int fails = 0;
                printf("status %ld, error %s\n", (long)cb.status, cb.error ? cb.error.localizedDescription.UTF8String : "none");
                if (cb.status != MTLCommandBufferStatusCompleted) fails++;
                const uint8_t *o = out.contents;
                int bad = 0;
                for (int i = 0; i < 32; i++) if (o[i] != 0x55) bad++;
                for (int y = 0; y < 8; y++) { for (int x = 0; x < 64; x++) if (o[32 + y * 96 + x] != expB[y * 64 + x]) bad++; for (int x = 64; x < 96; x++) if (o[32 + y * 96 + x] != 0x55) bad++; }
                printf("buffer -> texture -> texture -> buffer: %d bytes wrong\n", bad); fails += bad != 0;
                const uint8_t *g1 = f1.contents, *g2 = f2.contents;
                bad = 0;
                for (int i = 0; i < 64; i++) { if (g1[i] != ((i >= 8 && i < 40) ? 0xAB : 0x11)) bad++; }
                for (int i = 0; i < 64; i++) { uint8_t want = (i >= 8 && i < 24) ? g1[4 + i - 8] : 0x22; if (g2[i] != want) bad++; }
                printf("fillBuffer and buffer -> buffer: %d bytes wrong\n", bad); fails += bad != 0;
                bad = 0;
                IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
                for (int y = 0; y < 8; y++) for (int x = 0; x < 64; x++) if (((uint8_t *)IOSurfaceGetBaseAddress(surf))[y * IOSurfaceGetBytesPerRow(surf) + x] != expA[y * 64 + x]) bad++;
                IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
                printf("texture -> IOSurface texture: %d bytes wrong\n", bad); fails += bad != 0;
                printf("blit encoder through Metal on RADV: %s\n", fails ? "FAILED" : "all checks passed");
                return fails ? 1 : 0;
            }
            if (getenv("RN_TEX_TEST")) {
                // Texture kernels from a .metallib (mesa-bridge/test/vk_texcopy.metal) through the official API: a 20 x 12 RGBA8 texture, a grid of 3 x 2 threadgroups of 8 x 8 (bigger
                // than the texture: the kernels check the bounds). texture_copy adds 1 to the red channel (storage image written without format), texture_sample copies through the
                // sampler written in the shader.
                id<MTLDevice> d = (id<MTLDevice>)r;
                NSData *blob = [NSData dataWithContentsOfFile:@(getenv("RN_TEX_TEST"))];
                dispatch_data_t dd = dispatch_data_create(blob.bytes, blob.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
                NSError *e = nil;
                id<MTLLibrary> lib = [d newLibraryWithData:dd error:&e];
                if (!lib) { printf("no library\n"); return 40; }
                id<MTLCommandQueue> q = [d newCommandQueue];
                int failures = 0;
                for (int job = 0; job < 4; job++) {
                    NSString *name = job % 2 ? @"texture_sample" : @"texture_copy";
                    const int exact = job >= 2;                      // dispatchThreads: a grid of exactly 20 x 12 threads, so the edge threadgroups are smaller
                    id<MTLFunction> fn = [lib newFunctionWithName:name];
                    e = nil;
                    id<MTLComputePipelineState> pso = fn ? [d newComputePipelineStateWithFunction:fn error:&e] : nil;
                    printf("%s (%s): pipeline %p, error: %s\n", name.UTF8String, exact ? "dispatchThreads" : "dispatchThreadgroups", (__bridge void *)pso, e ? e.localizedDescription.UTF8String : "none");
                    if (!pso) { failures++; continue; }
                    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:20 height:12 mipmapped:NO];
                    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                    id<MTLTexture> src = [d newTextureWithDescriptor:td], dst = [d newTextureWithDescriptor:td];
                    if (!src || !dst) { printf("%s: no textures\n", name.UTF8String); failures++; continue; }
                    uint8_t in[20 * 12 * 4], out[20 * 12 * 4];
                    for (int y = 0; y < 12; y++) for (int x = 0; x < 20; x++) {
                        uint8_t *px = in + (y * 20 + x) * 4;
                        px[0] = (uint8_t)(x * 13 + y * 7); px[1] = (uint8_t)(x * 5); px[2] = (uint8_t)(y * 17); px[3] = 255 - x;
                    }
                    in[0] = 255;                                   // the red clamp at the top
                    [src replaceRegion:MTLRegionMake2D(0, 0, 20, 12) mipmapLevel:0 withBytes:in bytesPerRow:20 * 4];
                    id<MTLCommandBuffer> cb = [q commandBuffer];
                    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                    [enc setComputePipelineState:pso];
                    [enc setTexture:src atIndex:0];
                    [enc setTexture:dst atIndex:1];
                    if (exact) [enc dispatchThreads:MTLSizeMake(20, 12, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                    else [enc dispatchThreadgroups:MTLSizeMake(3, 2, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                    [enc endEncoding];
                    [cb commit]; [cb waitUntilCompleted];
                    [dst getBytes:out bytesPerRow:20 * 4 fromRegion:MTLRegionMake2D(0, 0, 20, 12) mipmapLevel:0];
                    unsigned bad = 0;
                    const int add = [name isEqualToString:@"texture_copy"] ? 1 : 0;
                    for (int i = 0; i < 20 * 12; i++) {
                        int rr = in[i * 4] + add; if (rr > 255) rr = 255;
                        if (out[i * 4] != rr || out[i * 4 + 1] != in[i * 4 + 1] || out[i * 4 + 2] != in[i * 4 + 2] || out[i * 4 + 3] != in[i * 4 + 3]) { if (bad < 3) printf("  texel %d: got %u %u %u %u, want %d %u %u %u\n", i, out[i*4], out[i*4+1], out[i*4+2], out[i*4+3], rr, in[i*4+1], in[i*4+2], in[i*4+3]); bad++; }
                    }
                    printf("%s (%s) through Metal on RADV: status %ld, error %s, %u of 240 texels wrong\n", name.UTF8String, exact ? "dispatchThreads" : "dispatchThreadgroups", (long)cb.status, cb.error ? cb.error.localizedDescription.UTF8String : "none", bad);
                    if (bad || cb.status != MTLCommandBufferStatusCompleted) failures++;
                }
                return failures;
            }
            if (getenv("RN_AIR_TEST")) {
                // A kernel from a .metallib through the official API, no SPIR-V hook: newLibraryWithData:error:, newFunctionWithName:, newComputePipelineStateWithFunction:error:.
                // With RN_AIR_FUNC the kernel is run like vk_dispatch.metal (buffer 0 = out, bytes at index 1 = add); without a translator the pipeline error is printed.
                id<MTLDevice> d = (id<MTLDevice>)r;
                NSData *blob = [NSData dataWithContentsOfFile:@(getenv("RN_AIR_TEST"))];
                dispatch_data_t dd = dispatch_data_create(blob.bytes, blob.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
                NSError *e = nil;
                id<MTLLibrary> lib = [d newLibraryWithData:dd error:&e];
                printf("library %p, %lu functions (%s)\n", (__bridge void *)lib, (unsigned long)lib.functionNames.count, e ? e.localizedDescription.UTF8String : "no error");
                if (!lib) return 30;
                const char *fname = getenv("RN_AIR_FUNC");
                NSString *want = fname ? @(fname) : nil;
                if (!want) for (NSString *n in lib.functionNames) { id<MTLFunction> f = [lib newFunctionWithName:n]; if (f.functionType == MTLFunctionTypeKernel) { want = n; break; } }
                id<MTLFunction> fn = [lib newFunctionWithName:want];
                printf("function %s -> %p type %ld\n", want.UTF8String, (__bridge void *)fn, fn ? (long)fn.functionType : -1L);
                if (!fn) return 31;
                id<MTLComputePipelineState> pso = [d newComputePipelineStateWithFunction:fn error:&e];
                printf("pipeline %p, error: %s\n", (__bridge void *)pso, e ? e.localizedDescription.UTF8String : "none");
                if (!pso || !fname) return pso ? 0 : 32;
                id<MTLCommandQueue> q = [d newCommandQueue];
                id<MTLBuffer> buf = [d newBufferWithLength:256 * 4 options:MTLResourceStorageModeShared];
                uint32_t *p = buf.contents;
                for (int i = 0; i < 256; i++) p[i] = 0xdeadbeef;
                uint32_t add = 7;
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:pso];
                [enc setBuffer:buf offset:0 atIndex:0];
                [enc setBytes:&add length:4 atIndex:1];
                [enc dispatchThreadgroups:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [enc endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                unsigned bad = 0;
                for (uint32_t i = 0; i < 256; i++) if (p[i] != i * 3u + add) bad++;
                printf("AIR kernel through Metal on RADV: status %ld, %u of 256 values wrong\n", (long)cb.status, bad);
                return bad ? 1 : 0;
            }
            if (getenv("RN_COMPUTE_TEST")) {
                // The compute path through the official Metal API (RedNaviMTL/compute.m), on RADV: v[i] = i * 3 + 7 over 4 threadgroups of 64.
                id<MTLDevice> d = (id<MTLDevice>)r;
                id<MTLCommandQueue> q = [d newCommandQueue];
                id<MTLBuffer> buf = [d newBufferWithLength:256 * 4 options:MTLResourceStorageModeShared];
                NSData *spv = [NSData dataWithBytes:vk_dispatch_spv length:vk_dispatch_spv_len];
                id<MTLComputePipelineState> pso = ((id (*)(id, SEL, NSData *, NSUInteger, NSUInteger))objc_msgSend)(d, sel_registerName("rn_newComputePipelineWithSPIRV:bufferCount:pushBytes:"), spv, 1, 4);
                printf("queue %p, buffer %p (length %lu), pipeline %p\n", (__bridge void *)q, (__bridge void *)buf, (unsigned long)buf.length, (__bridge void *)pso);
                if (!q || !buf || !pso) return 20;
                uint32_t *p = buf.contents;
                for (int i = 0; i < 256; i++) p[i] = 0xdeadbeef;
                uint32_t add = 7;
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:pso];
                [enc setBuffer:buf offset:0 atIndex:0];
                [enc setBytes:&add length:4 atIndex:1];
                [enc dispatchThreadgroups:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [enc endEncoding];
                __block int completed = 0;
                [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { completed = 1; }];
                [cb commit];
                [cb waitUntilCompleted];
                unsigned bad = 0;
                for (uint32_t i = 0; i < 256; i++) if (p[i] != i * 3u + add) bad++;
                printf("command buffer status %ld, completion handler %d, error %s\n", (long)cb.status, completed, cb.error ? cb.error.localizedDescription.UTF8String : "none");
                printf("compute through Metal on RADV: %u of 256 values wrong (v[255] = %u)\n", bad, p[255]);
                // Two buffers with offsets, bound with -setBuffers:offsets:withRange:. A (2048 bytes) holds a[i] = i from byte 512 on, B (2048 bytes) is written from byte 1024 on:
                // B[256 + i] = A[128 + i] * 2 + 5 for i < 128 (two threadgroups); everything else of B must keep its marker.
                id<MTLBuffer> A = [d newBufferWithLength:2048 options:MTLResourceStorageModeShared];
                id<MTLBuffer> B = [d newBufferWithLength:2048 options:MTLResourceStorageModeShared];
                id<MTLComputePipelineState> pso2 = ((id (*)(id, SEL, NSData *, NSUInteger, NSUInteger))objc_msgSend)(d, sel_registerName("rn_newComputePipelineWithSPIRV:bufferCount:pushBytes:"),
                    [NSData dataWithBytes:vk_copy2_spv length:vk_copy2_spv_len], 2, 4);
                uint32_t *pa = A.contents, *pb = B.contents;
                for (int i = 0; i < 512; i++) { pa[i] = 0xaaaa0000u + i; pb[i] = 0xbbbbbbbb; }
                for (int i = 0; i < 128; i++) pa[128 + i] = i;
                uint32_t add2 = 5;
                id<MTLCommandBuffer> cb2 = [q commandBuffer];
                id<MTLComputeCommandEncoder> e2 = [cb2 computeCommandEncoder];
                [e2 setComputePipelineState:pso2];
                id<MTLBuffer> bl[2] = {A, B};
                NSUInteger ol[2] = {512, 1024};
                [e2 setBuffers:bl offsets:ol withRange:NSMakeRange(0, 2)];
                [e2 setBytes:&add2 length:4 atIndex:2];
                [e2 dispatchThreadgroups:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [e2 endEncoding];
                [cb2 commit]; [cb2 waitUntilCompleted];
                unsigned bad2 = 0;
                for (int i = 0; i < 512; i++) {
                    uint32_t want = (i >= 256 && i < 384) ? (uint32_t)(i - 256) * 2u + add2 : 0xbbbbbbbbu;
                    if (pb[i] != want) bad2++;
                }
                printf("two buffers with offsets through setBuffers:offsets:withRange: status %ld: %u of 512 words wrong (B[256] = %u, B[383] = %u, B[384] = 0x%x)\n", (long)cb2.status, bad2, pb[256], pb[383], pb[384]);
                // An offset that is not a multiple of the alignment must fail the command buffer, not the kernel.
                id<MTLCommandBuffer> cb3 = [q commandBuffer];
                id<MTLComputeCommandEncoder> e3 = [cb3 computeCommandEncoder];
                [e3 setComputePipelineState:pso2];
                [e3 setBuffer:A offset:2 atIndex:0];
                [e3 setBuffer:B offset:0 atIndex:1];
                [e3 setBytes:&add2 length:4 atIndex:2];
                [e3 dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [e3 endEncoding];
                [cb3 commit]; [cb3 waitUntilCompleted];
                printf("misaligned offset: status %ld (5 = error), error: %s\n", (long)cb3.status, cb3.error ? cb3.error.localizedDescription.UTF8String : "none");
                return (bad || bad2 || cb2.status != MTLCommandBufferStatusCompleted || cb3.status != MTLCommandBufferStatusError) ? 1 : 0;
            }
            // The command queue: the plug-in class answers nil to newCommandQueue*, so the implementation of MTLIOAccelDevice is called directly.
            SEL qs = sel_registerName("newCommandQueueWithDescriptor:");
            Method qm = class_getInstanceMethod(objc_getClass("MTLIOAccelDevice"), qs);
            if (qm && getenv("RN_QUEUE")) {
                id desc = [[objc_getClass("MTLCommandQueueDescriptor") alloc] init];
                fprintf(stderr, "--- newCommandQueueWithDescriptor: (MTLIOAccelDevice's own)\n");
                id q = ((id (*)(id, SEL, id))method_getImplementation(qm))(r, qs, desc);
                printf("queue -> %p class %s\n", (__bridge void *)q, q ? class_getName(object_getClass(q)) : "-");
            }
        }
    }
    return 0;
}
