/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The submission ring's uploads (common/gpu/vk/ring.h's snx_vk_ring_uploads): a copy recorded in the open recording,
   then a fill of its source recorded in the uploads, copies the fill's bytes, as the uploads run first; a recording
   with no uploads submits as before; and the slots' uploads are begun afresh after each submission, past a turn of
   the ring. On whatever Vulkan device there is; none is a skip. */
#include "common/gpu/vk/ring.h"
#include "common/gpu/vk/window.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    VkDevice device;
    VkPhysicalDevice phys;
    uint32_t family;
} Made;
static bool make(VkPhysicalDevice phys, uint32_t family, void* user) {
    Made* m = user;
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (snx_vk_device_create(phys, family, &features, NULL, 0, &m->device) != VK_SUCCESS) return false;
    m->phys = phys;
    m->family = family;
    return true;
}
int main(void) {
    SnxVkWindow w;
    char why[256];
    if (!snx_vk_window_open("snx-test-vk-ring-uploads", "ring uploads", 64, 64, &w, why, sizeof why)) {
        printf("no Vulkan (%s): the ring's uploads skipped\n", why);
        return 77;
    }
    Made m = {0};
    if (!snx_vk_first_device(w.instance.instance, VK_API_VERSION_1_1, VK_QUEUE_COMPUTE_BIT, VK_NULL_HANDLE, make, &m)) {
        printf("no Vulkan device: the ring's uploads skipped\n");
        snx_vk_window_close(&w);
        return 77;
    }
    volkLoadDevice(m.device);
    VkQueue queue;
    vkGetDeviceQueue(m.device, m.family, 0, &queue);
    /* a buffer the host maps: the source word at 0, the copy's at 256 */
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = 512,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkMemoryRequirements req;
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t* words;
    if (vkCreateBuffer(m.device, &bi, NULL, &buffer) != VK_SUCCESS) abort();
    vkGetBufferMemoryRequirements(m.device, buffer, &req);
    vkGetPhysicalDeviceMemoryProperties(m.phys, &mp);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits,
                                                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m.device, &ai, NULL, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(m.device, buffer, memory, 0) != VK_SUCCESS ||
        vkMapMemory(m.device, memory, 0, VK_WHOLE_SIZE, 0, (void**)&words) != VK_SUCCESS)
        abort();
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 1,
                                     .poolSizeCount = 1,
                                     .pPoolSizes = &size};
    SnxVkRing ring;
    if (!snx_vk_ring_init(&ring, m.device, queue, m.family, 2, &dp)) abort();
    int failures = 0;
    /* five recordings in two slots: the even ones fill the source in their uploads after their copy is recorded */
    for (uint32_t i = 0; i < 5; ++i) {
        words[0] = 0x11110000u + i;
        words[64] = 0;
        VkBufferCopy copy = {0, 256, 4};
        vkCmdCopyBuffer(snx_vk_ring_commands(&ring), buffer, buffer, 1, &copy);
        uint32_t want = words[0];
        if (i % 2 == 0) {
            want = 0xc0de0000u + i;
            vkCmdFillBuffer(snx_vk_ring_uploads(&ring), buffer, 0, 4, want);
        }
        if (!snx_vk_ring_wait(&ring, snx_vk_ring_submit(&ring, VK_NULL_HANDLE, VK_NULL_HANDLE), UINT64_MAX)) abort();
        if (words[64] != want) {
            fprintf(stderr, "recording %u copied 0x%08x, 0x%08x expected\n", i, words[64], want);
            ++failures;
        }
    }
    snx_vk_ring_free(&ring);
    vkDestroyBuffer(m.device, buffer, NULL);
    vkFreeMemory(m.device, memory, NULL);
    vkDestroyDevice(m.device, NULL);
    snx_vk_window_close(&w);
    if (failures) return 1;
    puts("vk_ring_uploads: 5 recordings, 3 with uploads, each run before its recording");
    return 0;
}
