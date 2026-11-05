#ifndef REDNAVI_NAVI33_MES_PLAN_H
#define REDNAVI_NAVI33_MES_PLAN_H
// SPDX-License-Identifier: BSD-3-Clause
// GC 11.0.2 MES v11 offline plans based on public Linux amdgpu register
// definitions, cross-referenced with Cobalt64 G20/G42 evidence.
// NO queue ownership, firmware start, GPU MMIO, KIQ or command execution.
// These plans are never included in the production RedNavi kext.
#include <stdint.h>
namespace rn_navi33 { namespace mes_plan {
constexpr bool kHardwareWritesEnabled = false;
constexpr uint32_t kGcSegment = 1u, kCpMesCntl = 0x2807u,
                   kCpMesStart = 0x2800u, kCpMesStartHi = 0x289du;
constexpr uint32_t kResetPipe0 = 1u << 16, kResetPipe1 = 1u << 17,
                   kActivePipe0 = 1u << 26, kActivePipe1 = 1u << 27;
constexpr uint32_t kPacket3MapQueues = 0xa2u;
constexpr uint64_t kMaxGpuAddress = (1ull << 48) - 1ull;
constexpr uint32_t kMaxDoorbell = (1u << 26) - 1u;
constexpr uint32_t kMesEngine = 5u, kMesMe = 2u;
constexpr unsigned kSchedulerPipe = 0u, kKiqPipe = 1u;
enum class Error : uint8_t { Ok, NoOwnership, AlreadyActive, NoFirmware, BadAddress, BadQueue, BadDoorbell };
struct RegisterWrite {
    uint32_t segment = 0, dword = 0, value = 0;
    uint8_t grbmMe = 0, grbmPipe = 0xffu;
};
struct EnableInput {
    uint32_t initialControl = 0;
    uint64_t schedulerStart = 0;
    uint64_t kiqStart = 0;
    bool exclusiveOwner = false;
};
struct EnablePlan {
    Error error = Error::NoOwnership;
    RegisterWrite writes[6]{};
    unsigned count = 0;
};
inline EnablePlan makeEnable(const EnableInput &in) {
    EnablePlan out{};
    if (!in.exclusiveOwner) return out;
    if (in.initialControl & (kActivePipe0 | kActivePipe1)) {
        out.error = Error::AlreadyActive; return out;
    }
    if (!in.schedulerStart || !in.kiqStart) {
        out.error = Error::NoFirmware; return out;
    }
    if ((in.schedulerStart & 3ull) || (in.kiqStart & 3ull)) {
        out.error = Error::BadAddress; return out;
    }
    // Read-modify-write of reset bits; later activation deliberately clears
    // all other control bits as Linux mes_v11_0_enable does.
    out.writes[0] = {kGcSegment, kCpMesCntl, in.initialControl | kResetPipe0 | kResetPipe1, 3u, 0xffu};
    const uint64_t starts[2] = {in.schedulerStart >> 2u, in.kiqStart >> 2u};
    for (unsigned p = 0; p < 2u; ++p) {
        out.writes[p*2u+1u] = {kGcSegment, kCpMesStart,
            static_cast<uint32_t>(starts[p]), 3u, static_cast<uint8_t>(p)};
        out.writes[p*2u+2u] = {kGcSegment, kCpMesStartHi,
            static_cast<uint32_t>(starts[p] >> 32u), 3u, static_cast<uint8_t>(p)};
    }
    out.writes[5] = {kGcSegment, kCpMesCntl, kActivePipe0 | kActivePipe1, 3u, 0xffu};
    out.count = 6u; out.error = Error::Ok; return out;
}

struct MapInput {
    uint64_t mqdGpuAddress = 0, wptrGpuAddress = 0;
    uint32_t doorbellIndex = 0;
    uint8_t queue = 0, pipe = 0;
    bool exclusiveOwner = false;
};
struct Packet {
    Error error = Error::NoOwnership;
    uint32_t words[7]{};
    unsigned count = 0;
};
inline Packet makeMapMesQueue(const MapInput &in) {
    Packet p{};
    if (!in.exclusiveOwner) return p;
    if (in.queue >= 8u || in.pipe >= 2u) {
        p.error = Error::BadQueue; return p;
    }
    if (in.doorbellIndex > kMaxDoorbell) {
        p.error = Error::BadDoorbell; return p;
    }
    if (!in.mqdGpuAddress || !in.wptrGpuAddress ||
        in.mqdGpuAddress > kMaxGpuAddress ||
        in.wptrGpuAddress > kMaxGpuAddress ||
        (in.mqdGpuAddress & 3ull) || (in.wptrGpuAddress & 7ull)) {
        p.error = Error::BadAddress; return p;
    }
    // PACKET3(MAP_QUEUES, 5) followed by six DWORDs. This is a
    // **proposed** KIQ command image, NOT a verified Navi 33 queue map.
    p.words[0] = (3u << 30u) | (5u << 16u) | (kPacket3MapQueues << 8u);
    p.words[1] = (static_cast<uint32_t>(in.queue) << 13u) |
                 (static_cast<uint32_t>(in.pipe) << 16u) |
                 (kMesMe << 18u) | (kMesEngine << 26u) | (1u << 29u);
    p.words[2] = in.doorbellIndex << 2u;
    p.words[3] = static_cast<uint32_t>(in.mqdGpuAddress);
    p.words[4] = static_cast<uint32_t>(in.mqdGpuAddress >> 32u);
    p.words[5] = static_cast<uint32_t>(in.wptrGpuAddress);
    p.words[6] = static_cast<uint32_t>(in.wptrGpuAddress >> 32u);
    p.count = 7u; p.error = Error::Ok; return p;
}
}} // rn_navi33::mes_plan
#endif
