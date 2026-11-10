// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Test program for the RedNavi user client (docs/COMPAT.md, "The user client"): two buffers in host memory shared with the GPU, a
// shader and a command buffer written into the first, the result and the fence read back from the second. Everything between the two
// writes below is done by the GPU.
//
//     clang -O1 -mmacosx-version-min=12 -framework IOKit -framework CoreFoundation tools/rn_test.c -o rn_test
//     sudo ./rn_test            (or boot with rednavi_uc_anyone=1)
//
// The register offsets in the packets are those of the RX 7800 XT (GC segment 0 base 0x1260); another card needs its own base.
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../RedNavi/rn_uapi.h"

#define GC_BASE 0x1260u
#define SHREG(reg) (GC_BASE + (reg) - 0x2c00u)
#define PACKET3(op, n) ((3u << 30) | (((n) & 0x3FFFu) << 16) | (((op) & 0xFFu) << 8))


typedef struct { uint32_t handle; uint64_t va; uint64_t size; volatile uint32_t *cpu; } Bo;

static int createBo(io_connect_t c, uint64_t size, Bo *bo) {
    struct rn_uc_bo_create in = {size, 0, 0};
    struct rn_uc_bo_info out;
    size_t outSize = sizeof(out);
    kern_return_t kr = IOConnectCallStructMethod(c, RN_UC_BO_CREATE, &in, sizeof(in), &out, &outSize);
    if (kr != KERN_SUCCESS) { printf("create buffer: 0x%x\n", kr); return -1; }
    bo->handle = out.handle; bo->va = out.gpu_va; bo->size = out.size;
    mach_vm_address_t addr = 0;
    mach_vm_size_t len = 0;
    kr = IOConnectMapMemory64(c, bo->handle, mach_task_self(), &addr, &len, kIOMapAnywhere);
    if (kr != KERN_SUCCESS) { printf("map buffer %u: 0x%x\n", bo->handle, kr); return -1; }
    bo->cpu = (volatile uint32_t *)(uintptr_t)addr;
    return 0;
}

static uint32_t gStatus;                                   // the status of the last sequence wait: RN_UC_SEQ_DONE or RN_UC_SEQ_LOST
static uint32_t gSeq;                                      // the sequence number of the last submit that worked

static kern_return_t submitIb(io_connect_t c, uint64_t ibVa, uint32_t ibDwords) {
    struct rn_uc_submit in = {ibVa, ibDwords, 0};
    struct rn_uc_submit_result out = {0, 0};
    size_t outSize = sizeof(out);
    kern_return_t kr = IOConnectCallStructMethod(c, RN_UC_SUBMIT, &in, sizeof(in), &out, &outSize);
    if (kr == KERN_SUCCESS) gSeq = out.seq;
    return kr;
}

static kern_return_t submitSync(io_connect_t c, uint64_t ibVa, uint32_t ibDwords, const rn_uc_sync_ref *in, uint32_t nIn, const rn_uc_sync_ref *out, uint32_t nOut) {
    struct rn_uc_submit sub;
    memset(&sub, 0, sizeof(sub));
    sub.ib_va = ibVa; sub.ib_dwords = ibDwords; sub.in_count = nIn; sub.out_count = nOut;
    for (uint32_t i = 0; i < nIn; i++) sub.in[i] = in[i];
    for (uint32_t i = 0; i < nOut; i++) sub.out[i] = out[i];
    struct rn_uc_submit_result res = {0, 0};
    size_t resSize = sizeof(res);
    kern_return_t kr = IOConnectCallStructMethod(c, RN_UC_SUBMIT, &sub, sizeof(sub), &res, &resSize);
    if (kr == KERN_SUCCESS) gSeq = res.seq;
    return kr;
}

static kern_return_t soCreate(io_connect_t c, uint32_t flags, uint32_t *handle) {
    struct rn_uc_syncobj_create in = {flags, 0};
    struct rn_uc_syncobj_create_result out = {0, 0};
    size_t outSize = sizeof(out);
    kern_return_t kr = IOConnectCallStructMethod(c, RN_UC_SYNCOBJ_CREATE, &in, sizeof(in), &out, &outSize);
    *handle = out.handle;
    return kr;
}

static kern_return_t soDestroy(io_connect_t c, uint32_t handle) {
    struct rn_uc_syncobj_destroy in = {handle, 0};
    return IOConnectCallStructMethod(c, RN_UC_SYNCOBJ_DESTROY, &in, sizeof(in), NULL, NULL);
}

static kern_return_t soWait(io_connect_t c, const uint32_t *handles, const uint64_t *points, uint32_t n, uint32_t flags, int64_t timeoutMs) {
    struct rn_uc_syncobj_wait in;
    struct rn_uc_syncobj_wait_result out;
    size_t outSize = sizeof(out);
    memset(&in, 0, sizeof(in));
    in.timeout_nsec = timeoutMs * 1000000; in.count = n; in.flags = flags;
    for (uint32_t i = 0; i < n; i++) { in.handles[i] = handles[i]; in.points[i] = points ? points[i] : 0; }
    return IOConnectCallStructMethod(c, RN_UC_SYNCOBJ_WAIT, &in, sizeof(in), &out, &outSize);
}

static kern_return_t soArray(io_connect_t c, uint32_t selector, uint32_t handle, uint64_t point, uint64_t *pointOut) {
    struct rn_uc_syncobj_array in, out;
    size_t outSize = sizeof(out);
    memset(&in, 0, sizeof(in));
    in.count = 1; in.handles[0] = handle; in.points[0] = point;
    kern_return_t kr = selector == RN_UC_SYNCOBJ_QUERY ? IOConnectCallStructMethod(c, selector, &in, sizeof(in), &out, &outSize)
                                                      : IOConnectCallStructMethod(c, selector, &in, sizeof(in), NULL, NULL);
    if (kr == KERN_SUCCESS && pointOut) *pointOut = out.points[0];
    return kr;
}

static kern_return_t waitSeq(io_connect_t c, uint32_t seq, uint32_t ms, uint32_t *reached, uint32_t *signaled) {
    struct rn_uc_wait_seq in = {seq, ms};
    struct rn_uc_wait_seq_result out = {0, 0};
    size_t outSize = sizeof(out);
    kern_return_t kr = IOConnectCallStructMethod(c, RN_UC_WAIT_SEQ, &in, sizeof(in), &out, &outSize);
    *reached = out.reached;
    gStatus = out.status;
    if (signaled) *signaled = out.signaled;
    return kr;
}

static kern_return_t vaMap(io_connect_t c, uint32_t handle, uint64_t va, uint64_t offset, uint64_t size) {
    struct rn_uc_va_map in = {handle, 0, va, offset, size};
    return IOConnectCallStructMethod(c, RN_UC_VA_MAP, &in, sizeof(in), NULL, NULL);
}

static kern_return_t vaUnmap(io_connect_t c, uint64_t va, uint64_t size) {
    struct rn_uc_va_unmap in = {va, size};
    return IOConnectCallStructMethod(c, RN_UC_VA_UNMAP, &in, sizeof(in), NULL, NULL);
}

static kern_return_t diag(io_connect_t c, uint32_t flags, struct rn_uc_diag *d) {
    struct rn_uc_diag_req in = {flags, 0};
    size_t dSize = sizeof(*d);
    return IOConnectCallStructMethod(c, RN_UC_DIAG, &in, sizeof(in), d, &dSize);
}

// The shader (GFX11 machine code, LLVM MC test encodings): store 0x600DC0DE to the dword at storeVa. Fills code at +0x100 and returns
// the dwords of a command buffer that dispatches it, written at the start of the buffer.
static uint32_t buildDispatch(volatile uint32_t *cpu, uint64_t codeVa, uint64_t storeVa) {
    const uint32_t shader[11] = {0xBE8000FFu, (uint32_t)storeVa, 0xBE8100FFu, (uint32_t)(storeVa >> 32), 0x7E020280u, 0x7E0402FFu, 0x600DC0DEu,
                                 0xDC6A0000u, 0x00000201u, 0xBC7C0000u, 0xBFB00000u};
    for (int i = 0; i < 64; i++) cpu[0x100 / 4 + i] = i < 11 ? shader[i] : 0xBF9F0000u;   // s_code_end padding
    uint32_t ib[48], n = 0;
    const uint64_t sva = codeVa + 0x100;
    ib[n++] = PACKET3(0x76, 2); ib[n++] = SHREG(0x1bac); ib[n++] = (uint32_t)(sva >> 8); ib[n++] = (uint32_t)((sva >> 40) & 0xff);      // COMPUTE_PGM_LO/HI: the high register has 8 bits (COMPUTE_PGM_HI__DATA_MASK 0xff), bits above must stay 0
    ib[n++] = PACKET3(0x76, 2); ib[n++] = SHREG(0x1bb2); ib[n++] = 0x002C0000u; ib[n++] = 0x80u;                              // PGM_RSRC1/2
    ib[n++] = PACKET3(0x76, 1); ib[n++] = SHREG(0x1bc8); ib[n++] = 0;                                                         // PGM_RSRC3
    ib[n++] = PACKET3(0x76, 3); ib[n++] = SHREG(0x1ba4); ib[n++] = 0; ib[n++] = 0; ib[n++] = 0;                               // START_X/Y/Z
    ib[n++] = PACKET3(0x76, 3); ib[n++] = SHREG(0x1ba7); ib[n++] = 1; ib[n++] = 1; ib[n++] = 1;                               // NUM_THREAD_X/Y/Z
    ib[n++] = PACKET3(0x76, 6); ib[n++] = SHREG(0x1bb5);                                                                      // limits, SE masks
    ib[n++] = 0; ib[n++] = 0xFFFFFFFFu; ib[n++] = 0xFFFFFFFFu; ib[n++] = 0; ib[n++] = 0xFFFFFFFFu; ib[n++] = 0xFFFFFFFFu;
    ib[n++] = PACKET3(0x15, 3); ib[n++] = 1; ib[n++] = 1; ib[n++] = 1; ib[n++] = 0x00000001u | 0x00000004u | 0x00000040u | 0x00008000u;   // DISPATCH_DIRECT
    for (uint32_t i = 0; i < n; i++) cpu[i] = ib[i];
    return n;
}

static kern_return_t waitDword(io_connect_t c, uint32_t handle, uint32_t offset, uint32_t value, uint32_t ms, uint32_t *reached) {
    struct rn_uc_bo_wait in = {handle, offset, value, ms};
    struct rn_uc_bo_wait_result out = {0, 0};
    size_t outSize = sizeof(out);
    kern_return_t kr = IOConnectCallStructMethod(c, RN_UC_BO_WAIT, &in, sizeof(in), &out, &outSize);
    *reached = out.reached;
    return kr;
}

// A connection with a code buffer and a data buffer mapped at 0x10000 and 0x20000 and the dispatch built for them (one low dispatch, as the plain test).
typedef struct { io_connect_t c; Bo code, data; uint32_t n; } Conn;
static int connSetup(io_service_t svc, Conn *k) {
    if (IOServiceOpen(svc, mach_task_self(), 0, &k->c) != KERN_SUCCESS) return -1;
    if (createBo(k->c, 4096, &k->code) || createBo(k->c, 4096, &k->data)) return -1;
    if (vaMap(k->c, k->code.handle, 0x10000, 0, 4096) || vaMap(k->c, k->data.handle, 0x20000, 0, 4096)) return -1;
    k->n = buildDispatch(k->code.cpu, 0x10000, 0x20000);
    return 0;
}
static int connRun(Conn *k, const char *what) {
    k->data.cpu[0] = 0xDEAD0000u;
    kern_return_t kr = submitIb(k->c, 0x10000, k->n);
    uint32_t r = 0;
    waitSeq(k->c, gSeq, 1500, &r, NULL);
    printf("%-44s: submit 0x%x, fence %s, result 0x%08x\n", what, kr, r ? "reached" : "NOT reached", k->data.cpu[0]);
    return r && k->data.cpu[0] == 0x600DC0DEu;
}

int main(int argc, char **argv) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedNaviProbe"));
    if (!svc) { printf("no RedNaviProbe service\n"); return 1; }
    io_connect_t c;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c);
    if (kr != KERN_SUCCESS) { printf("IOServiceOpen: 0x%x (administrator or boot-arg rednavi_uc_anyone=1)\n", kr); return 1; }

    if (argc > 1 && !strcmp(argv[1], "reset")) {               // reset the compute queue and nothing else (after a test left it stuck)
        struct rn_uc_reset_result rr; size_t rrSize = sizeof(rr);
        kr = IOConnectCallStructMethod(c, RN_UC_QUEUE_RESET, NULL, 0, &rr, &rrSize);
        printf("queue reset: 0x%x, status %u, resets so far %u\n", kr, rr.status, rr.resets);
        IOServiceClose(c);
        return kr || rr.status ? 2 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "vram")) {                 // vram [visible]: the plain dispatch, its result dword in a VRAM buffer (device-local, or CPU-visible); the result is read back by peek (and by the mapping)
        const int visible = argc > 2 && !strcmp(argv[2], "visible");
        Bo code;
        if (createBo(c, 4096, &code) || vaMap(c, code.handle, 0x10000, 0, 4096)) return 1;
        struct rn_uc_bo_create in = {1u << 20, visible ? RN_BO_VRAM_VISIBLE : RN_BO_VRAM, 0};
        struct rn_uc_bo_info out;
        size_t outSize = sizeof(out);
        kr = IOConnectCallStructMethod(c, RN_UC_BO_CREATE, &in, sizeof(in), &out, &outSize);
        printf("create %s VRAM buffer: 0x%x, handle %u, placement %u, VRAM offset 0x%llx\n", visible ? "visible" : "device-local", kr, out.handle, out.flags, out.gpu_va);
        if (kr) return 1;
        const uint64_t vramOff = out.gpu_va;
        if (vaMap(c, out.handle, 0x20000, 0, 1u << 20)) { printf("va map failed\n"); return 1; }
        const uint32_t n = buildDispatch(code.cpu, 0x10000, 0x20000);
        kr = submitIb(c, 0x10000, n);
        uint32_t r = 0;
        kern_return_t kw = waitSeq(c, gSeq, 3000, &r, NULL);
        printf("submit 0x%x, wait 0x%x, fence %s\n", kr, kw, r ? "reached" : "NOT reached");
        struct rn_uc_vram_peek pk = {vramOff, 4, 0};
        struct rn_uc_vram_peek_result pr;
        size_t prSize = sizeof(pr);
        kr = IOConnectCallStructMethod(c, RN_UC_VRAM_PEEK, &pk, sizeof(pk), &pr, &prSize);
        printf("VRAM at 0x%llx: %08x %08x %08x %08x (want 600dc0de first)\n", vramOff, pr.v[0], pr.v[1], pr.v[2], pr.v[3]);
        if (visible) {
            mach_vm_address_t addr = 0; mach_vm_size_t len = 0;
            kr = IOConnectMapMemory64(c, out.handle, mach_task_self(), &addr, &len, kIOMapAnywhere);
            printf("CPU mapping: 0x%x, first dword through it 0x%08x\n", kr, kr ? 0 : *(volatile uint32_t *)(uintptr_t)addr);
        }
        IOServiceClose(c);
        return pr.v[0] == 0x600DC0DEu ? 0 : 2;
    }
    if (argc > 1 && !strcmp(argv[1], "userptr")) {              // userptr: the plain dispatch writes its result dword into memory this program allocated itself (a buffer over the caller's pages, RN_UC_BO_USERPTR), nothing copied
        Bo code;
        if (createBo(c, 4096, &code) || vaMap(c, code.handle, 0x10000, 0, 4096)) return 1;
        const size_t bytes = 1u << 20;
        void *mine = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (mine == MAP_FAILED) { printf("mmap failed\n"); return 1; }
        memset(mine, 0xAB, bytes);
        struct rn_uc_bo_userptr in = {(uint64_t)(uintptr_t)mine, bytes, 0, 0};
        struct rn_uc_bo_info out;
        size_t outSize = sizeof(out);
        kr = IOConnectCallStructMethod(c, RN_UC_BO_USERPTR, &in, sizeof(in), &out, &outSize);
        printf("userptr buffer over %p: 0x%x, handle %u, size 0x%llx\n", mine, kr, out.handle, out.size);
        if (kr) return 1;
        if (vaMap(c, out.handle, 0x20000, 0, bytes)) { printf("va map failed\n"); return 1; }
        const uint32_t n = buildDispatch(code.cpu, 0x10000, 0x20000);
        kr = submitIb(c, 0x10000, n);
        uint32_t r = 0;
        kern_return_t kw = waitSeq(c, gSeq, 3000, &r, NULL);
        printf("submit 0x%x, wait 0x%x, fence %s\n", kr, kw, r ? "reached" : "NOT reached");
        const uint32_t *w = mine;
        printf("my memory: %08x %08x %08x %08x (want 600dc0de first, abababab after)\n", w[0], w[1], w[2], w[3]);
        struct rn_uc_bo_free f = {out.handle, 0};
        kr = IOConnectCallStructMethod(c, RN_UC_BO_FREE, &f, sizeof(f), NULL, NULL);
        printf("free: 0x%x; my memory still readable: %08x\n", kr, w[0]);
        IOServiceClose(c);
        return w[0] == 0x600DC0DEu ? 0 : 2;
    }
    if (argc > 1 && !strcmp(argv[1], "gfx")) {                  // gfx [N]: the plain dispatch on the graphics queue (a compute dispatch is valid on it), N times, interleaved with the compute queue
        const int rounds = argc > 2 ? atoi(argv[2]) : 1;
        Bo code, data;
        if (createBo(c, 4096, &code) || createBo(c, 4096, &data) || vaMap(c, code.handle, 0x10000, 0, 4096) || vaMap(c, data.handle, 0x20000, 0, 4096)) return 1;
        const uint32_t n = buildDispatch(code.cpu, 0x10000, 0x20000);
        for (uint32_t i = 0; i < n; i++)
            if ((code.cpu[i] & 0xFFFF00FFu) == (PACKET3(0x15, 3) & 0xFFFF00FFu) && ((code.cpu[i] >> 8) & 0xFF) == 0x15) code.cpu[i] |= 2u;   // DISPATCH_DIRECT: shader type compute (bit 1)
        int fails = 0;
        for (int r = 0; r < rounds; r++) {
            for (int q = 1; q >= 0; q--) {                                   // graphics first, then compute
                data.cpu[0] = 0xDEAD0000u;
                struct rn_uc_submit in; memset(&in, 0, sizeof(in));
                in.ib_va = 0x10000; in.ib_dwords = n; in.flags = q ? RN_SUBMIT_GFX : 0;
                struct rn_uc_submit_result out; size_t outSize = sizeof(out);
                kr = IOConnectCallStructMethod(c, RN_UC_SUBMIT, &in, sizeof(in), &out, &outSize);
                uint32_t reached = 0;
                kern_return_t kw = kr ? kr : waitSeq(c, out.seq, 3000, &reached, NULL);
                const int ok = !kr && !kw && reached && data.cpu[0] == 0x600DC0DEu;
                if (!ok || r == 0 || r == rounds - 1)
                    printf("round %d %s queue: submit 0x%x seq 0x%08x, wait 0x%x, fence %s, result 0x%08x%s\n", r, q ? "graphics" : "compute ", kr, out.seq, kw, reached ? "reached" : "NOT reached", data.cpu[0], ok ? "" : "  FAIL");
                if (!ok) { fails++; if (fails > 3) goto gfxdone; }
            }
        }
gfxdone:;
        struct rn_uc_diag d;
        if (diag(c, 0, &d) == KERN_SUCCESS) printf("fault status 0x%x, address 0x%x%08x\n", d.fault_status, d.fault_addr_hi, d.fault_addr_lo);
        printf("%d rounds, %d failures\n", rounds, fails);
        IOServiceClose(c);
        return fails ? 2 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "gfxreset")) {             // gfxreset: hang the graphics queue three ways (CP wait, shader loop, and left to the kernel watchdog), recover each time, check both queues still run
        Bo code, data;
        if (createBo(c, 4096, &code) || createBo(c, 4096, &data) || vaMap(c, code.handle, 0x10000, 0, 4096) || vaMap(c, data.handle, 0x20000, 0, 4096)) return 1;
        int ok = 1;
        for (int step = 0; step < 6 && ok; step++) {
            // 0: good dispatch on gfx; 1: CP hang, reset on request; 2: good; 3: shader loop, reset on request; 4: CP hang, the watchdog resets; 5: good on both queues
            const int hang = step == 1 || step == 3 || step == 4;
            uint32_t n = buildDispatch(code.cpu, 0x10000, 0x20000);
            for (uint32_t i = 0; i < n; i++)
                if ((code.cpu[i] & 0xFFFF00FFu) == (PACKET3(0x15, 3) & 0xFFFF00FFu) && ((code.cpu[i] >> 8) & 0xFF) == 0x15) code.cpu[i] |= 2u;
            if (step == 3) code.cpu[0x100 / 4] = 0xBFA0FFFFu;                // s_branch -1: the wave never ends
            if (step == 1 || step == 4) {                                    // WAIT_REG_MEM on a dword in the data buffer that never holds the reference
                n = 0;
                code.cpu[n++] = PACKET3(0x3C, 5); code.cpu[n++] = 3u | (1u << 4); code.cpu[n++] = 0x20010; code.cpu[n++] = 0;
                code.cpu[n++] = 0xDEADBEEFu; code.cpu[n++] = 0xFFFFFFFFu; code.cpu[n++] = 4;
            }
            data.cpu[0] = 0xDEAD0000u; data.cpu[4] = 0;
            struct rn_uc_submit in; memset(&in, 0, sizeof(in));
            in.ib_va = 0x10000; in.ib_dwords = n; in.flags = RN_SUBMIT_GFX;
            struct rn_uc_submit_result out; size_t outSize = sizeof(out);
            kr = IOConnectCallStructMethod(c, RN_UC_SUBMIT, &in, sizeof(in), &out, &outSize);
            uint32_t reached = 0;
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            if (kr) { printf("step %d: submit 0x%x\n", step, kr); ok = 0; break; }
            waitSeq(c, out.seq, hang && step != 4 ? 1500 : 10000, &reached, NULL);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            const long ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
            printf("step %d (%s): seq 0x%08x, reached %u status %u after %ld ms, data 0x%08x\n", step, !hang ? "good dispatch" : step == 3 ? "shader loop" : "CP wait", out.seq, reached, gStatus, ms, data.cpu[0]);
            if (!hang) { if (!reached || data.cpu[0] != 0x600DC0DEu) ok = 0; continue; }
            if (step == 4) { if (!reached || gStatus != RN_UC_SEQ_LOST || ms < 7000) ok = 0; continue; }   // the kernel did it
            if (reached) { ok = 0; break; }                                  // it must still be stuck here
            uint32_t one = 1;
            struct rn_uc_reset_result rr; size_t rrSize = sizeof(rr);
            kr = IOConnectCallStructMethod(c, RN_UC_QUEUE_RESET, &one, sizeof(one), &rr, &rrSize);
            printf("  graphics queue reset: 0x%x, status %u, resets %u, lost 0x%x..0x%x\n", kr, rr.status, rr.resets, rr.lost_from, rr.lost_to);
            if (kr || rr.status) { ok = 0; break; }
            waitSeq(c, out.seq, 1000, &reached, NULL);
            printf("  waiting for the lost sequence: reached %u, status %u (expect 1 and %u = lost)\n", reached, gStatus, RN_UC_SEQ_LOST);
            if (!reached || gStatus != RN_UC_SEQ_LOST) ok = 0;
        }
        {   // both queues after all that
            const uint32_t n = buildDispatch(code.cpu, 0x10000, 0x20000);
            for (int q = 1; q >= 0; q--) {
                uint32_t nn = n;
                for (uint32_t i = 0; q && i < nn; i++)
                    if ((code.cpu[i] & 0xFFFF00FFu) == (PACKET3(0x15, 3) & 0xFFFF00FFu) && ((code.cpu[i] >> 8) & 0xFF) == 0x15) code.cpu[i] |= 2u;
                data.cpu[0] = 0xDEAD0000u;
                struct rn_uc_submit in; memset(&in, 0, sizeof(in));
                in.ib_va = 0x10000; in.ib_dwords = nn; in.flags = q ? RN_SUBMIT_GFX : 0;
                struct rn_uc_submit_result out; size_t outSize = sizeof(out);
                kr = IOConnectCallStructMethod(c, RN_UC_SUBMIT, &in, sizeof(in), &out, &outSize);
                uint32_t reached = 0;
                if (!kr) waitSeq(c, out.seq, 3000, &reached, NULL);
                printf("final %s dispatch: submit 0x%x, reached %u, result 0x%08x\n", q ? "graphics" : "compute", kr, reached, data.cpu[0]);
                if (kr || !reached || data.cpu[0] != 0x600DC0DEu) ok = 0;
            }
        }
        struct rn_uc_diag d;
        if (diag(c, 0, &d) == KERN_SUCCESS) printf("fault status 0x%x, address 0x%x%08x\n", d.fault_status, d.fault_addr_hi, d.fault_addr_lo);
        printf("%s\n", ok ? "PASS" : "FAIL");
        IOServiceClose(c);
        return ok ? 0 : 2;
    }
    if (argc > 3 && !strcmp(argv[1], "peekv")) {                // peekv OFFSET COUNT: dwords of VRAM at a byte offset (hex), by the index/data registers
        struct rn_uc_vram_peek in = {strtoull(argv[2], NULL, 16), (uint32_t)atoi(argv[3]), 0};
        struct rn_uc_vram_peek_result out;
        size_t outSize = sizeof(out);
        kr = IOConnectCallStructMethod(c, RN_UC_VRAM_PEEK, &in, sizeof(in), &out, &outSize);
        printf("peekv: 0x%x\n", kr);
        for (uint32_t i = 0; kr == KERN_SUCCESS && i < in.count; i++) printf("%08x%s", out.v[i], (i & 7) == 7 || i + 1 == in.count ? "\n" : " ");
        IOServiceClose(c);
        return kr != KERN_SUCCESS;
    }
    if (argc > 1 && !strcmp(argv[1], "stats")) {                // counters of the queue: sequence numbers, ring-full waits, flush failures
        struct rn_uc_diag d;
        if (diag(c, 0, &d) == KERN_SUCCESS)
            printf("rptr 0x%x wptr 0x%x, ring-full waits %u, fault status 0x%x address 0x%x%08x, TLB flush failures %u\n", d.hqd_rptr, d.hqd_wptr, d.ring_full_waits, d.fault_status, d.fault_addr_hi, d.fault_addr_lo, d.flush_failures);
        if (diag(c, 0, &d) == KERN_SUCCESS)
            printf("queue resets: compute %u, graphics %u; graphics ring rptr 0x%x wptr 0x%x\n", d.queue_resets, d.gfx_queue_resets, d.gfx_rptr, d.gfx_wptr);
        {
            struct rn_uc_info inf; size_t infSize = sizeof(inf);
            if (IOConnectCallStructMethod(c, RN_UC_INFO, NULL, 0, &inf, &infSize) == KERN_SUCCESS)
                printf("VRAM: device-local %llu MB, %llu MB free; CPU-visible %llu MB, %llu MB free; this connection %u buffers\n", inf.vram_bytes >> 20, inf.vram_free >> 20, inf.vram_vis_bytes >> 20, inf.vram_vis_free >> 20, inf.bo_count);
        }
        IOServiceClose(c);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "rawib")) {                // rawib FILE: submit the command buffer in FILE (one hex dword per line, e.g. from RADV_DEBUG=dumpibs) as is, wait, report; a stuck queue is reset
        FILE *f = fopen(argv[2], "r");
        if (!f) { printf("cannot open %s\n", argv[2]); return 1; }
        Bo code;
        if (createBo(c, 4096, &code) || vaMap(c, code.handle, 0x10000, 0, 4096)) return 1;
        uint32_t n = 0, v;
        char line[128];
        while (fgets(line, sizeof(line), f) && n < 1000) if (sscanf(line, "%x", &v) == 1) code.cpu[n++] = v;
        fclose(f);
        kr = submitIb(c, 0x10000, n);
        uint32_t r = 0;
        kern_return_t kw = waitSeq(c, gSeq, 3000, &r, NULL);
        printf("rawib: %u dwords, submit 0x%x, wait 0x%x, fence %s\n", n, kr, kw, r ? "reached" : "NOT reached");
        struct rn_uc_diag d;
        if (diag(c, RN_UC_DIAG_CLEAR_FAULT, &d) == KERN_SUCCESS)
            printf("       rptr 0x%x wptr 0x%x CP_STAT 0x%x fault status 0x%x addr 0x%x%08x\n", d.hqd_rptr, d.hqd_wptr, d.cp_stat, d.fault_status, d.fault_addr_hi, d.fault_addr_lo);
        IOServiceClose(c);
        return r ? 0 : 2;
    }
    if (argc > 3 && !strcmp(argv[1], "replay")) {               // replay DIR VA:DWORDS ... [peek=VA]: map every DIR/va_<hex>.bin (from the bridge's RN_BRIDGE_DUMP) at its address, submit the listed command buffers in order
        DIR *dir = opendir(argv[2]);
        if (!dir) { printf("cannot open %s\n", argv[2]); return 1; }
        struct { uint64_t va, size; volatile uint32_t *cpu; } maps[64];
        int nm = 0;
        struct dirent *de;
        while ((de = readdir(dir)) && nm < 64) {
            unsigned long long va;
            if (sscanf(de->d_name, "va_%llx.bin", &va) != 1) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", argv[2], de->d_name);
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            Bo b;
            if (createBo(c, (uint64_t)len, &b)) return 1;
            if (fread((void *)b.cpu, 1, (size_t)len, f) != (size_t)len) return 1;
            fclose(f);
            if (vaMap(c, b.handle, va, 0, b.size)) { printf("map 0x%llx failed\n", va); return 1; }
            maps[nm].va = va; maps[nm].size = b.size; maps[nm].cpu = b.cpu; nm++;
            printf("mapped %llu bytes at 0x%llx\n", (unsigned long long)b.size, va);
        }
        closedir(dir);
        unsigned long long peek = 0;
        int ok = 1;
        for (int i = 3; i < argc; i++) {
            unsigned long long va; unsigned dw;
            if (sscanf(argv[i], "peek=%llx", &peek) == 1) continue;
            if (sscanf(argv[i], "%llx:%u", &va, &dw) != 2) continue;
            kr = submitIb(c, va, dw);
            uint32_t r = 0;
            kern_return_t kw = waitSeq(c, gSeq, 3000, &r, NULL);
            printf("submit ib 0x%llx, %u dwords: 0x%x, wait 0x%x, fence %s\n", va, dw, kr, kw, r ? "reached" : "NOT reached");
            if (!r) ok = 0;
        }
        for (int i = 0; peek && i < nm; i++)
            if (peek >= maps[i].va && peek < maps[i].va + maps[i].size)
                printf("peek 0x%llx: 0x%08x 0x%08x 0x%08x 0x%08x\n", peek, maps[i].cpu[(peek - maps[i].va) / 4], maps[i].cpu[(peek - maps[i].va) / 4 + 1], maps[i].cpu[(peek - maps[i].va) / 4 + 2], maps[i].cpu[(peek - maps[i].va) / 4 + 3]);
        struct rn_uc_diag d;
        if (diag(c, RN_UC_DIAG_CLEAR_FAULT, &d) == KERN_SUCCESS)
            printf("       rptr 0x%x wptr 0x%x CP_STAT 0x%x fault status 0x%x addr 0x%x%08x\n", d.hqd_rptr, d.hqd_wptr, d.cp_stat, d.fault_status, d.fault_addr_hi, d.fault_addr_lo);
        IOServiceClose(c);
        return ok ? 0 : 2;
    }
    struct rn_uc_info info;
    size_t infoSize = sizeof(info);
    kr = IOConnectCallStructMethod(c, RN_UC_INFO, NULL, 0, &info, &infoSize);
    if (kr != KERN_SUCCESS || info.version != RN_UAPI_VERSION) { printf("info: 0x%x, interface version %u (built for %u)\n", kr, info.version, RN_UAPI_VERSION); return 1; }
    printf("GART base 0x%llx, %u of %u buffers in use, doorbell %s\n", info.gart_base, info.bo_count, info.bo_limit, info.doorbell_mapped ? "mapped" : "not mapped");

    Bo code, data;
    if (createBo(c, 4096, &code) || createBo(c, 4096, &data)) return 1;
    printf("code buffer: handle %u, kernel-window address 0x%llx\ndata buffer: handle %u, kernel-window address 0x%llx\n", code.handle, code.va, data.handle, data.va);
    printf("address space: VMID %u, window 0x%llx + 0x%llx\n", info.vmid, info.vm_va_start, info.vm_va_bytes);
    const uint64_t codeVa = info.vm_va_start, dataVa = info.vm_va_start + 0x10000;
    if (vaMap(c, code.handle, codeVa, 0, 4096) || vaMap(c, data.handle, dataVa, 0, 4096)) { printf("va map failed\n"); return 1; }
    const uint32_t n = buildDispatch(code.cpu, codeVa, dataVa);

    data.cpu[0] = 0xDEAD0000u;
    data.cpu[1] = 0;
    kr = submitIb(c, codeVa, n);
    printf("submit: 0x%x\n", kr);
    uint32_t reached = 0;
    kr = waitSeq(c, gSeq, 2000, &reached, NULL);
    printf("wait for sequence %u: 0x%x, reached %u\n", gSeq, kr, reached);
    printf("result 0x%08x (expected 0x600DC0DE)\n", data.cpu[0]);
    {   // the buffer-dword wait still works for results a shader writes itself
        uint32_t got = 0;
        kr = waitDword(c, data.handle, 0, 0x600DC0DEu, 1000, &got);
        printf("wait for the result dword: 0x%x, reached %u\n", kr, got);
        if (kr || !got) reached = 0;
    }
    int ok = reached && data.cpu[0] == 0x600DC0DEu;
    if (!ok) {
        struct rn_uc_diag d;
        if (diag(c, 0, &d) == KERN_SUCCESS)
            printf("queue: rptr 0x%x wptr 0x%x active %u, CP_STAT 0x%x, VM fault status 0x%x address 0x%x%08x, ring wptr 0x%x\n"
                   "       SDMA0_VERSION 0x%x (0x600 = GC powered), GRBM_STATUS 0x%x, RLC_GPM_STAT 0x%x\n"
                   "       HQD doorbell control 0x%x, CP_PQ_STATUS 0x%x, MEC doorbell range 0x%x-0x%x, DOORBELL_APER_EN 0x%x,\n"
                   "       PQ base 0x%x, PQ control 0x%x, MQD base 0x%x\n",
                   d.hqd_rptr, d.hqd_wptr, d.hqd_active, d.cp_stat, d.fault_status, d.fault_addr_hi, d.fault_addr_lo, d.ring_wptr,
                   d.sdma0_version, d.grbm_status, d.rlc_gpm_stat, d.hqd_doorbell_control, d.cp_pq_status, d.doorbell_range_lower,
                   d.doorbell_range_upper, d.doorbell_aper_en, d.pq_base, d.pq_control, d.mqd_base);
        if (diag(c, 0, &d) == KERN_SUCCESS) printf("       sequence %u submitted, %u signaled; TLB flush failures %u, last acknowledge 0x%x\n", d.seq_submitted, d.seq_signaled, d.flush_failures, d.flush_last_ack);
    }
    if (ok && argc > 1 && !strcmp(argv[1], "ring")) {          // rptr write-back word against the driver's wptr over many submits (the ring holds 2048 dwords)
        for (uint32_t k = 1; k <= 300; k++) {
            const uint32_t fv = 0xFEED0000u + k, h = 0;
            uint32_t r2 = 0;
            if (submitIb(c, codeVa, n) != KERN_SUCCESS) { printf("submit %u failed\n", k); ok = 0; break; }
            waitSeq(c, gSeq, 2000, &r2, NULL); (void)h; (void)fv;
            if (!r2) { printf("submit %u: fence not reached\n", k); ok = 0; break; }
            if (k == 1 || k % 50 == 0) {
                struct rn_uc_diag d;
                if (diag(c, 0, &d) == KERN_SUCCESS)
                    printf("after %u submits: rptr word %u, wptr word %u, driver wptr %u, full waits %u\n", k, d.ring_rptr_word, d.ring_wptr_word, d.driver_wptr, d.ring_full_waits);
            }
        }
        // a burst without waiting: 1500 submits through the ring
        for (uint32_t k = 1; k <= 1500 && ok; k++) {
            if (submitIb(c, codeVa, n) != KERN_SUCCESS) { printf("burst submit %u failed\n", k); ok = 0; }
        }
        uint32_t r2 = 0;
        if (ok) { waitSeq(c, gSeq, 5000, &r2, NULL); if (!r2) { printf("burst: last fence not reached\n"); ok = 0; } }
        struct rn_uc_diag d;
        if (diag(c, 0, &d) == KERN_SUCCESS)
            printf("after the burst: rptr word %u, wptr word %u, driver wptr %u, full waits %u, result 0x%08x\n", d.ring_rptr_word, d.ring_wptr_word, d.driver_wptr, d.ring_full_waits, data.cpu[0]);
    }
    if (ok && argc > 1 && !strcmp(argv[1], "seq")) {           // sequence numbers of one context
        uint32_t first = gSeq, r = 0, sig = 0, prev = gSeq;
        for (int k = 0; k < 5 && ok; k++) {
            data.cpu[0] = 0xDEAD0000u;
            if (submitIb(c, codeVa, n) != KERN_SUCCESS) { printf("submit failed\n"); ok = 0; break; }
            if (gSeq != prev + 1) { printf("sequence %u after %u: not consecutive\n", gSeq, prev); ok = 0; }
            prev = gSeq;
        }
        kr = waitSeq(c, gSeq, 2000, &r, &sig);
        printf("sequences %u..%u: wait 0x%x reached %u, fence now %u\n", first + 1, gSeq, kr, r, sig); if (kr || !r || sig != gSeq) ok = 0;
        kr = waitSeq(c, first + 1, 0, &r, &sig);
        printf("an older one, looked at without waiting: 0x%x reached %u (expect 1)\n", kr, r); if (kr || !r) ok = 0;
        kr = waitSeq(c, gSeq + 1000, 0, &r, &sig);
        printf("a sequence never submitted: 0x%x (expect an error)\n", kr); if (kr == KERN_SUCCESS) ok = 0;
        kr = waitSeq(c, 0, 0, &r, &sig);
        printf("sequence 0: 0x%x (expect an error)\n", kr); if (kr == KERN_SUCCESS) ok = 0;
        kr = submitIb(c, info.vm_va_start + 0x30000, n);
        uint32_t after = gSeq;
        printf("a refused submit does not use a number: 0x%x, last number %u (expect %u)\n", kr, after, prev); if (kr == KERN_SUCCESS || after != prev) ok = 0;
        if (submitIb(c, codeVa, n) == KERN_SUCCESS && gSeq == prev + 1) { waitSeq(c, gSeq, 2000, &r, &sig); printf("the next one is %u, reached %u\n", gSeq, r); if (!r) ok = 0; } else ok = 0;
        io_connect_t c2;                                       // a second connection counts on its own
        if (IOServiceOpen(svc, mach_task_self(), 0, &c2) == KERN_SUCCESS) {
            uint32_t keep = gSeq;
            kr = submitIb(c2, codeVa, n);                     // refused: not its buffer
            kr = waitSeq(c2, 1, 0, &r, &sig);
            printf("second connection, sequence 1 before any submit of its own: 0x%x (expect an error)\n", kr); if (kr == KERN_SUCCESS) ok = 0;
            gSeq = keep;
            IOServiceClose(c2);
        }
    }
    if (ok && argc > 1 && !strcmp(argv[1], "info")) {          // the INFO queries, in the structures DRM_AMDGPU_INFO uses
        struct drm_amdgpu_info req;
        uint8_t buf[256];
        size_t bufSize;
#define QUERY(ID, SIZE) do { memset(&req, 0, sizeof(req)); req.query = (ID); req.return_size = (SIZE); bufSize = (SIZE); memset(buf, 0xAA, sizeof(buf)); \
                              kr = IOConnectCallStructMethod(c, RN_UC_INFO_QUERY, &req, sizeof(req), buf, &bufSize); } while (0)
        QUERY(AMDGPU_INFO_ACCEL_WORKING, 4);
        printf("ACCEL_WORKING: 0x%x, %u (expect 1)\n", kr, *(uint32_t *)buf); if (kr || *(uint32_t *)buf != 1) ok = 0;
        memset(&req, 0, sizeof(req)); req.query = AMDGPU_INFO_HW_IP_COUNT; req.return_size = 4; req.query_hw_ip.type = AMDGPU_HW_IP_COMPUTE; bufSize = 4;
        kr = IOConnectCallStructMethod(c, RN_UC_INFO_QUERY, &req, sizeof(req), buf, &bufSize);
        printf("HW_IP_COUNT compute: 0x%x, %u (expect 1)\n", kr, *(uint32_t *)buf); if (kr || *(uint32_t *)buf != 1) ok = 0;
        req.query_hw_ip.type = AMDGPU_HW_IP_GFX; bufSize = 4;
        kr = IOConnectCallStructMethod(c, RN_UC_INFO_QUERY, &req, sizeof(req), buf, &bufSize);
        printf("HW_IP_COUNT gfx: 0x%x, %u (the kext submits to no graphics queue yet: expect 0)\n", kr, *(uint32_t *)buf); if (kr || *(uint32_t *)buf != 0) ok = 0;
        memset(&req, 0, sizeof(req)); req.query = AMDGPU_INFO_HW_IP_INFO; req.return_size = sizeof(struct drm_amdgpu_info_hw_ip); req.query_hw_ip.type = AMDGPU_HW_IP_COMPUTE; bufSize = req.return_size;
        kr = IOConnectCallStructMethod(c, RN_UC_INFO_QUERY, &req, sizeof(req), buf, &bufSize);
        struct drm_amdgpu_info_hw_ip *ip = (struct drm_amdgpu_info_hw_ip *)buf;
        printf("HW_IP_INFO compute: 0x%x, version %u.%u, rings 0x%x, alignment %u/%u, discovery 0x%x\n", kr, ip->hw_ip_version_major, ip->hw_ip_version_minor, ip->available_rings,
               ip->ib_start_alignment, ip->ib_size_alignment, ip->ip_discovery_version);
        if (kr || ip->hw_ip_version_major != 11 || ip->available_rings != 1) ok = 0;
        QUERY(AMDGPU_INFO_MEMORY, sizeof(struct drm_amdgpu_memory_info));
        struct drm_amdgpu_memory_info *mi = (struct drm_amdgpu_memory_info *)buf;
        printf("MEMORY: 0x%x, vram %llu MB, visible %llu MB, gtt %llu MB (used %llu MB)\n", kr, mi->vram.total_heap_size >> 20, mi->cpu_accessible_vram.total_heap_size >> 20,
               mi->gtt.total_heap_size >> 20, mi->gtt.heap_usage >> 20);
        if (kr || mi->vram.total_heap_size < (1ull << 30) || mi->cpu_accessible_vram.total_heap_size == 0) ok = 0;
        QUERY(AMDGPU_INFO_VRAM_GTT, sizeof(struct drm_amdgpu_info_vram_gtt));
        printf("VRAM_GTT: 0x%x, %llu MB, %llu MB, %llu MB\n", kr, ((struct drm_amdgpu_info_vram_gtt *)buf)->vram_size >> 20, ((struct drm_amdgpu_info_vram_gtt *)buf)->vram_cpu_accessible_size >> 20,
               ((struct drm_amdgpu_info_vram_gtt *)buf)->gtt_size >> 20);
        if (kr) ok = 0;
        QUERY(AMDGPU_INFO_GTT_USAGE, 8);
        printf("GTT_USAGE: 0x%x, %llu KB\n", kr, *(uint64_t *)buf >> 10); if (kr) ok = 0;
        QUERY(AMDGPU_INFO_ACCEL_WORKING, 2);
        printf("a buffer of 2 bytes for a 4-byte answer: 0x%x, bytes %02x %02x then %02x (expect 01 00 then the fill byte aa)\n", kr, buf[0], buf[1], buf[2]);
        if (kr || buf[0] != 1 || buf[1] != 0 || buf[2] != 0xAA) ok = 0;
        {   // DEV_INFO as Linux answered it on this card (RedNavi/linux_info_navi32.h), whole structure in one call
            uint8_t dev[sizeof(struct drm_amdgpu_info_device)];
            struct drm_amdgpu_info rq; size_t devSize = sizeof(dev);
            memset(&rq, 0, sizeof(rq)); rq.query = AMDGPU_INFO_DEV_INFO; rq.return_size = sizeof(dev);
            kr = IOConnectCallStructMethod(c, RN_UC_INFO_QUERY, &rq, sizeof(rq), dev, &devSize);
            struct drm_amdgpu_info_device *d = (struct drm_amdgpu_info_device *)dev;
            printf("DEV_INFO: 0x%x, device 0x%x, family %u, %u SE x %u SA, %u CU, rb mask 0x%x, clock %llu kHz, va 0x%llx-0x%llx, high va 0x%llx-0x%llx, mall %llu MB\n", kr, d->device_id,
                   d->family, d->num_shader_engines, d->num_shader_arrays_per_engine, d->cu_active_number, d->enabled_rb_pipes_mask, d->max_engine_clock, d->virtual_address_offset,
                   d->virtual_address_max, d->high_va_offset, d->high_va_max, d->mall_size >> 20);
            if (kr || d->device_id != 0x747e || d->family != 145 || d->num_shader_engines != 3 || d->cu_active_number != 60 || d->virtual_address_offset != 0x10000) ok = 0;
        }
        {   // firmware versions, as the kernel reports the ones it loaded (the kext loads the same set)
            static const struct { uint32_t type, ver, feat; const char *name; } fw[] = {{AMDGPU_INFO_FW_GFX_ME, 0x9a6, 29, "ME"}, {AMDGPU_INFO_FW_GFX_PFP, 0xa50, 29, "PFP"},
                                                                                       {AMDGPU_INFO_FW_GFX_MEC, 0xa78, 29, "MEC"}};
            for (int i = 0; i < 3; i++) {
                memset(&req, 0, sizeof(req)); req.query = AMDGPU_INFO_FW_VERSION; req.return_size = 8; req.query_fw.fw_type = fw[i].type; bufSize = 8;
                kr = IOConnectCallStructMethod(c, RN_UC_INFO_QUERY, &req, sizeof(req), buf, &bufSize);
                printf("FW_VERSION %s: 0x%x, version 0x%x feature %u (expect 0x%x and %u)\n", fw[i].name, kr, ((uint32_t *)buf)[0], ((uint32_t *)buf)[1], fw[i].ver, fw[i].feat);
                if (kr || ((uint32_t *)buf)[0] != fw[i].ver || ((uint32_t *)buf)[1] != fw[i].feat) ok = 0;
            }
            memset(&req, 0, sizeof(req)); req.query = AMDGPU_INFO_FW_VERSION; req.return_size = 8; req.query_fw.fw_type = 0x7f; bufSize = 8;
            kr = IOConnectCallStructMethod(c, RN_UC_INFO_QUERY, &req, sizeof(req), buf, &bufSize);
            printf("a firmware this chip does not have: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
        }
        QUERY(AMDGPU_INFO_MAX_IBS, 36);
        printf("MAX_IBS: 0x%x, compute %u (a submit takes one IB: expect 1)\n", kr, ((uint32_t *)buf)[1]); if (kr || ((uint32_t *)buf)[1] != 1) ok = 0;
        QUERY(AMDGPU_INFO_GPUVM_FAULT, 16);
        printf("GPUVM_FAULT: 0x%x, status 0x%x\n", kr, ((uint32_t *)buf)[2]); if (kr) ok = 0;
        QUERY(AMDGPU_INFO_TIMESTAMP, 8);
        printf("TIMESTAMP: 0x%x (not answered yet: expect an error)\n", kr); if (!kr) ok = 0;
        QUERY(0x7777, 4);
        printf("an unknown query: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
#undef QUERY
    }
    if (ok && argc > 1 && !strcmp(argv[1], "syncobj")) {       // synchronisation objects: binary state, timeline, work attached by a submit, transfer
        uint32_t b = 0, t = 0, dst = 0, sig = 0, never = 0, r_ = 0;
        uint64_t pt = 0, p5 = 5, p6 = 6;
        uint32_t two[2];
        if (soCreate(c, 0, &b) || soCreate(c, 0, &t) || soCreate(c, 0, &dst) || soCreate(c, RN_UC_SYNCOBJ_CREATE_SIGNALED, &sig) || soCreate(c, 0, &never)) { printf("create failed\n"); ok = 0; }
        else {
            printf("handles %u %u %u %u %u\n", b, t, dst, sig, never);
            kr = soWait(c, &sig, NULL, 1, RN_UC_SYNCOBJ_WAIT_ALL, 0);
            printf("a signaled object, timeout 0: 0x%x (expect 0)\n", kr); if (kr) ok = 0;
            kr = soWait(c, &b, NULL, 1, 0, 0);
            printf("an object nothing is attached to, no wait for submit: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
            kr = soWait(c, &b, NULL, 1, RN_UC_SYNCOBJ_WAIT_FOR_SUBMIT, 5);
            printf("the same, waiting for a submit for 5 ms: 0x%x (expect a timeout, 0xe00002d6)\n", kr); if (kr != (kern_return_t)0xe00002d6) ok = 0;
            if (soArray(c, RN_UC_SYNCOBJ_SIGNAL, b, 0, NULL) || soWait(c, &b, NULL, 1, 0, 0)) { printf("signal then wait failed\n"); ok = 0; }
            else printf("signaled by hand, then waited for: ok\n");
            if (soArray(c, RN_UC_SYNCOBJ_RESET, b, 0, NULL)) ok = 0;
            kr = soWait(c, &b, NULL, 1, RN_UC_SYNCOBJ_WAIT_FOR_SUBMIT, 5);
            printf("after a reset: 0x%x (expect a timeout)\n", kr); if (kr != (kern_return_t)0xe00002d6) ok = 0;

            // work attached by a submit: signaled when the IB is done
            data.cpu[0] = 0xDEAD0000u;
            rn_uc_sync_ref outB = {b, 0, 0};
            if (submitSync(c, codeVa, n, NULL, 0, &outB, 1)) { printf("submit with an output object failed\n"); ok = 0; }
            kr = soWait(c, &b, NULL, 1, RN_UC_SYNCOBJ_WAIT_FOR_SUBMIT, 2000);
            waitDword(c, data.handle, 0, 0x600DC0DEu, 1000, &r_);
            printf("binary object signaled by a submit: wait 0x%x, result 0x%08x (expect 0 and 0x600dc0de)\n", kr, data.cpu[0]); if (kr || !r_) ok = 0;

            // the timeline
            rn_uc_sync_ref out5 = {t, 0, 5};
            if (submitSync(c, codeVa, n, NULL, 0, &out5, 1)) ok = 0;
            kr = soWait(c, &t, &p5, 1, RN_UC_SYNCOBJ_WAIT_FOR_SUBMIT, 2000);
            soArray(c, RN_UC_SYNCOBJ_QUERY, t, 0, &pt);
            printf("timeline point 5 signaled by a submit: wait 0x%x, value %llu (expect 0 and 5)\n", kr, pt); if (kr || pt != 5) ok = 0;
            kr = soWait(c, &t, &p6, 1, RN_UC_SYNCOBJ_WAIT_FOR_SUBMIT, 20);
            printf("point 6, nobody signals it: 0x%x (expect a timeout)\n", kr); if (kr != (kern_return_t)0xe00002d6) ok = 0;
            if (soArray(c, RN_UC_SYNCOBJ_SIGNAL, t, 6, NULL) || soWait(c, &t, &p6, 1, 0, 0)) { printf("signal of point 6 failed\n"); ok = 0; }
            soArray(c, RN_UC_SYNCOBJ_QUERY, t, 0, &pt);
            printf("point 6 signaled by hand: value %llu (expect 6)\n", pt); if (pt != 6) ok = 0;

            // transfer: the destination takes the source's work
            rn_uc_sync_ref out9 = {never, 0, 9};
            if (submitSync(c, codeVa, n, NULL, 0, &out9, 1)) ok = 0;
            struct rn_uc_syncobj_transfer tr = {never, dst, 9, 0, 0, 0};
            kr = IOConnectCallStructMethod(c, RN_UC_SYNCOBJ_TRANSFER, &tr, sizeof(tr), NULL, NULL);
            uint64_t zero = 0;
            kern_return_t kw = soWait(c, &dst, &zero, 1, RN_UC_SYNCOBJ_WAIT_FOR_SUBMIT, 2000);
            printf("transfer of point 9 into a binary object: 0x%x, then wait 0x%x (expect 0 and 0)\n", kr, kw); if (kr || kw) ok = 0;

            // an input object: the IB waits for it first
            rn_uc_sync_ref inS = {sig, 0, 0};
            kr = submitSync(c, codeVa, n, &inS, 1, NULL, 0);
            printf("a submit that waits for a signaled object: 0x%x (expect 0)\n", kr); if (kr) ok = 0;
            rn_uc_sync_ref inBad = {99, 0, 0};
            const uint32_t before = gSeq;
            kr = submitSync(c, codeVa, n, &inBad, 1, NULL, 0);
            printf("a submit that waits for an object that does not exist: 0x%x, sequence stays %u (expect an error, same number)\n", kr, gSeq); if (!kr || gSeq != before) ok = 0;

            two[0] = never; two[1] = t;                          // a list with an object nothing was attached to
            kr = soWait(c, two, NULL, 2, 0, 5);
            printf("wait on a list where one object has no work (never = point 0): 0x%x\n", kr);
            uint32_t fresh = 0; soCreate(c, 0, &fresh);
            two[0] = fresh; two[1] = sig;
            kr = soWait(c, two, NULL, 2, 0, 5);
            printf("any-wait where one object has no work and one is signaled: 0x%x (expect an error, as DRM)\n", kr); if (!kr) ok = 0;
            io_connect_t c2;                                     // objects belong to their connection
            if (IOServiceOpen(svc, mach_task_self(), 0, &c2) == KERN_SUCCESS) {
                kr = soWait(c2, &t, &p5, 1, 0, 0);
                printf("another connection waits for this one's object: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
                IOServiceClose(c2);
            }
            kr = soDestroy(c, b);
            kern_return_t kr2 = soDestroy(c, b), kr3 = soWait(c, &b, NULL, 1, 0, 0);
            printf("destroy: 0x%x, again: 0x%x, wait on it: 0x%x (expect 0 and two errors)\n", kr, kr2, kr3); if (kr || !kr2 || !kr3) ok = 0;
        }
    }
    if (ok && argc > 1 && !strcmp(argv[1], "high")) {          // which part of a dispatch fails in the upper half: the command buffer, the shader, or the store
        const uint64_t lowC = 0x400000, highC = 0xffff800000200000ull, lowD = 0x500000, highD = 0xffff800ff0300000ull;
        if (vaMap(c, code.handle, lowC, 0, 4096) || vaMap(c, code.handle, highC, 0, 4096) || vaMap(c, data.handle, lowD, 0, 4096) || vaMap(c, data.handle, highD, 0, 4096)) {
            printf("map failed\n"); ok = 0;
        } else {
            static const struct { const char *name; int ibHigh, pgmHigh, storeHigh; } cases[] = {
                {"store high only", 0, 0, 1}, {"shader high only", 0, 1, 0}, {"shader and store high", 0, 1, 1},
                {"command buffer high only", 1, 0, 0}, {"command buffer and shader high", 1, 1, 0}, {"all high", 1, 1, 1}};   // the cases with the command buffer high last: they can leave the queue stuck
            const unsigned nCases = sizeof(cases) / sizeof(cases[0]);
            const int rev = argc > 2 && !strcmp(argv[2], "rev"), only = (argc > 2 && argv[2][0] >= '0' && argv[2][0] <= '9') ? atoi(argv[2]) : -1;
            const int repeat = (argc > 3) ? atoi(argv[3]) : 1;        // high N R: case N, R times, one line with the count
            for (unsigned kk = 0; kk < nCases * (unsigned)(only >= 0 ? repeat : 1); kk++) {
                const unsigned k = only >= 0 ? (unsigned)only : (rev ? nCases - 1 - kk : kk);
                if (only >= 0 && (int)k != only) continue;
                {   // every case from a freshly reset queue and a cleared fault latch
                    struct rn_uc_diag d0; struct rn_uc_reset_result r0; size_t r0Size = sizeof(r0);
                    IOConnectCallStructMethod(c, RN_UC_QUEUE_RESET, NULL, 0, &r0, &r0Size);
                    diag(c, RN_UC_DIAG_CLEAR_FAULT, &d0);
                }
                const uint64_t ibVa = cases[k].ibHigh ? highC : lowC, pgm = cases[k].pgmHigh ? highC : lowC, store = cases[k].storeHigh ? highD : lowD;
                const uint32_t nn = buildDispatch(code.cpu, pgm, store);
                data.cpu[0] = 0xDEAD0000u;
                kr = submitIb(c, ibVa, nn);
                if (cases[k].ibHigh) {
                    printf("%-34s: submit 0x%x (expect 0xe00002c7: refused, a command buffer in the upper half hangs the queue)\n", cases[k].name, kr);
                    if (kr != (kern_return_t)0xe00002c7) ok = 0;
                    continue;
                }
                uint32_t r1 = 0;
                waitSeq(c, gSeq, 1500, &r1, NULL);
                printf("%-34s: submit 0x%x, fence %s, result 0x%08x\n", cases[k].name, kr, r1 ? "reached" : "NOT reached", data.cpu[0]);
                {
                    struct rn_uc_diag d;
                    diag(c, 0, &d);
                    if (d.fault_status) printf("    VM fault 0x%x at page 0x%x%08x\n", d.fault_status, d.fault_addr_hi, d.fault_addr_lo);
                }
                if (!r1) {
                    struct rn_uc_diag d;
                    diag(c, 0, &d);
                    printf("    hqd rptr 0x%x wptr 0x%x active %u, pq control 0x%x, fault 0x%x at page 0x%x%08x, GRBM_STATUS 0x%x\n", d.hqd_rptr, d.hqd_wptr, d.hqd_active, d.pq_control,
                           d.fault_status, d.fault_addr_hi, d.fault_addr_lo, d.grbm_status);
                    diag(c, RN_UC_DIAG_CLEAR_FAULT, &d);
                    struct rn_uc_reset_result rr; size_t rrSize = sizeof(rr);
                    kr = IOConnectCallStructMethod(c, RN_UC_QUEUE_RESET, NULL, 0, &rr, &rrSize);
                    printf("    queue reset: 0x%x, status %u\n", kr, rr.status);
                }
            }
            buildDispatch(code.cpu, lowC, lowD);
            data.cpu[0] = 0xDEAD0000u;
            submitIb(c, lowC, buildDispatch(code.cpu, lowC, lowD));
            uint32_t r2 = 0; waitSeq(c, gSeq, 2000, &r2, NULL); waitDword(c, data.handle, 0, 0x600DC0DEu, 500, &r2);
            printf("a low dispatch afterwards: result 0x%08x, reached %u\n", data.cpu[0], r2); if (!r2) ok = 0;
        }
    }
    if (ok && argc > 1 && !strcmp(argv[1], "fresh")) {         // are fences of connections opened AFTER an IB-high hang broken, and the ones opened before not?
        Conn a, b, c3;
        const uint64_t highC = 0xffff800000200000ull;
        if (connSetup(svc, &a) || connSetup(svc, &b)) { printf("setup failed\n"); ok = 0; }
        else {
            connRun(&b, "connection B (opened before) first run");
            vaMap(a.c, a.code.handle, highC, 0, 4096);
            const uint32_t nh = buildDispatch(a.code.cpu, 0x10000, 0x20000);       // shader low, command buffer high: the case that hangs
            a.data.cpu[0] = 0xDEAD0000u;
            kr = submitIb(a.c, highC, nh);
            uint32_t r1 = 0; waitSeq(a.c, gSeq, 1500, &r1, NULL);
            printf("A: command buffer high, shader low: submit 0x%x, fence %s\n", kr, r1 ? "reached" : "NOT reached");
            struct rn_uc_reset_result rr; size_t rrSize = sizeof(rr);
            kr = IOConnectCallStructMethod(a.c, RN_UC_QUEUE_RESET, NULL, 0, &rr, &rrSize);
            printf("queue reset: 0x%x, status %u, info step %u\n", kr, rr.status, rr.status);
            int okA = connRun(&a, "connection A (the one that hung) afterwards");
            int okB = connRun(&b, "connection B (opened before) afterwards");
            const int okC = connSetup(svc, &c3) == 0 && connRun(&c3, "connection C (opened after) afterwards");
            printf("A %s, B %s, C %s\n", okA ? "ok" : "BROKEN", okB ? "ok" : "BROKEN", okC ? "ok" : "BROKEN");
        }
    }
    if (ok && argc > 1 && !strcmp(argv[1], "bo")) {            // buffer lifetime: handles, ownership, release on close
        Bo extra;
        struct rn_uc_bo_free fr = {extra.handle = 0, 0};
        if (createBo(c, 4096, &extra)) ok = 0;
        else if (vaMap(c, extra.handle, info.vm_va_start + 0x20000, 0, 4096)) { printf("va map of the extra buffer failed\n"); ok = 0; }
        else {
            fr.handle = extra.handle;
            kr = IOConnectCallStructMethod(c, RN_UC_BO_FREE, &fr, sizeof(fr), NULL, NULL);
            printf("free handle %u: 0x%x (expect 0)\n", fr.handle, kr); if (kr != KERN_SUCCESS) ok = 0;
            kr = IOConnectCallStructMethod(c, RN_UC_BO_FREE, &fr, sizeof(fr), NULL, NULL);
            printf("free it again: 0x%x (expect an error)\n", kr); if (kr == KERN_SUCCESS) ok = 0;
            kr = submitIb(c, info.vm_va_start + 0x20000, n);
            printf("submit from the freed buffer: 0x%x (expect an error)\n", kr); if (kr == KERN_SUCCESS) ok = 0;
            kr = submitIb(c, info.vm_va_start + 0x30000, n);
            printf("submit from an address of nobody's: 0x%x (expect an error)\n", kr); if (kr == KERN_SUCCESS) ok = 0;
            kr = submitIb(c, codeVa + 4096 - 8, n);
            printf("submit running past the end of the buffer: 0x%x (expect an error)\n", kr); if (kr == KERN_SUCCESS) ok = 0;
            Bo again;
            if (createBo(c, 4096, &again)) ok = 0;
            else printf("a new buffer after the free: handle %u\n", again.handle);
        }
        io_connect_t c2;                                       // a second connection must not reach the first one's buffers
        if (IOServiceOpen(svc, mach_task_self(), 0, &c2) == KERN_SUCCESS) {
            kr = submitIb(c2, codeVa, n);
            printf("second connection submits from the first one's buffer: 0x%x (expect an error)\n", kr); if (kr == KERN_SUCCESS) ok = 0;
            IOServiceClose(c2);
        }
        for (int round = 0; round < 5 && ok; round++) {        // 5 x 30 buffers through a table of 64: only works if a closed connection frees its buffers
            io_connect_t c3;
            if (IOServiceOpen(svc, mach_task_self(), 0, &c3) != KERN_SUCCESS) { printf("open %d failed\n", round); ok = 0; break; }
            for (int i = 0; i < 30; i++) { Bo b; if (createBo(c3, 4096, &b)) { printf("round %d: buffer %d failed\n", round, i); ok = 0; break; } }
            IOServiceClose(c3);
        }
        printf("5 connections x 30 buffers: %s\n", ok ? "ok" : "failed");
        if (ok) {
            submitIb(c, codeVa, n);
            uint32_t r2 = 0; waitSeq(c, gSeq, 2000, &r2, NULL);
            printf("the queue still works afterwards: %s\n", r2 ? "yes" : "no"); if (!r2) ok = 0;
        }
    }
    if (ok && argc > 1 && (!strcmp(argv[1], "vm") || !strcmp(argv[1], "vmfault") || !strcmp(argv[1], "vmhole"))) {   // the address space itself; vmfault also makes the shader fault (the compute queue is stuck afterwards, see docs/COMPAT.md)
        uint32_t r = 0;
        const uint64_t W = info.vm_va_start, WS = info.vm_va_bytes, dataVa2 = W + 0x200000;
        Bo b2;
        if (createBo(c, 8192, &b2)) ok = 0;
        else {
            kr = vaMap(c, b2.handle, codeVa + 0x800, 0, 4096);
            printf("a mapping that is not page aligned: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
            kr = vaMap(c, b2.handle, W - 0x1000, 0, 4096);
            printf("a mapping below the window: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
            kr = vaMap(c, b2.handle, W + WS, 0, 4096);
            printf("a mapping above the window: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
            kr = vaMap(c, b2.handle, W + 0x40000, 4096, 8192);
            printf("a mapping past the end of the buffer: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
            kr = vaMap(c, b2.handle, codeVa, 0, 4096);
            printf("a mapping over another one: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
            kr = vaMap(c, 77, W + 0x40000, 0, 4096);
            printf("a mapping of a handle that does not exist: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
            kr = vaUnmap(c, codeVa, 8192);
            printf("an unmap that is not exactly one mapping: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
            // the same buffer at two addresses: a write through one shows in the other
            if (vaMap(c, b2.handle, W + 0x40000, 0, 8192) || vaMap(c, b2.handle, W + 0x80000, 4096, 4096)) { printf("map of the second buffer failed\n"); ok = 0; }
            else printf("a buffer of 2 pages mapped whole at 0x%llx and its second page again at 0x%llx\n", W + 0x40000, W + 0x80000);
        }
        // Move the data buffer: unmap it, map it somewhere else, point the shader at the new address, and the result lands in the same buffer.
        if (ok) {
            if (vaUnmap(c, dataVa, 4096) || vaMap(c, data.handle, dataVa2, 0, 4096)) { printf("moving the data buffer failed\n"); ok = 0; }
            else {
                const uint32_t n2 = buildDispatch(code.cpu, codeVa, dataVa2);
                data.cpu[0] = 0xDEAD0000u;
                if (submitIb(c, codeVa, n2)) ok = 0;
                waitSeq(c, gSeq, 2000, &r, NULL); waitDword(c, data.handle, 0, 0x600DC0DEu, 1000, &r);
                printf("the shader stores through the new address: result 0x%08x, reached %u\n", data.cpu[0], r); if (!r) ok = 0;
            }
        }
        if (ok && !strcmp(argv[1], "vmhole")) {   // the upper half is addressed in canonical form; what is not canonical, or crosses the hole, is refused
            kr = vaMap(c, code.handle, 0x0000800000200000ull, 0, 4096);
            printf("a mapping with bit 47 set and the high bits clear: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
            kr = vaMap(c, code.handle, 0x00007fffffffe000ull, 0, 8192);
            printf("a mapping that crosses 2^47: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
            kr = vaMap(c, code.handle, 0xfffffffffffff000ull, 0, 8192);
            printf("a mapping that runs past the top of the address space: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
        }
        // Far apart addresses, so that all four levels of the page table are used: different entries at every directory level.
        if (ok) {
            const uint64_t farCode = 0x00007ffffffe0000ull;
            const uint64_t farData = !strcmp(argv[1], "vmhole") ? 0xffff800ff0300000ull : 0x0000400000001000ull;
            const uint64_t farShader = !strcmp(argv[1], "vmhole") ? 0xffff800000200000ull : farCode;      // the shader may sit in the upper half too; the command buffer may not
            if (vaMap(c, code.handle, farCode, 0, 4096) || vaMap(c, data.handle, farData, 0, 4096) || (farShader != farCode && vaMap(c, code.handle, farShader, 0, 4096))) { printf("map at the far addresses failed\n"); ok = 0; }
            else {
                const uint32_t nf = buildDispatch(code.cpu, farShader, farData);
                data.cpu[0] = 0xDEAD0000u;
                if (submitIb(c, farCode, nf)) ok = 0;
                waitSeq(c, gSeq, 2000, &r, NULL); waitDword(c, data.handle, 0, 0x600DC0DEu, 1000, &r);
                printf("command buffer at 0x%llx, shader at 0x%llx, store to 0x%llx: result 0x%08x, reached %u\n", farCode, farShader, farData, data.cpu[0], r); if (!r) ok = 0;
                if (vaUnmap(c, farCode, 4096) || vaUnmap(c, farData, 4096) || (farShader != farCode && vaUnmap(c, farShader, 4096))) { printf("unmap at the far addresses failed\n"); ok = 0; }
                kr = submitIb(c, farCode, nf);
                printf("a submit from the address just unmapped: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
                kr = vaMap(c, data.handle, 0x0001000000000000ull, 0, 4096);
                printf("an address that is not canonical: 0x%x (expect an error)\n", kr); if (!kr) ok = 0;
            }
            buildDispatch(code.cpu, codeVa, dataVa2);
        }
        // A store to an address that is mapped nowhere in this address space: the access faults, nothing is written, the fault is latched.
        if (ok && !strcmp(argv[1], "vmfault")) {
            const uint64_t bad = W + 0x300000;
            struct rn_uc_diag d;
            diag(c, RN_UC_DIAG_CLEAR_FAULT, &d);
            diag(c, 0, &d);
            printf("fault status before: 0x%x, GRBM_STATUS 0x%x (idle)\n", d.fault_status, d.grbm_status);
            const uint32_t n3 = buildDispatch(code.cpu, codeVa, bad);
            data.cpu[0] = 0xDEAD0000u;
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            if (submitIb(c, codeVa, n3)) ok = 0;
            waitSeq(c, gSeq, 3000, &r, NULL);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            printf("the dispatch with the bad store finished after %ld ms: reached %u, data dword 0x%08x (expect 0xdead0000)\n",
                   (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000), r, data.cpu[0]);
            diag(c, 0, &d);
            printf("fault status after: 0x%x, address page 0x%x%08x (the bad address is page 0x%llx), GRBM_STATUS 0x%x\n", d.fault_status, d.fault_addr_hi, d.fault_addr_lo, bad >> 12, d.grbm_status);
            if (data.cpu[0] != 0xDEAD0000u || d.fault_status == 0) ok = 0;   // isolation: nothing written, the fault latched
            const uint32_t badSeq = gSeq;
            diag(c, RN_UC_DIAG_CLEAR_FAULT, &d);
            // Recovery 1: the client's own watchdog asks for the reset.
            struct rn_uc_reset_result rr; size_t rrSize = sizeof(rr);
            kr = IOConnectCallStructMethod(c, RN_UC_QUEUE_RESET, NULL, 0, &rr, &rrSize);
            printf("queue reset on request: 0x%x, status %u, resets %u, lost %u..%u (the bad dispatch was %u)\n", kr, rr.status, rr.resets, rr.lost_from, rr.lost_to, badSeq);
            if (kr || rr.status || (int32_t)(badSeq - rr.lost_from) < 0 || (int32_t)(rr.lost_to - badSeq) < 0) ok = 0;
            waitSeq(c, badSeq, 1000, &r, NULL);
            printf("waiting for the lost sequence %u: reached %u, status %u (expect 1 and %u = lost)\n", badSeq, r, gStatus, RN_UC_SEQ_LOST);
            if (!r || gStatus != RN_UC_SEQ_LOST) ok = 0;
            const uint32_t n4 = buildDispatch(code.cpu, codeVa, dataVa2);
            data.cpu[0] = 0xDEAD0000u;
            if (submitIb(c, codeVa, n4)) ok = 0;
            uint32_t rs = 0, rd = 0;
            waitSeq(c, gSeq, 2000, &rs, NULL); waitDword(c, data.handle, 0, 0x600DC0DEu, 1000, &rd);
            printf("a good dispatch after the reset: result 0x%08x, stored %u, fence reached %u, status %u\n", data.cpu[0], rd, rs, gStatus);
            if (!rd || !rs || gStatus != RN_UC_SEQ_DONE) ok = 0;
            // Recovery 2: nobody asks. A wait that sees the fence stand still for rednavi_hang_ms (8 s) makes the kernel reset the queue.
            const uint32_t n5 = buildDispatch(code.cpu, codeVa, bad);
            data.cpu[0] = 0xDEAD0000u;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            if (submitIb(c, codeVa, n5)) ok = 0;
            const uint32_t bad2 = gSeq;
            waitSeq(c, bad2, 10000, &r, NULL);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            printf("waiting 10 s for the second bad dispatch (sequence %u): reached %u, status %u after %ld ms (expect reached, lost, about 8000 ms)\n", bad2, r, gStatus,
                   (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
            if (!r || gStatus != RN_UC_SEQ_LOST) ok = 0;
            diag(c, RN_UC_DIAG_CLEAR_FAULT, &d);
            const uint32_t n6 = buildDispatch(code.cpu, codeVa, dataVa2);
            data.cpu[0] = 0xDEAD0000u;
            if (submitIb(c, codeVa, n6)) ok = 0;
            waitSeq(c, gSeq, 2000, &rs, NULL); waitDword(c, data.handle, 0, 0x600DC0DEu, 1000, &rd);
            printf("a good dispatch after the automatic reset: result 0x%08x, stored %u, fence reached %u, status %u\n", data.cpu[0], rd, rs, gStatus);
            if (!rd || !rs || gStatus != RN_UC_SEQ_DONE) ok = 0;
        }
    }
    printf("%s\n", ok ? "PASS" : "FAIL");
    IOServiceClose(c);
    return ok ? 0 : 2;
}
