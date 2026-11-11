// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors
// Read-only AMDGPU_INFO_DEV_INFO userspace ABI smoke test.
// Build against the matching kernel UAPI headers, not an unrelated libdrm copy.
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <drm/amdgpu_drm.h>
#include <drm/drm.h>

#ifndef AMDGPU_INFO_DEV_INFO
#error "AMDGPU_INFO_DEV_INFO is missing from the selected UAPI headers"
#endif

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <render-node> [expected-device-id]\n", argv[0]);
        return 1;
    }

    const char *node = argv[1];
    uint32_t expected_device_id = 0;
    int check_device_id = argc == 3;
    if (check_device_id) {
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(argv[2], &end, 0);
        if (errno || end == argv[2] || *end != '\0' || parsed > UINT16_MAX) {
            fprintf(stderr, "invalid expected device ID: %s\n", argv[2]);
            return 1;
        }
        expected_device_id = (uint32_t)parsed;
    }

    int fd = open(node, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", node, strerror(errno));
        return 2;
    }

    struct drm_amdgpu_info_device dev;
    memset(&dev, 0, sizeof(dev));
    struct drm_amdgpu_info request;
    memset(&request, 0, sizeof(request));
    request.query = AMDGPU_INFO_DEV_INFO;
    request.return_pointer = (uintptr_t)&dev;
    request.return_size = (uint32_t)sizeof(dev);

    int rc = ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &request);
    int saved_errno = errno;
    close(fd);
    if (rc < 0) {
        fprintf(stderr, "AMDGPU_INFO_DEV_INFO: %s\n", strerror(saved_errno));
        return 3;
    }

    printf("dev_info_uapi_size=%zu\n", sizeof(dev));
    printf("requested_return_size=%u\n", request.return_size);
    printf("device_id=0x%04" PRIx32 "\n", dev.device_id);
    printf("family=%" PRIu32 "\n", dev.family);
    printf("vram_type=%" PRIu32 "\n", dev.vram_type);
    printf("vram_bit_width=%" PRIu32 "\n", dev.vram_bit_width);
    printf("shader_engines=%" PRIu32 "\n", dev.num_shader_engines);

    if (check_device_id && dev.device_id != expected_device_id) {
        fprintf(stderr, "unexpected PCI device ID: got 0x%04" PRIx32 ", expected 0x%04" PRIx32 "\n",
                dev.device_id, expected_device_id);
        return 4;
    }
    return 0;
}
