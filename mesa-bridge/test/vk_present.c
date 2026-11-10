// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

/*
 * Presentation test: what vk_draw renders (a red triangle over the upper-left half of a blue image) goes to the monitor. The image is as large as the display surface of RedNaviFB
 * (B8G8R8A8, which is the surface's XRGB8888 byte order), rendered in tiled device-local VRAM; the surface itself is imported with VK_EXT_external_memory_host (the bridge maps it
 * and hands out the pointer: rn_bridge_scanout) and vkCmdCopyImageToBuffer writes it, row pitch included, so the GPU puts the picture on the screen. Afterwards the CPU reads a few
 * pixels back through the same memory. Needs rednavi_fb=1 (and a display). Usage: vk_present [seconds to leave the picture up, default 20]
 *   clang -O1 -arch x86_64 -mmacosx-version-min=12 -I/opt/local/include test/vk_present.c -L/opt/local/lib -lvulkan -o vk_present
 * Exit status 0 when the pixels read back from the surface are right.
 *   vk_present scene [seconds]: the harder scene below (depth test, texture sampling, 4x multisampling with a resolve into the surface).
 *   vk_present spin [seconds [fps [flip]]]: two rotating cubes from vertex and index buffers, MVP matrices in push constants, a per-frame uniform buffer, 4x MSAA, rendered continuously at
 *   the given rate (default 20 s, 60 fps) with two frames in flight, every frame copied into the display surface; prints frame-time statistics. Exit status 0 when no frame was late
 *   by more than 4 frame times and the sampled pixels were always right.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "vk_draw_spv.h"
#include "vk_scene_spv.h"
#include "vk_cube_spv.h"
#include <math.h>
#include <time.h>

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

static int triangle_main(int argc, char **argv)
{
   const int seconds = argc > 1 ? atoi(argv[1]) : 20;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "rn_vk_present", .apiVersion = VK_API_VERSION_1_1};
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
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = (uint32_t)fam, .queueCount = 1, .pQueuePriorities = &prio};
   const char *exts[] = {"VK_EXT_external_memory_host"};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts};
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, (uint32_t)fam, 0, &q);

   /* the display surface, from the bridge in the driver library the loader opened */
   void *lib = dlopen("libvulkan_radeon.dylib", RTLD_LAZY | RTLD_NOLOAD);
   int (*scanout)(uint32_t, uint32_t *, uint32_t *, uint32_t *, void **, size_t *) = lib ? dlsym(lib, "rn_bridge_scanout") : NULL;
   if (!scanout) { fprintf(stderr, "rn_bridge_scanout not found\n"); return 2; }
   int (*present_fn)(uint32_t, int) = dlsym(lib, "rn_bridge_present");
   uint32_t W, H, pitch;
   void *scan;
   size_t slen;
   int sr = scanout(1, &W, &H, &pitch, &scan, &slen);
   if (sr) { fprintf(stderr, "no display surface (rn_bridge_scanout %d): needs rednavi_fb=1\n", sr); return 2; }
   printf("display surface %ux%u, pitch %u bytes, %zu bytes mapped at %p\n", W, H, pitch, slen, scan);

   PFN_vkGetMemoryHostPointerPropertiesEXT getHostProps = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");
   VkMemoryHostPointerPropertiesEXT hpp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
   CHECK(getHostProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, scan, &hpp));
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = slen, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer buf;
   CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, buf, &mr);
   VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = scan};
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = slen,
                               .memoryTypeIndex = memType(hpp.memoryTypeBits & mr.memoryTypeBits, 0, 0)};
   VkDeviceMemory bmem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &bmem));
   CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
   puts("display surface imported as a buffer");

   /* the target image: the surface's size and byte order, tiled, device-local */
   const VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
   VkImageCreateInfo imci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {W, H, 1}, .mipLevels = 1,
                             .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage img;
   CHECK(vkCreateImage(dev, &imci, NULL, &img));
   vkGetImageMemoryRequirements(dev, img, &mr);
   mai = (VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   VkDeviceMemory imem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
   CHECK(vkBindImageMemory(dev, img, imem, 0));
   VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView view;
   CHECK(vkCreateImageView(dev, &vci, NULL, &view));

   VkAttachmentDescription att = {.format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
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
   VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &clear};
   vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   VkBufferImageCopy cp = {.bufferRowLength = pitch / 4, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
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

   /* read the surface back through the CPU mapping: blue everywhere except the red upper-left half (the diagonal x + y = W - 1 of vk_draw, here over the whole W x H) */
   const uint32_t RED = 0x00ff0000u, BLUE = 0x000000ffu;
   uint32_t bad = 0, checked = 0, firstX = 0, firstY = 0, firstGot = 0, want;
   for (uint32_t y = 0; y < H; y += 7)
      for (uint32_t x = 0; x < W; x += 7) {
         const float u = (float)x / (float)W, v = (float)y / (float)H;
         if (u + v > 0.98f && u + v < 1.02f) continue;                      /* near the edge */
         want = (u + v < 1.0f) ? RED : BLUE;
         const uint32_t got = ((volatile uint32_t *)((uint8_t *)scan + (size_t)y * pitch))[x] & 0x00ffffffu;
         checked++;
         if (got != want) { if (!bad) { firstX = x; firstY = y; firstGot = got; } bad++; }
      }
   if (bad)
      for (uint32_t y = 0; y < H; y += 60)
         printf("row %4u: x=100 0x%08x x=1800 0x%08x\n", y, ((volatile uint32_t *)((uint8_t *)scan + (size_t)y * pitch))[100], ((volatile uint32_t *)((uint8_t *)scan + (size_t)y * pitch))[1800]);
   if (bad) printf("FAIL: %u of %u pixels wrong, first at (%u,%u): 0x%06x\n", bad, checked, firstX, firstY, firstGot);
   else printf("PASS: %u pixels read back from the display surface: red upper-left half, blue the rest\n", checked);
   if (seconds > 0) {
      present_fn(1, 1);                                     /* surface 1 on the display; the desktop's surface 0 is WindowServer's and would be drawn over */
      printf("leaving the picture up for %d s\n", seconds); fflush(stdout); sleep((unsigned)seconds);
      present_fn(0, 1);
   }
   return bad ? 1 : 0;
}

/* ------------------------------------------------------------------------------------------------------------------------------------------ scene
 * Two quads that cross each other and a thin slanted triangle, drawn with a D32_SFLOAT depth buffer, a sampled checkerboard texture and 4 samples per pixel. Quad A (texture) is
 * nearer than quad B (a gradient computed in the shader) for x < 0 and farther for x > 0, and B is drawn after A: only the depth test keeps A visible on the left. The multisampled
 * colour image is resolved by the render pass into a single-sample image, which is copied into the display surface. Checked against the picture computed here. */
static int scene_main(int argc, char **argv)
{
   const int seconds = argc > 1 ? atoi(argv[1]) : 20;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "rn_vk_scene", .apiVersion = VK_API_VERSION_1_1};
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
   VkImageFormatProperties ifp;
   r = vkGetPhysicalDeviceImageFormatProperties(pd, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &ifp);
   printf("B8G8R8A8 colour attachment: %d, sample counts 0x%x\n", (int)r, ifp.sampleCounts);
   r = vkGetPhysicalDeviceImageFormatProperties(pd, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &ifp);
   printf("D32_SFLOAT depth attachment: %d, sample counts 0x%x\n", (int)r, ifp.sampleCounts);
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = (uint32_t)fam, .queueCount = 1, .pQueuePriorities = &prio};
   const char *exts[] = {"VK_EXT_external_memory_host"};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts};
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, (uint32_t)fam, 0, &q);

   void *lib = dlopen("libvulkan_radeon.dylib", RTLD_LAZY | RTLD_NOLOAD);
   int (*scanout)(uint32_t, uint32_t *, uint32_t *, uint32_t *, void **, size_t *) = lib ? dlsym(lib, "rn_bridge_scanout") : NULL;
   if (!scanout) { fprintf(stderr, "rn_bridge_scanout not found\n"); return 2; }
   int (*present_fn)(uint32_t, int) = dlsym(lib, "rn_bridge_present");
   uint32_t W, H, pitch;
   void *scan;
   size_t slen;
   if (scanout(1, &W, &H, &pitch, &scan, &slen)) { fprintf(stderr, "no display surface: needs rednavi_fb=1\n"); return 2; }
   printf("display surface %ux%u, pitch %u\n", W, H, pitch);
   PFN_vkGetMemoryHostPointerPropertiesEXT getHostProps = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");
   VkMemoryHostPointerPropertiesEXT hpp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
   CHECK(getHostProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, scan, &hpp));
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = slen, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer sbuf;
   CHECK(vkCreateBuffer(dev, &bci, NULL, &sbuf));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, sbuf, &mr);
   VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = scan};
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = slen, .memoryTypeIndex = memType(hpp.memoryTypeBits & mr.memoryTypeBits, 0, 0)};
   VkDeviceMemory smem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &smem));
   CHECK(vkBindBufferMemory(dev, sbuf, smem, 0));

   /* images: 4x colour, depth, resolve target, and the texture */
   const VkFormat cfmt = VK_FORMAT_B8G8R8A8_UNORM, dfmt = VK_FORMAT_D32_SFLOAT;
   struct { VkFormat f; VkImageUsageFlags u; VkSampleCountFlagBits s; VkImageAspectFlags a; uint32_t w, h; VkImage img; VkDeviceMemory mem; VkImageView view; } im[4] = {
      {cfmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_ASPECT_COLOR_BIT, W, H},
      {dfmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, W, H},
      {cfmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, W, H},
      {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 64, 64},
   };
   for (int i = 0; i < 4; i++) {
      VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = im[i].f, .extent = {im[i].w, im[i].h, 1}, .mipLevels = 1, .arrayLayers = 1,
                              .samples = im[i].s, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = im[i].u, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
      if (i == 0) ic.usage &= ~VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
      CHECK(vkCreateImage(dev, &ic, NULL, &im[i].img));
      vkGetImageMemoryRequirements(dev, im[i].img, &mr);
      VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
      CHECK(vkAllocateMemory(dev, &ma, NULL, &im[i].mem));
      CHECK(vkBindImageMemory(dev, im[i].img, im[i].mem, 0));
      VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im[i].img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = im[i].f,
                                  .subresourceRange = {im[i].a, 0, 1, 0, 1}};
      CHECK(vkCreateImageView(dev, &vc, NULL, &im[i].view));
   }

   /* the texture: 64 x 64, 8 x 8 texel cells, yellow and dark grey, through a host staging buffer */
   VkBufferCreateInfo tbi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64 * 64 * 4, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer stage;
   CHECK(vkCreateBuffer(dev, &tbi, NULL, &stage));
   vkGetBufferMemoryRequirements(dev, stage, &mr);
   VkMemoryAllocateInfo sm = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
   VkDeviceMemory stmem;
   CHECK(vkAllocateMemory(dev, &sm, NULL, &stmem));
   CHECK(vkBindBufferMemory(dev, stage, stmem, 0));
   uint32_t *tx;
   CHECK(vkMapMemory(dev, stmem, 0, VK_WHOLE_SIZE, 0, (void **)&tx));
   for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) tx[y * 64 + x] = (((x / 8) + (y / 8)) & 1) ? 0xff00dcffu : 0xff282828u;   /* RGBA bytes: yellow (255,220,0), grey (40,40,40) */
   VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod = 0.25f};
   VkSampler samp;
   CHECK(vkCreateSampler(dev, &sci, NULL, &samp));
   VkDescriptorSetLayoutBinding dlb = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutCreateInfo dlci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &dlb};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl));
   VkDescriptorPoolSize dps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
   VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps};
   VkDescriptorPool dpool;
   CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
   VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet dset;
   CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));
   VkDescriptorImageInfo dii = {samp, im[3].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii};
   vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

   /* render pass: 4x colour (not stored), depth (not stored), resolve (stored, left as a transfer source) */
   VkAttachmentDescription at[3] = {
      {.format = cfmt, .samples = VK_SAMPLE_COUNT_4_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {.format = dfmt, .samples = VK_SAMPLE_COUNT_4_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
      {.format = cfmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}};
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}, rref = {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref, .pResolveAttachments = &rref, .pDepthStencilAttachment = &dref};
   VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 3, .pAttachments = at, .subpassCount = 1, .pSubpasses = &sub};
   VkRenderPass rp;
   CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
   VkImageView fbv[3] = {im[0].view, im[1].view, im[2].view};
   VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 3, .pAttachments = fbv, .width = W, .height = H, .layers = 1};
   VkFramebuffer fb;
   CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fb));

   VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
   VkShaderModule vs, fs;
   smi.codeSize = scene_vert_spv_len; smi.pCode = (const uint32_t *)scene_vert_spv;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &vs));
   smi.codeSize = scene_frag_spv_len; smi.pCode = (const uint32_t *)scene_frag_spv;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs));
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
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
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_4_BIT};
   VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
   VkGraphicsPipelineCreateInfo gpi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb, .layout = pl,
                                       .renderPass = rp, .subpass = 0};
   VkPipeline pipe;
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));
   puts("pipeline compiled");

   VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = (uint32_t)fam};
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
   VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(vkBeginCommandBuffer(cmd, &cbbi));
   /* texture upload and the layout change that makes it readable by the fragment shader */
   VkImageMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = im[3].img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
   VkBufferImageCopy tcp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {64, 64, 1}};
   vkCmdCopyBufferToImage(cmd, stage, im[3].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &tcp);
   VkImageMemoryBarrier b2 = b1;
   b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
   b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
   VkClearValue cv[3] = {{.color = {{0.0f, 0.0f, 0.4f, 1.0f}}}, {.depthStencil = {1.0f, 0}}, {0}};
   VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 3, .pClearValues = cv};
   vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dset, 0, NULL);
   vkCmdDraw(cmd, 6, 1, 0, 0);     /* A: the textured quad */
   vkCmdDraw(cmd, 6, 1, 0, 1);     /* B: the gradient quad, drawn later, hidden by A where A is nearer */
   vkCmdDraw(cmd, 6, 1, 0, 2);     /* C: the slanted triangle */
   vkCmdEndRenderPass(cmd);
   VkBufferImageCopy cp = {.bufferRowLength = pitch / 4, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
   vkCmdCopyImageToBuffer(cmd, im[2].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sbuf, 1, &cp);
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

   /* the picture, pixel by pixel, from the surface */
   #define PX(x, y) (((volatile uint32_t *)((uint8_t *)scan + (size_t)(y) * pitch))[x] & 0x00ffffffu)
   #define CH(p, sh) ((int)(((p) >> (sh)) & 0xff))
   uint32_t badTex = 0, nTex = 0, badGrad = 0, nGrad = 0, badBg = 0, nBg = 0, partial = 0, badPartial = 0;
   int levels[5] = {0};
   uint32_t first[3] = {0, 0, 0};
   for (uint32_t y = 0; y < H; y++) {
      for (uint32_t x = 0; x < W; x++) {
         const float xn = ((float)x + 0.5f) / (float)W * 2.0f - 1.0f, yn = ((float)y + 0.5f) / (float)H * 2.0f - 1.0f;
         const int inQuad = xn > -0.9f && xn < 0.9f && yn > -0.9f && yn < 0.9f;
         if (inQuad && xn < -0.01f && ((x % 5) == 0) && ((y % 5) == 0)) {              /* A, the textured quad, visible on the left only if the depth test works */
            const float u = xn / 0.9f * 0.5f + 0.5f, v = yn / 0.9f * 0.5f + 0.5f;
            const float fu = u * 64.0f, fv = v * 64.0f;
            if (fu - (int)fu < 0.2f || fu - (int)fu > 0.8f || fv - (int)fv < 0.2f || fv - (int)fv > 0.8f || xn < -0.895f || yn < -0.895f || yn > 0.895f) continue;
            const uint32_t want = (((int)fu + (int)fv) & 1) ? 0x00ffdc00u : 0x00282828u;
            nTex++;
            if (PX(x, y) != want) { if (!badTex) { first[0] = x; first[1] = y; first[2] = PX(x, y); } badTex++; }
         } else if (inQuad && xn > 0.01f && ((x % 5) == 0) && ((y % 5) == 0) && xn < 0.895f && yn > -0.895f && yn < 0.895f) {   /* B, the gradient */
            const float u = xn / 0.9f * 0.5f + 0.5f, v = yn / 0.9f * 0.5f + 0.5f;
            const uint32_t p = PX(x, y);
            nGrad++;
            if (abs(CH(p, 16) - (int)(u * 255.0f + 0.5f)) > 3 || abs(CH(p, 8) - (int)(v * 255.0f + 0.5f)) > 3 || abs(CH(p, 0) - 64) > 3) badGrad++;
         } else if (!inQuad && xn < 0.9f && ((x % 7) == 0) && ((y % 7) == 0)) {      /* the clear colour (0, 0, 0.4) around the quads */
            nBg++;
            if (PX(x, y) != 0x00000066u) badBg++;
         }
      }
   }
   /* the triangle's slanted edge: along each row the red channel steps 0 -> 255 through the 4-sample coverage levels (0, 64, 128, 191, 255, within 3) */
   for (uint32_t y = 120; y < H - 120; y += 3) {
      const float yn = ((float)y + 0.5f) / (float)H * 2.0f - 1.0f;
      const float edgeX = 0.92f + 0.06f * (yn + 0.9f) / 1.8f;             /* from (0.92, -0.9) to (0.98, 0.9): the left edge of the triangle's body is vertical at 0.92, the slanted one is its right side */
      const int xe = (int)((edgeX + 1.0f) * 0.5f * (float)W);
      for (int dx = -3; dx <= 3; dx++) {
         const uint32_t p = PX(xe + dx, y);
         const int rch = CH(p, 16);
         if (rch != 0 && rch != 255) {
            partial++;
            int k = -1;
            for (int l = 1; l <= 3; l++) if (abs(rch - l * 255 / 4) <= 3) k = l;
            if (k < 0 || CH(p, 8) != rch || abs(CH(p, 0) - (102 + (255 - 102) * rch / 255)) > 3) badPartial++; else levels[k]++;
         }
      }
   }
   printf("texture: %u of %u sampled pixels wrong%s\n", badTex, nTex, badTex ? "" : " (checkerboard right where the depth test must keep it)");
   if (badTex) printf("  first at (%u,%u): 0x%06x\n", first[0], first[1], first[2]);
   printf("gradient (right half, drawn later and nearer): %u of %u wrong\n", badGrad, nGrad);
   printf("clear colour: %u of %u wrong\n", badBg, nBg);
   printf("edge of the triangle: %u partly covered pixels, %u not a 4-sample level; levels 1/4: %d, 2/4: %d, 3/4: %d\n", partial, badPartial, levels[1], levels[2], levels[3]);
   const int ok = !badTex && !badGrad && !badBg && nTex > 1000 && nGrad > 1000 && partial > 100 && !badPartial && levels[1] && levels[2] && levels[3];
   puts(ok ? "PASS: depth test, texture sampling and 4x MSAA resolve are right" : "FAIL");
   if (seconds > 0) {
      present_fn(1, 1);                                     /* surface 1 on the display; the desktop's surface 0 is WindowServer's and would be drawn over */
      printf("leaving the picture up for %d s\n", seconds); fflush(stdout); sleep((unsigned)seconds);
      present_fn(0, 1);
   }
   return ok ? 0 : 1;
}


/* ------------------------------------------------------------------------------------------------------------------------------------------ spin
 * A render loop: two cubes (8 vertices, 36 16-bit indices, device-local buffers filled once through a staging buffer) rotate, one orbiting the other. The model-view-projection matrix of
 * each is a push constant, a uniform buffer written by the CPU every frame tints them, the frame is rendered with 4 samples and resolved, and copied into the display surface. Two
 * frames are in flight (two command buffers re-recorded in turn, two uniform buffers and fences). The loop is paced at the requested rate. */
#include "vk_mat.h"

static int spin_main(int argc, char **argv)
{
   const int seconds = argc > 1 ? atoi(argv[1]) : 20;
   const int fps = argc > 2 ? atoi(argv[2]) : 60;
   const int flip = argc > 3 && !strcmp(argv[3], "flip");      /* two display surfaces and a page flip per frame, one frame in flight */
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "rn_vk_spin", .apiVersion = VK_API_VERSION_1_1};
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
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = (uint32_t)fam, .queueCount = 1, .pQueuePriorities = &prio};
   const char *exts[] = {"VK_EXT_external_memory_host"};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts};
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, (uint32_t)fam, 0, &q);

   void *lib = dlopen("libvulkan_radeon.dylib", RTLD_LAZY | RTLD_NOLOAD);
   int (*scanout)(uint32_t, uint32_t *, uint32_t *, uint32_t *, void **, size_t *) = lib ? dlsym(lib, "rn_bridge_scanout") : NULL;
   if (!scanout) { fprintf(stderr, "rn_bridge_scanout not found\n"); return 2; }
   uint32_t W, H, pitch;
   void *scans[3] = {NULL, NULL, NULL};
   size_t slens[3] = {0, 0, 0};
   const int first = flip ? 1 : 0, nsurf = flip ? 3 : 1;           /* surface 0 is the desktop's: WindowServer keeps drawing into it, so the flipping loop uses 1 and 2 */
   for (int i = first; i < nsurf; i++)
      if (scanout(i, &W, &H, &pitch, &scans[i], &slens[i])) { fprintf(stderr, "no display surface %d: needs rednavi_fb=1 (and a mode that leaves room for two)\n", i); return 2; }
   void *scan = scans[first];
   int (*present)(uint32_t, int) = dlsym(lib, "rn_bridge_present");
   if (flip && !present) { fprintf(stderr, "rn_bridge_present not found\n"); return 2; }
   PFN_vkGetMemoryHostPointerPropertiesEXT getHostProps = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");
   VkMemoryHostPointerPropertiesEXT hpp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
   CHECK(getHostProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, scan, &hpp));
   VkBuffer sbufs[3];
   VkMemoryRequirements mr;
   for (int i = first; i < nsurf; i++) {
      VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = slens[i], .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
      CHECK(vkCreateBuffer(dev, &bci, NULL, &sbufs[i]));
      vkGetBufferMemoryRequirements(dev, sbufs[i], &mr);
      VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = scans[i]};
      VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = slens[i], .memoryTypeIndex = memType(hpp.memoryTypeBits & mr.memoryTypeBits, 0, 0)};
      VkDeviceMemory smem;
      CHECK(vkAllocateMemory(dev, &mai, NULL, &smem));
      CHECK(vkBindBufferMemory(dev, sbufs[i], smem, 0));
   }

   /* images: 4x colour, depth, the resolve target */
   const VkFormat cfmt = VK_FORMAT_B8G8R8A8_UNORM, dfmt = VK_FORMAT_D32_SFLOAT;
   struct { VkFormat f; VkImageUsageFlags u; VkSampleCountFlagBits s; VkImageAspectFlags a; VkImage img; VkDeviceMemory mem; VkImageView view; } im[3] = {
      {cfmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_ASPECT_COLOR_BIT},
      {dfmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_ASPECT_DEPTH_BIT},
      {cfmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT},
   };
   for (int i = 0; i < 3; i++) {
      VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = im[i].f, .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
                              .samples = im[i].s, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = im[i].u, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
      CHECK(vkCreateImage(dev, &ic, NULL, &im[i].img));
      vkGetImageMemoryRequirements(dev, im[i].img, &mr);
      VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
      CHECK(vkAllocateMemory(dev, &ma, NULL, &im[i].mem));
      CHECK(vkBindImageMemory(dev, im[i].img, im[i].mem, 0));
      VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im[i].img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = im[i].f, .subresourceRange = {im[i].a, 0, 1, 0, 1}};
      CHECK(vkCreateImageView(dev, &vc, NULL, &im[i].view));
   }

   /* the cube: position and colour per corner, indices of 12 triangles */
   float verts[8 * 6];
   for (int i = 0; i < 8; i++) {
      const float x = (i & 1) ? 1.f : -1.f, y = (i & 2) ? 1.f : -1.f, z = (i & 4) ? 1.f : -1.f;
      float *v = &verts[i * 6];
      v[0] = x; v[1] = y; v[2] = z; v[3] = (i & 1) ? 1.0f : 0.25f; v[4] = (i & 2) ? 1.0f : 0.25f; v[5] = (i & 4) ? 1.0f : 0.25f;
   }
   const uint16_t idx[36] = {0, 1, 3, 0, 3, 2,  4, 6, 7, 4, 7, 5,  0, 2, 6, 0, 6, 4,  1, 5, 7, 1, 7, 3,  0, 4, 5, 0, 5, 1,  2, 3, 7, 2, 7, 6};
   VkBuffer vbuf, ibuf, stage;
   VkDeviceMemory vmem, imemb, stmem;
   VkBufferCreateInfo b1 = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof(verts), .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBufferCreateInfo b2 = b1; b2.size = sizeof(idx); b2.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VkBufferCreateInfo b3 = b1; b3.size = 4096; b3.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
   CHECK(vkCreateBuffer(dev, &b1, NULL, &vbuf)); CHECK(vkCreateBuffer(dev, &b2, NULL, &ibuf)); CHECK(vkCreateBuffer(dev, &b3, NULL, &stage));
   VkBuffer *bl[3] = {&vbuf, &ibuf, &stage};
   VkDeviceMemory *ml[3] = {&vmem, &imemb, &stmem};
   for (int i = 0; i < 3; i++) {
      vkGetBufferMemoryRequirements(dev, *bl[i], &mr);
      VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = i < 2 ? memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                                                          : memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
      CHECK(vkAllocateMemory(dev, &ma, NULL, ml[i]));
      CHECK(vkBindBufferMemory(dev, *bl[i], *ml[i], 0));
   }
   uint8_t *sp;
   CHECK(vkMapMemory(dev, stmem, 0, VK_WHOLE_SIZE, 0, (void **)&sp));
   memcpy(sp, verts, sizeof(verts));
   memcpy(sp + 512, idx, sizeof(idx));

   /* two uniform buffers (one per frame in flight), one descriptor set each */
   enum { FRAMES = 2 };
   VkBuffer ubuf[FRAMES];
   VkDeviceMemory umem[FRAMES];
   float *uptr[FRAMES];
   VkDescriptorSetLayoutBinding dlb = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutCreateInfo dlci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &dlb};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl));
   VkDescriptorPoolSize dps = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES};
   VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = FRAMES, .poolSizeCount = 1, .pPoolSizes = &dps};
   VkDescriptorPool dpool;
   CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
   VkDescriptorSet dset[FRAMES];
   for (int f = 0; f < FRAMES; f++) {
      VkBufferCreateInfo ub = b1; ub.size = 256; ub.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      CHECK(vkCreateBuffer(dev, &ub, NULL, &ubuf[f]));
      vkGetBufferMemoryRequirements(dev, ubuf[f], &mr);
      VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
      CHECK(vkAllocateMemory(dev, &ma, NULL, &umem[f]));
      CHECK(vkBindBufferMemory(dev, ubuf[f], umem[f], 0));
      CHECK(vkMapMemory(dev, umem[f], 0, VK_WHOLE_SIZE, 0, (void **)&uptr[f]));
      VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
      CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset[f]));
      VkDescriptorBufferInfo dbi = {ubuf[f], 0, 16};
      VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset[f], .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi};
      vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);
   }

   VkAttachmentDescription at[3] = {
      {.format = cfmt, .samples = VK_SAMPLE_COUNT_4_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {.format = dfmt, .samples = VK_SAMPLE_COUNT_4_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
      {.format = cfmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}};
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}, rref = {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref, .pResolveAttachments = &rref, .pDepthStencilAttachment = &dref};
   VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 3, .pAttachments = at, .subpassCount = 1, .pSubpasses = &sub};
   VkRenderPass rp;
   CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
   VkImageView fbv[3] = {im[0].view, im[1].view, im[2].view};
   VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 3, .pAttachments = fbv, .width = W, .height = H, .layers = 1};
   VkFramebuffer fb;
   CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fb));

   VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
   VkShaderModule vs, fs;
   smi.codeSize = cube_vert_spv_len; smi.pCode = (const uint32_t *)cube_vert_spv;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &vs));
   smi.codeSize = cube_frag_spv_len; smi.pCode = (const uint32_t *)cube_frag_spv;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs));
   VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
   VkVertexInputBindingDescription vib = {0, 24, VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription via[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}};
   VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vib,
                                              .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = via};
   VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
   VkRect2D sc = {{0, 0}, {W, H}};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_4_BIT};
   VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
   VkGraphicsPipelineCreateInfo gpi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb, .layout = pl,
                                       .renderPass = rp, .subpass = 0};
   VkPipeline pipe;
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));

   VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = (uint32_t)fam};
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FRAMES + 1};
   VkCommandBuffer cmds[FRAMES + 1];
   CHECK(vkAllocateCommandBuffers(dev, &cbai, cmds));
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence[FRAMES + 1];
   for (int i = 0; i < FRAMES + 1; i++) CHECK(vkCreateFence(dev, &fi, NULL, &fence[i]));

   /* the buffers' contents, once */
   VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(vkBeginCommandBuffer(cmds[FRAMES], &cbbi));
   VkBufferCopy c1 = {0, 0, sizeof(verts)}, c2 = {512, 0, sizeof(idx)};
   vkCmdCopyBuffer(cmds[FRAMES], stage, vbuf, 1, &c1);
   vkCmdCopyBuffer(cmds[FRAMES], stage, ibuf, 1, &c2);
   CHECK(vkEndCommandBuffer(cmds[FRAMES]));
   VkSubmitInfo si0 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmds[FRAMES]};
   CHECK(vkQueueSubmit(q, 1, &si0, fence[FRAMES]));
   CHECK(vkWaitForFences(dev, 1, &fence[FRAMES], VK_TRUE, 8000000000ull));
   printf("spin: %ux%u, 4x MSAA + D32, %d fps for %d s, %d frames in flight%s\n", W, H, fps, seconds, flip ? 1 : FRAMES, flip ? ", two display surfaces, vsync page flip per frame" : "");
   fflush(stdout);

   mat4 proj, view;
   m_persp(proj, 0.8f, (float)W / (float)H, 0.5f, 20.0f);
   m_trans(view, 0, 0, -5.5f);
   const int total = seconds * fps;
   const double period = 1000.0 / fps, t0 = now_ms();
   double waitMin = 1e9, waitMax = 0, waitSum = 0, lateMax = 0;
   int late = 0, bad = 0, checks = 0, submitted[FRAMES] = {0}, flipBad = 0;
   double flipSum = 0, flipMax = 0;
   for (int fr = 0; fr < total; fr++) {
      const int slot = flip ? 0 : fr % FRAMES;
      const int surf = flip ? 1 + (fr & 1) : 0;                                      /* the surface that is not on the display */
      const double target = t0 + fr * period;
      for (double d = target - now_ms(); d > 0.3; d = target - now_ms()) {      /* pace the loop: sleep to this frame's time */
         struct timespec ts = {0, (long)((d > 1.0 ? d - 0.5 : 0.2) * 1e6)};
         nanosleep(&ts, NULL);
      }
      const double start = now_ms();
      if (start - target > lateMax) lateMax = start - target;
      if (start - target > 4 * period) late++;
      if (submitted[slot]) {                                                   /* this slot's previous frame must be done before its command buffer and uniform buffer are reused */
         const double w0 = now_ms();
         r = vkWaitForFences(dev, 1, &fence[slot], VK_TRUE, 10000000000ull);
         if (r != VK_SUCCESS) { printf("FAIL: frame %d: wait for fence %d\n", fr, (int)r); return 3; }
         const double w = now_ms() - w0;
         waitSum += w; if (w < waitMin) waitMin = w; if (w > waitMax) waitMax = w;
         CHECK(vkResetFences(dev, 1, &fence[slot]));
         if (fr % 30 == 0 && fr > 0) {                                         /* centre: always a cube; corner: always the clear colour */
            void *sc = scans[flip ? 1 + ((fr - 1) & 1) : 0];                          /* the frame that was just shown */
            const uint32_t cpx = ((volatile uint32_t *)((uint8_t *)sc + (size_t)(H / 2) * pitch))[W / 2] & 0xffffffu;
            const uint32_t kpx = ((volatile uint32_t *)((uint8_t *)sc + (size_t)4 * pitch))[4] & 0xffffffu;
            checks++;
            if (cpx == 0x000066u || kpx != 0x000066u) { if (bad < 8) printf("frame %d (%.2f s): centre 0x%06x corner 0x%06x\n", fr, (now_ms() - t0) / 1000.0, cpx, kpx); bad++; }
         }
      }
      const float a = (float)fr / (float)fps;
      uptr[slot][0] = 0.6f + 0.4f * sinf(a * 2.0f); uptr[slot][1] = 0.6f + 0.4f * sinf(a * 2.0f + 2.1f); uptr[slot][2] = 0.6f + 0.4f * sinf(a * 2.0f + 4.2f); uptr[slot][3] = 1.0f;
      VkCommandBuffer cmd = cmds[slot];
      CHECK(vkResetCommandBuffer(cmd, 0));
      CHECK(vkBeginCommandBuffer(cmd, &cbbi));
      VkMemoryBarrier all = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &all, 0, NULL, 0, NULL);   /* the other frame in flight uses the same images */
      VkClearValue cv[3] = {{.color = {{0.0f, 0.0f, 0.4f, 1.0f}}}, {.depthStencil = {1.0f, 0}}, {0}};
      VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 3, .pClearValues = cv};
      vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dset[slot], 0, NULL);
      VkDeviceSize off = 0;
      vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &off);
      vkCmdBindIndexBuffer(cmd, ibuf, 0, VK_INDEX_TYPE_UINT16);
      for (int c = 0; c < 2; c++) {                                            /* the big cube turns in place, the small one orbits it the other way */
         mat4 ry, rx, tr, sc2, model, vp2, mvp;
         m_rot_y(ry, a * (c ? -2.3f : 0.9f)); m_rot_x(rx, a * (c ? 1.1f : 0.6f));
         m_mul(model, ry, rx);
         if (c) { m_scale(sc2, 0.35f); m_mul(model, model, sc2); m_rot_y(ry, a * 1.7f); m_trans(tr, 2.6f, 0.0f, 0.0f); m_mul(tr, ry, tr); m_mul(model, tr, model); }
         m_mul(vp2, proj, view);
         m_mul(mvp, vp2, model);
         vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);
         vkCmdDrawIndexed(cmd, 36, 1, 0, 0, 0);
      }
      vkCmdEndRenderPass(cmd);
      VkBufferImageCopy cp = {.bufferRowLength = pitch / 4, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
      vkCmdCopyImageToBuffer(cmd, im[2].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sbufs[surf], 1, &cp);
      CHECK(vkEndCommandBuffer(cmd));
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
      CHECK(vkQueueSubmit(q, 1, &si, fence[slot]));
      submitted[slot] = 1;
      if (flip) {                                                              /* done rendering: show it, and wait until the display has it before the other surface is drawn into */
         CHECK(vkWaitForFences(dev, 1, &fence[slot], VK_TRUE, 10000000000ull));
         CHECK(vkResetFences(dev, 1, &fence[slot]));
         submitted[slot] = 0;
         const double p0 = now_ms();
         const int pr = present(surf, 1);
         const double pw = now_ms() - p0;
         flipSum += pw; if (pw > flipMax) flipMax = pw;
         if (fr % 30 == 0) {                                                   /* centre: always a cube; corner: always the clear colour */
            const uint32_t cpx = ((volatile uint32_t *)((uint8_t *)scans[surf] + (size_t)(H / 2) * pitch))[W / 2] & 0xffffffu;
            const uint32_t kpx = ((volatile uint32_t *)((uint8_t *)scans[surf] + (size_t)4 * pitch))[4] & 0xffffffu;
            checks++;
            if (cpx == 0x000066u || kpx != 0x000066u) { if (bad < 8) printf("frame %d: centre 0x%06x corner 0x%06x\n", fr, cpx, kpx); bad++; }
         }
         if (pr) { if (flipBad < 5) printf("frame %d: present returned %d after %.1f ms\n", fr, pr, pw); flipBad++; }
      }
      if (fr % (fps * 5) == 0) { printf("frame %d (%.1f s)\n", fr, (now_ms() - t0) / 1000.0); fflush(stdout); }
   }
   for (int i = 0; i < FRAMES; i++) if (submitted[i]) CHECK(vkWaitForFences(dev, 1, &fence[i], VK_TRUE, 10000000000ull));
   const double elapsed = (now_ms() - t0) / 1000.0;
   printf("%d frames in %.2f s = %.2f fps (asked %d); wait for the previous frame of the same slot: min %.2f, mean %.2f, max %.2f ms\n", total, elapsed, total / elapsed, fps, waitMin > 1e8 ? 0.0 : waitMin, waitSum / total, waitMax);
   if (flip) printf("page flips: %d, present wait mean %.2f ms, max %.2f ms, flips not done within 50 ms: %d\n", total, flipSum / total, flipMax, flipBad);
   printf("frames started more than 4 frame times late: %d (worst start delay %.1f ms); pixel checks: %d, wrong: %d\n", late, lateMax, checks, bad);
   if (flip) present(0, 1);                                                  /* give the screen back to the desktop */
   const int ok = !bad && !late && !flipBad;
   puts(ok ? "PASS" : "FAIL");
   return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
   if (argc > 1 && !strcmp(argv[1], "spin"))
      return spin_main(argc - 1, argv + 1);
   if (argc > 1 && !strcmp(argv[1], "scene"))
      return scene_main(argc - 1, argv + 1);
   return triangle_main(argc, argv);
}
