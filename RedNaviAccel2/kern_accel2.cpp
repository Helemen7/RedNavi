// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// RedNaviAccel2: second attempt at the M6 accelerator (docs/M6_ACCELERATOR_STUDY.md). RedNaviAccel answers the device client call by call and gets nowhere; this kext
// instead lets the family run: it subclasses IOGraphicsAccelerator2 and does NOT override start(), stop(), free(), requestProbe(), message() or newUserClient(), so the
// family's own code runs them (the slot table says it implements all six). What the family needs from the subclass are its 17 pure virtuals; this file implements them.
//
// It is opt-in: probe() refuses unless boot-arg rednavi_accel2 is set, and then RedNaviAccel (probe score 100) is not used for the card because this one scores 200 in the
// same match category. The value of the boot-arg is the stage:
//   1  every hook is logged and configureDevice() fails, so the family's start() stops at its first call into us and nothing else runs (the safe stage: shows what the
//      family calls, in which order, with which arguments);
//   2  configureDevice() and teardownDevice() succeed (they return bool: the family logs "configureDevice failed" on a returned kIOReturnSuccess, which is 0; the first
//      version declared them IOReturn), populateAccelConfig() succeeds without writing (the size of IOAccelConfig is not known; its first bytes are dumped
//      before and after), and the factories hand out instances of the family's own concrete classes by name when the class exists and is not abstract, else NULL.
// The log is the property rednavi-accel2-log of the PCI device (the provider outlives this object when start() fails, and IOLog is not reliably visible), last 256 lines.
// Clean-room: written from the exported symbol tables and the registry of this Mac, no code of the family or of NullMoth.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/graphics/IOAccelerator.h>
#include "../RedNaviAccel/IOGraphicsAccelerator2.h"
#include "IOAccelEventMachineFast2.h"
#include "IOAccelDisplayMachine.h"
#include "IOAccelDisplayPipe.h"
#include "IOAccelClasses.h"
#include <IOKit/IORangeAllocator.h>
#include "IOAccelTask.h"
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <stdarg.h>

static_assert(sizeof(IOGraphicsAccelerator2) == 3544, "the family class is 3544 bytes in the registry (docs/M6_ACCEL_ABI.md)");

static IOLock *gLogLock;
static OSArray *gLog;          // private; a copy is published (a collection that is a registry property is frozen)
static uint32_t gStage;

static IOService *gProv;
class RedNaviAccel2;
static class RedNaviDisplayMachine *gMachine;   // the display machine the family was given, for the power-on notification
static IOFramebuffer *gFb;    // the framebuffer createDisplayPipe was given, retained, for found_framebuffer
static IOService *gSelf;     // the accelerator, retained, for the delayed registerService()       // the PCI device, set in probe(); it outlives every object of this kext

// Alignment with the reference driver, switchable without reinstalling: boot-arg rednavi_accel2_exp=<bit mask>. Bit 0: the first megabyte of every GPU address space is reserved (the
// reference treats address 0 as "allocation failed"), bit 1: IOAccelResource2's seven pure virtuals answer instead of returning 0, bit 2: (free), bit 3: the event machine's progress
// checks say "fine" and its timeout returns nothing, bit 4: populateAccelConfig fills the configuration as the reference does, bit 5: IOAccelDisplayPipeCapabilities is published after the
// display bring-up, not in start(). Bit 6: also IOGLBundleName and VRAM,totalMB.
static uint32_t accelExp() {
    static uint32_t v, read;
    if (!read) { read = 1; PE_parse_boot_argn("rednavi_accel2_exp", &v, sizeof(v)); }
    return v;
}

// A line of the log, to IOLog and to the provider's property. Safe from any thread that may sleep.
static void rnNote(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void rnNote(const char *fmt, ...) {
    char line[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    IOLog("RedNaviAccel2: %s\n", line);
    if (!gLog || !gLogLock || !gProv) return;
    OSString *s = OSString::withCString(line);
    if (!s) return;
    IOLockLock(gLogLock);
    if (gLog->getCount() >= 600) gLog->removeObject(0);
    gLog->setObject(s);
    if (OSArray *snapshot = OSArray::withArray(gLog)) {
        gProv->setProperty("rednavi-accel2-log", snapshot);
        snapshot->release();
    }
    IOLockUnlock(gLogLock);
    s->release();
}

// The words the display bring-up reads: accelerator +0xc78 (bit 0x02 enabled) and +0xcb0 (framebuffer ceiling), machine +0x108 count and +0x10c mode-change nesting,
// pipe +0x282 (WSAA defer flag) and +0x298 (active). Offsets are measured on macOS 15.8.2 (24.6 kernels) from the family's code by a study of another driver: unverified here
// until the log shows them change.
static void rnFlags(const char *when, IOService *acc, IOAccelDisplayMachine *m) {
    const uint8_t *a = reinterpret_cast<const uint8_t *>(acc), *mb = reinterpret_cast<const uint8_t *>(m);
    IOAccelDisplayPipe *p = m ? m->getDisplayPipe(0) : nullptr;
    const uint8_t *pb = reinterpret_cast<const uint8_t *>(p);
    rnNote("flags %s: accel+0xc78 %02x, +0xcb0 %u; machine+0x108 %u, +0x10c %u; pipe+0x282 %02x, +0x298 %02x; active(0) %d", when,
           a[0xc78], *reinterpret_cast<const uint32_t *>(a + 0xcb0), *reinterpret_cast<const uint32_t *>(mb + 0x108), *reinterpret_cast<const uint32_t *>(mb + 0x10c),
           pb ? pb[0x282] : 0xff, pb ? pb[0x298] : 0xff, m ? (int)m->isFramebufferActive(0) : -1);
}

static void rnDumpWords(const char *what, const void *p, uint32_t bytes) {
    const uint32_t *w = static_cast<const uint32_t *>(p);
    for (uint32_t i = 0; i < bytes / 4; i += 8) {
        char s[96];
        int len = 0;
        for (uint32_t j = i; j < i + 8 && j < bytes / 4; j++) len += snprintf(s + len, sizeof(s) - (size_t)len, "%s%08x", j > i ? " " : "", w[j]);
        rnNote("%s +0x%x: %s", what, i * 4, s);
    }
}

// Stub subclasses of the abstract memory, resource, context and surface classes (generated by tools/accel_vtable.py --header-chain).
#include "stubs.inc"

// The stamps the event machine reads to learn how far the engines got. Two panics fixed the shape of this:
//  - NULL: IOAccelEventMachineFast2::checkGPUProgress read a null base address (the machine's init has to hand it over with setStampBaseAddress);
//  - raw memory from getStampMemory(): IOAccelDevice2::deviceStart + 0x32 read the first word of the returned pointer as a vtable and called slot 4 (OSObject::retain)
//    through it (RDI was the stamp page, RCX = 0, CR2 = 0x20), so getStampMemory() returns an OSObject, in all likelihood an IOMemoryDescriptor that the device maps into
//    its client.
// So: one zeroed 64 KB buffer descriptor (the event machine is initialised for 1000 stamps of 64 bytes, and with 4096 bytes IOAccelDeviceCreateWithAPIProperty in
// user space faulted one page past the mapped memory; the Intel accelerator's 64-byte slots are in docs/logs/intel_device_client_selector2.txt),
// its address for the event machine and the descriptor itself for getStampMemory(). Nothing writes stamps yet, so every channel looks idle.
static uint32_t rnStampSize() {
    uint32_t v = 0x10000;       // rednavi_accel2_stampsize=<hex>: length of the buffer (the experiment: does the fault move with the end?)
    PE_parse_boot_argn("rednavi_accel2_stampsize", &v, sizeof(v));
    return v;
}
static IOBufferMemoryDescriptor *rnStampDesc() {
    static IOBufferMemoryDescriptor *d;
    if (!d) {
        d = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, rnStampSize(), 0x1000);
        if (d) {
            bzero(d->getBytesNoCopy(), rnStampSize());
            // Experiment knobs: IOAccelDeviceCreateWithAPIProperty scans this memory and falls off its end when it is all zero (the fault is exactly at the end of the
            // mapping, with 4 KB and with 64 KB), so something nonzero is expected in it. rednavi_accel2_stampfill=<hex> fills every 32-bit word, rednavi_accel2_stampslot=<hex>
            // sets the first word of every 64-byte slot (applied after the fill).
            uint32_t *w = static_cast<uint32_t *>(d->getBytesNoCopy());
            uint32_t v = 0;
            if (PE_parse_boot_argn("rednavi_accel2_stampfill", &v, sizeof(v))) for (unsigned i = 0; i < rnStampSize() / 4; i++) w[i] = v;
            if (PE_parse_boot_argn("rednavi_accel2_stampslot", &v, sizeof(v))) for (unsigned i = 0; i < rnStampSize() / 4; i += 16) w[i] = v;
        }
    }
    return d;
}
static void *rnStampPage() {
    IOBufferMemoryDescriptor *d = rnStampDesc();
    return d ? d->getBytesNoCopy() : nullptr;
}

// The event machine: the family's IOAccelEventMachineFast2 implements all but four of its virtuals (the ones that write a stamp or a barrier into the command stream of
// the hardware engine); those four are ours. In this first version they log and do nothing, so the family can build and initialise the object.
class RedNaviEventMachine : public IOAccelEventMachineFast2 {
    OSDeclareDefaultStructors(RedNaviEventMachine)
public:
    bool init(IOGraphicsAccelerator2 *, unsigned int, int) override;
    unsigned int getStampOffset(int) override;
    long writeStamp(int, vendevtCommandRec *, unsigned int) override;
    long prepareBarrier(vendevtBarrierRec *) override;
    long completeBarrier(vendevtBarrierRec *) override;
    long writeBarrierElement(vendevtBarrierRec *, int, unsigned int) override;
    bool checkGPUProgress() override;
    bool checkChannelProgress(int) override;
    void *eventTimeout(int) override;
    virtual void rnPad0(); virtual void rnPad1(); virtual void rnPad2(); virtual void rnPad3();
};
static_assert(sizeof(RedNaviEventMachine) == 3376, "IOAccelEventMachineFast2 is 3376 bytes in the registry");
OSDefineMetaClassAndStructors(RedNaviEventMachine, IOAccelEventMachineFast2)
void RedNaviEventMachine::rnPad0() {} void RedNaviEventMachine::rnPad1() {} void RedNaviEventMachine::rnPad2() {} void RedNaviEventMachine::rnPad3() {}
// Progress checks (bit 3): the family's own run on an engine that never reports a stamp, and say "GPU stuck"; the reference driver says everything is fine and has no timeout action.
bool RedNaviEventMachine::checkGPUProgress() { return (accelExp() & 8) ? true : IOAccelEventMachineFast2::checkGPUProgress(); }
bool RedNaviEventMachine::checkChannelProgress(int c) { return (accelExp() & 8) ? true : IOAccelEventMachineFast2::checkChannelProgress(c); }
void *RedNaviEventMachine::eventTimeout(int c) { return (accelExp() & 8) ? nullptr : IOAccelEventMachineFast2::eventTimeout(c); }
bool RedNaviEventMachine::init(IOGraphicsAccelerator2 *a, unsigned int n, int m) {
    // Experiment knobs (internal EFI boot-args, no reinstall): the arguments the family passes (1000, 1) against what Intel's reply of selector 2 suggests (0x40 at +0x14).
    uint32_t arg = 0;
    // m is the number of stamps the device client lists (the table of selector 2 has that many offsets): the family passes 1, Intel's reply has 64.
    unsigned int n2 = n;
    int m2 = 64;
    if (PE_parse_boot_argn("rednavi_accel2_em_n", &arg, sizeof(arg))) n2 = arg;
    if (PE_parse_boot_argn("rednavi_accel2_em_m", &arg, sizeof(arg))) m2 = static_cast<int>(arg);
    void *st = rnStampPage();
    if (PE_parse_boot_argn("rednavi_accel2_em_early", &arg, sizeof(arg)) && arg && st) setStampBaseAddress(static_cast<volatile unsigned int *>(st));   // before the family's init
    bool ok = IOAccelEventMachineFast2::init(a, n2, m2);
    rnNote("event machine init(%p, %u, %d) asked by the family, called with (%u, %d): family init %s, stamp page %p", (void *)a, n, m, n2, m2, ok ? "true" : "false", st);
    if (ok && st && !(PE_parse_boot_argn("rednavi_accel2_em_early", &arg, sizeof(arg)) && arg)) setStampBaseAddress(static_cast<volatile unsigned int *>(st));
    return ok;
}
// The offset of a channel's stamp in the stamp memory. Intel's subclass overrides this, the family's default leaves the table that selector 2 of the device client returns
// (at 0x5c) all zero: the Intel one holds 63 offsets, 0x40 to 0xfc0 in steps of 0x40 (docs/logs/intel_device_client_selector2.txt), and the framework's IOAccelDeviceCreate
// crashed with ours. Intel's table starts at 0x58 with 0 and goes 0x40, 0x80, ... so the offset is channel * 64 (the first version had (channel + 1) * 64, one slot
// off); 1000 stamps fit in the 64 KB buffer.
unsigned int RedNaviEventMachine::getStampOffset(int channel) {
    return channel >= 0 && channel < 1000 ? static_cast<unsigned int>(channel) * 0x40u : 0u;
}
long RedNaviEventMachine::writeStamp(int ch, vendevtCommandRec *rec, unsigned int v) { rnNote("event machine writeStamp(channel %d, %p, %u)", ch, (void *)rec, v); return 0; }
long RedNaviEventMachine::prepareBarrier(vendevtBarrierRec *b) { rnNote("event machine prepareBarrier(%p)", (void *)b); return 0; }
long RedNaviEventMachine::completeBarrier(vendevtBarrierRec *b) { rnNote("event machine completeBarrier(%p)", (void *)b); return 0; }
long RedNaviEventMachine::writeBarrierElement(vendevtBarrierRec *b, int i, unsigned int v) { rnNote("event machine writeBarrierElement(%p, %d, %u)", (void *)b, i, v); return 0; }

// The display machine (what the family does when a framebuffer changes mode or powers off): all of it is the family's except the two notifications that tell the
// hardware-specific part, which are ours. Logged and answered with true for now.
class RedNaviDisplayMachine : public IOAccelDisplayMachine {
    OSDeclareDefaultStructors(RedNaviDisplayMachine)
public:
    long displayModeWillChange() override;
    long displayModeDidChange() override;
    virtual void rnPad0(); virtual void rnPad1(); virtual void rnPad2(); virtual void rnPad3();
};
static_assert(sizeof(RedNaviDisplayMachine) == 376, "IOAccelDisplayMachine is 376 bytes in the registry");
OSDefineMetaClassAndStructors(RedNaviDisplayMachine, IOAccelDisplayMachine)
void RedNaviDisplayMachine::rnPad0() {} void RedNaviDisplayMachine::rnPad1() {} void RedNaviDisplayMachine::rnPad2() {} void RedNaviDisplayMachine::rnPad3() {}
// The family calls both as bool and panics on false ("display_mode_did_change: driver returns false", IOAccelDisplayMachine.cpp:345, seen 2026-11-04): true is success.
long RedNaviDisplayMachine::displayModeWillChange() { rnNote("display machine displayModeWillChange"); return 1; }
long RedNaviDisplayMachine::displayModeDidChange() {
    rnNote("display machine displayModeDidChange");
    if (gSelf) static_cast<IOGraphicsAccelerator2 *>(gSelf)->enableAccelerator();
    return 1;
}


// The family's display pipe client, as far as it is called from here: only the function the family exports as IOAccelDisplayPipeUserClient2::init(OSDictionary*, task*) (docs/M6_ACCEL_ABI.md). No
// vtable and no metaclass are emitted for this declaration; an instance comes from the family's metaclass by name.
class IOAccelDisplayPipeUserClient2 : public IOUserClient {
public:
    bool init(OSDictionary *, task_t);
};

// The memory behind the framebuffer resource of the display pipe: the live scanout surface the probe brings up (VRAM seen through BAR0), not a new allocation. The family's
// init_framebuffer_resource calls the pipe's initFramebufferResource (virtual) and takes the IOAccelMemory it returns; NULL makes it fail and leaves the pipe inactive
// (measured on the desktop: pipe+0x298 stays 0 and isFramebufferActive(0) is 0). Layout, from the family's IOAccelVidMemory::init (+0x40 total length, +0xc8 and +0xc0 zeroed
// there) and from a study of another driver (+0xc8 holds the physical address): measured by the dumps this class logs, unverified until they agree.
class RedNaviScanoutMemory : public IOAccelVidMemory {
    OSDeclareDefaultStructors(RedNaviScanoutMemory)
public:
    long getPhysicalSegment(unsigned long long offset, unsigned long long *length) override;
    long allocPhysical() override;
    long deallocPhysical() override;
    virtual void rnPad0(); virtual void rnPad1(); virtual void rnPad2(); virtual void rnPad3();
};
static_assert(sizeof(RedNaviScanoutMemory) == 272, "IOAccelVidMemory is 272 bytes in the registry");
OSDefineMetaClassAndStructors(RedNaviScanoutMemory, IOAccelVidMemory)
void RedNaviScanoutMemory::rnPad0() {} void RedNaviScanoutMemory::rnPad1() {} void RedNaviScanoutMemory::rnPad2() {} void RedNaviScanoutMemory::rnPad3() {}
long RedNaviScanoutMemory::getPhysicalSegment(unsigned long long offset, unsigned long long *length) {
    uint8_t *b = reinterpret_cast<uint8_t *>(this);
    uint64_t total = *reinterpret_cast<uint64_t *>(b + 0x40), phys = *reinterpret_cast<uint64_t *>(b + 0xc8);
    if (offset >= total) { if (length) *length = 0; return 0; }
    if (length) *length = total - offset;
    return static_cast<long>(phys + offset);
}
long RedNaviScanoutMemory::allocPhysical() { return 1; }     // nothing to allocate: the surface exists
long RedNaviScanoutMemory::deallocPhysical() { return 1; }   // and is not ours to free

// The family's IOAccelSysMemory is declared abstract (its allocator answers nothing, "newSysMemory ... -> 0 (abstract)" in the log), so IOAccelSysMemory::withMemoryDescriptor, which the
// transaction end uses to wrap the IOSurface of a plane, got no object and answered kIOReturnNoMemory (0xe00002bd). The shipped AMD class overrides only init and free; this one
// overrides nothing, the family's implementations do the work.
class RedNaviSysMemory : public IOAccelSysMemory {
    OSDeclareDefaultStructors(RedNaviSysMemory)
public:
    virtual void rnPadS0(); virtual void rnPadS1(); virtual void rnPadS2(); virtual void rnPadS3();
};
static_assert(sizeof(RedNaviSysMemory) == 408, "IOAccelSysMemory is 408 bytes in the registry");
OSDefineMetaClassAndStructors(RedNaviSysMemory, IOAccelSysMemory)
void RedNaviSysMemory::rnPadS0() {} void RedNaviSysMemory::rnPadS1() {} void RedNaviSysMemory::rnPadS2() {} void RedNaviSysMemory::rnPadS3() {}

// IOAccelResource2's seven pure virtuals. They answer from the resource's fields as the reference driver reads them (offsets: +0x88 the resource's memory, +0xae the base level, +0xb8 and
// +0xc0 the two pitches, +0xd0 the size; the fields of the memory are the ones RedNaviScanoutMemory fills). Without bit 1 of rednavi_accel2_exp they return 0 as the stubs did.
long RedNaviResource2::rebuildPagingBuffer() { return 0; }
long RedNaviResource2::getLevelOffset(unsigned char a0, unsigned char a1, int *a2) {
    if (!(accelExp() & 2)) return 0;
    const uint8_t *r = reinterpret_cast<const uint8_t *>(this);
    if (a2) { const uint8_t base = r[0xae]; *a2 = static_cast<int>(*reinterpret_cast<const uint32_t *>(r + 0xb8) >> (a1 >= base ? a1 - base : 0)); }
    return static_cast<long>(static_cast<uint64_t>(a0) * *reinterpret_cast<const uint64_t *>(r + 0xd0));
}
long RedNaviResource2::getBackingLevelOffset(unsigned char a0, unsigned char a1, int *a2) {
    if (!(accelExp() & 2)) return 0;
    const uint8_t *r = reinterpret_cast<const uint8_t *>(this);
    if (a2) { const uint8_t base = r[0xae]; *a2 = static_cast<int>(*reinterpret_cast<const uint32_t *>(r + 0xc0) >> (a1 >= base ? a1 - base : 0)); }
    return static_cast<long>(static_cast<uint64_t>(a0) * *reinterpret_cast<const uint64_t *>(r + 0xd0));
}
long RedNaviResource2::calculateIOSurfaceDeviceCacheVRAMBytes(unsigned long long *a0, unsigned long long *a1) {
    if (!(accelExp() & 2)) return 0;
    const uint64_t size = *reinterpret_cast<const uint64_t *>(reinterpret_cast<const uint8_t *>(this) + 0xd0);
    if (a0) *a0 = size;
    if (a1) *a1 = 1;
    return static_cast<long>(size);
}
long RedNaviResource2::addToAperture() {
    if (!(accelExp() & 2)) return 0;
    const uint8_t *vm = *reinterpret_cast<uint8_t *const *>(reinterpret_cast<const uint8_t *>(this) + 0x88);
    return vm && *reinterpret_cast<const uint64_t *>(vm + 0xc8) != 0 && *reinterpret_cast<const uint64_t *>(vm + 0x40) != 0 ? 1 : 0;
}
long RedNaviResource2::removeFromAperture() { return 0; }
long RedNaviResource2::getApertureMemoryDescriptor(unsigned long long *) { return 0; }

// The memory map's three pure virtuals. A map of the scanout memory reports success without writing a page table entry: the display engine reads the surface at its physical
// address, not through the GPU's page tables (which this driver does not drive from the family). IOAccelResource2::prepare() needs commitIntoGPUPageTable to say true (the
// stub's 0 made it fail, so the pipe's active byte was never set). Every other map keeps the old answer, false. Which maps are the scanout's is found by looking for one of
// the scanout memories among the words of the map object (the offset of the memory pointer is logged once).
// A ring: WindowServer's display set-up rebuilds the pipe (and with it the scanout memory) on every display change, 35 times in 10 minutes; a table that stopped taking new entries after 8
// left the later scanout maps unrecognised, the pipe never became active again and transactionEnd answered NotReady.
static const unsigned kScanoutRing = 64;
static RedNaviScanoutMemory *gScanout[kScanoutRing];
static unsigned gScanoutCount, gScanoutNext;
static bool isScanoutMap(const void *map, unsigned *at) {
    const uint64_t *w = static_cast<const uint64_t *>(map);
    for (unsigned i = 2; i < 35; i++)
        for (unsigned k = 0; k < gScanoutCount; k++)
            if (w[i] == reinterpret_cast<uint64_t>(gScanout[k])) { if (at) *at = i * 8; return true; }
    return false;
}
long RedNaviMemoryMap::commitIntoGPUPageTable() {
    unsigned at = 0;
    bool sc = isScanoutMap(this, &at);
    const uint64_t *w = reinterpret_cast<const uint64_t *>(this);
    static unsigned said;
    if (said++ < 40) rnNote("MemoryMap::commitIntoGPUPageTable(%p): %s (memory at +0x%x, task %llx, gpu address word +0x98 %llx)", (void *)this, sc ? "scanout, true" : "other map, true", at, w[18], w[19]);
    // Every map says true: the transaction end prepares the map of each plane's surface, and a false from here made IOAccelMemoryMap::prepare() fail, IOAccelGraphicsAccelerator2::
    // freeWaitToPrepareVidMap give up and transactionEnd answer kIOReturnNoResources (0xe00002be). Nothing here writes a GPU page table; the surfaces are read by the CPU copy of performTransaction.
    return 1;
}
long RedNaviMemoryMap::updateGPUPageTable() {
    bool sc = isScanoutMap(this, nullptr);
    static unsigned said;
    if (said++ < 40) rnNote("MemoryMap::updateGPUPageTable(%p): %s", (void *)this, sc ? "scanout, true" : "other map, true");
    return 1;
}
long RedNaviMemoryMap::releaseFromGPUPageTable() {
    rnNote("MemoryMap::releaseFromGPUPageTable(%p)", (void *)this);
    return 0;
}

// What the transaction object and the kernel's IOSurface offer, declared by their exported (non-virtual) functions: the planes of a transaction hold a surface or a resource
// per (plane, layer); neither class has a header here. Return types are not in the symbol names: these are the natural ones (unverified until the log shows sane values).
class IOAccelDisplayPipeTransaction2 : public OSObject {
public:
    void *getPlaneIOSurface(unsigned int plane, unsigned int layer) const;
    void *getPlaneResource(unsigned int plane, unsigned int layer) const;
    unsigned int getTransactionID() const;
    unsigned int getTransactionDirtyBits() const;
    unsigned int getTransactionOptions() const;
};
class IOSurface : public OSObject {
public:
    unsigned int getWidth() const;
    unsigned int getHeight() const;
    unsigned int getBytesPerRow() const;
    unsigned int getPixelFormat() const;
    IOMemoryDescriptor *getMemoryDescriptor() const;
};

// The graphics device control object of the card. Every graphics driver publishes a subclass of AppleGraphicsDeviceControl (Intel's is IntelFBClientControl) attached to its PCI
// device; Apple's AppleGraphicsDevicePolicy then attaches under it, and the window server asks it two things when it starts (or restarts): who the vendor is (command 1) and which
// framebuffers the GPU drives (command 0x980), building one display pipe per answer. On this desktop no such object exists (debug-gpus has "agdp" 0, no AppleGraphicsDevicePolicy
// node), the window server composes in software ("Unsupported sw composite destination") and sends the display nothing. The layout is that of macOS 15.8.2 from the exported symbols
// (the pure virtual is the variant with IOExternalMethodArguments, slot 266; the one with AGDCClientState_t, slot 267, is the base's; size 0x110) and from a study of another
// driver for the two answers, which are packed structures described below. Unverified on hardware until WindowServer's requests show in the log.
class AGDCClientState_t;
class AppleGraphicsDeviceControl : public IOService {
    OSDeclareAbstractStructors(AppleGraphicsDeviceControl)
public:
    uint8_t rn_pad_AppleGraphicsDeviceControl[272 - sizeof(IOService)];
    // The base overrides these five IOService virtuals (exported symbols): declared so that a subclass's vtable points at them and not at IOService's.
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn message(UInt32, IOService *, void *) APPLE_KEXT_OVERRIDE;
    bool terminate(IOOptionBits = 0) APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE;
    virtual IOReturn vendor_doDeviceAttribute(unsigned int, unsigned long *, unsigned long, unsigned long *, unsigned long *, IOExternalMethodArguments *) = 0;   // slot 266
    virtual IOReturn vendor_doDeviceAttribute(unsigned int, unsigned long *, unsigned long, unsigned long *, unsigned long *, AGDCClientState_t *);   // slot 267
    virtual void _RESERVEDAppleGraphicsDeviceControl0();   // slot 268
    virtual void _RESERVEDAppleGraphicsDeviceControl1();
    virtual void _RESERVEDAppleGraphicsDeviceControl2();
    virtual void _RESERVEDAppleGraphicsDeviceControl3();
    virtual void _RESERVEDAppleGraphicsDeviceControl4();
    virtual void _RESERVEDAppleGraphicsDeviceControl5();
    virtual void _RESERVEDAppleGraphicsDeviceControl6();
    virtual void _RESERVEDAppleGraphicsDeviceControl7();   // slot 275
};
static_assert(sizeof(IOService) < 272, "AppleGraphicsDeviceControl is 272 bytes in the registry");
class RedNaviGraphicsControl : public AppleGraphicsDeviceControl {
    OSDeclareDefaultStructors(RedNaviGraphicsControl)
public:
    IOReturn vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize, unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args) override;
};
static_assert(sizeof(RedNaviGraphicsControl) == 272, "the subclass adds no members");
OSDefineMetaClassAndStructors(RedNaviGraphicsControl, AppleGraphicsDeviceControl)
static IOService *gControlPCI;          // the PCI device the control object is attached to
static unsigned gControlCalls;
struct RNModeTiming {                    // struct rn_mode of RedNavi/edid.h (the probe publishes it as "fb-timing"); same layout, 28 bytes
    uint32_t pclk_khz;
    uint16_t hactive, hfp, hsync, hbp, htotal;
    uint16_t vactive, vfp, vsync, vbp, vtotal;
    uint8_t hsync_pos, vsync_pos, preferred, vic;
    uint16_t refresh_hz;
};
#pragma pack(push, 1)
struct RNVendorInfo {                    // command 1, 0x2c bytes
    uint32_t version; char name[0x20]; uint32_t vendorID; uint32_t vendorClass;
};
struct RNGpuCaps {                       // command 0x980, 0xdc bytes
    uint64_t portMask; uint64_t portFeature[3]; uint32_t count[4]; uint32_t maxFramebuffers; uint64_t pci; uint64_t framebuffer[20];
};
#pragma pack(pop)
static_assert(sizeof(RNVendorInfo) == 0x2c && sizeof(RNGpuCaps) == 0xdc, "the packed answer sizes the window server checks");
IOReturn RedNaviGraphicsControl::vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize, unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *) {
    unsigned n = ++gControlCalls;
    if (n <= 60) rnNote("graphics control: command 0x%x, in %lu bytes, out %p, outSize %lu", cmd, inSize, (void *)out, outSize ? *outSize : 0);
    if (cmd == 1) {
        if (!out || !outSize || *outSize != sizeof(RNVendorInfo)) return kIOReturnBadArgument;
        RNVendorInfo *v = reinterpret_cast<RNVendorInfo *>(out);
        memset(v, 0, sizeof(*v));
        v->version = 0x30000;
        strlcpy(v->name, "AMD", sizeof(v->name));
        v->vendorID = 0x1002;
        v->vendorClass = 2;
        return kIOReturnSuccess;
    }
    // Command 3: asked right after command 1 when the object starts (the log shows out 4 bytes). The shipped AMD GPU wrangler (AMDRadeonX6000_AmdGpuWrangler, which answers only 1 and
    // 3) writes the 32-bit value 3 there and returns success; what the 3 means is not known (read from the class's code on this Mac, unverified here).
    if (cmd == 3) {
        if (!out || !outSize || *outSize < sizeof(uint32_t)) return kIOReturnBadArgument;
        *reinterpret_cast<uint32_t *>(out) = 3;
        return kIOReturnSuccess;
    }
    if (cmd == 0x980) {
        if (!out || !outSize || *outSize != sizeof(RNGpuCaps)) return kIOReturnBadArgument;
        RNGpuCaps *c = reinterpret_cast<RNGpuCaps *>(out);
        memset(c, 0, sizeof(*c));
        c->portMask = 1ULL << 1;                       // one head, port bit 1 (a head i has bit 1 + i)
        c->count[0] = c->count[1] = c->count[2] = c->count[3] = 1;
        c->maxFramebuffers = 1;
        c->pci = reinterpret_cast<uint64_t>(gControlPCI);
        c->framebuffer[0] = reinterpret_cast<uint64_t>(gFb);
        return kIOReturnSuccess;
    }
    // Command 0x921 (link configuration of one head) and 0x711 (extended capability, type 0x10 = plane scaler): both are what CoreDisplay's display-pipe set-up reads before it builds the
    // IOPresentment map. The layouts are those the reference driver answers, passed on as facts: 0x921 is 0xb0 bytes in and out (framebuffer index at +0, group at +4, flags at +8, a
    // timing record from +0x10, port at +0xa4, stream +0xa8, state +0xac); 0x711 is 0x196c bytes (index +0, type +4, count +8, entries of 0x32c bytes from +0xc). Timings come from the probe.
    if (cmd == 0x921 || cmd == 0x711) {
        uint32_t want = cmd == 0x921 ? 0xb0 : 0x196c;
        if (!in || !out || !outSize || inSize != want || *outSize != want) return kIOReturnBadArgument;
        if (out != in) memcpy(out, in, want);
        uint8_t *o = reinterpret_cast<uint8_t *>(out);
        uint32_t idx = *reinterpret_cast<uint32_t *>(o);
        if (idx >= 1) return kIOReturnBadArgument;
        IOService *prov = gFb ? gFb->getProvider() : nullptr;
        OSData *td = prov ? OSDynamicCast(OSData, prov->getProperty("fb-timing")) : nullptr;
        if (!td || td->getLength() != sizeof(RNModeTiming)) return kIOReturnNotFound;
        const RNModeTiming *m = reinterpret_cast<const RNModeTiming *>(td->getBytesNoCopy());
        if (cmd == 0x921) {
            memset(o + 4, 0, want - 4);
            *reinterpret_cast<int32_t *>(o + 4) = -1;
            *reinterpret_cast<uint64_t *>(o + 8) = 0x1 | 0x2 | 0x8;
            IODetailedTimingInformationV2 tv;
            memset(&tv, 0, sizeof(tv));
            uint64_t hz = static_cast<uint64_t>(m->pclk_khz) * 1000;
            tv.pixelClock = tv.minPixelClock = tv.maxPixelClock = hz;
            tv.horizontalActive = m->hactive; tv.horizontalBlanking = m->htotal - m->hactive;
            tv.horizontalSyncOffset = m->hfp; tv.horizontalSyncPulseWidth = m->hsync;
            tv.verticalActive = m->vactive; tv.verticalBlanking = m->vtotal - m->vactive;
            tv.verticalSyncOffset = m->vfp; tv.verticalSyncPulseWidth = m->vsync;
            tv.horizontalSyncConfig = m->hsync_pos ? 1 : 0; tv.verticalSyncConfig = m->vsync_pos ? 1 : 0;
            tv.numLinks = 1; tv.pixelEncoding = 1; tv.bitsPerColorComponent = 2; tv.colorimetry = 1; tv.dynamicRange = 1;
            static_assert(sizeof(IODetailedTimingInformationV2) == 160, "the record the answer holds is the V2 timing without its first 12 bytes (0x94)");
            memcpy(o + 0x10, reinterpret_cast<uint8_t *>(&tv) + 12, 0x94);
            *reinterpret_cast<uint32_t *>(o + 0xa4) = 1 + idx;
            *reinterpret_cast<uint32_t *>(o + 0xa8) = 0;
            *reinterpret_cast<uint32_t *>(o + 0xac) = 1;
            if (n <= 60) rnNote("0x921 answered: %ux%u, %llu Hz pixel clock, htotal %u vtotal %u", m->hactive, m->vactive, hz, m->htotal, m->vtotal);
            return kIOReturnSuccess;
        }
        uint32_t type = *reinterpret_cast<uint32_t *>(o + 4);
        if (type != 0x10) return kIOReturnUnsupported;
        memset(o + 8, 0, want - 8);
        *reinterpret_cast<uint32_t *>(o + 8) = 1;
        uint8_t *e = o + 0xc;
        uint32_t h = m->vactive, w = m->hactive;
        *reinterpret_cast<uint32_t *>(e + 0x000) = 0x80004; *reinterpret_cast<uint32_t *>(e + 0x01c) = 1;
        *reinterpret_cast<uint32_t *>(e + 0x024) = h; *reinterpret_cast<uint32_t *>(e + 0x028) = w;
        *reinterpret_cast<uint32_t *>(e + 0x02c) = h; *reinterpret_cast<uint32_t *>(e + 0x030) = w;
        *reinterpret_cast<uint32_t *>(e + 0x034) = 0x70007;
        *reinterpret_cast<uint32_t *>(e + 0x1a4) = 1;
        *reinterpret_cast<uint32_t *>(e + 0x1ac) = h; *reinterpret_cast<uint32_t *>(e + 0x1b0) = w;
        *reinterpret_cast<uint32_t *>(e + 0x1b4) = h; *reinterpret_cast<uint32_t *>(e + 0x1b8) = w;
        *reinterpret_cast<uint32_t *>(e + 0x1bc) = 0x70007;
        if (n <= 60) rnNote("0x711 type 0x10 answered: %ux%u", w, h);
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}
// Attach the control object to the PCI device now that the framebuffer exists (the answer to command 0x980 lists it). Thread context.
static void attachGraphicsControl() {
    if (!gFb || !gProv) { rnNote("graphics control: no framebuffer yet (%p) or no PCI device (%p): not attached", (void *)gFb, (void *)gProv); return; }
    RedNaviGraphicsControl *g = OSTypeAlloc(RedNaviGraphicsControl);
    if (!g) { rnNote("graphics control: no memory"); return; }
    gControlPCI = gProv;
    bool ok = g->init(nullptr) && g->attach(gProv) && g->start(gProv);
    rnNote("graphics control %p: init/attach/start -> %d", (void *)g, ok);
    if (!ok) { g->detach(gProv); g->release(); }
}

// The display pipe of this driver, as the real AMD pipe is built (AMDRadeonX5000_AMDAccelDisplayPipe derives from IOAccelDisplayPipe and redefines init, the framebuffer resource,
// the transaction interrupts and validate/perform/isTransactionComplete/submitTransaction, per tools/accel_vtable.py). First step: init and initFramebufferResource, logged; the rest is the
// family's. Boot-arg rednavi_accel2_pipe_class=1 makes newDisplayPipe hand this class to the family; rednavi_accel2_pipe_nores=1 makes initFramebufferResource not register a resource
// (what the family gets today is a stub IOAccelResource2, 384 bytes, which is not a scanout buffer).
class RedNaviDisplayPipe : public IOAccelDisplayPipe {
    OSDeclareDefaultStructors(RedNaviDisplayPipe)
public:
    bool init(IOGraphicsAccelerator2 *accel, IOAccelDisplayMachine *machine, IOFramebuffer *fb, unsigned int index) override;
    long initFramebufferResource(unsigned int index, IOAccelResource2 *res) override;
    long destroyFramebufferResource(unsigned int index, IOAccelResource2 *res) override;
    long validateTransaction(IOAccelDisplayPipeTransaction2 *tx) override;
    long performTransaction(IOAccelDisplayPipeTransaction2 *tx) override;
    bool isTransactionComplete(IOAccelDisplayPipeTransaction2 *tx) override;
    virtual void rnPadP0(); virtual void rnPadP1(); virtual void rnPadP2(); virtual void rnPadP3();
};
static_assert(sizeof(RedNaviDisplayPipe) == 792, "IOAccelDisplayPipe is 792 bytes in the registry");
OSDefineMetaClassAndStructors(RedNaviDisplayPipe, IOAccelDisplayPipe)
void RedNaviDisplayPipe::rnPadP0() {} void RedNaviDisplayPipe::rnPadP1() {} void RedNaviDisplayPipe::rnPadP2() {} void RedNaviDisplayPipe::rnPadP3() {}
bool RedNaviDisplayPipe::init(IOGraphicsAccelerator2 *accel, IOAccelDisplayMachine *machine, IOFramebuffer *fb, unsigned int index) {
    bool ok = IOAccelDisplayPipe::init(accel, machine, fb, index);
    rnNote("RedNaviDisplayPipe::init(%p, %p, %p, %u) -> %d", (void *)accel, (void *)machine, (void *)fb, index, ok);
    return ok;
}
static bool provNumber(IOService *prov, const char *key, uint64_t *out) {
    OSNumber *n = prov ? OSDynamicCast(OSNumber, prov->getProperty(key)) : nullptr;
    if (!n) return false;
    *out = n->unsigned64BitValue();
    return true;
}
long RedNaviDisplayPipe::initFramebufferResource(unsigned int index, IOAccelResource2 *res) {
    uint32_t nores = 0;
    PE_parse_boot_argn("rednavi_accel2_pipe_nores", &nores, sizeof(nores));
    rnNote("RedNaviDisplayPipe::initFramebufferResource(%u, %p), %s", index, (void *)res, nores ? "no resource registered" : "scanout memory");
    if (nores) return 0;
    // The scanout the probe published (physical address, pitch and height of the live mode) is read from the framebuffer's provider.
    IOService *prov = gFb ? gFb->getProvider() : nullptr;
    uint64_t phys = 0, pitch = 0, height = 0;
    if (!provNumber(prov, "fb-scanout-phys", &phys) || !provNumber(prov, "fb-pitch", &pitch) || !provNumber(prov, "fb-height", &height) || !phys || !pitch || !height) {
        rnNote("initFramebufferResource: no usable scanout (phys %llx, pitch %llu, height %llu): NULL", phys, pitch, height);
        return 0;
    }
    uint64_t bytes = pitch * height;
    RedNaviScanoutMemory *m = OSTypeAlloc(RedNaviScanoutMemory);
    if (!m) return 0;
    m->init(static_cast<IOGraphicsAccelerator2 *>(gSelf), nullptr, res, bytes, nullptr);
    uint8_t *b = reinterpret_cast<uint8_t *>(m);
    rnNote("scanout memory %p after init: +0x40 %llx, +0xc0 %llx, +0xc8 %llx", (void *)m, *reinterpret_cast<uint64_t *>(b + 0x40), *reinterpret_cast<uint64_t *>(b + 0xc0), *reinterpret_cast<uint64_t *>(b + 0xc8));
    *reinterpret_cast<uint64_t *>(b + 0x40) = bytes;
    *reinterpret_cast<uint64_t *>(b + 0xc8) = phys;
    rnNote("scanout memory: physical %llx, %llu bytes (pitch %llu x height %llu)", phys, bytes, pitch, height);
    gScanout[gScanoutNext] = m; gScanoutNext = (gScanoutNext + 1) % kScanoutRing; if (gScanoutCount < kScanoutRing) gScanoutCount++;
    return reinterpret_cast<long>(m);
}
long RedNaviDisplayPipe::destroyFramebufferResource(unsigned int index, IOAccelResource2 *res) {
    rnNote("RedNaviDisplayPipe::destroyFramebufferResource(%u, %p)", index, (void *)res);
    return IOAccelDisplayPipe::destroyFramebufferResource(index, res);
}

// Transactions. A transaction is what WindowServer composes for one display refresh. The family's own performTransaction answers kIOReturnUnsupported (0xe00002c7), which leaves
// the screen undrawn. This one copies plane 0, layer 0 into the scanout surface, synchronously, so the transaction is complete when it returns. Unverified on hardware: which of the
// surface and the resource WindowServer hands over, and in which format; the log says (first calls, then one in 600).
static IOLock *gXferLock;
static IOMemoryMap *gScanMap;
static uint64_t gScanMapPhys, gScanMapBytes;
static uint32_t gXferCount;
static uint8_t *scanoutCPU(uint64_t phys, uint64_t bytes) {    // caller holds gXferLock
    if (gScanMap && (gScanMapPhys != phys || gScanMapBytes != bytes)) { gScanMap->release(); gScanMap = nullptr; }
    if (!gScanMap) {
        IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(phys, bytes, kIODirectionInOut);
        if (!md) return nullptr;
        gScanMap = md->map(kIOMapWriteCombineCache);
        md->release();
        gScanMapPhys = phys; gScanMapBytes = bytes;
    }
    return gScanMap ? reinterpret_cast<uint8_t *>(gScanMap->getVirtualAddress()) : nullptr;
}
long RedNaviDisplayPipe::validateTransaction(IOAccelDisplayPipeTransaction2 *) { return 0; }
bool RedNaviDisplayPipe::isTransactionComplete(IOAccelDisplayPipeTransaction2 *) { return true; }
long RedNaviDisplayPipe::performTransaction(IOAccelDisplayPipeTransaction2 *tx) {
    uint32_t n = ++gXferCount;
    bool say = n <= 6 || n % 600 == 0;
    void *surfP = tx->getPlaneIOSurface(0, 0), *resP = tx->getPlaneResource(0, 0);
    if (say) rnNote("performTransaction #%u: tx %p, plane(0,0) surface %p, resource %p", n, (void *)tx, surfP, resP);
    if (n <= 3) {   // measurement: where WindowServer puts the planes (the transaction keeps a table of 112-byte plane records at +56)
        rnNote("  tx id %u, dirty bits %x, options %x", tx->getTransactionID(), tx->getTransactionDirtyBits(), tx->getTransactionOptions());
        rnDumpWords("  tx", tx, 0x60);
        const uint8_t *planes = *reinterpret_cast<uint8_t *const *>(reinterpret_cast<const uint8_t *>(tx) + 56);
        if (planes) for (unsigned pl = 0; pl < 3; pl++) for (unsigned l = 0; l < 2; l++)
            rnNote("  plane %u layer %u: surface %p resource %p", pl, l, tx->getPlaneIOSurface(pl, l), tx->getPlaneResource(pl, l));
        if (planes) rnDumpWords("  plane0 record", planes, 112);
    }
    if (!surfP) return 0;                                   // nothing to show on plane 0 (or a resource source, not handled yet)
    IOSurface *surf = static_cast<IOSurface *>(surfP);
    IOService *prov = gFb ? gFb->getProvider() : nullptr;
    {   // The Metal plug-in renders CoreDisplay's passes on the card and copies each finished frame into the scanout itself (RN_PRESENT_FRAME_DONE): then the surface is not read here.
        static const OSSymbol *framesSym;
        if (!framesSym) framesSym = OSSymbol::withCString("rednavi-gpu-frames");
        uint32_t direct = 0;
        if (prov && framesSym && prov->callPlatformFunction(framesSym, true, &direct, nullptr, nullptr, nullptr) == kIOReturnSuccess && direct) {
            if (say) rnNote("  the plug-in puts the frames into the scanout itself: not copied");
            return 0;
        }
    }
    uint64_t phys = 0, pitch = 0, height = 0, width = 0;
    if (!provNumber(prov, "fb-scanout-phys", &phys) || !provNumber(prov, "fb-pitch", &pitch) || !provNumber(prov, "fb-height", &height) || !provNumber(prov, "fb-width", &width)) return 0;
    uint32_t sw = surf->getWidth(), sh = surf->getHeight(), sp = surf->getBytesPerRow(), fmt = surf->getPixelFormat();
    if (say) rnNote("  surface %ux%u pitch %u format %08x; scanout %llux%llu pitch %llu", sw, sh, sp, fmt, width, height, pitch);
    if (fmt != 0x42475241 || sw != width || sh != height || sp < sw * 4 || sp % 4) {
        if (say) rnNote("  surface does not match the scanout (BGRA, same size): not copied");
        return 0;
    }
    IOMemoryDescriptor *md = surf->getMemoryDescriptor();
    if (!md) { if (say) rnNote("  surface has no memory descriptor"); return 0; }
    if (!gXferLock) return 0;
    IOLockLock(gXferLock);
    uint8_t *dst = scanoutCPU(phys, pitch * height);
    IOReturn pr = md->prepare();
    IOMemoryMap *src = pr == kIOReturnSuccess ? md->map(kIOMapReadOnly) : nullptr;
    if (dst && src) {
        const uint8_t *sb = reinterpret_cast<const uint8_t *>(src->getVirtualAddress());
        size_t row = sp < pitch ? sp : pitch;
        for (uint64_t y = 0; y < height; y++) memcpy(dst + y * pitch, sb + y * sp, row);
        __sync_synchronize();   // the scanout mapping is write-combined: the stores must have left the CPU before the display engine is let at them
        if (say) {   // a sample of the surface that was copied: how many of 4096 spread pixels are not black (alpha ignored), and a few values
            unsigned nz = 0; uint32_t v0 = 0, v1 = 0, v2 = 0;
            for (unsigned i = 0; i < 4096; i++) {
                uint64_t y = (static_cast<uint64_t>(i) * 2654435761u) % height, x = (static_cast<uint64_t>(i) * 40503u) % width;
                uint32_t px = *reinterpret_cast<const uint32_t *>(sb + y * sp + x * 4);
                if (px & 0x00ffffff) nz++;
                if (i == 0) v0 = px; else if (i == 1) v1 = px; else if (i == 2) v2 = px;
            }
            rnNote("  copied; %u of 4096 sampled pixels are not black (first three: %08x %08x %08x)", nz, v0, v1, v2);
        }
    } else if (say) rnNote("  copy skipped: scanout map %p, surface prepare %x, map %p", (void *)dst, pr, (void *)src);
    if (src) src->release();
    if (pr == kIOReturnSuccess) md->complete();
    IOLockUnlock(gXferLock);
    return 0;
}

class RedNaviAccel2 : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(RedNaviAccel2)
    void *allocByName(const char *hook, const char *cls);
    void *makeTask(const char *hook, unsigned int n);
    template <class T> void *allocStub(const char *hook) {
        if (gStage < 2) { rnNote("%s called (stage %u: refused)", hook, gStage); return nullptr; }
        T *o = OSTypeAlloc(T);
        rnNote("%s: %u bytes -> %p", hook, (unsigned)sizeof(T), (void *)o);
        return o;
    }
public:
    void dumpWords(const char *what, const void *p, uint32_t bytes);
    IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    // The display pipe (docs/M6_ACCELERATOR_STUDY.md, section 3): WindowServer opens client type 4 of the accelerator and the family answers kIOReturnUnsupported when no pipe exists for the framebuffer.
    // Both hooks log and call the family; start() calls createDisplayPipe for the framebuffer when it appears.
    void *newDisplayPipe() override;
    // Logged: what the family answers; boot-arg rednavi_accel2_headless=1 keeps the family's answer, default is false (a head exists, the display is composed by this accelerator).
    bool configIsHeadless() override;
    void *createDisplayPipe(IOFramebuffer *fb, unsigned int index) override;
    // Logged only: which client types the clients ask for (the display pipe's among them) and what the family answers.
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
    // The 17 pure virtuals, written out because each one logs its own arguments.
    void *getStampMemory(unsigned int *size) override;
    void *newEventMachine() override;
    void *createUserGPUTask() override;
    void populateAccelConfig(IOAccelConfig *cfg) override;
    bool configureDevice(IOPCIDevice *pci) override;
    bool teardownDevice(IOPCIDevice *pci) override;
    void *createKernelGPUTask() override;
    void *newDisplayMachine() override;
    void *newGLContext() override;
    void *newCLContext() override;
    void *newSurface() override;
    void *new2DContext() override;
    void *newVideoContext() override;
    void *newSysMemory() override;
    void *newVidMemory() override;
    void *newResource() override;
    void *newMemoryMap() override;
    // kxld refuses a subclass whose vtable is shorter than the superclass's; same padding as RedNaviAccel.
    virtual void rnPad0(); virtual void rnPad1(); virtual void rnPad2(); virtual void rnPad3(); virtual void rnPad4(); virtual void rnPad5(); virtual void rnPad6(); virtual void rnPad7();
};

OSDefineMetaClassAndStructors(RedNaviAccel2, IOGraphicsAccelerator2)

void RedNaviAccel2::rnPad0() {} void RedNaviAccel2::rnPad1() {} void RedNaviAccel2::rnPad2() {} void RedNaviAccel2::rnPad3() {}
void RedNaviAccel2::rnPad4() {} void RedNaviAccel2::rnPad5() {} void RedNaviAccel2::rnPad6() {} void RedNaviAccel2::rnPad7() {}

void RedNaviAccel2::dumpWords(const char *what, const void *p, uint32_t bytes) {
    const uint32_t *w = static_cast<const uint32_t *>(p);
    for (uint32_t i = 0; i < bytes / 4; i += 8) {
        char s[96];
        int len = 0;
        for (uint32_t j = i; j < i + 8 && j < bytes / 4; j++) len += snprintf(s + len, sizeof(s) - (size_t)len, "%s%08x", j > i ? " " : "", w[j]);
        rnNote("%s +0x%x: %s", what, i * 4, s);
    }
}

// An instance of the family's concrete class by name, via its metaclass (no header needed). NULL if the class is not registered or is abstract (an abstract class's
// metaclass alloc() returns NULL). The object is not initialised: the family's caller is expected to do that, which is one of the things this stage finds out.
void *RedNaviAccel2::allocByName(const char *hook, const char *cls) {
    if (gStage < 2) { rnNote("%s called (stage %u: refused)", hook, gStage); return nullptr; }
    const OSSymbol *sym = OSSymbol::withCString(cls);
    const OSMetaClass *mc = sym ? OSMetaClass::getMetaClassWithName(sym) : nullptr;
    if (sym) sym->release();
    if (!mc) { rnNote("%s: class %s is not registered", hook, cls); return nullptr; }
    OSObject *o = mc->alloc();
    rnNote("%s: %s size %u -> %p%s", hook, cls, mc->getClassSize(), o, o ? "" : " (abstract)");
    return o;
}

IOService *RedNaviAccel2::probe(IOService *provider, SInt32 *score) {
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_accel2", &arg, sizeof(arg)) || arg == 0) return nullptr;
    gStage = arg;
    if (!gLogLock) gLogLock = IOLockAlloc();
    if (!gXferLock) gXferLock = IOLockAlloc();
    if (!gLog) gLog = OSArray::withCapacity(256);
    gProv = provider;
    rnNote("probe on %s, stage %u", provider ? provider->getName() : "?", gStage);
    return IOService::probe(provider, score);
}

// IOAccelDisplayPipeCapabilities and AccelCaps. The reference driver publishes them only after the heads are registered and the transactions can be served; start() used to do it, before
// any pipe existed (bit 5 of rednavi_accel2_exp moves it to the end of the display bring-up).
static void rnPublishCaps(IOService *a) {
    OSDictionary *caps = OSDictionary::withCapacity(2);
    if (caps) {
        caps->setObject("DisplayPipeSupported", kOSBooleanTrue);
        caps->setObject("TransactionsSupported", kOSBooleanTrue);
        a->setProperty("IOAccelDisplayPipeCapabilities", caps);
        caps->release();
    }
    OSNumber *ac = OSNumber::withNumber(15ULL, 32);
    if (ac) { a->setProperty("AccelCaps", ac); ac->release(); }
    rnNote("published IOAccelDisplayPipeCapabilities and AccelCaps 15");
    // Bit 6: what the reference driver sets at this point (IOGLBundleName) and a size the Intel accelerator publishes and the display code may read (VRAM,totalMB, in MB).
    if (accelExp() & 64) {
        a->setProperty("IOGLBundleName", "AppleMetalOpenGLRenderer");
        OSNumber *vm = OSNumber::withNumber(16384ULL, 32);
        if (vm) { a->setProperty("VRAM,totalMB", vm); vm->release(); }
        rnNote("published IOGLBundleName and VRAM,totalMB (rednavi_accel2_exp bit 6)");
    }
}

// The family's start() builds everything but does not register the service (the node stayed !registered and Metal saw no device), which the Intel subclass presumably does
// after it; stage 3 and up do it here.
bool RedNaviAccel2::start(IOService *provider) {
    bool ok = IOGraphicsAccelerator2::start(provider);
    rnNote("family start -> %s", ok ? "true" : "false");
    if (!ok || gStage < 3) return ok;
    // What the Intel accelerator publishes and the family leaves to the subclass (docs/M6_ACCELERATOR_STUDY.md, section 3): WindowServer looks for the display pipe of each
    // framebuffer through these ("Unable to find display pipe for IOFB service" otherwise, and the display has no Metal composite destination). Boot-arg rednavi_accel2_pipe=0 leaves them out.
    uint32_t pipe = 1;
    PE_parse_boot_argn("rednavi_accel2_pipe", &pipe, sizeof(pipe));
    if (pipe && !(accelExp() & 32)) rnPublishCaps(this);
    // Boot-arg rednavi_accel2_delay=<seconds>: register later, so that a panic after the registration comes when SSH is up and tools/accel2_watch.sh can follow the log
    // (a panic loses the property; registration at once crashed 17 s into the boot, before SSH answers).
    uint32_t delay = 0;
    PE_parse_boot_argn("rednavi_accel2_delay", &delay, sizeof(delay));
    if (!delay) { registerService(); rnNote("registerService called"); return ok; }
    static thread_call_t call;
    gSelf = this;
    retain();
    call = thread_call_allocate([](thread_call_param_t, thread_call_param_t) {
        rnNote("registerService after the delay");
        attachGraphicsControl();
        gSelf->registerService();
        rnNote("registerService returned");
        // The family made the display pipe when the framebuffer appeared (createDisplayPipe in the log) but the pipe is inactive ("isFramebufferActive" false) while the framebuffer
        // has not told the display machine it is powered on, and WindowServer's request for the pipe (client type 4) is refused until it is. RedNaviFB stays at power state 0
        // (no MaxPowerState in its IOPowerManagement: it never registers a power driver), so the notification is made here (rednavi_accel2_pipe=0 skips it).
        uint32_t pipe = 1;
        PE_parse_boot_argn("rednavi_accel2_pipe", &pipe, sizeof(pipe));
        if (pipe && gMachine) {
            RedNaviAccel2 *self = static_cast<RedNaviAccel2 *>(gSelf);
            rnNote("display machine before: framebuffers %u, active(0) %d, deferred(0) %d", gMachine->getFramebufferCount(), gMachine->isFramebufferActive(0), gMachine->isDeferred(0));
            // The display machine starts with no framebuffer (count 0) and registers a head through found_framebuffer (it makes the pipe and fills its table); isFramebufferActive stays 0 until then.
            self->dumpWords("machine before", gMachine, 376);
            if (IOAccelDisplayPipe *p0 = gMachine->getDisplayPipe(0)) self->dumpWords("pipe before", p0, 792);
            if (gMachine->getFramebufferCount() == 0 && gFb) {
                gMachine->found_framebuffer(gFb);
                rnNote("found_framebuffer(%p) told to the display machine: framebuffers %u, active(0) %d", (void *)gFb, gMachine->getFramebufferCount(), gMachine->isFramebufferActive(0));
            }
            // Experiment 1: the family's own order after a head appears is display_mode_will_change, display_mode_did_change, then enableAccelerator (also after registerService).
            rnFlags("before mode change", self, gMachine);
            gMachine->display_mode_will_change(0);
            rnFlags("after display_mode_will_change", self, gMachine);
            gMachine->display_mode_did_change(0);
            rnFlags("after display_mode_did_change", self, gMachine);
            static_cast<IOGraphicsAccelerator2 *>(gSelf)->enableAccelerator();
            rnFlags("after enableAccelerator", self, gMachine);
            gMachine->framebuffer_did_power_on(0);
            rnFlags("after framebuffer_did_power_on", self, gMachine);
            rnNote("framebuffer_did_power_on(0) told to the display machine %p", (void *)gMachine);
            rnNote("display machine after: active(0) %d, deferred(0) %d", gMachine->isFramebufferActive(0), gMachine->isDeferred(0));
            // The pipe has its own hooks for the same event (IOAccelDisplayPipe virtual slots 304 framebuffer_did_power_on and 292 didPowerOn, docs/M6_ACCEL_ABI.md); the call goes through
            // the vtable because the class is not declared here.
            // Experiment (boot-arg rednavi_accel2_wsaa=1): the machine's own WindowServer-active transitions, in the order a framebuffer goes accelerated, the activity read after each.
            uint32_t wsaa = 0;
            PE_parse_boot_argn("rednavi_accel2_wsaa", &wsaa, sizeof(wsaa));
            if (wsaa) {
                for (int arg = 1; arg >= 0; arg--) {
                    gMachine->wsaa_will_enter_defer(0, arg);  rnNote("wsaa_will_enter_defer(0,%d): active %d deferred %d", arg, gMachine->isFramebufferActive(0), gMachine->isDeferred(0));
                    gMachine->wsaa_did_enter_defer(0, arg);   rnNote("wsaa_did_enter_defer(0,%d): active %d deferred %d", arg, gMachine->isFramebufferActive(0), gMachine->isDeferred(0));
                    gMachine->wsaa_will_exit_defer(0, arg);   rnNote("wsaa_will_exit_defer(0,%d): active %d deferred %d", arg, gMachine->isFramebufferActive(0), gMachine->isDeferred(0));
                    gMachine->wsaa_did_exit_defer(0, arg);    rnNote("wsaa_did_exit_defer(0,%d): active %d deferred %d", arg, gMachine->isFramebufferActive(0), gMachine->isDeferred(0));
                }
            }
            IOAccelDisplayPipe *p = gMachine->getDisplayPipe(0);
            rnNote("display pipe %p: machine active(0) %d", (void *)p, gMachine->isFramebufferActive(0));
            if (p) {
                void **vt = *reinterpret_cast<void ***>(p);
                reinterpret_cast<void (*)(void *)>(vt[304])(p);
                rnNote("after pipe framebuffer_did_power_on: machine active(0) %d", gMachine->isFramebufferActive(0));
                reinterpret_cast<void (*)(void *)>(vt[292])(p);
                rnNote("after pipe didPowerOn: machine active(0) %d", gMachine->isFramebufferActive(0));
                self->dumpWords("machine after", gMachine, 376);
                self->dumpWords("pipe after", p, 792);
            }
        }
        if (accelExp() & 32) rnPublishCaps(gSelf);
    }, nullptr);
    uint64_t deadline;
    clock_interval_to_deadline(delay, NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(call, deadline);
    rnNote("registerService in %u s", delay);
    return ok;
}

IOReturn RedNaviAccel2::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) {
    IOReturn r;
    if (type == 4 && gStage >= 3 && handler) {
        // Client type 4 is the display pipe. The family's newUserClient has no case for it (the dtrace of 2026-11-06: it counts the process's clients and answers kIOReturnUnsupported), so the
        // accelerator's own subclass makes it, as the Intel and AMD ones do: an IOAccelDisplayPipeUserClient2, init, attach, start. WindowServer then selects the pipe with its
        // set_pipe_index method. Since the pipe is active (pipe+0x298 = 1) the family makes the client itself, as the reference driver relies on: this hand-made one is only built with
        // the boot-arg rednavi_accel2_pipe_hand=1 (it was the default while the family refused type 4 for an inactive pipe; WindowServer closed it at once, 2026-11-07).
        uint32_t hand = 0;
        PE_parse_boot_argn("rednavi_accel2_pipe_hand", &hand, sizeof(hand));
        r = kIOReturnUnsupported;
        if (!hand) r = IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, properties, handler);
        else {
            const OSSymbol *sym = OSSymbol::withCString("IOAccelDisplayPipeUserClient2");
            const OSMetaClass *mc = sym ? OSMetaClass::getMetaClassWithName(sym) : nullptr;
            if (sym) sym->release();
            OSObject *o = mc ? mc->alloc() : nullptr;
            IOAccelDisplayPipeUserClient2 *uc = o ? static_cast<IOAccelDisplayPipeUserClient2 *>(OSDynamicCast(IOUserClient, o)) : nullptr;
            if (uc) {
                bool ok = uc->init(nullptr, owningTask);
                rnNote("display pipe client %p: init -> %d", (void *)uc, ok);
                ok = ok && uc->attach(this);
                rnNote("display pipe client: attach -> %d", ok);
                ok = ok && uc->start(this);
                rnNote("display pipe client: start -> %d", ok);
                rnNote("display pipe client: retain count %d", uc->getRetainCount());
                // The kernel panicked when WindowServer died and the client was stopped: "A kext releasing a(n) IOAccelDisplayPipeUserClient2 has corrupted the registry" (OSObject.cpp:224,
                // IOService::actionStop, 2026-11-07): the family's stop releases the client once more than this hand-over leaves it holding. One extra retain here (a leak of one object per
                // open, the price of an experiment) tells whether that is the difference; rednavi_accel2_pipe_retain=0 leaves it out.
                uint32_t extra = 1;
                PE_parse_boot_argn("rednavi_accel2_pipe_retain", &extra, sizeof(extra));
                if (ok && extra) uc->retain();
                if (ok) { *handler = uc; r = kIOReturnSuccess; }
                else { uc->detach(this); uc->release(); r = kIOReturnError; }
            } else if (o) o->release();
        }
    } else
        r = IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, properties, handler);
    static uint32_t seen;
    if (seen < 200) {                                  // the log keeps 256 lines: the first clients are enough
        seen++;
        rnNote("newUserClient(type %u, properties %p) -> 0x%x, client %p (%s)", (unsigned)type, properties, (unsigned)r, handler ? (void *)*handler : nullptr, handler && *handler ? (*handler)->getMetaClass()->getClassName() : "-");
    }
    return r;
}

bool RedNaviAccel2::configIsHeadless() {
    bool fam = IOGraphicsAccelerator2::configIsHeadless();
    uint32_t keep = 0;
    PE_parse_boot_argn("rednavi_accel2_headless", &keep, sizeof(keep));
    rnNote("configIsHeadless: family says %d, answering %d", (int)fam, keep ? (int)fam : 0);
    return keep ? fam : false;
}

void *RedNaviAccel2::newDisplayPipe() {
    // Boot-arg rednavi_accel2_legacy_pipe=1 hands the family an IOAccelLegacyDisplayPipe (896 bytes, a family class: the scanout and "accelerator backed" side of the pipe) instead of what the
    // family's own newDisplayPipe makes; the class of what it makes is logged. The measurement of 2026-11-07: the family stores the accelerator at +0x88, the display machine at +0x90
    // and the framebuffer at +0x98 of whichever pipe it is given. The family's own factory is NOT called when the legacy pipe is asked for and the object it makes is never released here:
    // an uninitialised pipe panics in IOAccelDisplayPipe::free (releaseLiveTransaction reads a null member; the first version of this did that, 2026-11-07).
    uint32_t ours = 0;
    PE_parse_boot_argn("rednavi_accel2_pipe_class", &ours, sizeof(ours));
    if (ours) {
        RedNaviDisplayPipe *rp = OSTypeAlloc(RedNaviDisplayPipe);
        rnNote("newDisplayPipe -> %p (RedNaviDisplayPipe, %u bytes)", (void *)rp, (unsigned)sizeof(RedNaviDisplayPipe));
        return rp;
    }
    uint32_t legacy = 0;
    PE_parse_boot_argn("rednavi_accel2_legacy_pipe", &legacy, sizeof(legacy));
    if (legacy) {
        void *p = allocByName("newDisplayPipe", "IOAccelLegacyDisplayPipe");
        OSObject *po = static_cast<OSObject *>(p);
        rnNote("newDisplayPipe -> %p (IOAccelLegacyDisplayPipe by name), class %s", p, po ? po->getMetaClass()->getClassName() : "-");
        return p;
    }
    void *p = IOGraphicsAccelerator2::newDisplayPipe();
    OSObject *po = static_cast<OSObject *>(p);
    rnNote("newDisplayPipe -> %p (family), class %s", p, po ? po->getMetaClass()->getClassName() : "-");
    if (!p) {
        p = allocByName("newDisplayPipe", "IOAccelDisplayPipe");
        rnNote("newDisplayPipe -> %p (IOAccelDisplayPipe by name)", p);
    }
    return p;
}
void *RedNaviAccel2::createDisplayPipe(IOFramebuffer *fb, unsigned int index) {
    rnNote("createDisplayPipe(%p %s, %u)", (void *)fb, fb ? fb->getName() : "-", index);
    if (fb && !gFb) { fb->retain(); gFb = fb; }
    void *r = IOGraphicsAccelerator2::createDisplayPipe(fb, index);
    rnNote("createDisplayPipe -> %p", r);
    // Measurement: where, in the pipe object (792 bytes for IOAccelDisplayPipe, 896 for the legacy one), the family keeps the framebuffer, the display machine and the accelerator.
    if (r) {
        const uint64_t *w = static_cast<const uint64_t *>(r);
        const uint64_t want[3] = {reinterpret_cast<uint64_t>(fb), reinterpret_cast<uint64_t>(gMachine), reinterpret_cast<uint64_t>(this)};
        static const char *const nm[3] = {"framebuffer", "display machine", "accelerator"};
        for (uint32_t i = 0; i < 112; i++)
            for (int k = 0; k < 3; k++)
                if (want[k] && w[i] == want[k]) rnNote("pipe %p +0x%x holds the %s", r, i * 8, nm[k]);
        rnNote("pipe words +0x70: %llx %llx %llx %llx %llx %llx %llx %llx", w[14], w[15], w[16], w[17], w[18], w[19], w[20], w[21]);
        rnNote("pipe words +0xb0: %llx %llx %llx %llx %llx %llx %llx %llx", w[22], w[23], w[24], w[25], w[26], w[27], w[28], w[29]);
    }
    return r;
}

void *RedNaviAccel2::getStampMemory(unsigned int *size) {
    rnNote("getStampMemory(size %p)", size);
    if (gStage < 2) return nullptr;
    IOBufferMemoryDescriptor *d = rnStampDesc();
    // The size told to the family is NOT the length of the buffer: with 0x10000 (the buffer's length) IOAccelDeviceCreateWithAPIProperty read past the end of the mapping,
    // with 0x108 it ran to the end and the device was created (observed 2026-11-04 with the one-process harness, tools/mtl_init_harness.m; 0x108 is also the number
    // Intel's reply of selector 2 has at +8). 0x108 is 64 stamps of 4 bytes plus 8. rednavi_accel2_stampreport=<hex> overrides it.
    uint32_t reported = 0x108;
    PE_parse_boot_argn("rednavi_accel2_stampreport", &reported, sizeof(reported));
    if (size) *size = reported;
    rnNote("getStampMemory -> descriptor %p (bytes %p)", (void *)d, rnStampPage());
    return d;
}

void *RedNaviAccel2::newEventMachine() {
    if (gStage < 2) { rnNote("newEventMachine called (stage %u: refused)", gStage); return nullptr; }
    RedNaviEventMachine *m = OSTypeAlloc(RedNaviEventMachine);
    rnNote("newEventMachine: RedNaviEventMachine size %u -> %p", (unsigned)sizeof(RedNaviEventMachine), (void *)m);
    return m;
}
// The task (a GPU address space with the allocators that hand out its addresses). The family does not initialise what these two factories return: an uninitialised task
// panicked in IOAccelSharedUserClient2::sharedStop -> IOAccelTask::prune_orphaned_mappings (a null accelerator pointer, a read at +0xc78 of it, 2026-11-04), and the Intel
// subclass of IOAccelTask overrides neither init nor anything but free, so the factory is where init(accelerator, n, allocators) is called. What the unsigned int means
// is not known (a count or a kind); the allocators are four ranges of 1 TB of GPU virtual address space, none of this is used yet.
void *RedNaviAccel2::makeTask(const char *hook, unsigned int n) {
    if (gStage < 2) { rnNote("%s called (stage %u: refused)", hook, gStage); return nullptr; }
    IOAccelTask *t = OSTypeAlloc(IOAccelTask);
    if (!t) { rnNote("%s: no memory", hook); return nullptr; }
    IORangeAllocator *ra[4] = {};
    for (int i = 0; i < 4; i++) { ra[i] = IORangeAllocator::withRange((1ULL << 40) - 1, 0x1000, 0, 0); if (ra[i] && (accelExp() & 1)) ra[i]->allocateRange(0, 0x100000); }
    // Initialised at every stage: left uninitialised at stage 2 it panicked as soon as any shared client was closed (IOAccelSharedUserClient2::sharedStop ->
    // IOAccelTask::prune_orphaned_mappings, 2026-11-04, 22 s after boot); at stage 3 the same init ran for hours of boots without a panic.
    bool ok = t->init(this, n, ra);
    rnNote("%s: IOAccelTask %p, init(%p, %u, allocators) -> %s", hook, (void *)t, (void *)this, n, ok ? "true" : "false");
    return t;
}
void *RedNaviAccel2::createUserGPUTask() { return makeTask("createUserGPUTask", 1); }
// The unsigned int is the number of allocators the task copies (IOAccelTask::init loops over it; slot i of the allocator table is read by IOAccelTask::allocate with the index in
// the memory map). The kernel task had 0 and panicked in allocate (null allocator, 2026-11-07) as soon as the framebuffer resource was prepared: it gets all four.
void *RedNaviAccel2::createKernelGPUTask() { return makeTask("createKernelGPUTask", 4); }
void *RedNaviAccel2::newDisplayMachine() {
    if (gStage < 2) { rnNote("newDisplayMachine called (stage %u: refused)", gStage); return nullptr; }
    RedNaviDisplayMachine *m = OSTypeAlloc(RedNaviDisplayMachine);
    gMachine = m;
    rnNote("newDisplayMachine: RedNaviDisplayMachine size %u -> %p", (unsigned)sizeof(RedNaviDisplayMachine), (void *)m);
    return m;
}
void *RedNaviAccel2::newGLContext() { return allocStub<RedNaviGLContext2>("newGLContext"); }
void *RedNaviAccel2::newCLContext() { return allocByName("newCLContext", "IOAccelCLContext2"); }
void *RedNaviAccel2::newSurface() { return allocStub<RedNaviSurface>("newSurface"); }
void *RedNaviAccel2::new2DContext() { return allocStub<RedNavi2DContext2>("new2DContext"); }
void *RedNaviAccel2::newVideoContext() { return allocByName("newVideoContext", "IOAccelVideoContext2"); }
void *RedNaviAccel2::newSysMemory() {
    void *p = OSTypeAlloc(RedNaviSysMemory);
    rnNote("newSysMemory: RedNaviSysMemory %p", p);
    return p;
}
void *RedNaviAccel2::newVidMemory() { return allocStub<RedNaviVidMemory>("newVidMemory"); }
void *RedNaviAccel2::newResource() { return allocStub<RedNaviResource2>("newResource"); }
void *RedNaviAccel2::newMemoryMap() { return allocStub<RedNaviMemoryMap>("newMemoryMap"); }

void RedNaviAccel2::populateAccelConfig(IOAccelConfig *cfg) {
    rnNote("populateAccelConfig(%p)", cfg);
    if (gStage < 2) return;
    // The structure's size is unknown; 64 bytes is a read that stays inside any plausible one, to see whether the family pre-fills it.
    if (cfg) dumpWords("IOAccelConfig", cfg, 64);
    // Bit 4 of rednavi_accel2_exp: the fields the reference driver writes (read from its code, unverified here): AccelCaps, limits, texture size, feature masks. +0x00 (a class name
    // pointer) and +0x08 (flags) are left as the family filled them. Only when the whole 0x70 bytes lie in one page (the structure's size is the reference's).
    if (cfg && (accelExp() & 16) && (reinterpret_cast<uintptr_t>(cfg) & 0xfff) + 0x70 <= 0x1000) {
        uint8_t *c = reinterpret_cast<uint8_t *>(cfg);
        *reinterpret_cast<uint32_t *>(c + 0x0c) = 15;
        *reinterpret_cast<uint32_t *>(c + 0x18) = 8;
        *reinterpret_cast<uint32_t *>(c + 0x28) = 1;
        *reinterpret_cast<uint32_t *>(c + 0x30) = 0x40004000;
        *reinterpret_cast<uint32_t *>(c + 0x34) = 0x8000;
        *reinterpret_cast<uint32_t *>(c + 0x38) = 0x8000;
        c[0x44] = 0; c[0x46] = 1; c[0x47] = 1;
        *reinterpret_cast<uint64_t *>(c + 0x48) = 0x0000001F0000000FULL;
        *reinterpret_cast<uint64_t *>(c + 0x50) = 0x7FFFFFFF0000001FULL;
        *reinterpret_cast<uint64_t *>(c + 0x58) = 0x0000000C00000000ULL;
        *reinterpret_cast<uint32_t *>(c + 0x64) = 0x4B;
        rnNote("IOAccelConfig filled as the reference driver does (rednavi_accel2_exp bit 4)");
    }
    // Experiment knob: boot-arg rednavi_accel2_cfg=<offset>=<value>,<offset>=<value>... (hexadecimal, 32-bit values, offsets multiple of 4 below 0x40, the part that was read).
    char spec[128];
    if (cfg && PE_parse_boot_argn("rednavi_accel2_cfg", spec, sizeof(spec))) {
        uint32_t *w = reinterpret_cast<uint32_t *>(cfg);
        for (const char *p = spec; *p;) {
            uint64_t off = 0, val = 0;
            if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
            for (; (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'); p++) off = off * 16 + (*p <= '9' ? *p - '0' : *p - 'a' + 10);
            if (*p == '=') p++;
            if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
            for (; (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'); p++) val = val * 16 + (*p <= '9' ? *p - '0' : *p - 'a' + 10);
            if (*p == ',') p++;
            if (off < 0x40 && !(off & 3)) { w[off / 4] = static_cast<uint32_t>(val); rnNote("IOAccelConfig +0x%x = 0x%x (rednavi_accel2_cfg)", (unsigned)off, (unsigned)val); }
            else rnNote("IOAccelConfig: offset 0x%x refused", (unsigned)off);
            if (!*p) break;
        }
        dumpWords("IOAccelConfig after", cfg, 64);
    }
}

bool RedNaviAccel2::configureDevice(IOPCIDevice *pci) {
    rnNote("configureDevice(%s)", pci ? pci->getName() : "?");
    return gStage >= 2;
}

bool RedNaviAccel2::teardownDevice(IOPCIDevice *pci) {
    rnNote("teardownDevice(%s)", pci ? pci->getName() : "?");
    return gStage >= 2;
}
