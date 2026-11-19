// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

/*
 * Does the card see small CPU writes to host-visible memory that another thread made just before the submit?
 *
 * One host buffer is rewritten over and over with a few dozen bytes (a partly filled cache line, like a small texture of a compositor), then the card copies the whole buffer to a second
 * host buffer (vkCmdCopyBuffer) and the copy is compared with what was written. Memory type of the source: "wc" is the first HOST_VISIBLE | HOST_COHERENT type (write-combined on RADV, what
 * RedNaviMTL's buffers use), "cached" is the first one that is also HOST_CACHED. Modes: "same" writes and submits on one thread, "split" writes on one thread and submits on another,
 * "splitfence" is "split" with a store fence after the write.
 *
 *   clang -O1 -arch x86_64 -mmacosx-version-min=12 -I/opt/local/include test/vk_wc.c -L/opt/local/lib -lvulkan -lpthread -o vk_wc
 *   ./vk_wc [iterations]       exit status 0 when no mismatch was seen
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, (int)r_); exit(2); } } while (0)
#define SIZE 4096

static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cb;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties mp;
static volatile uint8_t *src, *dstp;
static int mt_src[2];

static int find_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want && !(mp.memoryTypes[i].propertyFlags & avoid)) return (int)i;
    return -1;
}
static VkBuffer mkbuf(int type_wc, int cached_dst, volatile uint8_t **map) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = SIZE, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b; CHECK(vkCreateBuffer(dev, &bi, NULL, &b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b, &mr);
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int t = cached_dst ? find_type(mr.memoryTypeBits, hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                       : find_type(mr.memoryTypeBits, hv, type_wc ? (VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!cached_dst && !type_wc) t = find_type(mr.memoryTypeBits, hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) { fprintf(stderr, "no memory type\n"); exit(2); }
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)t};
    VkDeviceMemory m; CHECK(vkAllocateMemory(dev, &ai, NULL, &m));
    CHECK(vkBindBufferMemory(dev, b, m, 0));
    void *p; CHECK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &p));
    *map = p;
    return b;
}

// the writer: fills `len` bytes at `off` with the value of the round
static atomic_int round_no, done_no;
static int mode;
static unsigned iters;
static void put(unsigned k) {
    const unsigned off = 8 + (k * 97u) % (SIZE - 128), len = 24 + (k * 13u) % 40;
    for (unsigned i = 0; i < len; i++) src[off + i] = (uint8_t)(k + i);
    if (mode == 2) atomic_thread_fence(memory_order_seq_cst);
}
static int check(unsigned k) {
    const unsigned off = 8 + (k * 97u) % (SIZE - 128), len = 24 + (k * 13u) % 40;
    for (unsigned i = 0; i < len; i++) if (dstp[off + i] != (uint8_t)(k + i)) return 1;
    return 0;
}
static void submit_wait(void) {
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    CHECK(vkResetFences(dev, 1, &fence));
}
static void *writer(void *a) {
    (void)a;
    for (unsigned k = 1; k <= iters; k++) {
        while (atomic_load(&done_no) != (int)k - 1) ;
        put(k);
        atomic_store(&round_no, (int)k);
    }
    return NULL;
}

int main(int argc, char **argv) {
    iters = argc > 1 ? (unsigned)atoi(argv[1]) : 20000;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst; CHECK(vkCreateInstance(&ic, NULL, &inst));
    uint32_t n = 1; VkPhysicalDevice pd; VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) CHECK(r);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    CHECK(vkCreateDevice(pd, &di, NULL, &dev));
    vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &pi, NULL, &pool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(dev, &ca, &cb));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(dev, &fi, NULL, &fence));
    int bad = 0;
    for (int wc = 1; wc >= 0; wc--) {
        VkBuffer bs = mkbuf(wc, 0, &src), bd = mkbuf(0, 1, &dstp);
        VkCommandBufferBeginInfo bb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkResetCommandBuffer(cb, 0));
        CHECK(vkBeginCommandBuffer(cb, &bb));
        VkBufferCopy bc = {0, 0, SIZE};
        vkCmdCopyBuffer(cb, bs, bd, 1, &bc);
        VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(cb));
        for (mode = 0; mode < 3; mode++) {
            memset((void *)src, 0, SIZE); memset((void *)dstp, 0, SIZE);
            unsigned mism = 0;
            atomic_store(&round_no, 0); atomic_store(&done_no, 0);
            pthread_t th;
            if (mode) pthread_create(&th, NULL, writer, NULL);
            for (unsigned k = 1; k <= iters; k++) {
                if (mode) { while (atomic_load(&round_no) != (int)k) ; } else put(k);
                submit_wait();
                mism += check(k);
                atomic_store(&done_no, (int)k);
            }
            if (mode) pthread_join(th, NULL);
            printf("%-6s %-10s: %u mismatches of %u\n", wc ? "wc" : "cached", mode == 0 ? "same" : mode == 1 ? "split" : "splitfence", mism, iters);
            bad += mism != 0;
        }
    }
    return bad ? 1 : 0;
}
