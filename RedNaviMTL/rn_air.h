// The interface between the plug-in (BSD-3-Clause) and the AIR to SPIR-V translator. The translator is NOT part of this code base: it is a separate shared library,
// librn_air2spirv.dylib, looked for next to RADV (rn_vk_dir()), so that a translator under another licence (the candidate, metal2vulkan, is LGPL-3.0-or-later) stays a separate work
// that is dynamically loaded and can be replaced by the user. Nothing here includes or copies translator code. When the library is absent every pipeline from a Metal library fails
// with an error that says so.
//
// int rn_air_translate(const void *bitcode, size_t bytes, const char *entry, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen);
//   bitcode: the LLVM bitcode of one function of a metallib (RedNaviMTL/metallib.h); entry: its name. Returns 0 on success and a malloc'ed SPIR-V module (entry point "main",
//   storage buffers at set 0 binding = the Metal buffer index) in *spirv; non-zero with a one-line reason in err otherwise.
// void rn_air_free(void *spirv);
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t buffer_count;     // the kernel reads buffer(0) .. buffer(buffer_count - 1) as storage buffers
    uint32_t push_bytes;       // size of the push constant block that carries the constant data of the kernel (what the app passes with setBytes)
    uint32_t push_index;       // the Metal buffer index that setBytes:length:atIndex: fills; 0xffffffff if the kernel has none
    uint32_t local_size[3];    // threads per threadgroup the module was compiled for
} rn_air_kernel_info;

// 0 on success. dir: where to look for librn_air2spirv.dylib (NULL: rn_vk_dir()).
int rn_air_translate(const char *dir, const void *bitcode, size_t bytes, const char *entry, rn_air_kernel_info *info, uint32_t **spirv, size_t *spirv_bytes, char *err, size_t errlen);
void rn_air_free(uint32_t *spirv);

#ifdef __cplusplus
}
#endif
