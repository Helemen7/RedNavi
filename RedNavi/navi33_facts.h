#ifndef REDNAVI_NAVI33_FACTS_H
#define REDNAVI_NAVI33_FACTS_H

// Read-only Navi 33 reference identity from Linux amdgpu IP Discovery,
// cross-checked against Cobalt64's RX 7600 capture and diagnostic fixture.
// This header is not included by any production kext or device-matching code.
#include <stdint.h>

namespace rn_navi33 {

struct IpVersion {
    uint8_t major, minor, revision;
};

struct Identity {
    uint16_t vendor, device;
    IpVersion gc, sdma, psp, smu, dcn, mmhub;
};

constexpr Identity kReference = {
    0x1002u, 0x7480u,
    {11, 0, 2}, {6, 0, 2}, {13, 0, 7},
    {13, 0, 7}, {3, 2, 1}, {3, 0, 2}
};

constexpr bool same(IpVersion a, IpVersion b) {
    return a.major == b.major && a.minor == b.minor && a.revision == b.revision;
}

// Exact match for host-only diagnostics. Matching identity never authorizes
// firmware loading, VRAM allocation, MMIO writes or queue takeover.
constexpr bool matches(const Identity &observed) {
    return observed.vendor == kReference.vendor &&
           observed.device == kReference.device &&
           same(observed.gc, kReference.gc) &&
           same(observed.sdma, kReference.sdma) &&
           same(observed.psp, kReference.psp) &&
           same(observed.smu, kReference.smu) &&
           same(observed.dcn, kReference.dcn) &&
           same(observed.mmhub, kReference.mmhub);
}

constexpr bool kGpuWritesEnabled = false;

} // namespace rn_navi33

#endif // REDNAVI_NAVI33_FACTS_H
