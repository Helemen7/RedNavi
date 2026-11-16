# RX 7600 on macOS (read-only probe)

First macOS run of the probe on an RX 7600 (Navi 33, `1002:7480`) in a Mac Pro, 2026-11-16, reported by a tester. No boot-arg was set, so the probe only reads.

- `ioreg.txt`: `ioreg -rn RedNaviProbe -l -x` as sent, with the `vbios` property removed (vendor firmware, not committed).
- `compare_discovery.txt`: `python3 tools/compare_discovery.py ioreg.txt ../rx7600-gigabyte/ip_discovery.txt` (the Linux table of the same board): SAME, 39 blocks, versions and base addresses agree.

What it shows: the kext loads, the probe names the card Navi 33 / AMD Radeon RX 7600 (GC 11.0.2, DCN 3.2.1, MP0 and MP1 13.0.7, SDMA 6.0.2) and the discovery table read on macOS is the Linux one. What it does not show: anything about writes, display, firmware or acceleration on this card (none was attempted, `write_ok` is 0).
