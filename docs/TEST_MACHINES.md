# Test machines

## Primary development machine

| Item | Value |
|---|---|
| Board | ASUS ROG STRIX Z490-G GAMING, BIOS 3001 |
| CPU | Intel Core i7-10700K (Comet Lake) |
| RAM | 16 GB |
| Primary display | Intel UHD 630, `8086:9BC5` |
| Target GPU | Sapphire Radeon RX 7800 XT, `1002:747E`, subsystem `1DA2:D475`, rev `C8` |
| Link | PCIe 4.0 x16 (bus 3, device 0, function 0 on Windows) |
| Windows driver | 32.0.31041.1004 |

macOS (Sequoia 15.8.2) is booted on this machine through OpenCore. For most experiments the UHD 630 drives the screen, so the 7800 XT is free and panics can be read. Once the framebuffer works, `-wegnoigpu` (WhateverGreen) switches the iGPU off and the 7800 XT is the only display; keep SSH (Remote Login) enabled and a USB copy of the EFI without the kexts for that case. The machine boots macOS first and auto-mounts the EFI, so kext updates and logs can be handled remotely. Boot-args: `-v debug=0x100 keepsyms=1` plus the `rednavi_*` ones (for acceleration also `rednavi_mes_step=5 rednavi_mes_clean=1 rednavi_gc_init=1 rednavi_gc_awake=1 rednavi_mmhub=1 rednavi_gfx_queue=1`) (no `-wegnoegpu`); OpenCore file logging is on. With `-wegnoigpu` the 7800 XT is the only display: if the monitor is off at boot the machine does not come up on the network, so keep it on during tests. The internal EFI auto-mounts at `/Volumes/EFI` when booted from the internal disk, but its disk number moves when USB sticks are plugged in (`disk0s1` with none, `disk3s1` with the Linux and emergency sticks in). Before writing to it, identify it by what it is, not by its number: `diskutil info /Volumes/EFI` must say `Device Location: Internal` and `EFI/OC/config.plist` must carry the `rednavi_*` boot-args. The EFI of the stick is external; the EFI of the other internal disk (Windows) has no `EFI` folder. A kext that fails to link or load freezes the boot on the test screen: boot the stick, fix the internal EFI over SSH, reboot.

## To collect before running anything on macOS

- [x] Above 4G Decoding / Resizable BAR state in the BIOS: Resizable BAR ON. VRAM is 16 GB.
- [ ] VBIOS dump (GPU-Z), optional
- [x] IP discovery: read by the probe from VRAM (no Linux needed), see `docs/ARCHITECTURE.md`
- [ ] ACPI VFCT table presence

## The splash screen

Until WindowServer draws the desktop, the monitor shows the RedNavi splash (`assets/splash_1920x1080.png`, by Helemen7, embedded run-length encoded in `RedNavi/splash_1920x1080.h` by `tools/gen_splash.py`) in the 1920x1080 mode. It sits where the colour test pattern was and means the same: a screen that stays on it is a boot that did not go on (a kext that did not link, a hang). Boot-arg `rednavi_testpattern=1` brings the test pattern back, which is what to use when checking pitch, offset or format. Other modes still get the test pattern until a splash exists for them.

## Rollback over SSH: `scripts/rn_rollback.sh`

Before a test that can take the desktop's picture or WindowServer with it (the Metal plug-in registered in WindowServer's process, `rednavi_accel2=3`), the way back must exist and have been used once. `scripts/rn_rollback.sh` reads and changes the boot-args in the OpenCore config of the internal EFI over SSH, always after a backup (`config.plist.pre-rollback-<date>`, never overwritten), and puts a backup back. It needs no sudo on the desktop (the EFI is writable by the user) and it only accepts an EFI that is internal and whose config has the rednavi boot-args. Commands: `status`, `backup [TAG]`, `set ARG=VALUE ...`, `unset NAME ...` (a name ending in `*` is a prefix), `safe` (`rednavi_accel2=2`: the accelerator answers Metal's questions but is not registered, WindowServer is untouched), `off` (removes `rednavi_accel2*`), `restore [FILE]`, `processes` (stops the test programs), `root-help` (the commands that need the desktop's password), and `-n` in front of a command to show what it would do. A boot-arg change applies at the next boot: the script does not reboot, and what needs root (restarting WindowServer, rebooting, removing the installed bundle or kext) is printed by `root-help`, not done.

Tested on the desktop on 2026-11-05: `status`; every changing command with `-n` (the new boot-args differ from the current ones only in `rednavi_accel2`); `safe` followed by `restore`, after which `config.plist` is byte for byte the file it was before (same md5); `processes`. Not tested: a reboot with the changed config (nothing was rebooted), and `off`/`unset` for real. If the desktop does not answer over SSH at all, nothing here helps: power-cycle, boot the USB stick with the OpenCore EFI without the kexts, then `scripts/rn_rollback.sh safe` or `restore` works over SSH again.

## The firmware framebuffer (`.Display_boot`)

The stock `IONDRVFramebuffer` the firmware leaves on `GFX0` has no accelerator, and macOS lists it as a second "Display" with the monitor's serial number. **It is removed by default now (verified on the desktop, macOS 15.8.2, 2026-11-05): one display, no panic, the accelerator registers as before (Metal 3 in System Information after the 90 s delay).** `rednavi_kill_boot_fb` in `RedNaviFB/kern_fb.cpp` (`rednavi_keep_boot_fb` is read by nothing and does nothing):

| value | what | result |
|---|---|---|
| 0 | keep the firmware framebuffer | a second display |
| 2 | terminate only its `IODisplayConnect` | the framebuffer is still listed as a display |
| 1 | `IOFramebuffer::terminate` | **kernel panic** |
| 3 | its `AppleUpstreamUserClientDriver` first, then `IOFramebuffer::terminate` | **kernel panic**, same place |
| **4** (default) | `IOService::terminate(kIOServiceRequired)` | **one display, no panic** |
| 5, 6 | 4 after terminating the `AppleUpstreamUserClientDriver`; 4 synchronous | not run |

Why the panics: the panic reports (2026-10-29 and 2026-11-05, same stack) show `RedNaviFB::removeBootFramebuffer` -> `IOFramebuffer::terminate` -> `deliverFramebufferNotification(kIOFBNotifyTerminated)` -> `AppleUpstreamUserClientDriver::fbNotificationOccurred`, a null dereference at 0x1e bytes into Apple's handler. In IOGraphics' source, `IOFramebuffer::terminate` calls `IOService::terminate` first, which starts tearing down the framebuffer's children, and only then delivers the termination notification to the registered notifiers; the notifier of that driver outlives its target (terminating the driver first, mode 3, did not change that). Mode 4 calls the base class directly, so the override and the notification are skipped. What the skipped notification would have told WindowServer is not visible: after the removal the registry has only `RedNaviFB` under `GFX0`, System Information lists one display, and nothing misbehaved in the minutes watched. Remaining trace of the old framebuffer: `AGPM`'s property `attached-gpu-control-path` still names `.../GFX0@0/IONDRVFramebuffer/AGPM` (a string). Not checked: a second reboot with the new default, a long WindowServer session, sleep/wake.

To try another value: `scripts/rn_rollback.sh set rednavi_kill_boot_fb=N`, reboot; if the desktop panics, boot the USB stick and `scripts/rn_rollback.sh unset rednavi_kill_boot_fb` over SSH puts the default back.
