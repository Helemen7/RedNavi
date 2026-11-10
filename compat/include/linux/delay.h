// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#ifndef _LINUX_DELAY_H
#define _LINUX_DELAY_H

#include <rn/hooks.h>

static inline void udelay(unsigned long us) { rn_delay_us(us); }
static inline void ndelay(unsigned long ns) { rn_delay_us((ns + 999) / 1000); }
static inline void mdelay(unsigned long ms) { while (ms--) rn_delay_us(1000); }
static inline void msleep(unsigned int ms) { rn_sleep_ms(ms); }
static inline void usleep_range(unsigned long min_us, unsigned long max_us) {
    (void)max_us;
    if (min_us >= 1000) rn_sleep_ms((min_us + 999) / 1000); else rn_delay_us(min_us);
}
#define cpu_relax() rn_cpu_relax()

#endif
