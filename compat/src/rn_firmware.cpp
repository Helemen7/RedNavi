// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// request_firmware without a file system: blobs are registered by name, request_firmware looks them up.
#include <linux/firmware.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <string.h>

#define RN_FW_MAX 32

namespace {
struct Entry {
    const char *name;     // not copied, must stay valid (use string literals)
    const void *data;
    size_t size;
};
Entry gEntries[RN_FW_MAX];
spinlock_t gLock = __SPIN_LOCK_UNLOCKED(gLock);
}

extern "C" {

int rn_firmware_register(const char *name, const void *data, size_t size) {
    if (!name || !data || !size) return -EINVAL;
    int rc = -ENOMEM;
    spin_lock(&gLock);
    int freeSlot = -1;
    for (int i = 0; i < RN_FW_MAX; i++) {
        if (gEntries[i].name && !strcmp(gEntries[i].name, name)) { freeSlot = i; break; }   // replace
        if (!gEntries[i].name && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot >= 0) { gEntries[freeSlot] = { name, data, size }; rc = 0; }
    spin_unlock(&gLock);
    return rc;
}

void rn_firmware_unregister(const char *name) {
    spin_lock(&gLock);
    for (int i = 0; i < RN_FW_MAX; i++)
        if (gEntries[i].name && !strcmp(gEntries[i].name, name)) gEntries[i] = { nullptr, nullptr, 0 };
    spin_unlock(&gLock);
}

// The returned struct is a static slot per entry, so release_firmware has nothing to free.
int request_firmware(const struct firmware **fw, const char *name, struct device *dev) {
    static struct firmware views[RN_FW_MAX];
    (void)dev;
    int rc = -ENOENT;
    spin_lock(&gLock);
    for (int i = 0; i < RN_FW_MAX; i++) {
        if (gEntries[i].name && !strcmp(gEntries[i].name, name)) {
            views[i].size = gEntries[i].size;
            views[i].data = static_cast<const u8 *>(gEntries[i].data);
            *fw = &views[i];
            rc = 0;
            break;
        }
    }
    spin_unlock(&gLock);
    return rc;
}

void release_firmware(const struct firmware *fw) { (void)fw; }

}
