// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Host tests for the pure parts of the compat layer. Run with `make test`.
#include <amd/rn_device.h>
#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/byteorder.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

struct item { int v; struct list_head node; };

static void testBits() {
    CHECK(BIT(3) == 8);
    CHECK(GENMASK(7, 4) == 0xf0);
    CHECK(GENMASK_ULL(63, 0) == ~0ULL);
    CHECK(GENMASK(31, 31) == 0x80000000UL);
    CHECK(FIELD_GET(0x00f0u, 0x1234u) == 3);
    CHECK(FIELD_PREP(0x00f0u, 5) == 0x50);
    unsigned long w[2] = {0, 0};
    set_bit(65, w);
    CHECK(test_bit(65, w) && !test_bit(64, w) && w[1] == 2);
    CHECK(test_and_clear_bit(65, w) && !test_bit(65, w));
    CHECK(hweight32(0xf0f0) == 8 && __ffs(0x40) == 6 && fls(0x80000000u) == 32 && fls(0) == 0);
    CHECK(ALIGN(13, 8) == 16 && IS_ALIGNED(16, 8) && DIV_ROUND_UP(9, 4) == 3);
    CHECK(upper_32_bits(0x123456789abcdef0ull) == 0x12345678u && lower_32_bits(0x123456789abcdef0ull) == 0x9abcdef0u);
}

static void testList() {
    LIST_HEAD(head);
    item a = {1, {}}, b = {2, {}}, c = {3, {}};
    CHECK(list_empty(&head));
    list_add_tail(&a.node, &head);
    list_add_tail(&b.node, &head);
    list_add(&c.node, &head);                     // 3 1 2
    int order[3], n = 0;
    item *p;
    list_for_each_entry(p, &head, node) order[n++] = p->v;
    CHECK(n == 3 && order[0] == 3 && order[1] == 1 && order[2] == 2);
    item *q;
    list_for_each_entry_safe(p, q, &head, node) { if (p->v == 1) list_del(&p->node); }
    n = 0;
    list_for_each_entry(p, &head, node) order[n++] = p->v;
    CHECK(n == 2 && order[0] == 3 && order[1] == 2);
    CHECK(list_first_entry(&head, item, node)->v == 3);
    CHECK(container_of(&b.node, item, node) == &b);
}

static void testSlab() {
    char *p = (char *)kzalloc(100, GFP_KERNEL);
    CHECK(p != nullptr);
    for (int i = 0; i < 100; i++) CHECK(p[i] == 0);
    memset(p, 0x5a, 100);
    p = (char *)krealloc(p, 400, GFP_KERNEL);
    CHECK(p && p[99] == 0x5a);
    kfree(p);
    kfree(nullptr);
    CHECK(kcalloc((size_t)-1, 16, GFP_KERNEL) == nullptr);        // overflow is refused
    char *s = kstrdup("rednavi", GFP_KERNEL);
    CHECK(s && !strcmp(s, "rednavi"));
    kfree(s);
    void *d = kmemdup("abc", 3, GFP_KERNEL);
    CHECK(d && !memcmp(d, "abc", 3));
    kfree(d);
}

static void testLocksAndTime() {
    spinlock_t sl; spin_lock_init(&sl);
    unsigned long f;
    spin_lock_irqsave(&sl, f);
    CHECK(!spin_trylock(&sl));
    spin_unlock_irqrestore(&sl, f);
    CHECK(spin_trylock(&sl));
    spin_unlock(&sl);

    DEFINE_MUTEX(m);
    mutex_lock(&m);
    CHECK(mutex_is_locked(&m) && !mutex_trylock(&m));
    mutex_unlock(&m);
    CHECK(!mutex_is_locked(&m));

    atomic_t a = ATOMIC_INIT(5);
    CHECK(atomic_inc_return(&a) == 6 && atomic_dec_and_test(&a) == 0 && atomic_cmpxchg(&a, 5, 9) == 5 && atomic_read(&a) == 9);

    struct kref k; kref_init(&k); kref_get(&k);
    CHECK(kref_put(&k, [](struct kref *) {}) == 0);
    CHECK(kref_put(&k, [](struct kref *) {}) == 1);

    DECLARE_COMPLETION(done);
    CHECK(wait_for_completion_timeout(&done, 5) == 0);             // times out
    complete(&done);
    CHECK(wait_for_completion_timeout(&done, 50) > 0);

    unsigned long t0 = jiffies;
    msleep(20);
    CHECK(!time_before(jiffies, t0 + 15));
    ktime_t k0 = ktime_get();
    udelay(2000);
    CHECK(ktime_us_delta(ktime_get(), k0) >= 1900);
}

static void testFirmware() {
    static const unsigned char blob[] = {1, 2, 3, 4};
    const struct firmware *fw = nullptr;
    CHECK(request_firmware(&fw, "amdgpu/none.bin", nullptr) == -ENOENT);
    CHECK(rn_firmware_register("amdgpu/smu_13_0_10.bin", blob, sizeof(blob)) == 0);
    CHECK(request_firmware(&fw, "amdgpu/smu_13_0_10.bin", nullptr) == 0);
    CHECK(fw && fw->size == 4 && fw->data[2] == 3);
    release_firmware(fw);
    rn_firmware_unregister("amdgpu/smu_13_0_10.bin");
    CHECK(request_firmware(&fw, "amdgpu/smu_13_0_10.bin", nullptr) == -ENOENT);
}

static void testRegisters() {
    static u32 bar[256];                       // pretend 1 KB register BAR
    struct rn_device dev;
    rn_device_init(&dev, "test", bar, sizeof(bar));
    struct rn_device *adev = &dev;
    WREG32(0x10, 0xdeadbeef);
    CHECK(bar[0x10] == 0xdeadbeef && RREG32(0x10) == 0xdeadbeef);
    adev->reg_offset[RN_MP1_HWIP][0][0] = 0x20;
#define MP1_HWIP RN_MP1_HWIP
#define mmFAKE 3
#define mmFAKE_BASE_IDX 0
    WREG32_SOC15(MP1, 0, mmFAKE, 7);
    CHECK(bar[0x23] == 7);                     // segment base 0x20 + register 3
    CHECK(RREG32_SOC15(MP1, 0, mmFAKE) == 7);
    CHECK(SOC15_REG_OFFSET(MP1, 0, mmFAKE) == 0x23);
    // Past the BAR the accesses go through the RSMU index / data pair at byte 0x50 and 0x54.
    WREG32(0x400, 0x1234);
    CHECK(bar[0x14] == 0x1000 && bar[0x15] == 0x1234);      // index = 0x400 * 4, data written
}

int main() {
    testBits();
    testList();
    testSlab();
    testLocksAndTime();
    testFirmware();
    testRegisters();
    printf(failures ? "%d FAILED\n" : "all compat tests passed\n", failures);
    return failures ? 1 : 0;
}
