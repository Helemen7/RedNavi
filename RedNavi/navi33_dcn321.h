#ifndef REDNAVI_NAVI33_DCN321_H
#define REDNAVI_NAVI33_DCN321_H
// SPDX-License-Identifier: BSD-3-Clause
// Navi33 DCN 3.2.1: host-portable display-state inventory and flip preflight.
// Offsets are from Linux dcn_3_2_1_offset.h (DCN segment BASE_IDX 2).
// Referenced against Cobalt64's MIT DCN321 offsets and host tests.
// Never included in the shipping Kext; no MMIO write callback exists.
// Reading a power-gated DCN register can still hang hardware: the caller
// MUST independently identify a live, clocked pipe before invoking snapshot.
#include <stdint.h>

namespace rn_navi33 { namespace dcn321 {

constexpr unsigned kPipes = 4u;
constexpr uint16_t kDevice = 0x7480u;
constexpr uint32_t kDcnVersion = (3u << 16u) | (2u << 8u) | 1u;
constexpr bool kHardwareWritesEnabled = false;
constexpr uint32_t kFrameMask = 0x00ffffffu;

// Exact per-pipe dword offsets (no guessed common stride).
constexpr uint32_t kOtgControl[4] = {0x1b41u,0x1bc1u,0x1c41u,0x1cc1u};
constexpr uint32_t kOtgClock[4] = {0x1b85u,0x1c05u,0x1c85u,0x1d05u};
constexpr uint32_t kOtgLock[4] = {0x1b8au,0x1c0au,0x1c8au,0x1d0au};
constexpr uint32_t kOtgFrame[4] = {0x1b4cu,0x1bccu,0x1c4cu,0x1cccu};
constexpr uint32_t kHubpSurface[4] = {0x05e5u,0x06c1u,0x079du,0x0879u};
constexpr uint32_t kHubpTiling[4] = {0x05e7u,0x06c3u,0x079fu,0x087bu};
constexpr uint32_t kHubpViewport[4] = {0x05eau,0x06c6u,0x07a2u,0x087eu};
constexpr uint32_t kHubpreqPitch[4] = {0x0607u,0x06e3u,0x07bfu,0x089bu};
constexpr uint32_t kHubpreqControl[4] = {0x061au,0x06f6u,0x07d2u,0x08aeu};
constexpr uint32_t kHubpreqFlip[4] = {0x061bu,0x06f7u,0x07d3u,0x08afu};
constexpr uint32_t kHubpreqHigh[4] = {0x060bu,0x06e7u,0x07c3u,0x089fu};
constexpr uint32_t kHubpreqLow[4] = {0x060au,0x06e6u,0x07c2u,0x089eu};

enum class Error : uint8_t {
    Ok, NotNavi33, NotKnownLive, BadAddress, OutOfBAR,
    ReadFailure, NotReady, NoOwnership, UnsupportedLayout, OutOfVram
};
struct Device {
    uint32_t pciDevice = 0, dcnVersion = 0;
    uint64_t segment2Dwords = 0, barDwords = 0;
    bool livePipeKnown = false, safeReadConfirmed = false;
};
struct Reader {
    void *context = nullptr;
    bool (*read32)(void *, uint64_t barByteOffset, uint32_t *) = nullptr;
};
struct Snapshot {
    unsigned pipe = 0;
    uint32_t frame = 0, pitchPixels = 0, pixelFormat = 0, swizzle = 0;
    uint32_t viewportWidth = 0, viewportHeight = 0;
    uint64_t surfaceMcAddress = 0;
    bool otgEnabled = false, clockOn = false, updateLocked = false,
         flipPending = false, dccEnabled = false, tmzEnabled = false,
         rotated = false;
};

constexpr bool frameAdvanced(uint32_t earlier, uint32_t later) {
    return ((later - earlier) & kFrameMask) != 0u;
}

inline Error offset(const Device &d, unsigned pipe, const uint32_t *regs,
                    uint64_t &byteOffset) {
    if (d.pciDevice != kDevice || d.dcnVersion != kDcnVersion)
        return Error::NotNavi33;
    if (pipe >= kPipes || !d.segment2Dwords || !d.barDwords)
        return Error::BadAddress;
    if (d.segment2Dwords > d.barDwords ||
        regs[pipe] >= d.barDwords - d.segment2Dwords)
        return Error::OutOfBAR;
    byteOffset = (d.segment2Dwords + regs[pipe]) * 4ull;
    return Error::Ok;
}

inline Error snapshot(const Device &d, const Reader &reader,
                      unsigned pipe, Snapshot &out) {
    if (d.pciDevice != kDevice || d.dcnVersion != kDcnVersion)
        return Error::NotNavi33;
    if (!d.safeReadConfirmed || !d.livePipeKnown)
        return Error::NotKnownLive;
    if (!reader.read32) return Error::ReadFailure;
    if (pipe >= kPipes) return Error::BadAddress;
    Snapshot s{}; s.pipe = pipe;
    uint32_t ctl=0, clock=0, lock=0, flip=0, surface=0, tiling=0,
             viewport=0, pitch=0, extra=0, lo=0, hi=0;
    // Explicitly read *only* the small set for an independently verified
    // live pipe. No broad DCN scan or automatic retries.
    struct ReadOne { const uint32_t *regs; uint32_t *value; };
    ReadOne regs[] = {
        {kOtgControl, &ctl}, {kOtgClock, &clock}, {kOtgLock, &lock},
        {kOtgFrame, &s.frame}, {kHubpSurface, &surface},
        {kHubpTiling, &tiling}, {kHubpViewport, &viewport},
        {kHubpreqPitch, &pitch}, {kHubpreqControl, &extra},
        {kHubpreqFlip, &flip}, {kHubpreqHigh, &hi},
        {kHubpreqLow, &lo}
    };
    for (const ReadOne &r : regs) {
        uint64_t at=0;
        const Error e = offset(d, pipe, r.regs, at);
        if (e != Error::Ok) return e;
        if (!reader.read32(reader.context, at, r.value)) return Error::ReadFailure;
    }
    s.frame &= kFrameMask;
    s.otgEnabled = (ctl & 1u) != 0u;
    s.clockOn = (clock & 0x100u) != 0u;
    s.updateLocked = (lock & 0x101u) != 0u;
    s.flipPending = (flip & 0x101u) != 0u;
    s.pixelFormat = surface & 0x7fu;
    s.rotated = (surface & 0x700u) != 0u;
    s.swizzle = tiling & 0x1fu;
    s.viewportWidth = viewport & 0x3fffu;
    s.viewportHeight = (viewport >> 16u) & 0x3fffu;
    s.pitchPixels = (pitch & 0x3fffu) + 1u;
    s.dccEnabled = (extra & 2u) != 0u;
    s.tmzEnabled = (extra & 1u) != 0u;
    s.surfaceMcAddress = (uint64_t(hi) << 32u) | lo;
    out=s;
    return Error::Ok;
}

struct Surface {
    uint64_t mcAddress = 0;            // GPU-visible MC address
    uint64_t ownedVramBegin = 0, ownedVramEnd = 0;
    uint32_t pitchPixels = 0, height = 0;
    bool independentlyPinned = false; // caller-provided; not a hardware proof
};
struct PlannedWrite {
    uint64_t barByteOffset = 0;
    uint32_t value = 0;
};
struct FlipPlan {
    Error error = Error::NotReady;
    PlannedWrite high{}, low{};
    bool wouldRequireHardwareWrite = false; // never authorizes a write
};

inline FlipPlan planFlip(const Device &d, unsigned pipe,
                        const Snapshot &s, const Surface &surface) {
    FlipPlan plan{};
    if (d.pciDevice != kDevice || d.dcnVersion != kDcnVersion)
        { plan.error = Error::NotNavi33; return plan; }
    if (pipe >= kPipes || pipe != s.pipe)
        { plan.error = Error::BadAddress; return plan; }
    if (!s.otgEnabled || !s.clockOn || s.updateLocked || s.flipPending)
        { plan.error = Error::NotReady; return plan; }
    if (s.pixelFormat != 8u || s.swizzle || s.rotated ||
        s.dccEnabled || s.tmzEnabled || !s.viewportWidth ||
        !s.viewportHeight || s.viewportWidth > s.pitchPixels)
        { plan.error = Error::UnsupportedLayout; return plan; }
    if (!surface.independentlyPinned)
        { plan.error = Error::NoOwnership; return plan; }
    if (!surface.mcAddress || (surface.mcAddress & 255ull) ||
        !surface.pitchPixels || surface.pitchPixels > 16384u ||
        surface.pitchPixels != s.pitchPixels ||
        surface.height < s.viewportHeight || surface.height > 16383u)
        { plan.error = Error::BadAddress; return plan; }
    const uint64_t length = uint64_t(surface.pitchPixels) * surface.height * 4ull;
    if (!surface.ownedVramBegin || surface.ownedVramBegin >= surface.ownedVramEnd ||
        surface.mcAddress < surface.ownedVramBegin ||
        surface.mcAddress >= surface.ownedVramEnd ||
        length > surface.ownedVramEnd - surface.mcAddress)
        { plan.error = Error::OutOfVram; return plan; }
    uint64_t high=0, low=0;
    Error e=offset(d,pipe,kHubpreqHigh,high);
    if (e != Error::Ok) { plan.error=e; return plan; }
    e=offset(d,pipe,kHubpreqLow,low);
    if (e != Error::Ok) { plan.error=e; return plan; }
    // High-before-low latch as on DCN3.2.1; caller MUST NOT execute this
    // on an active/sole display GPU without independent reset/recovery.
    plan.high = {high, uint32_t(surface.mcAddress >> 32u)};
    plan.low = {low, uint32_t(surface.mcAddress)};
    plan.wouldRequireHardwareWrite = true;
    plan.error=Error::Ok;
    return plan;
}
}} // namespace rn_navi33::dcn321
#endif
