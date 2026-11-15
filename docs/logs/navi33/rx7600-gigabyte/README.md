# RX 7600 (Navi 33), second board: Gigabyte

Step 0 capture of `docs/ADDING_A_GPU.md` from a second RX 7600, contributed by GGBBZZS in issue 6 (thank you). It is read-only evidence from Linux; nothing from it has run under macOS.

- GPU: AMD Radeon RX 7600, Navi 33, PCI `1002:7480`, revision `cf`, subsystem `1458:242b` (Gigabyte), `0000:03:00.0`
- Host: CachyOS Linux, kernel 7.3.4-1-cachyos, `amdgpu` in use
- Capture date: 2026-11-15
- Display: the monitor is on `HDMI-A-1` (1920x1080); both DisplayPort ports are disconnected (`connectors.txt`), so `dcn_pipe.txt` and `lit_outputs.txt` describe an HDMI pipe (`SYMCLKA DIG front end 0`), not the DisplayPort path the Navi 32 replay is built on.

Files: `ip_discovery.txt`, `connectors.txt`, `lit_outputs.txt`, `dcn_pipe.txt`, `dcn_pipe_plane.txt` (the plane groups `hubbub`, `hubp0`, `dpp0`, `dcnvm` plus the clock, OTG, OPP and MPC groups, read from a text console with `tools/linux_dcn_pipe_dump.py`), `amdgpu_dmesg.txt` (from `tools/linux_collect.py`), `amdgpu_info_raw.txt` (from `tools/linux_amdgpu_info_raw.py`). The full `lspci` dump of the report is left out. No serial numbers, EDID or VBIOS image are in the files.

What it adds to the capture in `../rx7600-frame121/` (PowerColor `148c:2421`):

- the five IP versions that identify the die are the same (GC 11.0.2, DCN 3.2.1, MP0 and MP1 13.0.7, SDMA0 6.0.2); `tests/host/compare_discovery_test.py` checks it;
- the 45 `FW_VERSION` answers are identical and there is no answer for type `0x13` (PSP TA) on either board; the kernel log says the optional TAs are not available;
- `DEV_INFO` differs in three bytes (offsets 32 to 34, a clock field) and the memory answers differ in the usage counters;
- the PSP TMR is reserved at `0x81fc000000` (`0x1300000` bytes) with 8176M of VRAM, as on the other board.

The plane dump (`dcn_pipe_plane.txt`, 2026-11-15, text console, 1920x1080 on HDMI) next to the linear console plane of the RX 7800 XT (`docs/logs/linux-reference/dcn_plane_linux_linear.txt`): `HUBP0_DCSURF_SURFACE_CONFIG` (`0x8`), `HUBP0_DCSURF_TILING_CONFIG` (`0x80`), `HUBPREQ0_DCSURF_SURFACE_PITCH` (`0x77f`), `HUBP0_DCHUBP_REQ_SIZE_CONFIG`, `HUBP0_HUBP_CLK_CNTL` and `HUBP0_DCHUBP_CNTL` are equal. The surface address differs only in the high word (`0x81` here, `0x83` on the 7800 XT), which is the top of each card's VRAM. So the plane programming for a linear buffer looks the same on Navi 32 and Navi 33; this is a comparison of two register captures, not a test on hardware under macOS. The timing registers differ because the monitor and mode differ (the OTG totals here are for the HDMI monitor).
