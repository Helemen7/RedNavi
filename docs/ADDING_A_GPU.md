# Adding a GPU

This is the path for bringing RedNavi up on a card other than the RX 7800 XT. Read `CONTRIBUTING.md` first: the rules there (one bring-up step per pull request, evidence in `docs/logs/<chip>/`, no firmware in the repository) apply to every step below.

## How the driver decides what a card is

The probe reads the card's IP discovery table and looks the card up in the ASIC registry in `RedNavi/asic.cpp`. Generic lookup logic and the small registry live there; each ASIC's profile data is exported by its own translation unit (for example, `RedNavi/asic_navi32.cpp`) and registered in that list. A profile matches when the versions of five blocks (GC, DCN, MP0, MP1, SDMA) equal the table's and the PCI device ID is one of the profile's boards. The versions say which die it is (Navi 32 is GC 11.0.3, Navi 33 is GC 11.0.2, Navi 31 is GC 11.0.0); the device ID says which board, and a board is listed only after somebody has run the driver on it.

Boards of one die can share a device ID: the RX 7800 XT and the RX 7700 XT are both `1002:747E` with the same five versions. The lookup then also takes the memory size the register reports (`RCC_CONFIG_MEMSIZE`: 16368 MB on the 7800 XT, about 12272 MB on the 7700 XT) and picks the board whose `vram_mb` is within 64 MB of it. A size that fits no board of the ID is a card that is not in the table.

Each board has a `write_ok` flag. 1 means the constants compiled into the kext are those of this board and the write stages may run on it (the RX 7800 XT only). 0 means the board is known from a Linux capture, it is named in the `asic` property (`write-stages` says `no ...`), its Linux data is kept in the row (device ID, memory size, TMR offset, the `DRM_AMDGPU_INFO` answers), and the probe stays read-only on it exactly as on a card outside the table. Today the RX 7700 XT (`asic_navi32.cpp`) and the RX 7600 (`asic_navi33.cpp`, Navi 33) are in that state. A board gets `write_ok` 1 only in the pull request that moves the constants of its die behind the row and shows a boot on that board.

A card that matches no row is read-only. The probe publishes its IP discovery table and VBIOS as properties and writes nothing to it. That is the state of every card except the RX 7800 XT today.

The result of the lookup is the `asic` property of the probe node:

```
ioreg -rn RedNaviProbe -l -x | grep '"asic"'
```

It holds the five versions the probe read (`gc`, `dcn`, `mp0`, `mp1`, `sdma`) and, for a card in the table, the row's name and what it was built from.

## What is in the table and what is not yet

State of the lookup: verified on the RX 7800 XT (2026-11-10, and again 2026-11-16 after the board lookup by VRAM size and the `write_ok` flag: `name` Navi 32, `model` AMD Radeon RX 7800 XT, `write-stages` yes, `rn_test_new` PASS, with WindowServer in software mode): the card matches the Navi 32 row, the `asic` property shows GC 11.0.3, DCN 3.2.0, MP0 and MP1 13.0.10, SDMA 6.0.3, and the display, the framebuffer and RADV come up as before. The read-only path for a card outside the table is host-tested (`tests/host/asic_test.cpp`), including against the real IP discovery table of an RX 7600 captured on Linux (`docs/logs/navi33/rx7600-frame121/amdgpu_discovery.bin`: GC 11.0.2, DCN 3.2.1, MP0 and MP1 13.0.7, SDMA 6.0.2, no row matches, for any device ID). The kext itself has run on a card outside the table once since the change: an RX 7600 in a machine with a Mac Pro SMBIOS (2026-11-16, reported by a tester; `docs/logs/navi33/rx7600-macos/`): `RedNaviProbe` matched, the `asic` property showed Navi 33 / AMD Radeon RX 7600 with GC 11.0.2, DCN 3.2.1, MP0 and MP1 13.0.7, SDMA 6.0.2, and `tools/compare_discovery.py` against the Linux capture of the same board reported SAME (39 blocks). Read-only only; the RX 7700 XT has not run (unverified).

Selected through the row (verified on the RX 7800 XT, one boot, 2026-11-11: DisplayPort trains through the row's path, the replay leaves the same eleven read-back differences as before, the display runs at 1920x1080 at 144 Hz, and `rn_test info` gets `DEV_INFO` and the firmware versions of the row's snapshot):

- the `DRM_AMDGPU_INFO` answers (`drm_info` of the row, generated into `RedNavi/linux_info_<name>.h`). A card outside the table gets `kIOReturnUnsupported` for these queries;
- the display replay table (`replay` of the board, `RedNavi/dcn_replay.h`);
- the DisplayPort path (`dp` of the board): the AUX engine, the PHY id, the HPD pin, the connector id, the lane count and the DIG back end instance (`dig`, 4 on the RX 7800 XT). The DIG and DP register offsets of `dpTrain`, `dcnReplay`, `quiescePipe` and `programLive` are the DIG4/DP4 offsets moved to that instance with `rn_dig_off` (stride 0x100 dwords in `dcn_3_2_0_offset.h`); `rn_mode_register` accepts the DP block of any instance in a replay table's names. Verified on the RX 7800 XT, one boot, 2026-11-16: DP training `cr_ok` `eq_ok`, the replay leaves the same eleven read-back differences, `rn_test_new` PASS. The offsets for another instance are host-tested only (unverified).

Not done yet: the row's `firmware` and `reg_headers` fields are names, not data the code selects from. The register headers and the firmware headers are compiled in for Navi 32, and the code that uses them does not look at the row. So a second row is not enough to run another die: the Navi 32 constants listed below have to move behind the row first, one block at a time, each with a boot on the RX 7800 XT that shows nothing changed. Issue #7 tracks this.

Navi 32 constants still outside the table:

| What | Where | Tied to |
| --- | --- | --- |
| (done, one table) GC and SDMA register offsets | `RedNavi/gc_regs.h`, generated by `tools/gen_gc_regs.py ... --alt <gc_11_0_0_offset.h>` | The offsets are a table (`struct rn_gc_offsets`, read through `rn_gc`), and `rn_gc_select` in `asic.cpp` picks it from the GC version at probe time: 11.0.3 has its own memory hub offsets, 11.0.0 to 11.0.2 use the `gc_11_0_0` ones (Linux has no `gc_11_0_2` headers; 44 of the 199 registers the kext uses differ between the two, the base indices are the same). Verified on the RX 7800 XT, one boot, 2026-11-16: same behaviour, `rn_test_new` PASS. The `gc_11_0_0` table is host-tested for its values only and has never run (unverified). Field masks and the reset values are still those of 11.0.3 / `gc_11_0_0_default.h` |
| (checked) Display register offsets | `RedNavi/dcn_reglist.h`, offsets in `kern_probe.cpp` | DCN 3.2.0 and 3.2.1 (Navi 33) place the same registers at the same offsets: of the 4366 registers of the dump list, 4349 exist in `dcn_3_2_1_offset.h` and none differs in offset or base index; none of the 538 registers of the replay capture differs or is missing (compared 2026-11-16 on the Linux master headers, not run). So no second table is needed. The 17 registers that only 3.2.0 defines are listed in `kRNDcnRegAbsent321` (generated with `tools/gen_dcn_reglist.py dcn_3_2_0_offset.h dcn_3_2_1_offset.h`) and `dcnDump` does not read them on a 3.2.1 part (the dump stays behind `rednavi_dcn_dump_unsafe`, host-built only) |
| (by design) Firmware | `RedNavi/psp_fw.h`, `smu_fw.h`, `gc_fw.h`, `dmub_fw.h` (generated locally, git-ignored) | PSP, SMU, GC, SDMA and DCN versions. The blobs cannot be committed (`LICENSE.amdgpu`), so a build carries the set its generators were given; the row's `firmware` names the Linux files that set must come from. A kext with the sets of several dies is not a goal. The RX 7700 XT needs the same files as the RX 7800 XT; Navi 33 needs its own (`psp_13_0_7_sos.bin`, `smu_13_0_7.bin`, `dcn_3_2_1_dmcub.bin`, `gc_11_0_2_*`, `sdma_6_0_2.bin`, from the Linux log) |
| (done) Display replay generator | `tools/gen_dcn_replay.py` | DCN 3.2, pipe 0. `--dig N` renames the DIG, DP and SYMCLK registers for a connector on DIG N, `--max-group G` keeps the groups up to G, `--name` writes a second board's table under its own symbol. The RX 7700 XT row carries a table made with `--dig 0 --max-group 9` from its own capture (`dcn_replay_navi32_747e_7700xt.h`, 57 registers): only the test pattern groups, because that capture has no plane registers (a linear console capture with the plane groups is still needed) and it was taken at 3840x2160 with a custom EDID. Never run on a 7700 XT (unverified); the 7800 XT table regenerates unchanged with the default arguments |
| (done) DIG and DP back-end registers | `kern_probe.cpp` | Now selected through the board's `dp.dig` (see above). What remains for a second board is its own replay table and the DIG/PHY ids from its capture |
| MES, doorbell and memory layout constants | `RedNavi/gfx11.cpp`, `kern_probe.cpp` | GC 11.0.3 with MES. The end of the VRAM aperture the GMC is given is now read from `FB_LOCATION_TOP` (verified on the RX 7800 XT, one boot, 2026-11-16: same value, `rn_test_new` PASS); the fixed low-VRAM offsets (display, GART table, MES areas, DMCUB block) are below 1.5 GB and fit any board with 8 GB or more |
| Kext matches | `RedNavi/Info.plist.in`, `RedNaviAccel2/Info.plist.in` (`IOPCIPrimaryMatch`) | PCI device ID. The probe lists every card that may be read; the accelerator lists only boards in the table |

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

With several amdgpu cards (an APU next to a discrete card), name the one you mean: `sudo python3 tools/linux_amdgpu_info_raw.py 0x1586 > amdgpu_info_raw.txt`.

Commit the report to `docs/logs/<chip>/` in its own pull request, with the machine added to `docs/TEST_MACHINES.md`. `docs/logs/navi33/` and `docs/logs/navi31/` are examples.

## Step 1: read-only run on macOS

**RX 7700 XT and RX 7600 testers:** your device IDs are already in `IOPCIPrimaryMatch` and your boards have rows (`write_ok` 0), so there is nothing to add. Build, load the kext as below (display not on the card), and send `ioreg -rn RedNaviProbe -l -x > ioreg.txt` plus the output of `python3 tools/compare_discovery.py ioreg.txt <your ip_discovery.txt>`. What the `asic` property should show: RX 7700 XT `name` Navi 32, `model` AMD Radeon RX 7700 XT, `write-stages` starting with `no`; RX 7600 `name` Navi 33, `model` AMD Radeon RX 7600, `write-stages` starting with `no`. A `model` of the 7800 XT on a 7700 XT would be a bug in the VRAM lookup: say so. No boot-arg is needed and none should be set. The tracking issue is Helemen7/RedNavi#7.

Add the PCI device ID to `IOPCIPrimaryMatch` in `RedNavi/Info.plist.in`, build, and load the kext on a machine whose display does not depend on the card (`CONTRIBUTING.md`, "Safety"). The card is not in the table, so the probe only reads. Check:

- the `asic` property shows five versions and `name` = `not in the table`;
- the `ip-discovery` property agrees with the Linux `ip_discovery.txt` from step 0: save `ioreg -rn RedNaviProbe -l -x` to a file and run `python3 tools/compare_discovery.py ioreg.txt ip_discovery.txt`. It prints `SAME` and exits 0 when every block has the same version and base addresses, and lists the differences otherwise (`tools/decode_discovery.py` prints the decoded table if you want to read it).

You can check a table without booting anything: `make test` builds `build/asic_test`, and `build/asic_test table.bin 7480` prints the versions and the row a raw table would match.

This step is a pull request on its own: the device ID in the plist and the `ioreg` output in `docs/logs/<chip>/`.

## Step 2: the row

Compare your versions with the per-ASIC profiles (for example, `RedNavi/asic_navi32.cpp`) and the registry in `RedNavi/asic.cpp`.

**Same five versions as an existing row** (another board of the same die, for example a second Navi 32 card): add a line to that ASIC profile's board list in its `RedNavi/asic_<asic>.cpp` file with your device ID, the model name, the VRAM size and the TMR offset from your Linux log, with `write_ok` 0. If the device ID is already in the list, the VRAM size is what tells the boards apart (the lookup accepts the register value within 64 MB of the row's size). Before you boot it with the write stages enabled, compare the board-specific parts with your Linux report: which DIG, PHY, AUX channel and HPD pin your monitor is on (the replay table drives DIG4 / UNIPHY2 / AUX 0x94), and the VRAM size (the layout assumes 16 GB). If the same device ID is used by boards that differ in these, the board list needs a second key (the PCI revision) first; say so in the issue.

**Different versions**: a new profile translation unit, `RedNavi/asic_<asic>.cpp`. Export its profile, give it the versions, board list and provenance, then add the profile to the registry in `RedNavi/asic.cpp`. A new row makes the probe stop treating the card as read-only, so it may only be merged together with, or after, the code that makes each write stage use that row's data. Until the constants above have moved behind the profile, keep the new profile in your branch.

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

## Known limit: the RX 7700 XT is not told apart from the RX 7800 XT

The RX 7700 XT is the same die and reports the same device ID (`0x747E`) and the same five IP versions as the RX 7800 XT (GC 11.0.3, DCN 3.2.0, MP0 and MP1 13.0.10, SDMA 6.0.3; Step 0 capture in `docs/logs/navi32/rx7700xt-gigabyte/`, once merged). The ASIC lookup alone would give it the 7800 XT profile, so the probe also compares the VRAM size in `RCC_CONFIG_MEMSIZE` with the board's `vram_mb` (with 64 MB of slack) and treats a card with less memory as not in the table: read-only, like the RX 7600. Nobody has run RedNavi on a 7700 XT; it needs its own board row (and the TMR offset from its Linux log, `psp_tmr_mb`). Status: the size check is built but not run on hardware (the 7800 XT keeps its row if the register reads 16384 or 16368); the 7700 XT behaviour is read from the Linux log and the source.
