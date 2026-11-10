# Adding a GPU

This is the path for bringing RedNavi up on a card other than the RX 7800 XT. Read `CONTRIBUTING.md` first: the rules there (one bring-up step per pull request, evidence in `docs/logs/<chip>/`, no firmware in the repository) apply to every step below.

## How the driver decides what a card is

The probe reads the card's IP discovery table and looks the card up once in the ASIC table in `RedNavi/asic.cpp`. A row matches when the versions of five blocks (GC, DCN, MP0, MP1, SDMA) equal the table's and the PCI device ID is one of the row's boards. The versions say which die it is (Navi 32 is GC 11.0.3, Navi 33 is GC 11.0.2, Navi 31 is GC 11.0.0); the device ID says which board, and a board is listed only after somebody has run the driver on it.

A card that matches no row is read-only. The probe publishes its IP discovery table and VBIOS as properties and writes nothing to it. That is the state of every card except the RX 7800 XT today.

The result of the lookup is the `asic` property of the probe node:

```
ioreg -rn RedNaviProbe -l -x | grep '"asic"'
```

It holds the five versions the probe read (`gc`, `dcn`, `mp0`, `mp1`, `sdma`) and, for a card in the table, the row's name and what it was built from.

## What is in the table and what is not yet

State of the lookup: verified on the RX 7800 XT (2026-11-10): the card matches the Navi 32 row, the `asic` property shows GC 11.0.3, DCN 3.2.0, MP0 and MP1 13.0.10, SDMA 6.0.3, and the display, the framebuffer and RADV come up as before. The read-only path for a card outside the table is host-tested only (`tests/host/asic_test.cpp`); it has not run on such a card since the change (unverified).

Selected through the row (built and host-tested, not yet run on the card: unverified on hardware):

- the `DRM_AMDGPU_INFO` answers (`drm_info` of the row, generated into `RedNavi/linux_info_<name>.h`). A card outside the table gets `kIOReturnUnsupported` for these queries;
- the display replay table (`replay` of the board, `RedNavi/dcn_replay.h`).

Not done yet: the row's `firmware` and `reg_headers` fields are names, not data the code selects from. The register headers and the firmware headers are compiled in for Navi 32, and the code that uses them does not look at the row. So a second row is not enough to run another die: the Navi 32 constants listed below have to move behind the row first, one block at a time, each with a boot on the RX 7800 XT that shows nothing changed. Issue #7 tracks this.

Navi 32 constants still outside the table:

| What | Where | Tied to |
| --- | --- | --- |
| GC and SDMA register offsets | `RedNavi/gc_regs.h` (generated from the `gc_11_0_3` headers) | GC version. The memory hub registers differ between 11.0.0 and 11.0.3 |
| Display register offsets | `RedNavi/dcn_reglist.h`, offsets in `kern_probe.cpp` | DCN 3.2.0 |
| Firmware | `RedNavi/psp_fw.h`, `smu_fw.h`, `gc_fw.h`, `dmub_fw.h` (generated locally, git-ignored) | PSP, SMU, GC, SDMA and DCN versions |
| Display replay generator | `tools/gen_dcn_replay.py` (its register list and the output name `kRNReplay`) | DCN 3.2, pipe 0 on DIG4 / DP4. The table it writes is selected through the board; a second board needs its own symbol name |
| Connector path | `kern_probe.cpp` (`dpInit`, `probeAux`, `dpTrain`: AUX channel 4, DIG4, UNIPHY2, 4 lanes as literal numbers) | The board, not the die |
| MES, doorbell and memory layout constants | `RedNavi/gfx11.cpp`, `kern_probe.cpp` | GC 11.0.3 with MES; the VRAM offsets assume 16 GB |
| Accelerator match | `RedNaviAccel2/Info.plist.in` | PCI device ID |

## Step 0: collect the evidence on Linux

Boot a Linux live system on the machine with the card, monitor connected to it, and from a text console run:

```
sudo python3 tools/linux_collect.py
```

It is read-only and writes a report directory: PCI IDs, the IP blocks with versions and base addresses, the connectors, the firmware lines of the kernel log and the registers of the display pipe that is lit. Serial numbers are dropped. Do not read the whole DCN register list on any card: gated blocks hang the machine (see `CLAUDE.md`).

For the later steps also capture what the Linux driver answers to RADV:

```
sudo python3 tools/linux_amdgpu_info_raw.py > amdgpu_info_raw.txt
```

Commit the report to `docs/logs/<chip>/` in its own pull request, with the machine added to `docs/TEST_MACHINES.md`. `docs/logs/navi33/` and `docs/logs/navi31/` are examples.

## Step 1: read-only run on macOS

Add the PCI device ID to `IOPCIPrimaryMatch` in `RedNavi/Info.plist.in`, build, and load the kext on a machine whose display does not depend on the card (`CONTRIBUTING.md`, "Safety"). The card is not in the table, so the probe only reads. Check:

- the `asic` property shows five versions and `name` = `not in the table`;
- `tools/decode_discovery.py` on the `ip-discovery` property agrees with the Linux `ip_discovery.txt` from step 0.

You can check a table without booting anything: `make test` builds `build/asic_test`, and `build/asic_test table.bin 7480` prints the versions and the row a raw table would match.

This step is a pull request on its own: the device ID in the plist and the `ioreg` output in `docs/logs/<chip>/`.

## Step 2: the row

Compare your versions with the rows of `RedNavi/asic.cpp`.

**Same five versions as an existing row** (another board of the same die, for example a second Navi 32 card): add a line to that row's board list with your device ID, the model name and the VRAM size. Before you boot it with the write stages enabled, compare the board-specific parts with your Linux report: which DIG, PHY, AUX channel and HPD pin your monitor is on (the replay table drives DIG4 / UNIPHY2 / AUX 0x94), and the VRAM size (the layout assumes 16 GB). If the same device ID is used by boards that differ in these, the board list needs a second key (the PCI revision) first; say so in the issue.

**Different versions**: a new row. Give it the versions, the board list and the names of what it will be built from. A new row makes the probe stop treating the card as read-only, so it may only be merged together with, or after, the code that makes each write stage use that row's data. Until the constants above have moved behind the row, keep the new row in your branch.

## Step 3: bring-up, one block per pull request

Follow the order of `docs/ROADMAP.md`. Every stage that writes to the card is behind a `rednavi_*` boot-arg; enable one at a time and read the properties of the probe node after each boot.

1. PSP: `tools/gen_psp_fw_header.py` with your `psp_*_sos.bin`. `docs/PSP_BRINGUP.md`.
2. SMU: `tools/gen_smu_fw_header.py`. `docs/COMPAT.md`.
3. DMCUB: `tools/gen_dmub_fw_header.py` with your `dcn_*_dmcub.bin`. `docs/DMUB_BRINGUP.md`.
4. Display: your own pipe capture (`tools/linux_dcn_pipe_dump.py`) and a replay table generated from it (`tools/gen_dcn_replay.py`). `docs/DP_BRINGUP.md`, `docs/DC_PLAN.md`.
5. GC, SDMA, MES: register headers generated from the Linux headers of your GC version (`tools/gen_gc_regs.py` with `tools/gc_regs_spec.txt`), firmware with `tools/gen_gc_fw_header.py`. `docs/COMPAT.md`.
6. `DRM_AMDGPU_INFO`: `python3 tools/gen_linux_info_header.py amdgpu_info_raw.txt RedNavi/linux_info_<name>.h <name>`, then point the row's `drm_info` at `kRNLinuxInfo_<name>`.

Firmware headers are generated on your machine and never committed. Register headers are generated and committed together with the command that produced them; do not edit them by hand and do not copy the Linux headers into the repository.

For each stage the pull request says which boot-args were set, shows the properties that prove the stage ran, and marks everything else unverified. A value of 0 from a block that must have a non-zero ID register means powered down or wrong offset, not success.

## What a pull request for a new chip looks like

Small. The evidence commit is text logs. The row is a few lines. A generated header comes with its generator command. If a pull request for a chip carries thousands of hand-written lines or any binary file, it is doing several of the steps above at once and will be sent back to be split.
