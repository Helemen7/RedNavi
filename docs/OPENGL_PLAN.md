# OpenGL on the RX 7800 XT

Goal: OpenGL programs that need a 3.3 core context, Kitty first, run on the card. Status 2026-11-21: **a hardware OpenGL 4.1 context runs on the card; clears work, draws do not yet** (section "First run"). Status 2026-11-17 was research only. Marks: **V** = verified (on the laptop used for the research, an Intel UHD 617 Mac on macOS 15.7, or in a shipped file), **S** = read from sources or from strings, not run on our hardware, **U** = unknown.

## What macOS gives us

- **V** On this macOS, OpenGL is not a separate stack for new GPUs. The system ships three GL layers: the old per-vendor drivers (`AppleIntelKBLGraphicsGLDriver`, `AMDRadeonX6000GLDriver`, ...), and `AppleMetalOpenGLRenderer.bundle` (`/System/Library/Extensions`), which implements OpenGL on top of Metal. The last is a universal binary with an x86_64 slice, so it is not Apple-silicon only.
- **V** `AppleMetalOpenGLRenderer` imports only Metal (`MTLCopyAllDevices`, the pixel-format and swizzle helpers), IOSurface (`IOSurfaceLock`, notifiers, use counts), `IORegistryEntryGetRegistryEntryID` and the OpenGL framework itself. It imports no `IOAccel*` or `IOConnect*` function. Its strings show `GLR_OVERRIDE_MTL_INDEX` (it picks an `MTLDevice` from the list) and `Metal%@GLDriver`. **S** So it does not talk to the kernel accelerator itself: what it needs from us is a Metal device that works, which is what the plug-in provides. It also builds Metal shaders at run time (`gldBuildComputeProgram`, `cl2Metal`, the plug-in return-data helpers `gldMetalLibFromPluginReturnData`), so GLSL goes GLSL -> Apple's GL compiler -> AIR -> our AIR to SPIR-V translator -> RADV.
- **V** The GL driver that CGL loads is named by the accelerator's registry property `IOGLBundleName` (the Intel one publishes `AppleIntelKBLGraphicsGLDriver`; **S** the OpenGL framework reads it from the accelerator, the framework is in the dyld cache and was not inspected).
- **V** `RedNaviAccel2` already has the switch: boot-arg `rednavi_accel2_exp` bit 6 (value 64) publishes `IOGLBundleName = AppleMetalOpenGLRenderer` and `VRAM,totalMB` (`RedNaviAccel2/kern_accel2.cpp`, `rnPublishCaps`). Nobody has run with it set; `newGLContext()` in the kext is still a stub that fails, and it is **U** whether the GL-on-Metal path ever calls it (the renderer's imports say it does not, but the OpenGL framework in front of it might).

So the cheapest route is to find out whether Apple's own GL-on-Metal renderer works on our Metal device. No new GL code is needed for that, only gaps in the Metal plug-in.

## Kitty

- **V** Kitty 0.45.0 is installed on the laptop (`/usr/local/bin/kitty`); whether it is on the desktop is not known. Kitty needs OpenGL 3.3 core: instanced draws of glyph quads, texture arrays or buffer textures for the glyph atlas, alpha blending, uniform buffers, no depth buffer, no multisampling. **S** (from Kitty's behaviour, not read from its source here.)
- Gaps in the Metal plug-in that matter for that (README, "What this is not"): no depth/stencil (not needed), no multisampling (not needed), few attachment formats (the window's drawable is BGRA8, which exists), texture arrays and buffer textures (**U**, untested in the plug-in).

## The experiment

Files added for it, all new and not touching WindowServer-affecting code (`RedNaviMTL/compute.m`, `RedNaviMTL/rn_vk.c` are untouched):

- `tools/gl_probe.m`: lists the CGL renderers, creates a 3.2 core context on each, prints `GL_RENDERER`/`GL_VERSION`, compiles a vertex and a fragment shader, draws two instances of an alpha-blended quad into a texture-backed framebuffer and checks five pixels. **V** on the laptop: it passes on the Intel renderer (`Intel(R) UHD Graphics 617`, 4.1), so the program itself is right.
- `scripts/gl_experiment.sh [user@host]`: builds the probe, copies it to `~/rn/gl` on the test machine, runs it, and prints the registry's `IOGLBundleName` and the GL/Metal log lines of the last two minutes.

Steps when the desktop is back (boot-args are changed with `scripts/rn_rollback.sh`; check `status` first, since the current `rednavi_accel2_exp` value, if any, must keep its other bits and add 64):

1. `scripts/rn_rollback.sh -n set rednavi_accel2_exp=<old|64>` to see the change, then without `-n`; reboot (a boot-arg applies at the next boot; WindowServer is not restarted by this plan).
2. After the reboot: `ioreg -rc IOAccelerator -l | grep IOGLBundleName` must show `AppleMetalOpenGLRenderer`. If not, the property is published too late or on a different node: read `rnPublishCaps`.
3. `scripts/gl_experiment.sh`. Run `gl_probe` from the desktop's own Terminal if SSH sessions cannot create GL contexts (the window server refuses some of them from an SSH session; unverified for CGL off-screen contexts).
4. Kitty: copy `kitty.app` over, run `kitty --debug-gl` from the desktop's Terminal, and keep the output.

What each outcome means and what to do:

| Outcome | Meaning | Next |
|---|---|---|
| Only the software renderer (`0x00020400`) in the list, or no hardware context | CGL did not accept our accelerator for GL: the property, `IOVARendererID`-like keys or a GL context request to the kext (`newGLContext`) are missing | Compare the registry of the Intel accelerator with ours; implement `newGLContext` and the GL drawable user client minimally (kext work), or fall back to option B |
| A hardware context, shader compile or link fails | The AIR translator or the plug-in lacks something the GL compiler emits | Read the failure with the translator's log (`docs/M6_AIR_TRANSLATOR_LICENCE.md`, `docs/M6_METAL_ON_RADV.md`); fix in the plug-in/translator; this is the "gaps the GL path needs" list |
| Context and shaders work, pixels wrong | A plug-in draw-state bug (blending, instancing, vertex descriptor) | Bisect with the probe; the plug-in's selftest (`RedNaviMTL/rn_vk_selftest.c`) is the place for a regression test |
| `gl_probe` passes | GL-on-Metal-on-RADV works for the basics | Try Kitty; the open items become texture arrays, buffer textures and performance |

## First run (2026-11-21, desktop, **V**)

With `rednavi_accel2_exp=127` (bit 6 set), `IOGLBundleName = AppleMetalOpenGLRenderer` is on the accelerator and CGL loads the renderer, which loads our plug-in. What stood in the way, in order:

1. `/Users/Shared/rednavi-on` lists only `WindowServer`, so any other process gets no Metal device and CGL offers only the software renderer (`0x01020400`, no 3.2 core pixel format: Kitty's `NSGL: Failed to find a suitable pixel format`). A GL client must be let in (its name in `rednavi-on`, or `RN_METAL_COMPUTE=1`).
2. The renderer asks `respondsToSelector:@selector(originalObject)` and uses the answer instead of the device; the plug-in's catch-all resolver answered nil and the renderer crashed in `strdup` (`GLDDeviceRec::initWithDisplayMask`). The resolver now refuses `originalObject` (`RedNaviMTL/plugin.m`).
3. The renderer accepts only a device whose name contains AMD/ATI, NVIDIA or Intel ("Unknown GPU!!" and an assertion otherwise); the base class answers "Apple". The plug-in now answers the RADV device name and vendor "AMD", in every process but WindowServer (`RedNaviMTL/compute.m`).
4. `maxThreadsPerThreadgroup` returns a struct, which the catch-all stub cannot answer (crash); it, `supportsSampleCount:` (1 only) and `gpuCoreCount` are implemented.

Then `tools/gl_probe.m` gets renderer `0x01027f00`, accelerated, `GL_VENDOR AMD`, `GL_RENDERER AMD Radeon RX 7800 XT (RADV NAVI32)`, `GL_VERSION 4.1 Metal - 89.4`, GLSL 4.10. The clear colour is right; the two blended quads are not drawn. The renderer logs `Fallback to SW vertex processing because buildPipelineState failed` and the plug-in logs `render pipeline asked: vertex (null), fragment (null)` / `the function is not from a RedNavi library`: the GL renderer's shader functions do not come through the plug-in's `newLibraryWithData:`-style paths. Next: find how the renderer makes its `MTLFunction`s (the `gldMetalLibFromPluginReturnData` / compiler path) and give the plug-in that entry. Also logged and unimplemented: `-[MTLTexture swizzleKey]`, texture views with a different swizzle, `setColorStoreAction:atIndex:`, sampler states with LOD clamps.

This was run with the new plug-in loaded only into the test process (`DYLD_INSERT_LIBRARIES` with a small `dlopen` shim), not installed.

## If that fails: the options

All of these are **S** (assessment from the sources and from how the projects are built; none was tried here).

| Option | What it is | Cost | Licence |
|---|---|---|---|
| A. Fix the Metal plug-in's gaps (above) | Keep Apple's GL front end, make our Metal device good enough | Small to medium, incremental, helps every Metal client | Apple's code stays on the system; our plug-in is BSD-3 and the translator is LGPL as a separate component |
| B. Mesa Zink | OpenGL on Vulkan, on top of our RADV (Mesa's `-Dgallium-drivers=zink`) | Large: Zink is built on macOS upstream, but there is no CGL/NSGL window-system layer for it, so a program such as Kitty (GLFW, NSOpenGLContext) would need an interposed OpenGL/CGL implementation that creates a Zink context and presents into a window. Needs more of Vulkan than the bridge has been tested for (the README lists stencil, blending, filtering, tessellation as untested) and a WSI that can present into a window, which today only exists for a headless surface that owns the display | Mesa is MIT: allowed |
| C. A custom GL-to-Vulkan layer | Our own | Very large and duplicates Zink | BSD-3, our own |

Recommendation: do the experiment first (it costs nothing but a boot-arg), then A. B is the fallback if Apple's GL renderer refuses to attach to a third-party accelerator; it is also the only route for programs that need a GL version above the 4.1 that Apple's front end offers.

## Not done

- No run on the card. No test of `newGLContext`, of `IOVARendererID`, of an off-screen context from SSH, or of Kitty on the desktop.
- The OpenGL framework (dyld cache) was not inspected, so the renderer-selection logic is inferred from the Intel registry entry and from the strings of the renderer bundle.
