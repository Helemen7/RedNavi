#ifndef REDNAVI_NAVI33_P6_PSP_PREFLIGHT_H
#define REDNAVI_NAVI33_P6_PSP_PREFLIGHT_H
// SPDX-License-Identifier: BSD-3-Clause
// Navi33 P6.1: firmware-readiness decision based ONLY on P5 observations.
// No GPU commands, PSP ring creation, VRAM staging, or implicit ownership.
#include <stdint.h>

namespace rn_navi33 { namespace p6_psp {

constexpr bool kHardwareWritesEnabled = false;
constexpr uint32_t kIfwiReady = 1u << 31;

enum class Status : uint32_t {
    IdentityUnverified = 1,
    BasesMissing = 2,
    MemoryUnverified = 3,
    MailboxUnreadable = 4,
    InvalidMailboxValue = 5,
    IfwiNotReady = 6,
    SecureOsRunning = 7,
    NeedsVerifiedFirmware = 8,
};

struct Evidence {
    bool identityVerified = false;
    bool basesReady = false;
    bool memoryVerified = false;
    bool pspRegistersRead = false;
    uint32_t ifwi = 0;
    uint32_t sos = 0;
};

struct Report {
    Status status = Status::IdentityUnverified;
    bool ifwiReady = false;
    bool secureOsAlive = false;
    bool firmwareRequired = false;
    bool mayIssuePspCommands = false; // ALWAYS false in P6.1.
};

inline Report inspect(const Evidence &e) {
    Report r{};
    // Preserve a fail-closed order: never infer PSP readiness from partial
    // IP Discovery, a guessed FB window, or an unreadable mailbox.
    if (!e.identityVerified) return r;
    if (!e.basesReady) { r.status = Status::BasesMissing; return r; }
    if (!e.memoryVerified) { r.status = Status::MemoryUnverified; return r; }
    if (!e.pspRegistersRead) { r.status = Status::MailboxUnreadable; return r; }
    if (e.ifwi == 0xffffffffu || e.sos == 0xffffffffu) {
        r.status = Status::InvalidMailboxValue;
        return r;
    }
    r.ifwiReady = (e.ifwi & kIfwiReady) != 0;
    if (!r.ifwiReady) { r.status = Status::IfwiNotReady; return r; }
    // MP0 C2PMSG_81 contains the secure OS version when the sOS is alive.
    // Nonzero is an observation, NOT a successful PSP ring handshake.
    r.secureOsAlive = e.sos != 0;
    if (r.secureOsAlive) {
        r.status = Status::SecureOsRunning;
        return r;
    }
    // An absent sOS requires signed firmware validation and explicit GPU
    // memory ownership. No loader can be enabled from this report alone.
    r.status = Status::NeedsVerifiedFirmware;
    r.firmwareRequired = true;
    return r;
}

}} // namespace rn_navi33::p6_psp
#endif
