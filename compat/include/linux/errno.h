// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// The guard is not the kernel's _LINUX_ERRNO_H on purpose: on a Linux host the real header is included from here.
#ifndef RN_COMPAT_LINUX_ERRNO_H
#define RN_COMPAT_LINUX_ERRNO_H

// Host tests on Linux: the C library's <errno.h> takes the error numbers from <linux/errno.h>, and this directory is in front
// of the system headers, so it would find this file instead and define none.
#ifdef __linux__
#include_next <linux/errno.h>
#include <errno.h>
#else
#include <sys/errno.h>
#endif

#ifndef ENOTSUPP
#define ENOTSUPP 524
#endif
#ifndef ERESTARTSYS
#define ERESTARTSYS 512
#endif
#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif
#ifndef EPROBE_DEFER
#define EPROBE_DEFER 517
#endif

#endif
