# M6: from Vulkan test programs to Metal and a GPU-composited desktop

Status: plan, 2026-11-02. M4 (command submission) and M5 (RADV through the kext) work in test programs. M6 is "Metal plugin on RADV plus an accelerator kext for WindowServer" (docs/ROADMAP.md). This page orders the work. Nothing in the sections after "Order" is done unless it says so.

## What M6 needs that does not exist yet

1. **A way to show a rendered image that is not "write the visible surface".** Today `vk_present` copies into the one surface the display scans out, so there is tearing, the program and WindowServer fight over the same memory, and the surface has to sit in the 256 MB the CPU can map. A compositor needs at least two surfaces and a flip at the vertical blank, and then a Vulkan swapchain (Mesa's WSI) on top of that. This is also what a `CAMetalLayer` drawable ends up as.
2. **An accelerator that WindowServer and Metal can find.** On macOS the GPU stack is an `IOAccelerator` subclass with shared-surface user clients, a Metal device plugin (`MTLDevice`) and a shader compiler path. NullMoth (NVIDIA) did this for another GPU; its design is described in its `docs/HOW-IT-WORKS.md` and summarised in docs/ARCHITECTURE.md. What exactly WindowServer asks of such a kext on Sequoia is not known here and has to be found out before any code is written (read-only study, clean-room: no code is copied from NullMoth's kexts or plugin).
3. **A Metal front end.** The LGPL translator (AIR to SPIR-V) can be reused as a separate component; the plugin that calls RADV has to be written for RADV's types (docs/ARCHITECTURE.md).
4. **Robustness the real workloads need**: SDMA for asynchronous copies, interrupts instead of polling, GPU power management, clearing of device-local buffers between clients, a real security boundary (the client is an administrator today).

## Order

1. **Presentation (this is the first block).** (a) A second display surface and a page flip at the vertical blank, exposed by the user client (`RN_UC_PRESENT`); (b) a Vulkan WSI platform in the Mesa patch set that makes `VK_KHR_swapchain` present through it. Why first: it is the only item that turns the test programs into something a window system can use, it is small and testable on the monitor, and every later piece (the accelerator kext, Metal's drawables) needs a flip primitive underneath. (a) is done and verified, see below; (b) is next.
2. **Study of the accelerator interface**, in parallel and read-only: what `WindowServer` and the Metal runtime look for (`IOAccelerator` matching, user client types, the `MTLIOAccelDevice` path, the plugin bundle format), written up in a new doc. This decides how large the rest of M6 is and should not wait for the end of step 1.
3. **Accelerator kext skeleton**: matches the GPU, publishes what the study found, with the user client of the existing probe behind it.
4. **Metal device plugin and translator integration**, starting with one trivial compute kernel through `MTLComputePipelineState`.
5. **Hardening, in the order the workloads need it**: interrupts (MSI and the IH ring) so waits do not poll, SDMA through the MES for uploads, clearing and isolation of device-local buffers, power management.

## What is deliberately not first

- **SDMA through the MES.** It takes copy work off the graphics queue, which only matters under load the test programs do not produce (the 60 fps cube loop leaves the queue idle between frames). It does not block a window system or Metal. It comes with step 5, or earlier if a workload shows the copies are the bottleneck.
- **Dynamic modesetting (issue #3).** EDID parsing, mode changes from System Settings and the mode-dependent registers are done (docs/DP_BRINGUP.md). What is left is exact bandwidth values from the display mode library (DLG/TTU are scaled from the 1080p60 capture) and more connectors; both matter for more monitors, not for M6, and are tracked separately.

## Step 1 (a): the flip, as built (2026-11-02)

- Display surface 0 is the desktop's: the one the plane scans out after a mode change (VRAM 0x08000000), which `RedNaviFB` gives to WindowServer, and which WindowServer keeps drawing into. A program that writes it fights WindowServer (bands of desktop over the picture; this was seen with `vk_present spin`). Surfaces 1 and 2, of the same size, follow it packed at 1 MB alignment inside the 34 MB between the scanout and the CPU-visible pool, as many as fit (3 at 1080p, 2 at 1440p, 1 at 4K). `rn_uapi.h` version 11: `RN_BO_SCANOUT` takes the surface number in `rn_uc_bo_create.pad`, `rn_uc_info.fb_surfaces` tells how many exist, `RN_UC_PRESENT` flips. When a connection that flipped away from surface 0 closes, the kext flips back to it.
- The flip is `hubp1_program_surface_flip_and_addr` for one plane: vsync flip type, high half of the surface address, then the low half; with `RN_PRESENT_WAIT` it returns when `SURFACE_FLIP_PENDING` is clear and `SURFACE_EARLIEST_INUSE` is the new address (`hubp1_is_flip_pending`). Register numbers are those of `dcn_3_2_0_offset.h` and agree with the replay table.
- The bridge exports `rn_bridge_scanout(surface, ...)` and `rn_bridge_present(surface, wait)`; `vk_present spin SECONDS FPS flip` renders into surfaces 1 and 2 in turn, waits for the GPU, flips and waits for the flip, and gives the screen back to surface 0 at the end.
- Verified on the 7800 XT: 2400 flips in 40 s at 60 fps, none pending after 50 ms (present wait 4.6 ms on average, 11 ms at most), pixel checks right, no queue reset. Step 1 (b), the Vulkan swapchain, is not started.
