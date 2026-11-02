/*
 * Compute stress tests for RADV on the RedNavi kext (plain Vulkan 1.1, compute queue only):
 *
 *   burst [N]    N separate vkQueueSubmit calls (default 3000) of tiny dispatches into one buffer: IB turnover and ring-buffer saturation, a fence every 256 submits.
 *   saturate [N] N submits (default 200) that keep the GPU busy for a while each, so the ring buffer stays full and the kext must hold submits back.
 *   barrier [K]  K dependent dispatches in one command buffer (a writer, then K increments, a compute-to-compute barrier between each), then the same chain
 *                over several submits tied by semaphores (syncobj in/out in the kext).
 *   multi [R]    R rounds (default 20) of several storage buffers alive at once, one dispatch each, verified, then half of them freed and replaced.
 *   all          the three above.
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

static void bufCreate(Buf *b, uint32_t words)
{
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)words * 4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   CHECK(vkCreateBuffer(dev, &bci, NULL, &b->buf));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, b->buf, &mr);
   int mt = -1;
   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
      if ((mr.memoryTypeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) { mt = (int)i; break; }
   if (mt < 0) { fprintf(stderr, "no host visible memory type\n"); exit(2); }
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt};
   CHECK(vkAllocateMemory(dev, &mai, NULL, &b->mem));
   CHECK(vkBindBufferMemory(dev, b->buf, b->mem, 0));
   CHECK(vkMapMemory(dev, b->mem, 0, VK_WHOLE_SIZE, 0, (void **)&b->cpu));
   b->words = words;
   VkDescriptorSetAllocateInfo dsa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = descPool, .descriptorSetCount = 1, .pSetLayouts = &setLayout};
   CHECK(vkAllocateDescriptorSets(dev, &dsa, &b->set));
   VkDescriptorBufferInfo dbi = {.buffer = b->buf, .offset = 0, .range = VK_WHOLE_SIZE};
   VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = b->set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
   vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
}

static void bufDestroy(Buf *b)
{
   vkFreeDescriptorSets(dev, descPool, 1, &b->set);
   vkUnmapMemory(dev, b->mem);
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
   for (uint32_t i = 0; i < nq; i++) if (fam < 0 && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) fam = (int)i;
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
   printf(failures ? "RESULT: %d FAILED\n" : "RESULT: PASS\n", failures);
   return failures ? 1 : 0;
}
