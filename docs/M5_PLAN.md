# M5: what RADV needs from the kernel

Status: read from the Mesa sources (2026-11-01), nothing here is run on hardware. The goal of M5 is a Vulkan driver (RADV) that runs on the 7800 XT through our kext.

## Where the shim goes

The plan so far said "libdrm_amdgpu shim". Current Mesa (main) no longer calls libdrm_amdgpu from `src/amd/vulkan/winsys/amdgpu/`: the winsys and `src/amd/common/ac_gpu_info.c` call `ac_drm_*` wrappers (`src/amd/common/ac_linux_drm.c`), and those issue the raw `DRM_AMDGPU_*` ioctls on a DRM file descriptor (`drm_ioctl_write_read(dev->fd, ...)`). Mesa 24.2 still used `amdgpu_*` from libdrm. Consequences:

- The stable seam is the `ac_drm_*` layer (about 60 functions, listed below). Replacing `ac_linux_drm.c` with a backend that talks to our `IOUserClient` is a small, local patch to Mesa; there is no libdrm to emulate and no DRM fd on macOS.
- A fork of `ac_linux_drm.c` is the minimum Mesa change. It keeps the ioctl structures (`drm_amdgpu_cs`, `drm_amdgpu_gem_va`, ...) as the wire format, which keeps the patch mechanical.
- Pin the Mesa version we target; the `ac_drm_*` set moved between 24.2 and main.

## What RADV calls (grouped)

Device and queries (`radv_amdgpu_winsys.c`, `ac_gpu_info.c`): `ac_drm_device_initialize`, `query_info` (many `AMDGPU_INFO_*`), `query_hw_ip_info`, `query_hw_ip_count`, `query_gpu_info`, `query_heap_info`, `query_firmware_version` (12 calls), `query_pci_bus_info`, `query_sensor_info`, `read_mm_registers`, `vm_reserve_vmid` / `vm_unreserve_vmid`. These are static answers from the discovery table, the firmware headers and the SMU; no new hardware work.

Memory (`radv_amdgpu_bo.c`): `bo_alloc` (domains VRAM and GTT; flags `NO_CPU_ACCESS`, `CPU_GTT_USWC`, `CPU_ACCESS_REQUIRED`, `VM_ALWAYS_VALID`, `DISCARDABLE`), `bo_free`, `bo_cpu_map` / `bo_cpu_unmap`, `bo_query_info`, `bo_wait_for_idle`, `bo_export` / `bo_import`, `create_bo_from_user_mem`, `bo_set_metadata`, `va_range_alloc` / `va_range_free`, `bo_va_op_raw` / `bo_va_op_raw2` (ops MAP, UNMAP, REPLACE, CLEAR; page flags readable, writeable, executable, PRT, `MTYPE_UC`).

Submission and sync (`radv_amdgpu_cs.c`): `cs_ctx_create2` / `cs_ctx_free`, `cs_submit_raw2` (chunks: IB, fence, syncobj in/out, BO handles), `cs_query_fence_status`, `cs_ctx_stable_pstate`, DRM syncobjs (`create_syncobj2`, `destroy_syncobj`, `syncobj_timeline_wait`, `syncobj_query2`, `syncobj_transfer`, export/import sync file).

## What the kext has and lacks

Has: one GART window in system memory, buffers created by `IOBufferMemoryDescriptor` and mapped into it, one compute queue mapped through the MES, submit of one IB with a fence write, fence wait, ring-full check, one lock.

Lacks, in the order RADV needs it:

1. **Per-context virtual address spaces.** RADV chooses its own addresses (`va_range_alloc`) and maps BOs into them (`va_op`). Today every buffer lives at a kext-chosen GART address in the kernel's VM context 0. RADV needs a page table per context (VM contexts 1 to 14, `GCVM_CONTEXT*`), 4-level page tables the kext builds in VRAM or host memory, TLB invalidation after a map, and the MES `ADD_QUEUE` process fields (page table base, VMID, `process_context_addr`). This is the largest piece and touches hardware paths nothing has run yet.
2. **Queue per context.** `cs_ctx_create` maps a queue for a context through the MES (the process path of `ADD_QUEUE`, `rednavi_mes_step=7` in the code, never validated); `cs_submit_raw2` writes IBs into that queue. Graphics (GFX) and SDMA queues are needed too; today only compute exists.
3. **Fences and syncobjs.** Done for one context (2026-11-01): a sequence per connection, written by `RELEASE_MEM` into a kernel-owned page, `WAIT_SEQ` as `cs_query_fence_status` and `wait_cs`; one fence per ring will come with the queues. Still open: syncobjs with timeline wait in the kext (or in the userspace shim with a kext-side event for blocking waits).
4. **Interrupts.** A blocking wait needs an interrupt; polling the fence works first. MSI to the CPU is not wired.
5. **Buffer lifetime and teardown.** Per-client BO tables (today `bos[]` is global and never freed per client), eviction is not needed while everything stays pinned.
6. **Security boundary.** Every address in an IB is a GPU virtual address in the client's own VM; with per-context page tables that is enforced by the hardware, which the single shared window cannot do. Until then the client needs administrator privilege (`rednavi_uc_anyone`).

## Proposed user client shape

The current `externalMethod` has hand-numbered selectors with positional scalar arguments. M5 replaces them with struct-in/struct-out methods that mirror the ioctl structures, one selector per `DRM_AMDGPU_*` command:

| Selector | Mirrors | Notes |
|---|---|---|
| `INFO` | `DRM_AMDGPU_INFO` | request id in the struct, answer buffer out |
| `GEM_CREATE`, `GEM_MMAP`, `GEM_VA`, `GEM_WAIT_IDLE`, `GEM_METADATA` | same | BO handle table per client |
| `CTX` | `DRM_AMDGPU_CTX` | alloc, free, query, stable pstate |
| `CS` | `DRM_AMDGPU_CS` | chunk array copied in, IB chunk, fence chunk, syncobj chunks |
| `WAIT_CS`, `WAIT_FENCES` | same | fence by (ctx, ring, sequence) |
| `SYNCOBJ_*` | `DRM_IOCTL_SYNCOBJ_*` | create, destroy, wait, signal, transfer |

CPU mapping of a BO uses `IOConnectMapMemory` with the BO handle as memory type. Large structs go through `IOConnectCallStructMethod` (input and output struct sizes are fixed per selector, so each carries a size and version field).

## Order of work

1. Replace the positional methods by struct methods with the current features (create BO, submit, wait) and move `rn_test` onto them. Per-client BO table with free on close.
2. Per-context VM: page table builder, `va_op`, TLB flush, one VMID. Test with `rn_test` using a client-chosen address.
3. Process-path `ADD_QUEUE` (MES step 7 and up) so a context owns its queue, then fence sequence numbers.
4. GFX and SDMA queues.
5. Patched `ac_linux_drm.c` backend plus the minimal queries; run `vulkaninfo`, then a compute-only Vulkan test, then graphics.

Step 2 decides the schedule; steps 1 and 5 are mostly software.
