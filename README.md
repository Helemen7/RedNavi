# RedNavi

A native macOS display driver for AMD **Navi 32 (RDNA 3)** GPUs, starting with the **Radeon RX 7800 XT** (`1002:747E`).

> **For developers only.** This is a research prototype, not a driver you can install and use. It needs OpenCore, SIP turned off and boot-args, it writes to the GPU from the kernel, and it can hang or crash your machine. Run it only on a system whose display does not depend on this card, with a backup EFI. There is no Metal and WindowServer does not use the GPU; what does run on the card is Vulkan (Mesa's RADV, through a bridge to the kext) in test programs.

Goal: a real framebuffer on the card, driven by our own kext (DCN 3.2 display engine), with no GOP, VESA or firmware framebuffer in the way, and then acceleration: RADV (Vulkan) on top of the kext, which runs compute and 3D tests today, and Metal on top of RADV, which is the part that is not started and where we need help.

## Status

**Pre-alpha, 2026-11-02.** Display (2026-10-28): the macOS Sequoia desktop is shown on a real RX 7800 XT, over DisplayPort, through our own kext. The kext loads the card's PSP firmware chain and our own DMCUB display firmware, trains the DisplayPort link, lights the pipe in a mode read from the monitor's EDID (tested from 1024x768 to 1920x1080 at 144 Hz) and scans out a linear framebuffer in VRAM that WindowServer draws into; the resolution and refresh rate can be changed from System Settings while it runs. The picture does not come from the firmware's GOP/VESA framebuffer: without the kext the card's output stays dark.

**Acceleration (2026-11-02):** the kext loads the GFX, MES, SDMA, IMU and RLC firmware through the PSP, starts the MES and maps a compute queue and a graphics queue through it, gives each user-space connection its own GPU address space, and manages 16 GB of VRAM. Mesa's RADV runs on top of it on macOS (a patch set and a bridge in [mesa-bridge/](mesa-bridge/), no libdrm): `vulkaninfo` lists the RX 7800 XT, and test programs run compute dispatches (a burst of 20000 submits, ring saturation, 20000 dependent barriers, five processes at once), draw triangles, and render a scene with a depth buffer, a sampled texture and 4x multisampling that is shown on the monitor through the display surface. A hung queue is reset by the kernel (compute and graphics). All of this is test programs run as administrator on a development machine; the numbers and what is not done are in [docs/COMPAT.md](docs/COMPAT.md) and [docs/M5_MESA_BUILD.md](docs/M5_MESA_BUILD.md).

What this is **not**: there is no Metal, and WindowServer still renders on the CPU (expect fans spinning during animations); no application uses the GPU, the only window system integration is a swapchain on a headless surface that owns the whole display (one application at a time, no windows), and no video, no power management of the GPU, no interrupts to the CPU (every wait polls). Modes come from the EDID, but only on one DisplayPort connector, and the display-mode bandwidth values are borrowed from a 1080p60 capture, so unusual timings may underflow. macOS also lists the firmware's boot framebuffer (Apple's `AMDSupport`, which binds to any AMD GPU) as a second, nameless display; terminating it panics, and terminating only its display connect (`rednavi_kill_boot_fb=2`) does not remove the entry. There is no cursor plane, clock or power management (SMU), hot-plug, audio or HDMI, and only one DisplayPort connector has been tested. It needs OpenCore, and the framebuffer kext needs SIP off (details below). It can hang the machine. There is no release.

| Milestone | What | State |
|---|---|---|
| M0 | Repo, CI (macOS runner), build scripts | Done, CI green (Debug and Release) |
| M1 | Probe kext: matches `1002:747E`, maps the register BAR, reads the IP discovery table | Done: ran on a real RX 7800 XT, versions recorded in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| M1.5 | Probe stage 2: read-only PSP and SMU mailbox registers | Done: PSP bootloader ready, sOS not running, SMU alive |
| M2a | PSP bring-up and DMCUB load, as opt-in probe stages (PSP firmware chain, secure OS, command ring, `LOAD_TOC`, `SETUP_TMR`, DMCUB via the PSP) | Done on a real RX 7800 XT: the secure OS answers commands, and our DMCUB firmware runs and answers mailbox commands (see [docs/PSP_BRINGUP.md](docs/PSP_BRINGUP.md), [docs/DMUB_BRINGUP.md](docs/DMUB_BRINGUP.md)) |
| M2b | SMU 13 bring-up, and the LinuxKPI-style compatibility layer that replaces the probe's hand-written register code | Started: the compat layer exists with host tests ([docs/COMPAT.md](docs/COMPAT.md)); the SMU answers messages after its firmware is loaded through the PSP. Clock and power management on top of that is not written yet |
| M3 | Native framebuffer: DCN 3.2 display engine + `IOFramebuffer` | Works on one DisplayPort connector: link training, a pipe state replayed from a Linux capture with the mode-dependent registers recomputed (OTG, DP stream, DTO, plane sizes, DISPCLK), the monitor's EDID modes, and live mode changes through `RedNaviFB` (see [docs/DC_PLAN.md](docs/DC_PLAN.md), [docs/DP_BRINGUP.md](docs/DP_BRINGUP.md)). Missing: more connectors, a real DC port instead of a replay, exact bandwidth (DML) values |
| M4 | GFX11, MES, SDMA 6, GPU VM, user client | **Done for compute and graphics (2026-11-01 and 02)**: PSP-loaded firmware, RLC boot, GART and VRAM page tables, the MES maps a compute queue and the graphics queue (`ADD_QUEUE`), per-connection GPU address spaces (VMIDs 1 to 7, 4-level tables), VRAM allocator (13.5 GB device-local, 78 MB CPU-visible), sequence-numbered fences per queue, DRM-style syncobjs with timelines, queue reset and a 8 s watchdog on both queues, a user client with a versioned struct interface (`tools/rn_test.c` tests it). Details in [docs/COMPAT.md](docs/COMPAT.md). Missing: SDMA through the MES, MSI interrupts to the CPU, GC power management (GFXOFF is held off), the clear-state buffer and golden registers of the RLC, buffers above 1 GB, clearing of device-local buffers before reuse (so the client must be an administrator) |
| M5 | RADV (Mesa) running on macOS through an IOKit backend for Mesa's `ac_drm_*` layer (plan: [docs/M5_PLAN.md](docs/M5_PLAN.md), build and results: [docs/M5_MESA_BUILD.md](docs/M5_MESA_BUILD.md)) | **Works in test programs (2026-11-02)**: RADV builds on macOS (Mesa `bd830ce`, `-Dvulkan-drivers=amd`, ACO), enumerates the card, compiles shaders and runs compute, 3D and multisampled rendering, checked pixel by pixel and on the monitor. Missing: window system integration, many Vulkan features (stencil, blending, filtering, tessellation, ray tracing, video are untested), sparse resources, external memory, performance work |
| M6 | Metal plugin on top of RADV + accelerator kext for WindowServer (plan: [docs/M6_PLAN.md](docs/M6_PLAN.md), kernel side: [docs/M6_ACCELERATOR_STUDY.md](docs/M6_ACCELERATOR_STUDY.md), plug-in side: [docs/M6_METAL_ON_RADV.md](docs/M6_METAL_ON_RADV.md)) | **In progress (2026-11-07)**: `RedNaviAccel2` lets Apple's own `IOAcceleratorFamily2` start on the card and macOS Metal lists the RX 7800 XT as a device (opt-in by boot-arg `rednavi_accel2=3`). The Metal plug-in (`RedNaviMTL/`) runs, through the official Metal API on RADV and verified on the card: compute kernels compiled by Apple's `metal` (AIR translated to SPIR-V by a separate LGPL component), buffers with offsets, textures and samplers, render pipelines with vertex descriptors (`[[stage_in]]`), draws (indexed or not, culling, alpha blending) into textures, passes that only clear, a blit encoder, and textures made from IOSurfaces (by copy); a render target can be put on the monitor through the page flip. **2026-11-07**: WindowServer composes on the GPU for the first time (Metal compositor, CoreAnimation and CoreDisplay's GPU pipe through the plug-in, frames copied into the scanout; transparency seen on the monitor), see [docs/M6_DISPLAY_ACCEL.md](docs/M6_DISPLAY_ACCEL.md). Not stable: WindowServer crashes seconds after the mouse moves, frames glitch, WindowServer must be restarted by hand after the accelerator appears. **Not done**: depth and stencil, multisampling, several attachment formats, zero-copy IOSurface sharing, video decode, a distribution of the translator without LLVM tools |

M4 and M5 give you compute and Vulkan on the card but do not change how the desktop feels; a smooth, GPU-composited desktop needs M6, which is under way: Metal sees the card and runs compute, the desktop still renders in software.

## Why not Apple's drivers (like CarrizoRed does)?

macOS has no RDNA 3 support at all and Apple's Metal shader compiler has no GFX11 back end, so there is nothing to impersonate. The approach that has been shown to work for an unsupported GPU is the one used by [NullMoth](https://github.com/nullmoth/nvidia-macos-driver) for NVIDIA: host the vendor's open kernel driver inside a kext, run the vendor's Vulkan driver in userspace, translate Metal to Vulkan on top.

For AMD that means: **amdgpu in a kext, RADV (ACO) in userspace, Metal -> SPIR-V translation on top.** See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Hardware this is developed on

ASUS ROG STRIX Z490-G GAMING, i7-10700K, **Intel UHD 630 as the primary display**, Sapphire RX 7800 XT as the development target (PCIe 4.0 x16). See [docs/TEST_MACHINES.md](docs/TEST_MACHINES.md).

Navi 32 boards (RX 7700 XT / 7800 XT) are in scope; reports from other 7000-series owners are welcome now that the probe works.

## Building

Needs macOS with the Xcode command-line tools, or just push to GitHub: `.github/workflows/build.yml` builds Debug and Release on a macOS runner and uploads the kext.

```
sh scripts/bootstrap.sh
make DEBUG=1
```

## Trying it

Load `RedNavi.kext` on a machine where the 7800 XT is **not** the display you depend on. By default the probe only reads:

```
ioreg -rn RedNaviProbe -l -x > probe.txt
python3 tools/decode_discovery.py probe.txt
```

It publishes the IP discovery table (`ip-discovery`), PSP/SMU registers (`mailbox-regs`), MMHUB and DMCUB registers and the VBIOS read from the PCI ROM as properties. `IOLog` output may not show up in `log show` (cause unknown), so read the properties.

The stages that **write to the GPU** (PSP load, ring, TMR, DMCUB load, DisplayPort training, the display replay) are off unless enabled with `rednavi_*` boot-args, and need firmware headers generated locally from linux-firmware (`tools/gen_psp_fw_header.py`, `tools/gen_dmub_fw_header.py`); firmware is not redistributed here. The boot-args are listed in [docs/PSP_BRINGUP.md](docs/PSP_BRINGUP.md), [docs/DMUB_BRINGUP.md](docs/DMUB_BRINGUP.md) and [docs/DP_BRINGUP.md](docs/DP_BRINGUP.md). They can hang the machine: use a system whose display does not depend on this card, with a USB copy of your EFI.

### Vulkan (RADV)

Needs `boot-args` for the acceleration stages (`rednavi_mes_step=5 rednavi_gc_init=1 rednavi_gc_awake=1 rednavi_mmhub=1`, plus `rednavi_gfx_queue=1` for the graphics queue; the full list is in [docs/COMPAT.md](docs/COMPAT.md)), the firmware headers for the GC, MES and SDMA files (`tools/gen_gc_fw_header.py`), and an administrator account. The Mesa side is built from a Mesa checkout with `mesa-bridge/patches/0001-radv-darwin-iokit.patch` (recipe in [docs/M5_MESA_BUILD.md](docs/M5_MESA_BUILD.md)); `mesa-bridge/tools/` has the ICD manifest and run scripts. Then `vulkaninfo`, and the programs in `mesa-bridge/test/`: `vk_dispatch` (compute), `vk_stress` (bursts, barriers, VRAM), `vk_draw` (a triangle), `vk_present` (a triangle, `scene` for depth, texture and 4x MSAA, `spin` for an animation) and `vk_swapchain` (the cubes through `VK_KHR_swapchain`). `tools/desktop_test.sh` deploys the kext to a test machine over SSH, reboots it and runs the kernel-level tests of `tools/rn_test.c`.

### The framebuffer

With `rednavi_fb=1` (and `rednavi_dcn_replay=14`) the probe publishes where the scanout surface is, and a second kext, `RedNaviFB.kext`, hands it to WindowServer. It is a separate kext because it links against IOGraphicsFamily, which lives in the system kernel collection, so OpenCore cannot inject it. Install it through the auxiliary kernel collection (SIP off, `codesign -f -s -`, `kmutil load -p`), as described in [docs/DP_BRINGUP.md](docs/DP_BRINGUP.md). If macOS also shows a second, nameless display, that is the stock firmware boot framebuffer, not a second output. Boot-arg `-wegnoigpu` (WhateverGreen) switches the Intel iGPU off so the card is the only display.

**Linux owners (no macOS needed):** `sudo python3 tools/linux_collect.py` from a text console with the monitor lit writes one `rednavi_report_*.tar.gz` (IP blocks, connectors, amdgpu log lines, the display registers of the connector that is lit; no EDID, no serial numbers) to attach to an issue.

Please post decoded output in an issue, together with your board and BIOS settings (Above 4G Decoding, Resizable BAR).

## Contributing

We want contributors. Good first areas, each of which will get an issue:

- LinuxKPI-style compatibility layer for XNU (memory, locks, workqueues, dma-fence, firmware loading)
- PSP 13 / SMU 13 bring-up
- DCN 3.2 modeset from the EDID and more connectors (HDMI, second DisplayPort)
- SMU 13 clock and power management
- Cursor plane, vertical blank interrupts, hot-plug
- SDMA through the MES, interrupts (MSI/IH) to the CPU, GC power management (GFXOFF) (finishing M4)
- Vulkan feature coverage on RADV: stencil, blending, filtering and mipmaps, indexed draws, more formats, uniform buffers; bug reports with a test program are as useful as fixes (M5)
- The Metal side (M6): the AIR to SPIR-V translator, depth and stencil, zero-copy IOSurface sharing, running WindowServer on the plug-in and hardening of the accelerator kext (the swap chain for Vulkan already works)
- Decoding the IP discovery dump (`tools/`)

Please do not copy code from projects with restrictive licences (NullMoth's kexts and plugin are PolyForm Noncommercial). The Metal translator in that project is LGPL-3.0 and may be reused as a separate component.

## License

BSD-3-Clause for the code in this repository. Register definitions and behaviour taken from the Linux amdgpu driver are MIT, see [NOTICE](NOTICE).

GPU firmware is not included. It comes from linux-firmware under `LICENSE.amdgpu`, which allows redistribution in binary form only, with AMD's notice reproduced, and forbids reverse engineering, decompilation and disassembly. That is why firmware is never committed here and each user generates local headers from files they fetch themselves.
