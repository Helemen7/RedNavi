// RedNaviAccel: the accelerator skeleton of M6 (docs/M6_ACCELERATOR_STUDY.md). It is an IOAccelerator service for the 7800 XT, matched on the PCI device like
// Apple's and NullMoth's accelerators, and it does nothing but look and write down:
//  - at start it asks the OSMetaClass registry for the size and parent of every class of IOAcceleratorFamily2 and IOGraphicsFamily the study lists (property accel-class-info),
//    which needs those classes to be loaded, hence the dependency on the family in the Info.plist;
//  - it hands every client that opens it a user client that logs the call (selector, argument sizes, first words) and returns kIOReturnUnsupported; the log is the property
//    call-log (last 256 entries) and the counters open-count and call-count;
//  - it logs requestProbe(), the call IOFramebuffer::probeAccelerator makes on every IOAccelerator under the framebuffer's PCI device.
// With boot-arg rednavi_accel_link=1 requestProbe also publishes IOAccelIndex and IOAccelTypes on the sibling framebuffers, as Apple's accelerator does (observed on an Intel
// Mac, who writes them is not confirmed): that is what makes WindowServer try the accelerated path, so it is off by default.
// It is not a driver: it renders nothing and RedNaviProbe's engines are not used by it.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOLocks.h>
#include <IOKit/graphics/IOAccelerator.h>
#include "IOGraphicsAccelerator2.h"
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>

class RedNaviAccelClient;

static_assert(sizeof(IOGraphicsAccelerator2) == 3544, "the family class is 3544 bytes in the registry (docs/M6_ACCEL_ABI.md)");

// A subclass of the family's IOGraphicsAccelerator2 (declared in the generated header, 351 virtual slots, 3544 bytes) that implements its 17 pure virtuals as stubs
// which log and fail. It does NOT run the family's own start(): the base members are zero padding here, and the family code behind the hooks (configureDevice, the
// factories) is not available. start(), stop() and free() go to IOService directly. What it tests is whether WindowServer treats an accelerator of that class differently.
class RedNaviAccel : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(RedNaviAccel)

    IOLock *logLock {nullptr};
    OSArray *log {nullptr};
    uint32_t openCount {0}, callCount {0}, probeCount {0};
    bool link {false};
    bool answer {false};       // boot-arg rednavi_accel_answer=1: answer the first two calls of the device client (type 5) instead of refusing them

    // A collection that was made a property of a registry entry is frozen ("Trying to change a collection in the registry" panics in OSCollection::haveUpdated, which
    // is what the first version of this did on its second log line): the working array stays private and a copy is published.
    void publishLog() {
        if (OSArray *snapshot = OSArray::withArray(log)) {
            setProperty("call-log", snapshot);
            snapshot->release();
        }
        setProperty("open-count", openCount, 32);
        setProperty("call-count", callCount, 32);
        setProperty("probe-count", probeCount, 32);
    }
    void probeClasses();
    void linkFramebuffers();

public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
    IOReturn requestProbe(IOOptionBits options) APPLE_KEXT_OVERRIDE;
    IOReturn message(UInt32 type, IOService *provider, void *argument) APPLE_KEXT_OVERRIDE;
#define RN_PURE(R, N, A, Q) R N A Q override;
#include "IOGraphicsAccelerator2_pure.inc"
#undef RN_PURE
    // kxld refuses a subclass whose vtable has fewer entries than its superclass's ("Malformed vtable. Super class ... has 350 entries vs subclass ... with 349"),
    // and counts entries differently from the slot tables of tools/accel_vtable.py (the first build had one slot too few by its count). New virtuals after the family's
    // last slot are harmless: nothing in the family knows them.
    virtual void rnPad0(); virtual void rnPad1(); virtual void rnPad2(); virtual void rnPad3(); virtual void rnPad4(); virtual void rnPad5(); virtual void rnPad6(); virtual void rnPad7();

    // One line in the log; printf-style, kernel formatting.
    void note(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    bool answers() const { return answer; }
    void noteOpen() { IOLockLock(logLock); openCount++; IOLockUnlock(logLock); }
    void noteCall() { IOLockLock(logLock); callCount++; IOLockUnlock(logLock); }
};

OSDefineMetaClassAndStructors(RedNaviAccel, IOGraphicsAccelerator2)

void RedNaviAccel::rnPad0() {} void RedNaviAccel::rnPad1() {} void RedNaviAccel::rnPad2() {} void RedNaviAccel::rnPad3() {}
void RedNaviAccel::rnPad4() {} void RedNaviAccel::rnPad5() {} void RedNaviAccel::rnPad6() {} void RedNaviAccel::rnPad7() {}

// The pure virtuals: the family calls these on a real accelerator; here a call is logged and refused.
#define RN_PURE(R, N, A, Q) R RedNaviAccel::N A Q { note("family hook called: %s", #N); return sizeof(R) == sizeof(IOReturn) ? (R)(intptr_t)kIOReturnUnsupported : (R)0; }
#include "IOGraphicsAccelerator2_pure.inc"
#undef RN_PURE

class RedNaviAccelClient : public IOUserClient {
    OSDeclareDefaultStructors(RedNaviAccelClient)
    RedNaviAccel *acc {nullptr};
    UInt32 type {0};
    char proc[24] {};
    int pid {0};

public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 t) APPLE_KEXT_OVERRIDE;
    void bind(RedNaviAccel *a) { acc = a; }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32 t, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
    IOReturn registerNotificationPort(mach_port_t port, UInt32 t, UInt32 refCon) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    UInt32 openType() const { return type; }
};

OSDefineMetaClassAndStructors(RedNaviAccelClient, IOUserClient)

// ---------------------------------------------------------------------------------------------------------------------------------------------------------- RedNaviAccel

void RedNaviAccel::note(const char *fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    IOLog("RedNaviAccel: %s\n", line);
    if (!log || !logLock) return;
    OSString *s = OSString::withCString(line);
    if (!s) return;
    IOLockLock(logLock);
    if (log->getCount() >= 256) log->removeObject(0);
    log->setObject(s);
    publishLog();
    IOLockUnlock(logLock);
    s->release();
}

// Size and parent of every class the accelerator stack is made of. The sizes are what a subclass of these classes has to be declared with; this is the one number the
// symbol tables do not give (docs/M6_ACCELERATOR_STUDY.md, section 6).
void RedNaviAccel::probeClasses() {
    static const char *const names[] = {
        "OSObject", "IORegistryEntry", "IOService", "IOUserClient", "IOPCIDevice", "IOFramebuffer", "IOAccelerator",
        "IOGraphicsAccelerator2", "IOAccelDevice2", "IOAccelSharedUserClient2", "IOAccelCommandQueue", "IOAccelSurface", "IOAccelSurfaceMTL", "IOAccel2DContext2",
        "IOAccelContext2", "IOAccelCLContext2", "IOAccelGLContext2", "IOAccelVideoContext2", "IOAccelDisplayPipe", "IOAccelDisplayPipeUserClient2",
        "IOAccelDisplayPipeTransaction2", "IOAccelDisplayMachine", "IOAccelEventMachine2", "IOAccelEventMachineFast2", "IOAccelResource2", "IOAccelMemory",
        "IOAccelSysMemory", "IOAccelVidMemory", "IOAccelMemoryMap", "IOAccelRemoteMemory", "IOAccelShared2", "IOAccelSharedNamespace2", "IOAccelNamespace",
        "IOAccelChannel2", "IOAccelFIFOChannel2", "IOAccelSubmitter2", "IOAccelCommandBufferPool2", "IOAccelDrawable2", "IOAccelGLDrawableUserClient", "IOAccelTask",
        "IOAccelStatistics2", "IOAccelTraceStream2", "IOAccelMTLEvent2", "IOAccelEventFence", "IOAccelSharedEventFence", "IOAccelBlockFence", "IOAccelExternalEvent",
        "IOAccelDeviceShmem", "IOAccelMemoryAllocator2", "IOAccelRangeAllocator", "IOAccelKernelMappedMemory", "IOAccelAllocationInfo", "IOAccelLegacySurface",
        "IOAccelLegacyDisplayPipe", "IOAccelLegacyDisplayMachine", "IOAccelClientSharedMachine", "IOAccelFenceMachine", "IOAccelOrphanedMemoryPool",
        "IOAccelOrphanedVidMemoryPool", "IOAccelResidentMemorySet", "IOAccelBufferMemoryDescriptorPool2", "IOAccelMemoryInfoUserClient", "IOAccelGPURestartReport2",
        "IOAccelDisplayPipeGammaTable2", "IOAccelContextEventFence", "IOAccelBlockFencePort2",
    };
    OSDictionary *info = OSDictionary::withCapacity(80);
    OSArray *missing = OSArray::withCapacity(8);
    if (!info || !missing) { OSSafeReleaseNULL(info); OSSafeReleaseNULL(missing); return; }
    for (const char *n : names) {
        const OSSymbol *sym = OSSymbol::withCString(n);
        const OSMetaClass *mc = sym ? OSMetaClass::getMetaClassWithName(sym) : nullptr;
        if (sym) sym->release();
        if (!mc) {
            OSString *s = OSString::withCString(n);
            if (s) { missing->setObject(s); s->release(); }
            continue;
        }
        const OSMetaClass *super = mc->getSuperClass();
        char val[96];
        snprintf(val, sizeof(val), "%u %s", mc->getClassSize(), super ? super->getClassName() : "-");
        OSString *v = OSString::withCString(val);
        if (v) { info->setObject(n, v); v->release(); }
    }
    setProperty("accel-class-info", info);       // "size parent" per class
    setProperty("accel-class-missing", missing);
    IOLog("RedNaviAccel: class sizes published (%u found, %u missing)\n", info->getCount(), missing->getCount());
    info->release();
    missing->release();
}

bool RedNaviAccel::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    uint32_t arg = 0;
    link = PE_parse_boot_argn("rednavi_accel_link", &arg, sizeof(arg)) && arg == 1;
    answer = PE_parse_boot_argn("rednavi_accel_answer", &arg, sizeof(arg)) && arg == 1;
    logLock = IOLockAlloc();
    log = OSArray::withCapacity(256);
    if (!logLock || !log) return false;
    note("start on %s, provider %s", getName(), provider ? provider->getName() : "?");
    probeClasses();
    // Boot-arg rednavi_accel_caps=1: the capability properties the Intel accelerator has on its node (docs/M6_ACCELERATOR_STUDY.md, section 3), to see whether WindowServer
    // then opens the display-pipe clients.
    if (PE_parse_boot_argn("rednavi_accel_caps", &arg, sizeof(arg)) && arg == 1) {
        setProperty("IOAccelRevision", static_cast<uint32_t>(2), 32);
        setProperty("AccelCaps", static_cast<uint32_t>(15), 32);
        if (OSDictionary *pipe = OSDictionary::withCapacity(2)) {
            pipe->setObject("DisplayPipeSupported", kOSBooleanTrue);
            pipe->setObject("TransactionsSupported", kOSBooleanTrue);
            setProperty("IOAccelDisplayPipeCapabilities", pipe);
            pipe->release();
        }
        note("capability properties published");
    }
    publishLog();
    registerService();
    return true;
}

void RedNaviAccel::stop(IOService *provider) {
    IOService::stop(provider);
}

void RedNaviAccel::free() {
    if (log) { log->release(); log = nullptr; }
    if (logLock) { IOLockFree(logLock); logLock = nullptr; }
    IOService::free();
}

IOReturn RedNaviAccel::message(UInt32 type, IOService *provider, void *argument) {
    note("message(type 0x%x, provider %s)", type, provider ? provider->getName() : "?");
    return IOService::message(type, provider, argument);
}

IOReturn RedNaviAccel::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) {
    RedNaviAccelClient *c = OSTypeAlloc(RedNaviAccelClient);
    if (!c) return kIOReturnNoMemory;
    if (!c->initWithTask(owningTask, securityID, type) || !c->attach(this)) {
        c->release();
        return kIOReturnError;
    }
    c->bind(this);
    if (!c->start(this)) {
        c->detach(this);
        c->release();
        return kIOReturnError;
    }
    noteOpen();
    *handler = c;
    return kIOReturnSuccess;
}

IOReturn RedNaviAccel::requestProbe(IOOptionBits options) {
    probeCount++;
    note("requestProbe(options 0x%x)%s", (unsigned)options, link ? ", linking the framebuffers" : "");
    if (link) linkFramebuffers();
    return kIOReturnSuccess;
}

// What the framebuffer of an Intel Mac shows after its accelerator was probed: IOAccelTypes (the accelerator's path in the service plane) and IOAccelIndex.
void RedNaviAccel::linkFramebuffers() {
    IOService *pci = this;
    while ((pci = pci->getProvider())) if (OSDynamicCast(IOPCIDevice, pci)) break;
    if (!pci) return;
    char path[256];
    int len = sizeof(path);
    if (!getPath(path, &len, gIOServicePlane)) return;
    OSString *p = OSString::withCString(path);
    OSIterator *it = IORegistryIterator::iterateOver(pci, gIOServicePlane, kIORegistryIterateRecursively);
    if (!it || !p) { OSSafeReleaseNULL(it); OSSafeReleaseNULL(p); return; }
    while (OSObject *o = it->getNextObject()) {
        IOFramebuffer *fb = OSDynamicCast(IOFramebuffer, o);
        if (!fb) continue;
        fb->setProperty("IOAccelTypes", p);
        fb->setProperty("IOAccelIndex", static_cast<uint32_t>(0), 32);
        note("framebuffer %s: IOAccelTypes and IOAccelIndex set", fb->getName());
    }
    it->release();
    p->release();
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------ RedNaviAccelClient

bool RedNaviAccelClient::initWithTask(task_t owningTask, void *securityID, UInt32 t) {
    if (!IOUserClient::initWithTask(owningTask, securityID, t)) return false;
    type = t;
    pid = proc_selfpid();
    proc_selfname(proc, sizeof(proc));
    return true;
}

IOReturn RedNaviAccelClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a, IOExternalMethodDispatch *, OSObject *, void *) {
    if (!acc) return kIOReturnNotReady;
    acc->noteCall();
    const uint32_t *w = static_cast<const uint32_t *>(a ? a->structureInput : nullptr);
    const uint32_t n = a ? a->structureInputSize : 0;
    char words[96] = "";
    for (uint32_t i = 0, len = 0; i < 8 && i * 4 < n && len < sizeof(words) - 10; i++) len += snprintf(words + len, sizeof(words) - len, "%s%08x", i ? " " : "", w[i]);
    acc->note("%s[%d] open type %u: externalMethod %u, scalar in %u (first 0x%llx), struct in %u (%s), scalar out %u, struct out %u%s",
              proc, pid, (unsigned)type, selector, a ? a->scalarInputCount : 0, a && a->scalarInputCount ? a->scalarInput[0] : 0ull, n, words,
              a ? a->scalarOutputCount : 0, a ? a->structureOutputSize : 0, a && a->asyncWakePort ? ", async" : "");
    // Opt-in answers for what the Metal framework asks first of the device client (type 5), seen in the log: selector 9 with a 16-byte "Metal" string (the API name;
    // the Intel device node has the property API = "Metal") and selector 2 with a 600-byte structure out (I: device information). Zeros for the second; what Metal then asks
    // is what the log is for.
    if (acc->answers() && type == 5 && a) {
        if (selector == 9 && n == 16) return kIOReturnSuccess;
        if (selector == 2 && a->structureOutputSize == 600 && a->structureOutput) { bzero(a->structureOutput, 600); return kIOReturnSuccess; }
    }
    return kIOReturnUnsupported;
}

IOReturn RedNaviAccelClient::clientMemoryForType(UInt32 t, IOOptionBits *, IOMemoryDescriptor **) {
    if (acc) { acc->noteCall(); acc->note("%s[%d] open type %u: clientMemoryForType %u", proc, pid, (unsigned)type, (unsigned)t); }
    return kIOReturnUnsupported;
}

IOReturn RedNaviAccelClient::registerNotificationPort(mach_port_t, UInt32 t, UInt32 refCon) {
    if (acc) { acc->noteCall(); acc->note("%s[%d] open type %u: registerNotificationPort type %u refCon 0x%x", proc, pid, (unsigned)type, (unsigned)t, (unsigned)refCon); }
    return kIOReturnUnsupported;
}

IOReturn RedNaviAccelClient::clientClose() {
    if (acc) acc->note("%s[%d] open type %u: closed", proc, pid, (unsigned)type);
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}
