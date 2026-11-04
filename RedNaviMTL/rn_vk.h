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

// A compute pipeline from SPIR-V (entry point "main"): storage buffers at set 0 with the binding numbers given (at most 16) and push_bytes of push constants. The buffers of a
// dispatch are passed in the order the bindings were given here. With specialize_local_size the module's local size is the specialization constants 0, 1 and 2 (what the AIR
// translator emits) and a pipeline is built for each local size that is dispatched; without it the module's own local size is used.
typedef struct { uint32_t binding; uint32_t writable; } rn_vk_binding;
rn_vk_compute *rn_vk_compute_create(rn_vk *vk, const uint32_t *spirv, size_t bytes, unsigned nbind, const rn_vk_binding *binds, unsigned push_bytes, int specialize_local_size);
void rn_vk_compute_destroy(rn_vk *vk, rn_vk_compute *c);
unsigned rn_vk_compute_binding_count(const rn_vk_compute *c);

// Records one dispatch, submits it to the compute queue and waits for it (0 on success). The first synchronous version: a command buffer of the Metal side will batch these.
// groups: threadgroups per grid; local: threads per threadgroup (used with specialize_local_size, else ignored; NULL: 1,1,1). offsets: byte offset of each buffer binding (NULL: all 0);
// each binding covers the buffer from there to its end. Vulkan wants the offset to be a multiple of rn_vk_storage_offset_alignment(); a binding that is not is refused (return 4).
int rn_vk_dispatch(rn_vk *vk, rn_vk_compute *c, rn_vk_buffer *const *buffers, const uint64_t *offsets, const void *push, const uint32_t groups[3], const uint32_t local[3]);
uint64_t rn_vk_storage_offset_alignment(const rn_vk *vk);

// The test of tools/mtl_init_harness: the compute shader of mesa-bridge/test/vk_dispatch.comp (v[i] = i * 3 + 7, 64 invocations per group). Prints to stderr, returns 0 when every value is right.
int rn_vk_selftest(const char *dir);

#ifdef __cplusplus
}
#endif
