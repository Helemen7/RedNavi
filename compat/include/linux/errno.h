#ifndef _RN_COMPAT_LINUX_ERRNO_H
#define _RN_COMPAT_LINUX_ERRNO_H

#if defined(RN_HOST_TEST) && defined(__linux__)
#include_next <linux/errno.h>
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
