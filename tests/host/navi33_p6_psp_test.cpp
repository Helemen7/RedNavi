// SPDX-License-Identifier: BSD-3-Clause
// P6.1 readiness gate: no firmware bytes, MMIO, or hardware execution.
#include "navi33_p6_psp_preflight.h"
#include <cassert>
#include <cstdio>

using namespace rn_navi33::p6_psp;

int main() {
    static_assert(!kHardwareWritesEnabled, "P6.1 must never write to GPU");
    Evidence e{};
    auto r = inspect(e);
    assert(r.status == Status::IdentityUnverified && !r.mayIssuePspCommands);

    e.identityVerified = true;
    assert(inspect(e).status == Status::BasesMissing);
    e.basesReady = true;
    assert(inspect(e).status == Status::MemoryUnverified);
    e.memoryVerified = true;
    assert(inspect(e).status == Status::MailboxUnreadable);

    e.pspRegistersRead = true;
    e.ifwi = 0xffffffffu;
    assert(inspect(e).status == Status::InvalidMailboxValue);
    e.ifwi = 0x80000000u;
    e.sos = 0xffffffffu;
    assert(inspect(e).status == Status::InvalidMailboxValue);
    e.ifwi = 0u;
    e.sos = 0u;
    assert(inspect(e).status == Status::IfwiNotReady);
    assert(!inspect(e).firmwareRequired);

    e.ifwi = kIfwiReady;
    e.sos = 0x00330024u; // SOS version from 2026-11-08 Linux reference.
    r = inspect(e);
    assert(r.status == Status::SecureOsRunning);
    assert(r.ifwiReady && r.secureOsAlive && !r.firmwareRequired);
    assert(!r.mayIssuePspCommands);

    e.sos = 0u;
    r = inspect(e);
    assert(r.status == Status::NeedsVerifiedFirmware);
    assert(r.ifwiReady && !r.secureOsAlive && r.firmwareRequired);
    assert(!r.mayIssuePspCommands);

    // Rejected evidence always takes priority over an apparently valid sOS.
    e.memoryVerified = false;
    e.sos = 0x00330024u;
    assert(inspect(e).status == Status::MemoryUnverified);
    std::puts("Navi33 P6.1 PSP readiness tests passed (no GPU commands)");
    return 0;
}
