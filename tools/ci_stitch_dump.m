// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// What CoreImage asks of a Metal device when it compiles a filter graph: run on a Mac with a real GPU (the system device), it wraps the device's stitching and pipeline methods
// and prints the stitched-library descriptors (functions, graphs, nodes, arguments), the libraries' function names and the compute pipeline descriptors (kernel, linked functions)
// that CoreImage builds for the same filter chain as tools/ci_test.m. Only the public Metal API is read. usage: ci_stitch_dump [filter]   (default CIColorInvert)
#import <Cocoa/Cocoa.h>
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>

static NSArray *g_nodes;                // the nodes of the graph being printed: a function node that is one of them prints as #index
static NSString *node_str(id n) {
    NSUInteger at = g_nodes ? [g_nodes indexOfObjectIdenticalTo:n] : NSNotFound;
    if (at != NSNotFound) return [NSString stringWithFormat:@"#%lu", (unsigned long)at];
    const char *cn = class_getName(object_getClass(n));
    if ([n isKindOfClass:[MTLFunctionStitchingInputNode class]]) return [NSString stringWithFormat:@"in%lu", (unsigned long)((MTLFunctionStitchingInputNode *)n).argumentIndex];
    if (!strcmp(cn, "MTLFunctionStitchingInputBufferAddress"))
        return [NSString stringWithFormat:@"buf%@+%@%@", [n valueForKey:@"bindIndex"], [n valueForKey:@"byteOffset"], [[n valueForKey:@"dereference"] boolValue] ? @"*" : @""];
    if (!strcmp(cn, "MTLFunctionStitchingInputBuffer")) return [NSString stringWithFormat:@"buffer%@", [n valueForKey:@"bindIndex"]];
    if (!strcmp(cn, "MTLFunctionStitchingInputTexture")) return [NSString stringWithFormat:@"tex%@", [n valueForKey:@"bindIndex"]];
    if (!strcmp(cn, "MTLFunctionStitchingInputSampler")) return [NSString stringWithFormat:@"smp%@", [n valueForKey:@"bindIndex"]];
    if (!strcmp(cn, "MTLFunctionStitchingInputThreadgroup")) return [NSString stringWithFormat:@"tg%@", [n valueForKey:@"bindIndex"]];
    if (!strcmp(cn, "MTLFunctionStitchingBuiltinThreadPositionInGrid")) return @"tpig";
    if (!strcmp(cn, "MTLFunctionStitchingBuiltinThreadPositionInThreadgroup")) return @"tpitg";
    if ([n isKindOfClass:[MTLFunctionStitchingFunctionNode class]]) {
        MTLFunctionStitchingFunctionNode *f = n;
        NSMutableArray *a = [NSMutableArray array];
        for (id x in f.arguments) [a addObject:node_str(x)];
        NSMutableArray *c = [NSMutableArray array];
        for (id x in f.controlDependencies) [c addObject:node_str(x)];
        NSString *extra = @"";
        if ([f respondsToSelector:@selector(condition)]) extra = [NSString stringWithFormat:@" EARLY-RETURN if %@", node_str([(id)f valueForKey:@"condition"])];
        else if ([f respondsToSelector:@selector(isEarlyReturn)] && [[(id)f valueForKey:@"isEarlyReturn"] boolValue]) extra = @" isEarlyReturn";
        return [NSString stringWithFormat:@"%s %@(%@)%@%@", cn, f.name, [a componentsJoinedByString:@", "], c.count ? [NSString stringWithFormat:@" after[%@]", [c componentsJoinedByString:@","]] : @"", extra];
    }
    return [NSString stringWithFormat:@"<%s>", cn];
}

static void dump_stitched(MTLStitchedLibraryDescriptor *d) {
    printf("== stitched library: %lu functions, %lu graphs, options %lu\n", (unsigned long)d.functions.count, (unsigned long)d.functionGraphs.count, (unsigned long)d.options);
    for (id<MTLFunction> f in d.functions) printf("  function %s type %ld\n", f.name.UTF8String, (long)f.functionType);
    for (MTLFunctionStitchingGraph *g in d.functionGraphs) {
        printf("  graph %s (%s): %lu nodes, %lu attributes\n", g.functionName.UTF8String, class_getName(object_getClass(g)), (unsigned long)g.nodes.count, (unsigned long)g.attributes.count);
        g_nodes = g.nodes;
        for (NSUInteger i = 0; i < g.nodes.count; i++) { g_nodes = nil; NSString *s = nil; g_nodes = g.nodes; NSArray *keep = g_nodes; g_nodes = [keep subarrayWithRange:NSMakeRange(0, i)]; s = node_str(keep[i]); g_nodes = keep; printf("    #%lu %s\n", (unsigned long)i, s.UTF8String); }
        if (g.outputNode) printf("    output %s\n", node_str(g.outputNode).UTF8String);
        for (id a in g.attributes) printf("    attribute %s\n", class_getName(object_getClass(a)));
        g_nodes = nil;
    }
}

static void dump_function(const char *what, id<MTLFunction> f) {
    if (!f) return;
    printf("  %s %s type %ld\n", what, f.name.UTF8String, (long)f.functionType);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { puts("no device"); return 4; }
        printf("device %s (%s)\n", dev.name.UTF8String, class_getName(object_getClass(dev)));
        Class k = object_getClass(dev);
        // the wrappers: every original is called; only what passes is printed
        {
            SEL s = @selector(newLibraryWithStitchedDescriptor:error:);
            Method m = class_getInstanceMethod(k, s);
            id (*orig)(id, SEL, id, NSError **) = (void *)method_getImplementation(m);
            method_setImplementation(m, imp_implementationWithBlock(^id(id self, MTLStitchedLibraryDescriptor *d, NSError **e) {
                dump_stitched(d);
                id<MTLLibrary> l = orig(self, s, d, e);
                printf("  -> library %p functions [%s]\n", l, [l.functionNames componentsJoinedByString:@" "].UTF8String);
                return l;
            }));
        }
        {
            SEL s = @selector(newLibraryWithStitchedDescriptor:completionHandler:);
            Method m = class_getInstanceMethod(k, s);
            void (*orig)(id, SEL, id, id) = (void *)method_getImplementation(m);
            method_setImplementation(m, imp_implementationWithBlock(^(id self, MTLStitchedLibraryDescriptor *d, void (^h)(id<MTLLibrary>, NSError *)) {
                dump_stitched(d);
                orig(self, s, d, ^(id<MTLLibrary> l, NSError *e) { printf("  -> library (async) %p functions [%s]\n", l, [l.functionNames componentsJoinedByString:@" "].UTF8String); h(l, e); });
            }));
        }
        void (^dump_cpd)(MTLComputePipelineDescriptor *) = ^(MTLComputePipelineDescriptor *d) {
            printf("== compute pipeline descriptor\n");
            dump_function("kernel", d.computeFunction);
            MTLLinkedFunctions *lf = d.linkedFunctions;
            for (id<MTLFunction> f in lf.functions) dump_function("linked", f);
            for (id<MTLFunction> f in lf.privateFunctions) dump_function("private", f);
            for (id<MTLFunction> f in lf.binaryFunctions) dump_function("binary", f);
            for (NSString *g in lf.groups) printf("  group %s: %lu\n", g.UTF8String, (unsigned long)lf.groups[g].count);
            printf("  preloaded libraries %lu, maxCallStackDepth %lu, supportAddingBinaryFunctions %d\n", (unsigned long)d.preloadedLibraries.count, (unsigned long)d.maxCallStackDepth, d.supportAddingBinaryFunctions);
        };
        {
            SEL s = @selector(newComputePipelineStateWithDescriptor:options:reflection:error:);
            Method m = class_getInstanceMethod(k, s);
            id (*orig)(id, SEL, id, NSUInteger, void *, NSError **) = (void *)method_getImplementation(m);
            method_setImplementation(m, imp_implementationWithBlock(^id(id self, MTLComputePipelineDescriptor *d, NSUInteger o, void *r, NSError **e) {
                dump_cpd(d);
                return orig(self, s, d, o, r, e);
            }));
        }
        {
            SEL s = @selector(newComputePipelineStateWithDescriptor:options:completionHandler:);
            Method m = class_getInstanceMethod(k, s);
            void (*orig)(id, SEL, id, NSUInteger, id) = (void *)method_getImplementation(m);
            method_setImplementation(m, imp_implementationWithBlock(^(id self, MTLComputePipelineDescriptor *d, NSUInteger o, id h) {
                dump_cpd(d);
                orig(self, s, d, o, h);
            }));
        }
        {
            SEL s = @selector(newComputePipelineStateWithFunction:error:);
            Method m = class_getInstanceMethod(k, s);
            id (*orig)(id, SEL, id, NSError **) = (void *)method_getImplementation(m);
            method_setImplementation(m, imp_implementationWithBlock(^id(id self, id<MTLFunction> f, NSError **e) {
                printf("== compute pipeline from function\n"); dump_function("kernel", f);
                return orig(self, s, f, e);
            }));
        }
        {   // the textures CoreImage makes and fills (a colour cube is a 2D texture of stacked slices; how it is filled decides what the plug-in must support)
            SEL s = @selector(newTextureWithDescriptor:);
            Method m = class_getInstanceMethod(k, s);
            id (*orig)(id, SEL, MTLTextureDescriptor *) = (void *)method_getImplementation(m);
            method_setImplementation(m, imp_implementationWithBlock(^id(id self, MTLTextureDescriptor *d) {
                printf("== newTexture type %lu format %lu %lux%lux%lu usage 0x%lx storage %lu mips %lu\n", (unsigned long)d.textureType, (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.depth, (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned long)d.mipmapLevelCount);
                return orig(self, s, d);
            }));
        }
        {
            SEL s = @selector(newTextureWithDescriptor:iosurface:plane:);
            Method m = class_getInstanceMethod(k, s);
            id (*orig)(id, SEL, MTLTextureDescriptor *, void *, NSUInteger) = (void *)method_getImplementation(m);
            method_setImplementation(m, imp_implementationWithBlock(^id(id self, MTLTextureDescriptor *d, void *surf, NSUInteger plane) {
                printf("== newTexture from IOSurface type %lu format %lu %lux%lu plane %lu\n", (unsigned long)d.textureType, (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)plane);
                return orig(self, s, d, surf, plane);
            }));
            s = @selector(newTextureWithDescriptor:buffer:offset:bytesPerRow:);
            m = class_getInstanceMethod(k, s);
            id (*orig2)(id, SEL, MTLTextureDescriptor *, id, NSUInteger, NSUInteger) = (void *)method_getImplementation(m);
            method_setImplementation(m, imp_implementationWithBlock(^id(id self, MTLTextureDescriptor *d, id b, NSUInteger off, NSUInteger bpr) {
                printf("== newTexture over buffer type %lu format %lu %lux%lu offset %lu bpr %lu\n", (unsigned long)d.textureType, (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)off, (unsigned long)bpr);
                return orig2(self, s, d, b, off, bpr);
            }));
        }
        CIContext *ctx = [CIContext contextWithMTLDevice:dev options:@{kCIContextWorkingColorSpace: (__bridge id)CGColorSpaceCreateWithName(kCGColorSpaceSRGB), kCIContextCacheIntermediates: @NO}];
        CIImage *src = [[CIImage imageWithColor:[CIColor colorWithRed:1 green:0.5 blue:0 alpha:1]] imageByCroppingToRect:CGRectMake(0, 0, 64, 64)];
        const char *which = argc > 1 ? argv[1] : "CIColorInvert";
        CIFilter *f = [CIFilter filterWithName:@(which)];
        [f setValue:src forKey:kCIInputImageKey];
        if ([f.inputKeys containsObject:kCIInputRadiusKey]) [f setValue:@4 forKey:kCIInputRadiusKey];
        CIFilter *cc = [CIFilter filterWithName:@"CIColorControls" withInputParameters:@{kCIInputImageKey: f.outputImage, kCIInputSaturationKey: @1.4, kCIInputBrightnessKey: @0.1}];
        CIFilter *vg = [CIFilter filterWithName:@"CIVignette" withInputParameters:@{kCIInputImageKey: cc.outputImage, kCIInputIntensityKey: @0.5, kCIInputRadiusKey: @2}];
        CIImage *out = [vg.outputImage imageByCroppingToRect:CGRectMake(0, 0, 64, 64)];
        uint8_t px[64 * 64 * 4] = {0};
        [ctx render:out toBitmap:px rowBytes:64 * 4 bounds:CGRectMake(0, 0, 64, 64) format:kCIFormatRGBA8 colorSpace:CGColorSpaceCreateWithName(kCGColorSpaceSRGB)];
        printf("pixel (32,32) = %u %u %u %u\n", px[(32 * 64 + 32) * 4], px[(32 * 64 + 32) * 4 + 1], px[(32 * 64 + 32) * 4 + 2], px[(32 * 64 + 32) * 4 + 3]);
    }
    return 0;
}
