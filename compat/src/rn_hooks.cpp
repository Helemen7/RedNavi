// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Kernel side of the hooks the pure compat headers rely on.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <stdarg.h>

#include <rn/hooks.h>

extern "C" {

void rn_log(int level, const char *fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    IOLogv(fmt, ap);
    va_end(ap);
}

void rn_bug(const char *file, int line) {
    panic("RedNavi: BUG at %s:%d", file, line);
}

void *rn_raw_alloc(size_t size) { return IOMalloc(size); }
void rn_raw_free(void *ptr, size_t size) { IOFree(ptr, size); }

void rn_yield(void) { IOSleep(1); }

void rn_delay_us(unsigned long us) {
    while (us > 1000) { IODelay(1000); us -= 1000; }
    if (us) IODelay(static_cast<unsigned>(us));
}

void rn_sleep_ms(unsigned long ms) { IOSleep(static_cast<unsigned>(ms)); }

uint64_t rn_now_ns(void) {
    uint64_t abs = 0, ns = 0;
    clock_get_uptime(&abs);
    absolutetime_to_nanoseconds(abs, &ns);
    return ns;
}

}
