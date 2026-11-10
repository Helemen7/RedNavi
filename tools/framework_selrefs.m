// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Read-only: lists the selectors a system framework references (its __objc_selrefs) and which of them belong to Metal protocols but are not implemented by the RedNavi classes.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <dlfcn.h>

static NSMutableSet *selrefs_of(const char *path) {
    NSMutableSet *set = [NSMutableSet set];
    void *h = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = dlopen(path, RTLD_LAZY);
    if (!h) { fprintf(stderr, "cannot load %s\n", path); return set; }
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *name = _dyld_get_image_name(i);
        if (!name || strcmp(name, path) != 0) {
            // the cache reports the real path of a framework; compare by suffix of the last component
            const char *a = strrchr(name ? name : "", '/'), *b = strrchr(path, '/');
            if (!a || !b || strcmp(a, b) != 0) continue;
        }
        const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
        const char *segs[] = {"__DATA", "__DATA_CONST", "__DATA_DIRTY", "__AUTH_CONST", "__AUTH"};
        for (int k = 0; k < 5; k++) {
            unsigned long size = 0;
            uint8_t *p = getsectiondata(mh, segs[k], "__objc_selrefs", &size);
            if (!p) continue;
            SEL *sels = (SEL *)p;
            for (unsigned long j = 0; j < size / sizeof(SEL); j++) if (sels[j]) [set addObject:@(sel_getName(sels[j]))];
        }
        break;
    }
    return set;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        const char *fw[][2] = {{"SkyLight", "/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight"},
                               {"QuartzCore", "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore"},
                               {"CoreDisplay", "/System/Library/Frameworks/CoreDisplay.framework/Versions/A/CoreDisplay"}};
        NSMutableDictionary<NSString *, NSMutableSet *> *refs = [NSMutableDictionary dictionary];
        for (int i = 0; i < 3; i++) { refs[@(fw[i][0])] = selrefs_of(fw[i][1]); printf("%s: %lu selector references\n", fw[i][0], (unsigned long)refs[@(fw[i][0])].count); }
        // protocols and the RedNavi class that implements each
        NSBundle *b = [NSBundle bundleWithPath:@"/Library/GPUBundles/RedNaviMTLDriver.bundle"];
        [b load];
        const struct { const char *proto; const char *cls; } map[] = {
            {"MTLDevice", "RedNaviMTLDevice"}, {"MTLCommandQueue", "RedNaviMTLCommandQueue"}, {"MTLCommandBuffer", "RedNaviMTLCommandBuffer"},
            {"MTLRenderCommandEncoder", "RedNaviMTLRenderCommandEncoder"}, {"MTLBlitCommandEncoder", "RedNaviMTLBlitCommandEncoder"}, {"MTLComputeCommandEncoder", "RedNaviMTLComputeCommandEncoder"},
            {"MTLTexture", "RedNaviMTLTexture"}, {"MTLBuffer", "RedNaviMTLBuffer"}, {"MTLRenderPipelineState", "RedNaviMTLRenderPipelineState"}, {"MTLSamplerState", "RedNaviMTLSamplerState"},
            {"MTLLibrary", "RedNaviMTLLibrary"}, {"MTLFunction", "RedNaviMTLFunction"}, {"MTLResource", "RedNaviMTLTexture"}, {"MTLCommandEncoder", "RedNaviMTLRenderCommandEncoder"}};
        NSMutableSet *all = [NSMutableSet set];
        for (NSString *k in refs) [all unionSet:refs[k]];
        for (unsigned m = 0; m < sizeof(map) / sizeof(map[0]); m++) {
            Protocol *pr = objc_getProtocol(map[m].proto);
            Class cls = objc_getClass(map[m].cls);
            if (!pr) { printf("protocol %s missing\n", map[m].proto); continue; }
            printf("\n== %s (class %s%s)\n", map[m].proto, map[m].cls, cls ? "" : " NOT FOUND");
            for (int req = 1; req >= 0; req--) for (int inst = 1; inst >= 0; inst--) {
                unsigned n = 0;
                struct objc_method_description *d = protocol_copyMethodDescriptionList(pr, req, inst, &n);
                for (unsigned i = 0; i < n; i++) {
                    NSString *name = @(sel_getName(d[i].name));
                    NSMutableArray *who = [NSMutableArray array];
                    for (NSString *k in @[@"SkyLight", @"QuartzCore", @"CoreDisplay"]) if ([refs[k] containsObject:name]) [who addObject:k];
                    if (!who.count) continue;
                    const BOOL ok = cls && (inst ? class_respondsToSelector(cls, d[i].name) : class_getClassMethod(cls, d[i].name) != NULL);
                    printf("  %s %-4s %-90s %s  [%s]\n", ok ? "ok     " : "MISSING", req ? "req" : "opt", name.UTF8String, inst ? "" : "(class)", [who componentsJoinedByString:@","].UTF8String);
                }
                free(d);
            }
        }
    }
    return 0;
}
