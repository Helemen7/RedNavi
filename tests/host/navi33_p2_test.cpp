// SPDX-License-Identifier: BSD-3-Clause
// Host-only Navi 33 RADV/VA and DCN 3.2.1 tests. No GPU device is touched.
#include "rn_navi33_radv_admission.h"
#include "navi33_dcn321.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace dc = rn_navi33::dcn321;

static void radvTests() {
    static_assert(RN_N33_GC_VERSION == 0x0b0002u, "gfx1102");
    const rn_n33_report good = {
        RN_N33_PCI_VENDOR, RN_N33_PCI_DEVICE,
        RN_N33_GC_VERSION, RN_N33_PCI_DEVICE,
        RN_N33_AMDGPU_FAMILY_GC11, 32u, 4u,
        0x10000ull, RN_N33_47BIT_VA_LIMIT,
        0ull, 0ull, 4096u, 0u
    };
    auto q = rn_n33_inspect_radv(&good);
    assert(q.admission == RN_N33_NO_BACKEND && q.low_only && !q.can_submit);
    assert(q.low_va_first == 0x10000ull && q.low_va_max == RN_N33_47BIT_VA_LIMIT);
    auto x = good;
    x.native_backend_verified = 1u; // Synthetic fixture, never proof of live hardware
    q = rn_n33_inspect_radv(&x);
    assert(q.admission == RN_N33_META_VALID && q.low_only && !q.can_submit);

    x = good; x.pci_device = 0x747eu;
    assert(rn_n33_inspect_radv(&x).admission == RN_N33_NOT_NAVI33);
    x = good; x.gc_version = 0x0b0003u;
    assert(rn_n33_inspect_radv(&x).admission == RN_N33_BAD_GC_VERSION);
    x = good; x.dev_info_device_id = 0x747eu; // Canned Navi32 metadata rejected
    assert(rn_n33_inspect_radv(&x).admission == RN_N33_BAD_DEVICE_INFO);
    x = good; x.dev_info_family = 152u;       // Canned gfx12 metadata rejected
    assert(rn_n33_inspect_radv(&x).admission == RN_N33_BAD_DEVICE_INFO);
    x = good; x.cu_count = 0u;
    assert(rn_n33_inspect_radv(&x).admission == RN_N33_BAD_DEVICE_INFO);
    x = good; x.va_alignment = 1000u;
    assert(rn_n33_inspect_radv(&x).admission == RN_N33_BAD_LOW_VA);
    x = good; x.low_va_first = 0;
    assert(rn_n33_inspect_radv(&x).admission == RN_N33_BAD_LOW_VA);
    x = good; x.low_va_max = RN_N33_47BIT_VA_LIMIT + 4096ull;
    assert(rn_n33_inspect_radv(&x).admission == RN_N33_BAD_LOW_VA);
    x = good; x.high_va_max = 0x100000000ull; // Half-present high range
    assert(rn_n33_inspect_radv(&x).admission == RN_N33_BAD_HIGH_VA);
    x = good; x.high_va_first = 0x1000000ull; x.high_va_max = 0x2000000ull; // Overlap
    assert(rn_n33_inspect_radv(&x).admission == RN_N33_BAD_HIGH_VA);
    x = good; x.high_va_first = 0xffff800000000000ull;
    x.high_va_max = 0xffff800100000000ull;
    q = rn_n33_inspect_radv(&x);
    assert(q.admission == RN_N33_NO_BACKEND && !q.low_only &&
           q.high_va_first == 0xffff800000000000ull && !q.can_submit);
    assert(rn_n33_inspect_radv(nullptr).admission == RN_N33_BAD_DEVICE_INFO);
}
struct FakeBar {
    std::vector<uint32_t> words;
    unsigned reads = 0;
    unsigned failAtRead = 0;
    FakeBar() : words(0x40000u, 0u) {}
};
static bool fakeRead(void *context, uint64_t byteOffset, uint32_t *value) {
    auto &f=*static_cast<FakeBar *>(context);
    ++f.reads;
    if (f.failAtRead && f.failAtRead == f.reads) return false;
    if (!value || (byteOffset & 3ull) ||
        byteOffset / 4u >= f.words.size()) return false;
    *value = f.words[static_cast<size_t>(byteOffset / 4u)];
    return true;
}
static void assign(FakeBar &bar, const dc::Device &d, const uint32_t *r,
                   unsigned pipe, uint32_t value) {
    const uint64_t i=d.segment2Dwords+r[pipe];
    assert(i < bar.words.size());
    bar.words[static_cast<size_t>(i)]=value;
}
static void dcnTests() {
    static_assert(!dc::kHardwareWritesEnabled, "P2 DCN cannot write hardware");
    assert(dc::frameAdvanced(0x00fffffeu, 0x00000002u));
    assert(!dc::frameAdvanced(400u, 400u));
    FakeBar bar{};
    dc::Device d{};
    d.pciDevice = 0x7480u;
    d.dcnVersion = dc::kDcnVersion;
    d.segment2Dwords = 0x34c0u; // from RX7600 IP Discovery capture
    d.barDwords = bar.words.size();
    d.livePipeKnown = true;
    d.safeReadConfirmed = true; // simulated fixture only
    dc::Snapshot output{};
    auto rd=dc::Reader{&bar,&fakeRead};
    const unsigned pipe=2u; // check non-constant per-pipe register offsets
    const uint64_t mc = 0x8000020000ull;
    assign(bar,d,dc::kOtgControl,pipe,1u);
    assign(bar,d,dc::kOtgClock,pipe,0x100u);
    assign(bar,d,dc::kOtgLock,pipe,0u);
    assign(bar,d,dc::kOtgFrame,pipe,0x00fffffdu);
    assign(bar,d,dc::kHubpSurface,pipe,8u);
    assign(bar,d,dc::kHubpTiling,pipe,0u);
    assign(bar,d,dc::kHubpViewport,pipe,(1080u << 16u) | 1920u);
    assign(bar,d,dc::kHubpreqPitch,pipe,1919u);
    assign(bar,d,dc::kHubpreqControl,pipe,0u);
    assign(bar,d,dc::kHubpreqFlip,pipe,0u);
    assign(bar,d,dc::kHubpreqHigh,pipe,static_cast<uint32_t>(mc >> 32u));
    assign(bar,d,dc::kHubpreqLow,pipe,static_cast<uint32_t>(mc));
    assert(dc::snapshot(d,rd,pipe,output) == dc::Error::Ok);
    assert(bar.reads == 12u && output.pipe == pipe);
    assert(output.otgEnabled && output.clockOn && !output.flipPending);
    assert(output.frame == 0x00fffffdu);
    assert(output.pixelFormat == 8u && !output.swizzle);
    assert(output.pitchPixels == 1920u);
    assert(output.viewportWidth == 1920u && output.viewportHeight == 1080u);
    assert(output.surfaceMcAddress == mc);
    dc::Surface s{};
    s.mcAddress=mc+0x100000ull;
    s.ownedVramBegin=0x8000000000ull;
    s.ownedVramEnd=0x8001000000ull;
    s.pitchPixels=1920u;
    s.height=1080u;
    s.independentlyPinned=true; // simulated data, not hardware ownership
    auto flip=dc::planFlip(d,pipe,output,s);
    assert(flip.error == dc::Error::Ok && flip.wouldRequireHardwareWrite);
    assert(flip.high.barByteOffset == 4ull * (d.segment2Dwords+dc::kHubpreqHigh[pipe]));
    assert(flip.low.barByteOffset == 4ull * (d.segment2Dwords+dc::kHubpreqLow[pipe]));
    assert(flip.high.value == uint32_t(s.mcAddress >> 32u));
    assert(flip.low.value == uint32_t(s.mcAddress));
    s.independentlyPinned=false;
    assert(dc::planFlip(d,pipe,output,s).error == dc::Error::NoOwnership);
    s.independentlyPinned=true; s.mcAddress++;
    assert(dc::planFlip(d,pipe,output,s).error == dc::Error::BadAddress);
    s.mcAddress=mc+0x100000ull; s.ownedVramEnd=s.mcAddress+4096u;
    assert(dc::planFlip(d,pipe,output,s).error == dc::Error::OutOfVram);
    s.ownedVramEnd=0x8001000000ull;
    auto changed=output; changed.dccEnabled=true;
    assert(dc::planFlip(d,pipe,changed,s).error == dc::Error::UnsupportedLayout);
    changed=output; changed.flipPending=true;
    assert(dc::planFlip(d,pipe,changed,s).error == dc::Error::NotReady);
    assert(dc::planFlip(d,pipe+1u,output,s).error == dc::Error::BadAddress);
    // A raw read is forbidden unless the external caller separately proves
    // the target pipe is powered and safe to sample.
    const unsigned before=bar.reads;
    d.safeReadConfirmed=false;
    assert(dc::snapshot(d,rd,pipe,output) == dc::Error::NotKnownLive);
    assert(bar.reads == before);
    d.safeReadConfirmed=true;
    d.barDwords=d.segment2Dwords+dc::kHubpViewport[pipe];
    assert(dc::snapshot(d,rd,pipe,output) == dc::Error::OutOfBAR);
    d.barDwords=bar.words.size();
    bar.failAtRead=bar.reads+2u;
    assert(dc::snapshot(d,rd,pipe,output) == dc::Error::ReadFailure);
    d.pciDevice=0x747eu;
    assert(dc::snapshot(d,rd,pipe,output) == dc::Error::NotNavi33);
}
int main() {
    radvTests();
    dcnTests();
    std::puts("Navi33 P2 RADV metadata + DCN321 host tests passed (no GPU I/O)");
    return 0;
}
