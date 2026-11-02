# The virtual interface of IOAcceleratorFamily2, from symbol tables

Generated 2026-11-02 by `tools/accel_vtable.py` on macOS 15.7.3 (kernel 24.6.0, `IOAcceleratorFamily2` 485), from the data and symbol names of `/System/Library/KernelCollections/SystemKernelExtensions.kc` and the kernel. Nothing was disassembled (see the method note in the tool). This is the information a third-party subclass needs to line up with the family's C++ ABI; the object sizes are the other half and come from the run-time probe of `RedNaviAccel` (docs/M6_ACCELERATOR_STUDY.md, section 8). Parameter types are as the compiler mangled them; the structures behind `IOAccelConfig`, `IOAccelEvent`, ... are not known beyond their names.

Slot numbers count virtual functions in vtable order, from 0 after the two leading zero words. IOService has 267 slots; `IOAccelerator` (IOGraphicsFamily) adds none, so a derived class's own virtuals start at slot 267.

## Sizes of the tables

```
IOGraphicsAccelerator2       351 slots, 17 pure virtual
IOAccelDevice2               310 slots, 0 pure virtual
IOAccelSharedUserClient2     309 slots, 0 pure virtual
IOAccelCommandQueue          338 slots, 0 pure virtual
IOAccelSurface               328 slots, 5 pure virtual
IOAccel2DContext2            369 slots, 2 pure virtual
IOAccelDisplayPipe           307 slots, 0 pure virtual
IOAccelContext2              361 slots, 0 pure virtual
IOAccelResource2             80 slots, 7 pure virtual
IOAccelShared2               46 slots, 0 pure virtual
IOAccelChannel2              53 slots, 3 pure virtual
IOAccelMemory                61 slots, 11 pure virtual
IOAccelEventMachine2         85 slots, 34 pure virtual
IOAccelSubmitter2            317 slots, 5 pure virtual
```

## IOGraphicsAccelerator2: the 84 virtuals it adds to IOService (slots 267 to 350 of the full table; 17 are pure virtual)

The pure ones are named by what `IntelAccelerator` puts there (AMD's X6000 accelerator fills the same slots).

| slot | method | |
|---|---|---|
| 0 | `IOGraphicsAccelerator2::acceleratorWillUnlock(char const*, int)` |  |
| 1 | `IOGraphicsAccelerator2::tmpTotalVRAM()` |  |
| 2 | `IOGraphicsAccelerator2::totalTextureMemory()` |  |
| 3 | `IOGraphicsAccelerator2::isIdle()` |  |
| 4 | `IntelAccelerator::getStampMemory(unsigned int*)` | **pure** |
| 5 | `IOGraphicsAccelerator2::createEventMachine()` |  |
| 6 | `IntelAccelerator::newEventMachine()` | **pure** |
| 7 | `IOGraphicsAccelerator2::newClientSharedRO()` |  |
| 8 | `IOGraphicsAccelerator2::newClientSharedRW()` |  |
| 9 | `IOGraphicsAccelerator2::deleteClientSharedRO(IOAccelClientSharedRO*)` |  |
| 10 | `IOGraphicsAccelerator2::deleteClientSharedRW(IOAccelClientSharedRW*)` |  |
| 11 | `IOGraphicsAccelerator2::createShared(task*)` |  |
| 12 | `IOGraphicsAccelerator2::createSysMemory()` |  |
| 13 | `IOGraphicsAccelerator2::createVidMemory(IOAccelShared2*, IOAccelResource2*, unsigned long long, void*)` |  |
| 14 | `IOGraphicsAccelerator2::createMemoryMap(IOAccelTask*, IOAccelMemory*, unsigned int)` |  |
| 15 | `IOGraphicsAccelerator2::createResource(IOAccelShared2*, unsigned int)` |  |
| 16 | `IOGraphicsAccelerator2::createCommandDescriptor()` |  |
| 17 | `IOGraphicsAccelerator2::deleteCommandDescriptor(IOAccelCommandDescriptor*)` |  |
| 18 | `IOGraphicsAccelerator2::newBlockFence()` |  |
| 19 | `IOGraphicsAccelerator2::scrubEvents()` |  |
| 20 | `IOGraphicsAccelerator2::createDrawable(IOAccelSurface*, IOAccelShared2*, bool)` |  |
| 21 | `IOGraphicsAccelerator2::resourceTypeForIOSurface(IOSurface*)` |  |
| 22 | `IOGraphicsAccelerator2::createDisplayPipe(IOFramebuffer*, unsigned int)` |  |
| 23 | `IOGraphicsAccelerator2::createMemoryDescriptorWithAddressRange(unsigned long long, unsigned long long, unsigned int, task*)` |  |
| 24 | `IOGraphicsAccelerator2::createMemoryDescriptorWithPersistentMemoryDescriptor(IOMemoryDescriptor*)` |  |
| 25 | `IOGraphicsAccelerator2::createBufferMemoryDescriptorWithOptions(unsigned int, unsigned long, unsigned long)` |  |
| 26 | `IOGraphicsAccelerator2::createBufferMemoryDescriptorInTaskWithOptions(task*, unsigned int, unsigned long, unsigned long)` |  |
| 27 | `IOGraphicsAccelerator2::freeAllGPUMappings()` |  |
| 28 | `IOGraphicsAccelerator2::freeWaitToPrepareVidMemory(IOAccelVidMemory*, bool, bool)` |  |
| 29 | `IOGraphicsAccelerator2::freeWaitToPrepareVidMap(IOAccelMemoryMap*, bool, bool)` |  |
| 30 | `IOGraphicsAccelerator2::unwireAllVidMemory()` |  |
| 31 | `IOGraphicsAccelerator2::freeAllVidMemoryMappings()` |  |
| 32 | `IOGraphicsAccelerator2::pageoffSurfaceInLinear()` |  |
| 33 | `IOGraphicsAccelerator2::freeWaitToPrepareSysMemory(IOAccelSysMemory*, bool)` |  |
| 34 | `IOGraphicsAccelerator2::freeWaitToPrepareSysMap(IOAccelMemoryMap*, bool)` |  |
| 35 | `IOGraphicsAccelerator2::collectGartWirings()` |  |
| 36 | `IOGraphicsAccelerator2::unwireAllSysMemory()` |  |
| 37 | `IOGraphicsAccelerator2::freeAllSysMemoryMappings()` |  |
| 38 | `IOGraphicsAccelerator2::getMaxSingleSysMemorySize()` |  |
| 39 | `IOGraphicsAccelerator2::getMaxWiredSysMemorySize()` |  |
| 40 | `IntelAccelerator::createUserGPUTask()` | **pure** |
| 41 | `IOGraphicsAccelerator2::acceleratorFinalize()` |  |
| 42 | `IOGraphicsAccelerator2::submitSwapCopy(IOAccelEvent*, IOAccelResource2*, IOAccelResource2*)` |  |
| 43 | `IntelAccelerator::populateAccelConfig(IOAccelConfig*)` | **pure** |
| 44 | `IOGraphicsAccelerator2::calcMaxGPUPhysicalMemoryBytes(unsigned long long)` |  |
| 45 | `IntelAccelerator::configureDevice(IOPCIDevice*)` | **pure** |
| 46 | `IntelAccelerator::teardownDevice(IOPCIDevice*)` | **pure** |
| 47 | `IntelAccelerator::createKernelGPUTask()` | **pure** |
| 48 | `IOGraphicsAccelerator2::systemWillSleep()` |  |
| 49 | `IOGraphicsAccelerator2::systemDidWake()` |  |
| 50 | `IOGraphicsAccelerator2::systemWillChangeSpeed()` |  |
| 51 | `IOGraphicsAccelerator2::systemDidChangeSpeed()` |  |
| 52 | `IOGraphicsAccelerator2::configIsHeadless()` |  |
| 53 | `IntelAccelerator::newDisplayMachine()` | **pure** |
| 54 | `IOGraphicsAccelerator2::newShared()` |  |
| 55 | `IOGraphicsAccelerator2::newSharedUserClient()` |  |
| 56 | `IOGraphicsAccelerator2::newDevice()` |  |
| 57 | `IntelAccelerator::newGLContext()` | **pure** |
| 58 | `IntelAccelerator::newCLContext()` | **pure** |
| 59 | `IntelAccelerator::newSurface()` | **pure** |
| 60 | `IntelAccelerator::new2DContext()` | **pure** |
| 61 | `IntelAccelerator::newVideoContext()` | **pure** |
| 62 | `IOGraphicsAccelerator2::newDisplayPipe()` |  |
| 63 | `IOGraphicsAccelerator2::newDrawable()` |  |
| 64 | `IOGraphicsAccelerator2::newContext(unsigned int)` |  |
| 65 | `IntelAccelerator::newSysMemory()` | **pure** |
| 66 | `IntelAccelerator::newVidMemory()` | **pure** |
| 67 | `IntelAccelerator::newResource()` | **pure** |
| 68 | `IOGraphicsAccelerator2::newCommandDescriptor()` |  |
| 69 | `IOGraphicsAccelerator2::newStatistics()` |  |
| 70 | `IntelAccelerator::newMemoryMap()` | **pure** |
| 71 | `IOGraphicsAccelerator2::getMaxResourceSize()` |  |
| 72 | `IOGraphicsAccelerator2::newRemoteMemory()` |  |
| 73 | `IOGraphicsAccelerator2::createRemoteMemory(IOAccelShared2*, unsigned long long, unsigned int, unsigned int)` |  |
| 74 | `IOGraphicsAccelerator2::newContextEventFence()` |  |
| 75 | `IOGraphicsAccelerator2::createIODMACommand()` |  |
| 76 | `IOGraphicsAccelerator2::triage(char**, unsigned long long*)` |  |
| 77 | `IOGraphicsAccelerator2::_RESERVEDIOGraphicsAccelerator3()` |  |
| 78 | `IOGraphicsAccelerator2::_RESERVEDIOGraphicsAccelerator4()` |  |
| 79 | `IOGraphicsAccelerator2::_RESERVEDIOGraphicsAccelerator5()` |  |
| 80 | `IOGraphicsAccelerator2::describeDriverAllocations(IOAccelAllocationInfo*)` |  |
| 81 | `IOGraphicsAccelerator2::newCommandQueue()` |  |
| 82 | `IOGraphicsAccelerator2::ktraceStateChanged()` |  |
| 83 | `?` |  |

## IOAccelSurface: pure virtual slots

| slot | method (as the shipped subclass implements it) |
|---|---|
| 300 | `IGAccelSurface::copyFromBuffer(int, int, int, int, unsigned int, unsigned int, IOAccelEvent*, IOAccelResource2*, IOAccelSysMemory*, unsigned int, unsigned long long, unsigned int)` |
| 301 | `IGAccelSurface::copyToBuffer(int, int, int, int, unsigned int, unsigned int, IOAccelEvent*, IOAccelResource2*, IOAccelSysMemory*, unsigned int, unsigned long long, unsigned int)` |
| 302 | `IGAccelSurface::submitSwapCopy(IOAccelEvent*, IOAccelResource2*, IOAccelResource2*)` |
| 306 | `IGAccelSurface::shapeSurface(unsigned int, unsigned short, unsigned short)` |
| 307 | `IGAccelSurface::getDirtyBufferBitsFromPrivateModeBits(unsigned long long)` |

## IOAccel2DContext2: pure virtual slots

| slot | method (as the shipped subclass implements it) |
|---|---|
| 360 | `IGAccel2DContext::blitCopy(IOAccelEvent*, IOAccelResource2*, IOAccelResource2*, IOAccel2DBlitRectStruc*, unsigned int)` |
| 361 | `IGAccel2DContext::blitFill(IOAccelEvent*, unsigned int, IOAccelResource2*, IOAccel2DBlitRectStruc*, unsigned int)` |

## IOAccelResource2: pure virtual slots

| slot | method (as the shipped subclass implements it) |
|---|---|
| 58 | `IGAccelResource::rebuildPagingBuffer()` |
| 60 | `IGAccelResource::getLevelOffset(unsigned char, unsigned char, int*)` |
| 61 | `IGAccelResource::getBackingLevelOffset(unsigned char, unsigned char, int*)` |
| 62 | `IGAccelResource::calculateIOSurfaceDeviceCacheVRAMBytes(unsigned long long*, unsigned long long*)` |
| 63 | `IGAccelResource::addToAperture()` |
| 64 | `IGAccelResource::removeFromAperture()` |
| 65 | `IGAccelResource::getApertureMemoryDescriptor(unsigned long long*)` |

## IOAccelEventMachine2: pure virtual slots

| slot | method (as the shipped subclass implements it) |
|---|---|
| 36 | `IOAccelEventMachineFast2::getClientTestEventFunc()` |
| 40 | `IOAccelEventMachineFast2::initEvent(IOAccelEvent*)` |
| 41 | `IOAccelEventMachineFast2::cleanEvent(IOAccelEvent*)` |
| 42 | `IOAccelEventMachineFast2::finishStamp(int)` |
| 43 | `IOAccelEventMachineFast2::finishAllStamps()` |
| 44 | `IOAccelEventMachineFast2::testStamp(int)` |
| 45 | `IOAccelEventMachineFast2::testAllStamps()` |
| 46 | `IOAccelEventMachineFast2::testAllStampsUnlocked()` |
| 47 | `IOAccelEventMachineFast2::finishEventUnlocked(IOAccelEvent*)` |
| 48 | `IOAccelEventMachineFast2::testEventUnlocked(IOAccelEvent*)` |
| 49 | `IOAccelEventMachineFast2::finishEvent(IOAccelEvent*)` |
| 50 | `IOAccelEventMachineFast2::testEvent(IOAccelEvent*)` |
| 51 | `IOAccelEventMachineFast2::finishEventExcluding(IOAccelEvent*, int)` |
| 52 | `IOAccelEventMachineFast2::testEventExcluding(IOAccelEvent*, int)` |
| 53 | `IOAccelEventMachineFast2::moveEvent(IOAccelEvent*, IOAccelEvent*)` |
| 54 | `IOAccelEventMachineFast2::copyEvent(IOAccelEvent*, IOAccelEvent*)` |
| 55 | `IOAccelEventMachineFast2::mergeEvent(IOAccelEvent*, IOAccelEvent*)` |
| 56 | `IOAccelEventMachineFast2::copyEventExcluding(IOAccelEvent*, IOAccelEvent*, int)` |
| 57 | `IOAccelEventMachineFast2::mergeEventExcluding(IOAccelEvent*, IOAccelEvent*, int)` |
| 58 | `IOAccelEventMachineFast2::setEventStamp(int, IOAccelEvent*)` |
| 59 | `IOAccelEventMachineFast2::incrementStamp(int)` |
| 60 | `IOAccelEventMachineFast2::writeStampCommand(int, IOAccelEventQueue*, vendevtCommandRec*)` |
| 61 | `IOAccelEventMachineFast2::writeEventStampCommand(int, IOAccelEventQueue*, IOAccelEvent*, vendevtCommandRec*)` |
| 62 | `IOAccelEventMachineFast2::writeEventBarrierCommand(IOAccelEventQueue*, IOAccelEvent*, vendevtBarrierRec*, int)` |
| 63 | `IOAccelEventMachineFast2::logCommandSubmission(int)` |
| 64 | `IOAccelEventMachineFast2::stampIsIncremented()` |
| 65 | `IOAccelEventMachineFast2::getStamp(int)` |
| 66 | `IOAccelEventMachineFast2::checkGPUProgress()` |
| 67 | `IOAccelEventMachineFast2::checkChannelProgress(int)` |
| 68 | `IGAccelEventMachine::eventTimeout(int)` |
| 75 | `IOAccelEventMachineFast2::enableEventStampInterrupts(IOAccelEvent const*)` |
| 76 | `IOAccelEventMachineFast2::disableEventStampInterrupts(IOAccelEvent const*)` |
| 78 | `IOAccelEventMachineFast2::scrubEvent(IOAccelEvent*)` |
| 79 | `IOAccelEventMachineFast2::triage(char**, unsigned long long*)` |

## What this says about the next step

- A subclass of `IOGraphicsAccelerator2` must implement 17 virtuals, all of them factories and hooks (`getStampMemory`, `newEventMachine`, `newSurface`, `new2DContext`, `newGLContext`, `newCLContext`, `newVideoContext`, `newSysMemory`, `newVidMemory`, `newResource`, `newMemoryMap`, `newDisplayMachine`, `createUserGPUTask`, `createKernelGPUTask`, `populateAccelConfig`, `configureDevice`, `teardownDevice`). Each factory returns an object of a family class that the driver subclasses in turn, so the subclasses of `IOAccelSurface` (5 pure), `IOAccel2DContext2` (2), `IOAccelResource2` (7), the memory classes (`IOAccelMemory` 11), the event machine (34, most already given by `IOAccelEventMachineFast2`) and the rest come with it; `IOAccelDevice2`, `IOAccelSharedUserClient2` and `IOAccelCommandQueue` have no pure virtual and can be used as they are.
- The shipped drivers also override family virtuals that are not pure and add some of their own (Intel's table has 353 slots against the family's 351, AMD's 366), so a first driver can start with the 17 and add overrides as the logger shows what is called.
- The layout of the data members is not in the tables: it needs the class sizes (run-time probe) and, for the members the family code touches in a subclass, trial. A subclass declared with the right size and the right virtual order, adding its own members after them, works whatever is in between, as long as the base class is only used through its own methods.
