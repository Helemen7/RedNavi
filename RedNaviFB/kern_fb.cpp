// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// RedNaviFB: IOFramebuffer on top of the DCN scanout the probe brings up.
//
// The display hardware is lit by RedNaviProbe (linear XRGB8888). This class tells WindowServer where the surface
// is: an aperture over BAR0, which macOS maps as 256 MB, so the surface lives in the first 256 MB of VRAM. It lists
// the modes of the monitor's EDID (fb-modes, published by the probe) and a mode change is a platform function call
// to the probe ("rednavi-set-mode"), which reprograms the pipe. The cursor is the DCN cursor plane (the probe's platform functions "rednavi-cursor-image" and "rednavi-cursor-state"; boot-arg rednavi_hwcursor=0 keeps the software cursor). There is no gamma and no vertical
// blank interrupt yet. The firmware boot framebuffer (IONDRVFramebuffer) on the same PCI device would be listed by macOS as a
// second display (see docs/DP_BRINGUP.md); it is removed by default (rednavi_kill_boot_fb, mode 4 below; =0 keeps it). IOFramebuffer::terminate panics
// inside Apple's AppleUpstreamUserClient, so the removal bypasses it.
//
// This is a separate kext because IOGraphicsFamily sits in the system kernel collection and OpenCore can only
// link injected kexts against the boot collection. It matches the property RedNaviProbe sets after the replay
// (rednavi-fb-ready, only with the boot-arg rednavi_fb=1) and reads the surface location from the same node.

#include <stdarg.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IOGraphicsInterfaceTypes.h>
#include <pexpert/pexpert.h>

#define kRNFbMaxModes 32

// Experiments that can be switched without reinstalling the kext: boot-arg rednavi_fb_exp=<bit mask> (the boot-args line has a 1024 character limit, one mask is shorter than one name
// each). Bit 0: getTimingInfoForDisplayMode, bit 1: connectFlags, bit 2: IOFBDependentID/Index (also rednavi_fb_dependent=1), bit 3: the attribute answers of the reference
// driver ('wsrv' 1, power through handleEvent, colour and display attributes of the connection), bit 4: vertical blank interrupts with a 60 Hz timer, bit 5: getVRAMRange gives the
// whole aperture and setApertureEnable succeeds.
static uint32_t fbExp() {
    static uint32_t v, read;
    if (!read) { read = 1; PE_parse_boot_argn("rednavi_fb_exp", &v, sizeof(v)); }
    return v;
}
// The class name has to contain "AMD": CoreDisplay classifies the GPU of a framebuffer by substrings of its class name (Intel, AMD, AppleParavirt, NVDA); any other name counts as an unknown
// vendor, GetSystemGPUVendors then finds no GPU it knows and CoreDisplay does not use IOPresentment (the display surface has no IOSurface and WindowServer composes in software).
class AMDRedNaviFB : public IOFramebuffer {
    OSDeclareDefaultStructors(AMDRedNaviFB)

    uint64_t scanoutPhys {0};          // CPU physical address of the surface (BAR0 + offset)
    uint32_t fbWidth {1920}, fbHeight {1080}, fbPitch {7680}, fbRefresh {60};   // the mode on the wire

    struct ModeEntry { uint32_t w, h, refresh, probeIndex; };
    ModeEntry modes[kRNFbMaxModes] {};
    uint32_t modeCount {0};
    uint32_t ghostTries {0};
    uint32_t pmTries {0};
    bool ghostGone {false};
    uint32_t curMode {0};              // index into modes[]; the mode id WindowServer sees is index + 1

    static uint32_t pitchBytes(uint32_t w) { return ((w + 63u) & ~63u) * 4u; }       // matches rn_pitch_pixels in the probe

    // Diagnostics, published as the fb-diag property: what WindowServer asks and whether the surface changes.
    IOTimerEventSource *diagTimer {nullptr};
    IOTimerEventSource *vblTimer {nullptr};                          // at the mode's refresh rate while a vertical blank procedure is registered (bit 4)
    uint64_t vblNext {0};                                            // absolute time of its next tick
    volatile uint64_t vblLast {0}, vblPeriod {0};                    // the last tick and the period, as getVBLTime reports them (WindowServer's display link paces on them)
    // The family registers the vertical blank type more than once (its own handler, which keeps the shared-memory vblTime/vblDelta/vblCount that WindowServer paces on, and others):
    // every registration is kept, each with its own handle, and the timer calls all of them that are enabled.
    struct VblReg { IOFBInterruptProc proc; OSObject *target; void *ref; bool enabled; } vblRegs[4] {};
    IOFBInterruptProc connProc {nullptr}; OSObject *connTarget {nullptr}; void *connRef {nullptr};
    IOMemoryMap *diagMap {nullptr};
    uint32_t calls[8] {};              // 0 aperture, 1 setAttr, 2 getAttr, 3 setConnAttr, 4 getConnAttr, 5 setMode, 6 cursor, 7 other
    uint32_t attrLog[16] {};           // ring of "kind << 28 | low 28 bits of selector"; selectors are fourcc, see attrRaw
    uint32_t attrRaw[16] {};
    uint32_t attrPos {0};

    void note(unsigned kind, uint32_t sel) {
        calls[kind]++;
        attrLog[attrPos & 15] = kind;
        attrRaw[attrPos & 15] = sel;
        attrPos++;
        publishDiag();
    }

    void publishDiag() {
        uint32_t px[16] = {};
        uint32_t sum = 0;
        if (diagMap) {
            volatile uint32_t *fb = reinterpret_cast<volatile uint32_t *>(diagMap->getVirtualAddress());
            for (unsigned i = 0; i < 16; i++) {
                unsigned y = (8 + i * 64) % fbHeight, x = (100 + i * 100) % fbWidth;
                px[i] = fb[y * (fbPitch / 4) + x];
                sum = sum * 31 + px[i];
            }
        }
        OSDictionary *d = OSDictionary::withCapacity(8);
        if (!d) return;
        OSData *c = OSData::withBytes(calls, sizeof(calls));
        OSData *a = OSData::withBytes(attrRaw, sizeof(attrRaw));
        OSData *k = OSData::withBytes(attrLog, sizeof(attrLog));
        OSData *p = OSData::withBytes(px, sizeof(px));
        OSNumber *h = OSNumber::withNumber(sum, 32);
        OSNumber *n = OSNumber::withNumber(attrPos, 32);
        if (c && a && k && p && h && n) {
            d->setObject("calls", c); d->setObject("attr-sel", a); d->setObject("attr-kind", k);
            d->setObject("pixels", p); d->setObject("hash", h); d->setObject("count", n);
            setProperty("fb-diag", d);
        }
        if (c) c->release();
        if (a) a->release();
        if (k) k->release();
        if (p) p->release();
        if (h) h->release();
        if (n) n->release();
        d->release();
    }

    static bool readNumber(IOService *svc, const char *key, uint64_t *out) {
        OSNumber *n = OSDynamicCast(OSNumber, svc->getProperty(key));
        if (!n) return false;
        *out = n->unsigned64BitValue();
        return true;
    }

    // The firmware's framebuffer (GOP/VESA) sits on the same PCI device as an IONDRVFramebuffer. Left alone, macOS lists it as
    // a second display, makes it the main one and puts the desktop on it, where the monitor does not show it.
    // Returns how many it asked to terminate. The boot framebuffer is registered a little after this kext starts, so the timer
    // calls this again every second until it has been found.
    // rednavi_kill_boot_fb=2: leave the stock framebuffer alone (terminating it panics) and terminate only the IODisplayConnect
    // it published for its one mode, which is what macOS turns into the second display. Same-provider framebuffers other than
    // this one are the stock one.
    unsigned removeBootDisplayConnect(IOService *pci) {
        OSDictionary *match = IOService::serviceMatching("IOFramebuffer");
        OSIterator *it = match ? IOService::getMatchingServices(match) : nullptr;
        unsigned fbs = 0, asked = 0;
        if (it) {
            OSObject *o;
            while ((o = it->getNextObject())) {
                IOService *fb = OSDynamicCast(IOService, o);
                if (!fb || fb == this || fb->getProvider() != pci) continue;
                fbs++;
                OSIterator *kids = fb->getChildIterator(gIOServicePlane);
                if (!kids) continue;
                OSObject *k;
                while ((k = kids->getNextObject())) {
                    IOService *kid = OSDynamicCast(IOService, k);
                    if (kid && kid->metaCast("IODisplayConnect")) {
                        IOLog("RedNaviFB: terminating the display connect of the stock framebuffer\n");
                        if (kid->terminate()) asked++;
                    }
                }
                kids->release();
            }
            it->release();
        }
        if (match) match->release();
        char msg[64];
        snprintf(msg, sizeof(msg), "try %u: stock framebuffers %u, display connects terminated %u", ghostTries, fbs, asked);
        OSString *str = OSString::withCString(msg);
        if (str && (asked || ghostTries % 30 == 0)) setProperty("boot-fb-removal", str);
        if (str) str->release();
        return asked;
    }

    // What the panic report of 2026-10-29 and 2026-11-05 show, and what IOGraphics' source says (IOFramebuffer::terminate): the override calls IOService::terminate first, which
    // starts tearing down the framebuffer's children (among them its AppleUpstreamUserClientDriver), and only then delivers kIOFBNotifyTerminated to the registered notifiers. The
    // notifier of that driver is still registered, its target is gone, and fbNotificationOccurred dereferences null. Terminating the child first (mode 3) did not help: the same crash,
    // so the registration outlives the driver. Modes 4 to 6 call IOService::terminate directly, which skips the override and with it the notification:
    //   4 = IOService::terminate(kIOServiceRequired)   5 = the AppleUpstreamUserClientDriver first, then 4   6 = 4 with kIOServiceSynchronous
    // The panic report of 2026-10-29 (earlier, mode 1):: IOFramebuffer::terminate delivers kIOFBNotifyTerminated, and AppleUpstreamUserClientDriver::fbNotificationOccurred
    // of the stock framebuffer's own AppleUpstreamUserClientDriver child dereferences a null pointer (CR2 0, 0x1e bytes into the function). It is the termination notification
    // that crashes, not the removal of the display. rednavi_kill_boot_fb=3 terminates that child first, so that nothing is registered for the notification when the framebuffer goes.
    unsigned terminateUpstreamClients(IOService *fb) {
        OSArray *victims = OSArray::withCapacity(2);
        OSIterator *kids = fb->getChildIterator(gIOServicePlane);
        if (kids && victims) {
            OSObject *k;
            while ((k = kids->getNextObject())) {
                IOService *kid = OSDynamicCast(IOService, k);
                if (kid && strcmp(kid->getMetaClass()->getClassName(), "AppleUpstreamUserClientDriver") == 0) victims->setObject(kid);
            }
        }
        if (kids) kids->release();
        unsigned done = 0;
        for (unsigned i = 0; victims && i < victims->getCount(); i++) {
            IOService *kid = OSDynamicCast(IOService, victims->getObject(i));
            if (!kid) continue;
            IOLog("RedNaviFB: terminating the stock framebuffer's AppleUpstreamUserClientDriver first\n");
            if (kid->terminate(kIOServiceRequired | kIOServiceSynchronous)) done++;
        }
        if (victims) victims->release();
        return done;
    }

    unsigned removeBootFramebuffer(IOService *probe) {
        // On by default, in mode 4 (verified on macOS 15.8.2: one display, no panic, the accelerator registers as before). Terminating the stock IOFramebuffer through
        // IOFramebuffer::terminate (mode 1, also 3) makes IOGraphics notify AppleUpstreamUserClient, which dereferences a null pointer in fbNotificationOccurred and panics.
        // rednavi_kill_boot_fb=0 keeps the firmware framebuffer (it is then a second display); 2 = only its display connect (the second display stays), 3 = its
        // AppleUpstreamUserClientDriver first and then IOFramebuffer::terminate (panics too), 4 = IOService::terminate (the default), 5 = 3's first step and then 4, 6 = 4 synchronous.
        uint32_t kill = 4;
        PE_parse_boot_argn("rednavi_kill_boot_fb", &kill, sizeof(kill));
        if (!kill) return 0;
        IOService *pci = probe ? probe->getProvider() : nullptr;
        if (!pci) return 0;
        if (kill == 2) return removeBootDisplayConnect(pci);
        OSDictionary *match = IOService::serviceMatching("IONDRVFramebuffer");
        OSIterator *it = match ? IOService::getMatchingServices(match) : nullptr;
        unsigned found = 0, asked = 0, other = 0;
        if (it) {
            OSObject *o;
            while ((o = it->getNextObject())) {
                IOService *svc = OSDynamicCast(IOService, o);
                if (!svc) continue;
                found++;
                if (svc->getProvider() == pci) {
                    if (kill == 3) {
                        const unsigned n = terminateUpstreamClients(svc);
                        OSNumber *cn = OSNumber::withNumber(n, 32);
                        if (cn) { setProperty("boot-fb-upstream-clients-terminated", cn); cn->release(); }
                    }
                    if (kill == 5) terminateUpstreamClients(svc);
                    IOLog("RedNaviFB: terminating the firmware boot framebuffer (mode %u)\n", kill);
                    bool ok;
                    if (kill == 4 || kill == 5) ok = svc->IOService::terminate(kIOServiceRequired);                      // not IOFramebuffer::terminate: no kIOFBNotifyTerminated
                    else if (kill == 6) ok = svc->IOService::terminate(kIOServiceRequired | kIOServiceSynchronous);
                    else ok = svc->terminate(kIOServiceRequired);
                    if (ok) asked++;     // required: the clients (WindowServer) must let go
                } else {
                    other++;
                }
            }
            it->release();
        }
        if (match) match->release();
        char msg[64];
        snprintf(msg, sizeof(msg), "try %u: found %u, terminate accepted for %u, other provider %u", ghostTries, found, asked, other);
        OSString *str = OSString::withCString(msg);
        if (str && (asked || ghostTries % 30 == 0)) setProperty("boot-fb-removal", str);
        if (str) str->release();
        return asked;
    }

public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        if (!readNumber(provider, "fb-scanout-phys", &scanoutPhys)) {
            IOLog("RedNaviFB: provider has no fb-scanout-phys\n");
            return false;
        }
        uint64_t v;
        if (readNumber(provider, "fb-width", &v)) fbWidth = static_cast<uint32_t>(v);
        if (readNumber(provider, "fb-height", &v)) fbHeight = static_cast<uint32_t>(v);
        if (readNumber(provider, "fb-pitch", &v)) fbPitch = static_cast<uint32_t>(v);
        if (readNumber(provider, "fb-refresh", &v)) fbRefresh = static_cast<uint32_t>(v);
        uint64_t curIndex = 0;
        readNumber(provider, "fb-current-mode", &curIndex);
        OSData *list = OSDynamicCast(OSData, provider->getProperty("fb-modes"));
        if (list) {
            modeCount = static_cast<uint32_t>(list->getLength() / sizeof(ModeEntry));
            if (modeCount > kRNFbMaxModes) modeCount = kRNFbMaxModes;
            memcpy(modes, list->getBytesNoCopy(), modeCount * sizeof(ModeEntry));
            for (uint32_t i = 0; i < modeCount; i++) if (modes[i].probeIndex == curIndex) curMode = i;
        }
        if (modeCount == 0) {                                    // old probe: the one mode it lit
            modes[0] = {fbWidth, fbHeight, fbRefresh, 0};
            modeCount = 1;
        }
        removeBootFramebuffer(provider);
        {   // CoreDisplay maps a framebuffer to a GPU pipe by this property (a real AppleIntelFramebuffer has 0 here for its first display; 0xffff for an offline one): without it WindowServer logs
            // "Unable to find display pipe for IOFB service" and its Metal compositor has no destination for the display ("Unsupported sw composite destination").
            const uint32_t pipe = 0;
            OSData *pd = OSData::withBytes(&pipe, sizeof(pipe));
            if (pd) { setProperty("AAPL,DisplayPipe", pd); pd->release(); }
        }
        {   // A real framebuffer names the controller it depends on and its head: AppleIntelFramebuffer@0..2 all have the registry ID of one AppleIntelFramebufferController as IOFBDependentID and
            // 0, 1, 2 as IOFBDependentIndex (dev Mac, 2026-11-07). Here the object above is the probe. Boot-arg rednavi_fb_dependent=1.
            uint32_t dep = 0;
            PE_parse_boot_argn("rednavi_fb_dependent", &dep, sizeof(dep));
            if (provider) {    // always, like the reference driver (rednavi_fb_dependent and bit 2 are no longer needed)
                OSNumber *di = OSNumber::withNumber(static_cast<uint64_t>(0), 32);
                OSNumber *dn = OSNumber::withNumber(provider->getRegistryEntryID(), 64);
                if (di) { setProperty("IOFBDependentIndex", di); di->release(); }
                if (dn) { setProperty("IOFBDependentID", dn); dn->release(); }
            }
        }
        if (!IOFramebuffer::start(provider)) return false;
        if (pmregOn()) {
            // No power driver is registered for this framebuffer (IOPowerManagement has no MaxPowerState, CurrentPowerState stays 0, changePowerStateTo does nothing), so the accelerator's
            // display machine never sees it powered on. Two states: off, and on (usable, powered). Boot-arg rednavi_fb_pmreg=1.
            static IOPMPowerState states[2] = {
                {kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
                {kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
            };
            IOReturn pr = registerPowerDriver(this, states, 2);
            OSNumber *rn = OSNumber::withNumber(static_cast<uint64_t>(static_cast<uint32_t>(pr)), 32);
            if (rn) { setProperty("pm-register-result", rn); rn->release(); }
        }
        IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(scanoutPhys, fbPitch * fbHeight, kIODirectionInOut);
        if (md) { diagMap = md->map(kIOMapInhibitCache); md->release(); }
        return true;
    }

    void stop(IOService *provider) APPLE_KEXT_OVERRIDE {
        if (diagTimer) { diagTimer->cancelTimeout(); getWorkLoop()->removeEventSource(diagTimer); diagTimer->release(); diagTimer = nullptr; }
        if (vblTimer) { vblTimer->cancelTimeout(); getWorkLoop()->removeEventSource(vblTimer); vblTimer->release(); vblTimer = nullptr; }
        if (diagMap) { diagMap->release(); diagMap = nullptr; }
        IOFramebuffer::stop(provider);
    }

    // Every distinct (kind, selector) WindowServer uses on the framebuffer with the last answer and value, published as attr-table (24 bytes each: kind, selector, result, count, value):
    // the 16-entry ring of fb-diag only keeps the last calls. Selectors are four characters, read in memory order (the bytes "dpac" are the number 0x63617064).
    struct AttrRec { uint32_t kind, sel; int32_t result; uint32_t count; uint64_t value; };
    AttrRec attrTable[48] {};
    uint32_t attrTableN {0};
    void record(uint32_t kind, uint32_t sel, IOReturn r, uint64_t value) {
        AttrRec *e = nullptr;
        for (uint32_t i = 0; i < attrTableN; i++) if (attrTable[i].kind == kind && attrTable[i].sel == sel) { e = &attrTable[i]; break; }
        if (!e) {   // the slot is reserved atomically: two threads that both saw 47 entries must not both take the 48th
            SInt32 slot = OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&attrTableN));
            if (slot >= 48) { OSDecrementAtomic(reinterpret_cast<volatile SInt32 *>(&attrTableN)); return; }
            e = &attrTable[slot]; e->kind = kind; e->sel = sel;
        }
        e->result = static_cast<int32_t>(r); OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&e->count)); e->value = value;
        uint32_t n = attrTableN > 48 ? 48 : attrTableN;
        OSData *d = OSData::withBytes(attrTable, sizeof(AttrRec) * n);
        if (d) { setProperty("attr-table", d); d->release(); }
    }

    // Experiments without a reboot: the property "attr-override" (OSData, 24-byte entries: kind, selector, result, unused, value as in attr-table) set from user space replaces the answer of
    // that (kind, selector); an empty OSData clears them. tools/rn_fbknob.c writes it.
    AttrRec overrides[16] {};
    uint32_t overrideN {0};
    const AttrRec *overrideFor(uint32_t kind, uint32_t sel) const {
        for (uint32_t i = 0; i < overrideN; i++) if (overrides[i].kind == kind && overrides[i].sel == sel) return &overrides[i];
        return nullptr;
    }
    IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE {
        OSDictionary *d = OSDynamicCast(OSDictionary, properties);
        OSData *o = d ? OSDynamicCast(OSData, d->getObject("attr-override")) : nullptr;
        if (!o) return IOFramebuffer::setProperties(properties);
        const uint32_t n = static_cast<uint32_t>(o->getLength() / sizeof(AttrRec));
        overrideN = n > 16 ? 16 : n;
        if (overrideN) memcpy(overrides, o->getBytesNoCopy(), overrideN * sizeof(AttrRec));
        setProperty("attr-override", o);
        return kIOReturnSuccess;
    }

    // IOFramebuffer.h: a subclass calls handleEvent when it changes power state, kIOFBNotifyWillPowerOff before it leaves the maximum state and kIOFBNotifyDidPowerOn after it enters it; that is
    // what tells the accelerator's display machine the framebuffer is on. Boot-arg rednavi_fb_notify=1.
    // What the base class does about power (boot-arg rednavi_fb_pmtrace=1): the IOService power virtuals are redefined only to note each call, with arguments, in the property pm-trace (the
    // newest 24), and then call IOFramebuffer. docs 2026-11-07: IOPowerManagement of this node has no MaxPowerState, so registerPowerDriver is suspected never to be called.
    static bool pmtraceOn() { static uint32_t v = 0, rd = 0; if (!rd) { rd = 1; PE_parse_boot_argn("rednavi_fb_pmtrace", &v, sizeof(v)); } return v != 0; }
    char pmLines[24][72] {};
    uint32_t pmCount {0};
    void pmNote(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (!pmtraceOn()) return;
        va_list ap; va_start(ap, fmt);
        char *dst = pmLines[pmCount % 24];
        vsnprintf(dst, 72, fmt, ap);
        va_end(ap);
        pmCount++;
        OSArray *a = OSArray::withCapacity(24);
        if (!a) return;
        for (uint32_t i = pmCount > 24 ? pmCount - 24 : 0; i < pmCount; i++) { OSString *l = OSString::withCString(pmLines[i % 24]); if (l) { a->setObject(l); l->release(); } }
        setProperty("pm-trace", a);
        a->release();
    }
    void PMinit() APPLE_KEXT_OVERRIDE { pmNote("%u PMinit", pmCount); IOFramebuffer::PMinit(); }
    void PMstop() APPLE_KEXT_OVERRIDE { pmNote("%u PMstop", pmCount); IOFramebuffer::PMstop(); }
    void joinPMtree(IOService *driver) APPLE_KEXT_OVERRIDE { pmNote("%u joinPMtree(%p)", pmCount, (void *)driver); IOFramebuffer::joinPMtree(driver); }
    IOReturn registerPowerDriver(IOService *controllingDriver, IOPMPowerState *powerStates, unsigned long numberOfStates) APPLE_KEXT_OVERRIDE {
        IOReturn r = IOFramebuffer::registerPowerDriver(controllingDriver, powerStates, numberOfStates);
        pmNote("%u registerPowerDriver(%p, %lu states) -> 0x%x", pmCount, (void *)controllingDriver, numberOfStates, (unsigned)r);
        return r;
    }
    unsigned long initialPowerStateForDomainState(IOPMPowerFlags f) APPLE_KEXT_OVERRIDE { unsigned long r = IOFramebuffer::initialPowerStateForDomainState(f); pmNote("%u initialPowerStateForDomainState(0x%lx) -> %lu", pmCount, (unsigned long)f, r); return r; }
    unsigned long powerStateForDomainState(IOPMPowerFlags f) APPLE_KEXT_OVERRIDE { unsigned long r = IOFramebuffer::powerStateForDomainState(f); pmNote("%u powerStateForDomainState(0x%lx) -> %lu", pmCount, (unsigned long)f, r); return r; }
    IOReturn powerStateWillChangeTo(IOPMPowerFlags c, unsigned long o, IOService *w) APPLE_KEXT_OVERRIDE { IOReturn r = 0; /* base derefs its controller (null here, CR2 0x120): panic */ pmNote("%u powerStateWillChangeTo(0x%lx, %lu) -> %d", pmCount, (unsigned long)c, o, (int)r); return r; }
    IOReturn powerStateDidChangeTo(IOPMPowerFlags c, unsigned long o, IOService *w) APPLE_KEXT_OVERRIDE { IOReturn r = 0; /* same: base needs the controller */ pmNote("%u powerStateDidChangeTo(0x%lx, %lu) -> %d", pmCount, (unsigned long)c, o, (int)r); return r; }

    static uint32_t pmregMode() { static uint32_t v = 0, rd = 0; if (!rd) { rd = 1; PE_parse_boot_argn("rednavi_fb_pmreg", &v, sizeof(v)); } return v; }
    static bool pmregOn() { return pmregMode() != 0; }
    volatile int32_t pmPending {-1};      // -1 nothing, else the power state setPowerState was asked for, to be announced from the timer (handleEvent from the power thread hung the boot: mode 1)
    IOReturn setPowerState(unsigned long ordinal, IOService *device) APPLE_KEXT_OVERRIDE {
        if (!pmregOn()) { pmNote("%u setPowerState(%lu)", pmCount, ordinal); return IOFramebuffer::setPowerState(ordinal, device); }
        // IOFramebuffer.h: call handleEvent on power state changes (WillPowerOff before leaving the maximum state, DidPowerOn after entering it). Mode 1 does it here (the first try: the boot
        // stopped at the splash); mode 2 only notes the state and lets the timer do it.
        pmNote("%u setPowerState(%lu), mode %u", pmCount, ordinal, pmregMode());
        if (pmregMode() == 1) {
            if (ordinal > 0) handleEvent(kIOFBNotifyDidPowerOn);
            else handleEvent(kIOFBNotifyWillPowerOff);
        } else pmPending = static_cast<int32_t>(ordinal);
        OSNumber *on = OSNumber::withNumber(static_cast<uint64_t>(ordinal), 32);
        if (on) { setProperty("pm-set-state", on); on->release(); }
        return kIOPMAckImplied;
    }
    static bool notifyOn() { static uint32_t v = 0, rd = 0; if (!rd) { rd = 1; PE_parse_boot_argn("rednavi_fb_notify", &v, sizeof(v)); } return v != 0; }
    uint32_t notifySent {0};
    void notePower(const char *what, IOReturn r) {
        OSString *str = OSString::withCString(what);
        OSNumber *rn = OSNumber::withNumber(static_cast<uint64_t>(static_cast<uint32_t>(r)), 32);
        char key[24]; snprintf(key, sizeof(key), "notify-%u", notifySent++);
        if (str && notifySent < 8) setProperty(key, str);
        if (rn) setProperty("notify-last-result", rn);
        if (str) str->release();
        if (rn) rn->release();
    }

    IOReturn setAttribute(IOSelect attr, uintptr_t value) APPLE_KEXT_OVERRIDE {
        note(1, attr);
        if (attr == kIOPowerAttribute && notifyOn() && value == 0 && !(fbExp() & 8)) notePower("WillPowerOff (powr 0)", handleEvent(kIOFBNotifyWillPowerOff));
        const AttrRec *ov = overrideFor(1, attr);
        IOReturn r;
        if (ov) r = static_cast<IOReturn>(ov->result);
        else if ((fbExp() & 8) && attr == kIOWindowServerActiveAttribute) r = kIOReturnSuccess;            // swallowed, as the reference driver does
        else if ((fbExp() & 8) && attr == kIOPowerAttribute) {                                             // power: the base first, then the event, and the base's "unsupported" is success
            if (value != 0) { r = IOFramebuffer::setAttribute(attr, value); notePower("DidPowerOn (powr, rednavi_fb_exp 8)", handleEvent(kIOFBNotifyDidPowerOn)); }
            else { notePower("WillPowerOff (powr 0, rednavi_fb_exp 8)", handleEvent(kIOFBNotifyWillPowerOff)); r = IOFramebuffer::setAttribute(attr, value); }
            if (r == kIOReturnUnsupported) r = kIOReturnSuccess;
        }
        else r = IOFramebuffer::setAttribute(attr, value);
        if (attr == kIOPowerAttribute && notifyOn() && value != 0 && !(fbExp() & 8)) notePower("DidPowerOn (powr)", handleEvent(kIOFBNotifyDidPowerOn));
        record(1, attr, r, value);
        return r;
    }
    // Hardware cursor: the probe publishes cursor-hw when it mapped the cursor image; boot-arg rednavi_hwcursor=0 turns it off.
    bool hwCursor() {
        static uint32_t on = 1, read;
        if (!read) { read = 1; uint32_t v; if (PE_parse_boot_argn("rednavi_hwcursor", &v, sizeof(v))) on = v != 0; }
        IOService *probe = getProvider();
        return on && probe && probe->getProperty("cursor-hw") == kOSBooleanTrue;
    }
    uint32_t *cursorBuf {nullptr};                                   // 64 x 64 ARGB, the layout the probe copies into the cursor surface
    IOReturn setCursorImage(void *cursorImage) APPLE_KEXT_OVERRIDE {
        note(6, 1);
        IOService *probe = getProvider();
        if (!hwCursor()) return kIOReturnUnsupported;
        if (!cursorBuf) { cursorBuf = static_cast<uint32_t *>(IOMalloc(64 * 64 * 4)); if (!cursorBuf) return kIOReturnNoMemory; }
        memset(cursorBuf, 0, 64 * 64 * 4);
        IOHardwareCursorDescriptor desc;
        memset(&desc, 0, sizeof(desc));
        desc.majorVersion = kHardwareCursorDescriptorMajorVersion;
        desc.minorVersion = kHardwareCursorDescriptorMinorVersion;
        desc.height = 64; desc.width = 64;
        desc.bitDepth = kIO32ARGBPixelFormat;
        IOHardwareCursorInfo info;
        memset(&info, 0, sizeof(info));
        info.majorVersion = kHardwareCursorInfoMajorVersion;
        info.minorVersion = kHardwareCursorInfoMinorVersion;
        info.hardwareCursorData = reinterpret_cast<UInt8 *>(cursorBuf);
        if (!convertCursorImage(cursorImage, &desc, &info) || info.cursorWidth == 0 || info.cursorHeight == 0 || info.cursorWidth > 64 || info.cursorHeight > 64) {
            probe->callPlatformFunction("rednavi-cursor-state", true, nullptr, nullptr, nullptr, nullptr);          // hide the plane, the software cursor takes over
            return kIOReturnUnsupported;
        }
        // The converter packs the rows with the cursor's own width, the plane reads a pitch of 64 pixels: spread the rows out, last row first so that nothing is overwritten before it is moved.
        if (info.cursorWidth < 64) {
            for (int y = static_cast<int>(info.cursorHeight) - 1; y >= 0; y--) {
                memmove(cursorBuf + y * 64, cursorBuf + y * info.cursorWidth, info.cursorWidth * 4);
                memset(cursorBuf + y * 64 + info.cursorWidth, 0, (64 - info.cursorWidth) * 4);
            }
        }
        return probe->callPlatformFunction("rednavi-cursor-image", true, cursorBuf, reinterpret_cast<void *>(static_cast<uintptr_t>((info.cursorWidth << 16) | info.cursorHeight)), nullptr, nullptr);
    }
    IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) APPLE_KEXT_OVERRIDE {
        note(6, 2);
        if (!hwCursor()) return kIOReturnUnsupported;
        return getProvider()->callPlatformFunction("rednavi-cursor-state", true, reinterpret_cast<void *>(static_cast<intptr_t>(x)), reinterpret_cast<void *>(static_cast<intptr_t>(y)), visible ? reinterpret_cast<void *>(1) : nullptr, nullptr);
    }
    IOReturn getAttribute(IOSelect attr, uintptr_t *value) APPLE_KEXT_OVERRIDE {
        note(2, attr);
        const AttrRec *ov = overrideFor(2, attr);
        IOReturn r;
        if (!ov && attr == kIOHardwareCursorAttribute) { if (value) *value = hwCursor() ? 1 : 0; r = kIOReturnSuccess; }
        else if (ov) { if (value) *value = static_cast<uintptr_t>(ov->value); r = static_cast<IOReturn>(ov->result); }
        else if ((fbExp() & 8) && attr == kIOWindowServerActiveAttribute) { if (value) *value = 1; r = kIOReturnSuccess; }     // as the reference driver does
        else r = IOFramebuffer::getAttribute(attr, value);
        record(2, attr, r, value ? *value : 0);
        return r;
    }
    IOReturn setAttributeForConnection(IOIndex c, IOSelect attr, uintptr_t value) APPLE_KEXT_OVERRIDE {
        note(3, attr);
        const AttrRec *ov3 = overrideFor(3, attr);
        IOReturn r = ov3 ? static_cast<IOReturn>(ov3->result) : (fbExp() & 8) && attr == kConnectionPower ? kIOReturnSuccess : IOFramebuffer::setAttributeForConnection(c, attr, value);
        record(3, attr, r, value);
        return r;
    }
    IOReturn getAttributeForConnection(IOIndex c, IOSelect attr, uintptr_t *value) APPLE_KEXT_OVERRIDE {
        note(4, attr);
        // IODisplay::readFramebufferEDID passes a null value pointer to ask only whether the attribute is supported.
        uintptr_t scratch;
        if (!value) value = &scratch;
        IOReturn r;
        const AttrRec *ov4 = overrideFor(4, attr);
        if (ov4) { *value = static_cast<uintptr_t>(ov4->value); r = static_cast<IOReturn>(ov4->result); record(4, attr, r, *value); return r; }
        if (fbExp() & 8) {      // the answers of the reference driver; the base's for the rest
            switch (attr) {
            case kConnectionColorModesSupported:       *value = 1; record(4, attr, kIOReturnSuccess, *value); return kIOReturnSuccess;
            case kConnectionColorDepthsSupported:      *value = 2; record(4, attr, kIOReturnSuccess, *value); return kIOReturnSuccess;
            case kConnectionControllerColorDepth:      *value = 2; record(4, attr, kIOReturnSuccess, *value); return kIOReturnSuccess;
            case kConnectionControllerDepthsSupported: *value = 2; record(4, attr, kIOReturnSuccess, *value); return kIOReturnSuccess;
            case kConnectionColorMode:                 *value = 1; record(4, attr, kIOReturnSuccess, *value); return kIOReturnSuccess;
            case kConnectionCheckEnable:               *value = 1; record(4, attr, kIOReturnSuccess, *value); return kIOReturnSuccess;
            case kConnectionDisplayFlags:              *value = 0; record(4, attr, kIOReturnSuccess, *value); return kIOReturnSuccess;
            case kConnectionSupportsHLDDCSense:        r = hasDDCConnect(c) ? kIOReturnSuccess : kIOReturnUnsupported; *value = 1; record(4, attr, r, *value); return r;
            case kConnectionSupportsLLDDCSense:
            case kConnectionSupportsAppleSense:
            case kConnectionDisplayParameterCount:
            case kConnectionDisplayParameters:         record(4, attr, kIOReturnUnsupported, 0); return kIOReturnUnsupported;
            default: break;
            }
        }
        switch (attr) {
        case kConnectionFlags:             *value = 0; r = kIOReturnSuccess; break;
        case kConnectionEnable:            *value = 1; r = kIOReturnSuccess; break;
        case kConnectionSupportsHLDDCSense:
        case kConnectionSupportsLLDDCSense: *value = 1; r = kIOReturnSuccess; break;
        case kConnectionSupportsAppleSense: r = kIOReturnUnsupported; break;
        default: r = IOFramebuffer::getAttributeForConnection(c, attr, value); break;
        }
        record(4, attr, r, *value);
        return r;
    }

    // The EDID the probe read through AUX is a property of the probe node ("edid", 128-byte blocks).
    bool hasDDCConnect(IOIndex) APPLE_KEXT_OVERRIDE {
        OSData *e = OSDynamicCast(OSData, getProvider() ? getProvider()->getProperty("edid") : nullptr);
        return e && e->getLength() >= 128;
    }

    IOReturn getDDCBlock(IOIndex, UInt32 block, IOSelect type, IOOptionBits, UInt8 *data, IOByteCount *length) APPLE_KEXT_OVERRIDE {
        OSData *e = OSDynamicCast(OSData, getProvider() ? getProvider()->getProperty("edid") : nullptr);
        if (!e || !data || !length || type != kIODDCBlockTypeEDID || block == 0 || e->getLength() < block * 128u) return kIOReturnUnsupported;
        if (*length > 128) *length = 128;
        memcpy(data, static_cast<const uint8_t *>(e->getBytesNoCopy()) + (block - 1) * 128, *length);
        return kIOReturnSuccess;
    }

    IOReturn enableController() APPLE_KEXT_OVERRIDE {
        calls[7]++;
        IOWorkLoop *wl = getWorkLoop();
        if (wl) {
            diagTimer = IOTimerEventSource::timerEventSource(this, [](OSObject *o, IOTimerEventSource *t) {
                AMDRedNaviFB *self = static_cast<AMDRedNaviFB *>(o);
                if (!self->ghostGone && self->ghostTries < 300) {
                    self->ghostTries++;
                    if (self->removeBootFramebuffer(self->getProvider())) self->ghostGone = true;
                }
                self->publishDiag();
                // Power: a framebuffer at power state 0 never tells the accelerator's display machine it is on ("isFramebufferActive" false: the display pipe is inactive and
                // WindowServer's request for it, client type 4, is refused). A real framebuffer sits at its top state (2). The state is published and, while it is not 2, raised
                // (rednavi_fb_pm=0 leaves it alone).
                if (self->pmPending >= 0) {
                    const int32_t st = self->pmPending;
                    self->pmPending = -1;
                    self->notePower(st > 0 ? "DidPowerOn (setPowerState, timer)" : "WillPowerOff (setPowerState, timer)", self->handleEvent(st > 0 ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff));
                }
                if (notifyOn()) {
                    static uint32_t ticks = 0;
                    ticks++;
                    if (ticks == 45 || ticks == 130) {     // after the accelerator's display machine has started (about 20 s after boot); the second one after its registration (90 s later)
                        self->notePower("WillPowerOn (timer)", self->handleEvent(kIOFBNotifyWillPowerOn));
                        self->notePower("DidPowerOn (timer)", self->handleEvent(kIOFBNotifyDidPowerOn));
                    }
                }
                const uint32_t pm = static_cast<uint32_t>(self->getPowerState());
                OSNumber *pn = OSNumber::withNumber(pm, 32);
                if (pn) { self->setProperty("pm-state", pn); pn->release(); }
                static uint32_t pmArg = 1, pmRead;
                if (!pmRead) { pmRead = 1; PE_parse_boot_argn("rednavi_fb_pm", &pmArg, sizeof(pmArg)); }
                if (pmArg && pm < 2 && self->pmTries < 30) {
                    self->pmTries++;
                    IOReturn pr = self->changePowerStateTo(2);
                    OSNumber *rn = OSNumber::withNumber(static_cast<uint64_t>(static_cast<uint32_t>(pr)), 32);
                    if (rn) { self->setProperty("pm-raise-result", rn); rn->release(); }
                }
                t->setTimeoutMS(1000);
            });
            if (fbExp() & 16) {
                vblTimer = IOTimerEventSource::timerEventSource(this, [](OSObject *o, IOTimerEventSource *t) {
                    AMDRedNaviFB *self = static_cast<AMDRedNaviFB *>(o);
                    static uint32_t ticks;
                    // The period is the active mode's refresh rate and the next tick is an absolute time (the previous one plus a period), so the callback's own latency does not add up
                    // (found by the code review of 2026-11-08: a fixed 16667 us timer made WindowServer pace a 120 Hz mode at 60).
                    uint64_t per = 0, now = 0;
                    nanoseconds_to_absolutetime(1000000000ull / (self->fbRefresh ? self->fbRefresh : 60), &per);
                    clock_get_uptime(&now);
                    VblReg regs[4]; memcpy(regs, self->vblRegs, sizeof(regs));
                    self->vblPeriod = per; self->vblLast = self->vblNext ? self->vblNext : now;
                    bool any = false; for (const VblReg &r : regs) if (r.proc && r.enabled) { any = true; r.proc(r.target, r.ref); }
                    if (any) { ticks++; if (ticks == 1 || ticks % 3600 == 0) IOLog("RedNaviFB: vertical blank tick %u\n", ticks); }
                    self->vblNext += per;
                    if (self->vblNext <= now) self->vblNext = now + per;     // fell behind (or the rate changed): start again from now
                    t->wakeAtTime(self->vblNext);
                });
                if (vblTimer) {
                    wl->addEventSource(vblTimer);
                    uint64_t per = 0, now = 0;
                    nanoseconds_to_absolutetime(1000000000ull / (fbRefresh ? fbRefresh : 60), &per);
                    clock_get_uptime(&now);
                    vblNext = now + per;
                    vblTimer->wakeAtTime(vblNext);
                }
            }
            if (diagTimer && wl->addEventSource(diagTimer) == kIOReturnSuccess) diagTimer->setTimeoutMS(1000);
        }
        publishDiag();
        IOLog("RedNaviFB: enabled, %ux%u at physical 0x%llx\n", fbWidth, fbHeight, scanoutPhys);
        return kIOReturnSuccess;
    }

    // Interrupts (bit 4): the reference driver accepts the vertical blank and the connection-changed types and calls the vertical blank procedure from a 60 Hz timer; the other types are
    // unsupported. The reference it is a handle only, so is this one (not a pointer to anything).
    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **interruptRef) APPLE_KEXT_OVERRIDE {
        if (!(fbExp() & 16)) return IOFramebuffer::registerForInterruptType(type, proc, target, ref, interruptRef);
        IOLog("RedNaviFB: registerForInterruptType('%c%c%c%c')\n", (char)(type >> 24), (char)(type >> 16), (char)(type >> 8), (char)type);
        if (type == kIOFBVBLInterruptType) {
            for (unsigned i = 0; i < 4; i++) if (!vblRegs[i].proc) { vblRegs[i] = {proc, target, ref, true}; if (interruptRef) *interruptRef = reinterpret_cast<void *>(static_cast<uintptr_t>(0x100 + i)); return kIOReturnSuccess; }
            return kIOReturnNoResources;
        }
        if (type == kIOFBConnectInterruptType) { connProc = proc; connTarget = target; connRef = ref; if (interruptRef) *interruptRef = reinterpret_cast<void *>(static_cast<uintptr_t>(2)); return kIOReturnSuccess; }
        return kIOReturnUnsupported;
    }
    // The family's own answer comes from state its vertical blank handler keeps; this driver calls the registered procedure from a timer, so report the timer's own tick and period.
    void getVBLTime(AbsoluteTime *time, AbsoluteTime *delta) APPLE_KEXT_OVERRIDE {
        if (!(fbExp() & 16) || !vblLast) { IOFramebuffer::getVBLTime(time, delta); return; }
        if (time) AbsoluteTime_to_scalar(time) = vblLast;
        if (delta) AbsoluteTime_to_scalar(delta) = vblPeriod;
    }
    IOReturn unregisterInterrupt(void *interruptRef) APPLE_KEXT_OVERRIDE {
        if (!(fbExp() & 16)) return IOFramebuffer::unregisterInterrupt(interruptRef);
        const uintptr_t h = reinterpret_cast<uintptr_t>(interruptRef);
        if (h >= 0x100 && h < 0x104) vblRegs[h - 0x100] = {};
        else if (h == 2) { connProc = nullptr; connTarget = nullptr; connRef = nullptr; }
        return kIOReturnSuccess;
    }
    IOReturn setInterruptState(void *interruptRef, UInt32 state) APPLE_KEXT_OVERRIDE {
        if (!(fbExp() & 16)) return IOFramebuffer::setInterruptState(interruptRef, state);
        const uintptr_t h = reinterpret_cast<uintptr_t>(interruptRef);
        if (h >= 0x100 && h < 0x104) vblRegs[h - 0x100].enabled = (state == kEnabledInterruptState);
        return kIOReturnSuccess;
    }
    IOReturn setApertureEnable(IOPixelAperture aperture, IOOptionBits enable) APPLE_KEXT_OVERRIDE {
        if (!(fbExp() & 32)) return IOFramebuffer::setApertureEnable(aperture, enable);
        return kIOReturnSuccess;
    }

    IODeviceMemory *getApertureRange(IOPixelAperture aperture) APPLE_KEXT_OVERRIDE {
        calls[0]++;
        publishDiag();
        if (aperture != kIOFBSystemAperture) return nullptr;
        IODeviceMemory *m = IODeviceMemory::withRange(scanoutPhys, static_cast<IOPhysicalLength>(fbPitch) * fbHeight);
        if (m) m->setTag(kIOMapWriteCombineCache);    // VRAM through the BAR: write-combined, never plain cached
        return m;
    }

    IODeviceMemory *getVRAMRange() APPLE_KEXT_OVERRIDE {
        if (fbExp() & 32) {      // the reference returns the whole BAR that holds the scanout, not the sub-range of it (the 256 MB the machine maps; the scanout sits at the start of the visible VRAM)
            IODeviceMemory *m = IODeviceMemory::withRange(scanoutPhys & ~0x0fffffffULL, 0x10000000);
            if (m) m->setTag(kIOMapWriteCombineCache);
            return m;
        }
        return getApertureRange(kIOFBSystemAperture);
    }

    const char *getPixelFormats() APPLE_KEXT_OVERRIDE { return IO32BitDirectPixels; }

    IOItemCount getDisplayModeCount() APPLE_KEXT_OVERRIDE { return modeCount; }

    IOReturn getDisplayModes(IODisplayModeID *ids) APPLE_KEXT_OVERRIDE {
        for (uint32_t i = 0; i < modeCount; i++) ids[i] = static_cast<IODisplayModeID>(i + 1);
        return kIOReturnSuccess;
    }

    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) APPLE_KEXT_OVERRIDE {
        if (mode < 1 || static_cast<uint32_t>(mode) > modeCount) return kIOReturnUnsupportedMode;
        const ModeEntry &m = modes[mode - 1];
        bzero(info, sizeof(*info));
        info->nominalWidth = m.w;
        info->nominalHeight = m.h;
        info->refreshRate = m.refresh << 16;
        info->maxDepthIndex = 0;
        info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | (mode == 1 ? kDisplayModeDefaultFlag : 0);
        // Experiment (boot-arg rednavi_fb_accbacked=1): a mode the accelerator scans out carries kDisplayModeAcceleratorBackedFlag (IOGraphicsTypes.h); the accelerator's display machine
        // says "framebuffer active" (and its pipe accepts transactions) only for such a mode, if the guess of 2026-11-07 holds. Neither Intel's registry entry nor ours had it.
        static uint32_t accBacked = 0, accRead = 0;
        if (!accRead) { accRead = 1; PE_parse_boot_argn("rednavi_fb_accbacked", &accBacked, sizeof(accBacked)); }
        if (accBacked) info->flags |= kDisplayModeAcceleratorBackedFlag;
        return kIOReturnSuccess;
    }

    // The shipped framebuffer drivers answer these two; the base says unsupported. WindowServer asks getTimingInfoForDisplayMode twice while it starts (dtrace, 2026-11-07). The
    // answer is a zeroed structure with kIOTimingIDInvalid and no flags (the timing itself is in the EDID and the mode list, not here); connectFlags gives the same flags as
    // getInformationForDisplayMode. Unverified on hardware until the log shows what WindowServer does next.
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) APPLE_KEXT_OVERRIDE {
        if (!(fbExp() & 1)) return IOFramebuffer::getTimingInfoForDisplayMode(mode, info);
        if (!info || mode < 1 || static_cast<uint32_t>(mode) > modeCount) return kIOReturnUnsupportedMode;
        bzero(info, sizeof(*info));
        info->appleTimingID = kIOTimingIDInvalid;
        info->flags = 0;
        return kIOReturnSuccess;
    }
    IOReturn connectFlags(IOIndex index, IODisplayModeID mode, IOOptionBits *flags) APPLE_KEXT_OVERRIDE {
        if (!(fbExp() & 2)) return IOFramebuffer::connectFlags(index, mode, flags);
        if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
        return kIOReturnSuccess;
    }

    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) APPLE_KEXT_OVERRIDE { return 0; }

    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture,
                                 IOPixelInformation *pi) APPLE_KEXT_OVERRIDE {
        if (mode < 1 || static_cast<uint32_t>(mode) > modeCount || depth != 0 || aperture != kIOFBSystemAperture) return kIOReturnUnsupportedMode;
        const ModeEntry &m = modes[mode - 1];
        bzero(pi, sizeof(*pi));
        strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
        pi->flags = 0;
        pi->pixelType = kIORGBDirectPixels;
        pi->componentCount = 3;
        pi->bitsPerComponent = 8;
        pi->bitsPerPixel = 32;
        pi->bytesPerRow = pitchBytes(m.w);
        pi->activeWidth = m.w;
        pi->activeHeight = m.h;
        pi->componentMasks[0] = 0x00ff0000;
        pi->componentMasks[1] = 0x0000ff00;
        pi->componentMasks[2] = 0x000000ff;
        return kIOReturnSuccess;
    }

    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) APPLE_KEXT_OVERRIDE {
        if (mode) *mode = static_cast<IODisplayModeID>(curMode + 1);
        if (depth) *depth = 0;
        return kIOReturnSuccess;
    }

    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) APPLE_KEXT_OVERRIDE { return getCurrentDisplayMode(mode, depth); }
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) APPLE_KEXT_OVERRIDE { return kIOReturnSuccess; }

    // A mode change: ask the probe to reprogram the pipe (quiesce, DISPCLK, link training if needed, mode registers),
    // then follow its geometry. WindowServer reads the aperture and pixel information again afterwards.
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) APPLE_KEXT_OVERRIDE {
        calls[5]++;
        if (mode < 1 || static_cast<uint32_t>(mode) > modeCount || depth != 0) return kIOReturnUnsupportedMode;
        if (static_cast<uint32_t>(mode - 1) == curMode) return kIOReturnSuccess;
        IOService *probe = getProvider();
        if (!probe) return kIOReturnNotReady;
        const ModeEntry &m = modes[mode - 1];
        IOLog("RedNaviFB: mode change to %ux%u@%u\n", m.w, m.h, m.refresh);
        IOReturn r = probe->callPlatformFunction("rednavi-set-mode", true, reinterpret_cast<void *>(static_cast<uintptr_t>(m.probeIndex)), nullptr, nullptr, nullptr);
        if (r != kIOReturnSuccess) {
            IOLog("RedNaviFB: the probe refused the mode change (0x%x)\n", r);
            return r;
        }
        curMode = static_cast<uint32_t>(mode - 1);
        fbWidth = m.w; fbHeight = m.h; fbRefresh = m.refresh; fbPitch = pitchBytes(m.w);
        if (diagMap) { diagMap->release(); diagMap = nullptr; }
        IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(scanoutPhys, fbPitch * fbHeight, kIODirectionInOut);
        if (md) { diagMap = md->map(kIOMapInhibitCache); md->release(); }
        return kIOReturnSuccess;
    }
};

OSDefineMetaClassAndStructors(AMDRedNaviFB, IOFramebuffer)
