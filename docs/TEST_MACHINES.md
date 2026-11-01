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

macOS (Sequoia 15.8.2) is booted on this machine through OpenCore. For most experiments the UHD 630 drives the screen, so the 7800 XT is free and panics can be read. Once the framebuffer works, `-wegnoigpu` (WhateverGreen) switches the iGPU off and the 7800 XT is the only display; keep SSH (Remote Login) enabled and a USB copy of the EFI without the kexts for that case. The machine boots macOS first and auto-mounts the EFI, so kext updates and logs can be handled remotely. Boot-args: `-v debug=0x100 keepsyms=1` plus the `rednavi_*` ones (no `-wegnoegpu`); OpenCore file logging is on. With `-wegnoigpu` the 7800 XT is the only display: if the monitor is off at boot the machine does not come up on the network, so keep it on during tests. The internal EFI auto-mounts at `/Volumes/EFI` when booted from the internal disk, but its disk number moves when USB sticks are plugged in (`disk0s1` with none, `disk3s1` with the Linux and emergency sticks in). Before writing to it, identify it by what it is, not by its number: `diskutil info /Volumes/EFI` must say `Device Location: Internal` and `EFI/OC/config.plist` must carry the `rednavi_*` boot-args. The EFI of the stick is external; the EFI of the other internal disk (Windows) has no `EFI` folder. A kext that fails to link or load freezes the boot on the test screen: boot the stick, fix the internal EFI over SSH, reboot.

## To collect before running anything on macOS

- [x] Above 4G Decoding / Resizable BAR state in the BIOS: Resizable BAR ON. VRAM is 16 GB.
- [ ] VBIOS dump (GPU-Z), optional
- [x] IP discovery: read by the probe from VRAM (no Linux needed), see `docs/ARCHITECTURE.md`
- [ ] ACPI VFCT table presence
