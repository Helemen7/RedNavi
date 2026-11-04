// The interface between the plug-in (BSD-3-Clause) and the AIR to SPIR-V translator. The translator is NOT part of this code base: it is a separate shared library,
// librn_air2spirv.dylib, looked for next to RADV (rn_vk_dir()), so that a translator under another licence (the candidate, metal2vulkan, is LGPL-3.0-or-later) stays a separate work
// that is dynamically loaded and can be replaced by the user. Nothing here includes or copies translator code. When the library is absent every pipeline from a Metal library fails
// with an error that says so.
//
// int rn_air_translate(const void *bitcode, size_t bytes, const char *entry, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen);
//   bitcode: the LLVM bitcode of one function of a metallib (RedNaviMTL/metallib.h); entry: its name. Returns 0 on success and a SPIR-V module (entry point "main", validated)
//   in *spirv, to be released with rn_air_free(*spirv, *spirv_bytes); non-zero with a one-line reason in err otherwise.
// void rn_air_free(uint32_t *spirv, size_t bytes);
//
// The module has storage buffers at set 0, one per Metal buffer the kernel uses (info.bindings: Metal index, binding number, writable), including the ones the app fills with
// -setBytes:length:atIndex: (a constant argument is a read-only buffer too), no push constants, and the threadgroup size as specialization constants
// info.local_size_spec_ids (0, 1, 2), default info.local_size. Kernels that use textures, samplers or other resources are refused with an error, for now.
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
} rn_air_binding;

typedef struct {
    uint32_t binding_count;
    rn_air_binding bindings[RN_AIR_MAX_BINDINGS];
    uint32_t local_size[3];            // default threads per threadgroup of the module
    uint32_t local_size_spec_ids[3];   // specialization constant ids of the three dimensions
} rn_air_kernel_info;

// 0 on success. dir: where to look for librn_air2spirv.dylib (NULL: rn_vk_dir()).
int rn_air_translate(const char *dir, const void *bitcode, size_t bytes, const char *entry, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen);
void rn_air_free(uint32_t *spirv, size_t bytes);

#ifdef __cplusplus
}
#endif
