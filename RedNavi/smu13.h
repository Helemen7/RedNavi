// SMU 13.0.x message interface (MP1 mailbox), written against the compat layer so it can move into a ported
// amdgpu smu_v13_0 later. Protocol and message numbers: Linux drivers/gpu/drm/amd/pm/swsmu (smu_cmn.c,
// pmfw_if/smu_v13_0_0_ppsmc.h; SMU 13.0.10 uses the smu_v13_0_0 tables).
#ifndef REDNAVI_SMU13_H
#define REDNAVI_SMU13_H

#include <amd/rn_device.h>

#define PPSMC_MSG_TestMessage 0x1
#define PPSMC_MSG_GetSmuVersion 0x2
#define PPSMC_MSG_GetDriverIfVersion 0x3
#define PPSMC_MSG_SetAllowedFeaturesMaskLow 0x4
#define PPSMC_MSG_SetAllowedFeaturesMaskHigh 0x5
#define PPSMC_MSG_EnableAllSmuFeatures 0x6
#define PPSMC_MSG_SetDriverDramAddrHigh 0xE
#define PPSMC_MSG_SetDriverDramAddrLow 0xF
#define PPSMC_MSG_SetToolsDramAddrHigh 0x10
#define PPSMC_MSG_SetToolsDramAddrLow 0x11
#define PPSMC_MSG_RunDcBtc 0x36
#define PPSMC_MSG_TransferTableSmu2Dram 0x12
#define PPSMC_MSG_GetDpmFreqByIndex 0x1F
#define PPSMC_MSG_NotifyPowerSource 0x35
#define PPSMC_MSG_SetSoftMinByFreq 0x19      // smu_v13_0_0_ppsmc.h; argument (PPCLK_e << 16) | MHz
#define PPSMC_MSG_SetSoftMaxByFreq 0x1A
#define PPSMC_MSG_AllowGfxOff 0x28
#define PPSMC_MSG_DisallowGfxOff 0x29
#define PPSMC_MSG_GetRunningSmuFeaturesLow 0xC
#define PPSMC_MSG_GetRunningSmuFeaturesHigh 0xD

#define PPSMC_Result_OK 0x1
#define PPSMC_Result_Failed 0xFF
#define PPSMC_Result_UnknownCmd 0xFE
#define PPSMC_Result_CmdRejectedPrereq 0xFD
#define PPSMC_Result_CmdRejectedBusy 0xFC

// Register offsets from the Linux headers (dwords, MP1 base index 0).
#define mmMP1_SMN_C2PMSG_66 0x0282
#define mmMP1_SMN_C2PMSG_66_BASE_IDX 0
#define mmMP1_SMN_C2PMSG_82 0x0292
#define mmMP1_SMN_C2PMSG_82_BASE_IDX 0
#define mmMP1_SMN_C2PMSG_90 0x029a
#define mmMP1_SMN_C2PMSG_90_BASE_IDX 0

#ifdef __cplusplus
extern "C" {
#endif

// Sends one message and waits for the answer. Returns 0 and the answer argument in *out, or a negative errno:
// -ETIMEDOUT (no answer, or the previous message never completed, nothing was sent in that case),
// -EREMOTEIO (SMU failed the message), -EOPNOTSUPP (unknown command), -EBUSY / -EAGAIN (rejected).
// *resp gets the raw response register value when it is not NULL.
int rn_smu13_send(struct rn_device *adev, u32 msg, u32 arg, u32 *out, u32 *resp, unsigned long timeout_us);

#ifdef __cplusplus
}
#endif

#endif
