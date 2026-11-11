# CachyOS 7.3.1-1 amdgpu source cross-check — RX 7600 / Navi 33

- Date: 2026-11-11
- Running kernel: `7.3.1-1-cachyos`
- Matching CachyOS release tag: `cachyos-7.3.1-1`
- Tag target commit: `36ae4c219cfbd02576048c7ca1d366cea5db8034`
Release is based on Linux 7.3.1.

## Source provenance

The following files were fetched from the CachyOS source tag, rather than relying only on current upstream Linux:

| Source file | Git blob SHA |
|---|---|
| `drivers/gpu/drm/amd/amdgpu/amdgpu_kms.c` | `7c081b338fe2e50db9c96406796d7e05842152f7` |
| `drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c` | `bb672a923463005b5f46b7a3dc88c9ba7eb7f4bd` |
| `include/uapi/drm/amdgpu_drm.h` | `42a5fa8ad6b0abc7691a2e4bdd45c1f365142621` |
| `drivers/gpu/drm/amd/amdgpu/vcn_v4_0.c` | `c2ddf3cb368fde8141331682c0556ceac1f2a573` |

The installed local UAPI header at `/usr/lib/modules/7.3.1-1-cachyos/build/include/uapi/drm/amdgpu_drm.h` was reconstructed from two read-only file reads and compared byte-for-byte with the tag's UAPI file. They match exactly (53,006 bytes). The pre-existing local-header SHA-256 remains:

`ac7f6dae44b224f34591152123fbf2047d3486c35d39f3eb818a96ea47a6462a`

The installed header package does not contain `amdgpu_kms.c`. The release tag and exact UAPI match strongly anchor the source comparison to this kernel release, but this is not a binary build-provenance attestation of the loaded kernel module.

## Confirmed API semantics

### ACCEL_WORKING

In `amdgpu_info_ioctl()`, `AMDGPU_INFO_ACCEL_WORKING` copies `adev->accel_working` to userspace. It is the kernel driver's acceleration-status field. It is not a record that a GPU command buffer was submitted or completed.

### HW_IP_INFO versus HW_IP_COUNT

`amdgpu_hw_ip_info()` rejects an instance argument greater than or equal to `AMDGPU_HW_IP_INSTANCE_MAX_COUNT`. The matching UAPI defines that constant as `1`, so only argument `0` is accepted by this interface.

`AMDGPU_INFO_HW_IP_COUNT` is a separate query. For the ordinary non-XCP path, the SDMA count comes from `adev->sdma.num_instances`, while VCN count comes from `adev->vcn.num_vcn_inst`. A count of two SDMA instances does not make `HW_IP_INFO` instance argument 1 valid.

The HW_IP_INFO result structure is zero-initialized before the helper is called. If the requested IP block is not found in a valid state, the helper returns without filling the structure. This matches why zero-filled replies must not be treated as positive version evidence.

### Meaning of available_rings

For each engine type, the implementation counts rings whose scheduler is marked `ready` and which are not marked `no_user_submission`. It caps that count to the context entity limit, then constructs the mask as:

```c
result->available_rings = (1 << num_rings) - 1;
```

Therefore this field is a count-derived availability mask, not a raw physical-ring register or proof of a hardware ring's physical index. A mask of `0x1` means one user-submission-capable context ring was reported by this API at capture time; `0x0` means none were reported.

## RX 7600 VCN finding

The saved RX 7600 DRM_AMDGPU_INFO capture has:

| Query | Captured result | Source-based reading |
|---|---|---|
| VCN HW_IP_COUNT | 1 | One VCN instance is counted; this is not a ring count |
| VCN_DEC HW_IP_INFO | HW version 4.0, IP Discovery 4.0.4, mask 0 | The query counts the dedicated `ring_dec` structure, which this VCN 4.0 unified-ring path does not use as its unified queue |
| VCN_ENC HW_IP_INFO | HW version 4.0, IP Discovery 4.0.4, mask 0x1 | The query counts `ring_enc[0]`, which the VCN 4.0 source explicitly reuses as the unified ring; one submission-capable unified queue is reported |
| VCN_JPEG HW_IP_INFO | HW version 4.0, IP Discovery 4.0.4, mask 0x1 | One JPEG submission ring was reported available |

This source-level detail changes the interpretation of the decoder mask: it is not evidence that RX 7600 lacks decode capability or that a dedicated decoder ring failed to initialize. VCN 4.0 explicitly reuses the encoder ring as a unified ring, sets the firmware-shared `UNIFIED_QUEUE` flag, and enables the unified queue structure. The generic `HW_IP_INFO` implementation still counts `ring_dec` for the VCN_DEC query and `ring_enc` for VCN_ENC. That explains the captured mask pattern. It establishes the Linux driver's unified-queue design path, not a completed decode workload or macOS/Metal functionality.

### Which VCN implementation family is selected?

In the matching `amdgpu_discovery_set_mm_ip_blocks()`, UVD/VCN IP version `4.0.4` selects:

- `vcn_v4_0_ip_block`
- `jpeg_v4_0_ip_block`

It does not select `vcn_v4_0_3_ip_block` or `vcn_v4_0_5_ip_block` in this source revision. This matters when choosing which Linux implementation to study as a reference for Navi 33: start with VCN/JPEG 4.0 generic paths, then verify any ASIC-specific overrides separately.

## Firmware metadata boundary

The `AMDGPU_INFO_FW_VERSION` helper returns firmware version/feature values held in the driver's `adev` structures (for example, VCN, SDMA, PSP SOS/ASD, SMU, DMCUB and MES fields). These metadata values are useful for identifying the driver's recorded firmware versions, but do not independently prove that every firmware image successfully booted and remains operational.

## References

- CachyOS release tag: https://github.com/CachyOS/linux/tree/cachyos-7.3.1-1
- `amdgpu_kms.c`: https://github.com/CachyOS/linux/blob/cachyos-7.3.1-1/drivers/gpu/drm/amd/amdgpu/amdgpu_kms.c
- `amdgpu_discovery.c`: https://github.com/CachyOS/linux/blob/cachyos-7.3.1-1/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c
- `amdgpu_drm.h`: https://github.com/CachyOS/linux/blob/cachyos-7.3.1-1/include/uapi/drm/amdgpu_drm.h
- `vcn_v4_0.c`: https://github.com/CachyOS/linux/blob/cachyos-7.3.1-1/drivers/gpu/drm/amd/amdgpu/vcn_v4_0.c

## Next investigation

1. Trace VCN 4.0 generic initialization and ring setup in the matching release source, focusing on why the captured decoder ring did not appear available.
2. Compare the VCN 4.0 ring/scheduler setup with the saved IP Discovery and firmware metadata without executing hardware programming or queue submissions.
3. Keep macOS Probe, register access, firmware activation and Metal bring-up as separate future milestones. Linux API readiness is not proof of macOS acceleration.

## Scope

This is a source-code and saved-evidence cross-check. It did not access or modify GPU registers, load/write firmware, submit GPU work, reset/reconfigure the GPU, load a macOS kext, or edit EFI/macOS files.
