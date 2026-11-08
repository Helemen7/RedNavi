#ifndef REDNAVI_NAVI33_P6_PSP_STAGING_H
#define REDNAVI_NAVI33_P6_PSP_STAGING_H
// SPDX-License-Identifier: BSD-3-Clause
// Navi33 P6.2 host-only PSP buffer placement validator.
// No VRAM allocator, firmware transfer, PSP ring, MMIO, or GPU commands.
// All ownership / reservation assertions come from a future real GPU owner,
// NOT from this planner or from WSL. Never use test offsets on a live GPU.
#include <stdint.h>
#include "navi33_psp_plan.h"

namespace rn_navi33 { namespace p6_staging {

constexpr bool kHardwareWritesEnabled = false;
constexpr uint64_t kPage = 4096ull;
constexpr unsigned kRegionCount = 4u;
constexpr unsigned kMaxReservedRanges = 8u;

enum class Error : uint8_t {
    Ok, NoPreflight, NoFirmwareProof, NoOwnership, NoReservationMap,
    BadWindow, BadFirmwareLength, BadBufferSize, BadAlignment,
    OutsideVisibleVram, BadReservedRange, ReservedOverlap, BufferOverlap
};
enum class Buffer : unsigned { Firmware = 0, Ring = 1, Command = 2, Fence = 3 };
struct Range {
    uint64_t offset = 0; // offset from validated MMHUB FB start, NOT a PCI BAR address
    uint64_t bytes = 0;
};
struct Input {
    uint64_t fbStart = 0;           // validated P5 GPU MC address
    uint64_t fbBytes = 0;           // validated P5 usable FB span
    uint64_t cpuVisibleBytes = 0;   // validated P5 BAR0/FB intersection
    uint32_t firmwarePlanBytes = 0; // P1 transfer-plan total (NOT firmware file bytes)
    bool p6PreflightPassed = false;
    bool firmwareBytesVerified = false; // caller's externally checked SHA-256 assertion
    bool exclusiveVramOwner = false;    // must be proved by a real GPU allocator
    bool reservationMapVerified = false; // explicit external assertion
    Range buffers[kRegionCount]{};      // firmware, ring, command, fence
    Range reserved[kMaxReservedRanges]{};
    unsigned reservedCount = 0;
};
struct Placement {
    uint64_t gpuMcAddress = 0;
    uint64_t bytes = 0;
};
struct Result {
    Error error = Error::NoPreflight;
    Placement placement[kRegionCount]{};
    bool mayIssuePspCommands = false; // ALWAYS false in P6.2 planning phase
};

constexpr bool aligned(uint64_t n) { return (n & (kPage - 1u)) == 0u; }
constexpr bool overlaps(const Range &a, const Range &b) {
    // Only call after validating non-overflowing in-bounds ranges.
    return a.offset < b.offset + b.bytes && b.offset < a.offset + a.bytes;
}
inline Result validate(const Input &in) {
    Result out{};
    if (!in.p6PreflightPassed) return out;
    if (!in.firmwareBytesVerified) { out.error = Error::NoFirmwareProof; return out; }
    if (!in.exclusiveVramOwner) { out.error = Error::NoOwnership; return out; }
    if (!in.reservationMapVerified || in.reservedCount == 0u ||
        in.reservedCount > kMaxReservedRanges) {
        out.error = Error::NoReservationMap; return out;
    }
    if (in.fbStart == 0u || in.fbBytes == 0u || in.fbBytes > UINT64_MAX - in.fbStart ||
        in.cpuVisibleBytes == 0u || in.cpuVisibleBytes > in.fbBytes) {
        out.error = Error::BadWindow; return out;
    }
    if (in.firmwarePlanBytes == 0u ||
        in.firmwarePlanBytes > psp_plan::kMaxStage) {
        out.error = Error::BadFirmwareLength; return out;
    }

    for (unsigned i = 0; i < in.reservedCount; ++i) {
        const Range &p = in.reserved[i];
        if (!p.bytes || !aligned(p.offset) || !aligned(p.bytes) ||
            p.offset > in.fbBytes || p.bytes > in.fbBytes - p.offset) {
            out.error = Error::BadReservedRange; return out;
        }
    }
    for (unsigned i = 0; i < kRegionCount; ++i) {
        const Range &p = in.buffers[i];
        // This is a draft host layout, not the final PSP13 ring format.
        if ((i == 0u && (p.bytes < in.firmwarePlanBytes ||
                         p.bytes > psp_plan::kMaxStage)) ||
            (i != 0u && p.bytes != kPage)) {
            out.error = Error::BadBufferSize; return out;
        }
        if (!aligned(p.offset) || !aligned(p.bytes)) {
            out.error = Error::BadAlignment; return out;
        }
        if (p.offset > in.cpuVisibleBytes ||
            p.bytes > in.cpuVisibleBytes - p.offset) {
            out.error = Error::OutsideVisibleVram; return out;
        }
        for (unsigned j = 0; j < in.reservedCount; ++j) {
            if (overlaps(p, in.reserved[j])) {
                out.error = Error::ReservedOverlap; return out;
            }
        }
        for (unsigned j = 0; j < i; ++j) {
            if (overlaps(p, in.buffers[j])) {
                out.error = Error::BufferOverlap; return out;
            }
        }
    }
    for (unsigned i = 0; i < kRegionCount; ++i) {
        // fbStart + fbBytes was validated against integer overflow above.
        out.placement[i] = {in.fbStart + in.buffers[i].offset, in.buffers[i].bytes};
    }
    out.error = Error::Ok;
    return out;
}
}} // namespace rn_navi33::p6_staging
#endif
