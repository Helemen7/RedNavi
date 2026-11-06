// RedNaviFB: IOFramebuffer on top of the DCN scanout the probe brings up.
//
// The display hardware is lit by RedNaviProbe (linear XRGB8888). This class tells WindowServer where the surface
// is: an aperture over BAR0, which macOS maps as 256 MB, so the surface lives in the first 256 MB of VRAM. It lists
// the modes of the monitor's EDID (fb-modes, published by the probe) and a mode change is a platform function call
// to the probe ("rednavi-set-mode"), which reprograms the pipe. There is no cursor plane, no gamma and no vertical
// blank interrupt yet. The firmware boot framebuffer (IONDRVFramebuffer) on the same PCI device would be listed by macOS as a
// second display (see docs/DP_BRINGUP.md); it is removed by default (rednavi_kill_boot_fb, mode 4 below; =0 keeps it). IOFramebuffer::terminate panics
// inside Apple's AppleUpstreamUserClient, so the removal bypasses it.
//
// This is a separate kext because IOGraphicsFamily sits in the system kernel collection and OpenCore can only
// link injected kexts against the boot collection. It matches the property RedNaviProbe sets after the replay
// (rednavi-fb-ready, only with the boot-arg rednavi_fb=1) and reads the surface location from the same node.

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <pexpert/pexpert.h>

#define kRNFbMaxModes 32

class RedNaviFB : public IOFramebuffer {
    OSDeclareDefaultStructors(RedNaviFB)

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
        if (!IOFramebuffer::start(provider)) return false;
        IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(scanoutPhys, fbPitch * fbHeight, kIODirectionInOut);
        if (md) { diagMap = md->map(kIOMapInhibitCache); md->release(); }
        return true;
    }

    void stop(IOService *provider) APPLE_KEXT_OVERRIDE {
        if (diagTimer) { diagTimer->cancelTimeout(); getWorkLoop()->removeEventSource(diagTimer); diagTimer->release(); diagTimer = nullptr; }
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
        if (!e && attrTableN < 48) { e = &attrTable[attrTableN++]; e->kind = kind; e->sel = sel; }
        if (!e) return;
        e->result = static_cast<int32_t>(r); e->count++; e->value = value;
        OSData *d = OSData::withBytes(attrTable, sizeof(AttrRec) * attrTableN);
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

    IOReturn setAttribute(IOSelect attr, uintptr_t value) APPLE_KEXT_OVERRIDE {
        note(1, attr);
        const AttrRec *ov = overrideFor(1, attr);
        IOReturn r = ov ? static_cast<IOReturn>(ov->result) : IOFramebuffer::setAttribute(attr, value);
        record(1, attr, r, value);
        return r;
    }
    IOReturn getAttribute(IOSelect attr, uintptr_t *value) APPLE_KEXT_OVERRIDE {
        note(2, attr);
        const AttrRec *ov = overrideFor(2, attr);
        IOReturn r;
        if (ov) { if (value) *value = static_cast<uintptr_t>(ov->value); r = static_cast<IOReturn>(ov->result); }
        else r = IOFramebuffer::getAttribute(attr, value);
        record(2, attr, r, value ? *value : 0);
        return r;
    }
    IOReturn setAttributeForConnection(IOIndex c, IOSelect attr, uintptr_t value) APPLE_KEXT_OVERRIDE {
        note(3, attr);
        const AttrRec *ov3 = overrideFor(3, attr);
        IOReturn r = ov3 ? static_cast<IOReturn>(ov3->result) : IOFramebuffer::setAttributeForConnection(c, attr, value);
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
                RedNaviFB *self = static_cast<RedNaviFB *>(o);
                if (!self->ghostGone && self->ghostTries < 300) {
                    self->ghostTries++;
                    if (self->removeBootFramebuffer(self->getProvider())) self->ghostGone = true;
                }
                self->publishDiag();
                // Power: a framebuffer at power state 0 never tells the accelerator's display machine it is on ("isFramebufferActive" false: the display pipe is inactive and
                // WindowServer's request for it, client type 4, is refused). A real framebuffer sits at its top state (2). The state is published and, while it is not 2, raised
                // (rednavi_fb_pm=0 leaves it alone).
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
            if (diagTimer && wl->addEventSource(diagTimer) == kIOReturnSuccess) diagTimer->setTimeoutMS(1000);
        }
        publishDiag();
        IOLog("RedNaviFB: enabled, %ux%u at physical 0x%llx\n", fbWidth, fbHeight, scanoutPhys);
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

OSDefineMetaClassAndStructors(RedNaviFB, IOFramebuffer)
