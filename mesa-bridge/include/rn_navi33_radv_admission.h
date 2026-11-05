/* SPDX-License-Identifier: BSD-3-Clause
 * Navi33/gfx1102 RADV metadata admission, without device or DRM access.
 * Based on the RX 7600 Cobalt64 RADV 0010/0011 observations and the
 * Linux amdgpu GC11 UAPI. This is NOT the Navi48 N48N backend.
 * Strictly host-side: does not open an IOKit service or enable a GPU queue.
 */
#ifndef RN_NAVI33_RADV_ADMISSION_H
#define RN_NAVI33_RADV_ADMISSION_H
#include <stdint.h>

#define RN_N33_PCI_VENDOR 0x1002u
#define RN_N33_PCI_DEVICE 0x7480u
#define RN_N33_GC_VERSION ((11u << 16) | (0u << 8) | 2u)
#define RN_N33_AMDGPU_FAMILY_GC11 145u
#define RN_N33_47BIT_VA_LIMIT (1ull << 47)

enum rn_n33_admission {
    RN_N33_META_VALID = 0,     /* metadata only, NOT hardware acceleration */
    RN_N33_NOT_NAVI33,
    RN_N33_BAD_GC_VERSION,
    RN_N33_BAD_DEVICE_INFO,
    RN_N33_BAD_LOW_VA,
    RN_N33_BAD_HIGH_VA,
    RN_N33_NO_BACKEND
};

struct rn_n33_report {
    uint32_t pci_vendor, pci_device;
    uint32_t gc_version;
    uint32_t dev_info_device_id, dev_info_family;
    uint32_t cu_count, rb_count;
    uint64_t low_va_first, low_va_max;    /* [first,max), from live DEV_INFO */
    uint64_t high_va_first, high_va_max;  /* [first,max) or both zero */
    uint32_t va_alignment;
    uint8_t native_backend_verified;     /* caller must independently verify */
};

struct rn_n33_contract {
    enum rn_n33_admission admission;
    uint8_t low_only;
    uint8_t can_submit; /* always false: actual native queue ownership unproven */
    uint64_t low_va_first, low_va_max;
    uint64_t high_va_first, high_va_max;
};

static inline int rn_n33_pow2_u32(uint32_t x) {
    return x != 0u && (x & (x-1u)) == 0u;
}

static inline struct rn_n33_contract
rn_n33_inspect_radv(const struct rn_n33_report *r) {
    struct rn_n33_contract c = {RN_N33_NOT_NAVI33, 0, 0, 0, 0, 0, 0};
    if (!r) { c.admission = RN_N33_BAD_DEVICE_INFO; return c; }
    if (r->pci_vendor != RN_N33_PCI_VENDOR ||
        r->pci_device != RN_N33_PCI_DEVICE) return c;
    if (r->gc_version != RN_N33_GC_VERSION) {
        c.admission = RN_N33_BAD_GC_VERSION; return c;
    }
    if (r->dev_info_device_id != r->pci_device ||
        r->dev_info_family != RN_N33_AMDGPU_FAMILY_GC11 ||
        !r->cu_count || !r->rb_count) {
        c.admission = RN_N33_BAD_DEVICE_INFO; return c;
    }
    if (!rn_n33_pow2_u32(r->va_alignment) || r->va_alignment < 4096u ||
        r->low_va_first < 4096u ||
        r->low_va_first >= r->low_va_max ||
        r->low_va_max > RN_N33_47BIT_VA_LIMIT ||
        (r->low_va_first & (r->va_alignment - 1u)) ||
        (r->low_va_max & (r->va_alignment - 1u))) {
        c.admission = RN_N33_BAD_LOW_VA; return c;
    }
    const int low_only = !r->high_va_first && !r->high_va_max;
    if (!low_only &&
        (!r->high_va_first || r->high_va_first >= r->high_va_max ||
         (r->high_va_first & (r->va_alignment - 1u)) ||
         (r->high_va_max & (r->va_alignment - 1u)) ||
         r->high_va_first < r->low_va_max)) {
        c.admission = RN_N33_BAD_HIGH_VA; return c;
    }
    c.low_only = (uint8_t)low_only;
    c.low_va_first = r->low_va_first;
    c.low_va_max = r->low_va_max;
    c.high_va_first = r->high_va_first;
    c.high_va_max = r->high_va_max;
    /* Cobalt64's patch 0011 demonstrates a low-only gfx1102 VM. Never
     * invent a high range from Navi32 or gfx1201 device metadata.
     * The runtime driver must still independently prove command submission.
     */
    c.admission = r->native_backend_verified ?
        RN_N33_META_VALID : RN_N33_NO_BACKEND;
    return c;
}
#endif
