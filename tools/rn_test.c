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
#include <string.h>

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

static uint32_t gSeq;                                      // the sequence number of the last submit that worked

static kern_return_t submitIb(io_connect_t c, uint64_t ibVa, uint32_t ibDwords) {
    struct rn_uc_submit in = {ibVa, ibDwords, 0};
    struct rn_uc_submit_result out = {0, 0};
    size_t outSize = sizeof(out);
    kern_return_t kr = IOConnectCallStructMethod(c, RN_UC_SUBMIT, &in, sizeof(in), &out, &outSize);
    if (kr == KERN_SUCCESS) gSeq = out.seq;
    return kr;
}

static kern_return_t waitSeq(io_connect_t c, uint32_t seq, uint32_t ms, uint32_t *reached, uint32_t *signaled) {
    struct rn_uc_wait_seq in = {seq, ms};
    struct rn_uc_wait_seq_result out = {0, 0};
    size_t outSize = sizeof(out);
    kern_return_t kr = IOConnectCallStructMethod(c, RN_UC_WAIT_SEQ, &in, sizeof(in), &out, &outSize);
    *reached = out.reached;
    if (signaled) *signaled = out.signaled;
    return kr;
}

static kern_return_t waitDword(io_connect_t c, uint32_t handle, uint32_t offset, uint32_t value, uint32_t ms, uint32_t *reached) {
    struct rn_uc_bo_wait in = {handle, offset, value, ms};
    struct rn_uc_bo_wait_result out = {0, 0};
    size_t outSize = sizeof(out);
    kern_return_t kr = IOConnectCallStructMethod(c, RN_UC_BO_WAIT, &in, sizeof(in), &out, &outSize);
    *reached = out.reached;
    return kr;
}

int main(int argc, char **argv) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedNaviProbe"));
    if (!svc) { printf("no RedNaviProbe service\n"); return 1; }
    io_connect_t c;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c);
    if (kr != KERN_SUCCESS) { printf("IOServiceOpen: 0x%x (administrator or boot-arg rednavi_uc_anyone=1)\n", kr); return 1; }

    struct rn_uc_info info;
    size_t infoSize = sizeof(info);
    kr = IOConnectCallStructMethod(c, RN_UC_INFO, NULL, 0, &info, &infoSize);
    if (kr != KERN_SUCCESS || info.version != RN_UAPI_VERSION) { printf("info: 0x%x, interface version %u (built for %u)\n", kr, info.version, RN_UAPI_VERSION); return 1; }
    printf("GART base 0x%llx, %u of %u buffers in use, doorbell %s\n", info.gart_base, info.bo_count, info.bo_limit, info.doorbell_mapped ? "mapped" : "not mapped");

    Bo code, data;
    if (createBo(c, 4096, &code) || createBo(c, 4096, &data)) return 1;
    printf("code buffer: handle %u, GPU address 0x%llx\ndata buffer: handle %u, GPU address 0x%llx\n", code.handle, code.va, data.handle, data.va);

    // The shader (GFX11 machine code, LLVM MC test encodings): store 0x600DC0DE to the first dword of the data buffer.
    const uint32_t shader[11] = {0xBE8000FFu, (uint32_t)data.va, 0xBE8100FFu, (uint32_t)(data.va >> 32), 0x7E020280u, 0x7E0402FFu, 0x600DC0DEu,
                                 0xDC6A0000u, 0x00000201u, 0xBC7C0000u, 0xBFB00000u};
    for (int i = 0; i < 64; i++) code.cpu[0x100 / 4 + i] = i < 11 ? shader[i] : 0xBF9F0000u;   // s_code_end padding

    // The command buffer: a 1x1x1 compute dispatch of that shader.
    uint32_t ib[48], n = 0;
    const uint64_t sva = code.va + 0x100;
    ib[n++] = PACKET3(0x76, 2); ib[n++] = SHREG(0x1bac); ib[n++] = (uint32_t)(sva >> 8); ib[n++] = (uint32_t)(sva >> 40);      // COMPUTE_PGM_LO/HI
    ib[n++] = PACKET3(0x76, 2); ib[n++] = SHREG(0x1bb2); ib[n++] = 0x002C0000u; ib[n++] = 0x80u;                              // PGM_RSRC1/2
    ib[n++] = PACKET3(0x76, 1); ib[n++] = SHREG(0x1bc8); ib[n++] = 0;                                                         // PGM_RSRC3
    ib[n++] = PACKET3(0x76, 3); ib[n++] = SHREG(0x1ba4); ib[n++] = 0; ib[n++] = 0; ib[n++] = 0;                               // START_X/Y/Z
    ib[n++] = PACKET3(0x76, 3); ib[n++] = SHREG(0x1ba7); ib[n++] = 1; ib[n++] = 1; ib[n++] = 1;                               // NUM_THREAD_X/Y/Z
    ib[n++] = PACKET3(0x76, 6); ib[n++] = SHREG(0x1bb5);                                                                      // limits, SE masks
    ib[n++] = 0; ib[n++] = 0xFFFFFFFFu; ib[n++] = 0xFFFFFFFFu; ib[n++] = 0; ib[n++] = 0xFFFFFFFFu; ib[n++] = 0xFFFFFFFFu;
    ib[n++] = PACKET3(0x15, 3); ib[n++] = 1; ib[n++] = 1; ib[n++] = 1; ib[n++] = 0x00000001u | 0x00000004u | 0x00000040u | 0x00008000u;   // DISPATCH_DIRECT
    for (uint32_t i = 0; i < n; i++) code.cpu[i] = ib[i];

    data.cpu[0] = 0xDEAD0000u;
    data.cpu[1] = 0;
    kr = submitIb(c, code.va, n);
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
        size_t dSize = sizeof(d);
        if (IOConnectCallStructMethod(c, RN_UC_DIAG, NULL, 0, &d, &dSize) == KERN_SUCCESS)
            printf("queue: rptr 0x%x wptr 0x%x active %u, CP_STAT 0x%x, VM fault status 0x%x address 0x%x%08x, ring wptr 0x%x\n"
                   "       SDMA0_VERSION 0x%x (0x600 = GC powered), GRBM_STATUS 0x%x, RLC_GPM_STAT 0x%x\n"
                   "       HQD doorbell control 0x%x, CP_PQ_STATUS 0x%x, MEC doorbell range 0x%x-0x%x, DOORBELL_APER_EN 0x%x,\n"
                   "       PQ base 0x%x, PQ control 0x%x, MQD base 0x%x\n",
                   d.hqd_rptr, d.hqd_wptr, d.hqd_active, d.cp_stat, d.fault_status, d.fault_addr_hi, d.fault_addr_lo, d.ring_wptr,
                   d.sdma0_version, d.grbm_status, d.rlc_gpm_stat, d.hqd_doorbell_control, d.cp_pq_status, d.doorbell_range_lower,
                   d.doorbell_range_upper, d.doorbell_aper_en, d.pq_base, d.pq_control, d.mqd_base);
    }
    if (ok && argc > 1 && !strcmp(argv[1], "ring")) {          // rptr write-back word against the driver's wptr over many submits (the ring holds 2048 dwords)
        for (uint32_t k = 1; k <= 300; k++) {
            const uint32_t fv = 0xFEED0000u + k, h = 0;
            uint32_t r2 = 0;
            if (submitIb(c, code.va, n) != KERN_SUCCESS) { printf("submit %u failed\n", k); ok = 0; break; }
            waitSeq(c, gSeq, 2000, &r2, NULL); (void)h; (void)fv;
            if (!r2) { printf("submit %u: fence not reached\n", k); ok = 0; break; }
            if (k == 1 || k % 50 == 0) {
                struct rn_uc_diag d; size_t dSize = sizeof(d);
                if (IOConnectCallStructMethod(c, RN_UC_DIAG, NULL, 0, &d, &dSize) == KERN_SUCCESS)
                    printf("after %u submits: rptr word %u, wptr word %u, driver wptr %u, full waits %u\n", k, d.ring_rptr_word, d.ring_wptr_word, d.driver_wptr, d.ring_full_waits);
            }
        }
        // a burst without waiting: 1500 submits through the ring
        for (uint32_t k = 1; k <= 1500 && ok; k++) {
            if (submitIb(c, code.va, n) != KERN_SUCCESS) { printf("burst submit %u failed\n", k); ok = 0; }
        }
        uint32_t r2 = 0;
        if (ok) { waitSeq(c, gSeq, 5000, &r2, NULL); if (!r2) { printf("burst: last fence not reached\n"); ok = 0; } }
        struct rn_uc_diag d; size_t dSize = sizeof(d);
        if (IOConnectCallStructMethod(c, RN_UC_DIAG, NULL, 0, &d, &dSize) == KERN_SUCCESS)
            printf("after the burst: rptr word %u, wptr word %u, driver wptr %u, full waits %u, result 0x%08x\n", d.ring_rptr_word, d.ring_wptr_word, d.driver_wptr, d.ring_full_waits, data.cpu[0]);
    }
    if (ok && argc > 1 && !strcmp(argv[1], "seq")) {           // sequence numbers of one context
        uint32_t first = gSeq, r = 0, sig = 0, prev = gSeq;
        for (int k = 0; k < 5 && ok; k++) {
            data.cpu[0] = 0xDEAD0000u;
            if (submitIb(c, code.va, n) != KERN_SUCCESS) { printf("submit failed\n"); ok = 0; break; }
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
        kr = submitIb(c, 0x2000000000ull + 0x1000, n);
        uint32_t after = gSeq;
        printf("a refused submit does not use a number: 0x%x, last number %u (expect %u)\n", kr, after, prev); if (kr == KERN_SUCCESS || after != prev) ok = 0;
        if (submitIb(c, code.va, n) == KERN_SUCCESS && gSeq == prev + 1) { waitSeq(c, gSeq, 2000, &r, &sig); printf("the next one is %u, reached %u\n", gSeq, r); if (!r) ok = 0; } else ok = 0;
        io_connect_t c2;                                       // a second connection counts on its own
        if (IOServiceOpen(svc, mach_task_self(), 0, &c2) == KERN_SUCCESS) {
            uint32_t keep = gSeq;
            kr = submitIb(c2, code.va, n);                     // refused: not its buffer
            kr = waitSeq(c2, 1, 0, &r, &sig);
            printf("second connection, sequence 1 before any submit of its own: 0x%x (expect an error)\n", kr); if (kr == KERN_SUCCESS) ok = 0;
            gSeq = keep;
            IOServiceClose(c2);
        }
    }
    if (ok && argc > 1 && !strcmp(argv[1], "bo")) {            // buffer lifetime: handles, ownership, release on close
        Bo extra;
        struct rn_uc_bo_free fr = {extra.handle = 0, 0};
        if (createBo(c, 4096, &extra)) ok = 0;
        else {
            fr.handle = extra.handle;
            kr = IOConnectCallStructMethod(c, RN_UC_BO_FREE, &fr, sizeof(fr), NULL, NULL);
            printf("free handle %u: 0x%x (expect 0)\n", fr.handle, kr); if (kr != KERN_SUCCESS) ok = 0;
            kr = IOConnectCallStructMethod(c, RN_UC_BO_FREE, &fr, sizeof(fr), NULL, NULL);
            printf("free it again: 0x%x (expect an error)\n", kr); if (kr == KERN_SUCCESS) ok = 0;
            kr = submitIb(c, extra.va, n);
            printf("submit from the freed buffer: 0x%x (expect an error)\n", kr); if (kr == KERN_SUCCESS) ok = 0;
            kr = submitIb(c, 0x2000000000ull + 0x1000, n);
            printf("submit from an address of nobody's: 0x%x (expect an error)\n", kr); if (kr == KERN_SUCCESS) ok = 0;
            kr = submitIb(c, code.va + 4096 - 8, n);
            printf("submit running past the end of the buffer: 0x%x (expect an error)\n", kr); if (kr == KERN_SUCCESS) ok = 0;
            Bo again;
            if (createBo(c, 4096, &again)) ok = 0;
            else printf("a new buffer after the free: handle %u, address 0x%llx (the address is never reused)\n", again.handle, again.va);
        }
        io_connect_t c2;                                       // a second connection must not reach the first one's buffers
        if (IOServiceOpen(svc, mach_task_self(), 0, &c2) == KERN_SUCCESS) {
            kr = submitIb(c2, code.va, n);
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
            submitIb(c, code.va, n);
            uint32_t r2 = 0; waitSeq(c, gSeq, 2000, &r2, NULL);
            printf("the queue still works afterwards: %s\n", r2 ? "yes" : "no"); if (!r2) ok = 0;
        }
    }
    printf("%s\n", ok ? "PASS" : "FAIL");
    IOServiceClose(c);
    return ok ? 0 : 2;
}
