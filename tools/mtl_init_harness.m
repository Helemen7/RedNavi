// Runs what Metal does for one accelerator in a single process, without registering anything: loads the plug-in bundle, finds the accelerator in the registry (also an
// unregistered one), and sends the plug-in class -initWithAcceleratorPort:. A SIGSEGV handler prints the fault address, the registers and the regions around it, so a
// crash of the framework's user-space code shows where it reads (docs/M6_ACCELERATOR_STUDY.md, section 9).
// usage: mtl_init_harness [bundle]   (default /Library/GPUBundles/RedNaviMTLDriver.bundle)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <signal.h>
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
