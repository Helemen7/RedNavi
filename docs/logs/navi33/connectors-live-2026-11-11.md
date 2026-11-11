# RX 7600 live DRM connector evidence — 2026-11-11

## Device identity

- PCI BDF: `0000:03:00.0`
- PCI ID: `1002:7480` (Navi 33 / Radeon RX 7600 family)
- Subsystem: `148c:2421`
- Kernel driver: `amdgpu`
- DRM card: `card1`, resolved through sysfs to the same PCI BDF above

## Connector state

Captured from `/sys/class/drm/card1-*` on the same host and boot:

| Connector | Status | Enabled | EDID bytes | Modes |
|---|---|---|---:|---|
| `DP-1` | connected | enabled | 256 | 1920x1080 @ 180 Hz preferred; additional 1920x1080, 1680x1050, 1600x900 and lower modes |
| `DP-2` | disconnected | disabled | 0 | none |
| `DP-3` | disconnected | disabled | 0 | none |
| `HDMI-A-1` | disconnected | disabled | 0 | none |

Read-only `modetest -M amdgpu -c -e -p` enumeration additionally reports connector ID `402`, encoder ID `401`, and active CRTC ID `382` for DP-1. The connector properties report `DPMS=On`, `link-status=Good`, and `subconnector=DP`; the encoder object is reported as type `TMDS`, so its object type is recorded literally rather than used to infer the physical link. The preferred active mode is 1920x1080 at 180 Hz.

The `DP-1` EDID base block checksum is valid. Sanitized decode: vendor `SAM`, monitor model `LS24DG30X`, EDID 1.4, 1920x1080 at 180 Hz detailed timing, vertical range 48–180 Hz. The EDID serial string is intentionally not reproduced in this report.

EDID SHA-256: `4bfb8bdca8569cb852a27b9979d56f5792645cab92226a866c5a4e6c05b3dc9d`

## Interpretation and limitations

- This is direct Linux DRM connector evidence for this RX 7600 and this boot, not proof of a universal board connector map.
- The earlier `connectors.txt` snapshot reports `card0-HDMI-A-2`; current sysfs resolves the RX 7600 as `card1` with `DP-1` connected. DRM card numbering and the active cable/monitor can differ between captures, so the snapshots must not be merged as if they describe the same connector state.
- No connector register writes, link retraining, modeset experiments, or firmware changes were performed.
- The kernel debugfs DRM state file was not readable with the current user permissions, so no DCN pipe / PHY register mapping is claimed.
- This evidence does not establish a complete board-level port map or macOS display bring-up readiness.
