// SPDX-License-Identifier: BSD-3-Clause
// P1 Navi33 host-only regression coverage. No IOKit, firmware bytes, or GPU I/O.
#include "navi33_facts.h"
#include "navi33_psp_plan.h"
#include "navi33_vm_plan.h"
#include "navi33_mes_plan.h"
#include <cassert>
#include <cstdio>
using namespace rn_navi33;
namespace fw = rn_navi33::psp_plan;
namespace vm = rn_navi33::vm_plan;
namespace mes = rn_navi33::mes_plan;

static void checkFirmware() {
    static_assert(!fw::kCanSubmitToGpu, "Never enable P1 PSP hardware");
    fw::VerifiedFile images[fw::kFileCount]{};
    for (unsigned i = 0; i < fw::kFileCount; ++i) {
        images[i] = {fw::kFiles[i].size, fw::kFiles[i].sha256};
    }
    auto p = fw::plan(images);
    assert(p.error == fw::Error::Ok && p.count == 27u);
    assert(p.totalBytes == 2801664u);
    assert(p.totalBytes < fw::kMaxStage);
    assert(p.transfers[0].record == 26u); // SMU first
    assert(fw::kRecords[p.transfers[0].record].type == 18u);
    assert(p.transfers[1].record == 0u);
    assert(p.transfers[26].record == 25u);
    for (unsigned i = 0; i < 27u; ++i) {
        const auto &t = p.transfers[i];
        assert(t.opcode == fw::kLoadIpFw && t.opcode != fw::kAutoloadRlc);
        assert((t.gpuStagingOffset % 4096u) == 0u);
        assert((t.paddedBytes % 4096u) == 0u);
        const auto &r = fw::kRecords[t.record];
        assert(t.paddedBytes >= r.length);
        assert(r.offset + r.length <= fw::kFiles[r.file].size);
    }
    images[0].sha256 = nullptr;
    assert(fw::plan(images).error == fw::Error::MissingDigest);
    images[0].sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
    assert(fw::plan(images).error == fw::Error::WrongDigest);
    images[0].sha256 = fw::kFiles[0].sha256;
    images[0].size -= 1u;
    assert(fw::plan(images).error == fw::Error::WrongSize);
}

static void checkVm() {
    static_assert(!vm::kHardwareWritesEnabled, "VM plan is hardware-inert");
    vm::Bounds b{}; b.vramBytes = 8ull << 30u;
    b.cpuVisibleBytes = 256ull << 20u;
    b.forbiddenBegin = 0u; b.forbiddenEnd = 0x10000u;
    b.externallyOwned = true; // test fixture only, not a real device lease
    vm::Pages pages{0x10000u, 0x11000u, 0x12000u, 0x13000u, 0x20000u};
    auto p = vm::build(0x400000u, pages, b);
    assert(p.error == vm::Error::Ok && p.count == 4u);
    assert(vm::slot(0x400000u, 1u) == 2u);
    assert(p.writes[0].tableByteOffset == 0x10000u);
    assert(p.writes[0].value == (pages.pdb1 | vm::kPteValid));
    assert(p.writes[2].tableByteOffset == 0x12010u);
    assert(p.writes[3].value == (pages.data | vm::kPteVram));
    assert(vm::build(0x400001u, pages, b).error == vm::Error::BadVa);
    assert(vm::build(vm::kVaLimit, pages, b).error == vm::Error::BadVa);
    assert(vm::build(0x400000u, pages, b, vm::kPteSystem | vm::kPteValid).error == vm::Error::BadFlags);
    b.externallyOwned = false;
    assert(vm::build(0x400000u, pages, b).error == vm::Error::NotOwned);
    b.externallyOwned = true;
    b.forbiddenBegin = pages.data; b.forbiddenEnd = pages.data + vm::kPage;
    assert(vm::build(0x400000u, pages, b).error == vm::Error::ReservedOverlap);
    b.forbiddenBegin = 0u; b.forbiddenEnd = 0x10000u;
    pages.ptb = pages.pdb0;
    assert(vm::build(0x400000u, pages, b).error == vm::Error::PageAlias);
    pages.ptb = 0x13000u;
    b.cpuVisibleBytes = 0x13000u;
    assert(vm::build(0x400000u, pages, b).error == vm::Error::NotCpuVisible);
}

static void checkMes() {
    static_assert(!mes::kHardwareWritesEnabled, "MES plan is hardware-inert");
    mes::EnableInput e{};
    e.schedulerStart = 0xfffffffff0005000ull;
    e.kiqStart = 0xfffffffff0005000ull;
    e.exclusiveOwner = true; // test fixture only; does NOT grant ownership
    auto p = mes::makeEnable(e);
    assert(p.error == mes::Error::Ok && p.count == 6u);
    assert(p.writes[0].dword == mes::kCpMesCntl);
    assert(p.writes[0].value == (mes::kResetPipe0 | mes::kResetPipe1));
    assert(p.writes[1].grbmPipe == 0u && p.writes[3].grbmPipe == 1u);
    assert(p.writes[5].value == (mes::kActivePipe0 | mes::kActivePipe1));
    e.exclusiveOwner = false;
    assert(mes::makeEnable(e).error == mes::Error::NoOwnership);
    e.exclusiveOwner = true; e.initialControl = mes::kActivePipe0;
    assert(mes::makeEnable(e).error == mes::Error::AlreadyActive);
    e.initialControl = 0u; e.kiqStart = 0u;
    assert(mes::makeEnable(e).error == mes::Error::NoFirmware);
    e.kiqStart = 3u;
    assert(mes::makeEnable(e).error == mes::Error::BadAddress);

    mes::MapInput in{};
    in.mqdGpuAddress = 0x100000u; in.wptrGpuAddress = 0x200000u;
    in.doorbellIndex = 0x16u; in.exclusiveOwner = true;
    auto m = mes::makeMapMesQueue(in);
    assert(m.error == mes::Error::Ok && m.count == 7u);
    assert(m.words[0] == 0xc005a200u);
    assert(m.words[1] == 0x34080000u && m.words[2] == 0x58u);
    assert(m.words[3] == 0x100000u && m.words[5] == 0x200000u);
    in.exclusiveOwner = false;
    assert(mes::makeMapMesQueue(in).error == mes::Error::NoOwnership);
    in.exclusiveOwner = true; in.queue = 8u;
    assert(mes::makeMapMesQueue(in).error == mes::Error::BadQueue);
    in.queue = 0u; in.doorbellIndex = mes::kMaxDoorbell + 1u;
    assert(mes::makeMapMesQueue(in).error == mes::Error::BadDoorbell);
    in.doorbellIndex = 0x16u; in.wptrGpuAddress += 4u;
    assert(mes::makeMapMesQueue(in).error == mes::Error::BadAddress);
}
int main() {
    assert(matches(kReference));
    checkFirmware();
    checkVm();
    checkMes();
    std::puts("navi33 P1 host tests passed (no GPU I/O)");
    return 0;
}
