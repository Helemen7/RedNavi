// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// RedNavi compat layer: the few functions the pure headers need from their environment.
// The kext implements them in compat/src/rn_hooks.cpp; the host tests in tests/host/host_hooks.cpp.
#ifndef RN_HOOKS_H
#define RN_HOOKS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rn_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void rn_bug(const char *file, int line);          // BUG_ON failed: panics in the kernel, aborts in tests
void *rn_raw_alloc(size_t size);                  // 16-byte aligned, uninitialised, NULL on failure
void rn_raw_free(void *ptr, size_t size);         // size as given to rn_raw_alloc
void rn_yield(void);                              // let other threads run while a lock is contended
void rn_delay_us(unsigned long us);               // busy wait
void rn_sleep_ms(unsigned long ms);               // blocking sleep
uint64_t rn_now_ns(void);                         // monotonic

static inline void rn_cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    __asm__ volatile("" ::: "memory");
#endif
}

#ifdef __cplusplus
}
#endif

#endif
