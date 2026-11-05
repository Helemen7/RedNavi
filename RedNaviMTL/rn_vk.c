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
    X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer)

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
struct rn_vk_image { VkImage img; VkImageView view; VkDeviceMemory mem; rn_vk_buffer *stage; VkFormat fmt; uint32_t w, h; unsigned texel; int storage, initialized, dirty; };
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
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { fam = (int)i; break; }
    if (fam < 0) { seterr(err, errlen, "no compute queue family", 0); goto fail; }
    vk->family = (uint32_t)fam;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk->family, .queueCount = 1, .pQueuePriorities = &prio};
    // Storage images are read and written without a format in the translated shaders (a Metal texture's format is the app's choice).
    VkPhysicalDeviceFeatures have, want;
    vk->GetPhysicalDeviceFeatures(vk->pd, &have);
    memset(&want, 0, sizeof(want));
    want.shaderStorageImageReadWithoutFormat = have.shaderStorageImageReadWithoutFormat;
    want.shaderStorageImageWriteWithoutFormat = have.shaderStorageImageWriteWithoutFormat;
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
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = im->fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (im->storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0)};
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

static int dispatch_regions(rn_vk *vk, rn_vk_compute *c, const rn_vk_arg *args, const void *push, const region *regs, unsigned nregs) {
    VkDescriptorBufferInfo bi[RN_VK_MAX_BINDINGS];
    VkDescriptorImageInfo ii[RN_VK_MAX_BINDINGS];
    VkWriteDescriptorSet w[RN_VK_MAX_BINDINGS];
    rn_vk_image *up[RN_VK_MAX_BINDINGS], *down[RN_VK_MAX_BINDINGS];
    unsigned nup = 0, ndown = 0;
    for (unsigned i = 0; i < c->nbind; i++) {
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds, .dstBinding = c->binds[i], .descriptorCount = 1, .descriptorType = desc_type(c->kinds[i])};
        const rn_vk_arg *a = &args[i];
        switch (c->kinds[i]) {
        case RN_VK_BUFFER: {
            if (!a->buffer) return 6;
            if (a->offset % vk->storageAlign || a->offset >= a->buffer->size) return 4;
            bi[i] = (VkDescriptorBufferInfo){.buffer = a->buffer->buf, .offset = a->offset, .range = a->buffer->size - a->offset};
            w[i].pBufferInfo = &bi[i];
            break;
        }
        case RN_VK_SAMPLED_IMAGE:
        case RN_VK_STORAGE_IMAGE:
            if (!a->image || (c->kinds[i] == RN_VK_STORAGE_IMAGE && !a->image->storage)) return 6;
            ii[i] = (VkDescriptorImageInfo){.imageView = a->image->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
            w[i].pImageInfo = &ii[i];
            up[nup++] = a->image;
            if (c->kinds[i] == RN_VK_STORAGE_IMAGE) down[ndown++] = a->image;
            break;
        case RN_VK_SAMPLER:
            if (!a->sampler) return 6;
            ii[i] = (VkDescriptorImageInfo){.sampler = a->sampler->s};
            w[i].pImageInfo = &ii[i];
            break;
        default:
            ii[i] = (VkDescriptorImageInfo){.sampler = c->statics[i]};
            w[i].pImageInfo = &ii[i];
            break;
        }
    }
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
    // images: the first use takes them out of UNDEFINED (their layout stays GENERAL, which sampling and storage both accept); what the host wrote goes in
    for (unsigned i = 0; i < nup; i++) {
        rn_vk_image *im = up[i];
        if (!im->initialized) {
            VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                       .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im->img,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
            im->initialized = 1;
        }
        if (im->dirty) {
            VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {im->w, im->h, 1}};
            vk->CmdCopyBufferToImage(cb, im->stage->buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
            im->dirty = 0;
        }
    }
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
    for (unsigned i = 0; i < ndown; i++) {
        VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {down[i]->w, down[i]->h, 1}};
        vk->CmdCopyImageToBuffer(cb, down[i]->img, VK_IMAGE_LAYOUT_GENERAL, down[i]->stage->buf, 1, &bc);
    }
    if (ndown) {
        VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    }
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
