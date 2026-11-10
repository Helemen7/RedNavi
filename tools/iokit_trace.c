// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Interposing library: writes every IOKit connection call of the process to stderr (IOServiceOpen with the class of the service, IOConnectCall* with selector, sizes, first words
// and result, IOConnectMapMemory64, IOConnectSetNotificationPort). Used with DYLD_INSERT_LIBRARIES (the test desktop has SIP off) to see what Metal's user-space asks of the
// accelerator kext: DYLD_INSERT_LIBRARIES=/tmp/iokit_trace.dylib /tmp/mtl_init_harness
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_replacement, _replacee) \
    __attribute__((used)) static struct { const void *replacement; const void *replacee; } _interpose_##_replacee \
    __attribute__((section("__DATA,__interpose"))) = { (const void *)(unsigned long)&_replacement, (const void *)(unsigned long)&_replacee };

static void words(char *out, size_t cap, const void *p, size_t bytes) {
    const uint32_t *w = p; size_t len = 0; out[0] = 0;
    for (size_t i = 0; p && i < 8 && i * 4 < bytes && len + 10 < cap; i++) len += snprintf(out + len, cap - len, "%s%08x", i ? " " : "", w[i]);
}
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char b[700]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof(b) - 1, fmt, ap); va_end(ap);
    b[n++] = '\n'; write(2, b, n);
}

static kern_return_t t_open(io_service_t s, task_port_t t, uint32_t type, io_connect_t *c) {
    kern_return_t r = IOServiceOpen(s, t, type, c);
    io_name_t cls = "?"; IOObjectGetClass(s, cls);
    say("  [io] IOServiceOpen(class %s, type %u) -> 0x%x conn %u", cls, type, r, c ? *c : 0);
    return r;
}
DYLD_INTERPOSE(t_open, IOServiceOpen)
static kern_return_t t_close(io_connect_t c) { kern_return_t r = IOServiceClose(c); say("  [io] IOServiceClose(%u) -> 0x%x", c, r); return r; }
DYLD_INTERPOSE(t_close, IOServiceClose)

static kern_return_t t_call(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt, const void *inS, size_t inSCnt, uint64_t *out, uint32_t *outCnt, void *outS, size_t *outSCnt) {
    size_t cap = outSCnt ? *outSCnt : 0;
    kern_return_t r = IOConnectCallMethod(c, sel, in, inCnt, inS, inSCnt, out, outCnt, outS, outSCnt);
    char wi[96], wo[96]; words(wi, sizeof(wi), inS, inSCnt); words(wo, sizeof(wo), outS, outSCnt ? *outSCnt : 0);
    say("  [io] CallMethod(conn %u sel %u) scalar in %u [0x%llx] struct in %zu [%s] out cap %zu -> 0x%x scalar out %u [0x%llx] struct out %zu [%s]", c, sel, inCnt, inCnt && in ? in[0] : 0ull, inSCnt, wi, cap, r,
        outCnt ? *outCnt : 0, outCnt && *outCnt && out ? out[0] : 0ull, outSCnt ? *outSCnt : 0, wo);
    return r;
}
DYLD_INTERPOSE(t_call, IOConnectCallMethod)
static kern_return_t t_scalar(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt, uint64_t *out, uint32_t *outCnt) {
    kern_return_t r = IOConnectCallScalarMethod(c, sel, in, inCnt, out, outCnt);
    say("  [io] CallScalar(conn %u sel %u) in %u [0x%llx] -> 0x%x out %u [0x%llx]", c, sel, inCnt, inCnt && in ? in[0] : 0ull, r, outCnt ? *outCnt : 0, outCnt && *outCnt && out ? out[0] : 0ull);
    return r;
}
DYLD_INTERPOSE(t_scalar, IOConnectCallScalarMethod)
static kern_return_t t_struct(mach_port_t c, uint32_t sel, const void *inS, size_t inSCnt, void *outS, size_t *outSCnt) {
    size_t cap = outSCnt ? *outSCnt : 0;
    kern_return_t r = IOConnectCallStructMethod(c, sel, inS, inSCnt, outS, outSCnt);
    char wi[96], wo[96]; words(wi, sizeof(wi), inS, inSCnt); words(wo, sizeof(wo), outS, outSCnt ? *outSCnt : 0);
    say("  [io] CallStruct(conn %u sel %u) in %zu [%s] out cap %zu -> 0x%x out %zu [%s]", c, sel, inSCnt, wi, cap, r, outSCnt ? *outSCnt : 0, wo);
    return r;
}
DYLD_INTERPOSE(t_struct, IOConnectCallStructMethod)
static kern_return_t t_async(mach_port_t c, uint32_t sel, mach_port_t wake, uint64_t *ref, uint32_t refCnt, const uint64_t *in, uint32_t inCnt, const void *inS, size_t inSCnt,
                             uint64_t *out, uint32_t *outCnt, void *outS, size_t *outSCnt) {
    kern_return_t r = IOConnectCallAsyncMethod(c, sel, wake, ref, refCnt, in, inCnt, inS, inSCnt, out, outCnt, outS, outSCnt);
    say("  [io] CallAsync(conn %u sel %u) scalar in %u struct in %zu -> 0x%x", c, sel, inCnt, inSCnt, r);
    return r;
}
DYLD_INTERPOSE(t_async, IOConnectCallAsyncMethod)
static kern_return_t t_map(io_connect_t c, uint32_t type, task_port_t task, mach_vm_address_t *addr, mach_vm_size_t *size, IOOptionBits opt) {
    kern_return_t r = IOConnectMapMemory64(c, type, task, addr, size, opt);
    say("  [io] MapMemory64(conn %u type %u opt 0x%x) -> 0x%x addr 0x%llx size 0x%llx", c, type, opt, r, addr ? *addr : 0ull, size ? *size : 0ull);
    return r;
}
DYLD_INTERPOSE(t_map, IOConnectMapMemory64)
static kern_return_t t_notify(io_connect_t c, uint32_t type, mach_port_t port, uintptr_t ref) {
    kern_return_t r = IOConnectSetNotificationPort(c, type, port, ref);
    say("  [io] SetNotificationPort(conn %u type %u) -> 0x%x", c, type, r);
    return r;
}
DYLD_INTERPOSE(t_notify, IOConnectSetNotificationPort)
