// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// What MetalPerformanceShaders asks of a Metal device when MPSGraph runs a convolution with a bias and a clamp (ReLU6), the kind of graph that made Geekbench's Background Blur call the
// private -[_MTLDevice newLibraryWithDAG:functions:error:]. Run on a Mac with a real GPU (the system device): it wraps the device's DAG, function and pipeline methods and prints the DAG
// text, the functions given with it (name, specialized name, type), the functions MPS then takes from the returned library and the compute pipeline descriptors it builds (kernel,
// linked functions). usage: mps_dag_dump
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShadersGraph/MetalPerformanceShadersGraph.h>
#import <objc/runtime.h>
#import <objc/message.h>

static NSHashTable *g_dagLibs;           // libraries newLibraryWithDAG returned

// The values of a constant set, through the private accessors the plug-in also reads (index or name = bytes in hex).
static void print_constants(MTLFunctionConstantValues *v) {
    if (!v) return;
    SEL si = sel_registerName("newIndexedConstantArray"), sn = sel_registerName("newNamedConstantArray");
    if (![v respondsToSelector:si]) return;
    for (int k = 0; k < 2; k++) for (id c in ((id (*)(id, SEL))objc_msgSend)(v, k ? sn : si)) {
        NSUInteger t = ((NSUInteger (*)(id, SEL))objc_msgSend)(c, @selector(dataType));
        const uint8_t *p = ((const uint8_t *(*)(id, SEL))objc_msgSend)(c, @selector(data));
        unsigned n = t == MTLDataTypeBool || t == MTLDataTypeChar || t == MTLDataTypeUChar ? 1 : t == MTLDataTypeShort || t == MTLDataTypeUShort || t == MTLDataTypeHalf ? 2 : t >= 81 ? 8 : 4;
        char hex[40] = ""; for (unsigned i = 0; i < n && p; i++) snprintf(hex + 2 * i, 3, "%02x", p[i]);
        if (k) printf("      constant %s type %lu = %s\n", [((NSString *(*)(id, SEL))objc_msgSend)(c, @selector(name)) UTF8String], (unsigned long)t, hex);
        else printf("      constant %lu type %lu = %s\n", (unsigned long)((NSUInteger (*)(id, SEL))objc_msgSend)(c, @selector(index)), (unsigned long)t, hex);
    }
}

static void fn_line(const char *what, id f) {
    if (!f) { printf("  %s (nil)\n", what); return; }
    printf("  %s %s type %ld class %s\n", what, [[f name] UTF8String], (long)[f functionType], class_getName(object_getClass(f)));
}

static void dump_linked(MTLLinkedFunctions *lf) {
    if (!lf) return;
    for (id f in lf.functions) fn_line("  linked function", f);
    for (id f in lf.binaryFunctions) fn_line("  linked binary function", f);
    for (id f in lf.privateFunctions) fn_line("  linked private function", f);
    for (NSString *k in lf.groups) printf("    group %s: %lu functions\n", k.UTF8String, (unsigned long)[lf.groups[k] count]);
}

// Replaces the instance method `sel` of class `c` with a block; returns the old implementation.
static IMP wrap(Class c, SEL sel, id block) {
    Method m = class_getInstanceMethod(c, sel);
    if (!m) { printf("(no %s on %s)\n", sel_getName(sel), class_getName(c)); return NULL; }
    IMP old = method_getImplementation(m);
    if (!class_addMethod(c, sel, imp_implementationWithBlock(block), method_getTypeEncoding(m))) method_setImplementation(m, imp_implementationWithBlock(block));
    return old;
}

static void hook_library(Class lc) {
    static NSMutableSet *done; if (!done) done = [NSMutableSet set];
    if ([done containsObject:lc]) return;
    [done addObject:lc];
    SEL s1 = @selector(newFunctionWithName:);
    __block IMP o1 = wrap(lc, s1, ^id(id self, NSString *n) {
        id f = ((id (*)(id, SEL, id))o1)(self, s1, n);
        if ([g_dagLibs containsObject:self]) { printf("== DAG library %p newFunctionWithName:%s\n", self, n.UTF8String); fn_line("->", f); }
        return f;
    });
    SEL s2 = @selector(newFunctionWithDescriptor:error:);
    __block IMP o2 = wrap(lc, s2, ^id(id self, MTLFunctionDescriptor *d, NSError **e) {
        id f = ((id (*)(id, SEL, id, NSError **))o2)(self, s2, d, e);
        printf("== %slibrary %p newFunctionWithDescriptor: name %s specializedName %s constants %s\n", [g_dagLibs containsObject:self] ? "DAG " : "", self, d.name.UTF8String,
               d.specializedName.UTF8String ?: "-", d.constantValues ? "yes" : "no");
        print_constants(d.constantValues);
        fn_line("->", f);
        return f;
    });
    SEL s3 = @selector(newFunctionWithName:constantValues:error:);
    __block IMP o3 = wrap(lc, s3, ^id(id self, NSString *n, MTLFunctionConstantValues *cv, NSError **e) {
        id f = ((id (*)(id, SEL, id, id, NSError **))o3)(self, s3, n, cv, e);
        printf("== %slibrary %p newFunctionWithName:%s constantValues:\n", [g_dagLibs containsObject:self] ? "DAG " : "", self, n.UTF8String);
        print_constants(cv);
        fn_line("->", f);
        return f;
    });
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);                // line by line: the output is often a pipe or a file
    @autoreleasepool {
        g_dagLibs = [NSHashTable weakObjectsHashTable];
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        printf("device %s (%s)\n", dev.name.UTF8String, class_getName(object_getClass(dev)));
        Class dc = object_getClass(dev);
        SEL sd = sel_registerName("newLibraryWithDAG:functions:error:");
        __block IMP od = wrap(dc, sd, ^id(id self, NSString *dag, NSArray *fns, NSError **e) {
            printf("== newLibraryWithDAG: %lu functions\n", (unsigned long)fns.count);
            for (id f in fns) fn_line("function", f);
            printf("%s\n", [dag description].UTF8String);
            id lib = ((id (*)(id, SEL, id, id, NSError **))od)(self, sd, dag, fns, e);
            printf("== -> library %p class %s functionNames %s\n", lib, class_getName(object_getClass(lib)), [[[lib functionNames] description] UTF8String]);
            if (lib) { [g_dagLibs addObject:lib]; hook_library(object_getClass(lib)); }
            return lib;
        });
        SEL ss = sel_registerName("newLibraryWithStitchedDescriptor:error:");
        __block IMP os = wrap(dc, ss, ^id(id self, id d, NSError **e) {
            NSArray *fs = [d valueForKey:@"functions"];
            printf("== newLibraryWithStitchedDescriptor: %lu functions\n", (unsigned long)fs.count);
            for (id f in fs) fn_line("function", f);
            for (id g in [d valueForKey:@"functionGraphs"]) {
                printf("  graph %s:", [[g valueForKey:@"functionName"] UTF8String]);
                for (id n in [g valueForKey:@"nodes"]) if ([n respondsToSelector:@selector(name)]) printf(" %s", [[n valueForKey:@"name"] UTF8String]);
                printf("\n");
            }
            id lib = ((id (*)(id, SEL, id, NSError **))os)(self, ss, d, e);
            printf("== -> library %p functionNames %s\n", lib, [[[lib functionNames] description] UTF8String]);
            if (lib) hook_library(object_getClass(lib));
            return lib;
        });
        SEL sl = @selector(newLibraryWithData:error:);
        __block IMP ol = wrap(dc, sl, ^id(id self, dispatch_data_t d, NSError **e) {
            id lib = ((id (*)(id, SEL, id, NSError **))ol)(self, sl, d, e);
            if (lib) hook_library(object_getClass(lib));
            return lib;
        });
        SEL su = @selector(newLibraryWithURL:error:);
        __block IMP ou = wrap(dc, su, ^id(id self, NSURL *u, NSError **e) {
            id lib = ((id (*)(id, SEL, id, NSError **))ou)(self, su, u, e);
            if (lib) hook_library(object_getClass(lib));
            return lib;
        });
        SEL sp = @selector(newComputePipelineStateWithDescriptor:options:reflection:error:);
        __block IMP op = wrap(dc, sp, ^id(id self, MTLComputePipelineDescriptor *d, NSUInteger o, id *r, NSError **e) {
            printf("== compute pipeline (descriptor, options %lu)\n", (unsigned long)o);
            fn_line("kernel", d.computeFunction);
            dump_linked(d.linkedFunctions);
            for (id l in d.preloadedLibraries) printf("    preloaded library %s %s\n", class_getName(object_getClass(l)), [[[l functionNames] description] UTF8String]);
            return ((id (*)(id, SEL, id, NSUInteger, id *, NSError **))op)(self, sp, d, o, r, e);
        });
        SEL sa = @selector(newComputePipelineStateWithDescriptor:options:completionHandler:);
        __block IMP oa = wrap(dc, sa, ^void(id self, MTLComputePipelineDescriptor *d, NSUInteger o, id h) {
            printf("== compute pipeline (descriptor, async, options %lu)\n", (unsigned long)o);
            fn_line("kernel", d.computeFunction);
            dump_linked(d.linkedFunctions);
            ((void (*)(id, SEL, id, NSUInteger, id))oa)(self, sa, d, o, h);
        });
        SEL sf = @selector(newComputePipelineStateWithFunction:error:);
        __block IMP of = wrap(dc, sf, ^id(id self, id f, NSError **e) {
            printf("== compute pipeline (function)\n"); fn_line("kernel", f);
            return ((id (*)(id, SEL, id, NSError **))of)(self, sf, f, e);
        });

        // conv 3x3 (NHWC 1x32x32x8, 8 to 16 channels) + bias + clamp(0, 6)
        MPSGraph *g = [MPSGraph new];
        MPSGraphTensor *x = [g placeholderWithShape:@[@1, @32, @32, @8] dataType:MPSDataTypeFloat32 name:@"x"];
        NSMutableData *wd = [NSMutableData dataWithLength:3 * 3 * 8 * 16 * 4];
        float *wp = wd.mutableBytes; for (int i = 0; i < 3 * 3 * 8 * 16; i++) wp[i] = (float)((i % 7) - 3) * 0.05f;
        MPSGraphTensor *w = [g constantWithData:wd shape:@[@3, @3, @8, @16] dataType:MPSDataTypeFloat32];
        MPSGraphConvolution2DOpDescriptor *cd = [MPSGraphConvolution2DOpDescriptor descriptorWithStrideInX:1 strideInY:1 dilationRateInX:1 dilationRateInY:1 groups:1
            paddingStyle:MPSGraphPaddingStyleTF_SAME dataLayout:MPSGraphTensorNamedDataLayoutNHWC weightsLayout:MPSGraphTensorNamedDataLayoutHWIO];
        MPSGraphTensor *c = [g convolution2DWithSourceTensor:x weightsTensor:w descriptor:cd name:nil];
        MPSGraphTensor *b = [g constantWithScalar:0.25 shape:@[@16] dataType:MPSDataTypeFloat32];
        MPSGraphTensor *y = [g clampWithTensor:[g additionWithPrimaryTensor:c secondaryTensor:b name:nil]
                                minValueTensor:[g constantWithScalar:0 dataType:MPSDataTypeFloat32] maxValueTensor:[g constantWithScalar:6 dataType:MPSDataTypeFloat32] name:nil];
        NSMutableData *xd = [NSMutableData dataWithLength:32 * 32 * 8 * 4];
        float *xp = xd.mutableBytes; for (int i = 0; i < 32 * 32 * 8; i++) xp[i] = (float)(i % 13) * 0.1f;
        MPSGraphDevice *gd = [MPSGraphDevice deviceWithMTLDevice:dev];
        MPSGraphTensorData *xt = [[MPSGraphTensorData alloc] initWithDevice:gd data:xd shape:@[@1, @32, @32, @8] dataType:MPSDataTypeFloat32];
        MPSGraphTensor *r = [g reductionSumWithTensor:y axes:@[@1, @2] name:nil];      // a reduction (MPS stitches reductionStitched kernels for it)
        NSDictionary *res = [g runWithMTLCommandQueue:[dev newCommandQueue] feeds:@{x: xt} targetTensors:@[y, r] targetOperations:nil];
        // readBytes writes the whole tensor (1x32x32x16)
        float *out = calloc(32 * 32 * 16, sizeof(float));
        [[res[y] mpsndarray] readBytes:out strideBytes:nil];
        double sum = 0;
        for (int i = 0; i < 32 * 32 * 16; i++) sum += out[i];
        printf("result %g %g %g %g ... %g, sum %.4f\n", out[0], out[1], out[2], out[3], out[32 * 32 * 16 - 1], sum);
        free(out);
        float rs[16] = {0};
        [[res[r] mpsndarray] readBytes:rs strideBytes:nil];
        printf("reduction %g %g ... %g\n", rs[0], rs[1], rs[15]);
    }
    return 0;
}
