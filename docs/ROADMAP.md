# Roadmap

Current goal: **M5, RADV through a libdrm_amdgpu shim**, on top of **M4, command submission, which works on the RX 7800 XT as of 2026-11-01** (a queue mapped by the MES runs a compute dispatch started from user space). The native framebuffer of M3 works on one DisplayPort connector. Other cards (RX 7600 / Navi 33 first) are tracked in the issues; see `docs/ARCHITECTURE.md` for how Navi 33 differs.

| Milestone | Scope | Test |
|---|---|---|
| M0 | Repo, CI on a macOS runner, build scripts | CI green |
| M1 | Probe kext: PCI info, BARs, IP discovery dump (read-only). **Done 2026-10-27**, plus read-only PSP/SMU, MMHUB, DMCUB and VBIOS reads | `ioreg` shows the IP versions and registers |
| M2 | LinuxKPI-style compatibility layer for XNU; PSP 13 and SMU 13 bring-up; DMUB firmware load. **Done in the probe (2026-10-28): PSP chain, secure OS, ring, TOC, TMR, DMCUB load and mailbox.** The SMU 13 firmware loads and answers messages (2026-10-29). Remaining: SMU feature and clock management, and the compat layer (started, see `COMPAT.md`) | Firmware loaded, log of PSP/SMU state |
| M3 | Port of amdgpu DC for DCN 3.2 and an `IOFramebuffer` subclass. **2026-10-28: a test pattern, an image scanned out from VRAM, then the macOS desktop through `RedNaviFB` (an `IOFramebuffer`), on a DisplayPort monitor, modes from the EDID, changed live from System Settings (1024x768 to 1080p144)**; more connectors, a real DC port and exact bandwidth values remain | A native mode on one connector, desktop visible |
| M4 | GFX11, MES, SDMA 6, GPU VM, IH ring, user client (buffers, VA, submit, sync). **Done for compute, 2026-11-01: firmware loaded through the PSP, RLC boots, GART on both memory hubs, the MES starts and maps a legacy compute queue (`ADD_QUEUE`), and `tools/rn_test.c` runs a dispatch through the user client: shader result and fence read back, PASS on 5 of 5 boots, also after 2 minutes idle.** Queue submission is serialised by a lock (one boot, rn_test PASS twice). Left from M4: the graphics queue and SDMA through the MES, process queues (VM contexts 1 to 14), MSI interrupts to the CPU, GC power management (GFXOFF) | Command submission works (done for compute) |
| M5 | RADV on macOS via a libdrm_amdgpu shim | Offscreen Vulkan compute and triangle |
| M6 | Metal plugin on RADV (reusing the LGPL translator) and an accelerator kext for WindowServer | Metal app renders |

Notes:
- The path from the running DMCUB to a first picture is laid out step by step in `DC_PLAN.md`.
- PSP initialises before display and graphics; the display firmware (DMUB) is loaded through it.
- Each milestone should leave a test log format that users can paste into an issue.
- See `ARCHITECTURE.md` for what was learned from NullMoth and `COMPAT.md` for the compatibility layer.
