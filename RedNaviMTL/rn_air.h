// The interface between the plug-in (BSD-3-Clause) and the AIR to SPIR-V translator. The translator is NOT part of this code base: it is a separate shared library,
// librn_air2spirv.dylib, looked for next to RADV (rn_vk_dir()), so that a translator under another licence (the candidate, metal2vulkan, is LGPL-3.0-or-later) stays a separate work
// that is dynamically loaded and can be replaced by the user. Nothing here includes or copies translator code. When the library is absent every pipeline from a Metal library fails
// with an error that says so.
//
// int rn_air_translate(const void *bitcode, size_t bytes, const char *entry, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen);
//   bitcode: the LLVM bitcode of one function of a metallib (RedNaviMTL/metallib.h); entry: its name. Returns 0 on success and a SPIR-V module (entry point "main", validated)
//   in *spirv, to be released with rn_air_free(*spirv, *spirv_bytes); non-zero with a one-line reason in err otherwise.
//
// The module has descriptors at set 0: storage buffers, one per Metal buffer the kernel uses (info.bindings: Metal index, binding number, writable), including the ones the app fills with
// -setBytes:length:atIndex: (a constant argument is a read-only buffer too), and the threadgroup size as specialization constants info.local_size_spec_ids (0, 1, 2). The grid is
// in push constants (info.dispatch_push_bytes at offset 0): the app's grid is split into regions of whole threadgroups and one smaller threadgroup per edge (rn_vk_dispatch_threads). Textures (sampled or storage images) and samplers set by the app are listed in the same array with their kind;
// the Metal index spaces of buffers, textures and samplers are separate. A storage image has no format in the module (it is read and written without one). Multisampled textures and other resources are refused with an error, for now.
//
// The tools the translator runs: llvm-dis (bitcode to LLVM IR) and spirv-val. rn_air_translate points METAL2VULKAN_LLVM_DIS and METAL2VULKAN_SPIRV_VAL at copies in the
// library's directory when there are some, and otherwise leaves the environment and PATH alone.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define RN_AIR_MAX_BINDINGS 32

typedef struct {
    uint32_t metal_index;      // [[buffer(n)]]
    uint32_t binding;          // the descriptor binding in set 0
    uint32_t writable;         // 0: the kernel only reads it
    uint32_t kind;             // RN_AIR_BUFFER, RN_AIR_TEXTURE (sampled), RN_AIR_STORAGE_IMAGE or RN_AIR_SAMPLER; metal_index is [[texture(n)]] or [[sampler(n)]] for the last three
    uint32_t dim;              // textures: 0 1D, 1 2D, 2 3D, 3 cube, 4 buffer; +8 when arrayed
    // RN_AIR_STATIC_SAMPLER only (a constexpr sampler of the shader; the app does not set it): filters 0 nearest, 1 linear; mip filter 0 none, 1 nearest, 2 linear;
    // address modes 0 clamp to zero, 1 clamp to edge, 2 repeat, 3 mirrored repeat, 4 clamp to border; pixel_coords 1: unnormalized coordinates; border 0 transparent black,
    // 1 opaque black, 2 opaque white.
    uint32_t min_filter, mag_filter, mip_filter, address[3], pixel_coords, border;
    float lod_min, lod_max;
} rn_air_binding;
enum { RN_AIR_BUFFER = 0, RN_AIR_TEXTURE = 1, RN_AIR_STORAGE_IMAGE = 2, RN_AIR_SAMPLER = 3, RN_AIR_STATIC_SAMPLER = 4 };

typedef struct {
    uint32_t binding_count;
    rn_air_binding bindings[RN_AIR_MAX_BINDINGS];
    uint32_t local_size[3];            // default threads per threadgroup of the module
    uint32_t local_size_spec_ids[3];   // specialization constant ids of the three dimensions
    uint32_t dispatch_push_bytes;      // push constants (at offset 0) each dispatch must write: 48 = thread grid, thread base, threadgroup base, threadgroup grid (4 x uint32[3])
    // vertex and fragment stages
    uint32_t set;                      // the descriptor set of the module: vertex 0, fragment 1 (the two stages of a pipeline share one namespace of binding numbers)
    uint32_t render_target_count;      // fragment: the colour attachments it writes (locations)
    uint32_t render_target_locations[8];
    uint32_t uses_vertex_index, uses_instance_index;   // vertex
} rn_air_kernel_info;
enum { RN_AIR_KERNEL = 0, RN_AIR_VERTEX = 1, RN_AIR_FRAGMENT = 2 };

// 0 on success. dir: where to look for librn_air2spirv.dylib (NULL: rn_vk_dir()).
int rn_air_translate(const char *dir, const void *bitcode, size_t bytes, const char *entry, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen);
// The same for a vertex or fragment function (stage: RN_AIR_*). Vertex functions that read their input with [[stage_in]] attributes are refused for now; the varyings of a vertex
// and a fragment function that are translated apart meet by their Location numbers, which the translator derives from the names. A vertex module is in descriptor set 0, a fragment
// module in set 1 (info.set), with the same per-binding ABI as kernels; neither has the dispatch push constants.
int rn_air_translate_stage(const char *dir, int stage, const void *bitcode, size_t bytes, const char *entry, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen);
// A function constant value (newFunctionWithName:constantValues:error:): index is the constant's index, or negative when it is given by name; size bytes at data are the value in
// Metal's layout for its data type (bool 1 byte, int and float 4, vectors the components one after the other).
typedef struct { int32_t index; const char *name; uint32_t size; const void *data; } rn_air_const;
// rn_air_translate_stage for a function specialized with constants: the module is specialized before it is translated, so what a constant switches off is not in the bindings. A
// translator library without rn_air_translate_stage_fc fails here when nconsts is not 0.
int rn_air_translate_stage_fc(const char *dir, int stage, const void *bitcode, size_t bytes, const char *entry, const rn_air_const *consts, unsigned nconsts, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen);
void rn_air_free(uint32_t *spirv, size_t bytes);

#ifdef __cplusplus
}
#endif
