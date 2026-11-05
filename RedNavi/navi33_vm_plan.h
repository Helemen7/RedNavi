#ifndef REDNAVI_NAVI33_VM_PLAN_H
#define REDNAVI_NAVI33_VM_PLAN_H
// SPDX-License-Identifier: BSD-3-Clause
// gfx1102 / GMC11 four-level GPU-VM entry planner. Host-only: does not
// allocate GPU pages, map BAR0, emit MMIO, flush TLB or own a VMID.
// Based on Linux amdgpu GMC11 PTE/PDE formats; table addresses below are
// caller-provided VRAM offsets, NOT automatically valid GPU MC addresses.
#include <stdint.h>
namespace rn_navi33 { namespace vm_plan {
constexpr uint64_t kPage = 4096ull, kVaLimit = 1ull << 47;
constexpr uint64_t kPteValid = 1ull << 0, kPteSystem = 1ull << 1,
                   kPteExecute = 1ull << 4, kPteRead = 1ull << 5,
                   kPteWrite = 1ull << 6;
constexpr uint64_t kPteVram = kPteValid | kPteExecute | kPteRead | kPteWrite;
constexpr uint64_t kAllowedPteFlags = 0x7full | (7ull << 48);
constexpr bool kHardwareWritesEnabled = false;
constexpr unsigned kEntriesPerTable = 512u;
constexpr unsigned slot(uint64_t va, unsigned level) {
    return static_cast<unsigned>((va >> (12u + level * 9u)) & 511u);
}
struct Bounds {
    uint64_t vramBytes = 0;
    uint64_t cpuVisibleBytes = 0;
    uint64_t forbiddenBegin = 0, forbiddenEnd = 0;
    bool externallyOwned = false; // assertion from future owner, NOT proven by planner
};
struct Pages {
    uint64_t pdb2 = 0, pdb1 = 0, pdb0 = 0, ptb = 0, data = 0;
};
enum class Error : uint8_t {
    Ok, NotOwned, BadVa, BadBounds, PageOutsideVram, NotCpuVisible,
    ReservedOverlap, PageAlias, BadFlags
};
struct Entry { uint64_t tableByteOffset = 0, value = 0; unsigned slot = 0; };
struct Plan {
    Error error = Error::BadBounds;
    Entry writes[4]{};
    unsigned count = 0;
    unsigned vmid = 1u; // illustrative context; no VMID assignment performed
};
constexpr bool aligned(uint64_t v) { return (v & (kPage - 1u)) == 0u; }
inline Plan build(uint64_t va, const Pages &p, const Bounds &b, uint64_t flags = kPteVram) {
    Plan plan{};
    if (!b.externallyOwned) { plan.error = Error::NotOwned; return plan; }
    if (!aligned(va) || va >= kVaLimit) { plan.error = Error::BadVa; return plan; }
    if (!b.vramBytes || b.cpuVisibleBytes > b.vramBytes ||
        b.forbiddenBegin > b.forbiddenEnd || b.forbiddenEnd > b.vramBytes) {
        plan.error = Error::BadBounds; return plan;
    }
    if ((flags & kPteValid) == 0 || (flags & ~kAllowedPteFlags) != 0 ||
        (flags & kPteSystem) != 0) {
        // No system pages here: they need IOMMU and a different mapping path.
        plan.error = Error::BadFlags; return plan;
    }
    const uint64_t off[5] = {p.pdb2, p.pdb1, p.pdb0, p.ptb, p.data};
    for (unsigned i = 0; i < 5u; ++i) {
        if (!aligned(off[i]) || b.vramBytes < kPage ||
            off[i] > b.vramBytes - kPage) {
            plan.error = Error::PageOutsideVram; return plan;
        }
        if (i < 4u && (b.cpuVisibleBytes < kPage ||
            off[i] > b.cpuVisibleBytes - kPage)) {
            plan.error = Error::NotCpuVisible; return plan;
        }
        if (off[i] < b.forbiddenEnd && off[i] + kPage > b.forbiddenBegin) {
            plan.error = Error::ReservedOverlap; return plan;
        }
        for (unsigned j = 0; j < i; ++j) {
            if (off[i] == off[j]) { plan.error = Error::PageAlias; return plan; }
        }
    }
    for (unsigned i = 0; i < 4u; ++i) {
        const unsigned level = 3u - i;
        const unsigned entrySlot = slot(va, level);
        const uint64_t entryFlags = i == 3u ? flags : kPteValid;
        plan.writes[i] = {off[i] + 8u * entrySlot, off[i+1u] | entryFlags, entrySlot};
    }
    plan.count = 4u;
    plan.error = Error::Ok;
    return plan;
}
}} // rn_navi33::vm_plan
#endif
