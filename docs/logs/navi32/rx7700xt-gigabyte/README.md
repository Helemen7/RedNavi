# Gigabyte Radeon RX 7700 XT 12GB (`1002:747E`, Subsystem `1458:2414`) — Step 0 Linux Capture

Read-only Linux evidence collected on Ubuntu 24.04.5 LTS (kernel `7.0.0-43-generic`) for a Gigabyte Radeon RX 7700 XT 12GB (`1002:747E`, subsystem `1458:2414`, rev `FF`).

## Commands used to produce each file

1. `sudo python3 tools/linux_collect.py` (unpacked from `rednavi_report_0x747e_20261114-165026.tar.gz`):
   - `amdgpu_dmesg.txt` — kernel `dmesg` lines matching `amdgpu|drm|fb0|simple-framebuffer`
   - `connectors.txt` — `/sys/class/drm/card1-*/status`, `enabled`, and `modes`
   - `dcn_pipe.txt` — DCN 3.2 register dump produced by `tools/linux_dcn_pipe_dump.py`
   - `ip_discovery.txt` — `/sys/class/drm/card1/device/ip_discovery/die/0/*` tree walk
   - `lit_outputs.txt` — active `SYMCLK*` and `DIG*` front-end summary
   - `system.txt` — kernel/OS release info and `lspci -vvnn` output for the GPU PCIe hierarchy (`01:00.0`..`03:00.1`)

2. `python3 tools/amdgpu_info_raw.py > amdgpu_info_raw.txt`:
   - `amdgpu_info_raw.txt` — raw `DRM_IOCTL_AMDGPU_INFO` queries against `/dev/dri/renderD128`

## Privacy redactions

- `amdgpu_dmesg.txt`: root filesystem UUID replaced with `[REDACTED]`, custom EDID filename replaced with `edid/custom_edid.bin`, and `unique_id` lines removed.
- `system.txt`: `lspci -vvnn` trimmed to the GPU PCIe bridges/endpoints (`01:00.0`..`03:00.1`) with the `Device Serial Number` capability removed.

## Non-stock boot parameters warning

As shown in `amdgpu_dmesg.txt`, this machine booted with `drm.edid_firmware=DP-1:edid/custom_edid.bin amdgpu.dcfeaturemask=0x10`. Because of this override, the `DP-1` modes in `connectors.txt` and `DP-1` / `DIG0` pipe state in `dcn_pipe.txt` may not match a stock boot and should not be used as default reference data.
