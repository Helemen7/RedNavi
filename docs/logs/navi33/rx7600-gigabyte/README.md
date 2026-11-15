# RX 7600 (Navi 33), second board: Gigabyte

Step 0 capture of `docs/ADDING_A_GPU.md` from a second RX 7600, contributed by GGBBZZS in issue 6 (thank you). It is read-only evidence from Linux; nothing from it has run under macOS.

- GPU: AMD Radeon RX 7600, Navi 33, PCI `1002:7480`, revision `cf`, subsystem `1458:242b` (Gigabyte), `0000:03:00.0`
- Host: CachyOS Linux, kernel 7.3.4-1-cachyos, `amdgpu` in use
- Capture date: 2026-11-15
- Display: the monitor is on `HDMI-A-1` (1920x1080); both DisplayPort ports are disconnected (`connectors.txt`), so `dcn_pipe.txt` and `lit_outputs.txt` describe an HDMI pipe (`SYMCLKA DIG front end 0`), not the DisplayPort path the Navi 32 replay is built on.

Files: `ip_discovery.txt`, `connectors.txt`, `lit_outputs.txt`, `dcn_pipe.txt`, `amdgpu_dmesg.txt` (from `tools/linux_collect.py`), `amdgpu_info_raw.txt` (from `tools/linux_amdgpu_info_raw.py`). The full `lspci` dump of the report is left out. No serial numbers, EDID or VBIOS image are in the files.

What it adds to the capture in `../rx7600-frame121/` (PowerColor `148c:2421`):

- the five IP versions that identify the die are the same (GC 11.0.2, DCN 3.2.1, MP0 and MP1 13.0.7, SDMA0 6.0.2); `tests/host/compare_discovery_test.py` checks it;
- the 45 `FW_VERSION` answers are identical and there is no answer for type `0x13` (PSP TA) on either board; the kernel log says the optional TAs are not available;
- `DEV_INFO` differs in three bytes (offsets 32 to 34, a clock field) and the memory answers differ in the usage counters;
- the PSP TMR is reserved at `0x81fc000000` (`0x1300000` bytes) with 8176M of VRAM, as on the other board.
