#!/bin/bash
# Which Linux-only constructs does a Mesa checkout carry in the parts RADV is built from? Read-only, the analysis behind docs/M5_MESA_BUILD.md.
#     git clone --depth 1 --filter=blob:none --no-checkout https://gitlab.freedesktop.org/mesa/mesa.git mesa-src
#     (cd mesa-src && git sparse-checkout init --cone && git sparse-checkout set src/amd src/util src/vulkan include/drm-uapi include/vulkan && git checkout)
#     tools/mesa_linux_scan.sh mesa-src
cd "${1:?path of the Mesa checkout}" || exit 1
scan() { pat="$1"; shift; for d in "$@"; do f=$(grep -rIlE --include='*.c' --include='*.cpp' --include='*.h' -e "$pat" "$d" 2>/dev/null); n=$(echo "$f" | grep -c .); [ "$n" -gt 0 ] && echo "  $d: $n: $(echo "$f" | head -5 | xargs -n1 basename | tr '\n' ' ')"; done; }
for pat in 'futex' 'memfd_create|MFD_' 'shm_open' '<linux/' 'sys/ioctl' 'xf86drm' 'libsync|sync_file' 'epoll|eventfd' 'sched_setaffinity|sched_getcpu|pthread_setaffinity' '"/proc/|"/sys/' 'drmGetDevice|drmIoctl|drmCommand|drmSyncobj|drmPrime' 'dma-buf|dmabuf|DMA_BUF' 'prctl|syscall\(' 'mmap\('; do
    echo "== $pat"
    scan "$pat" src/amd/vulkan src/amd/common src/amd/compiler src/amd/addrlib src/vulkan/runtime src/vulkan/wsi src/util
done
