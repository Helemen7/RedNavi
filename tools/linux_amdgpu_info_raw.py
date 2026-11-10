#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Run on Linux as root on the machine with the RX 7800 XT: asks the amdgpu kernel driver the DRM_AMDGPU_INFO queries RADV uses and prints
the answers as raw bytes, so that the kext (RedNavi/kern_probe.cpp, infoQuery) can answer them the way the Linux kernel does, field for field.

    sudo python3 tools/linux_amdgpu_info_raw.py > amdgpu_info_raw.txt

Needs only python3. The size of a query's answer is found by running it twice with a buffer filled with 0x00 and then with 0xff: the bytes
the kernel wrote are the same in both runs, the bytes it left alone are not.
"""
import ctypes, fcntl, glob, os, struct, sys

DRM_COMMAND_BASE, DRM_AMDGPU_INFO = 0x40, 0x05
INFO_SIZE = 32                                           # struct drm_amdgpu_info: return_pointer, return_size, query, a 16-byte union
IOCTL = (1 << 30) | (INFO_SIZE << 16) | (ord('d') << 8) | (DRM_COMMAND_BASE + DRM_AMDGPU_INFO)   # DRM_IOW


def query(fd, qid, size, extra=b"", fill=0):
    buf = (ctypes.c_ubyte * size)(*([fill] * size))
    req = struct.pack("<QII", ctypes.addressof(buf), size, qid) + extra.ljust(16, b"\0")
    try:
        fcntl.ioctl(fd, IOCTL, req)
    except OSError as e:
        return e.errno, None
    return 0, bytes(buf)


def answer(fd, qid, maxsize, extra=b""):
    """(errno, bytes the kernel wrote)."""
    rc, a = query(fd, qid, maxsize, extra, 0x00)
    if rc:
        return rc, None
    _, b = query(fd, qid, maxsize, extra, 0xff)
    n = 0
    while n < maxsize and a[n] == b[n]:
        n += 1
    return 0, a[:n]


def show(name, fd, qid, maxsize, extra=b""):
    rc, data = answer(fd, qid, maxsize, extra)
    if rc:
        print("%-32s errno %d" % (name, rc))
    else:
        print("%-32s %3d bytes: %s" % (name, len(data), data.hex()))


def main():
    fd = -1
    for path in sorted(glob.glob("/dev/dri/renderD*")):
        try:
            f = os.open(path, os.O_RDWR)
        except OSError:
            continue
        rc, data = answer(f, 0x16, 2048)                 # DEV_INFO: device_id first
        if rc == 0 and struct.unpack_from("<I", data)[0] == 0x747E:
            fd = f
            print("device", path)
            break
        os.close(f)
    if fd < 0:
        sys.exit("no amdgpu render node with device id 0x747e (are you root?)")
    show("ACCEL_WORKING", fd, 0x00, 64)
    show("DEV_INFO", fd, 0x16, 2048)
    show("VRAM_GTT", fd, 0x14, 64)
    show("MEMORY", fd, 0x19, 256)
    for q, n in ((0x10, "VRAM_USAGE"), (0x11, "GTT_USAGE"), (0x17, "VIS_VRAM_USAGE"), (0x0f, "NUM_BYTES_MOVED"), (0x18, "NUM_EVICTIONS"),
                 (0x1e, "NUM_VRAM_CPU_PAGE_FAULTS"), (0x05, "TIMESTAMP")):
        show(n, fd, q, 64)
    show("MAX_IBS", fd, 0x22, 128)
    show("GPUVM_FAULT", fd, 0x23, 128)
    for t in range(0, 10):
        show("HW_IP_COUNT %d" % t, fd, 0x03, 64, struct.pack("<II", t, 0))
        for inst in (0, 1):
            show("HW_IP_INFO %d.%d" % (t, inst), fd, 0x02, 128, struct.pack("<II", t, inst))
    for t in range(0, 0x20):
        for inst in (0, 1):
            for idx in (0, 1, 2):
                rc, data = answer(fd, 0x0e, 64, struct.pack("<III", t, inst, idx))
                if rc == 0 and any(data):
                    print("FW_VERSION type 0x%02x instance %d index %d: %s" % (t, inst, idx, data.hex()))
    show("VBIOS size", fd, 0x1b, 64, struct.pack("<II", 0, 0))
    for q in (0x1d, 0x1f, 0x20, 0x21, 0x24, 0x25, 0x26, 0x27):
        show("query 0x%02x" % q, fd, q, 256)
    # three sensors RADV reads
    for s, n in ((0x1, "SENSOR GFX_SCLK"), (0x2, "SENSOR GFX_MCLK"), (0x3, "SENSOR GPU_TEMP"), (0x4, "SENSOR GPU_LOAD")):
        show(n, fd, 0x1d, 16, struct.pack("<I", s))


main()
