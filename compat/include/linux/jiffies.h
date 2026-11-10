// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_JIFFIES_H
#define _LINUX_JIFFIES_H

#include <rn/hooks.h>

// One jiffy is one millisecond.
#define HZ 1000
#define jiffies ((unsigned long)(rn_now_ns() / 1000000ull))
#define msecs_to_jiffies(ms) ((unsigned long)(ms))
#define jiffies_to_msecs(j) ((unsigned int)(j))
#define usecs_to_jiffies(us) ((unsigned long)(((us) + 999) / 1000))
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)

#endif
