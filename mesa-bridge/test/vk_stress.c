/*
 * Compute stress tests for RADV on the RedNavi kext (plain Vulkan 1.1, compute queue only):
 *
 *   burst [N]    N separate vkQueueSubmit calls (default 3000) of tiny dispatches into one buffer: IB turnover and ring-buffer saturation, a fence every 256 submits.
 *   saturate [N] N submits (default 200) that keep the GPU busy for a while each, so the ring buffer stays full and the kext must hold submits back.
 *   barrier [K]  K dependent dispatches in one command buffer (a writer, then K increments, a compute-to-compute barrier between each), then the same chain
 *                over several submits tied by semaphores (syncobj in/out in the kext).
 *   multi [R]    R rounds (default 20) of several storage buffers alive at once, one dispatch each, verified, then half of them freed and replaced.
 *   vram [MB]    buffers in device-local VRAM (written and checked by the GPU, read back by vkCmdCopyBuffer), CPU-visible VRAM, and as many 256 MB device-local buffers as fit (at most MB).
 *   all          all of the above.
 *
 * Exit status 0 when every check passed. Shaders: vk_stress.comp.write (v[base+i] = (base+i)*3 + add) and vk_stress.comp.inc (v[base+i] += add).
 *   clang -O1 -arch x86_64 -mmacosx-version-min=12 -I/opt/local/include test/vk_stress.c -L/opt/local/lib -lvulkan -o vk_stress
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "vk_stress_spv.h"

#define CHECK(x)                                                                                    \
   do {                                                                                             \
      VkResult r_ = (x);                                                                            \
      if (r_ != VK_SUCCESS) {                                                                       \
         fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, (int)r_);                      \
         exit(2);                                                                                   \
      }                                                                                             \
   } while (0)

static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t family;
static VkPhysicalDeviceMemoryProperties memProps;
static VkDescriptorSetLayout setLayout;
static VkPipelineLayout pipeLayout;
static VkPipeline pipeWrite, pipeInc;
static VkCommandPool cmdPool;
static VkDescriptorPool descPool;
static int failures;

typedef struct { uint32_t base, add, count; } PC;
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint32_t *cpu; uint32_t words; VkDescriptorSet set; } Buf;

static double now(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void fail(const char *what)
{
   printf("FAIL: %s\n", what);
   failures++;
}

static VkPipeline makePipeline(const unsigned char *spv, size_t len)
{
   VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = len, .pCode = (const uint32_t *)spv};
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &sm));
   VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"},
                                      .layout = pipeLayout};
   VkPipeline p;
   CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &p));
   return p;
}

/* A buffer in a memory type that has the properties `want` and none of `avoid`; returns the VkResult of the allocation (VK_SUCCESS, or an error when that memory is full), the buffer
 * is not usable otherwise. A buffer in host-visible memory is mapped (b->cpu), one in device-local memory is not. */
static VkResult bufCreateEx(Buf *b, uint32_t words, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
   memset(b, 0, sizeof(*b));
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)words * 4, .usage = usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   CHECK(vkCreateBuffer(dev, &bci, NULL, &b->buf));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, b->buf, &mr);
   int mt = -1;
   for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
      if ((mr.memoryTypeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want && !(memProps.memoryTypes[i].propertyFlags & avoid)) { mt = (int)i; break; }
   if (mt < 0) { fprintf(stderr, "no memory type with 0x%x and without 0x%x\n", want, avoid); exit(2); }
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt};
   VkResult ar = vkAllocateMemory(dev, &mai, NULL, &b->mem);
   if (ar != VK_SUCCESS) {
      vkDestroyBuffer(dev, b->buf, NULL);
      memset(b, 0, sizeof(*b));
      return ar;
   }
   CHECK(vkBindBufferMemory(dev, b->buf, b->mem, 0));
   if (memProps.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      CHECK(vkMapMemory(dev, b->mem, 0, VK_WHOLE_SIZE, 0, (void **)&b->cpu));
   b->words = words;
   VkDescriptorSetAllocateInfo dsa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = descPool, .descriptorSetCount = 1, .pSetLayouts = &setLayout};
   CHECK(vkAllocateDescriptorSets(dev, &dsa, &b->set));
   VkDescriptorBufferInfo dbi = {.buffer = b->buf, .offset = 0, .range = VK_WHOLE_SIZE};
   VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = b->set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
   vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
   return VK_SUCCESS;
}

static void bufCreate(Buf *b, uint32_t words)
{
   CHECK(bufCreateEx(b, words, 0, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0));
}

static void bufDestroy(Buf *b)
{
   vkFreeDescriptorSets(dev, descPool, 1, &b->set);
   if (b->cpu) vkUnmapMemory(dev, b->mem);
   vkDestroyBuffer(dev, b->buf, NULL);
   vkFreeMemory(dev, b->mem, NULL);
   memset(b, 0, sizeof(*b));
}

static VkCommandBuffer cbAlloc(void)
{
   VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmdPool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &cba, &cb));
   return cb;
}

static void cbBegin(VkCommandBuffer cb, int oneTime)
{
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = oneTime ? VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT : 0};
   CHECK(vkBeginCommandBuffer(cb, &bi));
}

static void cmdDispatch(VkCommandBuffer cb, VkPipeline p, const Buf *b, PC pc)
{
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &b->set, 0, NULL);
   vkCmdPushConstants(cb, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
   vkCmdDispatch(cb, (pc.count + 63) / 64, 1, 1);
}

static void cmdBarrier(VkCommandBuffer cb, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
   VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = dstAccess};
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dstStage, 0, 1, &mb, 0, NULL, 0, NULL);
}

static VkFence fenceCreate(void)
{
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence f;
   CHECK(vkCreateFence(dev, &fci, NULL, &f));
   return f;
}

static int fenceWait(VkFence f, double seconds)
{
   VkResult r = vkWaitForFences(dev, 1, &f, VK_TRUE, (uint64_t)(seconds * 1e9));
   if (r != VK_SUCCESS) { printf("fence wait: %d\n", (int)r); return -1; }
   return 0;
}

/* --------------------------------------------------------------------------------------------------------------------------------- burst */
static void testBurst(uint32_t n)
{
   printf("== burst: %u submits\n", n);
   const uint32_t per = 64;
   Buf b;
   bufCreate(&b, n * per);
   memset(b.cpu, 0xAB, (size_t)n * per * 4);
   /* one reusable command buffer per slot would hide IB turnover; record a fresh one for every submit and free the old ones in batches */
   VkFence fence = fenceCreate();
   const uint32_t batch = 256;
   VkCommandBuffer cbs[256];
   double t0 = now();
   uint32_t done = 0;
   while (done < n) {
      uint32_t k = n - done < batch ? n - done : batch;
      for (uint32_t i = 0; i < k; i++) {
         cbs[i] = cbAlloc();
         cbBegin(cbs[i], 1);
         cmdDispatch(cbs[i], pipeWrite, &b, (PC){(done + i) * per, done + i, per});
         cmdBarrier(cbs[i], VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
         CHECK(vkEndCommandBuffer(cbs[i]));
      }
      for (uint32_t i = 0; i < k; i++) {
         VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cbs[i]};
         CHECK(vkQueueSubmit(queue, 1, &si, i == k - 1 ? fence : VK_NULL_HANDLE));
      }
      if (fenceWait(fence, 20.0)) { fail("burst fence timeout"); return; }
      CHECK(vkResetFences(dev, 1, &fence));
      vkFreeCommandBuffers(dev, cmdPool, k, cbs);
      done += k;
   }
   double dt = now() - t0;
   uint32_t bad = 0;
   for (uint32_t s = 0; s < n; s++)
      for (uint32_t i = 0; i < per; i++)
         if (b.cpu[s * per + i] != (s * per + i) * 3u + s) bad++;
   printf("%u submits in %.2f s (%.0f/s); %u of %u values wrong\n", n, dt, n / dt, bad, n * per);
   if (bad) fail("burst values");
   vkDestroyFence(dev, fence, NULL);
   bufDestroy(&b);
}

/* --------------------------------------------------------------------------------------------------------------------------------- saturate */
/* Submits that take the GPU a while each, so that the CPU gets far ahead and the ring buffer (2048 dwords) is full most of the time: the kext has to hold the submit back. */
static void testSaturate(uint32_t n)
{
   printf("== saturate: %u submits of 64 dependent dispatches over 2M words\n", n);
   const uint32_t words = 2u << 20, dispatches = 64;
   Buf b;
   bufCreate(&b, words);
   memset(b.cpu, 0xAB, (size_t)words * 4);
   VkCommandBuffer init = cbAlloc();
   cbBegin(init, 1);
   cmdDispatch(init, pipeWrite, &b, (PC){0, 7, words});
   cmdBarrier(init, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
   CHECK(vkEndCommandBuffer(init));
   VkCommandBuffer work = cbAlloc();
   cbBegin(work, 0);
   for (uint32_t i = 0; i < dispatches; i++) {
      cmdDispatch(work, pipeInc, &b, (PC){0, 1, words});
      cmdBarrier(work, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
   }
   cmdBarrier(work, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
   CHECK(vkEndCommandBuffer(work));
   VkFence fence = fenceCreate();
   double t0 = now();
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &init};
   CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   VkSubmitInfo sw = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &work};
   for (uint32_t i = 0; i < n; i++) {
      double ts = now();
      CHECK(vkQueueSubmit(queue, 1, &sw, i == n - 1 ? fence : VK_NULL_HANDLE));
      double d = now() - ts;
      if (d > 0.5) printf("submit %u held back for %.2f s\n", i, d);
   }
   printf("all %u submitted after %.2f s\n", n, now() - t0);
   if (fenceWait(fence, 120.0)) { fail("saturate fence timeout"); return; }
   double dt = now() - t0;
   uint32_t bad = 0;
   const uint32_t want = 7 + n * dispatches;
   for (uint32_t w = 0; w < words; w++)
      if (b.cpu[w] != w * 3u + want) bad++;
   printf("%u submits, %u dispatches in %.2f s; %u of %u values wrong\n", n, n * dispatches, dt, bad, words);
   if (bad) {
      for (uint32_t w = 0; w < words; w++)
         if (b.cpu[w] != w * 3u + want) { printf("first wrong value: word %u is %u, want %u (%u increments done, want %u)\n", w, b.cpu[w], w * 3u + want, b.cpu[w] - w * 3u - 7u, n * dispatches); break; }
      fail("saturate values");
   }
   vkDestroyFence(dev, fence, NULL);
   bufDestroy(&b);
}

/* --------------------------------------------------------------------------------------------------------------------------------- barrier */
static void testBarrier(uint32_t k)
{
   printf("== barrier: chain of %u dependent dispatches\n", k);
   const uint32_t words = 64 * 256;
   Buf b;
   bufCreate(&b, words);
   memset(b.cpu, 0xAB, (size_t)words * 4);
   VkCommandBuffer cb = cbAlloc();
   cbBegin(cb, 1);
   cmdDispatch(cb, pipeWrite, &b, (PC){0, 5, words});
   for (uint32_t i = 0; i < k; i++) {
      cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
      cmdDispatch(cb, pipeInc, &b, (PC){0, 1, words});
   }
   cmdBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
   CHECK(vkEndCommandBuffer(cb));
   VkFence fence = fenceCreate();
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   CHECK(vkQueueSubmit(queue, 1, &si, fence));
   if (fenceWait(fence, 20.0)) { fail("barrier fence timeout"); return; }
   uint32_t bad = 0;
   for (uint32_t i = 0; i < words; i++)
      if (b.cpu[i] != i * 3u + 5u + k) bad++;
   printf("single command buffer: %u of %u values wrong\n", bad, words);
   if (bad) fail("barrier chain in one command buffer");

   /* the same chain over several submits tied by semaphores: write -> sem -> increments in two submits -> sem -> final increment */
   memset(b.cpu, 0xAB, (size_t)words * 4);
   VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore sem[3];
   for (int i = 0; i < 3; i++) CHECK(vkCreateSemaphore(dev, &sci, NULL, &sem[i]));
   VkCommandBuffer c[4];
   for (int i = 0; i < 4; i++) {
      c[i] = cbAlloc();
      cbBegin(c[i], 1);
      if (i == 0) cmdDispatch(c[i], pipeWrite, &b, (PC){0, 5, words});
      else cmdDispatch(c[i], pipeInc, &b, (PC){0, 1, words});
      cmdBarrier(c[i], VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
      CHECK(vkEndCommandBuffer(c[i]));
   }
   const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
   VkSubmitInfo s0 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c[0], .signalSemaphoreCount = 1, .pSignalSemaphores = &sem[0]};
   VkSubmitInfo s1 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem[0], .pWaitDstStageMask = &waitStage, .commandBufferCount = 1, .pCommandBuffers = &c[1],
                      .signalSemaphoreCount = 1, .pSignalSemaphores = &sem[1]};
   VkSubmitInfo s2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem[1], .pWaitDstStageMask = &waitStage, .commandBufferCount = 1, .pCommandBuffers = &c[2],
                      .signalSemaphoreCount = 1, .pSignalSemaphores = &sem[2]};
   VkSubmitInfo s3 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem[2], .pWaitDstStageMask = &waitStage, .commandBufferCount = 1, .pCommandBuffers = &c[3]};
   CHECK(vkResetFences(dev, 1, &fence));
   CHECK(vkQueueSubmit(queue, 1, &s0, VK_NULL_HANDLE));
   CHECK(vkQueueSubmit(queue, 1, &s1, VK_NULL_HANDLE));
   CHECK(vkQueueSubmit(queue, 1, &s2, VK_NULL_HANDLE));
   CHECK(vkQueueSubmit(queue, 1, &s3, fence));
   if (fenceWait(fence, 20.0)) { fail("semaphore chain fence timeout"); return; }
   bad = 0;
   for (uint32_t i = 0; i < words; i++)
      if (b.cpu[i] != i * 3u + 5u + 3u) bad++;
   printf("semaphore chain over 4 submits: %u of %u values wrong\n", bad, words);
   if (bad) fail("semaphore chain");
   for (int i = 0; i < 3; i++) vkDestroySemaphore(dev, sem[i], NULL);
   vkDestroyFence(dev, fence, NULL);
   bufDestroy(&b);
}

/* --------------------------------------------------------------------------------------------------------------------------------- multi */
/* --------------------------------------------------------------------------------------------------------------------------------- vram */
static void dispatchAll(VkCommandBuffer cb, VkPipeline p, const Buf *b, uint32_t add, int write)
{
   const uint32_t chunk = 65535u * 64u;                                   /* words per dispatch: the workgroup count limit */
   for (uint32_t base = 0; base < b->words; base += chunk) {
      cmdDispatch(cb, p, b, (PC){base, add, b->words - base < chunk ? b->words - base : chunk});
      (void)write;
   }
}

/* Copies [from, from + words) of a device-local buffer into a host-visible one with vkCmdCopyBuffer and waits. */
static void copyOut(const Buf *src, const Buf *dst, uint32_t from, uint32_t words)
{
   VkCommandBuffer cb = cbAlloc();
   cbBegin(cb, 1);
   cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
   VkBufferCopy rg = {.srcOffset = (VkDeviceSize)from * 4, .dstOffset = 0, .size = (VkDeviceSize)words * 4};
   vkCmdCopyBuffer(cb, src->buf, dst->buf, 1, &rg);
   VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
   CHECK(vkEndCommandBuffer(cb));
   VkFence f = fenceCreate();
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   CHECK(vkQueueSubmit(queue, 1, &si, f));
   if (fenceWait(f, 30.0)) { fail("copy fence timeout"); exit(3); }
   vkDestroyFence(dev, f, NULL);
   vkFreeCommandBuffers(dev, cmdPool, 1, &cb);
}

/* One device-local buffer of `mb` MB: written by a dispatch, incremented by a second, read back through a staging buffer in 16 MB pieces and checked. */
static int vramOne(uint32_t mb, uint32_t tag)
{
   const uint32_t words = mb << 18;
   Buf d, h;
   double t0 = now();
   VkResult r = bufCreateEx(&d, words, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   if (r != VK_SUCCESS) { printf("%u MB device-local: allocation failed (%d)\n", mb, (int)r); return -1; }
   const uint32_t pieceWords = words < (4u << 20) ? words : (4u << 20);
   CHECK(bufCreateEx(&h, pieceWords, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0));
   double tAlloc = now() - t0;
   VkCommandBuffer cb = cbAlloc();
   cbBegin(cb, 1);
   dispatchAll(cb, pipeWrite, &d, tag, 1);
   cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
   dispatchAll(cb, pipeInc, &d, 1, 0);
   cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
   CHECK(vkEndCommandBuffer(cb));
   VkFence f = fenceCreate();
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   double t1 = now();
   CHECK(vkQueueSubmit(queue, 1, &si, f));
   if (fenceWait(f, 60.0)) { fail("vram fence timeout"); exit(3); }
   double tGpu = now() - t1;
   uint64_t bad = 0;
   for (uint32_t from = 0; from < words; from += pieceWords) {
      const uint32_t n = words - from < pieceWords ? words - from : pieceWords;
      memset(h.cpu, 0xAB, (size_t)n * 4);
      copyOut(&d, &h, from, n);
      for (uint32_t i = 0; i < n; i++)
         if (h.cpu[i] != (from + i) * 3u + tag + 1u) bad++;
   }
   printf("%5u MB device-local: allocate and map %.2f s, write+increment %.3f s (%.1f GB/s), %llu of %u values wrong\n", mb, tAlloc, tGpu, (double)mb / 1024.0 / (tGpu > 0 ? tGpu : 1e-9) * 2,
          (unsigned long long)bad, words);
   if (bad) {
      memset(h.cpu, 0xAB, 64);
      copyOut(&d, &h, 0, 16);
      printf("first words read back: %08x %08x %08x %08x (want %08x %08x %08x %08x)\n", h.cpu[0], h.cpu[1], h.cpu[2], h.cpu[3], tag + 1u, 3u + tag + 1u, 6u + tag + 1u, 9u + tag + 1u);
      fail("device-local values");
   }
   vkDestroyFence(dev, f, NULL);
   vkFreeCommandBuffers(dev, cmdPool, 1, &cb);
   bufDestroy(&h);
   bufDestroy(&d);
   return bad ? 1 : 0;
}

static void testVram(uint32_t maxMb)
{
   printf("== vram\n");
   for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
      printf("memory type %u: flags 0x%x heap %u\n", i, memProps.memoryTypes[i].propertyFlags, memProps.memoryTypes[i].heapIndex);
   for (uint32_t i = 0; i < memProps.memoryHeapCount; i++)
      printf("heap %u: %llu MB, flags 0x%x\n", i, (unsigned long long)(memProps.memoryHeaps[i].size >> 20), memProps.memoryHeaps[i].flags);
   static const uint32_t sizes[] = {1, 16, 128, 512};
   for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
      if (sizes[i] <= maxMb) vramOne(sizes[i], 100 + i);

   /* CPU-visible VRAM: written through the aperture by the CPU, incremented by the GPU, read back through the aperture */
   {
      Buf v;
      VkResult r = bufCreateEx(&v, 1u << 18, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
      if (r != VK_SUCCESS || !v.cpu) {
         printf("CPU-visible VRAM: not available (%d)\n", (int)r);
      } else {
         for (uint32_t i = 0; i < v.words; i++) v.cpu[i] = i * 7u;
         VkCommandBuffer cb = cbAlloc();
         cbBegin(cb, 1);
         dispatchAll(cb, pipeInc, &v, 5, 0);
         cmdBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
         CHECK(vkEndCommandBuffer(cb));
         VkFence f = fenceCreate();
         VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
         CHECK(vkQueueSubmit(queue, 1, &si, f));
         if (fenceWait(f, 20.0)) { fail("visible vram fence timeout"); return; }
         uint32_t bad = 0;
         for (uint32_t i = 0; i < v.words; i++) if (v.cpu[i] != i * 7u + 5u) bad++;
         printf("CPU-visible VRAM, 1 MB: CPU writes, GPU increments, CPU reads: %u of %u values wrong\n", bad, v.words);
         if (bad) fail("visible vram values");
         vkDestroyFence(dev, f, NULL);
         bufDestroy(&v);
      }
   }

   /* How much device-local memory can be had: 256 MB buffers until the allocator says no, each written by the GPU, then every one checked at its ends, then freed */
   {
      Buf big[64];
      int n = 0;
      double t0 = now();
      while (n < 64 && (uint64_t)n * 256 < maxMb * 64ull) {
         if (bufCreateEx(&big[n], 64u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != VK_SUCCESS) break;
         n++;
      }
      printf("%d buffers of 256 MB device-local (%d MB) allocated in %.2f s\n", n, n * 256, now() - t0);
      Buf h;
      CHECK(bufCreateEx(&h, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0));
      VkCommandBuffer cb = cbAlloc();
      cbBegin(cb, 1);
      for (int i = 0; i < n; i++) {
         cmdDispatch(cb, pipeWrite, &big[i], (PC){0, 1000u + (uint32_t)i, 4096});                        /* the first 4096 words */
         cmdDispatch(cb, pipeWrite, &big[i], (PC){big[i].words - 4096, 1000u + (uint32_t)i, 4096});      /* and the last 4096 */
      }
      cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      CHECK(vkEndCommandBuffer(cb));
      VkFence f = fenceCreate();
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
      CHECK(vkQueueSubmit(queue, 1, &si, f));
      if (fenceWait(f, 60.0)) { fail("fill fence timeout"); return; }
      uint32_t bad = 0;
      for (int i = 0; i < n; i++) {
         for (int end = 0; end < 2; end++) {
            const uint32_t from = end ? big[i].words - 4096 : 0;
            memset(h.cpu, 0xAB, 4096 * 4);
            copyOut(&big[i], &h, from, 4096);
            for (uint32_t w = 0; w < 4096; w++) if (h.cpu[w] != (from + w) * 3u + 1000u + (uint32_t)i) bad++;
         }
      }
      printf("%u of %d values wrong at the ends of the %d buffers\n", bad, n * 8192, n);
      if (bad) fail("device-local ends");
      vkDestroyFence(dev, f, NULL);
      for (int i = 0; i < n; i++) bufDestroy(&big[i]);
      bufDestroy(&h);
      Buf again;
      VkResult r2 = bufCreateEx(&again, 64u << 20, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
      printf("after freeing them all, a 256 MB buffer again: %s\n", r2 == VK_SUCCESS ? "ok" : "FAILED");
      if (r2 != VK_SUCCESS) fail("realloc after free"); else bufDestroy(&again);
   }
}

#define NBUF 24
static uint32_t sizeFor(uint32_t i, uint32_t round)
{
   static const uint32_t kb[] = {4, 16, 64, 256, 1024, 4096, 8192};       /* up to 8 MB: 32768 workgroups, under the 65535 limit */
   return kb[(i + round) % 7] * 256;                                       /* words */
}

static void testMulti(uint32_t rounds)
{
   printf("== multi: %u rounds of %d buffers\n", rounds, NBUF);
   Buf b[NBUF];
   memset(b, 0, sizeof(b));
   uint32_t gen[NBUF];
   VkFence fence = fenceCreate();
   for (uint32_t r = 0; r < rounds; r++) {
      for (int i = 0; i < NBUF; i++) {
         if (!b[i].buf) {
            bufCreate(&b[i], sizeFor((uint32_t)i, r));
            memset(b[i].cpu, 0xAB, (size_t)b[i].words * 4);
            gen[i] = r * 100 + (uint32_t)i;
         }
      }
      VkCommandBuffer cb = cbAlloc();
      cbBegin(cb, 1);
      for (int i = 0; i < NBUF; i++) {
         cmdDispatch(cb, pipeWrite, &b[i], (PC){0, gen[i], b[i].words});
         cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
         cmdDispatch(cb, pipeInc, &b[i], (PC){0, 1, b[i].words});
      }
      cmdBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
      CHECK(vkEndCommandBuffer(cb));
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
      CHECK(vkQueueSubmit(queue, 1, &si, fence));
      if (fenceWait(fence, 30.0)) { fail("multi fence timeout"); return; }
      CHECK(vkResetFences(dev, 1, &fence));
      uint64_t total = 0;
      uint32_t bad = 0;
      for (int i = 0; i < NBUF; i++) {
         total += (uint64_t)b[i].words * 4;
         for (uint32_t w = 0; w < b[i].words; w++)
            if (b[i].cpu[w] != w * 3u + gen[i] + 1u) bad++;
      }
      printf("round %u: %d buffers, %llu KB, %u values wrong\n", r, NBUF, (unsigned long long)(total / 1024), bad);
      if (bad) fail("multi values");
      vkFreeCommandBuffers(dev, cmdPool, 1, &cb);
      for (int i = 0; i < NBUF; i += 2) bufDestroy(&b[i]);                 /* free every other buffer, the next round fills the holes with other sizes */
   }
   for (int i = 0; i < NBUF; i++) if (b[i].buf) bufDestroy(&b[i]);
   vkDestroyFence(dev, fence, NULL);
}

int main(int argc, char **argv)
{
   const char *mode = argc > 1 ? argv[1] : "all";
   const uint32_t arg = argc > 2 ? (uint32_t)atoi(argv[2]) : 0;

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "rn_vk_stress", .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n < 1) { fprintf(stderr, "no physical device\n"); return 2; }
   vkGetPhysicalDeviceMemoryProperties(pd, &memProps);
   uint32_t nq = 8;
   VkQueueFamilyProperties qf[8];
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   int fam = -1;
   const int computeOnly = getenv("VK_QUEUE_COMPUTE_ONLY") != NULL;      /* default: the first family that can compute (the graphics one when there is one); with the variable, one that cannot draw */
   for (uint32_t i = 0; i < nq; i++) {
      printf("queue family %u: flags 0x%x, %u queue(s)\n", i, qf[i].queueFlags, qf[i].queueCount);
      if (fam < 0 && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (!computeOnly || !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))) fam = (int)i;
   }
   if (fam < 0) { fprintf(stderr, "no compute queue\n"); return 2; }
   family = (uint32_t)fam;
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   vkGetDeviceQueue(dev, family, 0, &queue);

   VkDescriptorSetLayoutBinding bind = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo dsl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &bind};
   CHECK(vkCreateDescriptorSetLayout(dev, &dsl, NULL, &setLayout));
   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(PC)};
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &setLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pipeLayout));
   pipeWrite = makePipeline(stress_write_spv, stress_write_spv_len);
   pipeInc = makePipeline(stress_inc_spv, stress_inc_spv_len);
   VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 64};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, .maxSets = 64, .poolSizeCount = 1, .pPoolSizes = &ps};
   CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &descPool));
   VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
   CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cmdPool));

   const int all = !strcmp(mode, "all");
   if (all || !strcmp(mode, "burst")) testBurst(arg ? arg : 3000);
   if (all || !strcmp(mode, "saturate")) testSaturate(arg ? arg : 200);
   if (all || !strcmp(mode, "barrier")) testBarrier(arg ? arg : 100);
   if (all || !strcmp(mode, "multi")) testMulti(arg ? arg : 20);
   if (!strcmp(mode, "vram") || all) testVram(arg ? arg : 16384);
   printf(failures ? "RESULT: %d FAILED\n" : "RESULT: PASS\n", failures);
   return failures ? 1 : 0;
}
