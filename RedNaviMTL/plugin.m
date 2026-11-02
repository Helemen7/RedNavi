// RedNaviMTLDriver.bundle: a stub Metal driver plug-in (docs/M6_ACCELERATOR_STUDY.md). The accelerator kext names it in MetalPluginName; Metal.framework loads the bundle in
// every process that enumerates devices and asks it for the class named by MetalPluginClassName. This stub does nothing but write down what happens:
//  - when it is loaded, in which process, and the whole method list of the superclass the plug-in class has to derive from (MTLIOAccelDevice, found through the Objective-C
//    runtime: names only);
//  - every selector that is sent to RedNaviMTLDevice and that nobody implements (class or instance side): it is logged and answered with nil/0, so that the log
//    is the list of what the Metal framework expects from a driver beyond what the superclass already does.
// Everything inherited from the superclass runs as it is; its calls into the kernel reach the accelerator kext, which logs them as well.
// Log: /tmp/rednavi-metal-<uid>.log (append, one line per event).
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static void rn_log(const char *fmt, ...) {
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
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd >= 0) { write(fd, line, n); close(fd); }
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
    objc_registerClassPair(k);
    rn_log("class RedNaviMTLDevice registered, super %s", class_getName(sup));
}
