/*
 * ac_apple_iokit: Mesa's ac_drm_* layer over the RedNavi user client.
 *
 * Written for the Mesa fork that RADV runs on macOS through (docs/M5_MESA_BUILD.md), where it takes the place of ac_linux_drm.c: every function below has the signature
 * of its namesake in src/amd/common/ac_linux_drm.h and does what the DRM ioctl does, with one IOConnectCallStructMethod to the kext (RedNavi/rn_uapi.h) in between.
 * Mesa is MIT licensed and so is this file when it lives in that tree; in the RedNavi repository it is under the licence of the project.
 *
 * What the kext does not have yet, and what that means here:
 *   - memory is wired host memory for every domain (VRAM requests are served from it): up to 1 GB per buffer, 2 GB in all (the kext's boot-arg rednavi_bo_budget_mb), 1024 buffers per connection;
 *   - one compute queue: every context shares it and its sequence numbers; graphics and SDMA submits are refused;
 *   - a command buffer must lie in the lower half of the address space (a command buffer in the upper half hangs the queue); only 32-bit buffers get upper-half addresses;
 *   - no external handles (dma-buf, sync files, opaque fds), no user queues, no sensors, no timestamp.
 */
#include "ac_apple_iokit.h"

#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RN_WAIT_SLICE_MS 5000u                                /* the kext waits at most 10 s per call; longer waits are made of slices */

struct amdgpu_bo {
   uint32_t handle;
   uint32_t placement;                                        /* what the kext gave: 0 host memory, RN_BO_VRAM device-local, RN_BO_VRAM_VISIBLE CPU-visible VRAM */
   uint64_t size;
   void *cpu;
   mach_vm_size_t cpu_len;
};

struct vamgr_seg {
   uint64_t base, size;
};

struct vamgr {
   struct vamgr_seg *free_list;                               /* sorted by base, no two adjacent */
   uint32_t n, cap;
   uint64_t start, end;
};

struct amdgpu_va {
   struct ac_drm_device *dev;
   struct vamgr *mgr;
   uint64_t base, size;
};

struct ac_drm_device {
   io_connect_t conn;
   struct rn_uc_info info;
   struct drm_amdgpu_info_device dev_info;
   pthread_mutex_t lock;
   struct vamgr low, high32;
   struct amdgpu_bo **bos;                                    /* indexed by the kext's buffer handle, 1 to info.bo_limit */
   uint32_t nbos;
   uint32_t next_ctx;
   struct { uint32_t handle; uint64_t va, offset, size; } maps[4096];   /* what is mapped where, for RN_BRIDGE_DUMP only */
   uint32_t nmaps;
   int refs;                                                  /* the device and every clone of its sync provider (vk_device keeps one past the winsys) */
   struct util_sync_provider sync;
};

/* ---------------------------------------------------------------------------------------------------------------------------------------------- errors, time */

static int errno_of(IOReturn kr)
{
   switch (kr) {
   case kIOReturnSuccess: return 0;
   case kIOReturnBadArgument: return -EINVAL;
   case kIOReturnTimeout: return -ETIME;
   case kIOReturnNoResources: case kIOReturnNoMemory: return -ENOMEM;
   case kIOReturnNoSpace: return -ENOSPC;
   case kIOReturnBusy: return -EBUSY;
   case kIOReturnUnsupported: return -EOPNOTSUPP;
   case kIOReturnNotPrivileged: return -EPERM;
   case kIOReturnNotReady: return -EAGAIN;
   default: return -EIO;
   }
}

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int call(struct ac_drm_device *dev, uint32_t selector, const void *in, size_t in_size, void *out, size_t out_size)
{
   size_t n = out_size;
   kern_return_t kr = IOConnectCallStructMethod(dev->conn, selector, in, in_size, out, out ? &n : NULL);
   if (kr != KERN_SUCCESS && getenv("RN_BRIDGE_DEBUG"))
      fprintf(stderr, "rn-bridge: selector %u returned 0x%x (errno %d)\n", selector, kr, errno_of(kr));
   return errno_of(kr);
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------- virtual addresses */

/* The kernel does not hand out addresses (the user does, DRM_AMDGPU_GEM_VA maps at an address given), libdrm keeps a manager per range in user space. Same here: a sorted list of free segments. */
static void vamgr_init(struct vamgr *m, uint64_t start, uint64_t end)
{
   m->start = start;
   m->end = end;
   m->n = 1;
   m->cap = 16;
   m->free_list = calloc(m->cap, sizeof(*m->free_list));
   m->free_list[0].base = start;
   m->free_list[0].size = end - start;
}

static int vamgr_alloc(struct vamgr *m, uint64_t size, uint64_t align, uint64_t required, uint64_t *out)
{
   if (align == 0)
      align = 1;
   for (uint32_t i = 0; i < m->n; i++) {
      struct vamgr_seg *s = &m->free_list[i];
      uint64_t base = required ? required : (s->base + align - 1) / align * align;
      if (base < s->base || base + size > s->base + s->size || base + size < base)
         continue;
      uint64_t head = base - s->base, tail = s->base + s->size - (base + size);
      if (head && tail) {                                      /* split: the tail becomes a new segment */
         if (m->n == m->cap) {
            m->cap *= 2;
            m->free_list = realloc(m->free_list, m->cap * sizeof(*m->free_list));
            s = &m->free_list[i];
         }
         memmove(&m->free_list[i + 2], &m->free_list[i + 1], (m->n - i - 1) * sizeof(*m->free_list));
         m->free_list[i + 1].base = base + size;
         m->free_list[i + 1].size = tail;
         s->size = head;
         m->n++;
      } else if (head) {
         s->size = head;
      } else if (tail) {
         s->base = base + size;
         s->size = tail;
      } else {
         memmove(s, s + 1, (m->n - i - 1) * sizeof(*s));
         m->n--;
      }
      *out = base;
      return 0;
   }
   return -ENOMEM;
}

static void vamgr_free(struct vamgr *m, uint64_t base, uint64_t size)
{
   uint32_t i = 0;
   while (i < m->n && m->free_list[i].base < base)
      i++;
   int merge_prev = i > 0 && m->free_list[i - 1].base + m->free_list[i - 1].size == base;
   int merge_next = i < m->n && base + size == m->free_list[i].base;
   if (merge_prev && merge_next) {
      m->free_list[i - 1].size += size + m->free_list[i].size;
      memmove(&m->free_list[i], &m->free_list[i + 1], (m->n - i - 1) * sizeof(*m->free_list));
      m->n--;
   } else if (merge_prev) {
      m->free_list[i - 1].size += size;
   } else if (merge_next) {
      m->free_list[i].base = base;
      m->free_list[i].size += size;
   } else {
      if (m->n == m->cap) {
         m->cap *= 2;
         m->free_list = realloc(m->free_list, m->cap * sizeof(*m->free_list));
      }
      memmove(&m->free_list[i + 1], &m->free_list[i], (m->n - i) * sizeof(*m->free_list));
      m->free_list[i].base = base;
      m->free_list[i].size = size;
      m->n++;
   }
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------- sync provider */

#define PROVIDER_DEV(p) ((struct ac_drm_device *)((char *)(p) - offsetof(struct ac_drm_device, sync)))

/* DRM's syncobj wait takes an absolute CLOCK_MONOTONIC deadline in nanoseconds; the kext takes a relative timeout of at most 10 s. A longer wait is a loop of slices. */
static int syncobj_wait_points(struct ac_drm_device *dev, uint32_t *handles, const uint64_t *points, unsigned n, int64_t deadline_ns, unsigned flags,
                               uint32_t *first_signaled)
{
   if (n == 0 || n > RN_UC_SYNC_MAX)
      return -EINVAL;
   struct rn_uc_syncobj_wait in;
   struct rn_uc_syncobj_wait_result out;
   for (;;) {
      int64_t now = (int64_t)now_ns();
      int64_t rel = deadline_ns > now ? deadline_ns - now : 0;
      if (rel > (int64_t)RN_WAIT_SLICE_MS * 1000000)
         rel = (int64_t)RN_WAIT_SLICE_MS * 1000000;
      memset(&in, 0, sizeof(in));
      in.timeout_nsec = rel;
      in.count = n;
      in.flags = flags & (RN_UC_SYNCOBJ_WAIT_ALL | RN_UC_SYNCOBJ_WAIT_FOR_SUBMIT | RN_UC_SYNCOBJ_WAIT_AVAILABLE);
      for (unsigned i = 0; i < n; i++) {
         in.handles[i] = handles[i];
         in.points[i] = points ? points[i] : 0;
      }
      int r = call(dev, RN_UC_SYNCOBJ_WAIT, &in, sizeof(in), &out, sizeof(out));
      if (r == 0) {
         if (first_signaled)
            *first_signaled = out.first_signaled;
         return 0;
      }
      if (r != -ETIME)
         return r;
      if ((int64_t)now_ns() >= deadline_ns)
         return -ETIME;
   }
}

static int sp_create(struct util_sync_provider *p, uint32_t flags, uint32_t *handle)
{
   struct rn_uc_syncobj_create in = {flags & RN_UC_SYNCOBJ_CREATE_SIGNALED, 0};
   struct rn_uc_syncobj_create_result out;
   int r = call(PROVIDER_DEV(p), RN_UC_SYNCOBJ_CREATE, &in, sizeof(in), &out, sizeof(out));
   if (r == 0)
      *handle = out.handle;
   return r;
}

static int sp_destroy(struct util_sync_provider *p, uint32_t handle)
{
   struct rn_uc_syncobj_destroy in = {handle, 0};
   return call(PROVIDER_DEV(p), RN_UC_SYNCOBJ_DESTROY, &in, sizeof(in), NULL, 0);
}

static int sp_unsupported_fd(struct util_sync_provider *p, uint32_t handle, int *fd) { (void)p; (void)handle; (void)fd; return -ENOSYS; }
static int sp_unsupported_fd_to_handle(struct util_sync_provider *p, int fd, uint32_t *handle) { (void)p; (void)fd; (void)handle; return -ENOSYS; }
static int sp_unsupported_import(struct util_sync_provider *p, uint32_t handle, int fd) { (void)p; (void)handle; (void)fd; return -ENOSYS; }

static int sp_wait(struct util_sync_provider *p, uint32_t *handles, unsigned n, int64_t timeout_nsec, unsigned flags, uint32_t *first)
{
   return syncobj_wait_points(PROVIDER_DEV(p), handles, NULL, n, timeout_nsec, flags, first);
}

static int sp_timeline_wait(struct util_sync_provider *p, uint32_t *handles, uint64_t *points, unsigned n, int64_t timeout_nsec, unsigned flags,
                            uint32_t *first)
{
   return syncobj_wait_points(PROVIDER_DEV(p), handles, points, n, timeout_nsec, flags, first);
}

static int sp_array(struct ac_drm_device *dev, uint32_t selector, const uint32_t *handles, const uint64_t *points, uint32_t count)
{
   for (uint32_t done = 0; done < count; done += RN_UC_SYNC_MAX) {
      struct rn_uc_syncobj_array in;
      uint32_t n = count - done < RN_UC_SYNC_MAX ? count - done : RN_UC_SYNC_MAX;
      memset(&in, 0, sizeof(in));
      in.count = n;
      for (uint32_t i = 0; i < n; i++) {
         in.handles[i] = handles[done + i];
         in.points[i] = points ? points[done + i] : 0;
      }
      int r = call(dev, selector, &in, sizeof(in), NULL, 0);
      if (r)
         return r;
   }
   return 0;
}

static int sp_reset(struct util_sync_provider *p, const uint32_t *handles, uint32_t count) { return sp_array(PROVIDER_DEV(p), RN_UC_SYNCOBJ_RESET, handles, NULL, count); }
static int sp_signal(struct util_sync_provider *p, const uint32_t *handles, uint32_t count) { return sp_array(PROVIDER_DEV(p), RN_UC_SYNCOBJ_SIGNAL, handles, NULL, count); }
static int sp_timeline_signal(struct util_sync_provider *p, const uint32_t *handles, uint64_t *points, uint32_t count)
{
   return sp_array(PROVIDER_DEV(p), RN_UC_SYNCOBJ_SIGNAL, handles, points, count);
}

static int sp_query(struct util_sync_provider *p, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags)
{
   (void)flags;                                               /* DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED: the kext answers the signaled value only */
   struct ac_drm_device *dev = PROVIDER_DEV(p);
   for (uint32_t done = 0; done < count; done += RN_UC_SYNC_MAX) {
      struct rn_uc_syncobj_array in, out;
      uint32_t n = count - done < RN_UC_SYNC_MAX ? count - done : RN_UC_SYNC_MAX;
      memset(&in, 0, sizeof(in));
      in.count = n;
      for (uint32_t i = 0; i < n; i++)
         in.handles[i] = handles[done + i];
      int r = call(dev, RN_UC_SYNCOBJ_QUERY, &in, sizeof(in), &out, sizeof(out));
      if (r)
         return r;
      for (uint32_t i = 0; i < n; i++)
         points[done + i] = out.points[i];
   }
   return 0;
}

static int sp_transfer(struct util_sync_provider *p, uint32_t dst, uint64_t dst_point, uint32_t src, uint64_t src_point, uint32_t flags)
{
   struct rn_uc_syncobj_transfer in = {src, dst, src_point, dst_point, flags, 0};
   return call(PROVIDER_DEV(p), RN_UC_SYNCOBJ_TRANSFER, &in, sizeof(in), NULL, 0);
}

static void dev_release(struct ac_drm_device *dev);

static void sp_finalize(struct util_sync_provider *p) { dev_release(PROVIDER_DEV(p)); }

/* vk_device owns a clone and finalizes it after the winsys is gone, so a clone is a reference to the same device */
static struct util_sync_provider *sp_clone(struct util_sync_provider *p)
{
   struct ac_drm_device *dev = PROVIDER_DEV(p);
   __atomic_add_fetch(&dev->refs, 1, __ATOMIC_RELAXED);
   return &dev->sync;
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------- device */

static int info_query(struct ac_drm_device *dev, unsigned id, unsigned size, void *value, const struct drm_amdgpu_info *extra)
{
   struct drm_amdgpu_info req;
   if (extra)
      req = *extra;
   else
      memset(&req, 0, sizeof(req));
   req.return_pointer = 0;                                    /* the answer comes back in the output structure, not through a pointer */
   req.return_size = size;
   req.query = id;
   if (size == 0 || size > 4096)
      return -EINVAL;
   return call(dev, RN_UC_INFO_QUERY, &req, sizeof(req), value, size);
}

int ac_drm_device_initialize(int fd, bool is_virtio, uint32_t *major_version, uint32_t *minor_version, ac_drm_device **device_handle)
{
   (void)fd;
   (void)is_virtio;
   io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedNaviProbe"));
   if (!svc)
      return -ENODEV;
   struct ac_drm_device *dev = calloc(1, sizeof(*dev));
   if (!dev) {
      IOObjectRelease(svc);
      return -ENOMEM;
   }
   kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &dev->conn);
   IOObjectRelease(svc);
   if (kr != KERN_SUCCESS) {
      free(dev);
      return errno_of(kr);
   }
   pthread_mutex_init(&dev->lock, NULL);
   int r = call(dev, RN_UC_INFO, NULL, 0, &dev->info, sizeof(dev->info));
   if (r == 0 && dev->info.version != RN_UAPI_VERSION)
      r = -EPROTO;                                            /* the kext and the bridge were built for different interfaces */
   if (r == 0)
      r = info_query(dev, AMDGPU_INFO_DEV_INFO, sizeof(dev->dev_info), &dev->dev_info, NULL);
   if (r == 0) {
      dev->nbos = dev->info.bo_limit + 1;
      dev->bos = calloc(dev->nbos, sizeof(*dev->bos));
      if (!dev->bos)
         r = -ENOMEM;
   }
   if (r) {
      IOServiceClose(dev->conn);
      free(dev);
      return r;
   }
   /* The lower range from where DEV_INFO says it starts to its maximum; the 32-bit range is the 4 GB of the upper half whose high dword is address32_hi. */
   vamgr_init(&dev->low, dev->dev_info.virtual_address_offset, dev->dev_info.virtual_address_max);
   vamgr_init(&dev->high32, dev->dev_info.high_va_offset, dev->dev_info.high_va_offset + (1ull << 32));
   dev->next_ctx = 1;
   dev->refs = 1;
   dev->sync = (struct util_sync_provider) {
      .create = sp_create, .destroy = sp_destroy, .handle_to_fd = sp_unsupported_fd, .fd_to_handle = sp_unsupported_fd_to_handle,
      .import_sync_file = sp_unsupported_import, .export_sync_file = sp_unsupported_fd, .wait = sp_wait, .reset = sp_reset, .signal = sp_signal,
      .timeline_signal = sp_timeline_signal, .timeline_wait = sp_timeline_wait, .query = sp_query, .transfer = sp_transfer, .finalize = sp_finalize,
      .clone = sp_clone,
   };
   *major_version = 3;                                        /* what a DRM_AMDGPU_INFO user checks before it trusts the interface */
   *minor_version = 64;
   *device_handle = dev;
   return 0;
}

struct util_sync_provider *ac_drm_device_get_sync_provider(ac_drm_device *dev) { return &dev->sync; }
uintptr_t ac_drm_device_get_cookie(ac_drm_device *dev) { return (uintptr_t)dev; }
int ac_drm_device_get_fd(ac_drm_device *dev) { (void)dev; return -1; }

void ac_drm_device_deinitialize(ac_drm_device *dev)
{
   if (dev)
      dev_release(dev);
}

static void dev_release(struct ac_drm_device *dev)
{
   if (__atomic_sub_fetch(&dev->refs, 1, __ATOMIC_ACQ_REL) != 0)
      return;
   IOServiceClose(dev->conn);                                 /* the kext frees the buffers, the address space and the syncobjs of a closed connection */
   free(dev->low.free_list);
   free(dev->high32.free_list);
   free(dev->bos);
   pthread_mutex_destroy(&dev->lock);
   free(dev);
}

const char *ac_drm_get_marketing_name(ac_drm_device *device)
{
   (void)device;
   return "AMD Radeon RX 7800 XT";
}

int ac_apple_pci_bus_info(ac_drm_device *dev, uint16_t *domain, uint8_t *bus, uint8_t *device, uint8_t *function)
{
   (void)dev;
   *domain = 0;                                               /* the kext does not publish the location yet (the Linux run had 0000:03:00.0) */
   *bus = 3;
   *device = 0;
   *function = 0;
   return 0;
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------- queries */

int ac_drm_query_info(ac_drm_device *dev, unsigned info_id, unsigned size, void *value)
{
   /* The kext takes one IB per submit and says so, but this layer sends the IBs of a RADV submission one after the other, so RADV may use its own default (124 for compute).
    * With the kext's 1, a submission that has a preamble and a command buffer never fits and RADV's submit loop does not end. */
   if (info_id == AMDGPU_INFO_MAX_IBS)
      return -EINVAL;
   return info_query(dev, info_id, size, value, NULL);
}

int ac_drm_read_mm_registers(ac_drm_device *dev, unsigned dword_offset, unsigned count, uint32_t instance, uint32_t flags, uint32_t *values)
{
   struct drm_amdgpu_info req;
   memset(&req, 0, sizeof(req));
   req.read_mmr_reg.dword_offset = dword_offset;
   req.read_mmr_reg.count = count;
   req.read_mmr_reg.instance = instance;
   req.read_mmr_reg.flags = flags;
   return info_query(dev, AMDGPU_INFO_READ_MMR_REG, count * sizeof(uint32_t), values, &req);
}

int ac_drm_query_hw_ip_count(ac_drm_device *dev, unsigned type, uint32_t *count)
{
   struct drm_amdgpu_info req;
   memset(&req, 0, sizeof(req));
   req.query_hw_ip.type = type;
   return info_query(dev, AMDGPU_INFO_HW_IP_COUNT, sizeof(*count), count, &req);
}

int ac_drm_query_hw_ip_info(ac_drm_device *dev, unsigned type, unsigned ip_instance, struct drm_amdgpu_info_hw_ip *info)
{
   struct drm_amdgpu_info req;
   memset(&req, 0, sizeof(req));
   req.query_hw_ip.type = type;
   req.query_hw_ip.ip_instance = ip_instance;
   int r = info_query(dev, AMDGPU_INFO_HW_IP_INFO, sizeof(*info), info, &req);
   if (r || type != AMDGPU_HW_IP_GFX || info->ip_discovery_version)
      return r;
   /* The kext has no graphics queue and answers all zero. Mesa takes the GFX level from the GFX IP version, so give it that of the compute engine (the same GC block) with no rings:
    * num_queues stays 0 and RADV offers compute queues only. */
   req.query_hw_ip.type = AMDGPU_HW_IP_COMPUTE;
   r = info_query(dev, AMDGPU_INFO_HW_IP_INFO, sizeof(*info), info, &req);
   info->available_rings = 0;
   return r;
}

int ac_drm_query_firmware_version(ac_drm_device *dev, unsigned fw_type, unsigned ip_instance, unsigned index, uint32_t *version, uint32_t *feature)
{
   struct drm_amdgpu_info req;
   struct drm_amdgpu_info_firmware fw;
   memset(&req, 0, sizeof(req));
   req.query_fw.fw_type = fw_type;
   req.query_fw.ip_instance = ip_instance;
   req.query_fw.index = index;
   int r = info_query(dev, AMDGPU_INFO_FW_VERSION, sizeof(fw), &fw, &req);
   if (r)
      return r;
   *version = fw.ver;
   *feature = fw.feature;
   return 0;
}

int ac_drm_query_uq_fw_area_info(ac_drm_device *dev, unsigned type, unsigned ip_instance, struct drm_amdgpu_info_uq_metadata *info)
{
   (void)dev; (void)type; (void)ip_instance; (void)info;
   return -EOPNOTSUPP;                                        /* user queues */
}

/* As libdrm's amdgpu_query_gpu_info and Mesa's ac_drm_query_gpu_info: DEV_INFO and, below GFX9 only, registers. The one register read left is GB_ADDR_CONFIG. */
int ac_drm_query_gpu_info(ac_drm_device *dev, struct amdgpu_gpu_info *info)
{
   const struct drm_amdgpu_info_device *d = &dev->dev_info;
   memset(info, 0, sizeof(*info));
   info->asic_id = d->device_id;
   info->chip_rev = d->chip_rev;
   info->chip_external_rev = d->external_rev;
   info->family_id = d->family;
   info->max_engine_clk = d->max_engine_clock;
   info->max_memory_clk = d->max_memory_clock;
   info->gpu_counter_freq = d->gpu_counter_freq;
   info->enabled_rb_pipes_mask = d->enabled_rb_pipes_mask;
   info->rb_pipes = d->num_rb_pipes;
   info->ids_flags = d->ids_flags;
   info->num_hw_gfx_contexts = d->num_hw_gfx_contexts;
   info->num_shader_engines = d->num_shader_engines;
   info->num_shader_arrays_per_engine = d->num_shader_arrays_per_engine;
   info->vram_type = d->vram_type;
   info->vram_bit_width = d->vram_bit_width;
   info->ce_ram_size = d->ce_ram_size;
   info->vce_harvest_config = d->vce_harvest_config;
   info->pci_rev_id = d->pci_rev;
   int r = ac_drm_read_mm_registers(dev, 0x263e, 1, 0xffffffff, 0, &info->gb_addr_cfg);
   if (r)
      return r;
   info->cu_active_number = d->cu_active_number;
   info->cu_ao_mask = d->cu_ao_mask;
   memcpy(&info->cu_bitmap[0][0], &d->cu_bitmap[0][0], sizeof(info->cu_bitmap));
   return 0;
}

int ac_drm_query_heap_info(ac_drm_device *dev, uint32_t heap, uint32_t flags, struct amdgpu_heap_info *info)
{
   struct drm_amdgpu_info_vram_gtt vg;
   int r = ac_drm_query_info(dev, AMDGPU_INFO_VRAM_GTT, sizeof(vg), &vg);
   if (r)
      return r;
   switch (heap) {
   case AMDGPU_GEM_DOMAIN_VRAM:
      info->heap_size = (flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) ? vg.vram_cpu_accessible_size : vg.vram_size;
      info->max_allocation = vg.vram_cpu_accessible_size;
      return ac_drm_query_info(dev, (flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) ? AMDGPU_INFO_VIS_VRAM_USAGE : AMDGPU_INFO_VRAM_USAGE,
                               sizeof(info->heap_usage), &info->heap_usage);
   case AMDGPU_GEM_DOMAIN_GTT:
      info->heap_size = vg.gtt_size;
      info->max_allocation = vg.vram_cpu_accessible_size;
      return ac_drm_query_info(dev, AMDGPU_INFO_GTT_USAGE, sizeof(info->heap_usage), &info->heap_usage);
   default:
      return -EINVAL;
   }
}

int ac_drm_query_sensor_info(ac_drm_device *dev, unsigned sensor_type, unsigned size, void *value)
{
   (void)dev; (void)sensor_type; (void)size; (void)value;
   return -EOPNOTSUPP;                                        /* the SMU is not asked for sensors yet */
}

int ac_drm_query_video_caps_info(ac_drm_device *dev, unsigned cap_type, unsigned size, void *value)
{
   (void)dev; (void)cap_type; (void)size; (void)value;
   return -EOPNOTSUPP;
}

int ac_drm_query_gpuvm_fault_info(ac_drm_device *dev, unsigned size, void *value)
{
   return ac_drm_query_info(dev, AMDGPU_INFO_GPUVM_FAULT, size, value);
}

int ac_drm_vm_reserve_vmid(ac_drm_device *dev, uint32_t flags) { (void)dev; (void)flags; return 0; }
int ac_drm_vm_unreserve_vmid(ac_drm_device *dev, uint32_t flags) { (void)dev; (void)flags; return 0; }

int ac_drm_query_sw_info(ac_drm_device *dev, enum amdgpu_sw_info info, void *value)
{
   switch (info) {
   case amdgpu_sw_info_address32_hi:
      *(uint32_t *)value = (uint32_t)(dev->dev_info.high_va_offset >> 32);
      return 0;
   case amdgpu_sw_info_address_prt_wa_control_bit:
      *(uint32_t *)value = ~0u;                                /* "ignore", as the docs of this query say: no half of the address space is reserved for sparse residency here */
      return 0;
   default:
      return -EINVAL;
   }
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------- buffers */

int ac_drm_bo_alloc(ac_drm_device *dev, struct amdgpu_bo_alloc_request *req, ac_drm_bo *bo)
{
   /* GDS, GWS and OA do not exist. VRAM requests come from the kext's VRAM allocator: a request that says NO_CPU_ACCESS gets the device-local pool the CPU cannot touch, any other the small
    * CPU-visible part of the aperture; a request for VRAM|GTT falls back to host memory when VRAM has no room.
    * RN_BRIDGE_NO_VRAM=1 serves everything from host memory, for comparison. */
   if (req->alloc_size == 0 || (req->preferred_heap & ~(uint32_t)(AMDGPU_GEM_DOMAIN_VRAM | AMDGPU_GEM_DOMAIN_GTT)))
      return -EINVAL;
   /* A VRAM|GTT buffer the CPU may map (command buffers, shader code, descriptors, uploads) goes to host memory: the CPU-visible VRAM is small and a write-combined mapping is slow
    * to read. The device-local pool is for NO_CPU_ACCESS buffers, the visible part for what asks for VRAM alone. */
   const int cpu_may_map = !(req->flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS) || (req->flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED);
   const int want_vram = (req->preferred_heap & AMDGPU_GEM_DOMAIN_VRAM) && !getenv("RN_BRIDGE_NO_VRAM") &&
                         !(cpu_may_map && (req->preferred_heap & AMDGPU_GEM_DOMAIN_GTT) && !getenv("RN_BRIDGE_VRAM_FIRST"));
   const int allow_gtt = (req->preferred_heap & AMDGPU_GEM_DOMAIN_GTT) || !(req->preferred_heap & AMDGPU_GEM_DOMAIN_VRAM) || !want_vram;
   struct rn_uc_bo_info out;
   int r = -ENOMEM;
   if (want_vram) {
      /* Linux may place a VRAM buffer anywhere and moves it to the visible part when the CPU maps it; this kext cannot move buffers, so only a buffer that says NO_CPU_ACCESS goes
       * to the device-local pool (the CPU never maps it); every other VRAM buffer gets the CPU-visible part of the aperture. */
      const uint32_t kind = (req->flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS) && !(req->flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) ? RN_BO_VRAM : RN_BO_VRAM_VISIBLE;
      struct rn_uc_bo_create in = {req->alloc_size, kind, 0};
      r = call(dev, RN_UC_BO_CREATE, &in, sizeof(in), &out, sizeof(out));
   }
   if (r && allow_gtt) {
      struct rn_uc_bo_create in = {req->alloc_size, 0, 0};
      r = call(dev, RN_UC_BO_CREATE, &in, sizeof(in), &out, sizeof(out));
   }
   if (r)
      return r;
   struct amdgpu_bo *b = calloc(1, sizeof(*b));
   if (!b || out.handle >= dev->nbos) {
      struct rn_uc_bo_free f = {out.handle, 0};
      call(dev, RN_UC_BO_FREE, &f, sizeof(f), NULL, 0);
      free(b);
      return -ENOMEM;
   }
   b->handle = out.handle;
   b->size = out.size;
   b->placement = out.flags;
   if (getenv("RN_BRIDGE_DEBUG"))
      fprintf(stderr, "rn-bridge: bo %u, %llu bytes, domain 0x%x flags 0x%llx -> placement %u\n", b->handle, (unsigned long long)out.size, req->preferred_heap, (unsigned long long)req->flags, b->placement);
   pthread_mutex_lock(&dev->lock);
   dev->bos[b->handle] = b;
   pthread_mutex_unlock(&dev->lock);
   bo->abo = b;
   return 0;
}

int ac_drm_bo_free(ac_drm_device *dev, ac_drm_bo bo)
{
   struct amdgpu_bo *b = bo.abo;
   if (!b)
      return -EINVAL;
   if (b->cpu)
      ac_drm_bo_cpu_unmap(dev, bo);
   struct rn_uc_bo_free f = {b->handle, 0};
   int r = call(dev, RN_UC_BO_FREE, &f, sizeof(f), NULL, 0);
   pthread_mutex_lock(&dev->lock);
   dev->bos[b->handle] = NULL;
   pthread_mutex_unlock(&dev->lock);
   free(b);
   return r;
}

int ac_drm_bo_cpu_map(ac_drm_device *dev, ac_drm_bo bo, void **cpu)
{
   struct amdgpu_bo *b = bo.abo;
   if (b->placement == RN_BO_VRAM)
      return -EINVAL;                                          /* device-local VRAM: only the GPU reaches it */
   if (!b->cpu) {
      mach_vm_address_t addr = 0;
      mach_vm_size_t len = 0;
      kern_return_t kr = IOConnectMapMemory64(dev->conn, b->handle, mach_task_self(), &addr, &len, kIOMapAnywhere);
      if (kr != KERN_SUCCESS)
         return errno_of(kr);
      b->cpu = (void *)(uintptr_t)addr;
      b->cpu_len = len;
   }
   *cpu = b->cpu;
   return 0;
}

int ac_drm_bo_cpu_unmap(ac_drm_device *dev, ac_drm_bo bo)
{
   struct amdgpu_bo *b = bo.abo;
   if (!b->cpu)
      return 0;
   kern_return_t kr = IOConnectUnmapMemory64(dev->conn, b->handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)b->cpu);
   b->cpu = NULL;
   return errno_of(kr);
}

int ac_drm_bo_export(ac_drm_device *dev, ac_drm_bo bo, enum amdgpu_bo_handle_type type, uint32_t *shared_handle)
{
   (void)dev;
   if (type != amdgpu_bo_handle_type_kms)
      return -EOPNOTSUPP;                                      /* no dma-buf or flink names on macOS */
   *shared_handle = bo.abo->handle;
   return 0;
}

int ac_drm_bo_import(ac_drm_device *dev, enum amdgpu_bo_handle_type type, uint32_t shared_handle, struct ac_drm_bo_import_result *output)
{
   if (type != amdgpu_bo_handle_type_kms || shared_handle >= dev->nbos || !dev->bos[shared_handle])
      return -EOPNOTSUPP;
   output->bo.abo = dev->bos[shared_handle];
   output->alloc_size = dev->bos[shared_handle]->size;
   return 0;
}

int ac_drm_create_bo_from_user_mem(ac_drm_device *dev, void *cpu, uint64_t size, ac_drm_bo *bo)
{
   (void)dev; (void)cpu; (void)size; (void)bo;
   return -EOPNOTSUPP;
}

int ac_drm_bo_set_metadata(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_metadata *info)
{
   (void)dev; (void)bo_handle; (void)info;
   return 0;                                                  /* tiling and metadata stay in user space: nothing to share with another process yet */
}

int ac_drm_bo_query_info(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_info *info)
{
   if (bo_handle >= dev->nbos || !dev->bos[bo_handle])
      return -EINVAL;
   memset(info, 0, sizeof(*info));
   info->alloc_size = dev->bos[bo_handle]->size;
   info->phys_alignment = 4096;
   info->preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
   return 0;
}

/* The queue is one and in order, so a buffer is idle when what was submitted last is done. The kext has no per-buffer record of its users. */
int ac_drm_bo_wait_for_idle(ac_drm_device *dev, ac_drm_bo bo, uint64_t timeout_ns, bool *busy)
{
   (void)bo;
   struct rn_uc_diag_req req = {0, 0};
   struct rn_uc_diag d;
   int r = call(dev, RN_UC_DIAG, &req, sizeof(req), &d, sizeof(d));
   if (r)
      return r;
   uint32_t expired = 1;
   if (d.seq_submitted != d.seq_signaled && (int32_t)(d.seq_signaled - d.seq_submitted) < 0)
      r = ac_drm_cs_query_fence_status(dev, 0, AMDGPU_HW_IP_COMPUTE, 0, 0, d.seq_submitted, timeout_ns, 0, &expired);
   *busy = !expired;
   return r;
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------- address ranges and mappings */

int ac_drm_va_range_alloc(ac_drm_device *dev, enum amdgpu_gpu_va_range va_range_type, uint64_t size, uint64_t va_base_alignment, uint64_t va_base_required,
                          uint64_t *va_base_allocated, amdgpu_va_handle *va_range_handle, uint64_t flags)
{
   if (va_range_type != amdgpu_gpu_va_range_general || size == 0)
      return -EINVAL;
   /* The kext maps whole 4 KB pages: a range is rounded to them, and so is its alignment (RADV asks for 256 bytes for some small buffers; the next range would then start inside a page). */
   size = (size + 4095) & ~4095ull;
   if (va_base_alignment < 4096)
      va_base_alignment = 4096;
   /* Policy of the bridge, not of Linux: a buffer goes to the upper half only when it has to be reachable with a 32-bit pointer (AMDGPU_VA_RANGE_32_BIT). Everything else,
    * AMDGPU_VA_RANGE_HIGH included, is placed in the lower half, because a command buffer in the upper half hangs the compute queue and a buffer's use is not known here. */
   struct vamgr *m = (flags & AMDGPU_VA_RANGE_32_BIT) ? &dev->high32 : &dev->low;
   uint64_t va;
   pthread_mutex_lock(&dev->lock);
   int r = vamgr_alloc(m, size, va_base_alignment, va_base_required, &va);
   pthread_mutex_unlock(&dev->lock);
   if (r)
      return r;
   struct amdgpu_va *h = calloc(1, sizeof(*h));
   if (!h) {
      pthread_mutex_lock(&dev->lock);
      vamgr_free(m, va, size);
      pthread_mutex_unlock(&dev->lock);
      return -ENOMEM;
   }
   h->dev = dev;
   h->mgr = m;
   h->base = va;
   h->size = size;
   *va_base_allocated = va;
   *va_range_handle = h;
   return 0;
}

int ac_drm_va_range_free(amdgpu_va_handle h)
{
   if (!h)
      return 0;
   pthread_mutex_lock(&h->dev->lock);
   vamgr_free(h->mgr, h->base, h->size);
   pthread_mutex_unlock(&h->dev->lock);
   free(h);
   return 0;
}

int ac_drm_va_range_query(ac_drm_device *dev, enum amdgpu_gpu_va_range type, uint64_t *start, uint64_t *end)
{
   if (type != amdgpu_gpu_va_range_general)
      return -EINVAL;
   *start = dev->low.start;
   *end = dev->low.end;
   return 0;
}

/* DRM_AMDGPU_GEM_VA. The kext maps and unmaps exactly one range at a time: an unmap names the range of a map (RADV does that), and the page flags are always read, write and execute. */
static void track_map(struct ac_drm_device *dev, uint32_t handle, uint64_t va, uint64_t offset, uint64_t size)
{
   pthread_mutex_lock(&dev->lock);
   if (dev->nmaps < 4096)
   {
      dev->maps[dev->nmaps].handle = handle;
      dev->maps[dev->nmaps].va = va;
      dev->maps[dev->nmaps].offset = offset;
      dev->maps[dev->nmaps].size = size;
      dev->nmaps++;
   }
   pthread_mutex_unlock(&dev->lock);
}

static void track_unmap(struct ac_drm_device *dev, uint64_t va)
{
   pthread_mutex_lock(&dev->lock);
   for (uint32_t i = 0; i < dev->nmaps; i++) {
      if (dev->maps[i].va == va) {
         dev->maps[i] = dev->maps[--dev->nmaps];
         break;
      }
   }
   pthread_mutex_unlock(&dev->lock);
}

/* RN_BRIDGE_DUMP=dir: write every mapped range (up to 16 MB each) as dir/va_<hex>.bin at each submit, to replay a submission with tools/rn_test.c rawib */
static void dump_maps(struct ac_drm_device *dev, const char *dir)
{
   pthread_mutex_lock(&dev->lock);
   for (uint32_t i = 0; i < dev->nmaps; i++) {
      if (dev->maps[i].size > (16u << 20) || dev->maps[i].handle >= dev->nbos || !dev->bos[dev->maps[i].handle])
         continue;
      struct amdgpu_bo *b = dev->bos[dev->maps[i].handle];
      mach_vm_address_t addr = 0;
      mach_vm_size_t len = 0;
      int mapped_here = 0;
      void *cpu = b->cpu;
      if (!cpu) {
         if (IOConnectMapMemory64(dev->conn, b->handle, mach_task_self(), &addr, &len, kIOMapAnywhere) != KERN_SUCCESS)
            continue;
         cpu = (void *)(uintptr_t)addr;
         mapped_here = 1;
      }
      char path[512];
      snprintf(path, sizeof(path), "%s/va_%llx.bin", dir, (unsigned long long)dev->maps[i].va);
      FILE *f = fopen(path, "wb");
      if (f) {
         fwrite((char *)cpu + dev->maps[i].offset, 1, dev->maps[i].size, f);
         fclose(f);
      }
      if (mapped_here)
         IOConnectUnmapMemory64(dev->conn, b->handle, mach_task_self(), addr);
   }
   pthread_mutex_unlock(&dev->lock);
}

int ac_drm_bo_va_op_raw(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr, uint64_t flags, uint32_t ops)
{
   (void)flags;
   /* No buffer: a sparse (PRT) range. The kext has no page-fault-as-zero mapping, so the range is only reserved in the VA manager; touching it faults. Nothing to map or unmap. */
   if (bo_handle == 0 && (ops == AMDGPU_VA_OP_MAP || ops == AMDGPU_VA_OP_REPLACE || ops == AMDGPU_VA_OP_UNMAP || ops == AMDGPU_VA_OP_CLEAR))
      return 0;
   struct rn_uc_va_unmap un = {addr, size};
   struct rn_uc_va_map map = {bo_handle, 0, addr, offset, size};
   int r;
   switch (ops) {
   case AMDGPU_VA_OP_MAP:
      r = call(dev, RN_UC_VA_MAP, &map, sizeof(map), NULL, 0);
      if (!r) track_map(dev, bo_handle, addr, offset, size);
      else if (getenv("RN_BRIDGE_DEBUG"))
         fprintf(stderr, "rn-bridge: map failed: bo %u offset 0x%llx size 0x%llx at 0x%llx\n", bo_handle, (unsigned long long)offset, (unsigned long long)size, (unsigned long long)addr);
      return r;
   case AMDGPU_VA_OP_UNMAP:
   case AMDGPU_VA_OP_CLEAR:
      r = call(dev, RN_UC_VA_UNMAP, &un, sizeof(un), NULL, 0);
      if (!r) track_unmap(dev, addr);
      return r;
   case AMDGPU_VA_OP_REPLACE:
      call(dev, RN_UC_VA_UNMAP, &un, sizeof(un), NULL, 0);     /* nothing mapped there is not an error for a replace */
      track_unmap(dev, addr);
      r = call(dev, RN_UC_VA_MAP, &map, sizeof(map), NULL, 0);
      if (!r) track_map(dev, bo_handle, addr, offset, size);
      return r;
   default:
      return -EINVAL;
   }
}

int ac_drm_bo_va_op(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr, uint64_t flags, uint32_t ops)
{
   return ac_drm_bo_va_op_raw(dev, bo_handle, offset, size, addr, flags, ops);
}

/* The map is done when the call returns, so the timeline point is signaled right away; the syncobjs the update should wait for are already signaled by whoever made them. */
int ac_drm_bo_va_op_raw2(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr, uint64_t flags, uint32_t ops,
                         uint32_t vm_timeline_syncobj_out, uint64_t vm_timeline_point, uint64_t input_fence_syncobj_handles, uint32_t num_syncobj_handles)
{
   if (num_syncobj_handles) {
      const struct drm_amdgpu_cs_chunk_sem *in = (const struct drm_amdgpu_cs_chunk_sem *)(uintptr_t)input_fence_syncobj_handles;
      for (uint32_t i = 0; i < num_syncobj_handles; i++) {
         uint32_t h = in[i].handle;
         int r = syncobj_wait_points(dev, &h, NULL, 1, (int64_t)now_ns() + 10000000000ll, RN_UC_SYNCOBJ_WAIT_FOR_SUBMIT, NULL);
         if (r)
            return r;
      }
   }
   int r = ac_drm_bo_va_op_raw(dev, bo_handle, offset, size, addr, flags, ops);
   if (r == 0 && vm_timeline_syncobj_out) {
      struct rn_uc_syncobj_array a;
      memset(&a, 0, sizeof(a));
      a.count = 1;
      a.handles[0] = vm_timeline_syncobj_out;
      a.points[0] = vm_timeline_point;
      r = call(dev, RN_UC_SYNCOBJ_SIGNAL, &a, sizeof(a), NULL, 0);
   }
   return r;
}

/* ---------------------------------------------------------------------------------------------------------------------------------------------- contexts, submission, fences */

int ac_drm_cs_ctx_create2(ac_drm_device *dev, uint32_t priority, uint32_t *ctx_id)
{
   (void)priority;
   pthread_mutex_lock(&dev->lock);
   *ctx_id = dev->next_ctx++;                                  /* all contexts share the connection's queue and sequence numbers */
   pthread_mutex_unlock(&dev->lock);
   return 0;
}

int ac_drm_cs_ctx_free(ac_drm_device *dev, uint32_t ctx_id) { (void)dev; (void)ctx_id; return 0; }

int ac_drm_cs_ctx_stable_pstate(ac_drm_device *dev, uint32_t ctx_id, uint32_t op, uint32_t flags, uint32_t *out_flags)
{
   (void)dev; (void)ctx_id; (void)op; (void)flags;
   if (out_flags)
      *out_flags = 0;                                          /* AMDGPU_CTX_STABLE_PSTATE_NONE */
   return 0;
}

/* The kext reports work lost in a queue reset through the wait of a sequence number; a context-wide "was there a reset" flag is not kept yet. */
int ac_drm_cs_query_reset_state2(ac_drm_device *dev, uint32_t ctx_id, uint64_t *flags)
{
   (void)dev; (void)ctx_id;
   *flags = 0;
   return 0;
}

int ac_drm_cs_query_fence_status(ac_drm_device *dev, uint32_t ctx_id, uint32_t ip_type, uint32_t ip_instance, uint32_t ring, uint64_t fence_seq_no,
                                 uint64_t timeout_ns, uint64_t flags, uint32_t *expired)
{
   (void)ctx_id; (void)ip_instance; (void)ring;
   if ((ip_type != AMDGPU_HW_IP_COMPUTE && ip_type != AMDGPU_HW_IP_GFX) || fence_seq_no == 0 || fence_seq_no > 0xffffffffull)   /* the queue is in bit 31 of the number */
      return -EINVAL;
   uint64_t deadline;                                          /* libdrm: relative unless AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE, AMDGPU_TIMEOUT_INFINITE for ever */
   if (timeout_ns == AMDGPU_TIMEOUT_INFINITE)
      deadline = UINT64_MAX;
   else if (flags & AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE)
      deadline = timeout_ns;
   else
      deadline = now_ns() + timeout_ns;
   for (;;) {
      uint64_t now = now_ns();
      uint64_t rel_ms = deadline > now ? (deadline - now) / 1000000 : 0;
      if (rel_ms > RN_WAIT_SLICE_MS)
         rel_ms = RN_WAIT_SLICE_MS;
      struct rn_uc_wait_seq in = {(uint32_t)fence_seq_no, (uint32_t)rel_ms};
      struct rn_uc_wait_seq_result out;
      int r = call(dev, RN_UC_WAIT_SEQ, &in, sizeof(in), &out, sizeof(out));
      if (r)
         return r;
      if (out.reached) {
         *expired = 1;                                         /* a sequence lost in a queue reset counts as expired, out.status says it was not run */
         return 0;
      }
      if (now_ns() >= deadline) {
         *expired = 0;
         return 0;
      }
   }
}

int ac_drm_cs_create_syncobj2(ac_drm_device *dev, uint32_t flags, uint32_t *handle) { return sp_create(&dev->sync, flags, handle); }
int ac_drm_cs_destroy_syncobj(ac_drm_device *dev, uint32_t handle) { return sp_destroy(&dev->sync, handle); }
int ac_drm_cs_syncobj_wait(ac_drm_device *dev, uint32_t *handles, unsigned n, int64_t timeout_nsec, unsigned flags, uint32_t *first)
{
   return syncobj_wait_points(dev, handles, NULL, n, timeout_nsec, flags, first);
}
int ac_drm_cs_syncobj_timeline_wait(ac_drm_device *dev, uint32_t *handles, uint64_t *points, unsigned n, int64_t timeout_nsec, unsigned flags, uint32_t *first)
{
   return syncobj_wait_points(dev, handles, points, n, timeout_nsec, flags, first);
}
int ac_drm_cs_syncobj_query2(ac_drm_device *dev, uint32_t *handles, uint64_t *points, unsigned n, uint32_t flags) { return sp_query(&dev->sync, handles, points, n, flags); }
int ac_drm_cs_syncobj_transfer(ac_drm_device *dev, uint32_t dst, uint64_t dst_point, uint32_t src, uint64_t src_point, uint32_t flags)
{
   return sp_transfer(&dev->sync, dst, dst_point, src, src_point, flags);
}
int ac_drm_cs_import_syncobj(ac_drm_device *dev, int shared_fd, uint32_t *handle) { (void)dev; (void)shared_fd; (void)handle; return -ENOSYS; }
int ac_drm_cs_syncobj_export_sync_file(ac_drm_device *dev, uint32_t syncobj, int *sync_file_fd) { (void)dev; (void)syncobj; (void)sync_file_fd; return -ENOSYS; }
int ac_drm_cs_syncobj_import_sync_file(ac_drm_device *dev, uint32_t syncobj, int sync_file_fd) { (void)dev; (void)syncobj; (void)sync_file_fd; return -ENOSYS; }
int ac_drm_cs_syncobj_export_sync_file2(ac_drm_device *dev, uint32_t syncobj, uint64_t point, uint32_t flags, int *sync_file_fd)
{
   (void)dev; (void)syncobj; (void)point; (void)flags; (void)sync_file_fd;
   return -ENOSYS;
}

void ac_drm_cs_chunk_fence_info_to_data(uint32_t bo_handle, uint64_t offset, struct drm_amdgpu_cs_chunk_data *data)
{
   data->fence_data.handle = bo_handle;
   data->fence_data.offset = offset * sizeof(uint64_t);
}

/* DRM_AMDGPU_CS: the chunks of one submit. The IB chunks are submitted one after the other (the kext takes one command buffer per submit): the waits go with the first, the
 * signals with the last, and the sequence number returned is the last one. The user-fence chunk is not needed (the fence is the kext's sequence number) and the buffer list is
 * not needed (everything mapped stays mapped). */
int ac_drm_cs_submit_raw2(ac_drm_device *dev, uint32_t ctx_id, uint32_t bo_list_handle, int num_chunks, struct drm_amdgpu_cs_chunk *chunks, uint64_t *seq_no)
{
   (void)ctx_id; (void)bo_list_handle;
   const struct drm_amdgpu_cs_chunk_ib *ibs[8];
   unsigned n_ibs = 0, n_in = 0, n_out = 0;
   rn_uc_sync_ref in[RN_UC_SUBMIT_SYNC_MAX], out[RN_UC_SUBMIT_SYNC_MAX];

   for (int i = 0; i < num_chunks; i++) {
      const struct drm_amdgpu_cs_chunk *c = &chunks[i];
      const void *data = (const void *)(uintptr_t)c->chunk_data;
      switch (c->chunk_id) {
      case AMDGPU_CHUNK_ID_IB:
         if (n_ibs == 8)
            return -EINVAL;
         ibs[n_ibs++] = data;
         break;
      case AMDGPU_CHUNK_ID_FENCE:
      case AMDGPU_CHUNK_ID_BO_HANDLES:
      case AMDGPU_CHUNK_ID_CP_GFX_SHADOW:
         break;
      case AMDGPU_CHUNK_ID_DEPENDENCIES:
      case AMDGPU_CHUNK_ID_SCHEDULED_DEPENDENCIES: {
         const struct drm_amdgpu_cs_chunk_dep *dep = data;
         for (uint32_t k = 0; k < c->length_dw * 4 / sizeof(*dep); k++) {
            uint32_t expired = 0;
            int r = ac_drm_cs_query_fence_status(dev, dep[k].ctx_id, dep[k].ip_type, dep[k].ip_instance, dep[k].ring, dep[k].handle, 10000000000ull, 0, &expired);
            if (r || !expired)
               return r ? r : -ETIME;
         }
         break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_IN:
      case AMDGPU_CHUNK_ID_SYNCOBJ_OUT: {
         const struct drm_amdgpu_cs_chunk_sem *sem = data;
         unsigned *n = c->chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_IN ? &n_in : &n_out;
         rn_uc_sync_ref *a = c->chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_IN ? in : out;
         for (uint32_t k = 0; k < c->length_dw * 4 / sizeof(*sem); k++) {
            if (*n == RN_UC_SUBMIT_SYNC_MAX)
               return -EINVAL;
            a[(*n)++] = (rn_uc_sync_ref){sem[k].handle, 0, 0};
         }
         break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT:
      case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL: {
         const struct drm_amdgpu_cs_chunk_syncobj *so = data;
         unsigned *n = c->chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT ? &n_in : &n_out;
         rn_uc_sync_ref *a = c->chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT ? in : out;
         for (uint32_t k = 0; k < c->length_dw * 4 / sizeof(*so); k++) {
            if (*n == RN_UC_SUBMIT_SYNC_MAX)
               return -EINVAL;
            a[(*n)++] = (rn_uc_sync_ref){so[k].handle, 0, so[k].point};
         }
         break;
      }
      default:
         return -EINVAL;
      }
   }
   if (n_ibs == 0)
      return -EINVAL;

   uint32_t last_seq = 0;
   if (getenv("RN_BRIDGE_DUMP")) {
      dump_maps(dev, getenv("RN_BRIDGE_DUMP"));
      for (unsigned i = 0; i < n_ibs; i++)
         fprintf(stderr, "rn-bridge: dumped, ib %u at 0x%llx, %u bytes\n", i, (unsigned long long)ibs[i]->va_start, ibs[i]->ib_bytes);
   }
   for (unsigned i = 0; i < n_ibs; i++) {
      if ((ibs[i]->ip_type != AMDGPU_HW_IP_COMPUTE && ibs[i]->ip_type != AMDGPU_HW_IP_GFX) || ibs[i]->ib_bytes == 0 || (ibs[i]->ib_bytes & 3))
         return -EINVAL;
      /* The kext holds at most RN_UC_SUBMIT_SYNC_MAX = 8 of each per submit; more than that is split nowhere, it is refused. */
      struct rn_uc_submit sub;
      memset(&sub, 0, sizeof(sub));
      sub.ib_va = ibs[i]->va_start;
      sub.ib_dwords = ibs[i]->ib_bytes / 4;
      sub.flags = ibs[i]->ip_type == AMDGPU_HW_IP_GFX ? RN_SUBMIT_GFX : 0;
      if (i == 0) {
         if (n_in > RN_UC_SUBMIT_SYNC_MAX)
            return -EINVAL;
         sub.in_count = n_in;
         memcpy(sub.in, in, n_in * sizeof(in[0]));
      }
      if (i == n_ibs - 1) {
         if (n_out > RN_UC_SUBMIT_SYNC_MAX)
            return -EINVAL;
         sub.out_count = n_out;
         memcpy(sub.out, out, n_out * sizeof(out[0]));
      }
      struct rn_uc_submit_result res;
      if (getenv("RN_BRIDGE_DEBUG"))
         fprintf(stderr, "rn-bridge: submit ib_va 0x%llx, %u dwords, %u waits, %u signals (ib %u of %u)\n", (unsigned long long)sub.ib_va, sub.ib_dwords, sub.in_count, sub.out_count, i + 1, n_ibs);
      int r = call(dev, RN_UC_SUBMIT, &sub, sizeof(sub), &res, sizeof(res));
      if (r)
         return r;
      last_seq = res.seq;
   }
   *seq_no = last_seq;
   return 0;
}
