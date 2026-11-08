#ifndef REDNAVI_NAVI33_BOOTSTRAP_H
#define REDNAVI_NAVI33_BOOTSTRAP_H
// SPDX-License-Identifier: BSD-3-Clause
// Navi 33 live hardware preflight helpers. No GPU I/O occurs in this header.
// Values are derived from checked IP Discovery and the device's own MMHUB
// registers; none are copied from Navi 32's hard-coded 16 GiB address map.
#include <stdint.h>
#include "navi33_runtime.h"

namespace rn_navi33 {

enum class MemoryError : uint8_t {
    Ok, NoVram, BadLocation, WindowTooSmall, InvalidBar
};
struct MemoryWindow {
    MemoryError error = MemoryError::NoVram;
    uint64_t fbStart = 0;
    uint64_t fbEnd = 0; // inclusive
    uint64_t fbBytes = 0;
    uint64_t vramBytes = 0;
    uint64_t cpuVisibleBytes = 0;
    uint32_t fbOffsetRegister = 0; // keep raw; FB_OFFSET is not a VRAM allocation address
    bool valid() const { return error == MemoryError::Ok; }
};
constexpr uint64_t kFbUnit = 1ull << 24; // FB_LOCATION_* in 16 MiB units
inline MemoryWindow makeMemoryWindow(uint32_t fbBase, uint32_t fbTop,
                                     uint32_t fbOffset, uint32_t vramMib,
                                     uint64_t bar0Bytes) {
    MemoryWindow out{};
    out.fbOffsetRegister = fbOffset;
    if (!vramMib || vramMib > 65536u) return out;
    out.vramBytes = static_cast<uint64_t>(vramMib) << 20;
    if ((fbBase & 0xff000000u) || (fbTop & 0xff000000u) || fbTop < fbBase) {
        out.error = MemoryError::BadLocation;
        return out;
    }
    const uint64_t units = static_cast<uint64_t>(fbTop) - fbBase + 1u;
    out.fbStart = static_cast<uint64_t>(fbBase) * kFbUnit;
    out.fbBytes = units * kFbUnit;
    out.fbEnd = out.fbStart + out.fbBytes - 1u;
    // Navi33 Linux reference: physical VRAM/BAR0 = 8192 MiB, but the
    // reported accessible FB range can be 8176 MiB after a 16 MiB tail
    // reservation. Do not reject a full-size Resizable BAR for this.
    // Keep the allowed difference tight until another real ASIC capture
    // establishes whether larger reservations occur.
    constexpr uint64_t kMaxNavi33ReservedTail = 16ull << 20;
    if (out.fbBytes > out.vramBytes ||
        out.vramBytes - out.fbBytes > kMaxNavi33ReservedTail) {
        out.error = MemoryError::WindowTooSmall;
        return out;
    }
    // BAR0 is a CPU aperture; it may cover physical VRAM including the
    // reserved tail. The usable CPU-visible range must exclude that tail.
    if (bar0Bytes < 4096u || bar0Bytes > out.vramBytes) {
        out.error = MemoryError::InvalidBar;
        return out;
    }
    out.cpuVisibleBytes = bar0Bytes < out.fbBytes ? bar0Bytes : out.fbBytes;
    out.error = MemoryError::Ok;
    return out;
}

// Registers in BAR5 are addressed in DWORDs. Do not turn a malformed IP
// discovery segment into an unchecked MMIO access.
inline bool checkedBarDword(uint32_t ipBaseDword, uint32_t registerDword,
                            uint64_t mappedBarBytes, uint32_t *byteOffset) {
    if (!byteOffset) return false;
    const uint64_t off = (static_cast<uint64_t>(ipBaseDword) + registerDword) * 4ull;
    if (off > UINT32_MAX || off > mappedBarBytes ||
        mappedBarBytes - off < 4u) return false;
    *byteOffset = static_cast<uint32_t>(off);
    return true;
}

inline bool hasComputeRegisterBases(const RuntimeReport &r) {
    return r.eligibleForComputeBringup() &&
        r.bases.gc.count >= 4 && r.bases.mp0.count >= 1 &&
        r.bases.mp1.count >= 1 && r.bases.sdma0.count >= 1 &&
        r.bases.mmhub.count >= 2 && r.bases.osssys.count >= 2 &&
        r.bases.nbif.count >= 6;
}
} // namespace rn_navi33
#endif
