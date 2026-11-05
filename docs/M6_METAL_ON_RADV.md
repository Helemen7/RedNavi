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

## Textures, samplers and any grid size (done, V, 2026-11-05)

Run on the RX 7800 XT through the official Metal API (`tools/mtl_init_harness`, `RN_TEX_TEST`, kernels of `mesa-bridge/test/vk_texcopy.metal`): a 20 x 12 RGBA8 texture read by `texture2d<float, access::read>`, written by `texture2d<float, access::write>`, and read through a `constexpr sampler` (nearest, pixel coordinates); all four combinations of the two kernels and of `dispatchThreadgroups:` (3 x 2 groups of 8 x 8, bigger than the texture) and `dispatchThreads:` (exactly 20 x 12, so the edge threadgroups are smaller) give 0 of 240 texels wrong.

What it took:

- **Memory.** A Metal texture from an IOSurface cannot be imported by RADV today: `ac_drm_create_bo_from_user_mem` only accepts the display surfaces, because the kext has no userptr. A texture is a VkImage in RADV memory with a host copy ("staging", `rn_vk_image`): what the host wrote is copied in before the dispatch that uses the image, a storage image is copied back after it. The image stays in `VK_IMAGE_LAYOUT_GENERAL`. Importing host memory (a user-client selector that wires the pages and maps them in the GART) would remove the copies and is for later; an IOSurface-backed texture needs it or a copy per command buffer.
- **The translator's defaults are the NVK runtime's model, not a Vulkan pipeline layout.** By default it reads sampled textures from one runtime array of 131072 images in descriptor set 2, indexed from a table in a storage buffer (`BufferAddressTable`, binding 640). `NVMTL_NO_BINDLESS_ALL=1` (set by `rn-capi`) turns that off and gives the per-binding ABI of its `docs/REFLECTION.md`: sampled textures at 32 + n, storage images at 480 + n, samplers at 160 + n, all in set 0.
- **Storage image formats.** The translator infers a storage image's format from the texel type (a float4 write becomes `R32f`), which is wrong for RGBA8. `rn-capi` rewrites it to the unknown format with `StorageImageWriteWithoutFormat` (the device is created with that feature) unless the module uses image atomics.
- **A correction to the earlier compute work.** The `Workgroups` dispatch mode that `rn-capi` used first bakes `LocalSize 64 1 1` into the module; the specialization constants 0, 1, 2 that this page used to describe exist only in the other modes. The `dispatch_add` test passed because it dispatched 64 threads per group. The kernels are now translated with `ThreadsDynamic { offset: 0 }`: the threadgroup size is specialized per pipeline, the grid comes in 48 bytes of push constants (thread grid, thread base, threadgroup base, threadgroup grid), and `rn_vk_dispatch_threads` splits the grid into up to 8 regions (the full threadgroups and one smaller strip per edge, as `KernelDispatch::plan` does). That also settles `dispatchThreads:` with grids that are not a multiple of the threadgroup.
- **Interface.** `rn_air_binding` carries the kind (buffer, sampled texture, storage image, sampler, sampler written in the shader with its filter/address/border/LOD state) and the texture dimension. `rn_vk_dispatch` takes one `rn_vk_arg` per binding (buffer + offset, image, sampler).

Limits: 2D, one level, one sample, a dozen pixel formats (`rn_vk_format_of` in `compute.m`); no IOSurface-backed textures, no blit encoder (so no mipmap generation or texture copies), no texture arrays, cube or 3D textures, no multisampling. Not verified: textures sampled with filtering or normalized coordinates, runtime `MTLSamplerState` objects (code exists, untested), `read_write` textures.

Distribution debt: `llvm-dis` and `libLLVM.dylib` (124 MB) must go before release; options are a bitcode reader in Rust or a static subset of LLVM.

## Render encoder: triangles into a texture (done, V, 2026-11-05)

`RedNaviMTLRenderCommandEncoder` draws into 2D textures through the official Metal API on RADV. Test (`tools/mtl_init_harness`, `RN_TRI_TEST`, shaders `mesa-bridge/test/vk_tri.metal`): a 40 x 24 RGBA8 target cleared to grey, one triangle in the top-left quadrant of Metal's clip space with a different colour at each corner, the vertices read from buffers with `[[vertex_id]]`. Verified: 118 of the quadrant's 240 pixels drawn (a half is about 120), the three corners red, green and blue with the interpolated values the barycentric coordinates give (38 185 32 at one probe), nothing outside the quadrant, the bottom-left and bottom-right still grey (so the image is not flipped). The earlier compute, texture and AIR tests still pass.

What the translator does for vertex and fragment functions (read off its output, not assumed; the step that settled the design):

- **Descriptor sets collide.** Both stages default to set 0 and the buffer band 0-31, so vertex `buffer(0)` and fragment `buffer(0)` would be the same binding. `rn-capi` translates the fragment function with a descriptor layout in set 1 (`TransformOptions::with_descriptor_layout`); sets 2 and 3 are the translator's own (bindless heap, input attachments). The pipeline layout has the vertex set at 0 and the fragment set at 1.
- **Varyings** meet by `Location`, derived from the names: vertex output and fragment input both came out at location 0 in two independent translations. Not verified with several varyings or differently ordered structs.
- **Clip space.** The vertex shader stores `Position` as written, no y flip. Vulkan's clip space is y-down, so the host sets the viewport with origin at the bottom and a negative height (core Vulkan 1.1). The fragment shader uses `OriginUpperLeft`, which matches Metal's fragment position.
- Vertex modules declare the Int64 capability; the device is created with `shaderInt64`, `vertexPipelineStoresAndAtomics` and `fragmentStoresAndAtomics` when the card has them. The queue family now has to do graphics as well as compute.

How it is built: `rn_air_translate_stage` (vertex 1, fragment 2); `rn_vk_render_create` makes a classic render pass (every attachment loaded and stored, always in `GENERAL`, clears done with `vkCmdClearColorImage` before the pass) and a graphics pipeline with an empty vertex input, dynamic viewport and scissor, no culling, no depth, no blending, one sample. Vulkan fixes the topology in the pipeline and Metal passes it with the draw, so a pipeline is made per primitive type on first use. `rn_vk_render_pass` writes the descriptor sets of every draw from a per-pass pool, copies what the host wrote in, runs the draws, and copies the targets back to their host copies. In `compute.m` the command buffer keeps compute dispatches and render passes in one ordered list (they run synchronously at commit).

Limits (what is refused or ignored; the render encoder and the command queue exist only in processes with `RN_METAL_COMPUTE` set, so WindowServer is not affected):

- Vertex functions with `[[stage_in]]` attributes are refused (no vertex descriptors yet): vertices have to come from buffers.
- No depth or stencil, no blending (logged and ignored), no culling or winding state (culling is always off), no indexed draws, no indirect draws, no multisampling, no tessellation.
- A render pass with no draws does not apply its clear. Store actions are ignored (the target is always stored). Only textures made by this plug-in are render targets.
- Not verified: several draws or passes in one command buffer, instancing, fragment textures and samplers in a render pass, more than one colour attachment, line and point topologies, runtime `MTLSamplerState` in a render pass.

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
