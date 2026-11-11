# Navi 33 profile draft — validation record

Date: 2026-11-11
Host: CachyOS x86_64, kernel `7.3.1-1-cachyos`

## Passed

| Check | Result |
|---|---|
| Matching kernel-build UAPI `sizeof(struct drm_amdgpu_info_device)` | 440 bytes |
| Live `AMDGPU_INFO_DEV_INFO` ioctl using matching 440-byte UAPI | PASS (`rc=0`, PCI ID `0x7480`) |
| DEV_INFO payload boundary canary (8 bytes after 440-byte buffer) | PASS (all bytes unchanged) |
| Alternate installed libdrm UAPI struct size | 448 bytes; distinct header layout, not used for the 440-byte kernel-build test |
| Live RX 7600 connector / EDID capture | PASS (`card1-DP-1` connected; 256-byte EDID valid; DRM connector / encoder / CRTC IDs captured) |
| Generated `linux_info_navi33.h` DEV_INFO length | 440 bytes |
| Generated firmware metadata entries | 16 unique types (index 0) |
| Navi 33 structural host test | PASS |
| Existing ASIC lookup host test | PASS |
| Existing EDID host test | PASS |
| Existing mode-register host test | PASS |
| Header generator reproducibility | PASS (regenerated output SHA-256 unchanged) |
| Candidate JSON parse | PASS |
| Firmware inventory compressed-file hashes | 9/9 PASS |
| `git diff --check` | PASS |

The read-only Linux ABI smoke test is `tools/linux_amdgpu_abi_smoke.c`. Build it against the running kernel build UAPI headers (the `__user` define is needed when consuming kernel headers from a userspace test):

```sh
K=/usr/lib/modules/$(uname -r)/build
cc -D__user= -Wno-cpp -Wall -Wextra \
  -I"$K/include/uapi" -I"$K/include" \
  tools/linux_amdgpu_abi_smoke.c -o /tmp/linux_amdgpu_abi_smoke
/tmp/linux_amdgpu_abi_smoke /dev/dri/renderD128
```

The live test returned `device_id=0x7480`, `family=145`, `vram_type=9`, `vram_bit_width=128`, and `shader_engines=2`; the 8-byte post-payload canary remained `a5a5a5a5a5a5a5a5`. The system libdrm header independently reports 448 bytes because its struct layout differs from the matching kernel-build UAPI header. This is a Linux-side ABI smoke test, not a macOS `infoQuery()` integration test.

The live connector evidence is in `connectors-live-2026-11-11.md`. It maps `card1` to PCI BDF `0000:03:00.0` and records DP-1 connected with a valid 256-byte EDID, DRM connector 402 / encoder 401 / CRTC 382, DPMS On and link-status Good. The older `connectors.txt` snapshot reports a different card/connector state and is retained as a separate capture, not merged with the current one.

The new Navi 33 structural test verifies the generated payload length, HW_IP_INFO / HW_IP_COUNT / MAX_IBS array dimensions, firmware metadata count and PCI device ID. It does not register the profile with the live ASIC table.

## Full-suite limitation on this host

The repository's full `make test` target was attempted but stops in the compatibility-layer host tests before reaching the later targets. On CachyOS, the existing compatibility test sources currently encounter host-header / errno build issues, including undeclared `ENOENT`, `EINVAL`, `ENOMEM`, Linux `__u32` declarations and a `loff_t` typedef conflict.

The compatibility sources were not changed for this Navi 33 draft. The ASIC, EDID and mode-register tests were therefore compiled and run individually, along with the new Navi 33 test. The complete `make test` target should be rerun on the repository's supported macOS host or after a separate portability fix.

## Safety / integration checks

- `RedNavi/asic.cpp` unchanged.
- `RedNavi/Info.plist.in` unchanged.
- No Navi 33 PCI ID or accelerator match added.
- Generated Navi 33 DRM header is not included by the live ASIC table.
- No firmware/register header was wired into a hardware write path.
- ABI smoke test issues only the read-only `AMDGPU_INFO_DEV_INFO` query; no GPU work submitted.
- No GPU registers accessed or written; no firmware changed; no reset or reconfiguration performed.
