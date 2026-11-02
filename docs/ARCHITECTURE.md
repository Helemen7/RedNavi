# Architecture

## Target stack

```
Metal app -> Metal plugin -> translator (AIR -> SPIR-V) -> RADV (Mesa Vulkan + ACO) -> `ac_drm_*` backend (Mesa's replacement for libdrm_amdgpu, see M5_PLAN.md) -> user client -> kext (amdgpu: DC, PSP, SMU, GFX11, MES, GPUVM)
```

## What we learned from NullMoth (NVIDIA)

Read from their repository (`docs/HOW-IT-WORKS.md`, `plugin/`, `nvk/nvk-macos.patch`, `kexts/`):

- The Metal plugin does **not** use the standard Vulkan API. It calls a private `nvmtl_vk_*` layer that uses NVK internal types (`nvk_buffer`, `nvk_image`, `nvk_cmdbuf`, `nvk_pipeline`) and exposes Metal-specific features (bindless, tessellation, ray tracing). So for AMD the plugin has to be rewritten on top of RADV; it cannot be swapped in.
- The shader translator (`translator/`, LGPL-3.0, based on metal2vulkan) is GPU independent and can be reused as a component.
- NVK reaches the kernel through its `nvkmd` abstraction; their patch disables the nouveau/libdrm path and adds an `nvrm` backend that talks to the kext over IOKit. RADV's equivalent seam is the `amdgpu` winsys (`src/amd/vulkan/winsys/amdgpu`), which sits on libdrm_amdgpu. To be confirmed by reading the real call surface.
- Kernel side: the vendor driver runs inside a kext through an OS layer (`os-xnu*.cpp`). Separate kexts provide the framebuffer (`IOFramebuffer`), the accelerator (`IOAccelerator`, shared surfaces for WindowServer) and the AGDC policy. They load from `/Library/Extensions` via the Auxiliary Kernel Collection, not from OpenCore.

## Hardware facts

Confirmed: RX 7800 XT is Navi 32, gfx1101, PCI `1002:747E`. Linux handles it through the generic IP-discovery path, not a dedicated ID entry.

Where the MES keeps its memory (2026-11-01, verified on the 7800 XT; details in `docs/COMPAT.md`): the scheduler ring (32 KB), the KIQ ring (64 KB), the compute ring (8 KB), the write-back words, fences, scheduler context, event log with its scratch (0x44000 bytes), MQDs and EOP buffers sit in one 0xB0000-byte write-combined host buffer (it also holds the graphics queue's ring and MQD) behind the GART, as in Linux; the PTEs of that window are `VALID | SYSTEM | SNOOPED | EXECUTABLE | READABLE | WRITEABLE | MTYPE_UC << 48`. `rednavi_mes_host=0` puts the same area in VRAM at 0x0C400000. The MES takes its register bases (GC, MMHUB, OSSSYS) from `SET_HW_RESOURCES`, filled from the IP discovery table.

IP block families (from `amdgpu_discovery.c`): GC 11.0.x, MES 11, SDMA 6.x, PSP 13.0.x, SMU 13.0.x, DCN 3.2.x, VCN 4.0.x.

**Measured on the card (M1 probe, 2026-10-27, Sapphire RX 7800 XT, macOS 15.8.2).** The IP discovery table is binary version 1.2, 1556 bytes, checksum OK, read from VRAM at `VRAM size - 64 KB`. Raw dump: `docs/logs/ioreg-rednavi-747e.txt`; decode it with `tools/decode_discovery.py`.

| Block | Version |
|---|---|
| GC | 11.0.3 |
| SDMA0, SDMA1 | 6.0.3 |
| MP0 (PSP), MP1 (SMU) | 13.0.10 |
| DMU (DCN) | 3.2.0 |
| NBIF | 4.3.0 |
| OSSSYS (IH) | 6.0.0 |
| MMHUB | 3.0.0 |
| HDP | 6.0.0 |
| VCN | 4.0.0, 4.0.1 |
| SMUIO | 13.0.6 |
| PCIE | 7.4.0 |

So GC is 11.0.3, not 11.0.1.

## Firmware for this card

File names come from the `MODULE_FIRMWARE` lines in the Linux driver and were checked against `WHENCE` in linux-firmware. All of them exist there with the licence "Redistributable. See LICENSE.amdgpu". The licence text (read 2026-10-28) allows redistribution **in binary form only**, with AMD's copyright and permission notice reproduced in the documentation, and forbids reverse engineering, decompilation and disassembly; it also refers to US export laws. So the repo does not ship or mirror the files (nor C arrays generated from them): users fetch them from linux-firmware and generate local headers. See [NOTICE](../NOTICE).

| Block | File in `amdgpu/` |
|---|---|
| PSP | `psp_13_0_10_sos.bin`, `psp_13_0_10_ta.bin` |
| SMU | `smu_13_0_10.bin` |
| Display (DMCUB, DCN 3.2.0) | `dcn_3_2_0_dmcub.bin` |
| GC | `gc_11_0_3_pfp.bin`, `_me.bin`, `_mec.bin`, `_rlc.bin`, `_imu.bin` |
| MES | `gc_11_0_3_mes_2.bin`, `gc_11_0_3_mes1.bin` (`gc_11_0_3_mes.bin` is also declared) |
| SDMA | `sdma_6_0_3.bin` |
| VCN | `vcn_4_0_0.bin` |

For M3 (native framebuffer) only the PSP and DMCUB files matter, plus the SMU file if the clocks must be brought up first. The DMCUB name `dcn_3_2_0_dmcub.bin` is confirmed in `amdgpu_dm_dmub.c` (`FIRMWARE_DCN_V3_2_0_DMCUB`).

## Probe findings that shape M2

- The register window works: `MM_INDEX`/`MM_DATA` reads of VRAM return valid data, and `RCC_CONFIG_MEMSIZE` gave a size that placed the table correctly. M2 can read VRAM the same way before any aperture mapping exists.
- The IP table lists base addresses per block (for example GC `0x1260`, `0xa000`, `0x1c000`, `0x2402c00`; DMU `0x12`, `0xc0`, `0x34c0`, `0x9000`, `0x2403c00`). The compatibility layer can build its register-offset tables from these instead of hard-coding them.
- DF shows 5 instances and several blocks (CLKA, FUSE, THM, UMC) show instances 1 to 4 with other versions. These are most likely the four memory-cache dies (MCDs) of Navi 32 next to the graphics die; this is inferred from the four matching base-address groups, not checked against AMD documentation.

- **PSP and SMU state after the VBIOS (probe stage 2, 2026-10-28, raw dump in `docs/logs/ioreg-rednavi-probe2.txt`).** `MP0_C2PMSG_33` and `MP0_C2PMSG_35` read `0x80000000` (IFWI init done, bootloader ready for commands). `MP0_C2PMSG_81` is 0, so no PSP sOS is running yet: M2 has to load `psp_13_0_10_sos.bin` through the bootloader. `MP1_C2PMSG_90` is 1, so the SMU is alive and answered its last message OK; its message and argument registers are 0. All other PSP ring registers are 0. The ioreg values print as `18446744071562067968` because the probe stores them as sign-extended 32-bit numbers; that is `0x80000000`.

PSP initialises before display and graphics: the DMUB firmware used by the display engine is loaded through it.

## Milestone M3 (native framebuffer) in practice

1. Map BARs, read IP discovery (M1). Done.
2. Compatibility layer sufficient to run the needed amdgpu code (M2). Started (`COMPAT.md`).
3. PSP bring-up, load DMUB firmware (done); SMU bring-up (not done).
4. Display: a replay of a lit Linux pipe for one fixed mode (done, `DP_BRINGUP.md`); a port of DC for DCN 3.2 with modes from the EDID is still open.
5. `IOFramebuffer` subclass publishing the display to WindowServer. Done for one mode (`RedNaviFB`).

## Navi 33 (RX 7600, `1002:7480`) compared with Navi 32

From a community capture on Linux (issue #6; `docs/logs/navi33/ip_discovery.txt`, made with `tools/linux_ip_discovery.py`). 8 GB VRAM, 8 GB BAR0 (Resizable BAR on), 1 MB register BAR (BAR5), four memory channels (UMC x4).

| Block | Navi 32 (7800 XT) | Navi 33 (7600) | Consequence |
|---|---|---|---|
| GC | 11.0.3 | 11.0.2 | `gc_11_0_2_*.bin`; the memory hub is `gfxhub_v3_0` (the `gfxhub_v3_0_3` special case in `gmc_v11_0.c` is for 11.0.3 only), so the `gc_11_0_0` register offsets apply (unverified on hardware) |
| SDMA | 6.0.3 | 6.0.2 | `sdma_6_0_2.bin` |
| MP0 (PSP), MP1 (SMU) | 13.0.10 | 13.0.7 | `psp_13_0_7_*.bin`, `smu_13_0_7.bin`; the boot chain and the message set are not known to match (unverified) |
| DMU (DCN) | 3.2.0 | 3.2.1 | `dcn_3_2_1_dmcub.bin`; the first four DMU bases are the same |
| NBIF | 4.3.0 | 4.3.1 | |
| MMHUB, OSSSYS, HDP | 3.0.0, 6.0.0, 6.0.0 | 3.0.2, 6.0.2, 6.0.1 | |

The register bases are the same as on Navi 32: GC `0x1260 0xa000 0x1c000 0x2402c00`, MP0/MP1 `0x16000`, DMU `0x12 0xc0 0x34c0 0x9000`, NBIF `0x0 0x14 ...`. All 538 registers of the Navi 32 pipe list have the same addresses in the Navi 33 DCN capture, so the display engine registers carry over.

From the kernel log of the same machine (`docs/logs/navi33/amdgpu_dmesg.txt`): Display Core initialises on DCN 3.2.1, Linux loads DMUB firmware 0x07002D00, and reserves the PSP TMR at `0x81fc000000` with size `0x1300000`, which is 48 MB below the end of VRAM, like on the RX 7900 XTX (the RX 7800 XT is not at that distance, so the TMR place is read from each card's own log). The card uses BACO for runtime power management, and with it powered down the display engine registers read `0xffffffff`, so a register dump has to be taken with the screen awake.\n\nWhat does not carry over: the firmware (every file above), and the connector. The reporter's monitor is on HDMI-A-2 (the two DisplayPort connectors read `disconnected`), and `SYMCLKB_CLOCK_ENABLE` is the one enabled (`0x101`, front end 1), so the lit path is DIG1, not DIG4, and it is HDMI. The DisplayPort bring-up in this repository does not cover HDMI (TMDS/HDMI output, DDC over I2C instead of AUX). The Linux dump script has the groups `hpd_aux1 dig1_hdmi` for that path.

## Navi 31 (RX 7900 XTX, `1002:744C`) compared with Navi 32

From a community capture on Linux (issue #1; `docs/logs/navi31/`). 24 GB of VRAM (`0x8000000000` to `0x85feffffff`), 32 GB BAR0, six memory channels (UMC x6).

| Block | Navi 32 (7800 XT) | Navi 31 (7900 XTX) | Consequence |
|---|---|---|---|
| GC | 11.0.3 | 11.0.0 | `gc_11_0_0_*.bin`; the memory hub is `gfxhub_v3_0`, so the `gc_11_0_0` register offsets apply (what `gc_regs.h` had before it was moved to 11.0.3) |
| SDMA | 6.0.3 | 6.0.0 | `sdma_6_0_0.bin` |
| MP0 (PSP), MP1 (SMU) | 13.0.10 | 13.0.0 | `psp_13_0_0_*.bin`, `smu_13_0_0.bin`; Linux reports SMU firmware 78.135.0 |
| DMU (DCN) | 3.2.0 | 3.2.0 | the same, and Linux loads the same DMCUB file (`dcn_3_2_0_dmcub.bin`, version 0x07003300 there) |
| NBIF, OSSSYS, MMHUB, HDP | 4.3.0, 6.0.0, 3.0.0, 6.0.0 | 4.3.0, 6.0.0, 3.0.0, 6.0.0 | the same |

The register bases of GC, MP0/MP1, DMU and NBIF are the same as on Navi 32.

The reporter's monitor is on DisplayPort and the lit path is the same as on the development board: `SYMCLKE_CLOCK_ENABLE` 0x401, `DIG4_DIG_FE_CNTL` 0x01000100, `DIG4_DIG_BE_CNTL` 0x40001000, `HPD4` connected, `MPC_OUT0_MUX` 0x100. So this is the card where our DisplayPort bring-up has the best chance of carrying over, with the PSP and SMU firmware being the open part. The capture is of a 3840x2160 mode (`OTG0_OTG_H_TOTAL` 0xf9f, `V_TOTAL` 0x8ad, about 533 MHz, so 60 Hz) with two pipes combined (`ODM0_OPTC_WIDTH_CONTROL` 0x03c00780): our replay and mode code is single pipe, so a 4K mode is a separate piece of work; 1080p on this card would be the first target.

PSP TMR: Linux reserves `0x1300000` bytes at MC `0x85fc000000` (VRAM offset `0x5fc000000`), a different size and place than on the 7800 XT (`0xa700000` at offset `0x3e0000000`), so the TMR placement has to come from each card's own `dmesg` line (`reserve ... for PSP TMR`), not from a formula.
