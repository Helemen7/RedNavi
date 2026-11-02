/*
 * A Vulkan program that presents the standard way: VK_KHR_swapchain on a VK_EXT_headless_surface (on this driver the headless surface is the display), vkAcquireNextImageKHR,
 * render, vkQueuePresentKHR. Nothing here knows about the RedNavi kext or its bridge. Two cubes (vertex and index buffers, MVP in push constants, a uniform buffer rewritten every
 * frame) are drawn with 4x MSAA and a depth buffer, the multisampled image is resolved straight into the swapchain image, and the swapchain presents with a page flip at the
 * vertical blank (FIFO).
 *   clang -O1 -arch x86_64 -mmacosx-version-min=12 -I/opt/local/include test/vk_swapchain.c -L/opt/local/lib -lvulkan -o vk_swapchain
 * Usage: vk_swapchain [seconds [recreate]]. With "recreate" the swapchain is destroyed and made again every second (the oldSwapchain path and the teardown).
 * Exit status 0 when every frame was acquired and presented without an error; what is on the monitor is for the eye (the swapchain images are not readable from here).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "vk_cube_spv.h"
#include "vk_mat.h"

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

#define MAXIMG 4
static VkDevice dev;
static VkPhysicalDevice pd;
static VkSurfaceKHR surf;
static VkRenderPass rp;
static VkSwapchainKHR sc;
static uint32_t W, H, nimg;
static VkImage simg[MAXIMG];
static VkImageView sview[MAXIMG];
static VkFramebuffer sfb[MAXIMG];
static VkImage cimg, dimg;
static VkDeviceMemory cmem, dmem;
static VkImageView cview, dview;
static const VkFormat cfmt = VK_FORMAT_B8G8R8A8_UNORM, dfmt = VK_FORMAT_D32_SFLOAT;

static void make_target(VkImage *img, VkDeviceMemory *mem, VkImageView *view, VkFormat f, VkImageUsageFlags u, VkImageAspectFlags a)
{
   VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_4_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   CHECK(vkCreateImage(dev, &ic, NULL, img));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, *img, &mr);
   VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   CHECK(vkAllocateMemory(dev, &ma, NULL, mem));
   CHECK(vkBindImageMemory(dev, *img, *mem, 0));
   VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f, .subresourceRange = {a, 0, 1, 0, 1}};
   CHECK(vkCreateImageView(dev, &vc, NULL, view));
}

static void make_swapchain(VkSwapchainKHR old)
{
   VkSurfaceCapabilitiesKHR caps;
   CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
   W = caps.currentExtent.width; H = caps.currentExtent.height;
   VkSwapchainCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = caps.minImageCount, .imageFormat = cfmt,
                                   .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = caps.currentExtent, .imageArrayLayers = 1,
                                   .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                   .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE, .oldSwapchain = old};
   CHECK(vkCreateSwapchainKHR(dev, &sci, NULL, &sc));
   nimg = MAXIMG;
   CHECK(vkGetSwapchainImagesKHR(dev, sc, &nimg, simg));
   make_target(&cimg, &cmem, &cview, cfmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
   make_target(&dimg, &dmem, &dview, dfmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
   for (uint32_t i = 0; i < nimg; i++) {
      VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = simg[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = cfmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      CHECK(vkCreateImageView(dev, &vc, NULL, &sview[i]));
      VkImageView v[3] = {cview, dview, sview[i]};
      VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 3, .pAttachments = v, .width = W, .height = H, .layers = 1};
      CHECK(vkCreateFramebuffer(dev, &fci, NULL, &sfb[i]));
   }
}

static void destroy_swapchain_objects(void)
{
   CHECK(vkDeviceWaitIdle(dev));
   for (uint32_t i = 0; i < nimg; i++) { vkDestroyFramebuffer(dev, sfb[i], NULL); vkDestroyImageView(dev, sview[i], NULL); }
   vkDestroyImageView(dev, cview, NULL); vkDestroyImage(dev, cimg, NULL); vkFreeMemory(dev, cmem, NULL);
   vkDestroyImageView(dev, dview, NULL); vkDestroyImage(dev, dimg, NULL); vkFreeMemory(dev, dmem, NULL);
}

int main(int argc, char **argv)
{
   const int seconds = argc > 1 ? atoi(argv[1]) : 20;
   const int recreate = argc > 2 && !strcmp(argv[2], "recreate");
   const char *iext[] = {"VK_KHR_surface", "VK_EXT_headless_surface"};
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "rn_vk_swapchain", .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext};
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n < 1) { fprintf(stderr, "no device\n"); return 2; }
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   VkHeadlessSurfaceCreateInfoEXT hsi = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
   PFN_vkCreateHeadlessSurfaceEXT createHeadless = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(inst, "vkCreateHeadlessSurfaceEXT");
   CHECK(createHeadless(inst, &hsi, NULL, &surf));
   uint32_t nq = 8;
   VkQueueFamilyProperties qf[8];
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   int fam = -1;
   for (uint32_t i = 0; i < nq; i++) {
      VkBool32 sup = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &sup);
      if (fam < 0 && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && sup) fam = (int)i;
   }
   if (fam < 0) { fprintf(stderr, "no graphics queue family that can present\n"); return 2; }
   uint32_t nf = 8;
   VkSurfaceFormatKHR fmts[8];
   r = vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fmts);
   printf("surface formats (%d):", (int)nf);
   for (uint32_t i = 0; i < nf; i++) printf(" %d", (int)fmts[i].format);
   uint32_t npm = 4;
   VkPresentModeKHR pm[4];
   vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &npm, pm);
   printf("; present modes (%u):", npm);
   for (uint32_t i = 0; i < npm; i++) printf(" %d", (int)pm[i]);
   VkSurfaceCapabilitiesKHR caps;
   CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
   printf("; extent %ux%u, images %u..%u\n", caps.currentExtent.width, caps.currentExtent.height, caps.minImageCount, caps.maxImageCount);
   if (caps.currentExtent.width == 0xffffffffu) { fprintf(stderr, "the surface has no display behind it (needs rednavi_fb=1 and two display surfaces)\n"); return 2; }

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = (uint32_t)fam, .queueCount = 1, .pQueuePriorities = &prio};
   const char *dext[] = {"VK_KHR_swapchain"};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext};
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, (uint32_t)fam, 0, &q);

   /* render pass: 4x colour and depth (not stored), resolved into the swapchain image, which ends in the present layout */
   VkAttachmentDescription at[3] = {
      {.format = cfmt, .samples = VK_SAMPLE_COUNT_4_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {.format = dfmt, .samples = VK_SAMPLE_COUNT_4_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
      {.format = cfmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR}};
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}, rref = {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref, .pResolveAttachments = &rref, .pDepthStencilAttachment = &dref};
   VkSubpassDependency dep = {.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
   VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 3, .pAttachments = at, .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep};
   CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
   make_swapchain(VK_NULL_HANDLE);
   printf("swapchain: %ux%u, %u images\n", W, H, nimg);

   /* geometry, in device-local buffers filled through a staging buffer */
   float verts[8 * 6];
   for (int i = 0; i < 8; i++) {
      const float x = (i & 1) ? 1.f : -1.f, y = (i & 2) ? 1.f : -1.f, z = (i & 4) ? 1.f : -1.f;
      float *v = &verts[i * 6];
      v[0] = x; v[1] = y; v[2] = z; v[3] = (i & 1) ? 1.0f : 0.25f; v[4] = (i & 2) ? 1.0f : 0.25f; v[5] = (i & 4) ? 1.0f : 0.25f;
   }
   const uint16_t idx[36] = {0, 1, 3, 0, 3, 2,  4, 6, 7, 4, 7, 5,  0, 2, 6, 0, 6, 4,  1, 5, 7, 1, 7, 3,  0, 4, 5, 0, 5, 1,  2, 3, 7, 2, 7, 6};
   VkBuffer vbuf, ibuf, stage, ubuf;
   VkDeviceMemory vmem, imemb, stmem, umem;
   VkBufferCreateInfo b1 = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof(verts), .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBufferCreateInfo b2 = b1; b2.size = sizeof(idx); b2.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VkBufferCreateInfo b3 = b1; b3.size = 4096; b3.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
   VkBufferCreateInfo b4 = b1; b4.size = 256; b4.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
   CHECK(vkCreateBuffer(dev, &b1, NULL, &vbuf)); CHECK(vkCreateBuffer(dev, &b2, NULL, &ibuf)); CHECK(vkCreateBuffer(dev, &b3, NULL, &stage)); CHECK(vkCreateBuffer(dev, &b4, NULL, &ubuf));
   VkBuffer *bl[4] = {&vbuf, &ibuf, &stage, &ubuf};
   VkDeviceMemory *ml[4] = {&vmem, &imemb, &stmem, &umem};
   VkMemoryRequirements mr;
   for (int i = 0; i < 4; i++) {
      vkGetBufferMemoryRequirements(dev, *bl[i], &mr);
      VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = i < 2 ? memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                                                          : memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
      CHECK(vkAllocateMemory(dev, &ma, NULL, ml[i]));
      CHECK(vkBindBufferMemory(dev, *bl[i], *ml[i], 0));
   }
   uint8_t *sp;
   float *up;
   CHECK(vkMapMemory(dev, stmem, 0, VK_WHOLE_SIZE, 0, (void **)&sp));
   CHECK(vkMapMemory(dev, umem, 0, VK_WHOLE_SIZE, 0, (void **)&up));
   memcpy(sp, verts, sizeof(verts));
   memcpy(sp + 512, idx, sizeof(idx));

   VkDescriptorSetLayoutBinding dlb = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutCreateInfo dlci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &dlb};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl));
   VkDescriptorPoolSize dps = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps};
   VkDescriptorPool dpool;
   CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
   VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet dset;
   CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));
   VkDescriptorBufferInfo dbi = {ubuf, 0, 16};
   VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi};
   vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

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
   VkRect2D sc2 = {{0, 0}, {W, H}};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc2};
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
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
   VkCommandBuffer cmds[2];
   CHECK(vkAllocateCommandBuffers(dev, &cbai, cmds));
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
   VkFence fence;
   CHECK(vkCreateFence(dev, &fi, NULL, &fence));
   VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore acq, done;
   CHECK(vkCreateSemaphore(dev, &semi, NULL, &acq));
   CHECK(vkCreateSemaphore(dev, &semi, NULL, &done));

   VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(vkBeginCommandBuffer(cmds[1], &cbbi));
   VkBufferCopy c1 = {0, 0, sizeof(verts)}, c2 = {512, 0, sizeof(idx)};
   vkCmdCopyBuffer(cmds[1], stage, vbuf, 1, &c1);
   vkCmdCopyBuffer(cmds[1], stage, ibuf, 1, &c2);
   CHECK(vkEndCommandBuffer(cmds[1]));
   VkSubmitInfo si0 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmds[1]};
   CHECK(vkResetFences(dev, 1, &fence));
   CHECK(vkQueueSubmit(q, 1, &si0, fence));
   CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 8000000000ull));

   mat4 proj, view;
   m_persp(proj, 0.8f, (float)W / (float)H, 0.5f, 20.0f);
   m_trans(view, 0, 0, -5.5f);
   int frames = 0, bad = 0, lastMsg = -1, presented = 0, recreated = 0;
   double minPresent = 1e9, maxPresent = 0, sumPresent = 0;
   uint32_t lastImage = 0;
   const double t0 = now_ms();
   while (now_ms() - t0 < seconds * 1000.0) {
      if (recreate && (int)((now_ms() - t0) / 1000.0) > recreated) {            /* every second: a new swapchain in place of the old one */
         recreated++;
         destroy_swapchain_objects();
         VkSwapchainKHR old = sc;
         make_swapchain(old);
         vkDestroySwapchainKHR(dev, old, NULL);
         if (recreated <= 2) printf("swapchain recreated (%u images)\n", nimg);
      }
      uint32_t image;
      r = vkAcquireNextImageKHR(dev, sc, 2000000000ull, acq, VK_NULL_HANDLE, &image);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { printf("FAIL: acquire %d at frame %d\n", (int)r, frames); return 3; }
      CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 8000000000ull));
      CHECK(vkResetFences(dev, 1, &fence));
      const float a = (float)frames / 60.0f;
      up[0] = 0.6f + 0.4f * sinf(a * 2.0f); up[1] = 0.6f + 0.4f * sinf(a * 2.0f + 2.1f); up[2] = 0.6f + 0.4f * sinf(a * 2.0f + 4.2f); up[3] = 1.0f;
      VkCommandBuffer cmd = cmds[0];
      CHECK(vkResetCommandBuffer(cmd, 0));
      CHECK(vkBeginCommandBuffer(cmd, &cbbi));
      VkClearValue cv[3] = {{.color = {{0.0f, 0.0f, 0.4f, 1.0f}}}, {.depthStencil = {1.0f, 0}}, {0}};
      VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = sfb[image], .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 3, .pClearValues = cv};
      vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dset, 0, NULL);
      VkDeviceSize off = 0;
      vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &off);
      vkCmdBindIndexBuffer(cmd, ibuf, 0, VK_INDEX_TYPE_UINT16);
      for (int c = 0; c < 2; c++) {
         mat4 ry, rx, tr, sc3, model, vp2, mvp;
         m_rot_y(ry, a * (c ? -2.3f : 0.9f)); m_rot_x(rx, a * (c ? 1.1f : 0.6f));
         m_mul(model, ry, rx);
         if (c) { m_scale(sc3, 0.35f); m_mul(model, model, sc3); m_rot_y(ry, a * 1.7f); m_trans(tr, 2.6f, 0.0f, 0.0f); m_mul(tr, ry, tr); m_mul(model, tr, model); }
         m_mul(vp2, proj, view);
         m_mul(mvp, vp2, model);
         vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);
         vkCmdDrawIndexed(cmd, 36, 1, 0, 0, 0);
      }
      vkCmdEndRenderPass(cmd);
      CHECK(vkEndCommandBuffer(cmd));
      VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acq, .pWaitDstStageMask = &wait, .commandBufferCount = 1, .pCommandBuffers = &cmd,
                         .signalSemaphoreCount = 1, .pSignalSemaphores = &done};
      CHECK(vkQueueSubmit(q, 1, &si, fence));
      VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done, .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &image};
      const double p0 = now_ms();
      r = vkQueuePresentKHR(q, &pi);
      const double pw = now_ms() - p0;
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { printf("FAIL: present %d at frame %d\n", (int)r, frames); return 3; }
      presented++; sumPresent += pw; if (pw < minPresent) minPresent = pw; if (pw > maxPresent) maxPresent = pw;
      lastImage = image;
      frames++;
      if ((int)((now_ms() - t0) / 5000.0) != lastMsg) { lastMsg = (int)((now_ms() - t0) / 5000.0); printf("frame %d (%.1f s), image %u\n", frames, (now_ms() - t0) / 1000.0, image); fflush(stdout); }
   }
   CHECK(vkDeviceWaitIdle(dev));
   const double elapsed = (now_ms() - t0) / 1000.0;
   printf("%d frames in %.2f s = %.2f fps; vkQueuePresentKHR: min %.2f, mean %.2f, max %.2f ms; swapchains made again: %d\n", frames, elapsed, frames / elapsed, minPresent, sumPresent / presented, maxPresent, recreated);
   (void)lastImage; (void)bad;
   puts("PASS");
   return 0;
}
