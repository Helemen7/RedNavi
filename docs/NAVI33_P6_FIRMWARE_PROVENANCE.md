# Navi33 P6 firmware provenance — verified 2026-11-08

Branch: `feature/navi33-p6-hw-bringup`.

## Independently verified package source

A WSL Ubuntu 24.04 developer compared the **uncompressed** firmware
bytes against the P1 SHA-256/length manifest in
`RedNavi/navi33_psp_plan.h`, using
`tools/navi33_p6_firmware_verify.py`.

- Package: `linux-firmware-amd-graphics`
- **Matching** package version: `20260915.git1522c78a-0ubuntu1`
- Downloaded Debian archive:
  `https://archive.ubuntu.com/ubuntu/pool/main/l/linux-firmware-amd-graphics/linux-firmware-amd-graphics_20260915.git1522c78a-0ubuntu1_all.deb`
- Extracted location:
  `usr/lib/firmware/amdgpu/`
- Verification: **9 of 9 passed** for file length and SHA-256 of the
  uncompressed firmware content (host-side result).
- Previous package: `20240318.git3b128b60-0ubuntu3.4`
  (**9 of 9 differed** in size and/or SHA-256).

The nine files covered by the P1 manifest are:

| Component | Firmware name | Uncompressed bytes |
|---|---|---:|
| GC PFP | `gc_11_0_2_pfp` | 234944 |
| GC ME | `gc_11_0_2_me` | 324080 |
| GC MEC | `gc_11_0_2_mec` | 406528 |
| GC RLC | `gc_11_0_2_rlc` | 178848 |
| GC IMU | `gc_11_0_2_imu` | 132352 |
| SDMA | `sdma_6_0_2` | 34560 |
| MES scheduler | `gc_11_0_2_mes_2` | 263696 |
| MES KIQ | `gc_11_0_2_mes1` | 221408 |
| SMU | `smu_13_0_7` | 287080 |

**These are GC/SDMA/MES/SMU images, not the PSP secure-OS
firmware set.** The existing nine-file audit does **not** verify
`psp_13_0_7_*` signed SOS/TA or any PSP bootloader component. Before
implementing sOS load, separately identify and verify all required
PSP firmware images and firmware mode on this specific card.

A matching SHA-256 demonstrates equality with P1 reference bytes, not
that PSP ring creation, signed firmware compatibility, GPU memory
ownership or physical queue mapping works.

## Reproduce without modifying installed WSL firmware

```sh
mkdir -p ~/fw-reference/ubuntu-20260915
curl -fL --retry 3 -o ~/fw-reference/amd-20260915.deb \
  https://archive.ubuntu.com/ubuntu/pool/main/l/linux-firmware-amd-graphics/linux-firmware-amd-graphics_20260915.git1522c78a-0ubuntu1_all.deb
dpkg-deb -x ~/fw-reference/amd-20260915.deb \
  ~/fw-reference/ubuntu-20260915
python3 tools/navi33_p6_firmware_verify.py \
  --firmware-dir ~/fw-reference/ubuntu-20260915/usr/lib/firmware/amdgpu
```

Do not copy these blobs into the RedNavi repository, disable digest
checks, or install them over the host's firmware as a development
shortcut. This file records the developer's reported audit outcome;
no package archive SHA-256 is recorded because it was not supplied.
