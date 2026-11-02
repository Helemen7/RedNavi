# mesa-bridge: RADV on macOS over the RedNavi user client

Mesa's RADV talks to the kernel through the `ac_drm_*` functions of `src/amd/common/ac_linux_drm.h`, which on Linux issue amdgpu ioctls on a DRM file descriptor. This directory holds a replacement for that layer, `src/ac_apple_iokit.c`, which calls the kext's user client (`RedNavi/rn_uapi.h`, version 9) with `IOConnectCallStructMethod`, plus the patch set that makes RADV build and enumerate its device without a DRM node. Mesa is MIT licensed, so the file is meant to live in a Mesa fork; it sits here to be built, tested and kept in step with the kext. Build recipe, results and what stood in the way: [docs/M5_MESA_BUILD.md](../docs/M5_MESA_BUILD.md).

## What is here

- `patches/0001-radv-darwin-iokit.patch`: the whole change to Mesa at commit `bd830ce` (27.0.0-devel), generated from the working tree: the meson conditions, the in-tree copy of the bridge in `src/amd/common/apple/` (`ac_apple_iokit.c`, headers, a libdrm subset in `amdgpu.h` and `xf86drm.h`), and the RADV changes (device enumeration through the instance's `enumerate` hook, syncobjs from the bridge's sync provider, no external handles, sparse off). It builds `libvulkan_radeon.dylib` on macOS 15 (x86_64).
- `src/ac_apple_iokit.c` and `include/`: the same bridge as a standalone file that builds without a Mesa tree, for `test/bridge_test.c` (what `tools/rn_test.c` does, through the `ac_drm_*` functions only; `make -C mesa-bridge test`, run `./bridge_test` on the machine with the card). The copy inside the patch is the one RADV uses; keep the two in step.
- `test/`: Vulkan programs, plain Vulkan 1.1 (`clang -I/opt/local/include test/X.c -lvulkan`): `vk_dispatch` (one compute dispatch), `vk_stress` (modes `burst`, `saturate`, `barrier`, `multi`, `vram`, `all`; `VK_QUEUE_COMPUTE_ONLY=1` uses the compute-only queue family), `vk_draw` (a triangle, checked pixel by pixel), `vk_present` (the same triangle written to the display surface; `vk_present scene` renders depth, a sampled texture and 4x MSAA with a resolve into it). SPIR-V headers are generated with glslangValidator from the `.comp`/`.vert`/`.frag` files next to them.
- `tools/`: `rn_icd.json` (the Vulkan loader manifest), `vk_run.sh` / `vk_run_tool.sh` (run `vulkaninfo` or a test with the ICD and library path set), `vk_rebuild_and_run.sh` (rebuild, copy to the test machine, run), `vk_stress_watchdog.sh`.

## What the bridge does

Device open and interface check, the queries (`DEV_INFO` as the Linux kernel answered it on this card, firmware versions, heaps, GPU info as libdrm derives it), buffers (create in host memory, device-local VRAM or CPU-visible VRAM; CPU map; free), the user-space virtual address allocator, mappings (`bo_va_op*`), contexts, `cs_submit_raw2` with its chunks (IB, syncobj in and out, timeline wait and signal, dependencies; IBs go to the graphics or the compute queue by `ip_type`), fence status, a `struct util_sync_provider` over the kext's syncobjs, and, for the display surface, `rn_bridge_scanout()` plus a host-pointer import of that range (`ac_drm_create_bo_from_user_mem`).

## Differences from Linux, on purpose

- **Address ranges**: a buffer goes to the upper half only with `AMDGPU_VA_RANGE_32_BIT`; everything else goes to the lower half, because a command buffer in the upper half hangs the queue (`docs/COMPAT.md`).
- **Memory placement**: buffers the CPU never maps go to device-local VRAM; VRAM-only requests to the CPU-visible part; mappable VRAM|GTT buffers (command buffers, shaders, descriptors) to host memory unless `RN_BRIDGE_VRAM_FIRST=1`. `RN_BRIDGE_NO_VRAM=1` serves everything from host memory.
- **Queues**: one compute queue and one graphics queue, shared by all contexts of a connection; no SDMA.
- **Sparse resources** are off (`radv_sparse_enabled` is false on macOS).
- **Not supported**: dma-buf and sync-file export and import, user queues, sensors, video caps, the timestamp. They return `-EOPNOTSUPP` or `-ENOSYS`.

## Debugging

`RN_BRIDGE_DEBUG=1` prints every failing kext call, every submit and every buffer placement. `RN_BRIDGE_DUMP=dir` dumps all mapped ranges at each submit; `rn_test replay DIR VA:DWORDS...` and `rn_test rawib FILE` resubmit them. `RADV_DEBUG=dumpibs,shaders,info` works as on Linux.
