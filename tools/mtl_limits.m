// Prints the limits, alignments and sizes a Metal device reports, for the real device (`system`) and for the RedNavi plug-in (a bundle path, loaded as in ca_renderer_test), so the two can be diffed.
// QuartzCore sizes and aligns its buffer arenas from these. usage: mtl_limits <RedNaviMTLDriver.bundle|system>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <IOKit/IOKitLib.h>

typedef struct { MTLSize a; } sz;
int main(int argc, char **argv) {
    @autoreleasepool {
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
        printf("device %s class %s\n", dev.name.UTF8String, class_getName(object_getClass(dev)));
        // NSUInteger-returning getters
        const char *u[] = {"maxBufferLength", "maxThreadgroupMemoryLength", "maxArgumentBufferSamplerCount", "minimumLinearTextureAlignmentForPixelFormat:", "minimumTextureBufferAlignmentForPixelFormat:",
            "minBufferArgumentOffsetAlignment", "minimumBufferOffsetAlignment", "constantBufferOffsetAlignment", "bufferOffsetAlignment", "maxConstantBufferArgumentTableSize", "maxFragmentBufferArguments", "maxVertexBufferArguments",
            "maxTextureBufferWidth", "currentAllocatedSize", "recommendedMaxWorkingSetSize", "sparseTileSizeInBytes", "maxColorAttachments", "vertexBufferAlignment", "fragmentBufferAlignment", "bufferAlignment",
            "maxTextureDimension", "pageSize", "preferredBufferAlignment", "bufferPageAlignment", "linearTextureAlignment", "minimumTextureBufferAlignment", "constantBufferAlignment", "storageBufferAlignment", "uniformBufferAlignment",
            "maxVertexAttributes", "maxVertexBuffers", NULL};
        for (int i = 0; u[i]; i++) {
            SEL s = sel_registerName(u[i]);
            if (![dev respondsToSelector:s]) continue;
            const BOOL arg = strchr(u[i], ':') != NULL;
            NSUInteger v = arg ? ((NSUInteger (*)(id, SEL, NSUInteger))objc_msgSend)(dev, s, MTLPixelFormatBGRA8Unorm) : ((NSUInteger (*)(id, SEL))objc_msgSend)(dev, s);
            printf("%-52s %lu\n", u[i], (unsigned long)v);
        }
        // everything private with "align" / "Limit" / "max" / "min" in the name that takes no argument and returns an integer-like value (type encoding Q, q, I, i, L, B)
        unsigned n; 
        for (Class c = object_getClass(dev); c && c != [NSObject class]; c = class_getSuperclass(c)) {
            Method *ms = class_copyMethodList(c, &n);
            for (unsigned i = 0; i < n; i++) {
                SEL s = method_getName(ms[i]); const char *nm = sel_getName(s);
                if (strchr(nm, ':')) continue;
                if (!strcasestr(nm, "align") && !strcasestr(nm, "maxBuf") && !strcasestr(nm, "arena") && !strcasestr(nm, "offsetAlign")) continue;
                char *t = method_copyReturnType(ms[i]);
                if (t[0] == 'Q' || t[0] == 'q') printf("[%s] %-48s %lu\n", class_getName(c), nm, (unsigned long)((NSUInteger (*)(id, SEL))objc_msgSend)(dev, s));
                free(t);
            }
            free(ms);
        }
        // every boolean property of the device (supports..., is..., has...): QuartzCore picks its data formats (half or float colour parameters, say) from them
        for (Class c = object_getClass(dev); c && c != [NSObject class]; c = class_getSuperclass(c)) {
            Method *ms = class_copyMethodList(c, &n);
            for (unsigned i = 0; i < n; i++) {
                SEL s = method_getName(ms[i]); const char *nm = sel_getName(s);
                if (strchr(nm, ':') || strncmp(nm, "_", 1) == 0) continue;
                char *t = method_copyReturnType(ms[i]);
                if (t[0] == 'B' || t[0] == 'c') printf("bool %-60s %d\n", nm, (int)((BOOL (*)(id, SEL))objc_msgSend)(dev, s));
                free(t);
            }
            free(ms);
        }
        // buffer sizes and alignments
        const NSUInteger lens[] = {6144, 12288, 262144, 1};
        for (int i = 0; i < 4; i++) for (NSUInteger opt = 0; opt <= 0x10; opt += 0x10) {
            MTLSizeAndAlign sa = [dev heapBufferSizeAndAlignWithLength:lens[i] options:opt];
            printf("heapBufferSizeAndAlign len %lu options 0x%lx: size %lu align %lu\n", (unsigned long)lens[i], (unsigned long)opt, (unsigned long)sa.size, (unsigned long)sa.align);
        }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256 mipmapped:NO];
        MTLSizeAndAlign ta = [dev heapTextureSizeAndAlignWithDescriptor:td];
        printf("heapTextureSizeAndAlign 256x256 BGRA8: size %lu align %lu\n", (unsigned long)ta.size, (unsigned long)ta.align);
        // the buffers themselves: length asked and what comes back
        for (int i = 0; i < 4; i++) {
            id<MTLBuffer> bf = [dev newBufferWithLength:lens[i] options:MTLResourceStorageModeManaged];
            printf("newBuffer len %lu: length %lu allocatedSize %lu contents %s\n", (unsigned long)lens[i], (unsigned long)bf.length, (unsigned long)bf.allocatedSize, ((uintptr_t)bf.contents & 0xfff) ? "not page aligned" : "page aligned");
        }
        printf("maxThreadsPerThreadgroup %lux%lux%lu\n", (unsigned long)dev.maxThreadsPerThreadgroup.width, (unsigned long)dev.maxThreadsPerThreadgroup.height, (unsigned long)dev.maxThreadsPerThreadgroup.depth);
        printf("hasUnifiedMemory %d isLowPower %d isHeadless %d isRemovable %d\n", dev.hasUnifiedMemory, dev.isLowPower, dev.isHeadless, dev.isRemovable);
    }
    return 0;
}
