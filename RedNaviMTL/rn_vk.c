// See rn_vk.h. Plain Vulkan 1.1, loaded by hand: the process may be any Metal client, so nothing here links against a Vulkan library.
#include "rn_vk.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define FUNCS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) \
    X(CreateDevice) X(GetDeviceProcAddr) X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceFormatProperties)
#define DFUNCS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateComputePipelines) X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateCommandPool) \
    X(DestroyCommandPool) X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBindPipeline) X(CmdBindDescriptorSets) \
    X(CmdPushConstants) X(CmdDispatch) X(CmdPipelineBarrier) X(CreateFence) X(DestroyFence) X(WaitForFences) X(QueueSubmit) X(ResetDescriptorPool) \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) \
    X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdClearColorImage) X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CreateGraphicsPipelines) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw) X(CmdBindVertexBuffers) X(CmdSetBlendConstants)

struct rn_vk {
    void *lib;
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance inst;
    VkPhysicalDevice pd;
    VkPhysicalDeviceMemoryProperties mp;
    VkDeviceSize storageAlign;
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
struct rn_vk_image { VkImage img; VkImageView view; VkDeviceMemory mem; rn_vk_buffer *stage; VkFormat fmt; uint32_t w, h; unsigned texel; int storage, color, initialized, dirty; };
struct rn_vk_sampler { VkSampler s; };
struct rn_vk_compute {
    VkShaderModule sm; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkDescriptorPool dp; VkDescriptorSet ds;
    unsigned nbind, push, binds[RN_VK_MAX_BINDINGS], kinds[RN_VK_MAX_BINDINGS];
    int dyn;                                  // threads-dynamic kernels: 48 bytes of push constants per region
    VkSampler statics[RN_VK_MAX_BINDINGS];
    int spec;
    struct { uint32_t l[3]; VkPipeline pipe; } variants[16];
    unsigned nvariants;
};

static void seterr(char *err, size_t n, const char *what, long v) { if (err && n) snprintf(err, n, "%s (%ld)", what, v); }

const char *rn_vk_dir(void) {
    const char *d = getenv("RN_VK_DIR");
    return d ? d : "/Library/Application Support/RedNavi/vk";
}

rn_vk *rn_vk_open(const char *dir, char *err, size_t errlen) {
    if (!dir) dir = rn_vk_dir();
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
    vk->storageAlign = props.limits.minStorageBufferOffsetAlignment ? props.limits.minStorageBufferOffsetAlignment : 4;
    vk->GetPhysicalDeviceMemoryProperties(vk->pd, &vk->mp);
    uint32_t nq = 0;
    vk->GetPhysicalDeviceQueueFamilyProperties(vk->pd, &nq, NULL);
    VkQueueFamilyProperties qf[8];
    if (nq > 8) nq = 8;
    vk->GetPhysicalDeviceQueueFamilyProperties(vk->pd, &nq, qf);
    int fam = -1;
    for (uint32_t i = 0; i < nq; i++) if ((qf[i].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) == (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) { fam = (int)i; break; }
    if (fam < 0) { seterr(err, errlen, "no graphics and compute queue family", 0); goto fail; }
    vk->family = (uint32_t)fam;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk->family, .queueCount = 1, .pQueuePriorities = &prio};
    // Storage images are read and written without a format in the translated shaders (a Metal texture's format is the app's choice).
    VkPhysicalDeviceFeatures have, want;
    vk->GetPhysicalDeviceFeatures(vk->pd, &have);
    memset(&want, 0, sizeof(want));
    want.shaderStorageImageReadWithoutFormat = have.shaderStorageImageReadWithoutFormat;
    want.shaderStorageImageWriteWithoutFormat = have.shaderStorageImageWriteWithoutFormat;
    want.shaderInt64 = have.shaderInt64;                        // the translated vertex shaders declare Int64
    want.vertexPipelineStoresAndAtomics = have.vertexPipelineStoresAndAtomics;
    want.fragmentStoresAndAtomics = have.fragmentStoresAndAtomics;
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &want};
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
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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

static VkSamplerCreateInfo sampler_info(const rn_vk_sampler_desc *d) {
    static const VkSamplerAddressMode am[5] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                               VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.minFilter = d->min_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.magFilter = d->mag_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = d->mip_filter == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    for (int i = 0; i < 3; i++) (&si.addressModeU)[i] = am[d->address[i] < 5 ? d->address[i] : 1];
    // Metal's clamp_to_zero is a border of transparent black
    si.borderColor = d->border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : d->border == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    for (int i = 0; i < 3; i++) if (d->address[i] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000.0f ? 1000.0f : d->lod_max;
    if (d->pixel_coords) {                      // what Vulkan allows of unnormalized coordinates: one level, no repeat, same filter for magnification and minification
        si.unnormalizedCoordinates = VK_TRUE;
        si.minFilter = si.magFilter;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.minLod = si.maxLod = 0.0f;
        for (int i = 0; i < 3; i++) if ((&si.addressModeU)[i] != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) (&si.addressModeU)[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
    return si;
}

rn_vk_sampler *rn_vk_sampler_create(rn_vk *vk, const rn_vk_sampler_desc *d) {
    rn_vk_sampler *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    VkSamplerCreateInfo si = sampler_info(d);
    if (vk->CreateSampler(vk->dev, &si, NULL, &s->s) != VK_SUCCESS) { free(s); return NULL; }
    return s;
}
void rn_vk_sampler_destroy(rn_vk *vk, rn_vk_sampler *s) {
    if (!s) return;
    if (s->s) vk->DestroySampler(vk->dev, s->s, NULL);
    free(s);
}

static VkDescriptorType desc_type(unsigned kind) {
    switch (kind) {
    case RN_VK_SAMPLED_IMAGE: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case RN_VK_STORAGE_IMAGE: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case RN_VK_SAMPLER: case RN_VK_STATIC_SAMPLER: return VK_DESCRIPTOR_TYPE_SAMPLER;
    default: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
}

rn_vk_compute *rn_vk_compute_create(rn_vk *vk, const uint32_t *spirv, size_t bytes, unsigned nbind, const rn_vk_binding *binds, unsigned push_bytes, int specialize_local_size) {
    if (nbind == 0 || nbind > RN_VK_MAX_BINDINGS) return NULL;
    rn_vk_compute *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->nbind = nbind; c->push = push_bytes; c->spec = specialize_local_size != 0; c->dyn = specialize_local_size == 2;
    VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = spirv};
    if (vk->CreateShaderModule(vk->dev, &smi, NULL, &c->sm) != VK_SUCCESS) goto fail;
    VkDescriptorSetLayoutBinding b[RN_VK_MAX_BINDINGS];
    VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0}, {VK_DESCRIPTOR_TYPE_SAMPLER, 0}};
    for (unsigned i = 0; i < nbind; i++) {
        c->binds[i] = binds[i].binding;
        c->kinds[i] = binds[i].kind;
        const VkDescriptorType t = desc_type(binds[i].kind);
        b[i] = (VkDescriptorSetLayoutBinding){.binding = binds[i].binding, .descriptorType = t, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        for (int k = 0; k < 4; k++) if (ps[k].type == t) ps[k].descriptorCount++;
        if (binds[i].kind == RN_VK_STATIC_SAMPLER) {
            if (!binds[i].sampler) goto fail;
            VkSamplerCreateInfo si = sampler_info(binds[i].sampler);
            if (vk->CreateSampler(vk->dev, &si, NULL, &c->statics[i]) != VK_SUCCESS) goto fail;
        }
    }
    VkDescriptorSetLayoutCreateInfo dsl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nbind, .pBindings = b};
    if (vk->CreateDescriptorSetLayout(vk->dev, &dsl, NULL, &c->dsl) != VK_SUCCESS) goto fail;
    VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = push_bytes};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &c->dsl, .pushConstantRangeCount = push_bytes ? 1 : 0, .pPushConstantRanges = &pcr};
    if (vk->CreatePipelineLayout(vk->dev, &plci, NULL, &c->pl) != VK_SUCCESS) goto fail;
    VkDescriptorPoolSize used[4];
    uint32_t nps = 0;
    for (int k = 0; k < 4; k++) if (ps[k].descriptorCount) used[nps++] = ps[k];
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = nps, .pPoolSizes = used};
    if (vk->CreateDescriptorPool(vk->dev, &dpi, NULL, &c->dp) != VK_SUCCESS) goto fail;
    VkDescriptorSetAllocateInfo dsa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = c->dp, .descriptorSetCount = 1, .pSetLayouts = &c->dsl};
    if (vk->AllocateDescriptorSets(vk->dev, &dsa, &c->ds) != VK_SUCCESS) goto fail;
    // the pipeline of the module's own local size is built now so that a module RADV refuses is found at creation, not at the first dispatch
    uint32_t own[3] = {1, 1, 1};
    if (!c->spec) {
        VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                           .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = c->sm, .pName = "main"},
                                           .layout = c->pl};
        if (vk->CreateComputePipelines(vk->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &c->variants[0].pipe) != VK_SUCCESS) goto fail;
        memcpy(c->variants[0].l, own, sizeof(own));
        c->nvariants = 1;
    }
    return c;
fail:
    rn_vk_compute_destroy(vk, c);
    return NULL;
}
unsigned rn_vk_compute_binding_count(const rn_vk_compute *c) { return c->nbind; }

// The pipeline for a local size (specialization constants 0, 1, 2), made on first use.
static VkPipeline pipeline_for(rn_vk *vk, rn_vk_compute *c, const uint32_t l[3]) {
    for (unsigned i = 0; i < c->nvariants; i++) {
        if (!c->spec) return c->variants[0].pipe;
        if (!memcmp(c->variants[i].l, l, 12)) return c->variants[i].pipe;
    }
    if (c->nvariants >= 16) return VK_NULL_HANDLE;
    VkSpecializationMapEntry entries[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    uint32_t data[3] = {l[0], l[1], l[2]};
    VkSpecializationInfo si = {.mapEntryCount = 3, .pMapEntries = entries, .dataSize = sizeof(data), .pData = data};
    VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = c->sm, .pName = "main", .pSpecializationInfo = &si},
                                       .layout = c->pl};
    VkPipeline p;
    if (vk->CreateComputePipelines(vk->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    memcpy(c->variants[c->nvariants].l, l, 12);
    c->variants[c->nvariants].pipe = p;
    c->nvariants++;
    return p;
}

void rn_vk_compute_destroy(rn_vk *vk, rn_vk_compute *c) {
    if (!c) return;
    for (unsigned i = 0; i < c->nbind; i++) if (c->statics[i]) vk->DestroySampler(vk->dev, c->statics[i], NULL);
    if (c->dp) vk->DestroyDescriptorPool(vk->dev, c->dp, NULL);
    for (unsigned i = 0; i < c->nvariants; i++) if (c->variants[i].pipe) vk->DestroyPipeline(vk->dev, c->variants[i].pipe, NULL);
    if (c->pl) vk->DestroyPipelineLayout(vk->dev, c->pl, NULL);
    if (c->dsl) vk->DestroyDescriptorSetLayout(vk->dev, c->dsl, NULL);
    if (c->sm) vk->DestroyShaderModule(vk->dev, c->sm, NULL);
    free(c);
}

uint64_t rn_vk_storage_offset_alignment(const rn_vk *vk) { return vk->storageAlign; }

// ---------------------------------------------------------------------------------------------------------------------------------------- images

unsigned rn_vk_format_texel_bytes(int f) {
    switch (f) {
    case RN_VK_R8: return 1;
    case RN_VK_RG8: case RN_VK_R16F: return 2;
    case RN_VK_RGBA8: case RN_VK_BGRA8: case RN_VK_R32F: case RN_VK_R32U: case RN_VK_R32I: case RN_VK_RGBA8U: return 4;
    case RN_VK_RGBA16F: case RN_VK_RG32F: return 8;
    case RN_VK_RGBA32F: return 16;
    default: return 0;
    }
}
static VkFormat vk_format(int f) {
    switch (f) {
    case RN_VK_RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case RN_VK_BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    case RN_VK_R8: return VK_FORMAT_R8_UNORM;
    case RN_VK_RG8: return VK_FORMAT_R8G8_UNORM;
    case RN_VK_R16F: return VK_FORMAT_R16_SFLOAT;
    case RN_VK_RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RN_VK_R32F: return VK_FORMAT_R32_SFLOAT;
    case RN_VK_RG32F: return VK_FORMAT_R32G32_SFLOAT;
    case RN_VK_RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RN_VK_R32U: return VK_FORMAT_R32_UINT;
    case RN_VK_R32I: return VK_FORMAT_R32_SINT;
    case RN_VK_RGBA8U: return VK_FORMAT_R8G8B8A8_UINT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

rn_vk_image *rn_vk_image_create(rn_vk *vk, int format, uint32_t w, uint32_t h) {
    const unsigned texel = rn_vk_format_texel_bytes(format);
    if (!texel || !w || !h) return NULL;
    rn_vk_image *im = calloc(1, sizeof(*im));
    if (!im) return NULL;
    im->fmt = vk_format(format); im->w = w; im->h = h; im->texel = texel;
    VkFormatProperties fp;
    vk->GetPhysicalDeviceFormatProperties(vk->pd, im->fmt, &fp);
    const VkFormatFeatureFlags f = fp.optimalTilingFeatures;
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((f & need) != need) goto fail;
    im->storage = (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    im->color = (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = im->fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (im->storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0) | (im->color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0)};
    if (vk->CreateImage(vk->dev, &ici, NULL, &im->img) != VK_SUCCESS) goto fail;
    VkMemoryRequirements mr;
    vk->GetImageMemoryRequirements(vk->dev, im->img, &mr);
    int mt = -1;
    for (uint32_t i = 0; i < vk->mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (vk->mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = (int)i; break; }
    if (mt < 0) goto fail;
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt};
    if (vk->AllocateMemory(vk->dev, &mai, NULL, &im->mem) != VK_SUCCESS) goto fail;
    if (vk->BindImageMemory(vk->dev, im->img, im->mem, 0) != VK_SUCCESS) goto fail;
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = im->fmt,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vk->CreateImageView(vk->dev, &vci, NULL, &im->view) != VK_SUCCESS) goto fail;
    im->stage = rn_vk_buffer_create(vk, (size_t)w * h * texel);
    if (!im->stage) goto fail;
    memset(rn_vk_buffer_contents(im->stage), 0, (size_t)w * h * texel);
    im->dirty = 1;                              // the zeroes reach the image at its first use
    return im;
fail:
    rn_vk_image_destroy(vk, im);
    return NULL;
}
void *rn_vk_image_staging(rn_vk_image *im) { return rn_vk_buffer_contents(im->stage); }
void rn_vk_image_host_wrote(rn_vk_image *im) { im->dirty = 1; }
void rn_vk_image_destroy(rn_vk *vk, rn_vk_image *im) {
    if (!im) return;
    if (im->view) vk->DestroyImageView(vk->dev, im->view, NULL);
    if (im->img) vk->DestroyImage(vk->dev, im->img, NULL);
    if (im->mem) vk->FreeMemory(vk->dev, im->mem, NULL);
    rn_vk_buffer_destroy(vk, im->stage);
    free(im);
}

typedef struct { uint32_t local[3], groups[3], pc[12]; } region;

// Fills the descriptor writes of one set from the resources of a dispatch or a draw. Every image goes into `in` (it must be current on the card before the work), the ones the shader
// may write into `out` (copied back afterwards). 0, or 4 (buffer offset) or 6 (resource missing or unusable).
static int fill_writes(rn_vk *vk, unsigned n, const unsigned *kinds, const unsigned *binds, const VkSampler *statics, const rn_vk_arg *args, VkDescriptorSet set, VkDescriptorBufferInfo *bi,
                       VkDescriptorImageInfo *ii, VkWriteDescriptorSet *w, rn_vk_image **in, unsigned *nin, rn_vk_image **out, unsigned *nout) {
    for (unsigned i = 0; i < n; i++) {
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = binds[i], .descriptorCount = 1, .descriptorType = desc_type(kinds[i])};
        const rn_vk_arg *a = &args[i];
        switch (kinds[i]) {
        case RN_VK_BUFFER:
            if (!a->buffer) return 6;
            if (a->offset % vk->storageAlign || a->offset >= a->buffer->size) return 4;
            bi[i] = (VkDescriptorBufferInfo){.buffer = a->buffer->buf, .offset = a->offset, .range = a->buffer->size - a->offset};
            w[i].pBufferInfo = &bi[i];
            break;
        case RN_VK_SAMPLED_IMAGE:
        case RN_VK_STORAGE_IMAGE:
            if (!a->image || (kinds[i] == RN_VK_STORAGE_IMAGE && !a->image->storage)) return 6;
            ii[i] = (VkDescriptorImageInfo){.imageView = a->image->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
            w[i].pImageInfo = &ii[i];
            in[(*nin)++] = a->image;
            if (kinds[i] == RN_VK_STORAGE_IMAGE) out[(*nout)++] = a->image;
            break;
        case RN_VK_SAMPLER:
            if (!a->sampler) return 6;
            ii[i] = (VkDescriptorImageInfo){.sampler = a->sampler->s};
            w[i].pImageInfo = &ii[i];
            break;
        default:
            ii[i] = (VkDescriptorImageInfo){.sampler = statics[i]};
            w[i].pImageInfo = &ii[i];
            break;
        }
    }
    return 0;
}

// The images of a submission: the first use takes an image out of UNDEFINED (its layout stays GENERAL, which sampling, storage and attachment use all accept) and what the host wrote goes in.
static void images_in(rn_vk *vk, VkCommandBuffer cb, rn_vk_image *const *im, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        int seen = 0;
        for (unsigned j = 0; j < i; j++) seen |= im[j] == im[i];
        if (seen) continue;
        if (!im[i]->initialized) {
            VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                       .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i]->img,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
            im[i]->initialized = 1;
        }
        if (im[i]->dirty) {
            VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {im[i]->w, im[i]->h, 1}};
            vk->CmdCopyBufferToImage(cb, im[i]->stage->buf, im[i]->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
            im[i]->dirty = 0;
        }
    }
}
// After the work: the images the shaders wrote are copied to their host copies. The caller has made the writes visible to the transfer stage.
static void images_out(rn_vk *vk, VkCommandBuffer cb, rn_vk_image *const *im, unsigned n) {
    unsigned done = 0;
    for (unsigned i = 0; i < n; i++) {
        int seen = 0;
        for (unsigned j = 0; j < i; j++) seen |= im[j] == im[i];
        if (seen) continue;
        VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {im[i]->w, im[i]->h, 1}};
        vk->CmdCopyImageToBuffer(cb, im[i]->img, VK_IMAGE_LAYOUT_GENERAL, im[i]->stage->buf, 1, &bc);
        done++;
    }
    if (done) {
        VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    }
}

static int dispatch_regions(rn_vk *vk, rn_vk_compute *c, const rn_vk_arg *args, const void *push, const region *regs, unsigned nregs) {
    VkDescriptorBufferInfo bi[RN_VK_MAX_BINDINGS];
    VkDescriptorImageInfo ii[RN_VK_MAX_BINDINGS];
    VkWriteDescriptorSet w[RN_VK_MAX_BINDINGS];
    rn_vk_image *up[RN_VK_MAX_BINDINGS], *down[RN_VK_MAX_BINDINGS];
    unsigned nup = 0, ndown = 0;
    int fr = fill_writes(vk, c->nbind, c->kinds, c->binds, c->statics, args, c->ds, bi, ii, w, up, &nup, down, &ndown);
    if (fr) return fr;
    VkPipeline pipes[8];
    if (nregs > 8) return 5;
    for (unsigned i = 0; i < nregs; i++) if (!(pipes[i] = pipeline_for(vk, c, regs[i].local))) return 5;
    vk->UpdateDescriptorSets(vk->dev, c->nbind, w, 0, NULL);
    VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vk->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    if (vk->AllocateCommandBuffers(vk->dev, &cba, &cb) != VK_SUCCESS) return 1;
    VkCommandBufferBeginInfo cbb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    int rc = 2;
    VkFence fence = VK_NULL_HANDLE;
    if (vk->BeginCommandBuffer(cb, &cbb) != VK_SUCCESS) goto done;
    images_in(vk, cb, up, nup);
    VkMemoryBarrier in = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &in, 0, NULL, 0, NULL);
    vk->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pl, 0, 1, &c->ds, 0, NULL);
    for (unsigned i = 0; i < nregs; i++) {
        vk->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[i]);
        if (c->dyn) vk->CmdPushConstants(cb, c->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(regs[i].pc), regs[i].pc);
        else if (c->push && push) vk->CmdPushConstants(cb, c->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, c->push, push);
        vk->CmdDispatch(cb, regs[i].groups[0], regs[i].groups[1], regs[i].groups[2]);
    }
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT};
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    images_out(vk, cb, down, ndown);
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

int rn_vk_dispatch(rn_vk *vk, rn_vk_compute *c, const rn_vk_arg *args, const void *push, const uint32_t groups[3], const uint32_t local[3]) {
    if (c->dyn) return 6;
    region r = {.local = {1, 1, 1}, .groups = {groups[0], groups[1], groups[2]}};
    if (local) memcpy(r.local, local, 12);
    return dispatch_regions(vk, c, args, push, &r, 1);
}

// The grid of threads is split the way the translator's KernelDispatch::plan does it: the full threadgroups of the dimensions, and for every dimension whose size is not a multiple of
// the threadgroup a strip of one smaller threadgroup (up to 8 regions). The shader gets the grid, the region's thread base and threadgroup base and the threadgroup grid.
int rn_vk_dispatch_threads(rn_vk *vk, rn_vk_compute *c, const rn_vk_arg *args, const uint32_t threads[3], const uint32_t tpt[3]) {
    if (!c->dyn || !threads[0] || !threads[1] || !threads[2] || !tpt[0] || !tpt[1] || !tpt[2]) return 6;
    region regs[8];
    unsigned n = 0;
    uint32_t tgg[3];
    for (int d = 0; d < 3; d++) tgg[d] = (threads[d] + tpt[d] - 1) / tpt[d];
    for (unsigned mask = 0; mask < 8; mask++) {
        region r;
        memset(&r, 0, sizeof(r));
        int nonempty = 1;
        uint32_t tbase[3] = {0, 0, 0}, gbase[3] = {0, 0, 0};
        for (int d = 0; d < 3; d++) {
            const uint32_t full = threads[d] / tpt[d], tail = threads[d] % tpt[d];
            r.local[d] = tpt[d];
            if (!(mask & (1u << d))) r.groups[d] = full;
            else if (!tail) nonempty = 0;
            else { r.local[d] = tail; r.groups[d] = 1; tbase[d] = full * tpt[d]; gbase[d] = full; }
            if (!r.groups[d]) nonempty = 0;
        }
        if (!nonempty) continue;
        memcpy(&r.pc[0], threads, 12); memcpy(&r.pc[3], tbase, 12); memcpy(&r.pc[6], gbase, 12); memcpy(&r.pc[9], tgg, 12);
        regs[n++] = r;
    }
    return dispatch_regions(vk, c, args, NULL, regs, n);
}

// ---------------------------------------------------------------------------------------------------------------------------------------- render pipelines and passes

struct rn_vk_render {
    VkShaderModule vs, fs;
    VkDescriptorSetLayout dsl[2];
    VkPipelineLayout pl;
    VkRenderPass rp;
    VkPipeline pipe;
    unsigned n[2], kinds[2][RN_VK_MAX_BINDINGS], binds[2][RN_VK_MAX_BINDINGS];
    VkSampler statics[2][RN_VK_MAX_BINDINGS];
    unsigned nt;
    VkFormat fmts[8];
    unsigned nvl, vlbind[16];                  // the vertex buffer bindings the pipeline reads
};

// A set layout from the bindings of one stage; the descriptors of each type are counted into `count` (buffers, sampled images, storage images, samplers).
static int make_layout(rn_vk *vk, unsigned n, const rn_vk_binding *binds, VkShaderStageFlags stage, unsigned *kinds, unsigned *bn, VkSampler *statics, uint32_t count[4], VkDescriptorSetLayout *out) {
    if (n > RN_VK_MAX_BINDINGS) return 0;
    VkDescriptorSetLayoutBinding b[RN_VK_MAX_BINDINGS];
    for (unsigned i = 0; i < n; i++) {
        kinds[i] = binds[i].kind;
        bn[i] = binds[i].binding;
        const VkDescriptorType t = desc_type(binds[i].kind);
        b[i] = (VkDescriptorSetLayoutBinding){.binding = binds[i].binding, .descriptorType = t, .descriptorCount = 1, .stageFlags = stage};
        count[t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? 0 : t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? 1 : t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? 2 : 3]++;
        if (binds[i].kind == RN_VK_STATIC_SAMPLER) {
            if (!binds[i].sampler) return 0;
            VkSamplerCreateInfo si = sampler_info(binds[i].sampler);
            if (vk->CreateSampler(vk->dev, &si, NULL, &statics[i]) != VK_SUCCESS) return 0;
        }
    }
    VkDescriptorSetLayoutCreateInfo dsl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = n ? b : NULL};
    return vk->CreateDescriptorSetLayout(vk->dev, &dsl, NULL, out) == VK_SUCCESS;
}

static VkFormat render_vkformat(int f) { return vk_format(f); }

rn_vk_render *rn_vk_render_create(rn_vk *vk, const rn_vk_render_desc *d) {
    if (!d->ntargets || d->ntargets > 8) return NULL;
    rn_vk_render *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->nt = d->ntargets;
    uint32_t count[4] = {0, 0, 0, 0};
    VkShaderModuleCreateInfo sm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = d->vs_bytes, .pCode = d->vs};
    if (vk->CreateShaderModule(vk->dev, &sm, NULL, &r->vs) != VK_SUCCESS) goto fail;
    sm.codeSize = d->fs_bytes; sm.pCode = d->fs;
    if (vk->CreateShaderModule(vk->dev, &sm, NULL, &r->fs) != VK_SUCCESS) goto fail;
    r->n[0] = d->nvbind; r->n[1] = d->nfbind;
    if (!make_layout(vk, d->nvbind, d->vbinds, VK_SHADER_STAGE_VERTEX_BIT, r->kinds[0], r->binds[0], r->statics[0], count, &r->dsl[0])) goto fail;
    if (!make_layout(vk, d->nfbind, d->fbinds, VK_SHADER_STAGE_FRAGMENT_BIT, r->kinds[1], r->binds[1], r->statics[1], count, &r->dsl[1])) goto fail;
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = r->dsl};
    if (vk->CreatePipelineLayout(vk->dev, &plci, NULL, &r->pl) != VK_SUCCESS) goto fail;
    // The pass the pipeline is made for: every attachment is loaded and stored and stays in GENERAL (clears are done by the caller before the pass).
    VkAttachmentDescription att[8];
    VkAttachmentReference ref[8];
    VkPipelineColorBlendAttachmentState cba[8];
    for (unsigned i = 0; i < r->nt; i++) {
        r->fmts[i] = render_vkformat(d->target_formats[i]);
        if (r->fmts[i] == VK_FORMAT_UNDEFINED) goto fail;
        att[i] = (VkAttachmentDescription){.format = r->fmts[i], .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                           .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                           .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
        ref[i] = (VkAttachmentReference){i, VK_IMAGE_LAYOUT_GENERAL};
        const rn_vk_blend *bl = &d->blend[i];
        cba[i] = (VkPipelineColorBlendAttachmentState){.blendEnable = bl->enabled ? VK_TRUE : VK_FALSE, .srcColorBlendFactor = bl->src_rgb, .dstColorBlendFactor = bl->dst_rgb, .colorBlendOp = bl->op_rgb,
                                                       .srcAlphaBlendFactor = bl->src_a, .dstAlphaBlendFactor = bl->dst_a, .alphaBlendOp = bl->op_a, .colorWriteMask = bl->write_mask};
    }
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = r->nt, .pColorAttachments = ref};
    VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = r->nt, .pAttachments = att, .subpassCount = 1, .pSubpasses = &sub};
    if (vk->CreateRenderPass(vk->dev, &rpi, NULL, &r->rp) != VK_SUCCESS) goto fail;
    static const VkPrimitiveTopology topo[5] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = r->vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = r->fs, .pName = "main"}};
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    if (d->nvlayout > 16 || d->nvattr > 16) goto fail;
    for (unsigned i = 0; i < d->nvlayout; i++) {
        if (d->vlayouts[i].binding >= 16) goto fail;
        vb[i] = (VkVertexInputBindingDescription){d->vlayouts[i].binding, d->vlayouts[i].stride, d->vlayouts[i].per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        r->vlbind[r->nvl++] = d->vlayouts[i].binding;
    }
    for (unsigned i = 0; i < d->nvattr; i++) va[i] = (VkVertexInputAttributeDescription){d->vattrs[i].location, d->vattrs[i].binding, (VkFormat)d->vattrs[i].format, d->vattrs[i].offset};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = d->nvlayout, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = d->nvattr, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topo[d->topology < 5 ? d->topology : 3]};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = r->nt, .pAttachments = cba};
    const VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 3, .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = r->pl,
                                       .renderPass = r->rp, .subpass = 0};
    if (vk->CreateGraphicsPipelines(vk->dev, VK_NULL_HANDLE, 1, &gp, NULL, &r->pipe) != VK_SUCCESS) goto fail;
    return r;
fail:
    rn_vk_render_destroy(vk, r);
    return NULL;
}

void rn_vk_render_destroy(rn_vk *vk, rn_vk_render *r) {
    if (!r) return;
    if (r->pipe) vk->DestroyPipeline(vk->dev, r->pipe, NULL);
    if (r->rp) vk->DestroyRenderPass(vk->dev, r->rp, NULL);
    if (r->pl) vk->DestroyPipelineLayout(vk->dev, r->pl, NULL);
    for (int s = 0; s < 2; s++) {
        for (unsigned i = 0; i < r->n[s]; i++) if (r->statics[s][i]) vk->DestroySampler(vk->dev, r->statics[s][i], NULL);
        if (r->dsl[s]) vk->DestroyDescriptorSetLayout(vk->dev, r->dsl[s], NULL);
    }
    if (r->vs) vk->DestroyShaderModule(vk->dev, r->vs, NULL);
    if (r->fs) vk->DestroyShaderModule(vk->dev, r->fs, NULL);
    free(r);
}

int rn_vk_render_pass(rn_vk *vk, const rn_vk_target *targets, unsigned nt, const rn_vk_draw *draws, unsigned nd) {
    if (!nt || nt > 8 || !nd) return 6;
    rn_vk_render *p0 = draws[0].pipe;
    for (unsigned i = 0; i < nt; i++) {
        if (!targets[i].image || !targets[i].image->color || targets[i].image->w != targets[0].image->w || targets[i].image->h != targets[0].image->h) return 6;
    }
    for (unsigned d = 0; d < nd; d++) {
        const rn_vk_render *p = draws[d].pipe;
        if (!p || p->nt != nt) return 5;
        for (unsigned i = 0; i < nt; i++) if (p->fmts[i] != targets[i].image->fmt) return 5;
    }
    const uint32_t W = targets[0].image->w, H = targets[0].image->h;
    int rc = 2;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkDescriptorSet *sets = calloc(2 * (size_t)nd, sizeof(*sets));
    rn_vk_image **in = calloc(8 + (size_t)nd * 2 * RN_VK_MAX_BINDINGS, sizeof(*in));
    rn_vk_image **out = calloc(8 + (size_t)nd * 2 * RN_VK_MAX_BINDINGS, sizeof(*out));
    VkWriteDescriptorSet *w = calloc((size_t)nd * 2 * RN_VK_MAX_BINDINGS, sizeof(*w));
    VkDescriptorBufferInfo *bi = calloc((size_t)nd * 2 * RN_VK_MAX_BINDINGS, sizeof(*bi));
    VkDescriptorImageInfo *ii = calloc((size_t)nd * 2 * RN_VK_MAX_BINDINGS, sizeof(*ii));
    if (!sets || !in || !out || !w || !bi || !ii) goto done;
    unsigned nin = 0, nout = 0, nw = 0;
    for (unsigned i = 0; i < nt; i++) { in[nin++] = targets[i].image; out[nout++] = targets[i].image; }
    // descriptor pool for every set of every draw (a set is written when the commands are recorded, so draws must not share one)
    uint32_t cnt[4] = {0, 0, 0, 0};
    for (unsigned d = 0; d < nd; d++)
        for (int s = 0; s < 2; s++)
            for (unsigned i = 0; i < draws[d].pipe->n[s]; i++) {
                const unsigned k = draws[d].pipe->kinds[s][i];
                cnt[k == RN_VK_BUFFER ? 0 : k == RN_VK_SAMPLED_IMAGE ? 1 : k == RN_VK_STORAGE_IMAGE ? 2 : 3]++;
            }
    VkDescriptorPoolSize ps[4];
    static const VkDescriptorType ty[4] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    uint32_t nps = 0;
    for (int k = 0; k < 4; k++) if (cnt[k]) ps[nps++] = (VkDescriptorPoolSize){ty[k], cnt[k]};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2 * nd, .poolSizeCount = nps, .pPoolSizes = nps ? ps : NULL};
    if (vk->CreateDescriptorPool(vk->dev, &dpi, NULL, &pool) != VK_SUCCESS) goto done;
    for (unsigned d = 0; d < nd; d++) {
        for (int s = 0; s < 2; s++) {
            const rn_vk_render *p = draws[d].pipe;
            VkDescriptorSetAllocateInfo dsa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &p->dsl[s]};
            if (vk->AllocateDescriptorSets(vk->dev, &dsa, &sets[2 * d + s]) != VK_SUCCESS) goto done;
            if (!p->n[s]) continue;
            const rn_vk_arg *args = s ? draws[d].fargs : draws[d].vargs;
            if (!args) { rc = 6; goto done; }
            const int fr = fill_writes(vk, p->n[s], p->kinds[s], p->binds[s], p->statics[s], args, sets[2 * d + s], bi + nw, ii + nw, w + nw, in, &nin, out, &nout);
            if (fr) { rc = fr; goto done; }
            nw += p->n[s];
        }
    }
    if (nw) vk->UpdateDescriptorSets(vk->dev, nw, w, 0, NULL);
    VkImageView views[8];
    for (unsigned i = 0; i < nt; i++) views[i] = targets[i].image->view;
    VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = p0->rp, .attachmentCount = nt, .pAttachments = views, .width = W, .height = H, .layers = 1};
    if (vk->CreateFramebuffer(vk->dev, &fbi, NULL, &fb) != VK_SUCCESS) goto done;
    VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vk->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vk->AllocateCommandBuffers(vk->dev, &cba, &cb) != VK_SUCCESS) { cb = VK_NULL_HANDLE; goto done; }
    VkCommandBufferBeginInfo cbb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vk->BeginCommandBuffer(cb, &cbb) != VK_SUCCESS) goto done;
    images_in(vk, cb, in, nin);
    int clears = 0;
    for (unsigned i = 0; i < nt; i++) clears |= targets[i].clear != 0;
    VkMemoryBarrier tt = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    if (clears) {
        vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &tt, 0, NULL, 0, NULL);
        for (unsigned i = 0; i < nt; i++) {
            if (!targets[i].clear) continue;
            VkClearColorValue cc;
            memcpy(cc.float32, targets[i].color, sizeof(cc.float32));
            VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vk->CmdClearColorImage(cb, targets[i].image->img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rg);
        }
    }
    VkMemoryBarrier toGfx = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, 1, &toGfx, 0, NULL, 0, NULL);
    VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = p0->rp, .framebuffer = fb, .renderArea = {{0, 0}, {W, H}}};
    vk->CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    for (unsigned d = 0; d < nd; d++) {
        const rn_vk_draw *dr = &draws[d];
        vk->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, dr->pipe->pipe);
        vk->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, dr->pipe->pl, 0, 2, &sets[2 * d], 0, NULL);
        // Metal's clip space is y-up, Vulkan's y-down: the viewport is flipped (origin at the bottom, negative height)
        const double *v = dr->viewport;
        VkViewport vp = dr->has_viewport ? (VkViewport){(float)v[0], (float)(v[1] + v[3]), (float)v[2], (float)-v[3], (float)v[4], (float)v[5]} : (VkViewport){0, (float)H, (float)W, -(float)H, 0.0f, 1.0f};
        VkRect2D sc = dr->has_scissor ? (VkRect2D){{(int32_t)dr->scissor[0], (int32_t)dr->scissor[1]}, {dr->scissor[2], dr->scissor[3]}} : (VkRect2D){{0, 0}, {W, H}};
        vk->CmdSetViewport(cb, 0, 1, &vp);
        vk->CmdSetScissor(cb, 0, 1, &sc);
        vk->CmdSetBlendConstants(cb, dr->has_blend_color ? dr->blend_color : (const float[4]){0, 0, 0, 0});
        for (unsigned i = 0; i < dr->pipe->nvl; i++) {
            const rn_vk_vbuf *vbf = dr->vbufs ? &dr->vbufs[dr->pipe->vlbind[i]] : NULL;
            if (!vbf || !vbf->buffer || vbf->offset >= vbf->buffer->size) { rc = 6; goto done; }
            VkBuffer vbuf = vbf->buffer->buf;
            VkDeviceSize voff = vbf->offset;
            vk->CmdBindVertexBuffers(cb, dr->pipe->vlbind[i], 1, &vbuf, &voff);
        }
        vk->CmdDraw(cb, dr->vertex_count, dr->instance_count ? dr->instance_count : 1, dr->first_vertex, dr->first_instance);
    }
    vk->CmdEndRenderPass(cb);
    VkMemoryBarrier fromGfx = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT};
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fromGfx, 0, NULL, 0, NULL);
    images_out(vk, cb, out, nout);
    if (vk->EndCommandBuffer(cb) != VK_SUCCESS) goto done;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vk->CreateFence(vk->dev, &fci, NULL, &fence) != VK_SUCCESS) goto done;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    if (vk->QueueSubmit(vk->queue, 1, &si, fence) != VK_SUCCESS) goto done;
    rc = vk->WaitForFences(vk->dev, 1, &fence, VK_TRUE, 8000000000ull) == VK_SUCCESS ? 0 : 3;
done:
    if (fence) vk->DestroyFence(vk->dev, fence, NULL);
    if (cb) vk->FreeCommandBuffers(vk->dev, vk->pool, 1, &cb);
    if (fb) vk->DestroyFramebuffer(vk->dev, fb, NULL);
    if (pool) vk->DestroyDescriptorPool(vk->dev, pool, NULL);
    free(sets); free(in); free(out); free(w); free(bi); free(ii);
    return rc;
}
