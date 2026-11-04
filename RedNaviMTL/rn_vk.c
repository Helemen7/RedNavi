// See rn_vk.h. Plain Vulkan 1.1, loaded by hand: the process may be any Metal client, so nothing here links against a Vulkan library.
#include "rn_vk.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define FUNCS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) \
    X(CreateDevice) X(GetDeviceProcAddr)
#define DFUNCS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateComputePipelines) X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateCommandPool) \
    X(DestroyCommandPool) X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBindPipeline) X(CmdBindDescriptorSets) \
    X(CmdPushConstants) X(CmdDispatch) X(CmdPipelineBarrier) X(CreateFence) X(DestroyFence) X(WaitForFences) X(QueueSubmit) X(ResetDescriptorPool)

struct rn_vk {
    void *lib;
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance inst;
    VkPhysicalDevice pd;
    VkPhysicalDeviceMemoryProperties mp;
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkCommandPool pool;
#define X(n) PFN_vk##n n;
    FUNCS(X)
    DFUNCS(X)
#undef X
};

struct rn_vk_buffer { VkBuffer buf; VkDeviceMemory mem; void *map; size_t size; };
struct rn_vk_compute {
    VkShaderModule sm; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe; VkDescriptorPool dp; VkDescriptorSet ds;
    unsigned nbuf, push;
};

static void seterr(char *err, size_t n, const char *what, long v) { if (err && n) snprintf(err, n, "%s (%ld)", what, v); }

rn_vk *rn_vk_open(const char *dir, char *err, size_t errlen) {
    if (!dir) dir = getenv("RN_VK_DIR");
    if (!dir) dir = "/Library/Application Support/RedNavi/vk";
    char path[1024];
    snprintf(path, sizeof(path), "%s/libvulkan_radeon.dylib", dir);
    rn_vk *vk = calloc(1, sizeof(*vk));
    if (!vk) return NULL;
    vk->lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!vk->lib) { if (err) snprintf(err, errlen, "dlopen: %s", dlerror()); free(vk); return NULL; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk->lib, "vk_icdGetInstanceProcAddr");
    typedef VkResult (*negotiate_t)(uint32_t *);
    negotiate_t neg = (negotiate_t)dlsym(vk->lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (!gipa) { seterr(err, errlen, "no vk_icdGetInstanceProcAddr", 0); goto fail; }
    uint32_t ver = 5;
    if (neg) neg(&ver);
    vk->gipa = gipa;

    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "RedNaviMTL", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkResult r = createInstance(&ici, NULL, &vk->inst);
    if (r != VK_SUCCESS) { seterr(err, errlen, "vkCreateInstance", r); goto fail; }
#define X(n) vk->n = (PFN_vk##n)gipa(vk->inst, "vk" #n);
    FUNCS(X)
#undef X
    uint32_t n = 1;
    r = vk->EnumeratePhysicalDevices(vk->inst, &n, &vk->pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n < 1) { seterr(err, errlen, "no physical device", r); goto fail; }
    VkPhysicalDeviceProperties props;
    vk->GetPhysicalDeviceProperties(vk->pd, &props);
    strlcpy(vk->name, props.deviceName, sizeof(vk->name));
    vk->GetPhysicalDeviceMemoryProperties(vk->pd, &vk->mp);
    uint32_t nq = 0;
    vk->GetPhysicalDeviceQueueFamilyProperties(vk->pd, &nq, NULL);
    VkQueueFamilyProperties qf[8];
    if (nq > 8) nq = 8;
    vk->GetPhysicalDeviceQueueFamilyProperties(vk->pd, &nq, qf);
    int fam = -1;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { fam = (int)i; break; }
    if (fam < 0) { seterr(err, errlen, "no compute queue family", 0); goto fail; }
    vk->family = (uint32_t)fam;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk->family, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    r = vk->CreateDevice(vk->pd, &dci, NULL, &vk->dev);
    if (r != VK_SUCCESS) { seterr(err, errlen, "vkCreateDevice", r); goto fail; }
#define X(n) vk->n = (PFN_vk##n)vk->GetDeviceProcAddr(vk->dev, "vk" #n);
    DFUNCS(X)
#undef X
    vk->GetDeviceQueue(vk->dev, vk->family, 0, &vk->queue);
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vk->family};
    r = vk->CreateCommandPool(vk->dev, &cpci, NULL, &vk->pool);
    if (r != VK_SUCCESS) { seterr(err, errlen, "vkCreateCommandPool", r); goto fail; }
    return vk;
fail:
    rn_vk_close(vk);
    return NULL;
}

void rn_vk_close(rn_vk *vk) {
    if (!vk) return;
    if (vk->dev && vk->pool) vk->DestroyCommandPool(vk->dev, vk->pool, NULL);
    if (vk->dev) vk->DestroyDevice(vk->dev, NULL);
    if (vk->inst) vk->DestroyInstance(vk->inst, NULL);
    if (vk->lib) dlclose(vk->lib);
    free(vk);
}

const char *rn_vk_device_name(const rn_vk *vk) { return vk->name; }

rn_vk_buffer *rn_vk_buffer_create(rn_vk *vk, size_t bytes) {
    rn_vk_buffer *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->size = bytes;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vk->CreateBuffer(vk->dev, &bci, NULL, &b->buf) != VK_SUCCESS) goto fail;
    VkMemoryRequirements mr;
    vk->GetBufferMemoryRequirements(vk->dev, b->buf, &mr);
    int mt = -1;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < vk->mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (vk->mp.memoryTypes[i].propertyFlags & want) == want) { mt = (int)i; break; }
    if (mt < 0) goto fail;
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt};
    if (vk->AllocateMemory(vk->dev, &mai, NULL, &b->mem) != VK_SUCCESS) goto fail;
    if (vk->BindBufferMemory(vk->dev, b->buf, b->mem, 0) != VK_SUCCESS) goto fail;
    if (vk->MapMemory(vk->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) goto fail;
    return b;
fail:
    rn_vk_buffer_destroy(vk, b);
    return NULL;
}
void *rn_vk_buffer_contents(rn_vk_buffer *b) { return b->map; }
size_t rn_vk_buffer_length(const rn_vk_buffer *b) { return b->size; }
void rn_vk_buffer_destroy(rn_vk *vk, rn_vk_buffer *b) {
    if (!b) return;
    if (b->buf) vk->DestroyBuffer(vk->dev, b->buf, NULL);
    if (b->mem) vk->FreeMemory(vk->dev, b->mem, NULL);
    free(b);
}

rn_vk_compute *rn_vk_compute_create(rn_vk *vk, const uint32_t *spirv, size_t bytes, unsigned nbuf, unsigned push_bytes) {
    if (nbuf == 0 || nbuf > 16) return NULL;
    rn_vk_compute *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->nbuf = nbuf; c->push = push_bytes;
    VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = spirv};
    if (vk->CreateShaderModule(vk->dev, &smi, NULL, &c->sm) != VK_SUCCESS) goto fail;
    VkDescriptorSetLayoutBinding b[16];
    for (unsigned i = 0; i < nbuf; i++) b[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dsl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nbuf, .pBindings = b};
    if (vk->CreateDescriptorSetLayout(vk->dev, &dsl, NULL, &c->dsl) != VK_SUCCESS) goto fail;
    VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = push_bytes};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &c->dsl, .pushConstantRangeCount = push_bytes ? 1 : 0, .pPushConstantRanges = &pcr};
    if (vk->CreatePipelineLayout(vk->dev, &plci, NULL, &c->pl) != VK_SUCCESS) goto fail;
    VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = c->sm, .pName = "main"},
                                       .layout = c->pl};
    if (vk->CreateComputePipelines(vk->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &c->pipe) != VK_SUCCESS) goto fail;
    VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = nbuf};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    if (vk->CreateDescriptorPool(vk->dev, &dpi, NULL, &c->dp) != VK_SUCCESS) goto fail;
    VkDescriptorSetAllocateInfo dsa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = c->dp, .descriptorSetCount = 1, .pSetLayouts = &c->dsl};
    if (vk->AllocateDescriptorSets(vk->dev, &dsa, &c->ds) != VK_SUCCESS) goto fail;
    return c;
fail:
    rn_vk_compute_destroy(vk, c);
    return NULL;
}
void rn_vk_compute_destroy(rn_vk *vk, rn_vk_compute *c) {
    if (!c) return;
    if (c->dp) vk->DestroyDescriptorPool(vk->dev, c->dp, NULL);
    if (c->pipe) vk->DestroyPipeline(vk->dev, c->pipe, NULL);
    if (c->pl) vk->DestroyPipelineLayout(vk->dev, c->pl, NULL);
    if (c->dsl) vk->DestroyDescriptorSetLayout(vk->dev, c->dsl, NULL);
    if (c->sm) vk->DestroyShaderModule(vk->dev, c->sm, NULL);
    free(c);
}

int rn_vk_dispatch(rn_vk *vk, rn_vk_compute *c, rn_vk_buffer *const *buffers, const void *push, uint32_t gx, uint32_t gy, uint32_t gz) {
    VkDescriptorBufferInfo bi[16];
    VkWriteDescriptorSet w[16];
    for (unsigned i = 0; i < c->nbuf; i++) {
        bi[i] = (VkDescriptorBufferInfo){.buffer = buffers[i]->buf, .offset = 0, .range = VK_WHOLE_SIZE};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds, .dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i]};
    }
    vk->UpdateDescriptorSets(vk->dev, c->nbuf, w, 0, NULL);
    VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vk->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    if (vk->AllocateCommandBuffers(vk->dev, &cba, &cb) != VK_SUCCESS) return 1;
    VkCommandBufferBeginInfo cbb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    int rc = 2;
    VkFence fence = VK_NULL_HANDLE;
    if (vk->BeginCommandBuffer(cb, &cbb) != VK_SUCCESS) goto done;
    vk->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipe);
    vk->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pl, 0, 1, &c->ds, 0, NULL);
    if (c->push && push) vk->CmdPushConstants(cb, c->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, c->push, push);
    vk->CmdDispatch(cb, gx, gy, gz);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT};
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (vk->EndCommandBuffer(cb) != VK_SUCCESS) goto done;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vk->CreateFence(vk->dev, &fci, NULL, &fence) != VK_SUCCESS) goto done;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    if (vk->QueueSubmit(vk->queue, 1, &si, fence) != VK_SUCCESS) goto done;
    rc = vk->WaitForFences(vk->dev, 1, &fence, VK_TRUE, 8000000000ull) == VK_SUCCESS ? 0 : 3;
done:
    if (fence) vk->DestroyFence(vk->dev, fence, NULL);
    vk->FreeCommandBuffers(vk->dev, vk->pool, 1, &cb);
    return rc;
}
