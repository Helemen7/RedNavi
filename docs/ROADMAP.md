# Roadmap

Current goal: **M6, Metal on top of RADV**, and the rest of **M5** (window system integration, Vulkan feature coverage). **M4 (command submission) and M5 (RADV through an IOKit backend for Mesa's `ac_drm_*` layer, see [M5_PLAN.md](M5_PLAN.md)) work on the RX 7800 XT in test programs as of 2026-11-02**: compute dispatches, triangles, and a scene with depth, texture and 4x MSAA shown on the monitor. The native framebuffer of M3 works on one DisplayPort connector. Other cards (RX 7600 / Navi 33 first) are tracked in the issues; see `docs/ARCHITECTURE.md` for how Navi 33 differs.

| Milestone | Scope | Test |
|---|---|---|
| M0 | Repo, CI on a macOS runner, build scripts | CI green |
| M1 | Probe kext: PCI info, BARs, IP discovery dump (read-only). **Done 2026-10-27**, plus read-only PSP/SMU, MMHUB, DMCUB and VBIOS reads | `ioreg` shows the IP versions and registers |
| M2 | LinuxKPI-style compatibility layer for XNU; PSP 13 and SMU 13 bring-up; DMUB firmware load. **Done in the probe (2026-10-28): PSP chain, secure OS, ring, TOC, TMR, DMCUB load and mailbox.** The SMU 13 firmware loads and answers messages (2026-10-29). Remaining: SMU feature and clock management, and the compat layer (started, see `COMPAT.md`) | Firmware loaded, log of PSP/SMU state |
| M3 | Port of amdgpu DC for DCN 3.2 and an `IOFramebuffer` subclass. **2026-10-28: a test pattern, an image scanned out from VRAM, then the macOS desktop through `RedNaviFB` (an `IOFramebuffer`), on a DisplayPort monitor, modes from the EDID, changed live from System Settings (1024x768 to 1080p144)**; more connectors, a real DC port and exact bandwidth values remain | A native mode on one connector, desktop visible |
| M4 | GFX11, MES, SDMA 6, GPU VM, IH ring, user client (buffers, VA, submit, sync). **Done for compute and graphics (2026-11-01 and 02): firmware loaded through the PSP, RLC boots, the MES maps a compute and a graphics queue, per-connection address spaces, 16 GB VRAM allocator, fences and syncobjs, queue reset and watchdog on both queues.** Left from M4: SDMA through the MES, MSI interrupts to the CPU, GC power management (GFXOFF), RLC clear-state buffer and golden registers | Command submission works (done) |
| M5 | RADV on macOS via an IOKit backend for Mesa's `ac_drm_*` layer (what RADV needs from the kernel: [M5_PLAN.md](M5_PLAN.md); build recipe and results: [M5_MESA_BUILD.md](M5_MESA_BUILD.md)). **Works in test programs, 2026-11-02**: `vulkaninfo`, compute stress, triangle, depth + texture + 4x MSAA on the monitor. Left: window system integration, feature coverage, performance | Offscreen Vulkan compute and triangle (done) |
| M6 | Metal plugin on RADV (reusing the LGPL translator) and an accelerator kext for WindowServer | Metal app renders |

Notes:
- The path from the running DMCUB to a first picture is laid out step by step in `DC_PLAN.md`.
- PSP initialises before display and graphics; the display firmware (DMUB) is loaded through it.
- Each milestone should leave a test log format that users can paste into an issue.
- See `ARCHITECTURE.md` for what was learned from NullMoth and `COMPAT.md` for the compatibility layer.
