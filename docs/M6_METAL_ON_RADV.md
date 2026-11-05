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

## Metal render target on the screen (2026-11-05)

`tools/mtl_init_harness` with `RN_TRI_TEST` and `RN_PRESENT_SECONDS=N` runs the triangle test, then draws a triangle at the display's size (1920x1080 here) into a Metal texture, copies the pixels (RGBA to XRGB8888) into display surface 1 from `rn_bridge_scanout`, and calls `rn_bridge_present(1, wait)`; after N seconds it flips back to surface 0. Run on the 7800 XT: both flips returned 0, the earlier checks still pass. The copy is a CPU copy of `getBytes` rows, not a GPU blit: Metal textures cannot be imported as scanout memory yet (same limit as IOSurface). Whether the picture was on the monitor is reported by the person looking at it; the driver only knows the flip completed.

## `[[stage_in]]`, fragment textures, alpha blending (2026-11-05)

Test: `tools/mtl_init_harness` with `RN_QUAD_TEST` (shaders `mesa-bridge/test/vk_quad.metal`): a 32 x 32 target; pass 1 draws a quad with a 2 x 2 texture sampled by a runtime `MTLSamplerState` (nearest, clamp) in the fragment function, then a small square with the blend state `One` / `OneMinusSourceAlpha` and the colour (0.5, 0, 0, 0.5) over the white corner of the first draw. Verified on the 7800 XT: all seven probe pixels right (texel colours red, green, blue and white in the right quadrants, so the image and the texture coordinates are not flipped; (255, 128, 128, 255) inside the blended square; the pixels around it untouched). The triangle, texture, AIR and compute tests still pass.

- **Vertex descriptors.** `rn-capi` no longer refuses vertex functions with `[[stage_in]]` attributes; the translator puts `[[attribute(n)]]` at input location `n`. `MTLVertexDescriptor` becomes the Vulkan vertex input: attribute `n` reads from buffer `bufferIndex` (the Vulkan binding number is the Metal buffer index) at `offset`; a layout gives the stride and per-vertex or per-instance stepping. Vertex buffers come from `setVertexBuffer:`/`setVertexBytes:` at that index (the same buffer may also be a storage buffer of the shader). Supported formats: 8, 16 and 32-bit components (the 8 and 16-bit integer formats become the scaled Vulkan formats, so the shader reads them as floats; 32-bit integers stay integers), halves, floats, `UChar4Normalized_BGRA`. Not supported, and the pipeline is refused with a message: packed 10-10-10-2, stride 0, step functions other than per-vertex and per-instance, step rates other than 1. Vertex buffer indices are below 16.
- **Blending.** The colour attachment blend state (all factors including the constant colour ones, operations, write mask) is part of the Vulkan pipeline; `setBlendColor` is dynamic state. Only the first attachment's state has been tried.
- **Fragment textures and samplers** go through the same binding path as in kernels (set 1 for the fragment module); a runtime sampler state in a render pass had not been run before this test.
- **A bug the test found:** the render pass handed `fill_writes` array bases that already included the counter, so the first texture of a draw left a NULL entry in the list of images to transition (a crash in `images_out`). Triangle tests never bound a texture.
- **Still missing for a compositor:** depth and stencil, culling, indexed and indirect draws, `MTLVertexFormat` packed formats, several draws with different targets in one pass, IOSurface-backed textures, a blit encoder, a pass with no draw applying its clear.

## Textures from IOSurfaces (2026-11-05)

`newTextureWithDescriptor:iosurface:plane:` makes a texture whose contents live in an IOSurface. It is a copy, not a mapping: the kext has no host memory import, so the texture is an ordinary VkImage with its host copy, and the surface is synchronised at the borders of each command buffer. At `commit`, every IOSurface-backed texture the command buffer uses (render targets, sampled textures, textures of dispatches) is copied from the surface into the host copy (`IOSurfaceLock` read-only, row by row, the surface's own pitch to the packed host rows) and marked written, so the existing upload before the pass applies it; after the command buffer, the textures it wrote (render pass attachments, and dispatch textures with `MTLTextureUsageShaderWrite`) are copied back and the surface unlocked. `replaceRegion:` writes the surface as well, `getBytes:` reads it first. Supported: plane 0 of single-plane surfaces whose bytes per element match the pixel format and that are at least as big as the texture. Not supported: multi-planar (YCbCr) surfaces, IOSurface textures with several levels or arrays, cross-process sharing semantics beyond what the lock gives.

Test (`tools/mtl_init_harness`, `RN_IOSURFACE_TEST`, same shaders as `RN_QUAD_TEST`): the 2 x 2 sampled texture and the 32 x 32 render target are IOSurfaces with their pitch padded to 256 bytes; the texels go into the first through its base address, the target starts filled with 0x7f so a missing write-back would show, and the result is read from the target's base address with no `getBytes:`. All seven probes right on the 7800 XT, the same values as the plain-texture run. Cost: two full copies per use (surface to host copy to VRAM and back), so this is correct but not fast; a kext host-memory import would remove the copies on the host side.

## Blit encoder (2026-11-05)

`blitCommandEncoder` returns a `RedNaviMTLBlitCommandEncoder`. Copies and fills are done by the CPU at commit, in order with the passes and dispatches of the same command buffer: buffers are host memory and a texture's host copy is current after every pass (what the GPU wrote is copied back into it), so buffer to texture, texture to buffer, texture to texture (any sub-rectangle, same texel size), buffer to buffer and `fillBuffer:range:value:` are memcpy and memset. A texture written by a blit is marked as written by the host and uploaded before its next use on the GPU; an IOSurface-backed texture is synchronised as in the section above. `generateMipmapsForTexture:` (there is one level), `synchronizeResource:`, the optimize calls and fence calls do nothing. Slices and levels other than 0 fail the command buffer with a message.

Test (`tools/mtl_init_harness`, `RN_BLIT_TEST=1`): a buffer with a row pitch of 80 bytes into a 16 x 8 texture, an 8 x 4 rectangle from (2,1) to (5,3) of a second texture, that texture into a buffer with pitch 96 at offset 32 (the bytes around it stay untouched), `fillBuffer` over bytes 8 to 40, a buffer to buffer copy, and a copy into an IOSurface-backed texture checked through the surface's base address. 0 bytes wrong in all four checks on the 7800 XT; the IOSurface, triangle and texture tests still pass. Not tested: a blit between two GPU passes that use the same texture, blits of formats the render path does not support, `MTLBlitOption` (depth/stencil and PVRTC options are ignored).

## Clear-only passes, indexed draws, culling (2026-11-05)

Test: `tools/mtl_init_harness`, `RN_RASTER_TEST=<vk_quad.metallib>`. All checks pass on the 7800 XT.

- **A render pass without draws** now does its clears (`clear_targets` in `rn_vk.c`: `vkCmdClearColorImage` outside a render pass, then the host copies are brought up to date). Checked on a plain texture (0.25, 0.5, 0.75, 1 gives 64 128 191 255) and on an IOSurface-backed attachment (the surface, filled with 0x7f beforehand, holds the clear colour afterwards). Load actions other than clear, and store actions, are still not looked at: attachments are always loaded and stored.
- **Indexed draws.** `drawIndexedPrimitives:` with `MTLIndexTypeUInt16` and `UInt32`, `indexBufferOffset:` (bytes), `instanceCount:`, `baseVertex:` and `baseInstance:` map to `vkCmdBindIndexBuffer` and `vkCmdDrawIndexed`. Checked with five vertices (one dummy first, so base vertex 1 matters), indices at offset 8 in both index types: the four quadrant colours are right. Buffers now carry the vertex and index usage bits. Indirect draws are not implemented.
- **Culling.** `setCullMode:` and `setFrontFacingWinding:` are part of the Vulkan pipeline (there is no dynamic cull state in Vulkan 1.1), so a pipeline state holds up to 30 Vulkan pipelines, made when first needed, one per primitive type, cull mode and winding. Metal's clockwise front face is Vulkan's clockwise with the flipped viewport. Checked with a quad of two counter-clockwise triangles: hidden with back culling and Metal's default winding, drawn with front culling, and the two swap with counter-clockwise front faces.

## Load and store actions (2026-11-05)

Test: `tools/mtl_init_harness`, `RN_LOADSTORE_TEST=<vk_quad.metallib>`; all checks pass on the 7800 XT.

- **Load.** The Vulkan render pass always loads (`VK_ATTACHMENT_LOAD_OP_LOAD`) and `MTLLoadActionClear` is a `vkCmdClearColorImage` before it, so `Load` and `DontCare` both keep the old contents (a valid reading of "undefined"). Checked as a dirty-rectangle update: a target cleared to blue in one pass and drawn into red with a scissor rectangle with `MTLLoadActionLoad`, in a second command buffer and in the same one: the blue outside the rectangle survives. On an IOSurface-backed target a pixel the CPU wrote into the surface between the two command buffers also survives (the surface is copied in at each commit).
- **Store.** `MTLStoreActionStore` copies the attachment back to its host copy and, for an IOSurface, to the surface. `MTLStoreActionDontCare` skips both: the GPU image is updated, `getBytes:` and the surface keep their old contents (checked: a surface filled with 0x7f is untouched). A later pass that loads the texture still sees what the GPU drew. Multisample resolve stores are logged and treated as plain stores (no multisampling yet).
- **Cost.** The Vulkan render pass still loads and stores on the GPU whatever the actions say; only the copy-back to the host is saved by `DontCare`. Real `DONT_CARE` ops would need render passes per combination of actions.
