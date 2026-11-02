/*
 * ac_apple_iokit: the ac_drm_* functions of Mesa's src/amd/common/ac_linux_drm.h, implemented over the RedNavi user client (RedNavi/rn_uapi.h) instead of DRM ioctls.
 *
 * Inside a Mesa tree this file is not needed: ac_linux_drm.h declares the same functions and the Darwin branch of that header includes only the types below. It is here so
 * that the bridge can be built and run on its own (mesa-bridge/test/bridge_test.c). The prototypes are those of Mesa's ac_linux_drm.h at commit 01bab93 (MIT licence).
 * Return values are 0 or a negative errno, as in libdrm.
 */
#ifndef AC_APPLE_IOKIT_H
#define AC_APPLE_IOKIT_H

#include <stdbool.h>
#include <stdint.h>

#include "amdgpu_compat.h"
#include "rn_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* struct util_sync_provider, src/util/u_sync_provider.h */
struct util_sync_provider {
   int (*create)(struct util_sync_provider *p, uint32_t flags, uint32_t *handle);
   int (*destroy)(struct util_sync_provider *p, uint32_t handle);
   int (*handle_to_fd)(struct util_sync_provider *p, uint32_t handle, int *out_obj_fd);
   int (*fd_to_handle)(struct util_sync_provider *p, int obj_fd, uint32_t *handle);
   int (*import_sync_file)(struct util_sync_provider *p, uint32_t handle, int sync_file_fd);
   int (*export_sync_file)(struct util_sync_provider *p, uint32_t handle, int *out_sync_file_fd);
   int (*wait)(struct util_sync_provider *p, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
   int (*reset)(struct util_sync_provider *p, const uint32_t *handles, uint32_t handle_count);
   int (*signal)(struct util_sync_provider *p, const uint32_t *handles, uint32_t handle_count);
   int (*timeline_signal)(struct util_sync_provider *p, const uint32_t *handles, uint64_t *points, uint32_t handle_count);
   int (*timeline_wait)(struct util_sync_provider *p, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                        uint32_t *first_signaled);
   int (*query)(struct util_sync_provider *p, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags);
   int (*transfer)(struct util_sync_provider *p, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point, uint32_t flags);
   void (*finalize)(struct util_sync_provider *p);
   struct util_sync_provider *(*clone)(struct util_sync_provider *p);
};

struct ac_drm_device;
typedef struct ac_drm_device ac_drm_device;

typedef union ac_drm_bo {
   amdgpu_bo_handle abo;
} ac_drm_bo;

struct ac_drm_bo_import_result {
   ac_drm_bo bo;
   uint64_t alloc_size;
};

int ac_drm_device_initialize(int fd, bool is_virtio, uint32_t *major_version, uint32_t *minor_version, ac_drm_device **device_handle);
struct util_sync_provider *ac_drm_device_get_sync_provider(ac_drm_device *dev);
uintptr_t ac_drm_device_get_cookie(ac_drm_device *dev);
void ac_drm_device_deinitialize(ac_drm_device *dev);
int ac_drm_device_get_fd(ac_drm_device *dev);
int ac_drm_bo_set_metadata(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_metadata *info);
int ac_drm_bo_query_info(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_info *info);
int ac_drm_bo_wait_for_idle(ac_drm_device *dev, ac_drm_bo bo, uint64_t timeout_ns, bool *busy);
int ac_drm_bo_va_op(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr, uint64_t flags, uint32_t ops);
int ac_drm_bo_va_op_raw(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr, uint64_t flags, uint32_t ops);
int ac_drm_bo_va_op_raw2(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr, uint64_t flags, uint32_t ops,
                         uint32_t vm_timeline_syncobj_out, uint64_t vm_timeline_point, uint64_t input_fence_syncobj_handles, uint32_t num_syncobj_handles);
int ac_drm_cs_ctx_create2(ac_drm_device *dev, uint32_t priority, uint32_t *ctx_id);
int ac_drm_cs_ctx_free(ac_drm_device *dev, uint32_t ctx_id);
int ac_drm_cs_ctx_stable_pstate(ac_drm_device *dev, uint32_t ctx_id, uint32_t op, uint32_t flags, uint32_t *out_flags);
int ac_drm_cs_query_reset_state2(ac_drm_device *dev, uint32_t ctx_id, uint64_t *flags);
int ac_drm_cs_query_fence_status(ac_drm_device *dev, uint32_t ctx_id, uint32_t ip_type, uint32_t ip_instance, uint32_t ring, uint64_t fence_seq_no,
                                 uint64_t timeout_ns, uint64_t flags, uint32_t *expired);
int ac_drm_cs_create_syncobj2(ac_drm_device *dev, uint32_t flags, uint32_t *handle);
int ac_drm_cs_destroy_syncobj(ac_drm_device *dev, uint32_t handle);
int ac_drm_cs_syncobj_wait(ac_drm_device *dev, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int ac_drm_cs_syncobj_query2(ac_drm_device *dev, uint32_t *handles, uint64_t *points, unsigned num_handles, uint32_t flags);
int ac_drm_cs_import_syncobj(ac_drm_device *dev, int shared_fd, uint32_t *handle);
int ac_drm_cs_syncobj_export_sync_file(ac_drm_device *dev, uint32_t syncobj, int *sync_file_fd);
int ac_drm_cs_syncobj_import_sync_file(ac_drm_device *dev, uint32_t syncobj, int sync_file_fd);
int ac_drm_cs_syncobj_export_sync_file2(ac_drm_device *dev, uint32_t syncobj, uint64_t point, uint32_t flags, int *sync_file_fd);
int ac_drm_cs_syncobj_transfer(ac_drm_device *dev, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point, uint32_t flags);
int ac_drm_cs_submit_raw2(ac_drm_device *dev, uint32_t ctx_id, uint32_t bo_list_handle, int num_chunks, struct drm_amdgpu_cs_chunk *chunks, uint64_t *seq_no);
void ac_drm_cs_chunk_fence_info_to_data(uint32_t bo_handle, uint64_t offset, struct drm_amdgpu_cs_chunk_data *data);
int ac_drm_cs_syncobj_timeline_wait(ac_drm_device *dev, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                                    uint32_t *first_signaled);
int ac_drm_query_info(ac_drm_device *dev, unsigned info_id, unsigned size, void *value);
int ac_drm_read_mm_registers(ac_drm_device *dev, unsigned dword_offset, unsigned count, uint32_t instance, uint32_t flags, uint32_t *values);
int ac_drm_query_hw_ip_count(ac_drm_device *dev, unsigned type, uint32_t *count);
int ac_drm_query_hw_ip_info(ac_drm_device *dev, unsigned type, unsigned ip_instance, struct drm_amdgpu_info_hw_ip *info);
int ac_drm_query_firmware_version(ac_drm_device *dev, unsigned fw_type, unsigned ip_instance, unsigned index, uint32_t *version, uint32_t *feature);
int ac_drm_query_uq_fw_area_info(ac_drm_device *dev, unsigned type, unsigned ip_instance, struct drm_amdgpu_info_uq_metadata *info);
int ac_drm_query_gpu_info(ac_drm_device *dev, struct amdgpu_gpu_info *info);
int ac_drm_query_heap_info(ac_drm_device *dev, uint32_t heap, uint32_t flags, struct amdgpu_heap_info *info);
int ac_drm_query_sensor_info(ac_drm_device *dev, unsigned sensor_type, unsigned size, void *value);
int ac_drm_query_video_caps_info(ac_drm_device *dev, unsigned cap_type, unsigned size, void *value);
int ac_drm_query_gpuvm_fault_info(ac_drm_device *dev, unsigned size, void *value);
int ac_drm_vm_reserve_vmid(ac_drm_device *dev, uint32_t flags);
int ac_drm_vm_unreserve_vmid(ac_drm_device *dev, uint32_t flags);
const char *ac_drm_get_marketing_name(ac_drm_device *device);
int ac_drm_query_sw_info(ac_drm_device *dev, enum amdgpu_sw_info info, void *value);
int ac_drm_bo_alloc(ac_drm_device *dev, struct amdgpu_bo_alloc_request *alloc_buffer, ac_drm_bo *bo);
int ac_drm_bo_export(ac_drm_device *dev, ac_drm_bo bo, enum amdgpu_bo_handle_type type, uint32_t *shared_handle);
int ac_drm_bo_import(ac_drm_device *dev, enum amdgpu_bo_handle_type type, uint32_t shared_handle, struct ac_drm_bo_import_result *output);
int ac_drm_create_bo_from_user_mem(ac_drm_device *dev, void *cpu, uint64_t size, ac_drm_bo *bo);
int ac_drm_bo_free(ac_drm_device *dev, ac_drm_bo bo);
int ac_drm_bo_cpu_map(ac_drm_device *dev, ac_drm_bo bo, void **cpu);
int ac_drm_bo_cpu_unmap(ac_drm_device *dev, ac_drm_bo bo);
int ac_drm_va_range_alloc(ac_drm_device *dev, enum amdgpu_gpu_va_range va_range_type, uint64_t size, uint64_t va_base_alignment, uint64_t va_base_required,
                          uint64_t *va_base_allocated, amdgpu_va_handle *va_range_handle, uint64_t flags);
int ac_drm_va_range_free(amdgpu_va_handle va_range_handle);
int ac_drm_va_range_query(ac_drm_device *dev, enum amdgpu_gpu_va_range type, uint64_t *start, uint64_t *end);

/* Not part of ac_drm_*: the PCI location, which ac_drm_query_pci_bus_info of the Mesa tree puts into struct radeon_info. */
int ac_apple_pci_bus_info(ac_drm_device *dev, uint16_t *domain, uint8_t *bus, uint8_t *device, uint8_t *function);

#ifdef __cplusplus
}
#endif

#endif
