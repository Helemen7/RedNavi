# Corner radius and small-icon glitches in the WindowServer composite: research notes

State of the investigation (2026-11-16). The cause is **not identified yet**. This page lists what was ruled out, how, and what is still open, so that nobody repeats the work.

Symptom (RX 7800 XT, RedNaviMTL loaded by WindowServer): window corners show steps instead of a smooth arc, the title-bar buttons and some close icons are square, small ghost pixels flash in Activity Monitor text. The glitch is in the rendered image (a Cmd+Shift+3 screenshot shows it), not in scanout. Each window's own surface is correct; the final composite pass has the wrong corner.

## Ruled out (verified)

| Idea | How it was checked | Result |
| --- | --- | --- |
| The corner draws sample a mask texture that is missing (NULL placeholder of zeros) | Pass dump now prints the function constants and binding kinds of each pipeline (`NNN_draws.txt`). | The draws that make borders and discs use `fc_image_function0` 0x0a / 0x0b with `fc_has_image_0A=0`: an analytic variant that has no texture on purpose. The two NULL placeholders are normal. |
| Function constants reach the translator wrong | Same dump: 59 fragment constants, passed by name, no capture failure. | Not the cause. |
| Bindings are lost across `setRenderPipelineState:` | Read `compute.m`: pipeline change only replaces `pso`; buffers, textures and samplers live in encoder arrays copied into each draw. | Not the cause. |
| A render-encoder selector is stubbed to zero | Unified log of the plug-in on the desktop: stubbed selectors are heap, queue and protection-option ones only. | Not the cause. |
| Sampler state (filters, address, border, LOD) | Read `rn_new_sampler_state`, `sampler_info` and the static-sampler copy. | Mapping Metal to Vulkan is consistent. |
| Derivatives or implicit-LOD samples in divergent control flow | The cached SPIR-V keeps the function constants as `Private` variables loaded at entry, so every branch condition on them is uniform per draw. | Not the cause. |
| Write-combined memory: a small texture written on one thread reaches the card with older lines when another thread submits | `mesa-bridge/test/vk_wc.c` on the desktop: 60000 rewrites of a partly filled cache line of a host-visible buffer, copied by the card, in the write-combined memory type (the first host-visible coherent type of RADV, the one `rn_vk_buffer_create` takes) and in the cached one, same thread, split threads, split with a fence. | 0 mismatches in 360000 submits. Not reproduced. |

## Still open

1. **Small textures that arrive stale.** In pass captures a few small client-memory textures (R8 36x14, 70x14, 40x14, 27x27 RG8, BGRA 21x19) have older or other rows on the card than the host copy, in 0.1 to 0.4 percent of the sampled textures. The recorded copy, the copy at submit and the caller's memory all agree. Barriers, the image pool, a host lock and a post-upload barrier did not change it. The objects are the same size class as the broken icons, so it may be related, but no link is proven. The write-combining explanation above did not reproduce in isolation.
2. **The analytic corner shader itself.** `fixed_frag_lpf_cpf` is translated into one 1.4 MB SPIR-V module with about 800 branches, function constants as `Private` variables, 8-bit integer arithmetic and 370 image samples that are never reached in the analytic variant. The variants validate with `spirv-val --target-env vulkan1.2`. Whether the coverage arithmetic of the 0x0a / 0x0b functions matches Metal at the pixel level has not been checked. The translator is an external library and is not part of this repository.
3. **Inputs of the analytic draws.** The vertex data holds packed half or integer attributes (the vertex shader reads buffer 1 directly, no vertex descriptor). Whether the translator reads them with the right width and sign has not been checked.

## How to continue without more reboots

* Replay one recorded draw (vertex buffer, fragment constants buffer, function constants from the dump) through the SPIR-V on a software Vulkan device or an interpreter, and compare the corner pixels with the capture. Needs a CPU path for the module; none is in the repository yet.
* The pass dump (`rednavi-passdump-arm`) now records the specialisation of every pipeline, so a broken quad can be matched with its exact variant.

## Replay validation (2026-11-17, capture 7255)

The pass dump now also stores the index buffer of indexed draws, and `tools/rn_replay.c` replays one dumped draw through `rn_vk.c` and prints the pixels it writes.

* Pass 124 d1 (cursor strip) replays to exactly the 2x16 white strip of the capture.
* Pass 049 d14 (row pill, analytic corner shader): the corner alpha profile (4, 8, 10, 11, 12) matches the captured pixels exactly.
* Pass 150 d2 and d6 (the Dock border and fill, 48-vertex 9-slice draws): the replay gives a smooth arc of radius about 16, and the capture shows the same smooth arc.

So the analytic corner shader, the Metal to SPIR-V translation and the Vulkan path are correct for every draw replayed so far. This capture mostly holds the login screen (taken after `killall -9 WindowServer`), and no stepped window corner is in it. A capture of the logged-in desktop with a visibly stepped window corner is still needed.

## Window shadow notch: a hazard between draws (2026-11-17, experimental)

The badly rounded window corner comes from the shadow shape pass, not from the corner shader. A shadow texture (93x93 inactive, 137x137 active, RGBA8) is drawn as nine slices of a 27x27 A8 mask (`SimpleVertex` + `AlphaTextureFragment`): nine draws in one render pass, one pipeline, one shared vertex buffer with 96-byte offsets per draw.

* In captures of the failing shapes (pass dump with the square-size whitelist), the top-edge slice renders as if its uv ran over the corner slice's range: a stretched copy of the mask corner instead of a flat edge.
* `tools/rn_replay.c` with `RN_ONEPASS=1` (all draws in one render pass over one vertex buffer) reproduces it offline on the RX 7800 XT with RADV. Separate render passes per draw are correct. Any barrier between the draws fixes it, including an execution-only `VERTEX_INPUT` to `VERTEX_INPUT` barrier; a wider destination access mask on the pass-start barrier does not.
* Per-draw snapshots restart the pass, which is why captures taken with them (and the lock and dump experiments) hid the bug.

`rn_vk_render_pass` now emits that barrier between the draws of a pass (`RN_VK_DRAWSYNC=0` turns it off). Status: **experimental**. The cause inside the driver is not known; the barrier is applied to every draw of every pass and its cost is not measured. A live capture after installing it shows both shapes correct (one capture, no fresh boot). The stale rectangles and torn bands are a separate bug and are still open.
