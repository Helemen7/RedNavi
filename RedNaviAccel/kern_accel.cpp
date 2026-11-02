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

class RedNaviAccel : public IOAccelerator {
    OSDeclareDefaultStructors(RedNaviAccel)

    IOLock *logLock {nullptr};
    OSArray *log {nullptr};
    uint32_t openCount {0}, callCount {0}, probeCount {0};
    bool link {false};

    void publishLog() {
        setProperty("call-log", log);
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

    // One line in the log; printf-style, kernel formatting.
    void note(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    void noteOpen() { IOLockLock(logLock); openCount++; IOLockUnlock(logLock); }
    void noteCall() { IOLockLock(logLock); callCount++; IOLockUnlock(logLock); }
};

OSDefineMetaClassAndStructors(RedNaviAccel, IOAccelerator)

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
    if (!IOAccelerator::start(provider)) return false;
    uint32_t arg = 0;
    link = PE_parse_boot_argn("rednavi_accel_link", &arg, sizeof(arg)) && arg == 1;
    logLock = IOLockAlloc();
    log = OSArray::withCapacity(256);
    if (!logLock || !log) return false;
    note("start on %s, provider %s", getName(), provider ? provider->getName() : "?");
    probeClasses();
    publishLog();
    registerService();
    return true;
}

void RedNaviAccel::stop(IOService *provider) {
    IOAccelerator::stop(provider);
}

void RedNaviAccel::free() {
    if (log) { log->release(); log = nullptr; }
    if (logLock) { IOLockFree(logLock); logLock = nullptr; }
    IOAccelerator::free();
}

IOReturn RedNaviAccel::message(UInt32 type, IOService *provider, void *argument) {
    note("message(type 0x%x, provider %s)", type, provider ? provider->getName() : "?");
    return IOAccelerator::message(type, provider, argument);
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
    acc->note("%s[%d] open type %u: externalMethod %u, scalar in %u (first 0x%llx), struct in %u (words %08x %08x %08x %08x), scalar out %u, struct out %u%s",
              proc, pid, (unsigned)type, selector, a ? a->scalarInputCount : 0, a && a->scalarInputCount ? a->scalarInput[0] : 0ull, n,
              n >= 4 ? w[0] : 0, n >= 8 ? w[1] : 0, n >= 12 ? w[2] : 0, n >= 16 ? w[3] : 0, a ? a->scalarOutputCount : 0, a ? a->structureOutputSize : 0,
              a && a->asyncWakePort ? ", async" : "");
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
