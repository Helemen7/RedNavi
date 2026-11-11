# Navi 33 profile draft — validation record

Date: 2026-11-11
Host: CachyOS x86_64, kernel `7.3.1-1-cachyos`

## Passed

| Check | Result |
|---|---|
| Matching UAPI `sizeof(struct drm_amdgpu_info_device)` | 440 bytes |
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
- No GPU registers accessed or written; no firmware changed; no GPU work submitted; no reset or reconfiguration performed.
