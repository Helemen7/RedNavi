// See rn_vk.h. Plain Vulkan 1.1, loaded by hand: the process may be any Metal client, so nothing here links against a Vulkan library.
#include "rn_vk.h"
#include <dlfcn.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <vulkan/vulkan.h>

#define FUNCS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) \
    X(CreateDevice) X(GetDeviceProcAddr) X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceFormatProperties)
#define DFUNCS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateComputePipelines) X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateCommandPool) \
    X(DestroyCommandPool) X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBindPipeline) X(CmdBindDescriptorSets) \
    X(CmdPushConstants) X(CmdDispatch) X(CmdPipelineBarrier) X(CreateFence) X(DestroyFence) X(WaitForFences) X(QueueSubmit) X(ResetDescriptorPool) X(ResetFences) X(ResetCommandBuffer) \
    X(GetImageSubresourceLayout) X(GetMemoryHostPointerPropertiesEXT) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) \
    X(CmdCopyBufferToImage) X(CmdCopyImage) X(CmdCopyImageToBuffer) X(CmdClearColorImage) X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CreateGraphicsPipelines) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw) X(CmdBindVertexBuffers) X(CmdSetBlendConstants) X(CmdBindIndexBuffer) X(CmdDrawIndexed)

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
    VkDescriptorPool rpool; uint32_t rpoolSets, rpoolCnt[5];   // the render pass's descriptor pool, kept and reset (a pool owns a buffer object: creating one per pass cost kernel calls each time)
    VkCommandBuffer rcb; VkFence rfence;                      // and its command buffer and fence
    int brec, brc;                             // render passes are being recorded into rcb (one submit for all of a command buffer's passes); the failure a nested flush had
    VkFramebuffer *bfb; unsigned nbfb, capbfb; // framebuffers of the recorded passes, destroyed after the submit
    struct rn_vk_image **bimg; unsigned nbimg, capbimg;   // images the recorded work uses (pending until the submit has finished)
    uint32_t rpoolUsedSets, rpoolUsed[5];      // what the recorded passes have taken from the descriptor pool
    int hostimport;                            // the device has VK_EXT_external_memory_host: host memory (an IOSurface's) can back an image
#define X(n) PFN_vk##n n;
    FUNCS(X)
    DFUNCS(X)
#undef X
};

struct rn_vk_buffer { VkBuffer buf; VkDeviceMemory mem; void *map; size_t size; };
struct rn_vk_image { int pending; rn_vk_image *base; VkImage img; VkImageView view; VkDeviceMemory mem; rn_vk_buffer *stage; VkFormat fmt; uint32_t w, h, d; unsigned texel; int storage, color, initialized, dirty; int hostmem; void *hostptr; int gpudirty; };   // gpudirty: a render pass wrote the image and the host copy is older
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

// Where RADV and its libraries are: RN_VK_DIR, else Contents/Resources/vk of the bundle this code is in (WindowServer's sandbox maps code only from /Library/GPUBundles), else the shared folder.
const char *rn_vk_dir(void) {
    static char path[1024];
    if (path[0]) return path;
    const char *d = getenv("RN_VK_DIR");
    if (d) { snprintf(path, sizeof(path), "%s", d); return path; }
    Dl_info info;
    if (dladdr((void *)rn_vk_dir, &info) && info.dli_fname) {
        char probe[1024];
        snprintf(probe, sizeof(probe), "%s", info.dli_fname);
        char *slash = strstr(probe, "/Contents/MacOS/");
        if (slash) {
            *slash = 0;
            snprintf(path, sizeof(path), "%s/Contents/Resources/vk", probe);
            char lib[1100];
            snprintf(lib, sizeof(lib), "%s/libvulkan_radeon.dylib", path);
            if (access(lib, R_OK) == 0) return path;
        }
    }
    snprintf(path, sizeof(path), "/Library/Application Support/RedNavi/vk");
    return path;
}

rn_vk *rn_vk_open(const char *dir, char *err, size_t errlen) {
    if (!dir) dir = rn_vk_dir();
    char path[1024];
    snprintf(path, sizeof(path), "%s/libvulkan_radeon.dylib", dir);
    // RADV's on-disk shader cache returned wrong code for a pipeline compiled before (a QuartzCore scene drew differently on every run after the first, and right with the cache off): off.
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 0);
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
    static const char *const hostExt[] = {"VK_EXT_external_memory_host"};
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = hostExt;
    r = vk->CreateDevice(vk->pd, &dci, NULL, &vk->dev);
    vk->hostimport = r == VK_SUCCESS;
    if (r != VK_SUCCESS) { dci.enabledExtensionCount = 0; dci.ppEnabledExtensionNames = NULL; r = vk->CreateDevice(vk->pd, &dci, NULL, &vk->dev); }   // without the extension everything is copied as before
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

// Small buffers come and go at a high rate (every draw with setVertexBytes / setFragmentBytes makes one): each is a Vulkan buffer, a buffer object in the kernel, a CPU mapping and a GPU
// address, and freeing it undoes all of that, a few kernel calls each way (the sample of the compositor showed them as the largest cost of a commit). Freed small buffers are kept, by exact
// size class (a power of two, so a buffer is up to twice the size asked for), and handed out again zeroed.
#define RN_BUF_CACHE_MAX_BYTES 65536
#define RN_BUF_CACHE_SLOTS 1024
static rn_vk_buffer *g_buf_cache[RN_BUF_CACHE_SLOTS];
static unsigned g_buf_cache_n;
static pthread_mutex_t g_buf_cache_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t rn_vk_bufstat[6];                                    // hit, miss (small), big, kept on free, dropped on free (cache full), largest big size; read by the commit statistics
rn_vk_buffer *rn_vk_buffer_create(rn_vk *vk, size_t bytes) {
    if (bytes > RN_BUF_CACHE_MAX_BYTES) { __atomic_fetch_add(&rn_vk_bufstat[2], 1, __ATOMIC_RELAXED); if (bytes > rn_vk_bufstat[5]) rn_vk_bufstat[5] = bytes; }
    if (bytes <= RN_BUF_CACHE_MAX_BYTES) {                       // size classes (powers of two from 256 bytes): sizes of setBytes data vary, an exact match almost never hit
        size_t cls = 256; while (cls < bytes) cls <<= 1;
        bytes = cls;
        rn_vk_buffer *hit = NULL;
        pthread_mutex_lock(&g_buf_cache_lock);
        for (unsigned i = 0; i < g_buf_cache_n; i++)
            if (g_buf_cache[i]->size == bytes) { hit = g_buf_cache[i]; g_buf_cache[i] = g_buf_cache[--g_buf_cache_n]; break; }
        pthread_mutex_unlock(&g_buf_cache_lock);
        if (hit) { __atomic_fetch_add(&rn_vk_bufstat[0], 1, __ATOMIC_RELAXED); memset(hit->map, 0, bytes ? bytes : 1); return hit; }
        __atomic_fetch_add(&rn_vk_bufstat[1], 1, __ATOMIC_RELAXED);
    }
    rn_vk_buffer *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->size = bytes;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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
    if (b->map && b->buf && b->mem && b->size <= RN_BUF_CACHE_MAX_BYTES) {
        int kept = 0;
        pthread_mutex_lock(&g_buf_cache_lock);
        if (g_buf_cache_n < RN_BUF_CACHE_SLOTS) { g_buf_cache[g_buf_cache_n++] = b; kept = 1; }
        pthread_mutex_unlock(&g_buf_cache_lock);
        __atomic_fetch_add(&rn_vk_bufstat[kept ? 3 : 4], 1, __ATOMIC_RELAXED);
        if (kept) return;
    }
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
    case RN_VK_INPUT_ATTACHMENT: return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
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
    case RN_VK_R8: case RN_VK_A8: return 1;
    case RN_VK_RG8: case RN_VK_R16F: case RN_VK_R16: return 2;
    case RN_VK_RGBA8: case RN_VK_BGRA8: case RN_VK_RGBA8_SRGB: case RN_VK_BGRA8_SRGB: case RN_VK_RGB10A2: case RN_VK_BGR10A2: case RN_VK_RG16F: case RN_VK_RG16: case RN_VK_R32F: case RN_VK_R32U: case RN_VK_R32I: case RN_VK_RGBA8U: return 4;
    case RN_VK_RGBA16F: case RN_VK_RG32F: case RN_VK_RGBA16: return 8;
    case RN_VK_RGBA32F: return 16;
    default: return 0;
    }
}
static VkFormat vk_format(int f) {
    switch (f) {
    case RN_VK_RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case RN_VK_BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    case RN_VK_RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case RN_VK_RGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case RN_VK_BGR10A2: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    case RN_VK_RG16F: return VK_FORMAT_R16G16_SFLOAT;
    case RN_VK_R16: return VK_FORMAT_R16_UNORM;
    case RN_VK_RG16: return VK_FORMAT_R16G16_UNORM;
    case RN_VK_RGBA16: return VK_FORMAT_R16G16B16A16_UNORM;
    case RN_VK_BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case RN_VK_R8: case RN_VK_A8: return VK_FORMAT_R8_UNORM;
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

rn_vk_image *rn_vk_image_create(rn_vk *vk, int format, uint32_t w, uint32_t h) { return rn_vk_image_create3d(vk, format, w, h, 1); }
static __thread const char *g_image_err = "";
const char *rn_vk_image_last_error(void) { return g_image_err; }
rn_vk_image *rn_vk_image_create3d(rn_vk *vk, int format, uint32_t w, uint32_t h, uint32_t d) {
    const unsigned texel = rn_vk_format_texel_bytes(format);
    g_image_err = "bad size or format";
    if (!texel || !w || !h || !d) return NULL;
    rn_vk_image *im = calloc(1, sizeof(*im));
    if (!im) return NULL;
    im->fmt = vk_format(format); im->w = w; im->h = h; im->d = d; im->texel = texel;
    VkFormatProperties fp;
    vk->GetPhysicalDeviceFormatProperties(vk->pd, im->fmt, &fp);
    const VkFormatFeatureFlags f = fp.optimalTilingFeatures;
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((f & need) != need) { g_image_err = "format features"; goto fail; }
    im->storage = (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    im->color = d == 1 && (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;   // a 3D texture is never a render target here
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = im->fmt, .extent = {w, h, d}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (im->storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0) | (im->color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0)};
    if (vk->CreateImage(vk->dev, &ici, NULL, &im->img) != VK_SUCCESS) { g_image_err = "vkCreateImage"; goto fail; }
    VkMemoryRequirements mr;
    vk->GetImageMemoryRequirements(vk->dev, im->img, &mr);
    int mt = -1;
    for (uint32_t i = 0; i < vk->mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (vk->mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = (int)i; break; }
    if (mt < 0) { g_image_err = "no device-local memory type"; goto fail; }
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt};
    if (vk->AllocateMemory(vk->dev, &mai, NULL, &im->mem) != VK_SUCCESS) { g_image_err = "vkAllocateMemory of the image (out of device memory?)"; goto fail; }
    if (vk->BindImageMemory(vk->dev, im->img, im->mem, 0) != VK_SUCCESS) { g_image_err = "vkBindImageMemory"; goto fail; }
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = d > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, .format = im->fmt,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (format == RN_VK_A8) {                   // Metal's A8Unorm samples as (0, 0, 0, a); Vulkan has no such format, so the R8 view puts the texel in alpha. Not a render target (an attachment view must not swizzle).
        vci.components = (VkComponentMapping){VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
        im->color = 0;
    }
    if (vk->CreateImageView(vk->dev, &vci, NULL, &im->view) != VK_SUCCESS) { g_image_err = "vkCreateImageView"; goto fail; }
    im->stage = rn_vk_buffer_create(vk, (size_t)w * h * d * texel);
    if (!im->stage) { g_image_err = "the host copy of the image (buffer allocation failed)"; goto fail; }
    memset(rn_vk_buffer_contents(im->stage), 0, (size_t)w * h * d * texel);
    im->dirty = 1;                              // the zeroes reach the image at its first use
    return im;
fail:
    rn_vk_image_destroy(vk, im);
    return NULL;
}
// A view of an image with another format of the same texel size and optionally swizzled components: an alias that shares the image, its memory and its host copy with the root
// (state that changes - dirty, initialized - is the root's). The root must outlive its views.
rn_vk_image *rn_vk_image_view(rn_vk *vk, rn_vk_image *base, int format, const unsigned swizzle[4]) {
    rn_vk_image *root = base->base ? base->base : base;
    if (rn_vk_format_texel_bytes(format) != root->texel) return NULL;
    VkFormatProperties fp;
    const VkFormat vf = vk_format(format);
    vk->GetPhysicalDeviceFormatProperties(vk->pd, vf, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return NULL;
    rn_vk_image *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    *v = *root;
    v->base = root; v->fmt = vf; v->view = VK_NULL_HANDLE; v->initialized = v->dirty = 0;
    v->storage = root->storage && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    v->color = root->color && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = root->img, .viewType = root->d > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, .format = vf,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (swizzle) vci.components = (VkComponentMapping){swizzle[0], swizzle[1], swizzle[2], swizzle[3]};
    if (vk->CreateImageView(vk->dev, &vci, NULL, &v->view) != VK_SUCCESS) { free(v); return NULL; }
    return v;
}
// An image over host memory the caller owns (an IOSurface's pixels): linear, in the pages the kext wires for the GPU (RN_UC_BO_USERPTR through VK_EXT_external_memory_host), so the GPU
// samples and renders in them and nothing is copied. `pitch` is the surface's bytes per row and must be the one the driver gives the linear image; `bytes` its size. NULL (and
// rn_vk_image_last_error says why): the caller keeps copying.
rn_vk_image *rn_vk_image_from_host(rn_vk *vk, int format, uint32_t w, uint32_t h, void *ptr, size_t pitch, size_t bytes) {
    g_image_err = "bad size or format";
    const unsigned texel = rn_vk_format_texel_bytes(format);
    if (!vk->hostimport) { g_image_err = "no VK_EXT_external_memory_host"; return NULL; }
    if (!texel || !w || !h || !ptr || ((uintptr_t)ptr & 4095) || !bytes) return NULL;
    rn_vk_image *im = calloc(1, sizeof(*im));
    if (!im) return NULL;
    im->fmt = vk_format(format); im->w = w; im->h = h; im->d = 1; im->texel = texel; im->hostmem = 1; im->hostptr = ptr;
    VkFormatProperties fp;
    vk->GetPhysicalDeviceFormatProperties(vk->pd, im->fmt, &fp);
    const VkFormatFeatureFlags f = fp.linearTilingFeatures;
    if (!(f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { g_image_err = "linear format features"; goto fail; }
    im->storage = (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    im->color = (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = im->fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR, .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                             .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (im->color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) | (im->storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0)};
    if (vk->CreateImage(vk->dev, &ici, NULL, &im->img) != VK_SUCCESS) { g_image_err = "vkCreateImage (linear)"; goto fail; }
    VkImageSubresource sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout sl;
    vk->GetImageSubresourceLayout(vk->dev, im->img, &sr, &sl);
    if (sl.offset != 0 || sl.rowPitch != pitch) { g_image_err = "the surface's row pitch is not the linear image's"; goto fail; }
    VkMemoryRequirements mr;
    vk->GetImageMemoryRequirements(vk->dev, im->img, &mr);
    if (mr.size > bytes) { g_image_err = "the image is larger than the surface"; goto fail; }
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (vk->GetMemoryHostPointerPropertiesEXT(vk->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hp) != VK_SUCCESS) { g_image_err = "vkGetMemoryHostPointerPropertiesEXT"; goto fail; }
    int mt = -1;
    for (uint32_t i = 0; i < vk->mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & hp.memoryTypeBits & (1u << i)) && (vk->mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mt = (int)i; break; }
    if (mt < 0) { g_image_err = "no memory type for the host pointer"; goto fail; }
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = ptr};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = (bytes + 4095) & ~(size_t)4095, .memoryTypeIndex = (uint32_t)mt};
    if (vk->AllocateMemory(vk->dev, &mai, NULL, &im->mem) != VK_SUCCESS) { g_image_err = "vkAllocateMemory of the host pointer"; goto fail; }
    if (vk->BindImageMemory(vk->dev, im->img, im->mem, 0) != VK_SUCCESS) { g_image_err = "vkBindImageMemory (host)"; goto fail; }
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = im->fmt,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (format == RN_VK_A8) { vci.components = (VkComponentMapping){VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R}; im->color = 0; }
    if (vk->CreateImageView(vk->dev, &vci, NULL, &im->view) != VK_SUCCESS) { g_image_err = "vkCreateImageView (host)"; goto fail; }
    return im;
fail:
    rn_vk_image_destroy(vk, im);
    return NULL;
}
int rn_vk_image_is_host(rn_vk_image *im) { return (im->base ? im->base : im)->hostmem; }
static rn_vk *g_readback_vk;                                    // the device of the last render pass, for the lazy readback below
static void image_readback_now(rn_vk *vk, rn_vk_image *r);
static void bsync(rn_vk *vk);
void *rn_vk_image_staging(rn_vk_image *im) {
    rn_vk_image *r = im->base ? im->base : im;
    if (r->pending && g_readback_vk) bsync(g_readback_vk);
    if (r->gpudirty && !r->hostmem && g_readback_vk) image_readback_now(g_readback_vk, r);
    return r->hostmem ? r->hostptr : rn_vk_buffer_contents(im->stage);
}
void rn_vk_image_host_wrote(rn_vk_image *im) { (im->base ? im->base : im)->dirty = 1; }
void rn_vk_image_destroy(rn_vk *vk, rn_vk_image *im) {
    if (!im) return;
    if ((im->base ? im->base : im)->pending) bsync(vk);
    if (im->base) {                                                  // a view: only its own image view
        if (im->view) vk->DestroyImageView(vk->dev, im->view, NULL);
        free(im);
        return;
    }
    if (im->view) vk->DestroyImageView(vk->dev, im->view, NULL);
    if (im->img) vk->DestroyImage(vk->dev, im->img, NULL);
    if (im->mem) vk->FreeMemory(vk->dev, im->mem, NULL);
    rn_vk_buffer_destroy(vk, im->stage);
    free(im);
}

typedef struct { uint32_t local[3], groups[3], pc[12]; } region;

// Fills the descriptor writes of one set from the resources of a dispatch or a draw. Every image goes into `in` (it must be current on the card before the work), the ones the shader
// may write into `out` (copied back afterwards). 0, or 4 (buffer offset) or 6 (resource missing or unusable).
static int fill_writes(rn_vk *vk, unsigned n, const unsigned *kinds, const unsigned *binds, const VkSampler *statics, const rn_vk_arg *args, VkDescriptorSet set, VkDescriptorSet inset, VkDescriptorBufferInfo *bi,
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
        case RN_VK_INPUT_ATTACHMENT:                              // framebuffer fetch: the render target itself (already part of the pass), in the input attachment set
            if (!a->image || !inset) return 6;
            w[i].dstSet = inset;
            ii[i] = (VkDescriptorImageInfo){.imageView = a->image->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
            w[i].pImageInfo = &ii[i];
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


// Batching. A command buffer's render passes are recorded into one Vulkan command buffer and submitted once, at rn_vk_batch_end or when something else needs the card's results (a dispatch, a copy, the
// host reading an image the batch uses, an image being destroyed): one submit and one wait per command buffer instead of per pass. The mutex is held from begin to end, so other threads that touch
// an image in flight wait for the end of the batch instead of racing it.
static pthread_mutex_t g_bmu; static pthread_once_t g_bmu_once = PTHREAD_ONCE_INIT;
static void bmu_init(void) { pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&g_bmu, &a); }
uint64_t rn_vk_fence_ticks;
static void batch_hold(rn_vk *vk, rn_vk_image *im) {
    rn_vk_image *r = im->base ? im->base : im;
    if (r->pending) return;
    if (vk->nbimg == vk->capbimg) { vk->capbimg = vk->capbimg ? vk->capbimg * 2 : 64; vk->bimg = realloc(vk->bimg, vk->capbimg * sizeof(*vk->bimg)); }
    vk->bimg[vk->nbimg++] = r; r->pending = 1;
}
static int batch_flush(rn_vk *vk) {
    if (!vk->brec) return 0;
    vk->brec = 0;
    int rc = 3;
    VkCommandBuffer cb = vk->rcb;
    if (vk->EndCommandBuffer(cb) == VK_SUCCESS) {
        if (!vk->rfence) {
            VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (vk->CreateFence(vk->dev, &fci, NULL, &vk->rfence) != VK_SUCCESS) vk->rfence = VK_NULL_HANDLE;
        }
        if (vk->rfence) {
            vk->ResetFences(vk->dev, 1, &vk->rfence);
            VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
            if (vk->QueueSubmit(vk->queue, 1, &si, vk->rfence) == VK_SUCCESS) {
                const uint64_t wt0 = mach_absolute_time();
                rc = vk->WaitForFences(vk->dev, 1, &vk->rfence, VK_TRUE, 8000000000ull) == VK_SUCCESS ? 0 : 3;
                __atomic_fetch_add(&rn_vk_fence_ticks, mach_absolute_time() - wt0, __ATOMIC_RELAXED);
                if (rc == 3) { vk->DestroyFence(vk->dev, vk->rfence, NULL); vk->rfence = VK_NULL_HANDLE; }
            }
        }
    }
    for (unsigned i = 0; i < vk->nbfb; i++) vk->DestroyFramebuffer(vk->dev, vk->bfb[i], NULL);
    vk->nbfb = 0;
    for (unsigned i = 0; i < vk->nbimg; i++) vk->bimg[i]->pending = 0;
    vk->nbimg = 0;
    vk->rpoolUsedSets = 0; memset(vk->rpoolUsed, 0, sizeof(vk->rpoolUsed));
    if (rc) vk->brc = rc;
    return rc;
}
// Whatever needs the card's results or the card idle calls this first.
static void bsync(rn_vk *vk) {
    if (!vk) return;
    pthread_once(&g_bmu_once, bmu_init);
    pthread_mutex_lock(&g_bmu);
    batch_flush(vk);
    pthread_mutex_unlock(&g_bmu);
}
void rn_vk_batch_begin(rn_vk *vk) { pthread_once(&g_bmu_once, bmu_init); pthread_mutex_lock(&g_bmu); vk->brc = 0; }
int rn_vk_batch_end(rn_vk *vk) { batch_flush(vk); const int rc = vk->brc; vk->brc = 0; pthread_mutex_unlock(&g_bmu); return rc; }
// Waiting for something other than the card (an event another thread signals): the batch ends, and the lock is let go until the wait is over.
static void image_pending_wait(rn_vk *vk, rn_vk_image *r) { if (r->pending) bsync(vk); }

// The images of a submission: the first use takes an image out of UNDEFINED (its layout stays GENERAL, which sampling, storage and attachment use all accept) and what the host wrote goes in.
static void images_in(rn_vk *vk, VkCommandBuffer cb, rn_vk_image *const *im, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        rn_vk_image *r = im[i]->base ? im[i]->base : im[i];
        int seen = 0;
        for (unsigned j = 0; j < i; j++) seen |= (im[j]->base ? im[j]->base : im[j]) == r;
        if (seen) continue;
        if (!r->initialized) {
            VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = r->hostmem ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_UNDEFINED,
                                       .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = r->img,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
            r->initialized = 1;
        }
        if (r->dirty && !r->hostmem) {
            VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {r->w, r->h, r->d}};
            vk->CmdCopyBufferToImage(cb, r->stage->buf, r->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
            r->dirty = 0;
        }
    }
}
// After the work: the images the shaders wrote are copied to their host copies. The caller has made the writes visible to the transfer stage.
static void images_out(rn_vk *vk, VkCommandBuffer cb, rn_vk_image *const *im, unsigned n) {
    unsigned done = 0;
    for (unsigned i = 0; i < n; i++) {
        rn_vk_image *r = im[i]->base ? im[i]->base : im[i];
        int seen = 0;
        for (unsigned j = 0; j < i; j++) seen |= (im[j]->base ? im[j]->base : im[j]) == r;
        if (seen || r->hostmem) continue;                            // an image over host memory is already where the host reads it
        VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {r->w, r->h, r->d}};
        vk->CmdCopyImageToBuffer(cb, r->img, VK_IMAGE_LAYOUT_GENERAL, r->stage->buf, 1, &bc);
        r->gpudirty = 0;
        done++;
    }
    if (done) {
        VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    }
}
// Render passes do not read their targets back: the host copy is only needed when the host asks for the pixels (rn_vk_image_staging), and a full-image copy after every pass was
// a large part of the card time of a compositor frame. The image is marked and read back when the host looks at it.
static void images_mark_gpu(rn_vk_image *const *im, unsigned n) {
    for (unsigned i = 0; i < n; i++) { rn_vk_image *r = im[i]->base ? im[i]->base : im[i]; if (!r->hostmem) r->gpudirty = 1; }
}
static void image_readback_now(rn_vk *vk, rn_vk_image *r) {
    VkCommandBuffer cb = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vk->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vk->AllocateCommandBuffers(vk->dev, &cba, &cb) != VK_SUCCESS) return;
    VkCommandBufferBeginInfo cbb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    rn_vk_image *one[1] = {r};
    if (vk->BeginCommandBuffer(cb, &cbb) == VK_SUCCESS) {
        images_out(vk, cb, one, 1);                                  // clears gpudirty
        if (vk->EndCommandBuffer(cb) == VK_SUCCESS && vk->CreateFence(vk->dev, &fci, NULL, &fence) == VK_SUCCESS) {
            VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
            if (vk->QueueSubmit(vk->queue, 1, &si, fence) == VK_SUCCESS) vk->WaitForFences(vk->dev, 1, &fence, VK_TRUE, 8000000000ull);
        }
    }
    if (fence) vk->DestroyFence(vk->dev, fence, NULL);
    vk->FreeCommandBuffers(vk->dev, vk->pool, 1, &cb);
}

static int dispatch_regions(rn_vk *vk, rn_vk_compute *c, const rn_vk_arg *args, const void *push, const region *regs, unsigned nregs) {
    bsync(vk);
    VkDescriptorBufferInfo bi[RN_VK_MAX_BINDINGS];
    VkDescriptorImageInfo ii[RN_VK_MAX_BINDINGS];
    VkWriteDescriptorSet w[RN_VK_MAX_BINDINGS];
    rn_vk_image *up[RN_VK_MAX_BINDINGS], *down[RN_VK_MAX_BINDINGS];
    unsigned nup = 0, ndown = 0;
    int fr = fill_writes(vk, c->nbind, c->kinds, c->binds, c->statics, args, c->ds, VK_NULL_HANDLE, bi, ii, w, up, &nup, down, &ndown);
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
    VkDescriptorSetLayout dsl[2], dsl_empty, dsl_in;     // the pipeline layout has four sets: vertex 0, fragment 1, none 2, input attachments 3 (the translator's own set for [[color(n)]])
    unsigned n_in;                                       // input attachment bindings of the fragment stage (framebuffer fetch)
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
    unsigned nb = 0;
    for (unsigned i = 0; i < n; i++) {
        kinds[i] = binds[i].kind;
        bn[i] = binds[i].binding;
        if (binds[i].kind == RN_VK_INPUT_ATTACHMENT) continue;          // lives in set 3, see rn_vk_render_create
        const VkDescriptorType t = desc_type(binds[i].kind);
        b[nb++] = (VkDescriptorSetLayoutBinding){.binding = binds[i].binding, .descriptorType = t, .descriptorCount = 1, .stageFlags = stage};
        count[t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? 0 : t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? 1 : t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? 2 : 3]++;
        if (binds[i].kind == RN_VK_STATIC_SAMPLER) {
            if (!binds[i].sampler) return 0;
            VkSamplerCreateInfo si = sampler_info(binds[i].sampler);
            if (vk->CreateSampler(vk->dev, &si, NULL, &statics[i]) != VK_SUCCESS) return 0;
        }
    }
    VkDescriptorSetLayoutCreateInfo dsl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = nb ? b : NULL};
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
    {   // sets 2 (empty) and 3 (the input attachments of a framebuffer-fetch fragment shader, bindings as the translator numbers them)
        VkDescriptorSetLayoutBinding ib[RN_VK_MAX_BINDINGS];
        unsigned ni = 0;
        for (unsigned i = 0; i < d->nfbind; i++)
            if (d->fbinds[i].kind == RN_VK_INPUT_ATTACHMENT) ib[ni++] = (VkDescriptorSetLayoutBinding){.binding = d->fbinds[i].binding, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
        r->n_in = ni;
        VkDescriptorSetLayoutCreateInfo e = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}, in = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = ni, .pBindings = ni ? ib : NULL};
        if (vk->CreateDescriptorSetLayout(vk->dev, &e, NULL, &r->dsl_empty) != VK_SUCCESS) goto fail;
        if (vk->CreateDescriptorSetLayout(vk->dev, &in, NULL, &r->dsl_in) != VK_SUCCESS) goto fail;
    }
    VkDescriptorSetLayout all[4] = {r->dsl[0], r->dsl[1], r->dsl_empty, r->dsl_in};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 4, .pSetLayouts = all};
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
    // every colour attachment is also an input attachment (framebuffer fetch), with a self dependency so a draw sees what the earlier draws of the pass wrote at the same pixel
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .inputAttachmentCount = r->nt, .pInputAttachments = ref, .colorAttachmentCount = r->nt, .pColorAttachments = ref};
    VkSubpassDependency selfdep = {.srcSubpass = 0, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                   .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT};
    VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = r->nt, .pAttachments = att, .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &selfdep};
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
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = d->cull_mode,
                                                 .frontFace = d->front_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
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
    if (r->dsl_empty) vk->DestroyDescriptorSetLayout(vk->dev, r->dsl_empty, NULL);
    if (r->dsl_in) vk->DestroyDescriptorSetLayout(vk->dev, r->dsl_in, NULL);
    if (r->vs) vk->DestroyShaderModule(vk->dev, r->vs, NULL);
    if (r->fs) vk->DestroyShaderModule(vk->dev, r->fs, NULL);
    free(r);
}

// vkCmdClearColorImage works in the format of the image, not of the view the pass uses: for a view whose red-blue order differs from the image's (BGRA8 over RGBA8) the two colour
// components are swapped here so that the clear colour means what the view says. (sRGB views over UNORM images, or the reverse, are not corrected.)
static void clear_value(const rn_vk_image *im, const float c[4], VkClearColorValue *out) {
    memcpy(out->float32, c, sizeof(out->float32));
    if (!im->base) return;
    const int vb = im->fmt == VK_FORMAT_B8G8R8A8_UNORM || im->fmt == VK_FORMAT_B8G8R8A8_SRGB, rb = im->base->fmt == VK_FORMAT_B8G8R8A8_UNORM || im->base->fmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (vb != rb) { out->float32[0] = c[2]; out->float32[2] = c[0]; }
}

// A pass without draws: only its clears. They are done outside a render pass (vkCmdClearColorImage) and the images' host copies are brought up to date.
static int clear_targets(rn_vk *vk, const rn_vk_target *targets, unsigned nt) {
    bsync(vk);
    rn_vk_image *im[8];
    for (unsigned i = 0; i < nt; i++) {
        if (!targets[i].image || !targets[i].image->color) return 6;
        im[i] = targets[i].image;
    }
    int rc = 2;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vk->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vk->AllocateCommandBuffers(vk->dev, &cba, &cb) != VK_SUCCESS) return 2;
    VkCommandBufferBeginInfo cbb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vk->BeginCommandBuffer(cb, &cbb) != VK_SUCCESS) goto done;
    images_in(vk, cb, im, nt);
    VkMemoryBarrier tt = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &tt, 0, NULL, 0, NULL);
    unsigned cleared = 0;
    rn_vk_image *out[8];
    for (unsigned i = 0; i < nt; i++) {
        if (!targets[i].clear) continue;
        VkClearColorValue cc;
        clear_value(targets[i].image, targets[i].color, &cc);
        VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk->CmdClearColorImage(cb, targets[i].image->img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rg);
        if (!targets[i].discard) out[cleared++] = targets[i].image;
    }
    VkMemoryBarrier fromClear = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &fromClear, 0, NULL, 0, NULL);
    images_mark_gpu(out, cleared);
    if (vk->EndCommandBuffer(cb) != VK_SUCCESS) goto done;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vk->CreateFence(vk->dev, &fci, NULL, &fence) != VK_SUCCESS) goto done;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    if (vk->QueueSubmit(vk->queue, 1, &si, fence) != VK_SUCCESS) goto done;
    rc = vk->WaitForFences(vk->dev, 1, &fence, VK_TRUE, 8000000000ull) == VK_SUCCESS ? 0 : 3;
done:
    if (fence) vk->DestroyFence(vk->dev, fence, NULL);
    if (cb) vk->FreeCommandBuffers(vk->dev, vk->pool, 1, &cb);
    return rc;
}

// Does a draw sample one of the pass's own render targets (QuartzCore reads the destination as a texture when the device has no framebuffer fetch, and relies on Metal's texture
// barrier between draws)? Such a draw needs the earlier draws' writes made visible first, and the next draw must not overtake its reads.
static int samples_target(const rn_vk_draw *dr, const rn_vk_target *targets, unsigned nt) {
    for (int s = 0; s < 2; s++) {
        const rn_vk_arg *args = s ? dr->fargs : dr->vargs;
        if (!args) continue;
        for (unsigned i = 0; i < dr->pipe->n[s]; i++) {
            if (dr->pipe->kinds[s][i] != RN_VK_SAMPLED_IMAGE || !args[i].image) continue;
            const rn_vk_image *r = args[i].image->base ? args[i].image->base : args[i].image;
            for (unsigned t = 0; t < nt; t++) if ((targets[t].image->base ? targets[t].image->base : targets[t].image) == r) return 1;
        }
    }
    return 0;
}

int rn_vk_render_pass(rn_vk *vk, const rn_vk_target *targets, unsigned nt, const rn_vk_draw *draws, unsigned nd) {
    if (!nt || nt > 8) return 6;
    g_readback_vk = vk;
    if (!nd) return clear_targets(vk, targets, nt);
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
    VkCommandBuffer cb = VK_NULL_HANDLE;
    pthread_once(&g_bmu_once, bmu_init);
    pthread_mutex_lock(&g_bmu);                  // recursive: held for the whole batch by its thread
    VkDescriptorSet *sets = calloc(3 * (size_t)nd, sizeof(*sets));      // per draw: vertex, fragment, input attachments
    rn_vk_image **in = calloc(8 + (size_t)nd * 2 * RN_VK_MAX_BINDINGS, sizeof(*in));
    rn_vk_image **out = calloc(8 + (size_t)nd * 2 * RN_VK_MAX_BINDINGS, sizeof(*out));
    VkWriteDescriptorSet *w = calloc((size_t)nd * 2 * RN_VK_MAX_BINDINGS, sizeof(*w));
    VkDescriptorBufferInfo *bi = calloc((size_t)nd * 2 * RN_VK_MAX_BINDINGS, sizeof(*bi));
    VkDescriptorImageInfo *ii = calloc((size_t)nd * 2 * RN_VK_MAX_BINDINGS, sizeof(*ii));
    if (!sets || !in || !out || !w || !bi || !ii) goto done;
    unsigned nin = 0, nout = 0, nw = 0;
    for (unsigned i = 0; i < nt; i++) { in[nin++] = targets[i].image; if (!targets[i].discard) out[nout++] = targets[i].image; }
    // descriptor pool for every set of every draw (a set is written when the commands are recorded, so draws must not share one)
    uint32_t cnt[5] = {0, 0, 0, 0, 0};
    for (unsigned d = 0; d < nd; d++)
        for (int s = 0; s < 2; s++)
            for (unsigned i = 0; i < draws[d].pipe->n[s]; i++) {
                const unsigned k = draws[d].pipe->kinds[s][i];
                cnt[k == RN_VK_BUFFER ? 0 : k == RN_VK_SAMPLED_IMAGE ? 1 : k == RN_VK_STORAGE_IMAGE ? 2 : k == RN_VK_INPUT_ATTACHMENT ? 4 : 3]++;
            }
    VkDescriptorPoolSize ps[5];
    static const VkDescriptorType ty[5] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT};
    uint32_t nps = 0;
    for (int k = 0; k < 5; k++) if (cnt[k]) ps[nps++] = (VkDescriptorPoolSize){ty[k], cnt[k]};
    {   // One pool for all passes (they are committed one at a time): reset when it is big enough, rebuilt larger when not. Each count gets headroom so that the rebuilds stop quickly.
        if (vk->brec) {                          // the recorded passes keep their sets: the pass goes on only if the pool still has room for it, else the batch is submitted first
            int room = vk->rpool && vk->rpoolUsedSets + 3 * nd <= vk->rpoolSets;
            for (int k = 0; k < 5 && room; k++) if (vk->rpoolUsed[k] + cnt[k] > vk->rpoolCnt[k]) room = 0;
            if (!room) { const int fr = batch_flush(vk); if (fr) { rc = fr; goto done; } }
        }
        int fits = vk->rpool && 3 * nd <= vk->rpoolSets;
        for (int k = 0; k < 5 && fits; k++) if (cnt[k] > vk->rpoolCnt[k]) fits = 0;
        if (fits) {
            if (!vk->brec) vk->ResetDescriptorPool(vk->dev, vk->rpool, 0);
        } else {
            if (vk->rpool) vk->DestroyDescriptorPool(vk->dev, vk->rpool, NULL);
            vk->rpool = VK_NULL_HANDLE;
            uint32_t bigSets = 3 * nd * 2 > 192 ? 3 * nd * 2 : 192;
            VkDescriptorPoolSize bigps[5];
            uint32_t bigCnt[5], nb = 0;
            for (int k = 0; k < 5; k++) { bigCnt[k] = cnt[k] * 2 > 256 ? cnt[k] * 2 : 256; bigps[nb++] = (VkDescriptorPoolSize){ty[k], bigCnt[k]}; }
            VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = bigSets, .poolSizeCount = nb, .pPoolSizes = bigps};
            if (vk->CreateDescriptorPool(vk->dev, &dpi, NULL, &vk->rpool) != VK_SUCCESS) { vk->rpool = VK_NULL_HANDLE; goto done; }
            vk->rpoolSets = bigSets;
            for (int k = 0; k < 5; k++) vk->rpoolCnt[k] = bigCnt[k];
        }
        pool = vk->rpool;
        vk->rpoolUsedSets += 3 * nd;
        for (int k = 0; k < 5; k++) vk->rpoolUsed[k] += cnt[k];
    }
    for (unsigned d = 0; d < nd; d++) {
        const rn_vk_render *p = draws[d].pipe;
        if (p->n_in) {
            VkDescriptorSetAllocateInfo dsa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &p->dsl_in};
            if (vk->AllocateDescriptorSets(vk->dev, &dsa, &sets[3 * d + 2]) != VK_SUCCESS) goto done;
        }
        for (int s = 0; s < 2; s++) {
            VkDescriptorSetAllocateInfo dsa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &p->dsl[s]};
            if (vk->AllocateDescriptorSets(vk->dev, &dsa, &sets[3 * d + s]) != VK_SUCCESS) goto done;
            if (!p->n[s]) continue;
            const rn_vk_arg *args = s ? draws[d].fargs : draws[d].vargs;
            if (!args) { rc = 6; goto done; }
            const int fr = fill_writes(vk, p->n[s], p->kinds[s], p->binds[s], p->statics[s], args, sets[3 * d + s], sets[3 * d + 2], bi + nw, ii + nw, w + nw, in, &nin, out, &nout);
            if (fr) { rc = fr; goto done; }
            nw += p->n[s];
        }
    }
    if (nw) vk->UpdateDescriptorSets(vk->dev, nw, w, 0, NULL);
    VkImageView views[8];
    for (unsigned i = 0; i < nt; i++) views[i] = targets[i].image->view;
    VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = p0->rp, .attachmentCount = nt, .pAttachments = views, .width = W, .height = H, .layers = 1};
    if (vk->CreateFramebuffer(vk->dev, &fbi, NULL, &fb) != VK_SUCCESS) goto done;
    if (vk->nbfb == vk->capbfb) { vk->capbfb = vk->capbfb ? vk->capbfb * 2 : 32; vk->bfb = realloc(vk->bfb, vk->capbfb * sizeof(*vk->bfb)); }
    vk->bfb[vk->nbfb++] = fb;                            // destroyed once the batch has run (the pass keeps using it until then)
    const VkFramebuffer fbh = fb; fb = VK_NULL_HANDLE;
    if (!vk->rcb) {
        VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vk->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (vk->AllocateCommandBuffers(vk->dev, &cba, &vk->rcb) != VK_SUCCESS) { vk->rcb = VK_NULL_HANDLE; goto done; }
    }
    cb = vk->rcb;
    if (!vk->brec) {
        vk->ResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo cbb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if (vk->BeginCommandBuffer(cb, &cbb) != VK_SUCCESS) goto done;
        vk->brec = 1;
    }
    for (unsigned i = 0; i < nin; i++) batch_hold(vk, in[i]);
    for (unsigned i = 0; i < nout; i++) batch_hold(vk, out[i]);
    images_in(vk, cb, in, nin);
    {   // Experiment switch /Users/Shared/rednavi-fullbarrier: a full memory barrier before every pass (host writes and every earlier GPU write made visible to everything)
        static int full = -1;
        if (full < 0) full = access("/Users/Shared/rednavi-fullbarrier", F_OK) == 0;
        if (full) {
            VkMemoryBarrier fb_ = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT};
            vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &fb_, 0, NULL, 0, NULL);
        }
    }
    int clears = 0;
    for (unsigned i = 0; i < nt; i++) clears |= targets[i].clear != 0;
    VkMemoryBarrier tt = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    if (clears) {
        vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &tt, 0, NULL, 0, NULL);
        for (unsigned i = 0; i < nt; i++) {
            if (!targets[i].clear) continue;
            VkClearColorValue cc;
            clear_value(targets[i].image, targets[i].color, &cc);
            VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vk->CmdClearColorImage(cb, targets[i].image->img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rg);
        }
    }
    VkMemoryBarrier toGfx = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, 1, &toGfx, 0, NULL, 0, NULL);
    VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = p0->rp, .framebuffer = fbh, .renderArea = {{0, 0}, {W, H}}};
    vk->CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    int prevFeedback = 0;
    for (unsigned d = 0; d < nd; d++) {
        const rn_vk_draw *dr = &draws[d];
        const int feedback = samples_target(dr, targets, nt);
        if (d && (feedback || prevFeedback)) {          // the render pass is restarted (LOAD) around a draw that reads the target, with the writes made visible
            vk->CmdEndRenderPass(cb);
            VkMemoryBarrier fb2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                                   .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
            vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   0, 1, &fb2, 0, NULL, 0, NULL);
            vk->CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        }
        prevFeedback = feedback;
        vk->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, dr->pipe->pipe);
        vk->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, dr->pipe->pl, 0, 2, &sets[3 * d], 0, NULL);
        if (dr->pipe->n_in) {                           // framebuffer fetch: the earlier draws' colour writes at this pixel are made visible to the input attachment read (same-subpass self dependency)
            if (d) {
                VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT};
                vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL);
            }
            vk->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, dr->pipe->pl, 3, 1, &sets[3 * d + 2], 0, NULL);
        }
        // Metal's clip space is y-up, Vulkan's y-down: the viewport is flipped (origin at the bottom, negative height)
        const double *v = dr->viewport;
        VkViewport vp = dr->has_viewport ? (VkViewport){(float)v[0], (float)(v[1] + v[3]), (float)v[2], (float)-v[3], (float)v[4], (float)v[5]} : (VkViewport){0, (float)H, (float)W, -(float)H, 0.0f, 1.0f};
        VkRect2D sc = dr->has_scissor ? (VkRect2D){{(int32_t)dr->scissor[0], (int32_t)dr->scissor[1]}, {dr->scissor[2], dr->scissor[3]}} : (VkRect2D){{0, 0}, {W, H}};
        vk->CmdSetViewport(cb, 0, 1, &vp);
        vk->CmdSetScissor(cb, 0, 1, &sc);
        vk->CmdSetBlendConstants(cb, dr->has_blend_color ? dr->blend_color : (const float[4]){0, 0, 0, 0});
        for (unsigned i = 0; i < dr->pipe->nvl; i++) {
            const rn_vk_vbuf *vbf = dr->vbufs ? &dr->vbufs[dr->pipe->vlbind[i]] : NULL;
            if (!vbf || !vbf->buffer || vbf->offset >= vbf->buffer->size) { vk->CmdEndRenderPass(cb); rc = 6; goto done; }
            VkBuffer vbuf = vbf->buffer->buf;
            VkDeviceSize voff = vbf->offset;
            vk->CmdBindVertexBuffers(cb, dr->pipe->vlbind[i], 1, &vbuf, &voff);
        }
        if (dr->index_buffer) {
            if (dr->index_offset >= dr->index_buffer->size) { vk->CmdEndRenderPass(cb); rc = 4; goto done; }
            vk->CmdBindIndexBuffer(cb, dr->index_buffer->buf, dr->index_offset, dr->index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
            vk->CmdDrawIndexed(cb, dr->vertex_count, dr->instance_count ? dr->instance_count : 1, 0, dr->base_vertex, dr->first_instance);
        } else
            vk->CmdDraw(cb, dr->vertex_count, dr->instance_count ? dr->instance_count : 1, dr->first_vertex, dr->first_instance);
    }
    vk->CmdEndRenderPass(cb);
    VkMemoryBarrier fromGfx = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_HOST_READ_BIT};   // the next pass of the batch reads or blends over what this one drew   // host read: a target over an IOSurface's own memory is read by the CPU and by other processes
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &fromGfx, 0, NULL, 0, NULL);
    images_mark_gpu(out, nout);
    rc = 0;                                      // submitted and waited for at the end of the batch
done:
    if (fb) vk->DestroyFramebuffer(vk->dev, fb, NULL);
    free(sets); free(in); free(out); free(w); free(bi); free(ii);
    pthread_mutex_unlock(&g_bmu);
    return rc;
}

// Latency of the submit and wait path with no work in the command buffer: what one render pass costs the compositor before the card has done anything. Logs min / average / max over n round trips.
void rn_vk_latency_probe(rn_vk *vk, unsigned n, double *min_us, double *avg_us, double *max_us) {
    *min_us = *avg_us = *max_us = 0;
    VkCommandBuffer cb = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vk->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vk->AllocateCommandBuffers(vk->dev, &cba, &cb) != VK_SUCCESS) return;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vk->CreateFence(vk->dev, &fci, NULL, &fence) != VK_SUCCESS) goto out;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double sum = 0, mn = 1e9, mx = 0; unsigned ok = 0;
    for (unsigned i = 0; i < n; i++) {
        VkCommandBufferBeginInfo cbb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vk->ResetCommandBuffer(cb, 0);
        if (vk->BeginCommandBuffer(cb, &cbb) != VK_SUCCESS || vk->EndCommandBuffer(cb) != VK_SUCCESS) break;
        vk->ResetFences(vk->dev, 1, &fence);
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        const uint64_t t0 = mach_absolute_time();
        if (vk->QueueSubmit(vk->queue, 1, &si, fence) != VK_SUCCESS) break;
        if (vk->WaitForFences(vk->dev, 1, &fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) break;
        const double us = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e3;
        sum += us; if (us < mn) mn = us; if (us > mx) mx = us; ok++;
    }
    if (ok) { *min_us = mn; *avg_us = sum / ok; *max_us = mx; }
out:
    if (fence) vk->DestroyFence(vk->dev, fence, NULL);
    vk->FreeCommandBuffers(vk->dev, vk->pool, 1, &cb);
}

// Bulk transfer time of a 1920 x 1080 BGRA image between host memory and the card (a copy command, not shader accesses): upload = host staging to the image, download = the image to
// the host staging. Milliseconds per copy, averaged over n copies; compares with the 3 to 5 ms a compositor pass takes with its surfaces in system memory.
void rn_vk_bandwidth_probe(rn_vk *vk, int format, unsigned n, double *up_ms, double *down_ms) {
    *up_ms = *down_ms = 0;
    rn_vk_image *im = rn_vk_image_create(vk, format, 1920, 1080);
    if (!im) return;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double up = 0, down = 0;
    for (unsigned i = 0; i < n; i++) {
        VkCommandBuffer cb = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vk->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vk->AllocateCommandBuffers(vk->dev, &cba, &cb) != VK_SUCCESS) break;
        VkCommandBufferBeginInfo cbb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        rn_vk_image *one[1] = {im};
        im->dirty = 1;
        if (vk->BeginCommandBuffer(cb, &cbb) == VK_SUCCESS) {
            images_in(vk, cb, one, 1);
            if (vk->EndCommandBuffer(cb) == VK_SUCCESS && vk->CreateFence(vk->dev, &fci, NULL, &fence) == VK_SUCCESS) {
                VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
                const uint64_t t0 = mach_absolute_time();
                if (vk->QueueSubmit(vk->queue, 1, &si, fence) == VK_SUCCESS) vk->WaitForFences(vk->dev, 1, &fence, VK_TRUE, 2000000000ull);
                up += (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
            }
        }
        if (fence) vk->DestroyFence(vk->dev, fence, NULL);
        vk->FreeCommandBuffers(vk->dev, vk->pool, 1, &cb);
        im->gpudirty = 1;
        const uint64_t t1 = mach_absolute_time();
        image_readback_now(vk, im);
        down += (double)(mach_absolute_time() - t1) * tb.numer / tb.denom / 1e6;
    }
    *up_ms = up / n; *down_ms = down / n;
    rn_vk_image_destroy(vk, im);
}

// A symbol of the Vulkan driver library (it is loaded RTLD_LOCAL): the bridge's scanout and present helpers.
void *rn_vk_symbol(rn_vk *vk, const char *name) { return vk && vk->lib ? dlsym(vk->lib, name) : NULL; }

// Copies the whole of one image to another of the same format and size on the card (both stay in GENERAL layout), submitted and waited for. For a render result in VRAM into the
// display's own surface: the copy runs at memory speed, with no PCIe traffic when the destination is VRAM. 0 on success.
static void record_copy(rn_vk *vk, VkCommandBuffer cb, rn_vk_image *src, rn_vk_image *dst) {
    rn_vk_image *both[2] = {src, dst};
    images_in(vk, cb, both, 2);
    VkMemoryBarrier toCopy = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toCopy, 0, NULL, 0, NULL);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {src->w, src->h, 1}};
    vk->CmdCopyImage(cb, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    VkMemoryBarrier done_ = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT};
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &done_, 0, NULL, 0, NULL);
}
int rn_vk_image_copy(rn_vk *vk, rn_vk_image *src, rn_vk_image *dst) {
    if (!src || !dst || src->w != dst->w || src->h != dst->h || src->fmt != dst->fmt) return 6;
    bsync(vk);
    VkCommandBuffer cb = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; int rc = 2;
    VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vk->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vk->AllocateCommandBuffers(vk->dev, &cba, &cb) != VK_SUCCESS) return 2;
    VkCommandBufferBeginInfo cbb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vk->BeginCommandBuffer(cb, &cbb) != VK_SUCCESS) goto done;
    record_copy(vk, cb, src, dst);
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
// The same copy recorded at the end of the open batch: it runs on the card after the passes, in the same submit.
int rn_vk_batch_copy(rn_vk *vk, rn_vk_image *src, rn_vk_image *dst) {
    if (!src || !dst || src->w != dst->w || src->h != dst->h || src->fmt != dst->fmt) return 6;
    if (!vk->brec) return rn_vk_image_copy(vk, src, dst);
    record_copy(vk, vk->rcb, src, dst);
    batch_hold(vk, src); batch_hold(vk, dst);
    return 0;
}

// A rectangle of one image into another of the same format on the card (a blit between surfaces): both stay in GENERAL layout, submitted and waited for. 0 on success.
int rn_vk_image_copy_region(rn_vk *vk, rn_vk_image *src, rn_vk_image *dst, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
    bsync(vk);
    if (!src || !dst || src->fmt != dst->fmt || sx + w > src->w || sy + h > src->h || dx + w > dst->w || dy + h > dst->h) return 6;
    VkCommandBuffer cb = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; int rc = 2;
    VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vk->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vk->AllocateCommandBuffers(vk->dev, &cba, &cb) != VK_SUCCESS) return 2;
    VkCommandBufferBeginInfo cbb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vk->BeginCommandBuffer(cb, &cbb) != VK_SUCCESS) goto done;
    rn_vk_image *both[2] = {src, dst};
    images_in(vk, cb, both, 2);
    VkMemoryBarrier toCopy = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toCopy, 0, NULL, 0, NULL);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffset = {(int32_t)sx, (int32_t)sy, 0}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffset = {(int32_t)dx, (int32_t)dy, 0}, .extent = {w, h, 1}};
    vk->CmdCopyImage(cb, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    VkMemoryBarrier done_ = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT};
    vk->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &done_, 0, NULL, 0, NULL);
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
