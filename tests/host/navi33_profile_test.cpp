// SPDX-License-Identifier: BSD-3-Clause
// Evidence-only structural test for the generated Navi 33 Linux INFO snapshot.
// This does not register the profile with the live ASIC table.
#include <cassert>
#include <cstdint>
#include "../../docs/logs/navi33/linux_info_navi33.h"

static_assert(sizeof(kRNLinuxDevInfo_navi33) == 440, "captured DEV_INFO size changed");
static_assert(sizeof(kRNLinuxHwIpCount_navi33) / sizeof(kRNLinuxHwIpCount_navi33[0]) == 10,
              "HW_IP_COUNT must cover all DRM IP types");
static_assert(sizeof(kRNLinuxHwIpInfo_navi33[0]) == 40, "HW_IP_INFO ABI size changed");
static_assert(sizeof(kRNLinuxMaxIbs_navi33) / sizeof(kRNLinuxMaxIbs_navi33[0]) == 10,
              "MAX_IBS must cover all DRM IP types");

int main() {
    assert(kRNLinuxInfo_navi33.dev_info == kRNLinuxDevInfo_navi33);
    assert(kRNLinuxInfo_navi33.dev_info_size == sizeof(kRNLinuxDevInfo_navi33));
    assert(kRNLinuxInfo_navi33.fw_count == 16);
    assert(kRNLinuxInfo_navi33.max_ibs_count == 10);

    // DEV_INFO.device_id is the first little-endian u32: PCI ID 0x7480.
    assert(kRNLinuxDevInfo_navi33[0] == 0x80);
    assert(kRNLinuxDevInfo_navi33[1] == 0x74);
    assert(kRNLinuxDevInfo_navi33[2] == 0x00);
    assert(kRNLinuxDevInfo_navi33[3] == 0x00);
    return 0;
}
