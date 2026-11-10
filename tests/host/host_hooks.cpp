// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Host implementations of the compat hooks, so the pure parts of the layer run as a normal program.
#include <rn/hooks.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

extern "C" {
void rn_log(int level, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("[log %d] ", level); vprintf(fmt, ap);
    va_end(ap);
}
void rn_bug(const char *file, int line) { printf("BUG %s:%d\n", file, line); abort(); }
void *rn_raw_alloc(size_t size) { return malloc(size); }
void rn_raw_free(void *ptr, size_t) { free(ptr); }
void rn_yield(void) { sched_yield(); }
void rn_delay_us(unsigned long us) { usleep((useconds_t)us); }
void rn_sleep_ms(unsigned long ms) { usleep((useconds_t)(ms * 1000)); }
uint64_t rn_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
}
