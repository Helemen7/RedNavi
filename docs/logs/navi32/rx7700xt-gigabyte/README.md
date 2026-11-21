# Gigabyte Radeon RX 7700 XT 12GB (`1002:747E`, Subsystem `1458:2414`) — Linux Capture

Read-only Linux evidence collected on Ubuntu 24.04.5 LTS (kernel `7.0.0-43-generic`) for a Gigabyte Radeon RX 7700 XT 12GB (`1002:747E`, subsystem `1458:2414`).

## Verification status

- **Hardware / OS:** Linux read-only register and log capture only. No `RedNavi` macOS kext or replay code has been executed on this card yet (Run 1 on macOS is still pending). The machine remained completely healthy after the `dcio` abort described below, with no hang or crash.

## Commands used to produce each file

1. Linear plane capture (Run 2, `fbcon` text console with `gdm3` stopped; `DP-1` active at native `3840x2160` on stock EDID, `DP-2` active at `2560x1440`):
   `sudo python3 tools/linux_dcn_pipe_dump.py --out docs/logs/navi32/rx7700xt-gigabyte/dcn_pipe_plane.txt clocks hpd_aux0 dig0_dp0 hpd_aux1 dig1_hdmi otg0 opp0 mpc hubbub hubp0 dpp0 dcnvm dcio`
   - `dcn_pipe_plane.txt` — DCN 3.2 register dump covering `clocks`, `hpd_aux0`, `dig0_dp0`, `hpd_aux1`, `dig1_hdmi`, `otg0`, `opp0`, `mpc`, and linear scanout plane groups (`hubbub`, `hubp0`, `dpp0`, `dcnvm`, `dcio`). Note: `dig1_hdmi` is the tool's group name for `DIG1`, while `DP-2` is the DisplayPort sink on `DIG1`.
   - **Truncated `dcio` group:** `dcn_pipe_plane.txt` ends at line 1376 with `# ABORT: the GPU stopped answering (4 reads of 0xffffffff in a row) after DCIO_UNIPHY0_UNIPHY_MACRO_CNTL_RESERVED3` (no `# done dcio` marker). The trailing `0xffffffff` reads (`DCIO_UNIPHY0_UNIPHY_MACRO_CNTL_RESERVED0..3`) are not valid register values and should not be treated as live reads by replay generators.

2. Stock-boot log collection (`sudo python3 tools/linux_collect.py`):
   - `amdgpu_dmesg.txt` — stock-boot `dmesg` lines matching `amdgpu|drm|fb0|simple-framebuffer`
   - `connectors.txt` — stock-boot `/sys/class/drm/card1-*/status`, `enabled`, and `modes` (`DP-1` native `3840x2160` plus `2560x1440`/`1920x1080` modes on stock EDID; `DP-2` `2560x1440`)

3. Earlier / raw captures not regenerated in Run 2:
   - `dcn_pipe.txt`, `lit_outputs.txt`, `ip_discovery.txt`, `system.txt` — from the earlier capture (`dcn_pipe.txt` and `lit_outputs.txt` were captured at `3840x2160` with a custom EDID on `DP-1`, so do not treat them as stock reference data; `ip_discovery.txt` and `lit_outputs.txt` were not regenerated in Run 2).
   - `amdgpu_info_raw.txt` — raw DRM query output (`python3 tools/linux_amdgpu_info_raw.py > amdgpu_info_raw.txt`).

## Privacy redactions

- `amdgpu_dmesg.txt`: kernel command line and `unique_id` lines removed.
- `system.txt`: `lspci -vvnn` trimmed to the GPU PCIe bridges/endpoints (`01:00.0`..`03:00.1`) with the `Device Serial Number` capability removed.
