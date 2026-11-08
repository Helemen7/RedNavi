# RX7600 / Navi33 native Linux reference — 2026-11-08

Data: Sapphire `1002:7480` rev `CF`, subsystem `1DA2:E452`, Ubuntu
kernel `7.0.0-30-generic`, `amdgpu 3.64.0`, capture at 13:34:55 UTC.
Collected from the real device via read-only sysfs and debugfs snapshots.
This is **NOT** a macOS RedNavi physical bring-up result.

## Hardware information confirmed on the device

- IP Discovery binary header **v1.3**, 1280 bytes declared, checksum
  `0x5784` verified; IP table **v3**, 39 IP instances on one die.
- **GC 11.0.2:** four bases
  `0x1260,0xA000,0x1C000,0x2402C00`.
- **MMHUB 3.0.2:** **two** bases `0x1A000,0x2408800`.
- **MP0/MP1 13.0.7:** five bases each,
  `0x16000,0xDC0000,0xE00000,0xE40000,0x243FC00`.
- **SDMA0 6.0.2:** four bases
  `0x1260,0xA000,0x1C000,0x2402C00`.
- **OSSSYS 6.0.2:** two bases `0x10A0,0x240A000`.
- **NBIF 4.3.1:** six bases
  `0,0x14,0xD20,0x10400,0x241B000,0x4040000`.
- **DCN 3.2.1:** five bases, starting `0x12,0xC0`.
- All base offsets above are **DWORD register offsets**, NOT CPU physical
  addresses and not a promise that a base lies inside the 1 MiB BAR5.

## Memory

- PCI BAR0: **8192 MiB** of CPU-mapped VRAM via ReBAR
  (`0x7C00000000..0x7DFFFFFFFF`).
- PCI BAR2: 256 MiB (`0x7E00000000..0x7E0FFFFFFF`).
- PCI BAR5: **1 MiB** MMIO (`0xFC900000..0xFC9FFFFF`).
- Linux-reported **usable** VRAM: **8176 MiB**, GPU MC range
  `0x8000000000..0x81FEFFFFFF`.
- GART: 512 MiB at `0x7FFF00000000..0x7FFF1FFFFFFF`;
  page table GPU MC address `0x8000300000`.
- PSP TMR: `0x1300000` bytes (19 MiB) reserved from
  `0x81FC000000`.

These are Linux's live allocations. Do **not** hard-code the GART/PSP TMR
positions as universal Navi33 constants. The physical `RN_VRAM_MC_BASE`
constant `0x8000000000` happens to match this device, but the macOS
driver should derive the value from its GPU registers.

## Firmware evidence

PSP SOS `0x00330024`, SMU `0x00525F00` (82.95.0), MES scheduler
`0x86`, MES KIQ `0x104`, PFP `0xA50`, ME `0x9A6`, MEC `0xA78`,
RLC `0x61`, IMU `0x0B214700`, SDMA `0x18`, DMCUB `0x07002F00`.
A Linux SMU firmware interface version warning does not prevent Linux
initialization. No firmware file bytes were copied into this repository;
the archive listed only compressed .zst file names and sizes, so the
existing P1 SHA-256 manifest is not validated by this capture.

## Queue evidence

- Linux `amdgpu_fence_info`: graphics ring `gfx_0.0.0` has emitted =
  signaled `0x521B`; MES scheduler `mes_3.0.0` = `0x61`.
- Eight compute rings have matched emitted/signaled fences.
- Eight compute MQDs and one graphics MQD were present, each **2048 bytes**.
- `struct v11_gfx_mqd`, DWORD offset 130 `cp_gfx_hqd_active` = **0**,
  offset 156 `cp_gfx_hqd_mapped` = **0** in the **saved MQD**, despite
  the completed graphics fences. These values are not a selected live
  hardware register snapshot, and cannot by themselves prove mapping failed.
- Compute MQDs have header `0xC0310800`, `cp_hqd_active=1`,
  `cp_hqd_error=0`, and populated ring, EOP and doorbell configuration.
- Graphics MQD GPU address example: `0x81FE91B000` (VRAM).
  Graphics ring GPU address example: `0x7FFF0068D000` (GART).
- The MES/KIQ MQD files were zero-length; do not infer their ring state
  from them.

## Direct P5 changes prompted by this capture

- Fixed the incorrect **MMHUB >= 5 segments** gate: this card has only 2.
- Copy only the actual MMHUB segment count into the GPU register-base
  array.
- Accept a 16 MiB usable-FB tail reservation even when BAR0 spans all
  **8192 MiB** of the physical VRAM. Restrict the CPU-visible usable
  extent to `min(BAR0, FB range)`.
- Added host regression with actual observed IP base values and the
  8176 MiB/8192 MiB memory case.

These are code/host-test fixes, not proof of successful macOS PSP/GMC/MES
initialization.

## Upcoming physical bring-up gates

1. Native macOS P5 probe should report valid IP-base and FB preflight.
2. PSP13.0.7 sOS / command-ring acknowledgements must be measured on
   Navi33, not inferred from Navi32.
3. Bring up GMC11 GPU VM and correctly allocated Ring/MQD/EOP addresses.
4. Distinguish MES `ADD_QUEUE` scheduler API from KIQ
   `PACKET3(MAP_QUEUES)`, and validate replies and ring fences.
5. Demonstrate GPU-written marker plus completed fence before claiming
   RX7600 hardware compute or any Metal acceleration.

Upstream references:
- https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/include/v11_structs.h
- https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/mes_v11_0.c
- https://docs.kernel.org/gpu/amdgpu/debugfs.html

No raw MQD snapshots or firmware were added to this repository.
