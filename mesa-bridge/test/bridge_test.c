/*
 * What tools/rn_test.c does against the user client, done through the ac_drm_* functions only, in the way RADV's winsys calls them: device, queries, buffers, address ranges,
 * a context, a submit with chunks, syncobjs, fence queries. If this passes the bridge does what Mesa's ac_linux_drm.c does for the winsys.
 *
 *     make -C mesa-bridge test     # builds bridge_test for x86_64; run it on the machine with the card: ./bridge_test
 */
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ac_apple_iokit.h"

#define GC_BASE 0x1260u
#define SHREG(reg) (GC_BASE + (reg) - 0x2c00u)
#define PACKET3(op, n) ((3u << 30) | (((n) & 0x3FFFu) << 16) | (((op) & 0xFFu) << 8))

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAILED: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The shader (GFX11, LLVM MC encodings) stores 0x600DC0DE to storeVa; the IB dispatches it. IB at cpu[0], shader at cpu[0x100/4]. */
static uint32_t build_dispatch(volatile uint32_t *cpu, uint64_t code_va, uint64_t store_va)
{
   const uint32_t shader[11] = {0xBE8000FFu, (uint32_t)store_va, 0xBE8100FFu, (uint32_t)(store_va >> 32), 0x7E020280u, 0x7E0402FFu, 0x600DC0DEu,
                                0xDC6A0000u, 0x00000201u, 0xBC7C0000u, 0xBFB00000u};
   for (int i = 0; i < 64; i++)
      cpu[0x100 / 4 + i] = i < 11 ? shader[i] : 0xBF9F0000u;
   uint32_t ib[48], n = 0;
   const uint64_t sva = code_va + 0x100;
   ib[n++] = PACKET3(0x76, 2); ib[n++] = SHREG(0x1bac); ib[n++] = (uint32_t)(sva >> 8); ib[n++] = (uint32_t)((sva >> 40) & 0xff);
   ib[n++] = PACKET3(0x76, 2); ib[n++] = SHREG(0x1bb2); ib[n++] = 0x002C0000u; ib[n++] = 0x80u;
   ib[n++] = PACKET3(0x76, 1); ib[n++] = SHREG(0x1bc8); ib[n++] = 0;
   ib[n++] = PACKET3(0x76, 3); ib[n++] = SHREG(0x1ba4); ib[n++] = 0; ib[n++] = 0; ib[n++] = 0;
   ib[n++] = PACKET3(0x76, 3); ib[n++] = SHREG(0x1ba7); ib[n++] = 1; ib[n++] = 1; ib[n++] = 1;
   ib[n++] = PACKET3(0x76, 6); ib[n++] = SHREG(0x1bb5);
   ib[n++] = 0; ib[n++] = 0xFFFFFFFFu; ib[n++] = 0xFFFFFFFFu; ib[n++] = 0; ib[n++] = 0xFFFFFFFFu; ib[n++] = 0xFFFFFFFFu;
   ib[n++] = PACKET3(0x15, 3); ib[n++] = 1; ib[n++] = 1; ib[n++] = 1; ib[n++] = 0x00000001u | 0x00000004u | 0x00000040u | 0x00008000u;
   for (uint32_t i = 0; i < n; i++)
      cpu[i] = ib[i];
   return n;
}

static int make_bo(ac_drm_device *dev, uint64_t size, ac_drm_bo *bo, uint32_t *handle, volatile uint32_t **cpu)
{
   struct amdgpu_bo_alloc_request req = {.alloc_size = size, .phys_alignment = 4096, .preferred_heap = AMDGPU_GEM_DOMAIN_GTT, .flags = 0};
   int r = ac_drm_bo_alloc(dev, &req, bo);
   if (r)
      return r;
   r = ac_drm_bo_export(dev, *bo, amdgpu_bo_handle_type_kms, handle);
   if (r)
      return r;
   void *p;
   r = ac_drm_bo_cpu_map(dev, *bo, &p);
   *cpu = p;
   return r;
}

/* Submits the IB at ib_va with the chunks RADV builds (IB, syncobj out), returns the sequence number. */
static int submit(ac_drm_device *dev, uint32_t ctx, uint64_t ib_va, uint32_t dwords, uint32_t out_syncobj, uint64_t out_point, uint64_t *seq)
{
   struct drm_amdgpu_cs_chunk_ib ib = {.va_start = ib_va, .ib_bytes = dwords * 4, .ip_type = AMDGPU_HW_IP_COMPUTE, .ip_instance = 0, .ring = 0};
   struct drm_amdgpu_cs_chunk_sem sem = {.handle = out_syncobj};
   struct drm_amdgpu_cs_chunk_syncobj tl = {.handle = out_syncobj, .flags = 0, .point = out_point};
   struct drm_amdgpu_cs_chunk chunks[2];
   int n = 0;
   chunks[n++] = (struct drm_amdgpu_cs_chunk){.chunk_id = AMDGPU_CHUNK_ID_IB, .length_dw = sizeof(ib) / 4, .chunk_data = (uintptr_t)&ib};
   if (out_syncobj && out_point == 0)
      chunks[n++] = (struct drm_amdgpu_cs_chunk){.chunk_id = AMDGPU_CHUNK_ID_SYNCOBJ_OUT, .length_dw = sizeof(sem) / 4, .chunk_data = (uintptr_t)&sem};
   else if (out_syncobj)
      chunks[n++] = (struct drm_amdgpu_cs_chunk){.chunk_id = AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL, .length_dw = sizeof(tl) / 4, .chunk_data = (uintptr_t)&tl};
   return ac_drm_cs_submit_raw2(dev, ctx, 0, n, chunks, seq);
}

int main(void)
{
   ac_drm_device *dev;
   uint32_t major, minor;
   int r = ac_drm_device_initialize(-1, false, &major, &minor, &dev);
   if (r) {
      printf("device: %d\n", r);
      return 1;
   }
   printf("device %s, interface %u.%u\n", ac_drm_get_marketing_name(dev), major, minor);

   /* queries, as RADV's ac_query_gpu_info asks them */
   struct amdgpu_gpu_info gi;
   r = ac_drm_query_gpu_info(dev, &gi);
   CHECK(r == 0, "query_gpu_info %d", r);
   printf("gpu info: asic 0x%x family %u, %u SE x %u SA, %u CU, GB_ADDR_CONFIG 0x%x, clock %llu kHz\n", gi.asic_id, gi.family_id, gi.num_shader_engines,
          gi.num_shader_arrays_per_engine, gi.cu_active_number, gi.gb_addr_cfg, (unsigned long long)gi.max_engine_clk);
   CHECK((gi.asic_id == 0x747e || gi.asic_id == 0x1586) && gi.family_id == 145 && gi.gb_addr_cfg == 0x444, "gpu info values");
   struct amdgpu_heap_info heap;
   r = ac_drm_query_heap_info(dev, AMDGPU_GEM_DOMAIN_VRAM, 0, &heap);
   CHECK(r == 0 && heap.heap_size > 0, "vram heap %d", r);
   printf("vram heap %llu MB, usage %llu MB; ", (unsigned long long)(heap.heap_size >> 20), (unsigned long long)(heap.heap_usage >> 20));
   r = ac_drm_query_heap_info(dev, AMDGPU_GEM_DOMAIN_GTT, 0, &heap);
   CHECK(r == 0 && heap.heap_size > 0, "gtt heap %d", r);
   printf("gtt heap %llu MB\n", (unsigned long long)(heap.heap_size >> 20));
   uint32_t count = 99;
   r = ac_drm_query_hw_ip_count(dev, AMDGPU_HW_IP_COMPUTE, &count);
   CHECK(r == 0 && count == 1, "hw ip count %d %u", r, count);
   struct drm_amdgpu_info_hw_ip ip;
   r = ac_drm_query_hw_ip_info(dev, AMDGPU_HW_IP_COMPUTE, 0, &ip);
   CHECK(r == 0 && ip.hw_ip_version_major == 11 && ip.available_rings == 1, "hw ip info %d", r);
   uint32_t ver, feat;
   r = ac_drm_query_firmware_version(dev, AMDGPU_INFO_FW_GFX_ME, 0, 0, &ver, &feat);
   CHECK(r == 0 && ver == 0x9a6 && feat == 29, "ME firmware %d 0x%x %u", r, ver, feat);
   uint32_t hi = 0;
   r = ac_drm_query_sw_info(dev, amdgpu_sw_info_address32_hi, &hi);
   CHECK(r == 0 && hi == 0xffff8000u, "address32_hi %d 0x%x", r, hi);
   printf("firmware ME %u (feature %u), address32_hi 0x%x\n", ver, feat, hi);
   CHECK(ac_drm_query_sensor_info(dev, 1, 4, &hi) != 0, "sensors are not answered yet");

   /* buffers and addresses: the code in the lower half, the data in the 32-bit range of the upper half */
   ac_drm_bo code, data;
   uint32_t code_h, data_h;
   volatile uint32_t *code_cpu, *data_cpu;
   r = make_bo(dev, 4096, &code, &code_h, &code_cpu);
   CHECK(r == 0, "code bo %d", r);
   r = make_bo(dev, 4096, &data, &data_h, &data_cpu);
   CHECK(r == 0, "data bo %d", r);
   if (failures)
      return 1;
   uint64_t code_va, data_va;
   amdgpu_va_handle code_vah, data_vah;
   r = ac_drm_va_range_alloc(dev, amdgpu_gpu_va_range_general, 4096, 4096, 0, &code_va, &code_vah, AMDGPU_VA_RANGE_HIGH);
   CHECK(r == 0 && code_va < (1ull << 47), "code va 0x%llx (%d): HIGH without 32_BIT must land in the lower half", (unsigned long long)code_va, r);
   r = ac_drm_va_range_alloc(dev, amdgpu_gpu_va_range_general, 4096, 4096, 0, &data_va, &data_vah, AMDGPU_VA_RANGE_HIGH | AMDGPU_VA_RANGE_32_BIT);
   CHECK(r == 0 && (data_va >> 32) == 0xffff8000ull, "data va 0x%llx (%d): a 32-bit buffer has the high dword address32_hi", (unsigned long long)data_va, r);
   printf("code at 0x%llx, data at 0x%llx\n", (unsigned long long)code_va, (unsigned long long)data_va);
   r = ac_drm_bo_va_op_raw(dev, code_h, 0, 4096, code_va, AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE, AMDGPU_VA_OP_MAP);
   CHECK(r == 0, "map code %d", r);
   r = ac_drm_bo_va_op_raw(dev, data_h, 0, 4096, data_va, AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE, AMDGPU_VA_OP_MAP);
   CHECK(r == 0, "map data %d", r);
   /* more than the old 32 buffers, and a big one: 64 MB mapped at a 32-bit address, written through at its far end by a shader, read by the CPU */
   {
      ac_drm_bo many[100];
      uint32_t h;
      volatile uint32_t *c;
      int made = 0;
      for (; made < 100; made++)
         if (make_bo(dev, 4096, &many[made], &h, &c))
            break;
      CHECK(made == 100, "100 buffers: %d made", made);
      for (int i = 0; i < made; i++)
         ac_drm_bo_free(dev, many[i]);
      ac_drm_bo bigbo;
      uint32_t big_h;
      volatile uint32_t *big_cpu;
      r = make_bo(dev, 64ull << 20, &bigbo, &big_h, &big_cpu);
      CHECK(r == 0, "a 64 MB buffer: %d", r);
      if (r == 0) {
         uint64_t big_va;
         amdgpu_va_handle big_vah;
         r = ac_drm_va_range_alloc(dev, amdgpu_gpu_va_range_general, 64ull << 20, 2ull << 20, 0, &big_va, &big_vah, AMDGPU_VA_RANGE_32_BIT);
         CHECK(r == 0, "va for 64 MB: %d", r);
         r = ac_drm_bo_va_op_raw(dev, big_h, 0, 64ull << 20, big_va, AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE, AMDGPU_VA_OP_MAP);
         CHECK(r == 0, "map 64 MB: %d", r);
         const uint64_t far_off = (64ull << 20) - 4096;
         const char *sub_env = getenv("RN_TEST_SUBMIT");
         int do_submit = sub_env && atoi(sub_env) == 1;
         if (do_submit) {
            uint32_t cz;
            r = ac_drm_cs_ctx_create2(dev, 0, &cz);
            uint32_t nn = build_dispatch(code_cpu, code_va, big_va + far_off);
            big_cpu[far_off / 4] = 0xDEAD0000u;
            uint64_t bseq = 0;
            r = submit(dev, cz, code_va, nn, 0, 0, &bseq);
            uint32_t ex = 0;
            if (r == 0)
               r = ac_drm_cs_query_fence_status(dev, cz, AMDGPU_HW_IP_COMPUTE, 0, 0, bseq, 2000000000ull, 0, &ex);
            CHECK(r == 0 && ex && big_cpu[far_off / 4] == 0x600DC0DEu, "dispatch storing into the last page of a 64 MB buffer: %d expired %u value 0x%08x", r, ex, big_cpu[far_off / 4]);
            printf("64 MB buffer at 0x%llx: last page written by the shader, value 0x%08x\n", (unsigned long long)big_va, big_cpu[far_off / 4]);
         }
         ac_drm_bo_va_op_raw(dev, big_h, 0, 64ull << 20, big_va, 0, AMDGPU_VA_OP_UNMAP);
         ac_drm_va_range_free(big_vah);
         ac_drm_bo_free(dev, bigbo);
      }
      struct amdgpu_bo_alloc_request huge = {.alloc_size = 3ull << 30, .phys_alignment = 4096, .preferred_heap = AMDGPU_GEM_DOMAIN_GTT};
      ac_drm_bo nobo;
      CHECK(ac_drm_bo_alloc(dev, &huge, &nobo) != 0, "a 3 GB buffer is refused");
   }

   const char *sub_env = getenv("RN_TEST_SUBMIT");
   int do_submit = sub_env && atoi(sub_env) == 1;
   if (!do_submit) {
      ac_drm_bo_va_op_raw(dev, code_h, 0, 4096, code_va, 0, AMDGPU_VA_OP_UNMAP);
      ac_drm_bo_va_op_raw(dev, data_h, 0, 4096, data_va, 0, AMDGPU_VA_OP_UNMAP);
      ac_drm_va_range_free(code_vah);
      ac_drm_va_range_free(data_vah);
      ac_drm_bo_free(dev, code);
      ac_drm_bo_free(dev, data);
      ac_drm_device_deinitialize(dev);
      printf("SUCCESS: Device queries, BO allocations, and GPUVM 48-bit address mapping PASSED!\n");
      printf("(Set RN_TEST_SUBMIT=1 to enable command queue dispatches once queues are mapped)\n");
      return failures ? 2 : 0;
   }

   /* a context, a syncobj, a submit with chunks, the fence */
   uint32_t ctx, so;
   r = ac_drm_cs_ctx_create2(dev, 0, &ctx);
   CHECK(r == 0, "ctx %d", r);
   r = ac_drm_cs_create_syncobj2(dev, 0, &so);
   CHECK(r == 0, "syncobj %d", r);
   uint32_t n = build_dispatch(code_cpu, code_va, data_va);
   data_cpu[0] = 0xDEAD0000u;
   uint64_t seq = 0;
   r = submit(dev, ctx, code_va, n, so, 0, &seq);
   CHECK(r == 0, "submit %d", r);
   struct util_sync_provider *sp = ac_drm_device_get_sync_provider(dev);
   uint32_t first = 99;
   r = sp->wait(sp, &so, 1, (int64_t)now_ns() + 3000000000ll, RN_UC_SYNCOBJ_WAIT_ALL | RN_UC_SYNCOBJ_WAIT_FOR_SUBMIT, &first);
   CHECK(r == 0, "wait for the syncobj %d", r);
   uint32_t expired = 0;
   r = ac_drm_cs_query_fence_status(dev, ctx, AMDGPU_HW_IP_COMPUTE, 0, 0, seq, 1000000000ull, 0, &expired);
   CHECK(r == 0 && expired, "fence %llu %d %u", (unsigned long long)seq, r, expired);
   CHECK(data_cpu[0] == 0x600DC0DEu, "result 0x%08x (shader at 0x%llx stored through 0x%llx)", data_cpu[0], (unsigned long long)(code_va + 0x100), (unsigned long long)data_va);
   printf("dispatch: sequence %llu, result 0x%08x\n", (unsigned long long)seq, data_cpu[0]);

   /* timeline syncobj through the timeline chunk */
   uint32_t tl;
   r = ac_drm_cs_create_syncobj2(dev, 0, &tl);
   data_cpu[0] = 0xDEAD0000u;
   r = submit(dev, ctx, code_va, n, tl, 3, &seq);
   CHECK(r == 0, "submit with a timeline point %d", r);
   uint64_t point = 3;
   r = ac_drm_cs_syncobj_timeline_wait(dev, &tl, &point, 1, (int64_t)now_ns() + 3000000000ll, RN_UC_SYNCOBJ_WAIT_FOR_SUBMIT, &first);
   CHECK(r == 0, "timeline wait %d", r);
   uint64_t value = 0;
   r = ac_drm_cs_syncobj_query2(dev, &tl, &value, 1, 0);
   CHECK(r == 0 && value == 3, "timeline value %llu (%d)", (unsigned long long)value, r);
   point = 4;
   r = ac_drm_cs_syncobj_timeline_wait(dev, &tl, &point, 1, (int64_t)now_ns() + 50000000ll, RN_UC_SYNCOBJ_WAIT_FOR_SUBMIT, &first);
   CHECK(r == -ETIME, "point 4 nobody signals: %d (expect -ETIME)", r);
   printf("timeline: value %llu, point 4 times out\n", (unsigned long long)value);

   /* a command buffer in the 32-bit range (upper half) is refused */
   amdgpu_va_handle ib_vah;
   uint64_t ib_va;
   r = ac_drm_va_range_alloc(dev, amdgpu_gpu_va_range_general, 4096, 4096, 0, &ib_va, &ib_vah, AMDGPU_VA_RANGE_32_BIT);
   r = ac_drm_bo_va_op_raw(dev, code_h, 0, 4096, ib_va, AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_EXECUTABLE, AMDGPU_VA_OP_MAP);
   CHECK(r == 0, "map code in the 32-bit range %d", r);
   r = submit(dev, ctx, ib_va, n, 0, 0, &seq);
   CHECK(r != 0, "a command buffer at 0x%llx must be refused", (unsigned long long)ib_va);
   printf("a command buffer at 0x%llx: %d (refused)\n", (unsigned long long)ib_va, r);

   /* teardown: unmap, free the ranges and the buffers, and a second round of the same dispatch in the same places */
   CHECK(ac_drm_bo_va_op_raw(dev, code_h, 0, 4096, ib_va, 0, AMDGPU_VA_OP_UNMAP) == 0, "unmap");
   ac_drm_va_range_free(ib_vah);
   CHECK(ac_drm_bo_va_op_raw(dev, data_h, 0, 4096, data_va, 0, AMDGPU_VA_OP_UNMAP) == 0, "unmap data");
   ac_drm_va_range_free(data_vah);
   uint64_t again;
   amdgpu_va_handle again_h;
   r = ac_drm_va_range_alloc(dev, amdgpu_gpu_va_range_general, 4096, 4096, 0, &again, &again_h, AMDGPU_VA_RANGE_32_BIT);
   CHECK(r == 0 && again == data_va, "a freed range is handed out again: 0x%llx (was 0x%llx)", (unsigned long long)again, (unsigned long long)data_va);
   ac_drm_va_range_free(again_h);
   ac_drm_cs_destroy_syncobj(dev, so);
   ac_drm_cs_destroy_syncobj(dev, tl);
   ac_drm_bo_free(dev, data);
   ac_drm_bo_free(dev, code);
   ac_drm_va_range_free(code_vah);
   ac_drm_cs_ctx_free(dev, ctx);
   ac_drm_device_deinitialize(dev);
   printf("%s\n", failures ? "FAIL" : "PASS");
   return failures ? 2 : 0;
}
