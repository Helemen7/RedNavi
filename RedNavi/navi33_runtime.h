#ifndef REDNAVI_NAVI33_RUNTIME_H
#define REDNAVI_NAVI33_RUNTIME_H
// SPDX-License-Identifier: BSD-3-Clause
// Navi 33 / gfx1102 runtime discovery validator. This has no IOKit dependency.
// It checks actual IP Discovery bytes rather than trusting the PCI device ID.
// A successful match is a prerequisite for later compute bring-up, NOT proof
// that PSP, GART, MES or a graphics/compute queue has been initialized.
#include <stddef.h>
#include <stdint.h>
#include "navi33_facts.h"

namespace rn_navi33 {

enum class RuntimeError : uint8_t {
    Ok, NotNavi33, Truncated, BadSignature, BadSize, BadChecksum,
    UnsupportedHeader, BadIpTable, MissingBlock, IpMismatch, UnsupportedBase
};
// An IP Discovery register base is a DWORD offset, not a byte offset.
struct RegisterBases {
    uint32_t dwords[6]{};
    uint8_t count = 0;
};
struct RuntimeBases {
    RegisterBases gc, sdma0, mp0, mp1, mmhub, osssys, nbif, dmu;
};
struct RuntimeReport {
    RuntimeError error = RuntimeError::Truncated;
    Identity observed{};
    RuntimeBases bases{};
    uint16_t binaryMajor = 0, binaryMinor = 0;
    uint16_t tableVersion = 0;
    uint16_t blocksFound = 0;
    // An eligible ASIC may proceed to dedicated Navi 33 firmware, VM and MES
    // bring-up once these implementations exist. No code should interpret this
    // property as an active queue or a verified firmware load.
    bool eligibleForComputeBringup() const { return error == RuntimeError::Ok; }
};

inline bool byteRange(size_t len, size_t off, size_t n) {
    return off <= len && n <= len - off;
}
inline uint16_t readLe16(const uint8_t *b, size_t off) {
    return static_cast<uint16_t>(b[off] | (static_cast<uint16_t>(b[off + 1]) << 8));
}
inline uint32_t readLe32(const uint8_t *b, size_t off) {
    return static_cast<uint32_t>(readLe16(b, off)) | (static_cast<uint32_t>(readLe16(b, off + 2)) << 16);
}

inline RuntimeReport inspectRuntimeDiscovery(uint16_t vendor, uint16_t device,
                                              const uint8_t *b, size_t bytes) {
    RuntimeReport out{};
    out.observed.vendor = vendor;
    out.observed.device = device;
    if (vendor != 0x1002u || device != 0x7480u) {
        out.error = RuntimeError::NotNavi33;
        return out;
    }
    if (!b || !byteRange(bytes, 0, 16)) return out;
    if (readLe32(b, 0) != 0x28211407u) {
        out.error = RuntimeError::BadSignature;
        return out;
    }
    out.binaryMajor = readLe16(b, 4);
    out.binaryMinor = readLe16(b, 6);
    const uint16_t declaredSize = readLe16(b, 10);
    if (declaredSize < 80 || declaredSize > bytes) {
        out.error = RuntimeError::BadSize;
        return out;
    }
    uint32_t checksum = 0;
    for (size_t i = 10; i < declaredSize; ++i) checksum += b[i];
    if (static_cast<uint16_t>(checksum) != readLe16(b, 8)) {
        out.error = RuntimeError::BadChecksum;
        return out;
    }
    // Linux discovery.h: binary_header v1 has seven fixed table entries at
    // offset 12; v2 has num_tables at 12 and table entries at offset 16.
    size_t tableList = 12;
    unsigned tableCount = 7;
    if (out.binaryMajor == 2) {
        tableCount = readLe16(b, 12);
        tableList = 16;
    } else if (out.binaryMajor != 1) {
        out.error = RuntimeError::UnsupportedHeader;
        return out;
    }
    if (tableCount < 1 || tableCount > 16 || !byteRange(declaredSize, tableList, tableCount * 8u)) {
        out.error = RuntimeError::BadIpTable;
        return out;
    }
    const size_t ipOff = readLe16(b, tableList);
    if (!byteRange(declaredSize, ipOff, 80) ||
        readLe32(b, ipOff) != 0x53445049u) { // "IPDS"
        out.error = RuntimeError::BadIpTable;
        return out;
    }
    out.tableVersion = readLe16(b, ipOff + 4);
    const size_t ipSize = readLe16(b, ipOff + 6);
    if (ipSize < 80 || !byteRange(declaredSize, ipOff, ipSize)) {
        out.error = RuntimeError::BadIpTable;
        return out;
    }
    const size_t ipEnd = ipOff + ipSize;
    const unsigned numDies = readLe16(b, ipOff + 12);
    if (!numDies || numDies > 16) {
        out.error = RuntimeError::BadIpTable;
        return out;
    }
    const bool base64 = out.tableVersion == 4 && (b[ipOff + 78] & 1) != 0;
    const size_t baseWidth = base64 ? 8 : 4;
    bool present[6]{};
    IpVersion *versions[6] = {&out.observed.gc, &out.observed.sdma,
                              &out.observed.psp, &out.observed.smu,
                              &out.observed.dcn, &out.observed.mmhub};
    const uint16_t hardwareIds[6] = {11, 42, 255, 1, 271, 34};
    for (unsigned die = 0; die < numDies; ++die) {
        const size_t dieOff = readLe16(b, ipOff + 14 + die * 4u + 2u);
        if (dieOff < ipOff + 80 || !byteRange(ipEnd, dieOff, 4)) {
            out.error = RuntimeError::BadIpTable;
            return out;
        }
        const unsigned numIps = readLe16(b, dieOff + 2);
        if (numIps > 128) { out.error = RuntimeError::BadIpTable; return out; }
        size_t pos = dieOff + 4;
        for (unsigned ip = 0; ip < numIps; ++ip) {
            if (!byteRange(ipEnd, pos, 8)) {
                out.error = RuntimeError::BadIpTable;
                return out;
            }
            const uint16_t hwId = readLe16(b, pos);
            const uint8_t instance = b[pos + 2];
            const uint8_t nbase = b[pos + 3];
            const size_t entryBytes = 8u + static_cast<size_t>(nbase) * baseWidth;
            if (!byteRange(ipEnd, pos, entryBytes)) {
                out.error = RuntimeError::BadIpTable;
                return out;
            }
            if (instance == 0) {
                RegisterBases *selected = nullptr;
                switch (hwId) {
                    case 11: selected = &out.bases.gc; break;
                    case 42: selected = &out.bases.sdma0; break;
                    case 255: selected = &out.bases.mp0; break;
                    case 1: selected = &out.bases.mp1; break;
                    case 34: selected = &out.bases.mmhub; break;
                    case 40: selected = &out.bases.osssys; break;
                    case 108: selected = &out.bases.nbif; break;
                    case 271: selected = &out.bases.dmu; break;
                    default: break;
                }
                if (selected) {
                    // Strict, bounded extraction: base offsets will drive
                    // physical register reads and later GPU VM bring-up.
                    if (selected->count || nbase == 0 || nbase > 6) {
                        out.error = RuntimeError::BadIpTable;
                        return out;
                    }
                    for (unsigned segment = 0; segment < nbase; ++segment) {
                        const size_t baseAt = pos + 8u + static_cast<size_t>(segment) * baseWidth;
                        if (base64 && readLe32(b, baseAt + 4u) != 0u) {
                            out.error = RuntimeError::UnsupportedBase;
                            return out;
                        }
                        selected->dwords[segment] = readLe32(b, baseAt);
                    }
                    selected->count = nbase;
                }
                for (unsigned k = 0; k < 6; ++k) {
                    if (hwId != hardwareIds[k]) continue;
                    if (present[k] || nbase == 0) {
                        out.error = RuntimeError::BadIpTable;
                        return out;
                    }
                    present[k] = true;
                    *versions[k] = {b[pos + 4], b[pos + 5], b[pos + 6]};
                    out.blocksFound++;
                }
            }
            pos += entryBytes;
        }
    }
    for (unsigned i = 0; i < 6; ++i) {
        if (!present[i]) { out.error = RuntimeError::MissingBlock; return out; }
    }
    out.error = matches(out.observed) ? RuntimeError::Ok : RuntimeError::IpMismatch;
    return out;
}

} // namespace rn_navi33
#endif
