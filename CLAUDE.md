# RedNavi: notes for coding agents and contributors

Native macOS display driver for the AMD Radeon RX 7800 XT (Navi 32, RDNA 3, PCI `1002:747E`). Read `README.md` for the state, then `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`, `docs/TEST_MACHINES.md` and the bring-up notes in `docs/`.
Edit: Not RX 7800XT but Ryzen AI Max+ 395 APU w/ Radeon 8060s Graphics iGPU (RDNA 3.5)
ROG Flow Z13 2025 GZ302EA-DRU161W
- Ryzen AI Max+ 395 w/ Radeon 8060s Graphics
- 128Gb LPDDX5X UMA 8000MHz 256bit-bus 256GB/s

## Layout

- `RedNavi/`: the probe kext (`kern_probe.cpp`, matches the GPU; every stage that writes to the card is opt-in through a `rednavi_*` boot-arg), `smu13.cpp`, the generated `dcn_reglist.h` and `dcn_replay.h`.
- `RedNaviFB/`: the `IOFramebuffer` kext. It is separate because IOGraphicsFamily is in the system kernel collection, which OpenCore cannot link against; see `docs/DP_BRINGUP.md` for installing it.
- `mesa-bridge/`: Mesa's `ac_drm_*` layer over the kext's user client, the RADV patch set and the Vulkan test programs (`mesa-bridge/README.md`, `docs/M5_MESA_BUILD.md`). `RedNavi/rn_uapi.h` is the kext's user interface and must stay in step with the bridge's copy.
- `compat/`: LinuxKPI-style compatibility layer (`docs/COMPAT.md`). `tests/host/`: host tests, run with `make test`.
- `tools/`: Python helpers (IP discovery decoder, VBIOS and EDID parsers, firmware header generators, Linux register-dump scripts).
- `docs/logs/`: raw dumps from real runs, with vendor firmware and monitor serial numbers left out.

## Build

```
sh scripts/bootstrap.sh     # fetches MacKernelSDK
make DEBUG=1                # or: make
make test                   # host tests for the compat layer
```

`make` builds `RedNavi.kext` and `RedNaviFB.kext` under `build/<Config>/`. Firmware headers (`RedNavi/psp_*.h`, `RedNavi/dmub_*.h`) are generated locally from linux-firmware with `tools/gen_*_fw_header.py` and are git-ignored; without them the corresponding stages are compiled out.

## Conventions

- Code, comments, docs and commit messages are in English, in plain prose.
- Licence is BSD-3-Clause. Do not copy code from projects with other or restrictive licences (NullMoth's kexts and plugin are PolyForm Noncommercial; also do not copy from NootedRed or LegacyRed). Write from scratch; FreeBSD's drm-kmod and the Linux amdgpu driver are references for ideas and register definitions.
- Do not commit vendor firmware (linux-firmware's LICENSE.amdgpu allows binary redistribution only and forbids reverse engineering and disassembly; do not disassemble it either), headers generated from it, the VBIOS image or raw EDIDs (they carry serial numbers). The probe publishes them as ioreg properties; cut them out of logs before committing.
- Say what is verified on hardware and what is not. Mark anything read from the Linux sources but never run as unverified.

## Hard-won facts

- The GC memory hub registers (`GCMC_VM_*`, `GCVM_*`) have different offsets in GC 11.0.3 (`gfxhub_v3_0_3`) than in 11.0.0; `RedNavi/gc_regs.h` is generated from the 11.0.3 headers. Check which `gc_11_0_x` header a Linux file includes before copying register numbers.
- GC and SDMA registers read 0 while the GC is in GFXOFF (also on Linux, unless `amdgpu_gfxoff` is written 0). A read of 0 from a block that must have a nonzero ID register means "powered down" or "wrong offset", not "works".
- The compat layer's `gpu.reg_offset` bases are only valid after the IP discovery table is parsed; set them just before use.

- OpenCore refuses to inject a kext whose `OSBundleLibraries` is incomplete or names a library outside the boot kernel collection. The only trace is `Prelinked injection ... - Invalid Parameter` in the OpenCore log.
- `IOLog` output may not show up in `log show`; read the properties on the `RedNaviProbe` node (`ioreg -rn RedNaviProbe -l -x`).
- Reading all registers of the DCN list (`tools/dcn32_reglist.txt`) hangs the machine (clock- or power-gated blocks). Read only registers of live blocks.
- `IODisplay::readFramebufferEDID` calls `getAttributeForConnection` with a NULL value pointer.
- The verbose boot console lags the kernel: the last visible line can be stale.
- A kext with an unresolved symbol of its own (a global whose definition was deleted, only an `extern` left) builds and links, and then fails when it is linked into the kernel collection at boot: frozen test screen, no SSH, recovery needs the emergency stick. The Makefile fails the build on any unresolved `_g[A-Z]` symbol; run `nm -u` on the binary after removing code. The kernel stack is 16 KB and cannot be widened: big local structs in `sdmaTest` are `static`.
- GART pages of system memory need the Linux PTE: `VALID | SYSTEM | SNOOPED | EXECUTABLE | READABLE | WRITEABLE | MTYPE_UC << 48` (`MTYPE_UC` is 3, `navi10_enum.h`). Without the MTYPE the MES scheduler read its ring from host memory but never wrote its answers back.
- The MES log area is 0x4000 bytes of log plus 0x40000 of scratch (0x44000; read it off the size of `debugfs amdgpu_mes_event_log`). A smaller area is overrun by the firmware, in host memory with a GCVM fault one page past the buffer, in VRAM over whatever follows (it was the GART page table).
- The MES KIQ ring is filled with the NOP `0xFFFF1000` and each commit is padded to 256 dwords with it (`gfx_v11_0_ring_funcs_kiq`); a zero dword in the PM4 stream parks the KIQ pipe at 0x51ad and the scheduler's `MAP_QUEUES` is dropped. The healthy idle value is 0x518c (Linux, 3000 of 3000 samples).
- A register that Linux reads without consequence can matter here: the MES firmware uses `GRBM_GFX_CNTL` itself, so poll loops that select ME/pipe while it runs are a suspect. Debug a hang by reading properties on a boot you can reach, not by looping reboots; and a result that depends on the boot is a layout problem until proven otherwise.

## Safety

The probe writes to a real GPU from the kernel and can hang the machine. Test on a system whose display does not depend on this card, keep SSH enabled, and keep a USB copy of the OpenCore EFI without the kexts.


## Windows binaries for gfx1151/ROG Flow Z13
- ROCm GFX1151 sources "C:\Users\Naessah\AppData\Local\Bubo\runtime\rocm-10.0.0-gfx1151"
- G-Helper sources (Asus Management for ROG Flow Z13) : "D:\GIT\g-helper"
- Reverse Engineering Anything "D:\GIT\RedNavi\skills\rea"
- Ghidra "D:\AI_Stack\Tools\Ghidra"