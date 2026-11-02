# M6 step 2: what macOS expects of an accelerator (read-only study)

Status: study, 2026-11-02. Nothing here ran on the 7800 XT; no accelerator kext exists yet. Every statement is marked **V** (observed on a running Mac or in a shipped file, with the command that shows it) or **I** (inferred, not checked). The questions the study could not answer from public material are listed in section 6 together with a clean-room way to answer them.

## 1. Method, and what was not read

Sources used:

- **The development Mac** (macOS 15.7.3, Intel UHD 630 driven by Apple's own `AppleIntelKBLGraphics` stack, so a working accelerator is loaded and WindowServer is using it): `ioreg`, the `Info.plist` files of the shipped graphics kexts and Metal driver bundles in `/System/Library/Extensions`.
- **Apple's public SDK headers** (`IOKit.framework/Headers/graphics/IOAccel*.h`).
- **Symbol names** exported by `IOAcceleratorFamily2` in `/System/Library/KernelCollections/SystemKernelExtensions.kc` (`nm -gU`, names only; nothing was disassembled).
- **NullMoth's documentation** (`README.md`, `docs/HOW-IT-WORKS.md`, `docs/SLEEP-WAKE.md`, and the personality `kexts/NVRM/accel/Info.plist`, a data file) as an architectural reference.

Not read: the source of NullMoth's kexts and Metal plugin, including its `kexts/NVRM/accel/re/*.h` headers, which are reverse-engineered descriptions of Apple's private classes. They are under the PolyForm Noncommercial licence and this project's rule is clean-room (CLAUDE.md). What the study could not get without them is in section 6.

## 2. The stack on a Mac that has a working GPU (V)

`ioreg -r -c IntelAccelerator -l` on the development Mac:

```
IOPCIDevice (IGPU)
 +- AppleIntelFramebuffer x3        IOFramebuffer subclasses: IOAccelTypes = "IOService:/.../IGPU@2/IntelAccelerator", IOAccelIndex = 0
 +- IntelAccelerator                the accelerator (class chain: IntelAccelerator -> ... -> IOGraphicsAccelerator2 -> IOAccelerator, kext IOAcceleratorFamily2)
     user clients, created by processes (IOUserClientCreator), WindowServer being one of them:
       IGAccel2DContext, IOAccelDisplayPipeUserClient2   (three of each, created first, one pair per display)
       IGAccelDevice  (property API = "Metal"), IGAccelSharedUserClient
       IGAccelSurface (eleven), IGAccelCommandQueue (two)
```

WindowServer (pid 161 in that run) owns 21 of the accelerator's clients: 3 `IGAccel2DContext`, 3 `IOAccelDisplayPipeUserClient2`, 1 `IGAccelDevice`, 1 `IGAccelSharedUserClient`, 11 `IGAccelSurface`, 2 `IGAccelCommandQueue` (registry ids give the creation order: the 2D contexts and display pipes first, then the device and its shared client, then surfaces, then queues). Other processes (Finder, browsers, Dock, ...) open their own `IGAccelDevice` + `IGAccelSharedUserClient` pairs and command queues. The class names with the `IGAccel` prefix are Intel's subclasses of `IOAccelDevice2`, `IOAccelSharedUserClient2`, `IOAccelCommandQueue`, `IOAccelSurface`, `IOAccel2DContext2` (V: the family exports those base classes, see section 4).

The same pattern holds for Apple's other accelerators shipped with Sequoia: the AMD X6000 kext (`com.apple.kext.AMDRadeonX6000`, five personalities, Navi 12, 14 and 23 among them) depends on `IOAcceleratorFamily2`, `IOGraphicsFamily`, `IOPCIFamily` and `IOSurface` (V, Info.plist). The paravirtual GPU (`AppleParavirtGPUIOGPUFamily.kext`, used by virtual machines) is a second, newer model on `IOGPUFamily` with the match category `IOAcceleratorES` (V). A discrete AMD GPU on an Intel Mac therefore goes the `IOAcceleratorFamily2` way.

## 3. How the accelerator is found and matched (V)

Personality of the Intel accelerator (`AppleIntelKBLGraphics.kext`, personality `Gen7`) and of AMD (`AMDRadeonX6000.kext`, `AMDNavi23GraphicsAccelerator`), reduced to the keys that carry meaning:

| Key | Intel | AMD X6000 | Meaning |
|---|---|---|---|
| `IOClass` | `IntelAccelerator` | `AMDRadeonX6000_AMDNavi23GraphicsAccelerator` | the accelerator class, subclass of the family's `IOAccelerator` |
| `IOProviderClass` | `IOPCIDevice` | `IOPCIDevice` | attaches to the GPU's PCI node |
| `IOMatchCategory` | `IOAccelerator` | `IOAccelerator` | the category that makes `IOServiceMatching("IOAccelerator")` find it; also lets it coexist with the framebuffer driver on the same PCI node |
| `IOPCIPrimaryMatch` / `IOPCIMatch` / `IOPCIClassMatch` | device ids; class `0x03000000&0xff000000` | `0x73E31002 0x73FF1002 0x73E01002` | PCI matching |
| `IOPropertyMatch` | none | `{"LoadAccelerator": true}` | AMD only: the accelerator loads when the PCI node carries this property, which the AMD framebuffer driver is expected to set (I: not observed being set) |
| `IOProbeScore` | 1000 | | |
| `MetalPluginName` | `AppleIntelKBLGraphicsMTLDriver` | (bundle `AMDRadeonX6000MTLDriver`) | name of the Metal driver bundle in `/System/Library/Extensions` |
| `IOGLBundleName`, `IODVDBundleName` | GL driver, video-decode driver | `AMDRadeonVADriver2` | GL and video plug-ins; not needed for Metal-only |
| `IOAccelRevision`, `AccelCaps`, `IOAccelDisplayPipeCapabilities` | 2, 15, `{DisplayPipeSupported, TransactionsSupported}` | | set by the family or the subclass; see section 6 |
| `IOCFPlugInTypes` | `{"ACCF0000-...-000a2789904e": "IOAccelerator2D.plugin"}` | | the 2D (CGS) plug-in |
| `VRAM,totalMB`, `IOVARendererID` | 1536, 17301536 | | dynamic properties on the live node |

The Metal driver bundle (`AppleIntelKBLGraphicsMTLDriver.bundle`, `AMDRadeonX6000MTLDriver.bundle`) is an ordinary bundle: `NSPrincipalClass` names the Objective-C device class (`MTLIGAccelDevice` for Intel, `GFX10_MtlDevice` for AMD, `AppleParavirtDevice` for the paravirtual GPU, whose kext personality also carries `MetalPluginClassName`), `CFBundleExecutable` the dylib. The kext's `MetalPluginName` selects it. NullMoth's accelerator personality shows the same keys with a path in `MetalPluginName` (`../../../Library/GPUBundles/NVMTLDriver`, i.e. relative to `/System/Library/Extensions`, which lets a third-party bundle live in `/Library/GPUBundles`) and `MetalPluginClassName` (V: its `Info.plist`; I: that Sequoia resolves the path that way).

Link between framebuffer and accelerator (V, Intel): the framebuffer node has `IOAccelTypes` = the registry path of the accelerator and `IOAccelIndex` = 0. Who writes them is not known (section 6). On the test desktop today there is no `IOAccelerator` in the registry and no `IOAccelTypes` on `RedNaviFB` (V, `ioreg -c IOAccelerator` on the desktop), so WindowServer composites on the CPU, as the README says.

## 4. What the kernel family offers (V: exported symbol names)

`IOAcceleratorFamily2` (bundle version 485 on this macOS) exports 2641 functions. The base class is `IOGraphicsAccelerator2`; the others (each with the `IOAccel` prefix) are: `Device2`, `DeviceShmem`, `SharedUserClient2`, `Shared2`, `SharedNamespace2`, `Namespace`; `CommandQueue`, `Submitter2`, `Channel2`, `FIFOChannel2`, `CommandBufferPool2`, `Context2`, `2DContext2`, `CLContext2`, `GLContext2`, `VideoContext2`; `Resource2`, `Memory`, `MemoryMap`, `SysMemory`, `VidMemory`, `RemoteMemory`, `KernelMappedMemory`, `AllocationInfo`, `MemoryAllocator2`, `RangeAllocator`, `OrphanedMemoryPool`, `OrphanedVidMemoryPool`, `ResidentMemorySet`, `BufferMemoryDescriptorPool2`; `Surface`, `SurfaceMTL`, `Drawable2`, `GLDrawableUserClient`, `LegacySurface`; `DisplayPipe`, `DisplayPipeUserClient2`, `DisplayPipeTransaction2`, `DisplayPipeGammaTable2`, `DisplayMachine`, `LegacyDisplayPipe`; `EventMachine2`, `EventMachineFast2`, `EventFence`, `BlockFence`, `ExternalEvent`, `MTLEvent2`, `SharedEventFence`, `FenceMachine`; `Task`, `Statistics2`, `TraceStream2`, `GPURestartReport2`, `MemoryInfoUserClient`. These are the objects the Intel and AMD kexts subclass (`IGAccel*`, `AMDRadeonX6000_*`).

`IOGraphicsAccelerator2` exports, among its ~100 members, the virtual-looking hooks `createDisplayPipe`, `createDrawable`, `createEventMachine`, `createShared`, `createResource`, `createSysMemory`, `createVidMemory`, `createMemoryMap`, `createRemoteMemory`, `createCommandDescriptor`, `enableAccelerator`, `disableAccelerator`, `acceleratorDidLock`, `acceleratorWillUnlock`, `acceleratorWaitEnabled`, `configIsHeadless`, `calcMaxGPUPhysicalMemoryBytes`, `collectGartWirings`, `deviceCacheForIOSurface`, `finalize`. The full list is in the output of `nm -gU` on the collection (command in section 7). Which of them a driver must override, and in which vtable slot, the symbol names do not say.

## 5. Public client interface (V: SDK headers)

`IOKit/graphics/IOAccelClientConnect.h`: service class name `"IOAccelerator"`; public client types `kIOAccelSurfaceClientType` (0) and `kIOAccelNumClientTypes`; private types start after that (`kIOAccelSurface2ClientType = 0x20` exists outside open-source builds).

`IOAccelSurfaceConnect.h`: the legacy surface client methods, in order: `ReadLockOptions, ReadUnlockOptions, GetState, WriteLockOptions, WriteUnlockOptions, Read, SetShapeBacking, SetIDMode, SetScale, SetShape, Flush, QueryLock, ReadLock, ReadUnlock, WriteLock, WriteUnlock, Control, SetShapeBackingAndLength`; the surface mode bits (colour depths `8888` = 4, `BGRA32` = 0xA, `2101010` = 0xF, windowed bit 0x20, `Surface2` 0x4000, beam sync 0x8000), shape bits, scale bits, lock bits, `IOAccelDeviceRegion` (rect list). `IOAccelTypes.h`: bounds and key types. This is the old interface; the live registry shows WindowServer using the newer `IOAccelDevice2`/`SharedUserClient2`/`CommandQueue`/`Surface` set, whose selectors and structures are private.

## 6. What is not known, and how to find it without copying anyone's work

1. **The private user-client protocol**: the selectors and argument structures of `IOAccelSharedUserClient2`, `IOAccelDevice2`, `IOAccelCommandQueue`, `IOAccelSurface`, `IOAccelDisplayPipeUserClient2` and `IOAccel2DContext2`, what WindowServer sends at boot, in which order, and what it expects back (memory types mapped, notification ports, shared-memory page layouts, `IOAccelRevision` semantics). Not in public headers, not in the symbol names.
2. **The C++ ABI of the family**: object sizes and virtual-table slots of `IOGraphicsAccelerator2` and friends, needed to subclass them from a third-party kext. The header files are private. Class sizes can be asked at run time (`OSMetaClass::getMetaClassWithName(...)->getClassSize()`); the order of virtuals can be read from the vtable data (`__ZTV...`) against the exported symbols, which is reading metadata, not code; neither has been tried.
3. **What the Metal framework asks of the plug-in bundle**: the abstract class behind `NSPrincipalClass` (`MTLIOAccelDevice`, I) and the methods it requires.
4. **Who sets `IOAccelTypes` / `IOAccelIndex`** on the framebuffer and what in `IOFramebuffer` depends on an accelerator (IOGraphics is open source and can be read for this).
5. **The policy layer**: `AppleGraphicsControl.kext` has plug-ins (`AppleGraphicsDevicePolicy`, `AppleGPUWrangler`, `AGDCPluginDisplayMetrics`, mux and power control) that exist on this Mac (V); the policy keys by device-tree names (`GFX0`, `IGPU`, `EDID`, `FeatureControl`) (V). NullMoth ships its own policy shim (NVRMAGDC); whether a desktop with OpenCore needs one here, and what it must answer, is unknown (I).
6. **System settings**: NullMoth's documentation says WindowServer can only load a third-party driver bundle with AMFI relaxed (`amfi_get_out_of_my_way=0x1 amfi=0x80 ipc_control_port_options=0` in boot-args) and SIP in a value of its choosing, that the kexts must load from the Auxiliary Kernel Collection (they link IOGraphicsFamily/IOAcceleratorFamily2, which only exist in Apple's own collections; same reason as `RedNaviFB`), and that the firmware framebuffer's `IONDRVSupport` must be blocked so the real framebuffer gets display index 0. This project already does the AuxKC part for `RedNaviFB`; the AMFI part is untested here (I).

A clean-room route to 1 to 3 that needs no one else's code: build a **logging accelerator** kext. It subclasses the family classes only as far as the layout derived in step 2 allows, registers with the personality of section 3, and for every user-client entry (`externalMethod`, `clientMemoryForType`, `registerNotificationPort`) logs the selector, the input and output sizes and the first words of the input, returning an error. WindowServer and the Metal framework then show on their own what they send first. Run on the test desktop (no second GPU: a failed accelerator leaves WindowServer on the CPU path it already uses), this produces the table that section 6.1 lacks, from observation only. The prerequisite is 6.2.

## 7. Hooks for RedNavi, in the order they are needed

| # | Hook | Where | State |
|---|---|---|---|
| H1 | Accelerator personality: `IOClass` RedNaviAccel, `IOProviderClass` IOPCIDevice, `IOPCIPrimaryMatch` `0x747E1002`, `IOMatchCategory` `IOAccelerator`, `IOProbeScore`, `MetalPluginName`, `MetalPluginClassName` | new kext, linked against IOAcceleratorFamily2, IOGraphicsFamily, IOPCIFamily, IOSurface (what Apple's and NullMoth's kexts list) | not written |
| H2 | Install path | AuxKC like `RedNaviFB` (`docs/DP_BRINGUP.md`), because the family is not in the boot collection | known to work for `RedNaviFB` |
| H3 | Subclass of `IOGraphicsAccelerator2` and its device, shared client, command queue, surface, display pipe classes | the new kext | needs 6.2 |
| H4 | Bridge to the existing user client: the accelerator's command-queue and memory methods drive `RedNaviClient`-style submit, VRAM allocation and VM mapping (`RN_UC_*`), instead of the NVIDIA RM calls NullMoth makes | `RedNavi/kern_probe.cpp` | the engine side exists (M4) |
| H5 | Framebuffer link: `IOAccelTypes` / `IOAccelIndex` on `RedNaviFB` | `RedNaviFB/kern_fb.cpp` | needs 6.4 |
| H6 | Metal driver bundle in `/Library/GPUBundles`: `NSPrincipalClass`, device/queue/buffer/texture/pipeline on RADV through the Vulkan API, AIR to SPIR-V translation (metal2vulkan, LGPL, as a separate component) | new, user space | needs 6.3 |
| H7 | Policy (AGDC) answers if the system asks for them | new kext or plug-in | unknown, needs 6.5 |
| H8 | boot-args and SIP for loading third-party driver bundles | OpenCore config | needs testing on the desktop |

Commands that reproduce the observations:

```
ioreg -r -c IntelAccelerator -l                      # classes, properties, user clients and their creators
ioreg -a -r -c IntelAccelerator -l | python3 ...     # the WindowServer client count by class (plist parse)
plutil -convert json -o - /System/Library/Extensions/AppleIntelKBLGraphics.kext/Contents/Info.plist
plutil -convert json -o - /System/Library/Extensions/AMDRadeonX6000.kext/Contents/Info.plist
plutil -convert json -o - /System/Library/Extensions/AppleIntelKBLGraphicsMTLDriver.bundle/Contents/Info.plist
nm -gU -arch x86_64 /System/Library/KernelCollections/SystemKernelExtensions.kc | c++filt   # section "Symbols for com.apple.iokit.IOAcceleratorFamily2"
```
