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

#define GC_BASE 0x1260u
#define SHREG(reg) (GC_BASE + (reg) - 0x2c00u)
#define PACKET3(op, n) ((3u << 30) | (((n) & 0x3FFFu) << 16) | (((op) & 0xFFu) << 8))

enum { RN_CREATE = 0, RN_SUBMIT = 1, RN_WAIT = 2, RN_INFO = 3, RN_DIAG = 4, RN_DIAG2 = 5, RN_RING = 6 };

typedef struct { uint32_t handle; uint64_t va; uint64_t size; volatile uint32_t *cpu; } Bo;

static int createBo(io_connect_t c, uint64_t size, Bo *bo) {
    uint64_t in[1] = {size}, out[3];
    uint32_t outCnt = 3;
    kern_return_t kr = IOConnectCallScalarMethod(c, RN_CREATE, in, 1, out, &outCnt);
    if (kr != KERN_SUCCESS) { printf("create buffer: 0x%x\n", kr); return -1; }
    bo->handle = (uint32_t)out[0]; bo->va = out[1]; bo->size = out[2];
    mach_vm_address_t addr = 0;
    mach_vm_size_t len = 0;
    kr = IOConnectMapMemory64(c, bo->handle, mach_task_self(), &addr, &len, kIOMapAnywhere);
    if (kr != KERN_SUCCESS) { printf("map buffer %u: 0x%x\n", bo->handle, kr); return -1; }
    bo->cpu = (volatile uint32_t *)(uintptr_t)addr;
    return 0;
}

int main(int argc, char **argv) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedNaviProbe"));
    if (!svc) { printf("no RedNaviProbe service\n"); return 1; }
    io_connect_t c;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c);
    if (kr != KERN_SUCCESS) { printf("IOServiceOpen: 0x%x (administrator or boot-arg rednavi_uc_anyone=1)\n", kr); return 1; }

    uint64_t info[3];
    uint32_t infoCnt = 3;
    IOConnectCallScalarMethod(c, RN_INFO, NULL, 0, info, &infoCnt);
    printf("GART base 0x%llx, %llu buffers in use, doorbell %s\n", info[0], info[1], info[2] ? "mapped" : "not mapped");

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
    uint64_t sub[4] = {code.va, n, data.va + 4, 0xFEEDFACEu};
    kr = IOConnectCallScalarMethod(c, RN_SUBMIT, sub, 4, NULL, NULL);
    printf("submit: 0x%x\n", kr);
    uint64_t w[4] = {data.handle, 4, 0xFEEDFACEu, 2000}, reached = 0;
    uint32_t cnt = 1;
    kr = IOConnectCallScalarMethod(c, RN_WAIT, w, 4, &reached, &cnt);
    printf("wait fence: 0x%x, reached %llu\n", kr, reached);
    printf("result 0x%08x (expected 0x600DC0DE), fence 0x%08x\n", data.cpu[0], data.cpu[1]);
    int ok = reached && data.cpu[0] == 0x600DC0DEu;
    if (!ok) {
        uint64_t dg[11];
        uint32_t dgCnt = 11;
        if (IOConnectCallScalarMethod(c, RN_DIAG, NULL, 0, dg, &dgCnt) == KERN_SUCCESS)
            printf("queue: rptr 0x%llx wptr 0x%llx active %llu, CP_STAT 0x%llx, VM fault status 0x%llx address 0x%llx%08llx, ring wptr 0x%llx\n"
                   "       SDMA0_VERSION 0x%llx (0x600 = GC powered), GRBM_STATUS 0x%llx, RLC_GPM_STAT 0x%llx\n",
                   dg[0], dg[1], dg[2], dg[3], dg[4], dg[6], dg[5], dg[7], dg[8], dg[9], dg[10]);
    }
    if (!ok) {
        uint64_t d2[8];
        uint32_t d2Cnt = 8;
        if (IOConnectCallScalarMethod(c, RN_DIAG2, NULL, 0, d2, &d2Cnt) == KERN_SUCCESS)
            printf("       HQD doorbell control 0x%llx, CP_PQ_STATUS 0x%llx, MEC doorbell range 0x%llx-0x%llx, DOORBELL_APER_EN 0x%llx,\n"
                   "       PQ base 0x%llx, PQ control 0x%llx, MQD base 0x%llx\n", d2[0], d2[1], d2[2], d2[3], d2[4], d2[5], d2[6], d2[7]);
    }
    if (ok && argc > 1 && !strcmp(argv[1], "ring")) {          // rptr write-back word against the driver's wptr over many submits (the ring holds 2048 dwords)
        for (uint32_t k = 1; k <= 300; k++) {
            const uint32_t fv = 0xFEED0000u + k;
            uint64_t s2[4] = {code.va, n, data.va + 4, fv};
            if (IOConnectCallScalarMethod(c, RN_SUBMIT, s2, 4, NULL, NULL) != KERN_SUCCESS) { printf("submit %u failed\n", k); ok = 0; break; }
            uint64_t w2[4] = {data.handle, 4, fv, 2000}, r2 = 0; uint32_t c2 = 1;
            IOConnectCallScalarMethod(c, RN_WAIT, w2, 4, &r2, &c2);
            if (!r2) { printf("submit %u: fence not reached\n", k); ok = 0; break; }
            if (k == 1 || k % 50 == 0) {
                uint64_t rs[4]; uint32_t rc3 = 4;
                if (IOConnectCallScalarMethod(c, RN_RING, NULL, 0, rs, &rc3) == KERN_SUCCESS)
                    printf("after %u submits: rptr word %llu, wptr word %llu, driver wptr %llu, full waits %llu\n", k, rs[0], rs[1], rs[2], rs[3]);
            }
        }
    }
    if (ok && argc > 1 && !strcmp(argv[1], "ring")) {          // a burst without waiting: 1500 submits, about 70 000 dwords through a 2048-dword ring
        uint32_t last = 0;
        for (uint32_t k = 1; k <= 1500 && ok; k++) {
            last = 0xBEEF0000u + k;
            uint64_t s2[4] = {code.va, n, data.va + 4, last};
            if (IOConnectCallScalarMethod(c, RN_SUBMIT, s2, 4, NULL, NULL) != KERN_SUCCESS) { printf("burst submit %u failed\n", k); ok = 0; }
        }
        uint64_t w2[4] = {data.handle, 4, last, 5000}, r2 = 0; uint32_t c2 = 1;
        if (ok) { IOConnectCallScalarMethod(c, RN_WAIT, w2, 4, &r2, &c2); if (!r2) { printf("burst: last fence not reached\n"); ok = 0; } }
        uint64_t rs[4]; uint32_t rc3 = 4;
        if (IOConnectCallScalarMethod(c, RN_RING, NULL, 0, rs, &rc3) == KERN_SUCCESS)
            printf("after the burst: rptr word %llu, wptr word %llu, driver wptr %llu, full waits %llu, result 0x%08x\n", rs[0], rs[1], rs[2], rs[3], data.cpu[0]);
    }
    printf("%s\n", ok ? "PASS" : "FAIL");
    IOServiceClose(c);
    return ok ? 0 : 2;
}
