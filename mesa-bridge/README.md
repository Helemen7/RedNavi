# mesa-bridge: Mesa's `ac_drm_*` layer over the RedNavi user client

`src/ac_apple_iokit.c` implements the functions of Mesa's `src/amd/common/ac_linux_drm.h` (the layer RADV's amdgpu winsys calls instead of libdrm) on top of `IOConnectCallStructMethod` to the kext (`RedNavi/rn_uapi.h`, version 6). It is the file `docs/M5_MESA_BUILD.md` calls `ac_apple_iokit.c`. Mesa is MIT licensed, so the file is meant to live in a Mesa fork; it sits here to be built, tested and kept in step with the kext.

## What is here

- `src/ac_apple_iokit.c`: device open and interface check, the queries (`DEV_INFO` cached at open, hw ip, firmware versions, heaps, gpu info as libdrm derives it), buffers (create, CPU map, free), the user-space virtual address allocator (two ranges), mappings (`bo_va_op*`), contexts, `cs_submit_raw2` with its chunks (IB, syncobj in and out, timeline wait and signal, dependencies), fence status, and a `struct util_sync_provider` over the kext's syncobjs (create, destroy, wait with DRM's absolute deadline, reset, signal, timeline signal and wait, query, transfer).
- `include/ac_apple_iokit.h`: the prototypes (those of Mesa at commit `01bab93`) and the types, so that the bridge builds without a Mesa tree.
- `include/amdgpu_compat.h`: libdrm's `amdgpu.h` types and the `amdgpu_drm.h` structures it needs, copied unchanged (MIT).
- `test/bridge_test.c`: what `tools/rn_test.c` does, through the `ac_drm_*` functions only, in the way the winsys calls them. `make -C mesa-bridge test`, run `./bridge_test` on the machine with the card. Verified on the 7800 XT: gpu info, heaps, a 32-bit buffer in the upper half and a code buffer in the lower half, a dispatch submitted with an IB chunk and a syncobj chunk, the syncobj and the fence status, a timeline point, a command buffer in the upper half refused, a range handed out again after it was freed.

## What it does differently from Linux, on purpose

- **Address ranges**: a buffer goes to the upper half only with `AMDGPU_VA_RANGE_32_BIT`; everything else, `AMDGPU_VA_RANGE_HIGH` included, goes to the lower half, because a command buffer in the upper half hangs the compute queue (`docs/COMPAT.md`).
- **Memory**: every domain is host memory behind the GART, 1 MB per buffer, 32 per connection (the kext's limits): a 64 MB request is refused.
- **Queues**: one compute queue; all contexts share it and its sequence numbers. Graphics and SDMA submits are refused (`-EINVAL`).
- **Not supported**: dma-buf and sync-file export and import, user memory buffers, user queues, sensors, video caps, the timestamp. They return `-EOPNOTSUPP` or `-ENOSYS`.

## Putting it into Mesa

The patches around it are listed in `docs/M5_MESA_BUILD.md`: the meson conditions for libdrm_amdgpu and the winsys, a Darwin branch in `ac_linux_drm.h` and `ac_linux_drm.c` that routes to this file (as the virtio branch routes to `amdvgpu_*`), the physical-device creation in RADV that finds the IOService instead of opening a render node, the sync type from `vk_drm_syncobj_get_type_from_provider`, and the external-memory extensions switched off. None of that has been built: no full Mesa tree has been configured yet.
