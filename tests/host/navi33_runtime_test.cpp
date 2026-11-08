// Synthetic PCI + IP Discovery verification; no GPU access.
#include "navi33_runtime.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace rn_navi33;
static void wr16(std::vector<uint8_t>& b, size_t p, uint16_t v) {
    b[p] = static_cast<uint8_t>(v); b[p + 1] = static_cast<uint8_t>(v >> 8);
}
static void wr32(std::vector<uint8_t>& b, size_t p, uint32_t v) {
    wr16(b, p, static_cast<uint16_t>(v)); wr16(b, p + 2, static_cast<uint16_t>(v >> 16));
}
static void checksum(std::vector<uint8_t>& b) {
    unsigned sum = 0;
    for (size_t i = 10; i < b.size(); ++i) sum += b[i];
    wr16(b, 8, static_cast<uint16_t>(sum));
}
static std::vector<uint8_t> fixture(bool v2 = false) {
    std::vector<uint8_t> b(512, 0);
    wr32(b, 0, 0x28211407u);
    wr16(b, 4, v2 ? 2 : 1);
    wr16(b, 10, static_cast<uint16_t>(b.size()));
    const size_t list = v2 ? 16 : 12;
    if (v2) wr16(b, 12, 1);
    wr16(b, list, 0x60);
    wr32(b, 0x60, 0x53445049u);
    wr16(b, 0x64, 4);
    wr16(b, 0x66, 0x1A0);
    wr16(b, 0x6C, 1);
    wr16(b, 0x60 + 16, 0x100);
    wr16(b, 0x102, 6);
    const uint16_t hw[] = {11, 42, 255, 1, 271, 34};
    const IpVersion versions[] = {kReference.gc, kReference.sdma, kReference.psp,
                                  kReference.smu, kReference.dcn, kReference.mmhub};
    for (unsigned i = 0; i < 6; ++i) {
        size_t p = 0x104 + i * 12u;
        wr16(b, p, hw[i]);
        b[p + 2] = 0;
        b[p + 3] = 1;
        b[p + 4] = versions[i].major;
        b[p + 5] = versions[i].minor;
        b[p + 6] = versions[i].revision;
        wr32(b, p + 8, static_cast<uint32_t>(0x1000u * (i + 1u)));
    }
    checksum(b);
    return b;
}
int main() {
    for (bool v2 : {false, true}) {
        auto b = fixture(v2);
        auto p = inspectRuntimeDiscovery(0x1002, 0x7480, b.data(), b.size());
        assert(p.error == RuntimeError::Ok && p.eligibleForComputeBringup());
        assert(p.blocksFound == 6 && matches(p.observed));
        assert(inspectRuntimeDiscovery(0x1002, 0x747e, b.data(), b.size()).error == RuntimeError::NotNavi33);
        assert(inspectRuntimeDiscovery(0x1002, 0x7480, b.data(), 10).error == RuntimeError::Truncated);
        b[400] ^= 1;
        assert(inspectRuntimeDiscovery(0x1002, 0x7480, b.data(), b.size()).error == RuntimeError::BadChecksum);
        b = fixture(v2); b[0] = 0;
        assert(inspectRuntimeDiscovery(0x1002, 0x7480, b.data(), b.size()).error == RuntimeError::BadSignature);
        b = fixture(v2); b[0x104+6] = 3; checksum(b);
        assert(inspectRuntimeDiscovery(0x1002, 0x7480, b.data(), b.size()).error == RuntimeError::IpMismatch);
        b = fixture(v2); b[0x104+12+0] = 0; b[0x104+12+1] = 0; checksum(b);
        assert(inspectRuntimeDiscovery(0x1002, 0x7480, b.data(), b.size()).error == RuntimeError::MissingBlock);
        b = fixture(v2); b[0x104+3] = 255; checksum(b);
        assert(inspectRuntimeDiscovery(0x1002, 0x7480, b.data(), b.size()).error == RuntimeError::BadIpTable);
    }
    puts("Navi33 P4 runtime discovery tests passed (no GPU I/O)");
}
