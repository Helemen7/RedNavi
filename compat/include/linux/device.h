// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_DEVICE_H
#define _LINUX_DEVICE_H

#include <linux/printk.h>

struct device {
    const char *name;
    void *driver_data;
};

#define dev_name(d) ((d) && (d)->name ? (d)->name : "rednavi")
#define dev_err(d, fmt, ...) rn_log(RN_LOG_ERR, "%s: " fmt, dev_name(d), ##__VA_ARGS__)
#define dev_warn(d, fmt, ...) rn_log(RN_LOG_WARN, "%s: " fmt, dev_name(d), ##__VA_ARGS__)
#define dev_info(d, fmt, ...) rn_log(RN_LOG_INFO, "%s: " fmt, dev_name(d), ##__VA_ARGS__)
#define dev_dbg(d, fmt, ...) pr_debug("%s: " fmt, dev_name(d), ##__VA_ARGS__)
#define DRM_ERROR(fmt, ...) pr_err(fmt, ##__VA_ARGS__)
#define DRM_WARN(fmt, ...) pr_warn(fmt, ##__VA_ARGS__)
#define DRM_INFO(fmt, ...) pr_info(fmt, ##__VA_ARGS__)
#define DRM_DEBUG(fmt, ...) pr_debug(fmt, ##__VA_ARGS__)

#endif
