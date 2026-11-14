# RX 7600 firmware runtime evidence — 2026-11-14

**Capture time:** `2026-11-14T03:13:04+07:00`
**Host kernel:** `Linux cachyos-x8664 7.3.1-1-cachyos #1 SMP PREEMPT_DYNAMIC Sat, 31 Oct 2026 06:43:52 +0000 x86_64 GNU/Linux`
**GPU:** AMD Radeon RX 7600, PCI `1002:7480`, subsystem `148c:2421`, revision `0xcf`, BDF `0000:03:00.0`
**Scope:** read-only inspection of the running boot journal, module metadata, kernel config and firmware filesystem. No firmware was manually loaded or changed; no GPU register writes or queue submissions were performed.

## Running module metadata

- Module file: `/lib/modules/7.3.1-1-cachyos/kernel/drivers/gpu/drm/amd/amdgpu/amdgpu.ko.zst`
- Module `srcversion`: `C407ADC0B867F2BF3B6977C`
- Module vermagic: `7.3.1-1-cachyos SMP preempt mod_unload`
- Kernel config: `CONFIG_EXTRA_FIRMWARE=""`

The module metadata declares these relevant firmware names:

```text
amdgpu/dcn_3_2_1_dmcub.bin
amdgpu/gc_11_0_2_imu.bin
amdgpu/gc_11_0_2_me.bin
amdgpu/gc_11_0_2_mec.bin
amdgpu/gc_11_0_2_mes1.bin
amdgpu/gc_11_0_2_mes_2.bin
amdgpu/gc_11_0_2_mes.bin
amdgpu/gc_11_0_2_pfp.bin
amdgpu/gc_11_0_2_rlc.bin
amdgpu/psp_13_0_7_sos.bin
amdgpu/psp_13_0_7_ta.bin
amdgpu/vcn_4_0_4.bin
```

This is a declaration list, not evidence that each image was requested or loaded.

## PSP 13.0.7 filesystem check

A read-only search of both `/usr/lib/firmware` and `/lib/firmware` found:

```text
/lib/firmware/amdgpu/psp_13_0_7_sos.bin.zst
/usr/lib/firmware/amdgpu/psp_13_0_7_sos.bin.zst
```

No `psp_13_0_7_ta` file was present in those searched filesystem paths at capture time. The kernel config reports `CONFIG_EXTRA_FIRMWARE=""`; this does not establish what may have been present in the boot initramfs, which was not inspected as part of this capture.

## Current-boot runtime messages

```text
[    6.049388] cachyos kernel: amdgpu 0000:03:00.0: initializing kernel modesetting (IP DISCOVERY 0x1002:0x7480 0x148C:0x2421 0xCF).
[    6.049930] cachyos kernel: amdgpu 0000:03:00.0: detected ip block number 0 <common_v1_0_0> (soc21_common)
[    6.050052] cachyos kernel: amdgpu 0000:03:00.0: detected ip block number 1 <gmc_v11_0_0> (gmc_v11_0)
[    6.050168] cachyos kernel: amdgpu 0000:03:00.0: detected ip block number 2 <ih_v6_0_0> (ih_v6_0)
[    6.050286] cachyos kernel: amdgpu 0000:03:00.0: detected ip block number 3 <psp_v13_0_0> (psp)
[    6.050401] cachyos kernel: amdgpu 0000:03:00.0: detected ip block number 4 <smu_v13_0_0> (smu)
[    6.050517] cachyos kernel: amdgpu 0000:03:00.0: detected ip block number 5 <dce_v1_0_0> (dm)
[    6.050634] cachyos kernel: amdgpu 0000:03:00.0: detected ip block number 6 <gfx_v11_0_0> (gfx_v11_0)
[    6.050749] cachyos kernel: amdgpu 0000:03:00.0: detected ip block number 7 <sdma_v6_0_0> (sdma_v6_0)
[    6.050864] cachyos kernel: amdgpu 0000:03:00.0: detected ip block number 8 <vcn_v4_0_0> (vcn_v4_0)
[    6.050992] cachyos kernel: amdgpu 0000:03:00.0: detected ip block number 9 <jpeg_v4_0_0> (jpeg_v4_0)
[    6.051135] cachyos kernel: amdgpu 0000:03:00.0: detected ip block number 10 <mes_v11_0_0> (mes_v11_0)
[    6.070378] cachyos kernel: amdgpu 0000:03:00.0: [drm] Loading DMUB firmware via PSP: version=0x07003600
[    6.070495] cachyos kernel: amdgpu 0000:03:00.0: [VCN instance 0] Found VCN firmware Version ENC: 1.24 DEC: 9 VEP: 0 Revision: 55
[    6.127301] cachyos kernel: amdgpu 0000:03:00.0: reserve 0x1300000 from 0x81fc000000 for PSP TMR
[    6.223017] cachyos kernel: amdgpu 0000:03:00.0: RAS: optional ras ta ucode is not available
[    6.231217] cachyos kernel: amdgpu 0000:03:00.0: RAP: optional rap ta ucode is not available
[    6.231444] cachyos kernel: amdgpu 0000:03:00.0: SECUREDISPLAY: optional securedisplay ta ucode is not available
[    6.282042] cachyos kernel: amdgpu 0000:03:00.0: SMU is initialized successfully!
[    6.282265] cachyos kernel: amdgpu 0000:03:00.0: [drm] Display Core v3.2.392 initialized on DCN 3.2.1
[    6.284097] cachyos kernel: amdgpu 0000:03:00.0: [drm] DMUB hardware initialized: version=0x07003600
[    6.455241] cachyos kernel: amdgpu: Topology: Add GPU node [0x1002:0x7480]
[    6.456495] cachyos kernel: amdgpu 0000:03:00.0: ring sdma0 uses VM inv eng 12 on hub 0
[    6.456609] cachyos kernel: amdgpu 0000:03:00.0: ring sdma1 uses VM inv eng 13 on hub 0
[    6.456726] cachyos kernel: amdgpu 0000:03:00.0: ring vcn_unified_0 uses VM inv eng 0 on hub 8
[    6.456834] cachyos kernel: amdgpu 0000:03:00.0: ring jpeg_dec uses VM inv eng 1 on hub 8
```

The running boot initialized the RX 7600 with the PSP IP block, loaded DMUB via PSP, initialized SMU, reported VCN firmware metadata, and enumerated SDMA / unified VCN / JPEG rings. The log also reports that optional RAS, RAP and SecureDisplay TA microcode is unavailable.

These messages do not name the base PSP TA blob. They therefore do not resolve the source-derived request-path discrepancy recorded in [firmware_inventory.md](firmware_inventory.md): the matching CachyOS source selects `psp_13_0_7_ta.bin` as a required request for MP0 13.0.7 on this non-kicker device, while the searched root filesystem contains only the SOS blob. The initramfs was not inspected, and the running module's `srcversion` is not a build attestation tying it to the source tag. Do not treat this capture as proof that the TA blob is absent from every boot source or that the profile's PSP firmware list is complete.

## Limits

This capture is Linux runtime evidence only. It does not prove macOS compatibility, RedNavi firmware loading, GPU command submission by RedNavi, Metal acceleration, or WindowServer acceleration.
