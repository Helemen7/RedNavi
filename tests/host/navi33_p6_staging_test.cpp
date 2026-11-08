// SPDX-License-Identifier: BSD-3-Clause
// P6.2 PSP memory layout regression: host data only, no real GPU ownership.
#include "navi33_p6_psp_staging.h"
#include <cassert>
#include <cstdio>
#include <cstdint>

namespace st = rn_navi33::p6_staging;
namespace fw = rn_navi33::psp_plan;

static st::Input fixture() {
    fw::VerifiedFile files[fw::kFileCount]{};
    for (unsigned i = 0; i < fw::kFileCount; ++i)
        files[i] = {fw::kFiles[i].size, fw::kFiles[i].sha256};
    const auto transfer = fw::plan(files);
    assert(transfer.error == fw::Error::Ok);

    st::Input in{};
    // Data below are TEST FIXTURES derived from Linux RX7600 measurements;
    // they do not assign macOS GPU memory or verify any real owner.
    in.fbStart = 0x8000000000ull;
    in.fbBytes = 8176ull << 20u;
    in.cpuVisibleBytes = in.fbBytes;
    in.firmwarePlanBytes = transfer.totalBytes;
    in.p6PreflightPassed = true;      // synthetic test assertion
    in.firmwareBytesVerified = true;  // synthetic test assertion
    in.exclusiveVramOwner = true;     // synthetic test assertion only
    in.reservationMapVerified = true; // synthetic test assertion only
    in.reservedCount = 1u;
    in.reserved[0] = {0x1fc000000ull, 19ull << 20u}; // observed Linux TMR

    in.buffers[0] = {0x01000000ull, 4ull << 20u}; // 4 MiB staging
    in.buffers[1] = {0x01400000ull, st::kPage};   // 4 KiB PSP ring
    in.buffers[2] = {0x01401000ull, st::kPage};   // command buffer
    in.buffers[3] = {0x01402000ull, st::kPage};   // fence buffer
    return in;
}

int main() {
    static_assert(!st::kHardwareWritesEnabled, "P6.2 must not touch the GPU");
    st::Input in = fixture();
    assert(in.firmwarePlanBytes == 2801664u);
    auto r = st::validate(in);
    assert(r.error == st::Error::Ok);
    assert(r.placement[0].gpuMcAddress == in.fbStart + in.buffers[0].offset);
    assert(r.placement[1].gpuMcAddress == in.fbStart + in.buffers[1].offset);
    assert(r.placement[3].bytes == st::kPage);
    assert(!r.mayIssuePspCommands);

    in.cpuVisibleBytes = 256ull << 20u; // non-ReBAR CPU aperture
    assert(st::validate(in).error == st::Error::Ok);
    in = fixture();
    in.p6PreflightPassed = false;
    assert(st::validate(in).error == st::Error::NoPreflight);
    in = fixture();
    in.firmwareBytesVerified = false;
    assert(st::validate(in).error == st::Error::NoFirmwareProof);
    in = fixture();
    in.exclusiveVramOwner = false;
    assert(st::validate(in).error == st::Error::NoOwnership);
    in = fixture();
    in.reservationMapVerified = false;
    assert(st::validate(in).error == st::Error::NoReservationMap);
    in = fixture();
    in.reservedCount = 0u;
    assert(st::validate(in).error == st::Error::NoReservationMap);
    in = fixture();
    in.reservedCount = st::kMaxReservedRanges + 1u;
    assert(st::validate(in).error == st::Error::NoReservationMap);
    in = fixture();
    in.fbStart = UINT64_MAX - 1024u;
    assert(st::validate(in).error == st::Error::BadWindow);
    in = fixture();
    in.cpuVisibleBytes = in.fbBytes + 1u;
    assert(st::validate(in).error == st::Error::BadWindow);
    in = fixture();
    in.firmwarePlanBytes = 0u;
    assert(st::validate(in).error == st::Error::BadFirmwareLength);
    in = fixture();
    in.firmwarePlanBytes = fw::kMaxStage + 1u;
    assert(st::validate(in).error == st::Error::BadFirmwareLength);
    in = fixture();
    in.buffers[0].bytes = 1ull << 20u;
    assert(st::validate(in).error == st::Error::BadBufferSize);
    in = fixture();
    in.buffers[1].bytes = 2ull * st::kPage;
    assert(st::validate(in).error == st::Error::BadBufferSize);
    in = fixture();
    in.buffers[1].offset += 1ull;
    assert(st::validate(in).error == st::Error::BadAlignment);
    in = fixture();
    in.buffers[3].offset = in.cpuVisibleBytes;
    assert(st::validate(in).error == st::Error::OutsideVisibleVram);
    in = fixture();
    in.reserved[0].bytes = 0u;
    assert(st::validate(in).error == st::Error::BadReservedRange);
    in = fixture();
    in.reserved[0] = {in.fbBytes - st::kPage, 2ull * st::kPage};
    assert(st::validate(in).error == st::Error::BadReservedRange);
    in = fixture();
    in.buffers[0].offset = in.reserved[0].offset;
    assert(st::validate(in).error == st::Error::ReservedOverlap);
    in = fixture();
    in.buffers[2].offset = in.buffers[1].offset;
    assert(st::validate(in).error == st::Error::BufferOverlap);
    std::puts("Navi33 P6.2 PSP staging layout tests passed (NO GPU I/O)");
    return 0;
}
