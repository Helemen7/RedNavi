// Host-only IP register-map and VRAM window checks for P5 compute bring-up.
#include "navi33_bootstrap.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace rn_navi33;
static void put16(std::vector<uint8_t>& b, size_t o, uint16_t v) {
    b[o] = static_cast<uint8_t>(v);
    b[o + 1] = static_cast<uint8_t>(v >> 8);
}
static void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    put16(b, o, static_cast<uint16_t>(v));
    put16(b, o + 2, static_cast<uint16_t>(v >> 16));
}
static void finish(std::vector<uint8_t>& b) {
    uint32_t sum = 0;
    for (size_t i = 10; i < b.size(); ++i) sum += b[i];
    put16(b, 8, static_cast<uint16_t>(sum));
}
static std::vector<uint8_t> makeDiscovery(bool version2 = false, bool optional = true) {
    std::vector<uint8_t> b(1024, 0);
    put32(b, 0, 0x28211407);
    put16(b, 4, version2 ? 2 : 1);
    put16(b, 10, static_cast<uint16_t>(b.size()));
    const size_t list = version2 ? 16 : 12;
    if (version2) put16(b, 12, 1);
    put16(b, list, 0x60);
    put32(b, 0x60, 0x53445049);
    put16(b, 0x64, 4);
    put16(b, 0x66, 0x1c0);
    put16(b, 0x6c, 1);
    put16(b, 0x70, 0x100);
    const uint16_t ids[] = {11, 42, 255, 1, 271, 34, 40, 108};
    const uint8_t counts[] = {4, 2, 1, 1, 3, 5, 2, 6};
    const IpVersion versions[] = {kReference.gc, kReference.sdma, kReference.psp,
        kReference.smu, kReference.dcn, kReference.mmhub, {1, 0, 0}, {4, 0, 0}};
    unsigned count = optional ? 8 : 6;
    put16(b, 0x102, static_cast<uint16_t>(count));
    size_t pos = 0x104;
    for (unsigned i = 0; i < count; ++i) {
        put16(b, pos, ids[i]);
        b[pos + 2] = 0;
        b[pos + 3] = counts[i];
        b[pos + 4] = versions[i].major;
        b[pos + 5] = versions[i].minor;
        b[pos + 6] = versions[i].revision;
        for (unsigned j = 0; j < counts[i]; ++j)
            put32(b, pos + 8u + 4u * j, 0x1000u * (i + 1u) + 0x100u * j);
        pos += 8u + 4u * counts[i];
    }
    finish(b);
    return b;
}

int main() {
    for (bool v2 : {false, true}) {
        auto b = makeDiscovery(v2);
        const auto r = inspectRuntimeDiscovery(0x1002, 0x7480, b.data(), b.size());
        assert(r.error == RuntimeError::Ok);
        assert(hasComputeRegisterBases(r));
        assert(r.bases.gc.count == 4 && r.bases.gc.dwords[3] == 0x1300u);
        assert(r.bases.mmhub.count == 5 && r.bases.mmhub.dwords[4] == 0x6400u);
        assert(r.bases.nbif.count == 6 && r.bases.nbif.dwords[5] == 0x8500u);
        b = makeDiscovery(v2, false);
        const auto missingOptional = inspectRuntimeDiscovery(0x1002, 0x7480, b.data(), b.size());
        assert(missingOptional.error == RuntimeError::Ok);
        assert(!hasComputeRegisterBases(missingOptional));
    }

    uint32_t off = 0;
    assert(checkedBarDword(0x100u, 0x8ecu, 0x4000, &off));
    assert(off == (0x100u + 0x8ecu) * 4u);
    assert(!checkedBarDword(0x100u, 0x8ecu, 0x2000, &off));
    assert(!checkedBarDword(UINT32_MAX, 0, 0x100000u, &off));
    assert(!checkedBarDword(0, 0, 4096, nullptr));

    const auto window = makeMemoryWindow(0x8000, 0x81ff, 0, 8192, 0x10000000ull);
    assert(window.valid());
    assert(window.fbStart == 0x8000000000ull);
    assert(window.fbBytes == 8ull * 1024 * 1024 * 1024);
    assert(window.cpuVisibleBytes == 256ull * 1024 * 1024);
    assert(!makeMemoryWindow(0x8000, 0x81ff, 0, 0, 0x10000000).valid());
    assert(makeMemoryWindow(0x8001, 0x8000, 0, 8192, 0x10000000).error == MemoryError::BadLocation);
    assert(makeMemoryWindow(0x8000, 0x80ff, 0, 8192, 0x10000000).error == MemoryError::WindowTooSmall);
    assert(makeMemoryWindow(0x8000, 0x81ff, 0, 8192, 1024).error == MemoryError::InvalidBar);
    std::puts("Navi33 P5 compute preflight tests passed (no GPU commands)");
    return 0;
}
