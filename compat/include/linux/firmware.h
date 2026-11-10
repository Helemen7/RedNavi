// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_FIRMWARE_H
#define _LINUX_FIRMWARE_H

#include <linux/types.h>
#include <linux/device.h>

#ifdef __cplusplus
extern "C" {
#endif

struct firmware {
    size_t size;
    const u8 *data;
};

// Firmware is never read from disk by the kext. The component that owns a blob (a generated header, a file
// handed over by a helper) registers it by its linux-firmware name, e.g. "amdgpu/smu_13_0_10.bin".
// The memory must stay valid until it is unregistered.
int rn_firmware_register(const char *name, const void *data, size_t size);
void rn_firmware_unregister(const char *name);

int request_firmware(const struct firmware **fw, const char *name, struct device *dev);
void release_firmware(const struct firmware *fw);

#ifdef __cplusplus
}
#endif

#endif
