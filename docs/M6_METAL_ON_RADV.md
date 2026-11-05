# M6: Metal objects on RADV (the plug-in side)

Status: started 2026-11-04. The kernel side makes Metal find a device (docs/M6_ACCELERATOR_STUDY.md, sections 9 and 10). This page is about what the plug-in
(`RedNaviMTL/`, loaded by Metal.framework into every process that uses Metal) has to implement so that the device does work, and how RADV (M5) carries it.
**V** = observed on the 7800 XT or in a shipped file; **I** = inferred, not checked.

## Why the plug-in and not the kernel

Metal asks the vendor plug-in for the objects that matter: the device class (`RedNaviMTLDevice`, a subclass of `MTLIOAccelDevice`), and through it the command queue,
command buffers and encoders (V: `MTLIGAccelCommandQueue` -> `MTLIOAccelCommandQueue` -> `_MTLCommandQueue`; the Intel queue overrides `-commandBuffer`, so those are vendor
classes too), buffers, textures, samplers and pipeline states. The base class `-newCommandQueueWithDescriptor:` calls `-newCommandQueueWithMaxCommandBufferCount:` and
that is where the vendor makes its queue (V: without an override the two base methods call each other for ever). The kernel client of the device (types 5 and 6) is only used for
the setup calls listed in the study (V). Nothing in the observed device creation submits GPU work. Consequence (I): the plug-in is free to implement the Metal objects on
anything, and RADV in user space is the obvious choice: it is already running here (M5), it speaks to the kext through the user client, and it brings its own shader compiler.

## The layer under the Metal objects: `RedNaviMTL/rn_vk.[ch]` (done, V)

A small C layer that opens RADV from the plug-in: `dlopen` of `libvulkan_radeon.dylib` and `vk_icdGetInstanceProcAddr`, **without the Vulkan loader** (RADV needs only the system
libraries and `libzstd` next to it, `@loader_path`), instance, device, one compute queue, a command pool. Host-visible storage buffers, a compute pipeline from SPIR-V with
N storage buffers and push constants, one synchronous dispatch (record, submit, wait). Nothing is opened until `rn_vk_open()`, so a process that only enumerates devices does
not pay for it, and the kext's user client is not opened by every Dock or Spotlight.

Test: `rn_vk_selftest()` (the shader of `mesa-bridge/test/vk_dispatch.comp`) called by `tools/mtl_init_harness.m` with `RN_VK_SELFTEST=1 RN_VK_DIR=<dir>` in a process that has
Metal.framework loaded: `rn_vk: device AMD Radeon RX 7800 XT (RADV NAVI32)`, `0 of 256 values wrong` (2026-11-04, stage 3, RedNaviAccel2 registered at the same time).

Where the libraries live: the bundle looks in `$RN_VK_DIR`, else `/Library/Application Support/RedNavi/vk` (RADV 19 MB, `libzstd`). Shipping them inside the bundle
(`Contents/Frameworks`) is the alternative, with the 19 MB signed along; undecided.

## Compute path through the Metal API (done, V, 2026-11-04)

`RedNaviMTL/compute.m`: `RedNaviMTLCommandQueue`, `RedNaviMTLCommandBuffer` (runs the recorded dispatches synchronously at `-commit`, then the completion handlers),
`RedNaviMTLComputeCommandEncoder`, `RedNaviMTLBuffer` (an `rn_vk_buffer`, `contents` is the mapped pointer) and `RedNaviMTLComputePipelineState`. They are classes of their own
that declare the Metal protocols, not subclasses of the Apple vendor classes. Handed out only in a process that has `RN_METAL_COMPUTE` set; without it `newCommandQueue*` answer nil and
`newBuffer*` go to the superclass, so Core Animation and WindowServer stay on the software path. Render and blit encoders are nil. The shader bypass is
`-[RedNaviMTLDevice rn_newComputePipelineWithSPIRV:bufferCount:pushBytes:]`; `-setBytes:length:atIndex:` at an index >= the buffer count fills the push constants.

Test (`tools/mtl_init_harness.m`, `RN_METAL_COMPUTE=1 RN_COMPUTE_TEST=1 RN_VK_DIR=~/rn/vk`), all through the official API (`newCommandQueue`, `newBufferWithLength:options:`,
`commandBuffer`, `computeCommandEncoder`, `setComputePipelineState:`, `setBuffer:offset:atIndex:`, `setBytes:length:atIndex:`, `dispatchThreadgroups:threadsPerThreadgroup:`,
`endEncoding`, `addCompletedHandler:`, `commit`, `waitUntilCompleted`):

```
queue 0x600003aca2a0, buffer 0x6000034c85d0 (length 1024), pipeline 0x6000034c0720
command buffer status 4, completion handler 1, error none
compute through Metal on RADV: 0 of 256 values wrong (v[255] = 772)
```

Not done in this slice: buffer offsets, `setBuffers`, textures, blit, render, `MTLLibrary` / `MTLFunction` (the shader still comes in as SPIR-V), asynchronous execution,
`MTLSharedEvent`, private-storage buffers in VRAM.

## Buffer offsets and a real Metal kernel (done, V, 2026-11-05)

Run on the RX 7800 XT through the official Metal API (`tools/mtl_init_harness`, `RN_METAL_COMPUTE=1`):

- SPIR-V compute: 0 of 256 values wrong, with the rewritten `rn_vk` (binding list, specialization variants).
- Two buffers with offsets 512 and 1024 through `setBuffers:offsets:withRange:`: 0 of 512 words wrong. A misaligned offset fails the command buffer (status 5) with an error message.
- An AIR kernel (`dispatch_add` from a `.metallib` built by `xcrun metal`/`metallib`) goes through `newLibraryWithData:`, `newFunctionWithName:`, `librn_air2spirv.dylib` (the LGPL translator, separate repo) and RADV: 0 of 256 values wrong.

The translator needs `llvm-dis` and `spirv-val` at run time. For this test they were copied next to the library: the real `llvm-dis` binary (the MacPorts `llvm-dis-mp-19` is only a wrapper script around `xcrun`) with `libLLVM.dylib` (124 MB), `spirv-val` with `libSPIRV-Tools.dylib`; the harness ran with `DYLD_LIBRARY_PATH` pointing there. Shipping libLLVM at that size is not acceptable for users: still open (options: a smaller static `llvm-dis`, or reading the bitcode in Rust without LLVM).

## Textures: where it stands (2026-11-05)

- Memory: a Metal texture from an IOSurface cannot be imported by RADV today. `ac_drm_create_bo_from_user_mem` only accepts the display surfaces, because the kext has no userptr. A copy-in/copy-out texture (VkImage in RADV memory, synchronized with the IOSurface at command-buffer boundaries) works without touching the kernel and comes first; importing host memory (a user-client selector that wires the pages and maps them in the GART) is an optimization for later.
- Interface: `rn_air_binding` now carries `kind` (buffer, sampled texture, storage image, sampler) and `dim`. The kernels `texture_copy` and `texture_sample` (`mesa-bridge/test/vk_texcopy.metal`) are the first test cases.
- Open finding: for `texture_copy` the translator reports a sampled texture at binding 32 and a storage image at 481, but the module it emits reads the source texture through a runtime array of sampled images in set 2 (indexed from a table in a storage buffer at binding 640, `BufferAddressTable`) and declares the storage image as `R32f`. That is not the per-binding ABI of its own `docs/REFLECTION.md`. Until this is understood (a translator limitation for sampled reads in kernels, or an ABI the plug-in has to implement with `descriptor_indexing`), texture kernels are refused by the plug-in. `texture_sample` is refused because of the `constexpr sampler`.

Distribution debt: `llvm-dis` and `libLLVM.dylib` (124 MB) must go before release; options are a bitcode reader in Rust or a static subset of LLVM.

## Order of work

1. **Queue, command buffer, compute encoder, buffer** (smallest slice that runs something). `RedNaviMTLCommandQueue` (subclass of `MTLIOAccelCommandQueue`, init by
   `-initWithDevice:descriptor:`) answering `-commandBuffer`; a command buffer class that records into a list and executes it in `-commit`; `MTLComputeCommandEncoder` methods
   `setComputePipelineState:`, `setBuffer:offset:atIndex:`, `setBytes:length:atIndex:`, `dispatchThreadgroups:threadsPerThreadgroup:`, `endEncoding`; `MTLBuffer` over an
   `rn_vk_buffer` (`contents` is the mapped pointer). Which methods each protocol needs is read from the protocol list at run time, not guessed (the stub already logs every
   selector nobody implements).
2. **Shader path (reader, `MTLLibrary`/`MTLFunction`, the translator seam and the translator itself, built from a separate repository, done as far as a local test goes; not yet run on the GPU; see M6_AIR_TRANSLATOR_LICENCE.md).** `-newLibraryWithSource:` is compiled to AIR by Apple's compiler service (not by the driver); the driver receives the function through the vendor hooks listed in
   the Intel class (`getComputeFunctionId:compilerOptions:...`, `computeVariantWithCompilerOutput:...`, `newComputePipelineWithDescriptor:variant:`). The AIR-to-SPIR-V
   translator has to be an LGPL component of its own (metal2vulkan, as in docs/ARCHITECTURE.md); until it exists a test can hand the plug-in SPIR-V through a private hook.
3. **Fences and events**: `MTLCommandBuffer` completion handlers on a `VkFence` thread; `waitUntilCompleted`.
4. **Textures, samplers, render pipelines, presentation**: textures over `VkImage`, a drawable of a `CAMetalLayer` over the swapchain of M6 step 1 (flip primitive of the kext).
5. **Device properties**: `name`, `registryID` (the accelerator's, V: the log shows it), limits, the 173 methods Intel's class adds over `MTLIOAccelDevice`; every `supports*` answered from RADV's
   physical device features. `featureProfile` is 10002 (V, section 9).

## Not decided / unknown

- One Vulkan device per process or a shared service: per process is how a Metal driver works (each process has its own device client) and is what is built; a service would save
  memory but needs IPC for every buffer.
- Memory: Metal's `MTLStorageModeShared` maps to host-visible memory (done); `Private` should be device-local VRAM, with copies through a transfer queue (the bridge has no SDMA).
- Whether Core Animation, once it gets a real queue, needs render pipelines at once (it does for compositing windows): until step 4 the queue must keep answering nil to
  its `newCommandQueue` so it falls back to software (V: this is how Terminal works now).
- Hardening: every client is an administrator today (docs/M6_PLAN.md, step 5).
