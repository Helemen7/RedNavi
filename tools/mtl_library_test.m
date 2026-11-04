// Builds an MTLLibrary of the plug-in (RedNaviMTL/compute.m) from a .metallib file, without a device, and lists its functions: tools/mtl_library_test BUNDLE METALLIB
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 3) { puts("usage: mtl_library_test BUNDLE METALLIB"); return 2; }
        NSBundle *b = [NSBundle bundleWithPath:@(argv[1])];
        NSError *e = nil;
        if (![b loadAndReturnError:&e]) { printf("bundle load failed: %s\n", e.localizedDescription.UTF8String); return 1; }
        void *h = dlopen([b executablePath].UTF8String, RTLD_NOLOAD | RTLD_NOW);
        id (*mk)(NSData *, NSError **) = h ? dlsym(h, "rn_test_library") : NULL;
        if (!mk) { puts("rn_test_library missing"); return 1; }
        NSData *d = [NSData dataWithContentsOfFile:@(argv[2])];
        id<MTLLibrary> lib = mk(d, &e);
        if (!lib) { printf("library failed: %s\n", e.localizedDescription.UTF8String); return 1; }
        printf("library type %ld, %lu functions\n", (long)lib.type, (unsigned long)lib.functionNames.count);
        int bad = 0;
        for (NSString *n in lib.functionNames) {
            id<MTLFunction> f = [lib newFunctionWithName:n];
            if (!f || ![f.name isEqualToString:n]) { printf("  %s: newFunctionWithName failed\n", n.UTF8String); bad++; continue; }
            printf("  %-40s type %ld\n", n.UTF8String, (long)f.functionType);
        }
        id<MTLFunction> none = [lib newFunctionWithName:@"no_such_function"];
        printf("unknown name -> %s\n", none ? "object (wrong)" : "nil");
        return bad || none ? 1 : 0;
    }
}
