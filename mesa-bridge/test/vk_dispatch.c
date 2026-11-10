// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

/*
 * First compute dispatch through RADV on the RedNavi kext: one storage buffer, one compute shader (vk_dispatch.comp) that writes v[i] = i * 3 + push_constant,
 * 64 invocations per group, a number of groups from the command line (default 1). Checks every dword on the CPU.
 *
 *   clang -O1 -arch x86_64 -mmacosx-version-min=12 -I/opt/local/include test/vk_dispatch.c -L/opt/local/lib -lvulkan -o vk_dispatch
 *   ./run_tool.sh ./vk_dispatch [groups]      (same environment as vulkaninfo: VK_ICD_FILENAMES, DYLD_LIBRARY_PATH)
 *
 * Exit status 0 when every value is right. Plain Vulkan 1.0, no extensions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "vk_dispatch_spv.h"

#define CHECK(x)                                                                                                  \
   do {                                                                                                           \
      VkResult r_ = (x);                                                                                          \
      if (r_ != VK_SUCCESS) {                                                                                     \
         fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, (int)r_);                                    \
         exit(2);                                                                                                 \
      }                                                                                                           \
   } while (0)

static int find_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want)
         return (int)i;
   return -1;
}

int main(int argc, char **argv)
{
   const uint32_t groups = argc > 1 ? (uint32_t)atoi(argv[1]) : 1;
   const uint32_t count = groups * 64, add = 7;
   const VkDeviceSize bytes = (VkDeviceSize)count * 4;

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "rn_vk_dispatch", .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));

   uint32_t n = 1;
   VkPhysicalDevice pd;
   VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if (r != VK_SUCCESS && r != VK_INCOMPLETE) CHECK(r);
   if (n < 1) { fprintf(stderr, "no physical device\n"); return 2; }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pd, &props);
   printf("device: %s\n", props.deviceName);

   uint32_t nq = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties qf[8];
   if (nq > 8) nq = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   int fam = -1;
   for (uint32_t i = 0; i < nq; i++) {
      printf("queue family %u: flags 0x%x, %u queue(s)\n", i, qf[i].queueFlags, qf[i].queueCount);
      if (fam < 0 && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) fam = (int)i;
   }
   if (fam < 0) { fprintf(stderr, "no compute queue family\n"); return 2; }

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = (uint32_t)fam, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, (uint32_t)fam, 0, &q);
   puts("device and queue created");

   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer buf;
   CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, buf, &mr);
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   int mt = find_type(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (mt < 0) { fprintf(stderr, "no host visible coherent memory type\n"); return 2; }
   printf("buffer %llu bytes, memory type %d (flags 0x%x)\n", (unsigned long long)bytes, mt, mp.memoryTypes[mt].propertyFlags);
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt};
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
   CHECK(vkBindBufferMemory(dev, buf, mem, 0));
   uint32_t *p;
   CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p));
   for (uint32_t i = 0; i < count; i++) p[i] = 0xdeadbeef;
   puts("buffer allocated, bound, mapped");

   VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vk_dispatch_spv_len, .pCode = (const uint32_t *)vk_dispatch_spv};
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &sm));

   VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo dsl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
   VkDescriptorSetLayout setLayout;
   CHECK(vkCreateDescriptorSetLayout(dev, &dsl, NULL, &setLayout));
   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 4};
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &setLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
   VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"},
                                      .layout = pl};
   VkPipeline pipe;
   CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
   puts("pipeline compiled (ACO)");

   VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
   VkDescriptorSetAllocateInfo dsa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &setLayout};
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(dev, &dsa, &ds));
   VkDescriptorBufferInfo dbi = {.buffer = buf, .offset = 0, .range = VK_WHOLE_SIZE};
   VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
   vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

   VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = (uint32_t)fam};
   VkCommandPool cp;
   CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
   VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &cba, &cb));
   VkCommandBufferBeginInfo cbb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(vkBeginCommandBuffer(cb, &cbb));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &add);
   vkCmdDispatch(cb, groups, 1, 1);
   VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
   CHECK(vkEndCommandBuffer(cb));

   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK(vkCreateFence(dev, &fci, NULL, &fence));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   puts("submitting");
   fflush(stdout);
   CHECK(vkQueueSubmit(q, 1, &si, fence));
   r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 8000000000ull);
   printf("wait for fence: %d\n", (int)r);
   if (r != VK_SUCCESS) return 3;

   uint32_t bad = 0, first = 0;
   for (uint32_t i = 0; i < count; i++) {
      if (p[i] != i * 3u + add) {
         if (!bad) first = i;
         bad++;
      }
   }
   if (bad)
      printf("FAIL: %u of %u values wrong, first at %u: got 0x%08x want 0x%08x\n", bad, count, first, p[first], first * 3u + add);
   else
      printf("PASS: %u values right (v[%u] = %u)\n", count, count - 1, p[count - 1]);
   return bad ? 1 : 0;
}
