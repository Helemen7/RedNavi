// RedNaviMTLDriver.bundle: a stub Metal driver plug-in (docs/M6_ACCELERATOR_STUDY.md). The accelerator kext names it in MetalPluginName; Metal.framework loads the bundle in
// every process that enumerates devices and asks it for the class named by MetalPluginClassName. This stub does nothing but write down what happens:
//  - when it is loaded, in which process, and the whole method list of the superclass the plug-in class has to derive from (MTLIOAccelDevice, found through the Objective-C
//    runtime: names only);
//  - every selector that is sent to RedNaviMTLDevice and that nobody implements (class or instance side): it is logged and answered with nil/0, so that the log
//    is the list of what the Metal framework expects from a driver beyond what the superclass already does.
// Everything inherited from the superclass runs as it is; its calls into the kernel reach the accelerator kext (the family's own code answers them).
// Second version (2026-11-04): the device that MTLAddDevice got back was not an MTLDevice (assertion [newDevice conformsToProtocol:@protocol(MTLDevice)]), so
//  - every init* method the plug-in class inherits is wrapped: arguments, result, class of the result, conformance to MTLDevice;
//  - while one of those runs, the IOKit calls of the thread are written down too (IOServiceOpen with the class of the service and the connection type, IOConnectCall*
//    with selector, sizes, first words of the structure out and the IOReturn, IOConnectMapMemory64, IOConnectSetNotificationPort): they are interposed with dyld's
//    __interpose section. Whether dyld applies the section of a bundle loaded with dlopen is not certain; if no IOKit line shows up, it does not.
// Log: /tmp/rednavi-metal-<uid>.log (append, one line per event, mode 0666 so that the files of other processes of the same user can be appended to).
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <IOKit/IOKitLib.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

void rn_log(const char *fmt, ...) {
    static int enabled = -1;
    if (enabled < 0) enabled = (getenv("RN_METAL_LOG") != NULL) ? 1 : 0;
    if (!enabled) return;
    static char name[64];
    if (!name[0]) { extern const char *getprogname(void); strlcpy(name, getprogname(), sizeof(name)); }
    char line[1024];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int n = snprintf(line, sizeof(line), "%ld.%03d pid %d %s: ", (long)tv.tv_sec, (int)(tv.tv_usec / 1000), getpid(), name);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    line[n++] = '\n';
    char path[64];
    snprintf(path, sizeof(path), "/tmp/rednavi-metal-%d.log", (int)getuid());   // one file per user: a file made by a root process cannot be appended to by the others
    mode_t old = umask(0);
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0666);
    umask(old);
    if (fd >= 0) { write(fd, line, n); close(fd); }
}


// ----------------------------------------------------------------------------------------------------------------------------------------------- IOKit interposing
static __thread int rn_depth;     // > 0 while one of the wrapped init methods runs on this thread

#define DYLD_INTERPOSE(_replacement, _replacee) \
    __attribute__((used)) static struct { const void *replacement; const void *replacee; } _interpose_##_replacee \
    __attribute__((section("__DATA,__interpose"))) = { (const void *)(unsigned long)&_replacement, (const void *)(unsigned long)&_replacee };

static void rn_words(char *out, size_t cap, const void *p, size_t bytes) {
    const uint32_t *w = p;
    size_t len = 0;
    out[0] = 0;
    for (size_t i = 0; p && i < 8 && i * 4 < bytes && len + 10 < cap; i++) len += snprintf(out + len, cap - len, "%s%08x", i ? " " : "", w[i]);
}

static kern_return_t rn_IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect) {
    kern_return_t r = IOServiceOpen(service, task, type, connect);
    if (rn_depth) {
        io_name_t cls = "?";
        IOObjectGetClass(service, cls);
        rn_log("  IOServiceOpen(service %u class %s, type %u) -> 0x%x connect %u", service, cls, type, r, connect ? *connect : 0);
    }
    return r;
}
DYLD_INTERPOSE(rn_IOServiceOpen, IOServiceOpen)

static kern_return_t rn_IOServiceClose(io_connect_t c) {
    kern_return_t r = IOServiceClose(c);
    if (rn_depth) rn_log("  IOServiceClose(%u) -> 0x%x", c, r);
    return r;
}
DYLD_INTERPOSE(rn_IOServiceClose, IOServiceClose)

static kern_return_t rn_IOConnectCallMethod(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt, const void *inS, size_t inSCnt,
                                            uint64_t *out, uint32_t *outCnt, void *outS, size_t *outSCnt) {
    size_t outSize = outSCnt ? *outSCnt : 0;
    kern_return_t r = IOConnectCallMethod(c, sel, in, inCnt, inS, inSCnt, out, outCnt, outS, outSCnt);
    if (rn_depth) {
        char wi[96], wo[96];
        rn_words(wi, sizeof(wi), inS, inSCnt);
        rn_words(wo, sizeof(wo), outS, outSCnt ? *outSCnt : 0);
        rn_log("  IOConnectCallMethod(conn %u, sel %u, scalar in %u [0x%llx], struct in %zu [%s], scalar out %u, struct out cap %zu) -> 0x%x; scalar out %u [0x%llx], struct out %zu [%s]",
               c, sel, inCnt, inCnt && in ? in[0] : 0ull, inSCnt, wi, outCnt ? *outCnt : 0, outSize, r, outCnt ? *outCnt : 0, outCnt && *outCnt && out ? out[0] : 0ull, outSCnt ? *outSCnt : 0, wo);
    }
    return r;
}
DYLD_INTERPOSE(rn_IOConnectCallMethod, IOConnectCallMethod)

static kern_return_t rn_IOConnectCallScalarMethod(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt, uint64_t *out, uint32_t *outCnt) {
    kern_return_t r = IOConnectCallScalarMethod(c, sel, in, inCnt, out, outCnt);
    if (rn_depth) rn_log("  IOConnectCallScalarMethod(conn %u, sel %u, in %u [0x%llx]) -> 0x%x; out %u [0x%llx]", c, sel, inCnt, inCnt && in ? in[0] : 0ull, r, outCnt ? *outCnt : 0, outCnt && *outCnt && out ? out[0] : 0ull);
    return r;
}
DYLD_INTERPOSE(rn_IOConnectCallScalarMethod, IOConnectCallScalarMethod)

static kern_return_t rn_IOConnectCallStructMethod(mach_port_t c, uint32_t sel, const void *inS, size_t inSCnt, void *outS, size_t *outSCnt) {
    size_t cap = outSCnt ? *outSCnt : 0;
    kern_return_t r = IOConnectCallStructMethod(c, sel, inS, inSCnt, outS, outSCnt);
    if (rn_depth) {
        char wi[96], wo[96];
        rn_words(wi, sizeof(wi), inS, inSCnt);
        rn_words(wo, sizeof(wo), outS, outSCnt ? *outSCnt : 0);
        rn_log("  IOConnectCallStructMethod(conn %u, sel %u, struct in %zu [%s], struct out cap %zu) -> 0x%x; struct out %zu [%s]", c, sel, inSCnt, wi, cap, r, outSCnt ? *outSCnt : 0, wo);
    }
    return r;
}
DYLD_INTERPOSE(rn_IOConnectCallStructMethod, IOConnectCallStructMethod)

static kern_return_t rn_IOConnectCallAsyncMethod(mach_port_t c, uint32_t sel, mach_port_t wake, uint64_t *ref, uint32_t refCnt, const uint64_t *in, uint32_t inCnt,
                                                 const void *inS, size_t inSCnt, uint64_t *out, uint32_t *outCnt, void *outS, size_t *outSCnt) {
    kern_return_t r = IOConnectCallAsyncMethod(c, sel, wake, ref, refCnt, in, inCnt, inS, inSCnt, out, outCnt, outS, outSCnt);
    if (rn_depth) rn_log("  IOConnectCallAsyncMethod(conn %u, sel %u, scalar in %u, struct in %zu) -> 0x%x", c, sel, inCnt, inSCnt, r);
    return r;
}
DYLD_INTERPOSE(rn_IOConnectCallAsyncMethod, IOConnectCallAsyncMethod)

static kern_return_t rn_IOConnectMapMemory64(io_connect_t c, uint32_t type, task_port_t task, mach_vm_address_t *addr, mach_vm_size_t *size, IOOptionBits options) {
    kern_return_t r = IOConnectMapMemory64(c, type, task, addr, size, options);
    if (rn_depth) rn_log("  IOConnectMapMemory64(conn %u, type %u, options 0x%x) -> 0x%x addr 0x%llx size 0x%llx", c, type, options, r, addr ? *addr : 0ull, size ? *size : 0ull);
    return r;
}
DYLD_INTERPOSE(rn_IOConnectMapMemory64, IOConnectMapMemory64)

static kern_return_t rn_IOConnectSetNotificationPort(io_connect_t c, uint32_t type, mach_port_t port, uintptr_t ref) {
    kern_return_t r = IOConnectSetNotificationPort(c, type, port, ref);
    if (rn_depth) rn_log("  IOConnectSetNotificationPort(conn %u, type %u) -> 0x%x", c, type, r);
    return r;
}
DYLD_INTERPOSE(rn_IOConnectSetNotificationPort, IOConnectSetNotificationPort)

// Every init* method the plug-in class inherits, wrapped. The wrapper passes three more integer-class arguments through (enough for the init methods of a device).
// Object pointers travel as integers: an init method consumes self and returns a +1 object, which ARC would get wrong through a plain function pointer.
typedef uintptr_t (*rn_init3)(uintptr_t, SEL, uintptr_t, uintptr_t, uintptr_t);
static void rn_wrap_init(Class k, Class from) {
    unsigned n = 0;
    Method *list = class_copyMethodList(from, &n);
    for (unsigned i = 0; i < n; i++) {
        SEL sel = method_getName(list[i]);
        if (strncmp(sel_getName(sel), "init", 4) != 0) continue;
        const char *enc0 = method_getTypeEncoding(list[i]);
        if (!enc0 || enc0[0] != '@') continue;                 // only the ones that return an object
        rn_init3 orig = (rn_init3)method_getImplementation(list[i]);
        const char *enc = method_getTypeEncoding(list[i]);
        rn_log("wrapping -%s (%s) of %s", sel_getName(sel), enc ? enc : "?", class_getName(from));
        class_addMethod(k, sel, imp_implementationWithBlock(^uintptr_t(uintptr_t self, uintptr_t a, uintptr_t b, uintptr_t c) {
            io_name_t cls = "?";
            IOObjectGetClass((io_object_t)a, cls);
            rn_log("-[RedNaviMTLDevice %s] begin: args 0x%lx 0x%lx 0x%lx (arg0 as an IOKit object: class %s)", sel_getName(sel), (unsigned long)a, (unsigned long)b, (unsigned long)c, cls);
            rn_depth++;
            uintptr_t r = orig(self, sel, a, b, c);
            rn_depth--;
            Protocol *p = objc_getProtocol("MTLDevice");
            id ro = (__bridge id)(void *)r;
            rn_log("-[RedNaviMTLDevice %s] end: result 0x%lx class %s, conforms to MTLDevice: %s", sel_getName(sel), (unsigned long)r,
                   r ? class_getName(object_getClass(ro)) : "-", r && p && [ro conformsToProtocol:p] ? "yes" : "no");
            return r;
        }), enc);
    }
    free(list);
}

static id rn_generic(id self, SEL _cmd, ...) {
    Class k = object_getClass(self);
    rn_log("unimplemented %c[%s %s]", class_isMetaClass(k) ? '+' : '-', class_isMetaClass(k) ? class_getName(self) : class_getName(k), sel_getName(_cmd));
    return nil;
}

static BOOL rn_resolve_instance(id self, SEL _cmd, SEL sel) {
    class_addMethod((Class)self, sel, (IMP)rn_generic, "@@:");
    rn_log("resolveInstanceMethod: -%s", sel_getName(sel));
    return YES;
}

static BOOL rn_resolve_class(id self, SEL _cmd, SEL sel) {
    class_addMethod(object_getClass(self), sel, (IMP)rn_generic, "@@:");
    rn_log("resolveClassMethod: +%s", sel_getName(sel));
    return YES;
}

static void rn_dump_methods(Class k, const char *label) {
    unsigned n = 0;
    Method *m = class_copyMethodList(k, &n);
    char buf[900];
    int len = 0;
    for (unsigned i = 0; i < n; i++) {
        const char *s = sel_getName(method_getName(m[i]));
        if (len + (int)strlen(s) + 2 > (int)sizeof(buf)) { rn_log("%s:%s", label, buf); len = 0; }
        len += snprintf(buf + len, sizeof(buf) - len, " %s", s);
        buf[len] = 0;
    }
    if (len) rn_log("%s:%s", label, buf);
    rn_log("%s: %u methods", label, n);
    free(m);
}

extern void rn_compute_install(Class k, Class sup);

__attribute__((constructor)) static void rn_load(void) {
    rn_log("bundle loaded");
    Class sup = objc_getClass("MTLIOAccelDevice");
    if (!sup) { rn_log("MTLIOAccelDevice not found, falling back to NSObject"); sup = [NSObject class]; }
    for (Class c = sup; c; c = class_getSuperclass(c)) {
        char label[160];
        rn_log("superclass chain: %s (instance size %zu)", class_getName(c), class_getInstanceSize(c));
        if (c == [NSObject class]) break;
        snprintf(label, sizeof(label), "instance methods of %s", class_getName(c)); rn_dump_methods(c, label);
        snprintf(label, sizeof(label), "class methods of %s", class_getName(c)); rn_dump_methods(object_getClass(c), label);
    }
    Class k = objc_allocateClassPair(sup, "RedNaviMTLDevice", 0);
    if (!k) { rn_log("objc_allocateClassPair failed (class exists?)"); return; }
    class_addMethod(object_getClass(k), @selector(resolveInstanceMethod:), (IMP)rn_resolve_instance, "c@::");
    class_addMethod(object_getClass(k), @selector(resolveClassMethod:), (IMP)rn_resolve_class, "c@::");
    // The first message Metal sends to the plug-in class, seen in the log of the first stub: +bundleForClass (a class method with no argument); the answer is the bundle.
    // It must not be [NSBundle bundleForClass:self]: that asks this very method again and recurses until the stack is gone (every Metal process crashed with it, 2026-11-02).
    // The bundle is found from the address of this code instead.
    class_addMethod(object_getClass(k), sel_registerName("bundleForClass"), imp_implementationWithBlock(^id(id self) {
        Dl_info info;
        NSBundle *b = nil;
        if (dladdr((const void *)rn_load, &info) && info.dli_fname) {
            NSString *exe = [NSString stringWithUTF8String:info.dli_fname];                 // .../RedNaviMTLDriver.bundle/Contents/MacOS/RedNaviMTLDriver
            b = [NSBundle bundleWithPath:[[[exe stringByDeletingLastPathComponent] stringByDeletingLastPathComponent] stringByDeletingLastPathComponent]];
        }
        rn_log("+bundleForClass answered: %s", b ? b.bundlePath.UTF8String : "nil");
        return b;
    }), "@@:");
    for (Class c = sup; c && c != [NSObject class]; c = class_getSuperclass(c)) rn_wrap_init(k, c);
    // Command queues, buffers and the compute path: compute.m (nil unless the process has RN_METAL_COMPUTE set).
    rn_compute_install(k, sup);
    // -featureProfile has type Q (a 64-bit integer, read from the Intel driver's metadata on a Mac that has it: MTLIGAccelDevice, "Q16@0:8"). Unimplemented it answered 0 and
    // -[_MTLDevice initLimits] asserted _limits.maxColorAttachments > 0. The value is a version threshold: +[_MTLDevice featureProfile:supportsFeatureSet:] says profile 10000 supports
    // the feature sets 10000, 10001, 10003 and 10004, any profile from 10001 up also 10005; the Intel UHD 617 reports 10002, which is used here until a reason to change it shows up.
    class_addMethod(k, sel_registerName("featureProfile"), imp_implementationWithBlock(^unsigned long long(id self) {
        rn_log("featureProfile asked, answering 10002");
        return 10002ull;
    }), "Q16@0:8");
    class_addMethod(k, sel_registerName("name"), imp_implementationWithBlock(^NSString *(id self) {
        return @"AMD Radeon 8060S";
    }), "@@:");
    class_addMethod(k, sel_registerName("hasUnifiedMemory"), imp_implementationWithBlock(^BOOL(id self) {
        return YES;
    }), "B16@0:8");
    class_addMethod(k, sel_registerName("dedicatedMemorySize"), imp_implementationWithBlock(^unsigned long long(id self) {
        return 96ULL * 1024 * 1024 * 1024;
    }), "Q16@0:8");
    class_addMethod(k, sel_registerName("sharedMemorySize"), imp_implementationWithBlock(^unsigned long long(id self) {
        return 32ULL * 1024 * 1024 * 1024;
    }), "Q16@0:8");
    class_addMethod(k, sel_registerName("recommendedMaxWorkingSetSize"), imp_implementationWithBlock(^unsigned long long(id self) {
        return 96ULL * 1024 * 1024 * 1024;
    }), "Q16@0:8");
    class_addMethod(k, sel_registerName("maxBufferLength"), imp_implementationWithBlock(^unsigned long long(id self) {
        return 64ULL * 1024 * 1024 * 1024;
    }), "Q16@0:8");
    // Metal asserts [newDevice conformsToProtocol:@protocol(MTLDevice)] in MTLAddDevice, and a class made at run time declares no protocol (the log of 2026-11-04 showed
    // -initWithAcceleratorPort: succeeding and "conforms to MTLDevice: no"). The vendor classes declare it in their class metadata; here it is added by hand.
    // The superclasses conform in fact (they implement the methods), only the declaration is missing.
    const char *protoNames[] = {"MTLDevice", "MTLDeviceSPI"};
    for (size_t i = 0; i < sizeof(protoNames) / sizeof(protoNames[0]); i++) {
        Protocol *p = objc_getProtocol(protoNames[i]);
        rn_log("protocol %s: %s%s", protoNames[i], p ? "found" : "not found", p && class_addProtocol(k, p) ? ", added" : "");
    }
    objc_registerClassPair(k);
    rn_log("class RedNaviMTLDevice registered, super %s", class_getName(sup));
}
