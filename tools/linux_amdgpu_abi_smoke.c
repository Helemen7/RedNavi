// SPDX-License-Identifier: BSD-3-Clause
// Read-only AMDGPU_INFO_DEV_INFO userspace ABI smoke test.
// Build against the matching kernel UAPI headers, not an unrelated libdrm copy.
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <drm/amdgpu_drm.h>
#include <drm/drm.h>

#ifndef AMDGPU_INFO_DEV_INFO
#error "AMDGPU_INFO_DEV_INFO is missing from the selected UAPI headers"
#endif

int main(int argc, char **argv) {
    const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    int fd = open(node, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", node, strerror(errno));
        return 2;
    }

    struct drm_amdgpu_info_device dev;
    unsigned char payload[sizeof(dev) + 8];
    memset(payload, 0xa5, sizeof(payload));
    struct drm_amdgpu_info request;
    memset(&request, 0, sizeof(request));
    request.query = AMDGPU_INFO_DEV_INFO;
    request.return_pointer = (uintptr_t)payload;
    request.return_size = (uint32_t)sizeof(dev);

    int rc = ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &request);
    int saved_errno = errno;
    close(fd);
    if (rc < 0) {
        fprintf(stderr, "AMDGPU_INFO_DEV_INFO: %s\n", strerror(saved_errno));
        return 3;
    }

    memcpy(&dev, payload, sizeof(dev));
    printf("dev_info_uapi_size=%zu\n", sizeof(dev));
    printf("requested_return_size=%u\n", request.return_size);
    printf("device_id=0x%04" PRIx32 "\n", dev.device_id);
    printf("family=%" PRIu32 "\n", dev.family);
    printf("vram_type=%" PRIu32 "\n", dev.vram_type);
    printf("vram_bit_width=%" PRIu32 "\n", dev.vram_bit_width);
    printf("shader_engines=%" PRIu32 "\n", dev.num_shader_engines);
    printf("post_payload_canary=");
    for (size_t i = sizeof(dev); i < sizeof(payload); ++i) printf("%02x", payload[i]);
    printf("\n");
    for (size_t i = sizeof(dev); i < sizeof(payload); ++i) {
        if (payload[i] != 0xa5) {
            fprintf(stderr, "kernel wrote beyond the requested DEV_INFO payload\n");
            return 5;
        }
    }

    if (dev.device_id != 0x7480) {
        fprintf(stderr, "unexpected PCI device ID (expected 0x7480)\n");
        return 4;
    }
    return 0;
}
