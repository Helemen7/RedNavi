// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#include "smu13.h"

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/ktime.h>

namespace {

// Waits for the response register to become non-zero. Returns 0 on timeout.
u32 pollResponse(struct rn_device *adev, unsigned long timeout_us) {
    ktime_t start = ktime_get();
    for (;;) {
        u32 r = RREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_90);
        if (r) return r;
        if (ktime_us_delta(ktime_get(), start) >= (s64)timeout_us) return 0;
        udelay(2);
    }
}

int decode(u32 resp) {
    switch (resp) {
    case PPSMC_Result_OK: return 0;
    case PPSMC_Result_Failed: return -EREMOTEIO;
    case PPSMC_Result_UnknownCmd: return -EOPNOTSUPP;
    case PPSMC_Result_CmdRejectedPrereq: return -EAGAIN;
    case PPSMC_Result_CmdRejectedBusy: return -EBUSY;
    default: return -EREMOTEIO;
    }
}

}

extern "C" int rn_smu13_send(struct rn_device *adev, u32 msg, u32 arg, u32 *out, u32 *resp, unsigned long timeout_us) {
    // The previous message has to be finished before the response register can be reused.
    u32 r = pollResponse(adev, timeout_us);
    if (!r) { if (resp) *resp = 0; return -ETIMEDOUT; }

    WREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_90, 0);
    WREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_82, arg);
    WREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_66, msg);

    r = pollResponse(adev, timeout_us);
    if (resp) *resp = r;
    if (!r) return -ETIMEDOUT;
    int rc = decode(r);
    if (rc == 0 && out) *out = RREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_82);
    return rc;
}
