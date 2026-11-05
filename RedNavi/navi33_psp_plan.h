#ifndef REDNAVI_NAVI33_PSP_PLAN_H
#define REDNAVI_NAVI33_PSP_PLAN_H
// SPDX-License-Identifier: BSD-3-Clause
// RX 7600 / PSP 13.0.7 reference transfer plan; HOST-ONLY, no hardware I/O.
// This is an independently authored planner, using the publicly documented
// amdgpu firmware interface and observed Cobalt64 RX 7600 test metadata.
// No licensed firmware, ROM dump, kernel driver or GPU write is included.
// Strings identify reference SHA-256 digests; this module DOES NOT compute hashes.
#include <stdint.h>

namespace rn_navi33 { namespace psp_plan {
constexpr unsigned kFileCount = 9u, kRecordCount = 27u;
constexpr uint32_t kPage = 4096u, kMaxStage = 4u*1024u*1024u;
constexpr uint32_t kLoadIpFw = 6u, kAutoloadRlc = 0x21u;
constexpr bool kCanSubmitToGpu = false;
struct FileInfo { const char *name; uint32_t size; const char *sha256; };
constexpr FileInfo kFiles[kFileCount] = {
    {"gc_11_0_2_pfp", 234944u, "9020f53788ad881fa01aa656fc082f9f8d3cdfc81f70aaac0bed6e6001491128"},
    {"gc_11_0_2_me", 324080u, "f5fe48f97acbd3ce13b35929290bfbac01ce522631cc91dcef1ddeb3ff35c8ed"},
    {"gc_11_0_2_mec", 406528u, "19bf080d6e672de5ed3fb86e3fdbdda4d700d8e3bda2dbdcc923101484ad645b"},
    {"gc_11_0_2_rlc", 178848u, "b43eb2fd0600f50a1a5796bc9983d6b39b5c20960234920f5e89cb362193e0b8"},
    {"gc_11_0_2_imu", 132352u, "9befca62b0b0cfd252c3df4a9edca295526f4d43821cd99a6326454995a6ca2d"},
    {"sdma_6_0_2", 34560u, "16c374344d2894da751f7028f9ec1f7520035fa9548d8c750d99a00a4afa86c7"},
    {"gc_11_0_2_mes_2", 263696u, "719f446830ec8766f47b9cfcfcdc60e0767d1a7"},
    {"gc_11_0_2_mes1", 221408u, "5f8f0b50b7a9f8c34f68ee516a51968ed190cb0d0fb36c99e5301799dd65097c"},
    {"smu_13_0_7", 287080u, "6046f6379f42b4d3618151eace413d8c076ea374944afb770a8871ad79fcec55"}
};
struct Record { uint8_t order; uint8_t type; uint8_t file; uint32_t offset; uint32_t length; const char *name; };
constexpr Record kRecords[kRecordCount] = {
    {9, 71, 5, 256u, 17408u, "SDMA_UCODE_TH0"},
    {10, 72, 5, 17664u, 16896u, "SDMA_UCODE_TH1"},
    {15, 87, 0, 256u, 95424u, "CP_RS64_PFP"},
    {16, 88, 1, 256u, 184560u, "CP_RS64_ME"},
    {17, 89, 2, 256u, 267008u, "CP_RS64_MEC"},
    {18, 90, 0, 95680u, 139264u, "CP_RS64_PFP_P0_STACK"},
    {19, 91, 0, 95680u, 139264u, "CP_RS64_PFP_P1_STACK"},
    {20, 92, 1, 184816u, 139264u, "CP_RS64_ME_P0_STACK"},
    {21, 93, 1, 184816u, 139264u, "CP_RS64_ME_P1_STACK"},
    {22, 94, 2, 267264u, 139264u, "CP_RS64_MEC_P0_STACK"},
    {23, 95, 2, 267264u, 139264u, "CP_RS64_MEC_P1_STACK"},
    {24, 96, 2, 267264u, 139264u, "CP_RS64_MEC_P2_STACK"},
    {25, 97, 2, 267264u, 139264u, "CP_RS64_MEC_P3_STACK"},
    {30, 33, 6, 256u, 132368u, "CP_MES"},
    {31, 34, 6, 132624u, 131072u, "CP_MES_DATA"},
    {32, 81, 7, 256u, 90080u, "CP_MES1"},
    {33, 82, 7, 90336u, 131072u, "CP_MES1_DATA"},
    {34, 68, 4, 256u, 66048u, "IMU_I"},
    {35, 69, 4, 66304u, 66048u, "IMU_D"},
    {42, 20, 3, 25344u, 2560u, "RLC_RESTORE_LIST_GPM_MEM"},
    {43, 21, 3, 27904u, 30112u, "RLC_RESTORE_LIST_SRM_MEM"},
    {44, 26, 3, 58016u, 66048u, "RLC_IRAM"},
    {45, 48, 3, 124064u, 33280u, "RLC_DRAM"},
    {48, 25, 3, 157344u, 8704u, "RLC_P"},
    {49, 7, 3, 166048u, 12800u, "RLC_V"},
    {50, 8, 3, 256u, 25088u, "RLC_G"},
    {52, 18, 8, 256u, 261632u, "SMC"}
};

struct VerifiedFile {
    uint32_t size = 0;
    const char *sha256 = nullptr; // externally computed digest, not untrusted filename
};
enum class Error : uint8_t { Ok, MissingDigest, WrongDigest, WrongSize, BadRecord, Overflow };
struct Transfer { uint8_t record; uint32_t gpuStagingOffset; uint32_t paddedBytes; uint32_t opcode; };
struct Plan {
    Error error = Error::MissingDigest;
    uint32_t totalBytes = 0;
    unsigned count = 0;
    Transfer transfers[kRecordCount]{};
};
constexpr uint32_t pageAlign(uint32_t size) { return (size + kPage - 1u) & ~(kPage - 1u); }
inline bool sameDigest(const char *a, const char *b) {
    if (!a || !b) return false;
    for (unsigned i = 0; i < 64u; ++i) {
        if (a[i] == 0 || b[i] == 0 || a[i] != b[i]) return false;
    }
    return a[64] == 0 && b[64] == 0;
}
inline Plan plan(const VerifiedFile (&inputs)[kFileCount]) {
    Plan result{};
    for (unsigned i = 0; i < kFileCount; ++i) {
        if (!inputs[i].sha256) { result.error = Error::MissingDigest; return result; }
        if (inputs[i].size != kFiles[i].size) { result.error = Error::WrongSize; return result; }
        if (!sameDigest(inputs[i].sha256, kFiles[i].sha256)) { result.error = Error::WrongDigest; return result; }
    }
    uint32_t offsets[kRecordCount]{}, lengths[kRecordCount]{}, total = 0;
    for (unsigned i = 0; i < kRecordCount; ++i) {
        const Record &r = kRecords[i];
        if (r.file >= kFileCount || r.length == 0 ||
            r.offset > kFiles[r.file].size ||
            r.length > kFiles[r.file].size - r.offset ||
            (i && r.order <= kRecords[i-1u].order) ||
            (i == kRecordCount-1u && (r.order != 52u || r.type != 18u || r.file != 8u))) {
            result.error = Error::BadRecord; return result;
        }
        const uint32_t len = pageAlign(r.length);
        if (len > kMaxStage - total) { result.error = Error::Overflow; return result; }
        offsets[i] = total; lengths[i] = len; total += len;
    }
    // Linux PSP order: SMU separately first, then other firmware in
    // AMDGPU_UCODE_ID order. AUTOLOAD_RLC is a separate, explicitly gated
    // command and is NEVER generated by this offline plan.
    for (unsigned i = 0; i < kRecordCount; ++i) {
        const unsigned id = i == 0 ? kRecordCount-1u : i-1u;
        result.transfers[i] = {static_cast<uint8_t>(id), offsets[id], lengths[id], kLoadIpFw};
    }
    result.totalBytes = total;
    result.count = kRecordCount;
    result.error = Error::Ok;
    return result;
}
}} // rn_navi33::psp_plan
#endif
