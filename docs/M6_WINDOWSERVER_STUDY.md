# What WindowServer, SkyLight and QuartzCore want from a Metal device (study, 2026-11-05)

Read-only, done on the development Mac (macOS 15.7.3); nothing was changed on the test desktop. Method: the selectors a framework references (`tools/framework_selrefs.m` reads the framework's `__objc_selrefs` in memory and intersects them with the methods of the Metal protocols, marking those the plug-in's classes lack), the C strings of the loaded frameworks (`tools/framework_strings.c`), the imported symbols (`dyld_info -imports`), and the translator run over the shader libraries that ship in macOS (`tools/air_batch.c`). No code of Apple's was disassembled. A selector that appears in a framework is a reference, not proof of a call on the path that matters: what follows is a list of what to prepare, and what is verified is marked.

## Who does what

- **SkyLight** (WindowServer) has two compositors, `CompositorSW.cc` and `CompositorMetal.mm` (`kWSCompositorMetal`); the log line `WS current compositor : %s` and `/tmp/WindowServer.metalinfo.out` (written by a WindowServer state dump) tell which one runs. It does not pick a device itself: `metal_device != nullptr && "CoreDisplay failed to provide a MTLTDevice"` is an assertion, so a Metal compositor without a device from CoreDisplay would be fatal, not a quiet fallback. The device comes from `CoreDisplay_DisplaySurface_GetMTLDeviceSPI`, and SkyLight imports `CARenderMTL*` (SetState, SetDestinationTextures) from QuartzCore: the drawing is QuartzCore's, SkyLight composites groups, shadows and ripples with its own shaders (`SkyLightShaders.air64.metallib`).
- **CoreDisplay** finds the device through the display's accelerator: strings `Failed to create MetalDevice for accelerator %d %#llx`, `Display accelerator changed`, `Service Port 0x%x, Accelerator Port 0x%x`. QuartzCore matches it by registry ID: `Could not find Metal device with registry id %llx`, and `Mismatched MTLCommandQueue and MTLDevice`. So the `RedNaviFB` framebuffer must point at `RedNaviAccel2` (the `rednavi_accel_link` stage) and `-[MTLDevice registryID]` must be the accelerator's registry entry ID (the device inherits it from the superclass; not yet checked against the framebuffer's `IOAccelIndex` link).
- Switches found as strings (not tried): the boot-arg `-disable_ws_acceleration` (CoreDisplay), the preference `BootToMetalCompositor` (SkyLight), QuartzCore environment variables including `CA_DISABLE_GENERIC_SHADERS`, `CA_FORCE_GENERIC_SHADERS`, `CA_DISABLE_INPLACE_SHADERS`, `CA_DISABLE_PRECOMPILED_PIPELINES`, `CA_DISABLE_DIRTY_REGIONS`, `CA_DISABLE_OPAQUE_FRAMEBUFFER`, `CA_DISABLE_ENCODER_SHARING`. These are how to ask for the simpler paths while the plug-in is incomplete (inferred from the names).

## Device and queue calls seen in the frameworks

`name`, `registryID`, `location`, `isHeadless`, `isLowPower`, `isRemovable`, `peerGroupID`, `recommendedMaxWorkingSetSize`, `maxBufferLength`, `supportsFamily:`, `minimumLinearTextureAlignmentForPixelFormat:`, `newCommandQueue`, `newCommandQueueWithMaxCommandBufferCount:`, `newBufferWith{Length,Bytes,BytesNoCopy}`, `newTextureWithDescriptor:` and `...iosurface:plane:`, `newSamplerStateWithDescriptor:`, `newLibraryWith{File,URL,Source}`, `newRenderPipelineStateWithDescriptor:error:`, `newRenderPipelineStateWithTileDescriptor:...`, `newComputePipelineStateWithFunction:error:`, `newDepthStencilStateWithDescriptor:`, `newHeapWithDescriptor:`, `newSharedEvent`, `newSharedEventWithHandle:`, `newRasterizationRateMapWithDescriptor:`. The plug-in answers all of them (the device class inherits Apple's implementation of many; only what the plug-in overrides is the plug-in's behaviour).

## Not answered yet (verified: `respondsToSelector` is NO on the plug-in's classes)

- **Command buffer:** `encodeSignalEvent:value:`, `encodeWaitForEvent:value:` (shared events; both SkyLight and QuartzCore reference them; they are how WindowServer orders GPU work against the display and against clients). `newSharedEvent` is on the device list above but only inherited. This is the first thing to implement.
- **Render encoder:** `setDepthClipMode:`, `setVisibilityResultMode:offset:`, `textureBarrier` (SkyLight), and the tile shading calls (`setTile*`, `tileWidth/Height`, `dispatchThreadsPerTile:`) which only matter on GPU families that have tiles.
- **Compute encoder:** `setThreadgroupMemoryLength:atIndex:`.
- **Texture:** `newTextureViewWithPixelFormat:` and the `textureType:levels:slices:` variants (texture views: SkyLight and QuartzCore both use them), `newRemoteTextureViewForDevice:`, `newSharedTextureHandle`, `remoteStorageTexture`, `swizzle`, `compressionType`, `gpuResourceID`. **Resource:** `setOwnerWithIdentity:` (both frameworks). `gpuResourceID` also on pipeline and sampler states.
- **Library:** `newFunctionWithDescriptor:error:` (QuartzCore).
- `-[MTLLibrary newFunctionWithName:constantValues:error:]` is present but **ignores the constant values** (it returns the plain function): see the next section.

## The shaders (measured with `tools/air_batch`)

| library | functions | translate to SPIR-V | not translated |
|---|---|---|---|
| CoreDisplay `default.metallib` | 4 | 4 | |
| SkyLight `SkyLightShaders.air64.metallib` | 37 | 34 | 3, framebuffer fetch (`ColorInput`) |
| QuartzCore `default.metallib` (the AIR slice; the other twelve slices of the fat file are native GPU binaries) | 161 | 147 | 11 framebuffer fetch, 3 threadgroup memory (`ThreadgroupBuffer`) |

"Translates" means the translator produced SPIR-V that `spirv-val` accepted; none of it has run on the card, and the translator's output for a function is not checked against what it should compute. The framebuffer-fetch functions are probably the "in place" shaders (`fixed_frag_*`, `InPlaceAlphaUnpremultiply`) that the `CA_DISABLE_INPLACE_SHADERS` switch name suggests can be turned off (not verified).

**Function constants are the big gap.** QuartzCore's fragment and vertex functions are built from one generic source with function constants (`fc_generic`, `fc_framebuffer_fetch`, `fc_has_*`, `fc_color_opcodes`, `fc_texture_function`, `fc_blend_function`, ...; `air.function_constant` occurs in 7 of the library's modules, plus the strings `Metal failed to specialize fragment function` / `vertex function`). `newFunctionWithName:constantValues:error:` must therefore evaluate the constants and either pass them to the translator (SPIR-V specialization constants) or substitute them into the bitcode before translation. The `MTLFunctionConstantValues` object, the `air.function_constant` metadata and what the translator offers for it have not been looked at yet.

Other things the shader metadata shows: half precision everywhere (`half4` colours, `Dv4_Dh`), 3D textures (`color_cube0`), `texture2d<half>`, `air.compile.framebuffer_fetch_enable`, argument-buffer-like structs (`config_buffer`).

## What the fallback looks like

Strings, not traced: QuartzCore `CoreAnimation: Failed to initialize Metal or OpenGLES contexts!`, `CoreAnimation: failed to create Metal context!`, `Software renderer! Simulating crash!`, `Metal failed to load library / pipeline library / binary archive / render pipeline / compute shader`, `Command buffer allocation failed!`; CoreDisplay `Failed to create MetalDevice.`, `Fallback GPU Display Pipe failed`, `CoreDisplay is not detached because of forcing GPU fallback`; SkyLight `MTLRenderPipelineState creation failed for device %@`, `This typically indicates a Metal compiler failure.`, `Error from Metal: %@`, `No MTLDevice.`. What the strings suggest and what is not known: a failure to create a context or a pipeline is handled by reporting and, in some places, asserting; there is no sign in the strings of a retreat to software after a command buffer fails at run time (the software compositor is chosen at start, as a boot-time decision: `BootToMetalCompositor`). The plug-in therefore should not expect a command buffer error to be survivable: a failed command buffer in WindowServer is more likely a frozen or blank screen than a quiet fallback to software. This is the main risk of enabling it, and the reason to keep the SSH route to a boot-arg that turns it off (`-disable_ws_acceleration`, to be checked) and the emergency USB EFI.

## Suggested order before WindowServer is allowed to use the plug-in

1. ~~Function constants in `newFunctionWithName:constantValues:error:` (translator side too).~~ Done 2026-11-05, see the section at the end.
2. Shared events (`newSharedEvent`, `encodeSignalEvent:`, `encodeWaitForEvent:`), `MTLSharedEvent` listeners and `notifyListener:atValue:block:`.
3. Texture views (`newTextureViewWithPixelFormat:...`), `setOwnerWithIdentity:`, `newFunctionWithDescriptor:error:`, `setDepthClipMode:`, `setVisibilityResultMode:offset:`, `textureBarrier`, `setThreadgroupMemoryLength:atIndex:`.
4. Framebuffer fetch (`ColorInput`) and threadgroup memory in the translator, or the environment switches that avoid those shaders.
5. Half-precision and 3D-texture formats in the render path (the plug-in supports RGBA8, BGRA8, R8, RG8, R16F, RGBA16F, R32F, RG32F, RGBA32F, R32U, R32I, RGBA8U), and BGRA8 / RGB10A2 / RGBA16F framebuffers for HDR paths.
6. Registry ID check between `RedNaviFB`'s accelerator link and `-[MTLDevice registryID]`.
7. Only then a run of WindowServer on the plug-in, from a state that can be undone over SSH.

## Function constants, as built (2026-11-05)

`-[MTLLibrary newFunctionWithName:constantValues:error:]` (and the completion handler variant) now keep the values with the function, and the pipeline creation hands them to the translator, which specializes the AIR as LLVM text before translating it (`translate_reflected_specialized` in the translator fork, exported to the plug-in as `rn_air_translate_stage_fc`; `RedNaviMTL/rn_air.h`). A resource that a constant switches off is not in the module's reflection, so the app does not have to bind it. Names are matched against the module's constants (the globals carry the C++ mangled name, `_Z<length><name>`). `MTLFunctionConstantValues` has no public getter: the plug-in reads the values through its internal class's `newIndexedConstantArray` and `newNamedConstantArray` (private; used only if the object answers them, otherwise the function is refused with a message rather than silently left unspecialized).

Test (`tools/mtl_init_harness`, `RN_FC_TEST=<vk_fc.metallib>`, shader `mesa-bridge/test/vk_fc.metal`): an int, two bools and floats, one constant left undefined (`is_function_constant_defined`), and a texture argument that exists only when a constant says so. Three cases (by index; by name with one constant undefined and no texture bound; texture argument present) give 0 of 64 wrong each on the 7800 XT. Not tried: constants on vertex and fragment functions of a render pipeline (the same call path), vector constants, a real QuartzCore function with its real constant set, caching the specialized modules (every pipeline creation runs `llvm-dis` and the translator again).
