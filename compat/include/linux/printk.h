// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_PRINTK_H
#define _LINUX_PRINTK_H

#include <rn/hooks.h>

// Levels follow the kernel's KERN_* numbers.
#define RN_LOG_ERR 3
#define RN_LOG_WARN 4
#define RN_LOG_INFO 6
#define RN_LOG_DEBUG 7

#define pr_err(fmt, ...) rn_log(RN_LOG_ERR, fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) rn_log(RN_LOG_WARN, fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) rn_log(RN_LOG_INFO, fmt, ##__VA_ARGS__)
#ifdef DEBUG
#define pr_debug(fmt, ...) rn_log(RN_LOG_DEBUG, fmt, ##__VA_ARGS__)
#else
#define pr_debug(fmt, ...) do { } while (0)
#endif

#define WARN_ON(cond) ({ int _w = !!(cond); if (_w) rn_log(RN_LOG_WARN, "WARN_ON(%s) at %s:%d\n", #cond, __FILE__, __LINE__); _w; })
#define WARN_ON_ONCE(cond) ({ static int _once; int _w = !!(cond); if (_w && !_once) { _once = 1; rn_log(RN_LOG_WARN, "WARN_ON(%s) at %s:%d\n", #cond, __FILE__, __LINE__); } _w; })
#define WARN(cond, fmt, ...) ({ int _w = !!(cond); if (_w) rn_log(RN_LOG_WARN, fmt, ##__VA_ARGS__); _w; })
#define BUG_ON(cond) do { if (cond) rn_bug(__FILE__, __LINE__); } while (0)
#define BUG() rn_bug(__FILE__, __LINE__)

#endif
