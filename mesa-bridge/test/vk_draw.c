// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

/*
 * First graphics test: a triangle drawn by RADV on the RedNavi graphics queue. A 256 x 256 R8G8B8A8_UNORM image (optimal tiling, device-local VRAM) is cleared to blue by a render
 * pass and a triangle covering its upper-left half is drawn in red; the image is copied to a host buffer and checked pixel by pixel away from the diagonal edge.
 *   clang -O1 -arch x86_64 -mmacosx-version-min=12 -I/opt/local/include test/vk_draw.c -L/opt/local/lib -lvulkan -o vk_draw
 * Exit status 0 when every checked pixel has the right colour. Plain Vulkan 1.1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "vk_draw_spv.h"

#define CHECK(x)                                                                                    \
   do {                                                                                             \
      VkResult r_ = (x);                                                                            \
      if (r_ != VK_SUCCESS) {                                                                       \
         fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, (int)r_);                      \
         exit(2);                                                                                   \
      }                                                                                             \
   } while (0)

static VkPhysicalDeviceMemoryProperties mp;

static uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want && !(mp.memoryTypes[i].propertyFlags & avoid)) return i;
   fprintf(stderr, "no memory type 0x%x / 0x%x\n", want, avoid);
   exit(2);
}

int main(int argc, char **argv)
{
   const uint32_t W = argc > 1 && atoi(argv[1]) > 0 ? (uint32_t)atoi(argv[1]) : 256, H = W;
   const char *mode = argc > 2 ? argv[2] : "";
   const int linear = !strcmp(mode, "linear") || !strcmp(mode, "clear");   /* "clear": vkCmdClearColorImage instead of a render pass; */
   const int doClear = !strcmp(mode, "clear");   /* a linear image in host-visible memory: the CPU reads the target itself, no tiling or compression in play */
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "rn_vk_draw", .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   VkPhysicalDevice pd;
   VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n < 1) { fprintf(stderr, "no device\n"); return 2; }
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   uint32_t nq = 8;
   VkQueueFamilyProperties qf[8];
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   int fam = -1;
   for (uint32_t i = 0; i < nq; i++) if (fam < 0 && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) fam = (int)i;
   if (fam < 0) { fprintf(stderr, "no graphics queue family\n"); return 2; }
   printf("graphics queue family %d (flags 0x%x)\n", fam, qf[fam].queueFlags);
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = (uint32_t)fam, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, (uint32_t)fam, 0, &q);

   /* the target image, in device-local memory */
   VkImageCreateInfo imci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1}, .mipLevels = 1,
                             .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage img;
   CHECK(vkCreateImage(dev, &imci, NULL, &img));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = linear ? memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                                                                          : memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   VkDeviceMemory imem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
   CHECK(vkBindImageMemory(dev, img, imem, 0));
   VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView view;
   CHECK(vkCreateImageView(dev, &vci, NULL, &view));

   /* the host buffer the image is copied to */
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer buf;
   CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
   vkGetBufferMemoryRequirements(dev, buf, &mr);
   mai = (VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
   VkDeviceMemory bmem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &bmem));
   CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
   uint32_t *px;
   CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
   memset(px, 0xAB, (size_t)W * H * 4);

   /* render pass: clear, draw, leave in transfer-source layout */
   VkAttachmentDescription att = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
   VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref};
   VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub};
   VkRenderPass rp;
   CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
   VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1};
   VkFramebuffer fb;
   CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fb));

   VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
   VkShaderModule vs, fs;
   smi.codeSize = draw_vert_spv_len; smi.pCode = (const uint32_t *)draw_vert_spv;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &vs));
   smi.codeSize = draw_frag_spv_len; smi.pCode = (const uint32_t *)draw_frag_spv;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs));
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
   VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
   VkRect2D sc = {{0, 0}, {W, H}};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
   VkGraphicsPipelineCreateInfo gpi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl, .renderPass = rp, .subpass = 0};
   VkPipeline pipe;
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));
   puts("graphics pipeline compiled");

   VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = (uint32_t)fam};
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
   VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(vkBeginCommandBuffer(cmd, &cbbi));
   VkClearValue clear = {.color = {{0.0f, 0.0f, 1.0f, 1.0f}}};
   if (doClear) {
      VkImageMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                 .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
      vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear.color, 1, &b1.subresourceRange);
      VkImageMemoryBarrier b2 = b1;
      b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
   }
   VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &clear};
   if (!doClear) {
      vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      vkCmdDraw(cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(cmd);
   }
   VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
   vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
   VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
   CHECK(vkEndCommandBuffer(cmd));
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK(vkCreateFence(dev, &fi, NULL, &fence));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
   puts("submitting");
   fflush(stdout);
   CHECK(vkQueueSubmit(q, 1, &si, fence));
   r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 8000000000ull);
   printf("wait for fence: %d\n", (int)r);
   if (r != VK_SUCCESS) return 3;

   /* the image is RGBA8: red is 0xff0000ff (little endian, R in the low byte), blue is 0xffff0000 */
   const uint32_t RED = 0xff0000ffu, BLUE = 0xffff0000u;
   if (linear) {                                           /* read the target in place, row by row */
      VkImageSubresource isr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
      VkSubresourceLayout lay;
      vkGetImageSubresourceLayout(dev, img, &isr, &lay);
      uint8_t *base;
      CHECK(vkMapMemory(dev, imem, 0, VK_WHOLE_SIZE, 0, (void **)&base));
      for (uint32_t y = 0; y < H; y++) memcpy(&px[y * W], base + lay.offset + (size_t)y * lay.rowPitch, (size_t)W * 4);
   }
   uint32_t bad = 0, checked = 0, firstX = 0, firstY = 0, firstGot = 0;
   for (uint32_t y = 0; y < H; y += 3)
      for (uint32_t x = 0; x < W; x += 3) {
         const int sum = (int)x + (int)y - (int)(W - 1);                      /* the diagonal edge is x + y = W - 1 */
         if (sum > -4 && sum < 4) continue;
         const uint32_t want = (sum < 0 && !doClear) ? RED : BLUE;
         checked++;
         if (px[y * W + x] != want) { if (!bad) { firstX = x; firstY = y; firstGot = px[y * W + x]; } bad++; }
      }
   if (doClear) printf("clear: first pixel 0x%08x, last 0x%08x\n", px[0], px[W * H - 1]);
   if (bad) printf("FAIL: %u of %u pixels wrong, first at (%u,%u): 0x%08x, want 0x%08x\n", bad, checked, firstX, firstY, firstGot, (firstX + firstY < W - 1) ? RED : BLUE);
   else printf("PASS: %u pixels checked, red upper-left half, blue lower-right half (%08x %08x)\n", checked, px[10 * W + 10], px[(H - 10) * W + (W - 10)]);
   return bad ? 1 : 0;
}
