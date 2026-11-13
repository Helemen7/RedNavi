# Navi 33 profile draft — validation record

Date: 2026-11-14
Host: CachyOS x86_64, kernel `7.3.1-1-cachyos`

This file records commands and outcomes only. Hardware interpretation, profile facts and the integration boundary are maintained in [ASIC_PROFILE_DRAFT.md](ASIC_PROFILE_DRAFT.md).

## Passed

| Check | Result |
|---|---|
| Matching kernel-build UAPI `sizeof(struct drm_amdgpu_info_device)` | 440 bytes |
| Live `AMDGPU_INFO_DEV_INFO` ioctl using matching 440-byte UAPI | PASS (`rc=0`, PCI ID `0x7480`) |
| Live RX 7600 connector / EDID capture | PASS (`card1-DP-1` connected; 256-byte EDID valid) |
| Generated `linux_info_navi33.h` DEV_INFO length | 440 bytes |
| Generated firmware metadata entries | 16 unique types (index 0) |
| Navi 33 structural host test | PASS; included in the shared `make test` target |
| Existing ASIC lookup host test | PASS in full suite |
| Existing EDID host test | PASS in full suite |
| Existing mode-register host test | PASS in full suite |
| IP Discovery comparison tests | PASS (9/9) |
| Full `make test` on CachyOS | PASS (compat, EDID, mode-register, ASIC, Navi 33 structural and IP Discovery comparison tests) |
| Header generator reproducibility | PASS (regenerated output SHA-256 unchanged) |
| Firmware inventory compressed-file hashes | 9/9 PASS for files present at inventory collection time |
| `git diff --check` | PASS |

The read-only Linux ABI smoke test is `tools/linux_amdgpu_abi_smoke.c`. Build it against the running kernel build UAPI headers (the `__user` define is needed when consuming kernel headers from a userspace test):

```sh
K=/usr/lib/modules/$(uname -r)/build
cc -D__user= -Wno-cpp -Wall -Wextra \
  -I"$K/include/uapi" -I"$K/include" \
  tools/linux_amdgpu_abi_smoke.c -o /tmp/linux_amdgpu_abi_smoke
/tmp/linux_amdgpu_abi_smoke /dev/dri/renderD128 0x7480
```

The live test returned `device_id=0x7480`, `family=145`, `vram_type=9`, `vram_bit_width=128`, and `shader_engines=2`. This is a Linux-side userspace ioctl smoke test, not a macOS `infoQuery()` integration test. The alternate installed libdrm header reports a 448-byte layout and was not used for the matching kernel-build UAPI check.

## Full-suite result on CachyOS

The full `make test` target passes on this CachyOS host, including the Navi 33 structural test and the IP Discovery comparison suite. The Navi 33 test is compiled with `-IRedNavi` so the generated evidence header can resolve the shared `asic.h` declarations. This is host-side structural and parser validation only; it does not exercise the macOS kext or perform GPU register writes. The maintainer independently confirmed that CI is green and `make test` passes on macOS.
## Not yet validated

- macOS-side `infoQuery()` integration against the captured 440-byte payload.
- Exact firmware filename/load-path attestation for every required IP.
- DCN pipe / PHY mapping for the live DP-1 connector.
- Per-register compatibility of candidate register headers.
