# M6: WindowServer composes on the GPU (2026-11-07)

State at the end of the day, measured on the RX 7800 XT desktop (macOS 15.8.2, 1920x1080). Everything below was run on that machine unless it says otherwise.

**Result.** WindowServer picks the Metal compositor for the display, CoreAnimation renders through the RedNaviMTL plug-in (RADV), CoreDisplay's GPU display pipe runs its pass and hands the frame to the accelerator's display pipe, which copies it into the scanout. The user saw an accelerated desktop with transparency. It is not stable yet: WindowServer crashes within seconds of moving the mouse (see "Open"), and the frames glitch.

The chain of causes, in the order they were found. Each step was a reason the screen stayed black or the compositor stayed on the CPU. The reference for what is asked of a driver is the shipped macOS code (symbols and disassembly of what the Mac itself ships) and facts passed on from a study of another driver; no code was copied.

## 1. WindowServer chose the software compositor

- `SkyLight` decides once per process, at first use, whether Metal can composite: `WSSystemCanCompositeWithMetal` enumerates `MTLCopyAllDevices` and sets `metal_lovin` if one device answers NO to `-isHeadless`. WindowServer starts at boot, before the accelerator registers (about 90 s later with `rednavi_accel2_delay=90`), so it saw no device and stayed on `CompositorSW` for its whole life. After a manual restart of WindowServer (`killall -9 WindowServer`) the check passes. A permanent fix is to keep the I/O registry busy until the display is armed (the reference driver does this with `adjustBusy`), not done yet.
- `-[MTLIOAccelDevice isHeadless]` is bit 11 of `_configBits`, which comes from the accelerator's answer to the device client (selector 2, `IOAccelDeviceGetConfig64`). Ours is 0, so NO: that was not the blocker. `configIsHeadless()` of the family was never called.

## 2. The display surface had no IOSurface

- `CDDisplay::begin_update` puts the software compositor in the update when the display surface has no IOSurface (byte `+0x40` of the `CDSurface`, set by `CoreDisplay_DisplaySurface_GetIOSurface == NULL`). CoreDisplay hands out IOSurface-backed surfaces only if `CoreDisplay::UseIOPresentment()` is true.
- `UseIOPresentment` needs the user default `useIOP` and a vendor mask: the vendor of each `IOFramebuffer` is read from the substring of its **class name** (`Intel`, `AMD`, `AppleParavirt`, `NVDA`; read from CoreDisplay's strings). The framebuffer class was `RedNaviFB`, an unknown vendor, so the answer was NO. Now `AMDRedNaviFB` (`RedNaviFB/kern_fb.cpp`). Without the rename nothing below is reachable.
- CoreDisplay also asks the graphics control (`RedNaviGraphicsControl`, attached to the PCI device) for command 0x921 (link configuration of a head, 0xb0 bytes, timing in the layout of `IODetailedTimingInformationV2` from byte 12) and 0x711 (extended capability, type 0x10, 0x196c bytes). Both are answered from the probe's new property `fb-timing`. 0x925, 0x2001 and 0x941 are still refused and nothing breaks.

## 3. The transaction was refused by the family

`IOAccelDisplayPipeUserClient2::transactionEnd` returned an error before our pipe was reached. Found with `fbt` return probes (`tools/k_notready.sh`, `tools/k_txret.sh`, `tools/k_txend.sh`) and the disassembly of the function:

| Return | Cause | Fix |
|---|---|---|
| `kIOReturnNotReady` 0xe00002d8 | `IOAccelDisplayPipe::isActive()` (byte `pipe+0x298`) was 0: the table of scanout memories held 8 entries, the pipe is rebuilt on every display change (35 times in 10 minutes), later scanout maps were not recognised | ring of 64 |
| `kIOReturnNoMemory` 0xe00002bd | `newSysMemory` returned NULL: `IOAccelSysMemory` is declared abstract in the family | `RedNaviSysMemory`, a concrete subclass that overrides nothing (the shipped AMD one overrides only `init` and `free`) |
| `kIOReturnNoResources` 0xe00002be | `IOAccelMemoryMap::prepare()` false because our `commitIntoGPUPageTable` said false for non-scanout maps, and `IOAccelVidMemory::wire` failed because the stub's `allocPhysical` returned 0 | commit and update say true for every map; `allocPhysical` and `deallocPhysical` say success (nothing is allocated: the CPU reads the surface in `performTransaction`) |

After these, `performTransaction` runs about 10 times a second with a 1920x1080 BGRA surface and copies it into the scanout.

## 4. CoreDisplay's GPU pipe needs Metal features the plug-in lacked

- `RunFullDisplayPipe` asserts when `m_combinedLUTTexture` is nil. It is a `R32Float` 1D array texture (16384 x 3 or 6). The plug-in accepted only 2D and 3D. 1D and 1D array textures are now one image row per slice (`RedNaviMTL/compute.m`), blits with a slice move the row.
- Command buffers: `-commitAndWaitUntilSubmitted` is the private commit that SkyLight's `MetalContext::Flush` sends (found by counting the selectors sent inside `Flush` with `tools/ws_flush.sh` and decoding them with `tools/ws_msgs_decode.py`). It was not implemented, so every command buffer of CoreAnimation was enqueued, filled and released without ever running (the plug-in now logs "command buffer released without commit"). `commitAndWaitUntilSubmitted`, `...Scheduled` and `...Completed` now call `commit`, which is synchronous here. This was the last step to a non-black screen.

## Installing kexts on this machine (lessons)

- `rn_install.sh` only calls `kmutil load -p`, which loads the kext for the running kernel. The auxiliary kernel collection (`/Library/KernelCollections/AuxiliaryKernelExtensions.kc`) is rebuilt on the next boot only after the user approves the changed extension in System Settings (Privacy & Security). Until then the previous build keeps loading. Check the collection's date and a string of the new build in it (`grep -a`), and the size of the loaded kext in `kmutil showloaded`, before trusting a test.
- The script leaves a kext alone when `/Library/Extensions` already holds an identical copy. To force the rebuild remove the kext first.
- The Makefile did not list `stubs.inc` as a dependency of `RedNaviAccel2`, so an edit of that file was built into nothing; fixed.
- A WindowServer that cannot restart (assertion, crash loop) makes the userspace watchdog panic the kernel after 120 s. Two panics of 2026-11-07 were that, not the kexts.

## Open (not fixed yet)

- WindowServer crashes a few seconds after the first mouse movement: `memmove` in `+[RedNaviMTLBuffer syncNoCopyIn:]`, called from `commitBody` (SIGSEGV once, SIGBUS once). It is the copy of buffers made over the caller's memory (`newBufferWithBytesNoCopy`) when the memory has gone.
- Visible glitches on the first frames, and the frame rate is low: `performTransaction` and the plug-in copy whole surfaces on the CPU every time; each command buffer waits on RADV fences.
- The compositor choice is still made at boot with no device (restart WindowServer by hand once the accelerator is attached).
- `performTransaction` does not wait for the GPU to finish the surface (the reference driver waits up to 100 ms for a per-surface ready flag); it happens to be right because `commit` is synchronous.
- `+0x08 = 0x30844300` of the accelerator configuration (shared events, bit 14) was not applied.
- Video decode (no IOGVA* or VA driver) and AGDP are not implemented.

## Tools added

`tools/ws_*.sh`, `tools/k_*.sh`: dtrace scripts, run as root on the desktop, each a single question (comments at the top say what and how to run). `tools/ws_msgs_decode.py` turns selector addresses into names on the same boot. `tools/rn_iopmap.c` calls `IOPresentmentGetMap`. `tools/rn_peek*.c` read the scanout through the probe's user client: heavy, they stalled the desktop's network once; do not run them while the machine is in use. `scripts/rn_install.sh` and `scripts/rn_install_plugin.sh` install the kexts and the plug-in executable from `/Users/Shared`.
