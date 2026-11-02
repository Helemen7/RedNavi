# RADV on macOS: what the Mesa build and code need

The first sections are a read-only analysis of 2026-11-01 of Mesa `main` at commit `01bab93` (sparse checkout; the scan is reproducible with `tools/mesa_linux_scan.sh`), written before anything was built. The sections from "Build result" on (2026-11-02, Mesa `bd830ce`) record the real build and what ran on the 7800 XT; where the two differ, the later ones are right (for example the open questions under "Not known yet" are answered there).

## Short answer

The Linux-specific surface of RADV is small and already has seams. ACO, NIR, addrlib and the Vulkan runtime carry nothing that stops them on Darwin (Mesa builds Zink and KosmicKrisp there already). What does not build on Darwin is concentrated in five places, and for four of them Mesa has an extension point that its own virtio and Windows ports use:

1. **Device discovery and open** (`radv_physical_device.c`, `radv_device.c`): a DRM render node is opened and its file descriptor is the device.
2. **`ac_linux_drm.{c,h}`**: the one layer that turns the winsys's calls into `DRM_AMDGPU_*` ioctls. About 60 functions, all named `ac_drm_*`.
3. **Syncobjs**: Vulkan fences and semaphores are DRM syncobjs through `vk_drm_syncobj`, which can run on a `struct util_sync_provider` instead of a file descriptor.
4. **External handles** (`sync_file`, dma-buf, opaque fds): advertised unconditionally, not needed headless.
5. **The meson build**: three places that require libdrm_amdgpu or the Linux winsys.

The plan is therefore a small Mesa patch set plus one new file, `ac_apple_iokit.c`, not a port.

## The meson build on Darwin

- `-Dvulkan-drivers=auto` gives an empty list on Darwin (`meson.build:288`, "no vulkan driver supports windows or macOS currently"): pass `-Dvulkan-drivers=amd`.
- `system_has_kms_drm` is false, so `dep_libdrm` is a null dependency (`meson.build:804`). That is what we want. But `_libdrm_checks['amdgpu']` is enabled for `with_amd_vk and not with_platform_windows` (line 784) and then `dependency('libdrm_amdgpu', ...)` is required: **patch 1**, treat Darwin like Windows there.
- `src/amd/vulkan/meson.build`: the `winsys/amdgpu/*.c` files are compiled `if not with_platform_windows` (line 221) and `dep_libdrm_amdgpu` is in the link line (294): **patch 2**, same condition plus our new winsys file.
- Darwin forces `with_vulkan_beta` (portability and beta extensions, line 449) and `with_dri_platform = 'apple'`; both harmless for RADV.
- `-Dplatforms=auto` on Darwin means `x11,macos`. Headless needs neither (`-Dplatforms=` empty if meson accepts it, otherwise `macos`); the X11 WSI would pull in xcb.
- Build-time tools: python3 with `mako` (not installed here), `glslang` for the BVH shaders (`src/amd/vulkan/bvh/*.comp`, `bvh_spv` in the radv meson), bison and flex (present), `-Dllvm=disabled` (ACO is the default compiler; the LLVM backend is optional), zlib, expat. No `mesa_clc` dependency was found in the radv meson. `brew install meson glslang` and `pip install mako` would cover it.

## Linux constructs in the code

From `tools/mesa_linux_scan.sh` over `src/amd/{vulkan,common,compiler,addrlib}`, `src/vulkan/{runtime,wsi}`, `src/util`:

| Construct | Where | Verdict |
|---|---|---|
| `futex` | `src/util` only (`futex.h`, `simple_mtx.h`, `u_queue.c`) | not a blocker: `UTIL_FUTEX_SUPPORTED` is 0 on Darwin and the mutex falls back; Vulkan timeline semaphores (`vk_sync_timeline`) use a mutex and condition variable, not futexes |
| `memfd_create`, `shm_open` | `anon_file.c`, `os_memory_fd.c` | not in RADV's path (WSI, Wayland) |
| `<linux/...>` headers | `libsync.h`, `futex.c`, `anon_file.c` | `libsync.h` is used by `radv_amdgpu_cs.c` and `radv_device_memory.c` for sync-file export; guard or stub |
| `xf86drm.h` / `drmGetDevice*` / `drmIoctl` | `radv_physical_device.c`, `radv_device.c`, `radv_amdgpu_bo.c`, `ac_gpu_info.c`, `ac_surface.c`, `vk_drm_syncobj.c`, `u_sync_provider.c`, WSI | seam 1 and 3 below |
| `/proc/self/fd`, `/sys` | `ac_gpu_info.c:1952` (PCI info from an fd), `radv_rmv.c`, `radv_sqtt.c`, `ac_sqtt.c`, `ac_rgp.c` | the first is replaced by `ac_drm_query_pci_bus_info`; the others are tracing, off by default |
| `mmap(` | `radv_amdgpu_bo.c` once | anonymous placeholder mappings for virtual BOs, fine on Darwin |
| `epoll`, `eventfd`, `sched_setaffinity`, `prctl` | `src/util` | outside RADV's path or guarded by OS |
| dma-buf | `radv_device_memory.c`, `radv_device.c`, `radv_formats.c` | external memory: do not advertise it |

`radv_physical_device.c:719-723` sets `KHR_external_fence_fd`, `KHR_external_memory_fd` and `KHR_external_semaphore_fd` to `true` without a condition: **patch 3**, make them depend on the backend.

## The seams Mesa already has

- **Windows mode of `ac_linux_drm.h`** (2026): under `_WIN32` the header drops libdrm and `amdgpu.h`, defines the few constants it needs, and every `ac_drm_*` becomes a `static inline` stub through `util/u_stub.h`. The Darwin build needs the first half of that (no libdrm, own typedefs); the stubs are not wanted.
- **The virtio / vpipe mode**: `RADV_DRM_DEVICE_AMDGPU_VPIPE` is a device without a file descriptor. `radv_create_winsys` passes `fd = -1`, `ac_drm_device_initialize(-1, is_virtio, ...)` creates a device whose calls all go to `amdvgpu_*` functions (`is_virtio` appears 38 times in `ac_linux_drm.c`). The set the virtio backend implements is the minimum RADV needs: `device_initialize/deinitialize/get_fd`, `bo_alloc/free/cpu_map/cpu_unmap/query_info/set_metadata/export/import/wait_for_idle/va_op_raw`, `va_range_alloc`, `cs_ctx_create2/free/stable_pstate/query_fence_status/query_reset_state2/cs_submit_raw2`, `query_info`, `query_sw_info`, `get_marketing_name`, `has_vm_always_valid`, `vm_reserve_vmid`. Our backend is the same shape: a device that is not a file descriptor, with its own implementations behind an `is_apple` flag.
- **`struct util_sync_provider`** (`src/util/u_sync_provider.h`): the syncobj operations as a function table (`create`, `destroy`, `wait`, `reset`, `signal`, `timeline_signal`, `timeline_wait`, `query`, `transfer`, `import/export_sync_file`, `handle_to_fd`, `fd_to_handle`). `vk_drm_syncobj_get_type_from_provider` builds the Vulkan sync type from it: it needs `create` (with `DRM_SYNCOBJ_CREATE_SIGNALED`) and a `wait` that works with timeout 0; it advertises timeline semaphores only when `timeline_wait` is non-NULL. Leaving `timeline_*` NULL would make the Vulkan runtime emulate timelines with `vk_sync_timeline`, but see patch 3 below: `copy_sync_payloads` then falls back to sync files, so the provider supplies the timeline entries and `transfer`.

## The bridge: `ac_apple_iokit.c`

One new file in `src/amd/common/`, plus a small `.h`, in a Mesa fork (Mesa is MIT; RedNavi's own code stays BSD-3, the patch set lives in a separate tree). It opens `RedNaviProbe` with `IOServiceOpen` and implements, over `IOConnectCallStructMethod` with the structs of `RedNavi/rn_uapi.h`:

1. **Device**: `ac_drm_device_initialize` (opens the service, checks the interface version, returns `dev` with `fd = -1`), `deinitialize`, `get_fd` (-1), `get_sync_provider` (the provider below), `get_cookie`.
2. **Queries** (all `DRM_AMDGPU_INFO` on Linux): `query_info`, `query_hw_ip_count`, `query_hw_ip_info`, `query_firmware_version`, `query_gpu_info`, `query_heap_info`, `query_sensor_info` (return an error: not supported), `query_video_caps_info` (error), `query_uq_fw_area_info` (error), `query_gpuvm_fault_info`, `query_sw_info`, `query_pci_bus_info`, `query_has_vm_always_valid`, `read_mm_registers`, `get_marketing_name`. The `AMDGPU_INFO_*` ids actually used are 32: `ACCEL_WORKING`, `DEV_INFO`, `HW_IP_INFO`, `HW_IP_COUNT`, `FW_VERSION` (`GFX_PFP`, `GFX_ME`, `GFX_MEC`, UVD, VCE, VCN), `MEMORY`, `VRAM_GTT`, `VRAM_USAGE`, `VIS_VRAM_USAGE`, `GTT_USAGE`, `TIMESTAMP`, `NUM_BYTES_MOVED`, `NUM_EVICTIONS`, `NUM_VRAM_CPU_PAGE_FAULTS`, `READ_MMR_REG`, `MAX_IBS`, `SENSOR` (`GPU_TEMP`, `GFX_SCLK`, `GFX_MCLK`), `GPUVM_FAULT`, `VIDEO_CAPS`, `UQ_FW_AREAS`.
3. **Virtual addresses**: the user-space range allocator puts a buffer in the upper half only when its flags ask for the 32-bit range (`AMDGPU_VA_RANGE_32_BIT`, addresses with the high dword `address32_hi` 0xffff8000, from DEV_INFO); every other buffer, command buffers included, goes to the lower half whatever `AMDGPU_VA_RANGE_HIGH` says, because a command buffer in the upper half hangs the compute queue (docs/COMPAT.md). Upstream Mesa asks for the high range for all buffers: that is a deviation of the bridge, not a patch to RADV.
3b. **Memory**: `bo_alloc`, `bo_free`, `bo_cpu_map/unmap` (IOConnectMapMemory), `bo_query_info`, `bo_set_metadata`, `bo_wait_for_idle`, `bo_export/import` (not supported), `create_bo_from_user_mem` (not supported), `bo_va_op_raw` and `bo_va_op_raw2` (the `VA_MAP`/`VA_UNMAP` of the kext), `va_range_alloc/free`. **The VA range allocator runs in user space** on Linux too (libdrm's `amdgpu_vamgr`): the bridge carries its own, over the window the kext reports.
4. **Submission**: `cs_ctx_create2/free` (one kernel context per connection for now), `cs_submit_raw2` (parses the chunk array: `IB`, `FENCE`, `BO_HANDLES`, `SYNCOBJ_IN/OUT`, `SYNCOBJ_TIMELINE_*`; hands IBs and the syncobj lists to the kext), `cs_query_fence_status`, `cs_query_reset_state2`, `cs_ctx_stable_pstate`, `vm_reserve_vmid` (no-op).
5. **Sync provider**: a `struct util_sync_provider` over kext syncobj methods: `create`, `destroy`, `wait`, `reset`, `signal`, `query`, `transfer`; `import/export_sync_file`, `handle_to_fd`, `fd_to_handle` and the `timeline_*` entries return an error or stay NULL.

The expected size is of the order of 1500 lines of C. The structs on the wire are the native `drm_amdgpu_*` and `drm_syncobj_*` ones (trimmed copies of Mesa's `include/drm-uapi/amdgpu_drm.h` and `drm.h`, MIT, in `RedNavi/` for the kext and in the bridge), so that the bridge forwards what RADV builds without translating it.

## The RADV patches around it

1. `meson.build`, `src/amd/vulkan/meson.build`: Darwin takes the windows branch for libdrm_amdgpu and gets the new winsys file.
2. `radv_physical_device.h/.c`, `radv_device.c`: a `RADV_DRM_DEVICE_APPLE` type. `radv_physical_device_try_create` has an Apple variant that finds the IOService and fills the PCI info instead of `drmGetVersion` and `open`; `radv_create_winsys` and `radv_create_drm_device_locked` pass `fd = -1` as for the vpipe device. Physical device enumeration must go through the generic `enumerate` hook of `vk_instance` (the DRM path calls `drmGetDevices2`).
3. `radv_amdgpu_winsys.c:362`: `info->syncobj_sync_type = vk_drm_syncobj_get_type_from_provider(provider)` instead of `vk_drm_syncobj_get_type(fd)`. `copy_sync_payloads` (line 283, `vk_drm_syncobj_copy_payloads`) is the catch: with a timeline-capable syncobj type it uses `transfer`, without one it exports and imports **sync files**, which macOS does not have. So the kext's syncobjs have to be timeline syncobjs with `transfer`, not binary ones with Vulkan's emulation on top.
4. External handle extensions off (`radv_physical_device.c:719-723`), `libsync` stubs for `radv_amdgpu_cs.c` and `radv_device_memory.c`.
5. `ac_linux_drm.h`: a third branch next to `_WIN32`: no libdrm, the constants and opaque typedefs, the real prototypes.

## Not known yet

- Whether the Vulkan loader on macOS (LunarG's) lists an ICD that is not a portability driver without flags, and which JSON it wants. `vulkaninfo` may need `VK_LOADER_DRIVERS_SELECT` or the portability enumeration flag set for the first runs.
- Whether `-Dplatforms=` empty is accepted by this Mesa.
- What else `radv_physical_device.c` asks of the fd at creation (`wsi_master_fd`, `render_devid`, `local_fd`): the DRM variables are scattered in `radv_physical_device_try_create` over about 350 lines.
- The BVH shader build and `glslang` on macOS.
- How much memory model RADV insists on: it expects CPU-visible device-local memory and a GTT heap. The kext has host memory only for now (docs/M5_PLAN.md).

That measurement was done on 2026-11-02, see "Build result" below.

## Build result, 2026-11-02 (Mesa bd830ce, 27.0.0-devel)

`mesa-bridge/patches/0001-radv-darwin-iokit.patch` applies on that commit and builds `libvulkan_radeon.dylib` on macOS 15 (x86_64) with `-Dvulkan-drivers=amd`. 

```
git apply mesa-bridge/patches/0001-radv-darwin-iokit.patch      # in a Mesa checkout at bd830ce
PATH=<venv with mako, meson, ninja>:$PATH:/opt/local/bin \
meson setup build -Dvulkan-drivers=amd -Dgallium-drivers= -Dllvm=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dplatforms= -Dvulkan-layers= -Dbuild-tests=false -Dbuildtype=release -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dvideo-codecs=
ninja -C build
```

Needs glslang (BVH shaders) from MacPorts. What the patch does: the `ac_drm_*` layer is `src/amd/common/apple/ac_apple_iokit.c` instead of `ac_linux_drm.c`; `apple/amdgpu.h` and `apple/xf86drm.h` stand in for libdrm; RADV enumerates its one device through `physical_devices.enumerate` with no DRM node (`RADV_DRM_DEVICE_APPLE_IOKIT`, fd -1); syncobjs come from the bridge's sync provider; buffers are mapped by `ac_drm_bo_cpu_map` as on virtio; addrlib is built with `-DHAVE_TSERVER` (the Apple branch wants a private header); weak layer entrypoints need `-Wl,-undefined,dynamic_lookup`. `libzstd` must be bundled next to the dylib (`tools/vk_run.sh` and `tools/rn_icd.json` run `vulkaninfo` with `VK_ICD_FILENAMES` and `DYLD_LIBRARY_PATH` pointing at the bundle).

### First `vulkaninfo`, 2026-11-02 (verified on the desktop, 7800 XT)

`VK_ICD_FILENAMES=rn_icd.json vulkaninfo --summary` with the bundle in `~/rn/vk` (Vulkan loader and vulkaninfo from MacPorts, `libvulkan_radeon.dylib`, `libzstd`) lists the card: `AMD Radeon RX 7800 XT (RADV NAVI32)`, `DRIVER_ID_MESA_RADV`, Vulkan 1.4.366, discrete GPU, `0x1002:0x747e`. Getting there took four changes beyond the build:

- GFX IP version: the kext answers all zero for the graphics engine, and Mesa takes the GFX level from it ("Unknown gfx version: 0.0"). The bridge copies the compute engine's version into the GFX answer with `available_rings` 0, so RADV sees GFX11 with no graphics queue.
- `amdgpu_sw_info_address_prt_wa_control_bit` answers 47.
- Sparse (PRT) ranges, `bo_handle` 0 in a VA op: success without a mapping. The range is only reserved; touching it faults. Sparse residency does not work.
- The sync provider clone is a reference to the device: `vk_device` finalizes its clone after the winsys freed the device (crash in `vk_device_finish`).

Not done: the PCI location (placeholder 0000:03:00.0, so the device UUID is a placeholder too), memory in VRAM (all buffers are host memory), the graphics queue.

### First Vulkan compute dispatch, 2026-11-02 (verified on the desktop, 7800 XT)

`mesa-bridge/test/vk_dispatch.c` (plain Vulkan 1.1: one storage buffer, a compute shader that writes `v[i] = i * 3 + push_constant`, `vkQueueSubmit`, fence wait, CPU check) passes with 1, 16, 1024 and 4096 workgroups of 64 (262144 values). The pipeline is compiled by ACO on the Mac. Three things stood in the way:

- RADV's submit loop never ended: the kext says "1 IB per submit" in `MAX_IBS` and a RADV submission has a preamble IB and a command buffer, so it never fits. The bridge answers `-EINVAL` to `AMDGPU_INFO_MAX_IBS` (RADV uses its default of 124 for compute) and sends the IBs of a submission to the kext one after the other.
- The shader hung the wave without a fault. RADV's NULL-PRT workaround for scalar loads clears bit `address_prt_wa_control_bit` of every SMEM address; with the answer 47 it cleared bit 47 of the `0xffff8000` high dword and the shader read its descriptor from the lower half, where the data buffer is. The bridge now answers `~0` ("ignore", as the query's documentation says) and `radv_sparse_enabled()` is false on macOS (no unbacked mappings in the kext). The replay tools that found it: `RN_BRIDGE_DUMP=dir` dumps every mapped range at each submit, `rn_test replay DIR VA:DWORDS...` maps them in a connection and resubmits the command buffers, `rn_test rawib FILE` submits dwords from a file (`RADV_DEBUG=dumpibs` prints them); the hang was bisected by replacing packets with NOPs.
- `RN_BRIDGE_DEBUG=1` prints every failing kext call and every submit.

The queue is stuck after a hang until it is reset (`rn_test reset`, or the kext's own watchdog after `rednavi_hang_ms`).

### Compute stress tests, 2026-11-02 (verified on the desktop, 7800 XT)

`mesa-bridge/test/vk_stress.c` (plain Vulkan 1.1, compute only; `tools/vk_stress_watchdog.sh SECONDS mode args` runs it on the desktop with a timeout) checks every value on the CPU. All of these pass:

- `burst 20000`: 20000 separate `vkQueueSubmit` calls of one dispatch each (40000 kext submits, 1.28 M values), about 37000 submits per second; three processes at once, 5000 submits each.
- `saturate 600`: 600 submits of 64 dependent dispatches over 2 M words, about 30 s of GPU time. The submit thread is held back about 1.5 s per 32 submits while the ring (2048 dwords) is full; 17 ring-full waits per run, no reset, six runs in a row.
- `barrier 20000`: a writer and 20000 increments with a compute-to-compute barrier between each, in one command buffer (a chained IB of hundreds of thousands of dwords), and the same chain over four submits tied by semaphores (syncobj in and out in the kext).
- `multi 20`: 24 storage buffers alive at once (36 to 45 MB per round, 4 KB to 8 MB each), one write and one increment dispatch each, then every other buffer freed and replaced by another size; 20 rounds. Run next to three burst processes, a saturate process and a second multi process (five VMIDs at once): all pass.

Two kext faults found by these tests, both fixed: the ring-full wait gave up after 2 s even when the read pointer kept moving (a long GPU backlog), reset the queue and dropped the queued work; it now gives up after 2 s without the read pointer moving. And the kext refused command buffers above 4096 dwords (16 KB), which any real command buffer exceeds; the limit is now the hardware's 20-bit size field.

One failure is unexplained: the first `saturate 600` right after the first boot with the new kext lost all its work (every value wrong, a VM fault status latched, address not recorded). It did not repeat in 12 further runs, including one right after a reboot. If a fault shows up again, `rn_test stats` prints the status and address of the latched fault.

### VRAM, 2026-11-02

Buffers in device-local VRAM (about 13.5 GB) and CPU-visible VRAM (78 MB) from the kext's allocator; design, the PTE finding (offset, not MC address) and the results are in docs/COMPAT.md, "VRAM buffers". Memory types RADV now reports: host (heap of 2 GB), device-local VRAM, CPU-visible VRAM (78 MB heap).

### Graphics, 2026-11-02

RADV now reports a queue family with GRAPHICS (flags 0xf) next to the compute-only one; a triangle renders correctly into a tiled image in VRAM. Details and what is missing in docs/COMPAT.md, "Graphics queue". Tools: `test/vk_draw.c` (`vk_draw [size] [linear|clear]`), `test/vk_stress.c` (`VK_QUEUE_COMPUTE_ONLY=1` picks the compute-only family).

### Presentation and 3D, 2026-11-02

`vk_present` (`mesa-bridge/test/vk_present.c`) writes into the display surface: the bridge's `rn_bridge_scanout()` maps it and RADV imports the pointer with `VK_EXT_external_memory_host`; the kext hands the same VRAM out as an `RN_BO_SCANOUT` buffer. `vk_present` draws the triangle at 1920 x 1080; `vk_present scene` renders two crossing quads (a checkerboard texture and a gradient, with a D32_SFLOAT depth buffer) and a slanted triangle with 4x MSAA and a resolve, and checks the surface pixel by pixel (depth rejection, texture sampling, the 1/4, 2/4 and 3/4 coverage levels on the edge). Both confirmed on the monitor. The graphics queue is reset and watched like the compute queue (`rn_test gfxreset`). Details in docs/COMPAT.md.

### Swapchain, 2026-11-02

`VK_KHR_swapchain` works on the headless surface, which is the display (docs/M6_PLAN.md, step 1b): `__APPLE__` branches in `wsi_common_headless.c`, `radv_wsi.[ch]`, a `stride_align` for CPU images in the common WSI code. `mesa-bridge/test/vk_swapchain.c` is the test.
