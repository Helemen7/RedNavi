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
#include <dlfcn.h>
#include "../mesa-bridge/test/vk_dispatch_spv.h"
#include "../mesa-bridge/test/vk_copy2_spv.h"
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
