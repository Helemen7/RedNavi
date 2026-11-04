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
#include <IOKit/graphics/IOAccelerator.h>
#include "../RedNaviAccel/IOGraphicsAccelerator2.h"
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>

static_assert(sizeof(IOGraphicsAccelerator2) == 3544, "the family class is 3544 bytes in the registry (docs/M6_ACCEL_ABI.md)");

static IOLock *gLogLock;
static OSArray *gLog;          // private; a copy is published (a collection that is a registry property is frozen)
static uint32_t gStage;

class RedNaviAccel2 : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(RedNaviAccel2)
    // A line of the log, to IOLog and to the provider's property. Safe from any thread that may sleep.
    void note(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    void *allocByName(const char *hook, const char *cls);
    void dumpWords(const char *what, const void *p, uint32_t bytes);
    IOService *prov {nullptr};

public:
    IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    // The 17 pure virtuals, written out because each one logs its own arguments.
    void *getStampMemory(unsigned int *size) override;
    void *newEventMachine() override;
    void *createUserGPUTask() override;
    bool populateAccelConfig(IOAccelConfig *cfg) override;
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

void RedNaviAccel2::note(const char *fmt, ...) {
    char line[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    IOLog("RedNaviAccel2: %s\n", line);
    IOService *p = getProvider();
    if (!p) p = prov;
    if (!gLog || !gLogLock || !p) return;
    OSString *s = OSString::withCString(line);
    if (!s) return;
    IOLockLock(gLogLock);
    if (gLog->getCount() >= 256) gLog->removeObject(0);
    gLog->setObject(s);
    if (OSArray *snapshot = OSArray::withArray(gLog)) {
        p->setProperty("rednavi-accel2-log", snapshot);
        snapshot->release();
    }
    IOLockUnlock(gLogLock);
    s->release();
}

void RedNaviAccel2::dumpWords(const char *what, const void *p, uint32_t bytes) {
    const uint32_t *w = static_cast<const uint32_t *>(p);
    for (uint32_t i = 0; i < bytes / 4; i += 8) {
        char s[96];
        int len = 0;
        for (uint32_t j = i; j < i + 8 && j < bytes / 4; j++) len += snprintf(s + len, sizeof(s) - (size_t)len, "%s%08x", j > i ? " " : "", w[j]);
        note("%s +0x%x: %s", what, i * 4, s);
    }
}

// An instance of the family's concrete class by name, via its metaclass (no header needed). NULL if the class is not registered or is abstract (an abstract class's
// metaclass alloc() returns NULL). The object is not initialised: the family's caller is expected to do that, which is one of the things this stage finds out.
void *RedNaviAccel2::allocByName(const char *hook, const char *cls) {
    if (gStage < 2) { note("%s called (stage %u: refused)", hook, gStage); return nullptr; }
    const OSSymbol *sym = OSSymbol::withCString(cls);
    const OSMetaClass *mc = sym ? OSMetaClass::getMetaClassWithName(sym) : nullptr;
    if (sym) sym->release();
    if (!mc) { note("%s: class %s is not registered", hook, cls); return nullptr; }
    OSObject *o = mc->alloc();
    note("%s: %s size %u -> %p%s", hook, cls, mc->getClassSize(), o, o ? "" : " (abstract)");
    return o;
}

IOService *RedNaviAccel2::probe(IOService *provider, SInt32 *score) {
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_accel2", &arg, sizeof(arg)) || arg == 0) return nullptr;
    gStage = arg;
    if (!gLogLock) gLogLock = IOLockAlloc();
    if (!gLog) gLog = OSArray::withCapacity(256);
    prov = provider;
    note("probe on %s, stage %u", provider ? provider->getName() : "?", gStage);
    return IOService::probe(provider, score);
}

void *RedNaviAccel2::getStampMemory(unsigned int *size) {
    note("getStampMemory(size %p)", size);
    if (gStage >= 2 && size) *size = 0;
    return nullptr;
}

void *RedNaviAccel2::newEventMachine() { return allocByName("newEventMachine", "IOAccelEventMachineFast2"); }
void *RedNaviAccel2::createUserGPUTask() { return allocByName("createUserGPUTask", "IOAccelTask"); }
void *RedNaviAccel2::createKernelGPUTask() { return allocByName("createKernelGPUTask", "IOAccelTask"); }
void *RedNaviAccel2::newDisplayMachine() { return allocByName("newDisplayMachine", "IOAccelDisplayMachine"); }
void *RedNaviAccel2::newGLContext() { return allocByName("newGLContext", "IOAccelGLContext2"); }
void *RedNaviAccel2::newCLContext() { return allocByName("newCLContext", "IOAccelCLContext2"); }
void *RedNaviAccel2::newSurface() { return allocByName("newSurface", "IOAccelSurface"); }
void *RedNaviAccel2::new2DContext() { return allocByName("new2DContext", "IOAccel2DContext2"); }
void *RedNaviAccel2::newVideoContext() { return allocByName("newVideoContext", "IOAccelVideoContext2"); }
void *RedNaviAccel2::newSysMemory() { return allocByName("newSysMemory", "IOAccelSysMemory"); }
void *RedNaviAccel2::newVidMemory() { return allocByName("newVidMemory", "IOAccelVidMemory"); }
void *RedNaviAccel2::newResource() { return allocByName("newResource", "IOAccelResource2"); }
void *RedNaviAccel2::newMemoryMap() { return allocByName("newMemoryMap", "IOAccelMemoryMap"); }

bool RedNaviAccel2::populateAccelConfig(IOAccelConfig *cfg) {
    note("populateAccelConfig(%p)", cfg);
    if (gStage < 2) return false;
    // The structure's size is unknown; 64 bytes is a read that stays inside any plausible one, to see whether the family pre-fills it.
    if (cfg) dumpWords("IOAccelConfig", cfg, 64);
    return true;
}

bool RedNaviAccel2::configureDevice(IOPCIDevice *pci) {
    note("configureDevice(%s)", pci ? pci->getName() : "?");
    return gStage >= 2;
}

bool RedNaviAccel2::teardownDevice(IOPCIDevice *pci) {
    note("teardownDevice(%s)", pci ? pci->getName() : "?");
    return gStage >= 2;
}
