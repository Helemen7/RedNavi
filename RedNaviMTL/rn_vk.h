// A small layer over RADV for the Metal plug-in (docs/M6_METAL_ON_RADV.md). RADV's ICD library is opened directly with dlopen, without the Vulkan loader: the ICD only needs
// the system libraries and libzstd next to it (@loader_path), and vk_icdGetInstanceProcAddr gives every entry point. Nothing is opened until rn_vk_open(), so a process that only
// enumerates Metal devices (Dock, Core Animation clients that never encode) does not pay for it or open the kext's user client.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rn_vk rn_vk;
typedef struct rn_vk_buffer rn_vk_buffer;
typedef struct rn_vk_compute rn_vk_compute;

// dir: the directory with libvulkan_radeon.dylib (NULL: $RN_VK_DIR, else /Library/Application Support/RedNavi/vk). err receives a one-line reason on failure.
rn_vk *rn_vk_open(const char *dir, char *err, size_t errlen);
// The directory the libraries are looked for in: $RN_VK_DIR, else /Library/Application Support/RedNavi/vk.
const char *rn_vk_dir(void);
void rn_vk_close(rn_vk *vk);
const char *rn_vk_device_name(const rn_vk *vk);

// Host-visible, host-coherent storage buffer, mapped for good.
rn_vk_buffer *rn_vk_buffer_create(rn_vk *vk, size_t bytes);
void *rn_vk_buffer_contents(rn_vk_buffer *b);
size_t rn_vk_buffer_length(const rn_vk_buffer *b);
void rn_vk_buffer_destroy(rn_vk *vk, rn_vk_buffer *b);

// Images. A 2D, single-level image in RADV memory with a host copy ("staging") next to it, kept in step at the borders of a dispatch: what the host wrote (rn_vk_image_host_wrote)
// is copied in before the dispatch that uses the image, and an image bound as a storage image is copied back after it, so the host copy is current once the dispatch has returned.
// This is the first version of Metal textures (docs/M6_METAL_ON_RADV.md): an IOSurface or any host memory cannot be imported by RADV yet (the kext has no userptr).
typedef struct rn_vk_image rn_vk_image;
typedef struct rn_vk_sampler rn_vk_sampler;
enum { RN_VK_RGBA8 = 1, RN_VK_BGRA8, RN_VK_R8, RN_VK_RG8, RN_VK_R16F, RN_VK_RGBA16F, RN_VK_R32F, RN_VK_RG32F, RN_VK_RGBA32F, RN_VK_R32U, RN_VK_R32I, RN_VK_RGBA8U };
unsigned rn_vk_format_texel_bytes(int format);                      // 0: unknown format
rn_vk_image *rn_vk_image_create(rn_vk *vk, int format, uint32_t width, uint32_t height);   // NULL: format not supported by the card or no memory
void *rn_vk_image_staging(rn_vk_image *img);                        // width * height texels, rows packed (width * texel bytes)
void rn_vk_image_host_wrote(rn_vk_image *img);
void rn_vk_image_destroy(rn_vk *vk, rn_vk_image *img);

// Sampler state as the translator reports it for a sampler written in the shader (RedNaviMTL/rn_air.h) and as MTLSamplerDescriptor has it.
typedef struct {
    uint32_t min_filter, mag_filter;     // 0 nearest, 1 linear
    uint32_t mip_filter;                 // 0 none, 1 nearest, 2 linear
    uint32_t address[3];                 // 0 clamp to zero, 1 clamp to edge, 2 repeat, 3 mirrored repeat, 4 clamp to border
    uint32_t pixel_coords;               // 1: unnormalized coordinates
    uint32_t border;                     // 0 transparent black, 1 opaque black, 2 opaque white
    float lod_min, lod_max;
} rn_vk_sampler_desc;
rn_vk_sampler *rn_vk_sampler_create(rn_vk *vk, const rn_vk_sampler_desc *d);
void rn_vk_sampler_destroy(rn_vk *vk, rn_vk_sampler *s);

// A compute pipeline from SPIR-V (entry point "main"): descriptors at set 0 with the binding numbers and kinds given (at most RN_VK_MAX_BINDINGS) and push_bytes of push constants.
// The resources of a dispatch are passed in the order the bindings were given here. With specialize_local_size the module's local size is the specialization constants 0, 1 and 2 (what
// the AIR translator emits) and a pipeline is built for each local size that is dispatched; without it the module's own local size is used. specialize_local_size 2: the
// translator's threads-dynamic kernels, which also take 48 bytes of push constants (push_bytes) per dispatch region and are run with rn_vk_dispatch_threads.
#define RN_VK_MAX_BINDINGS 32
enum { RN_VK_BUFFER = 0, RN_VK_SAMPLED_IMAGE = 1, RN_VK_STORAGE_IMAGE = 2, RN_VK_SAMPLER = 3, RN_VK_STATIC_SAMPLER = 4 };    // the numbers of rn_air.h
typedef struct { uint32_t binding; uint32_t writable; uint32_t kind; const rn_vk_sampler_desc *sampler; } rn_vk_binding;   // sampler: for RN_VK_STATIC_SAMPLER
rn_vk_compute *rn_vk_compute_create(rn_vk *vk, const uint32_t *spirv, size_t bytes, unsigned nbind, const rn_vk_binding *binds, unsigned push_bytes, int specialize_local_size);
void rn_vk_compute_destroy(rn_vk *vk, rn_vk_compute *c);
unsigned rn_vk_compute_binding_count(const rn_vk_compute *c);

// What one binding gets in a dispatch: buffer (and the byte offset, covering the buffer from there to its end) for a buffer, image for either image kind, sampler for RN_VK_SAMPLER.
// Static samplers take nothing.
typedef struct { rn_vk_buffer *buffer; uint64_t offset; rn_vk_image *image; rn_vk_sampler *sampler; } rn_vk_arg;

// Records one dispatch, submits it to the compute queue and waits for it (0 on success). The first synchronous version: a command buffer of the Metal side will batch these.
// groups: threadgroups per grid; local: threads per threadgroup (used with specialize_local_size, else ignored; NULL: 1,1,1). Vulkan wants a buffer offset to be a multiple of
// rn_vk_storage_offset_alignment(); a binding that is not is refused (return 4). Return 6: a resource is missing or of the wrong kind.
int rn_vk_dispatch(rn_vk *vk, rn_vk_compute *c, const rn_vk_arg *args, const void *push, const uint32_t groups[3], const uint32_t local[3]);
// For specialize_local_size 2: a grid of `threads` threads in threadgroups of `tpt`, any size (the edges get smaller threadgroups, as with Metal's dispatchThreads:).
// Returns what rn_vk_dispatch returns.
int rn_vk_dispatch_threads(rn_vk *vk, rn_vk_compute *c, const rn_vk_arg *args, const uint32_t threads[3], const uint32_t tpt[3]);
uint64_t rn_vk_storage_offset_alignment(const rn_vk *vk);

// The test of tools/mtl_init_harness: the compute shader of mesa-bridge/test/vk_dispatch.comp (v[i] = i * 3 + 7, 64 invocations per group). Prints to stderr, returns 0 when every value is right.
int rn_vk_selftest(const char *dir);

#ifdef __cplusplus
}
#endif
