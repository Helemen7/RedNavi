// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#include <mach/mach_types.h>
#include <libkern/libkern.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(org.rednavi.RedNaviAccel2, "0.0.1", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
