# RX 7600 (Navi 33): the bring-up ladder

State (2026-11-17): **rung 1 ran to `rung1-done` on the tester's RX 7600, with no hang** (`nvram rednavi-trace` = `ladder33: done`). Verified there: PSP chain, ring, `BOOT_CFG_GET`, `LOAD_TOC`, `SETUP_TMR`, SMU firmware load through the PSP, SMU messages. Not verified and unexplained: `mmhub-regs` reads all `0xffffffff` (expected VRAM base `0x8000`), the `BOOT_CFG_GET` status is `0x80000310` and the ring status `0x80020000` (both differ from the 7800 XT), and `driver_if_version` 0x40 was not compared with the 13.0.7 value in Linux. GC, SDMA, MES, compute and display have not run on a Navi 33. The code is the 7800 XT sequence cut after the SMU, with the Navi 33 firmware and the TMR offset of the board's row.

## Why a ladder

The Navi 33 row has `write_ok` 0, so the probe only reads (`docs/ADDING_A_GPU.md`). Turning `write_ok` on would run the whole 7800 XT start sequence, including the display stages. The RX 7600 tester drives the monitor from the integrated GPU, so the display blocks of the card are clock-gated, and reading them hangs the card (a Linux register dump did exactly that, `device lost from bus`, in the `hpd_aux4` group). So rung 1 is a separate path:

* opt-in with `rednavi_ladder33=1`, only on the Navi 33 row, only in a build whose firmware headers are Navi 33's;
* PSP and SMU only: chain load, ring, `BOOT_CFG_GET`, `LOAD_TOC`, `SETUP_TMR`, SMU firmware through the PSP, SMU test messages;
* no DCN, DMCUB, HPD or AUX register is read or written, and no GC, SDMA, MES or compute stage runs;
* `write_ok` stays 0 until a rung has been seen to work on the board.

## What is different from the 7800 XT

| Item | Navi 32 (7800 XT) | Navi 33 (7600) | Source |
| --- | --- | --- | --- |
| PSP | 13.0.10, `psp_13_0_10_sos.bin` | 13.0.7, `psp_13_0_7_sos.bin`: components KDB, SYS_DRV, SOS and a 2304-byte TOC (no SOC_DRV, INTF_DRV, DBG_DRV, RAS_DRV) | `tools/gen_psp_fw_header.py` output |
| SMU | 13.0.10, `smu_13_0_10.bin` | 13.0.7, `smu_13_0_7.bin`, ucode 0x526100 (82.97.0, the version the Linux log of the board reports) | `tools/gen_smu_fw_header.py` |
| SMU message numbers | | `TestMessage` 1, `GetSmuVersion` 2, `GetDriverIfVersion` 3, `EnableAllSmuFeatures` 6, `SetDriverDramAddr` 0xE/0xF, `AllowGfxOff` 0x28: the same as 13.0.0 | Linux `smu_v13_0_0_ppsmc.h` and `smu_v13_0_7_ppsmc.h` |
| MP0/MP1 register bases | `0x16000` ... | the same | `docs/logs/navi33/rx7600-gigabyte/ip_discovery.txt` |
| TMR | offset 15872 MB | offset 8128 MB (`0x1fc000000`, size `0x1300000`, from "reserve 0x1300000 from 0x81fc000000 for PSP TMR" in the Linux log) | the row (`psp_tmr_mb`) |
| VRAM | 16 GB | 8176 MB reported (row 8192) | row, `RCC_CONFIG_MEMSIZE` |

The fixed VRAM offsets of the PSP buffers (1 GB, ring and command buffers behind it) are below the TMR and are written through `MM_INDEX`/`MM_DATA`, so the BAR size (256 MB with Re-Size BAR off) does not matter.

Not checked: that the PSP chain load of the 13.0.7 bootloader takes the same register writes as 13.0.10 (the Linux `psp_v13_0.c` paths are shared, read but never run here), and that `SETUP_TMR` is wanted by this PSP (the Linux log of the board shows the TMR reserved, which suggests it is).

## Build

The firmware is fetched from linux-firmware and turned into headers outside the repository tree, so the 7800 XT build is not touched:

```
mkdir ~/fw33 && cd ~/fw33
# psp_13_0_7_sos.bin and smu_13_0_7.bin from linux-firmware, amdgpu/
python3 tools/gen_psp_fw_header.py psp_13_0_7_sos.bin ~/fw33/psp_fw.h      # prints KDB, SYS_DRV, SOS, TOC
python3 tools/gen_smu_fw_header.py smu_13_0_7.bin ~/fw33/smu_fw.h          # prints "header v2.1, ip 13.0, ucode version 0x526100"
make RN_FW_DIR=$HOME/fw33 RN_FW_ASIC="Navi 33" RN_OUT_SUFFIX=-navi33 build/Release-navi33/RedNavi.kext
```

`RN_FW_ASIC` is compiled into the kext. If the card's die is another one, `start()` refuses every write stage and sets the property `fw-die-mismatch`. A kext with firmware inside is vendor firmware redistribution (`LICENSE.amdgpu`: allowed as binary with the notice, no reverse engineering): it is handed to a tester as a file, never committed.

## Boot-args (tester)

```
rednavi_ladder33=1 rednavi_psp_load=1 rednavi_psp_ring=1 rednavi_psp_cmd=1 rednavi_psp_toc=1 rednavi_psp_tmr=2 rednavi_smu_load=1 rednavi_smu=1
```

The display must stay on the integrated GPU (`-wegnoegpu` must not be set, no `-wegnoigpu`). Keep a USB copy of the OpenCore EFI without the kext.

## Reading the result

After the boot, if the machine is up:

```
ioreg -rn RedNaviProbe -l -x > ioreg.txt
nvram rednavi-trace
```

Properties to look at: `ladder33` (`rung1-start`, `rung1-done`, or `rung1-stopped: ...`), `mailbox-regs`, `mmhub-regs` (the VRAM base should be `0x8000`, as on the Linux log), `smu-info-early`, `smu-info`, and the PSP properties the stages publish. On the 7800 XT the same stages gave: sOS alive (`C2PMSG_81` nonzero), ring response `0x80C20000`, `LOAD_TOC` status 0, SMU version after the firmware load.

If the machine hangs or resets: after the power cycle, `nvram rednavi-trace` is the last step written before the hang (`ladder33: psp chain`, `psp ring`, `psp toc`, `psp tmr`, `smu firmware`, `smu messages`). OpenCore may keep NVRAM variables only in an emulated store that is written at a clean shutdown; if `nvram rednavi-trace` is empty after a hang, that tells us the breadcrumb did not survive and the last line of the OpenCore or verbose log has to be used instead.

## What comes after rung 1

Only when rung 1 comes back clean: rung 2 adds the GC, SDMA and MES firmware (`gc_11_0_2_*.bin`, `sdma_6_0_2.bin`) with `rn_gc_select` (GC 11.0.2 uses the `gc_11_0_0` offsets, 44 of 199 differ from 11.0.3, never run), then the compute test `tools/rn_test.c`. The display, the HDMI pipe of DCN 3.2.1 and WindowServer come after the card computes (`docs/ROADMAP.md`).

## Rung 2: GC firmware and the RLC autoload (ran on the RX 7600 2026-11-18: GC boots)

**Result on the tester's RX 7600 (`ladder33` = `rung2-done`, no hang):** `gc-load` `loaded` 0x1a of `images` 0x1a, `autoload_status` 0; after the SMU features were enabled `gc-status-late` shows `bootload_status` 0xc000001f (the value the Linux dump has), `cp_stat` 0, `rlc_cntl` 1, `grbm_status` 0x302c, `rlc_gpm_stat` 0xb40016 and `SDMA0_VERSION` 0x600. Before the enable the 2000 ms wait of `loadGcFw` times out (`bootload_status` 0, as on the 7800 XT). In `smu-info` the two `SetAllowedFeaturesMask` messages answer rc 0xffffffdd (not accepted on 13.0.7) but `EnableAllSmuFeatures` rc 0 and the running mask 0x71ffffff / 0x1a3bd follow; the message numbers of 13.0.7 for the allowed mask are not checked.

(Built 2026-11-18, before the run:)

Rung 1 is done (`ladder33: done`, `mmhub-regs` 0x8000 / 0x81ff after the 11.0.2 table, see `kRNMmhubRegs302`). Rung 2 is `rednavi_ladder33=2`: everything of rung 1, then the GC, IMU, RLC, MES and SDMA firmware through the PSP (`loadGcFw`, `LOAD_IP_FW` and `AUTOLOAD_RLC`), then the SMU messages with `rednavi_smu_enable=1` (the RLC boot waits for the features on the 7800 XT), then a read of CP_STAT, `RLC_RLCS_BOOTLOAD_STATUS`, `GRBM_STATUS`, `RLC_GPM_STAT`, `RLC_CNTL`, the IMU scratch and `SDMA0_VERSION` as the property `gc-status-late`. No SDMA, MES, ring, GART or memory hub register is written, and no display block is touched.

Boot-args: `rednavi_ladder33=2 rednavi_psp_load=1 rednavi_psp_ring=1 rednavi_psp_cmd=1 rednavi_psp_toc=1 rednavi_psp_tmr=2 rednavi_smu_load=1 rednavi_smu=1 rednavi_smu_enable=1 rednavi_gc_load=1`.

Build: the firmware headers come from `GC_FW=11_0_2 SDMA_FW=6_0_2 python3 tools/gen_gc_fw_header.py <dir with gc_11_0_2_{pfp,me,mec,rlc,imu,mes_2,mes1}.bin and sdma_6_0_2.bin> ~/fw33/gc_fw.h` (26 images), then the same `make` line as rung 1.

What a good result looks like (the 7800 XT and the Linux dump of the RX 7600, `docs/logs/navi33/rx7600-gigabyte/gc_sdma_regs_linux.txt`): `gc-load` `loaded` = `images`, `autoload_status` 0; `gc-status-late` `bootload_status` with bit 31 set (Linux: `RLC_RLCS_BOOTLOAD_STATUS` 0xc000001f), `cp_stat` 0, `SDMA0_VERSION` 0x600. The GC registers read 0 while the GC is in GFXOFF: a 0 is not a result.

Unverified: the GC offsets of 11.0.2 against the real card (the Linux dump above reads sensible values at the addresses of `gc_regs_navi33.h`, which supports them), that the 13.0.7 PSP takes the same GC `LOAD_IP_FW` types as 13.0.10, and that `EnableAllSmuFeatures` and the other SMU messages of `smuProbe` behave on 13.0.7.

## Rung 3: GFXHUB and an SDMA0 write test (ran on the RX 7600 2026-11-19: passes)

**Result on the tester's RX 7600:** `sdma-test` `ok` 1, `step` 5, `rptr` = `wptr` = 0x14, `fault_status` 0, `freeze` 0, and the CPU reads `value` 0xdeadbeef where it had left 0xcafedead (`sdma-scan` `target_after` 0xdeadbeef): SDMA0 wrote to VRAM through the GFXHUB with the gc_11_0_0 offsets. `sdma-smu` shows SMU version 0x526100. The first attempt did not run the test: the tester had typed `rednavi_sdmal_test=1`, and `sdmaTest()` returned without a trace while `ladder33` still said `rung3-done`. Since then the early returns publish `sdma-skip` (the reason) and `ladder33` reads `rung3-skipped: see sdma-skip`. The memory-hub readbacks match the Linux dump (`gcvm-readback`: `ctx0_cntl` 0x1fffe01, `l2_cntl` 0x80e01, `fb_base` 0x8000) and the CPU writes to VRAM persist (`vram-persist`).

(Built 2026-11-18, before the run:)

`rednavi_ladder33=3 rednavi_sdma_test=1` on top of the rung 2 boot-args. After the rung 2 steps it runs `sdmaTest()` as on the 7800 XT: DisallowGfxOff, a probe of CPU access to VRAM, `rn_gfxhub_enable` (GC memory hub on VRAM and a GART window, with the gc_11_0_0 offsets), then SDMA0 gets a ring in VRAM at 0x0C000000 (192 MB) and writes 0xDEADBEEF to 0x0C002000, which the CPU reads. Results are the properties `sdma-test`, `gcvm-readback`, `vram-persist`, `sdma-scan` and `sdma-smu`. **Leave `rednavi_mmhub` unset:** its register table is MMHUB 3.0.0, the Navi 33 one is 3.0.2 (offsets 0x80 lower, `kRNMmhubRegs302`). The VRAM pools that `sdmaTest` sets up for the user client reach 15.25 GB, more than the 8 GB board has; nothing allocates from them in this rung, and they must be sized from the board before rung 4 (compute).

Linux reference for the GFXHUB registers of this card: `docs/logs/navi33/rx7600-gigabyte/gc_sdma_regs_linux.txt` (for example `GCVM_L2_CNTL` 0x80e01, `GCMC_VM_FB_LOCATION_BASE` 0x8000, `GCVM_CONTEXT0_CNTL` 0x1fffe01). Risk: the GFXHUB writes are the first writes of the kext to the memory hub of a Navi 33; the tester's display is on the iGPU, so the card is not driving it.

## Rung 4: graphics ring, compute ring and the MES (4a, 4b and 4c ran on the RX 7600 2026-11-19: pass)

`rednavi_ladder33=4` is rung 3 with the later stages that `sdmaTest()` chains on their own boot-args, in the order of the 7800 XT. Nothing here has run on a Navi 33; the GC offsets are those of gc_11_0_0 (`rn_gc_select`), and the NBIO and doorbell registers used by the compute stage are the tables of the 7800 XT (NBIO 4.3.0): that is the main unknown. The device-local VRAM pool now ends 256 MB below the board's PSP TMR (7.69 GB on the RX 7600), so nothing is handed out past the end of the 8 GB. `ladder33-args` publishes what the kernel parsed of the stage boot-args (`0xffffffff` means not set), so a mistyped name is visible (rung 3 was first run with `rednavi_sdmal_test`).

Run it in steps, one boot each, adding to the rung 3 boot-args (keep `rednavi_mmhub` unset):

- 4a: `rednavi_ladder33=4 rednavi_gc_init=1 rednavi_gfx_test=1`. Properties: `gfx-test` (`ok` 1, `scratch`), `gfx-trace`.
- 4b: 4a plus `rednavi_compute_test=1 rednavi_gc_awake=1`. Property `compute-test` (`ok` 1).
- 4c: 4b plus `rednavi_mes_clean=1 rednavi_mes_step=5`. Properties `mes-progress`, `mes-state` and the MES result properties (the compute queue is mapped by the MES, `ok` set when the queue is active).

After a hang: `nvram rednavi-trace` and the last lines of the verbose log. Not verified: every register of these stages on Navi 33.

**4a and 4b on the RX 7600:** `gfx-test` `ok` 1 (`scratch` 0xdeadbeef, `rptr_shadow` = `wptr` = 8, no fault) and `compute-test` `ok` 1 (`scratch` 0xdeadbeef, queue active, `rptr` = `wptr` = 8, no fault). The first 4c run did nothing: `rn_mes_start` returns at once without a doorbell map, so 4c also needs `rednavi_doorbell_test=1 rednavi_doorbell_s2a=1` (as on the 7800 XT; this writes the NBIO doorbell aperture and S2A entries, an unverified block on NBIF 4.3.1). The kext now publishes `mes-skip` when the map is missing.

**4c on the RX 7600 (with `rednavi_doorbell_test=1 rednavi_doorbell_s2a=1`):** the MES runs. `mes-start` `step` 5, `hw_res_ok` 1, `add_ok` 1, `q_active` 1 (the legacy compute queue, doorbell control 0x60000018), `sched_active` 1, `kiq_active` 1, `ring_fence` 3, `api_status` 1, `fault_status` 0, `mepc` and `mcause` 0; the firmware reports scheduler version 0x1026096 and KIQ version 0x6038113 (`mes-fw-version`). The doorbell BAR is 2 MB (`doorbell-info` `length_kb` 0x800) and the aperture and S2A entries were off before the kext enabled them. No hang. Not yet run: a shader dispatch through this queue and the user client (`tools/rn_test.c`).

**First shader on the RX 7600 (2026-11-19, 4c boot-args plus `rednavi_uc_anyone=1`):** `rn_test` PASS (a compute dispatch through the user client: the shader stores 0x600dc0de, the fence is reached) and `rn_test vm` PASS (VMID 1 address space, a buffer mapped at two addresses, the command buffer at 0x7ffffffe0000 storing to 0x400000001000, every refused map). `rn_test info` printed FAIL only because its expectations were those of the 7800 XT (device 0x747e, 3 SE, 60 CU, its firmware versions): the Navi 33 answers are the Linux capture's (device 0x7480, 2 SE x 2 SA, 32 CU, rb mask 0xff, clock 2250 MHz). The expectations are now per device. `MEMORY` reports 6189 MB of device-local VRAM, 77 MB CPU-visible and 2048 MB of GTT; `mall` is 0 MB (Linux's capture value to be checked). `rn_test` has the GC base 0x1260 in its packets, which is also the Navi 33 base. Not run: `rn_test` modes beyond these three, the Mesa bridge and Vulkan on this card.
