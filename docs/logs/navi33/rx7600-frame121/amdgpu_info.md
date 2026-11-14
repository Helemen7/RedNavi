# RX 7600 (Navi 33) DRM_AMDGPU_INFO evidence — Frame121

This is a read-only Linux DRM_AMDGPU_INFO response capture from the RX 7600 listed in the adjacent IP Discovery evidence. It records the answers returned by the Linux AMDGPU kernel driver for the query set used by RedNavi's Linux reference collector.

## Device and capture provenance

- GPU: AMD Radeon RX 7600, Navi 33
- PCI vendor/device: 1002:7480
- Subsystem: 148c:2421
- PCI BDF: 0000:03:00.0
- Host: CachyOS Linux, x86_64
- Kernel: 7.3.1-1-cachyos
- Capture date: 2026-11-11 (Asia/Bangkok local time)
- Interface: read-only DRM_AMDGPU_INFO ioctl against /dev/dri/renderD128
- Raw text: amdgpu_info_raw.txt in this directory

The capture was produced with a temporary local copy of tools/linux_amdgpu_info_raw.py, changing only its device-ID selector from 0x747E (RX 7800 XT) to 0x7480 (RX 7600). No collector modification is proposed in this evidence-only change.

## Integrity

- Raw capture SHA-256: 2959675fc2d0e30979e673ae0c350d084208bd73a4e925987af4fcc12ff5d158
- Formatting note: trailing whitespace was removed from the zero-byte TIMESTAMP record; response payload bytes are unchanged.
- Text lines: 102
- Successful firmware metadata records: 45
- Successful HW_IP_COUNT queries: 6; EINVAL: 4
- Successful HW_IP_INFO instance-0 queries: 10; instance-1 requests: 10 EINVAL

## Additional query coverage

The capture also retains the collector's auxiliary query results:

- Query `0x21` (`AMDGPU_INFO_VIDEO_CAPS`): 192-byte response retained verbatim.
- Query `0x24` (`AMDGPU_INFO_UQ_FW_AREAS`): 16-byte response retained verbatim.
- Query `0x1f`: 4-byte zero response.
- Query `0x1d` (sensor query family): individual GFX clock, memory clock, temperature and load values are retained in the raw capture.

These are Linux driver API responses from this capture; they are not claims about RedNavi's firmware or runtime state.

## Findings

- ACCEL_WORKING returned 1. This is the Linux driver's status flag, not proof that a particular workload was submitted.
- `AMDGPU_INFO_FW_TA` (`FW_VERSION` type `0x13`) returned nonzero metadata for indices 0, 1 and 2: version `0x13`, feature `0x1` in each response. The Linux UAPI identifies type `0x13` as PSP TA. This is positive runtime evidence that the PSP TA firmware was loaded for this boot; the ioctl does not reveal the source file path or payload hash.
- The successful HW_IP_INFO responses report GFX 11.0, COMPUTE 11.0, DMA 6.0, and VCN decoder/encoder/JPEG 4.0. Their IP Discovery versions cross-check with the existing Navi 33 inventory: GC 11.0.2, SDMA 6.0.2 and VCN/UVD 4.0.4.
- HW_IP_COUNT reports two DMA instances. This is distinct from the HW_IP_INFO instance argument; the public UAPI in the installed kernel headers sets AMDGPU_HW_IP_INSTANCE_MAX_COUNT to 1, so instance argument 1 is rejected.
- VCN decoder reports no available ring bits in this snapshot, while VCN encoder and JPEG report ring 0. This is a Linux API observation only; it does not demonstrate that RedNavi can initialize or submit work to those engines.
- Four instance-0 structures (UVD, VCE, UVD_ENC and VPE) are zero-filled, with corresponding count queries returning EINVAL. They are not positive evidence that those engines are initialized.

## Method and limitations

The capture is raw hexadecimal ioctl output. The decoder and analysis used during local review are not included in this evidence-only PR; the raw output is retained so reviewers can independently parse it. The installed kernel build headers were fingerprinted locally (SHA-256 ac7f6dae44b224f34591152123fbf2047d3486c35d39f3eb818a96ea47a6462a), but the matching amdgpu_kms.c implementation source was not present in the build tree. Implementation-specific interpretation therefore remains cross-checked against upstream Linux source, not certified against the exact CachyOS source revision.

This capture did not read GPU registers, submit GPU work, reset or reconfigure the device, load or write firmware, or run RedNavi on macOS. It does not establish PSP/SMU/DMCUB initialization, GPU queue submission, Metal support, or WindowServer acceleration.

The capture contains no VBIOS image, EDID, ACPI table or firmware binary.
