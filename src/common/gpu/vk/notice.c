/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/notice.h"
#include "common/gpu/vk/vk_common.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char* mapped;
    VkDeviceSize size;
} NoticeBuffer;

struct SnxVkNotice {
    VkDevice dev;
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t slots;
    int format_said; /* a window format it does not write was said */
    NoticeBuffer buffers[];
};

SnxVkNotice* snx_vk_notice_create(VkPhysicalDevice phys, VkDevice dev, uint32_t slots) {
    SnxVkNotice* vn = calloc(1, sizeof *vn + slots * sizeof(NoticeBuffer));
    if (!vn) return NULL;
    vn->dev = dev;
    vn->slots = slots;
    vkGetPhysicalDeviceMemoryProperties(phys, &vn->memory);
    return vn;
}

static void buffer_free(SnxVkNotice* vn, NoticeBuffer* b) {
    if (b->memory) vkFreeMemory(vn->dev, b->memory, NULL); /* unmapped with it */
    if (b->buffer) vkDestroyBuffer(vn->dev, b->buffer, NULL);
    *b = (NoticeBuffer){0};
}

void snx_vk_notice_destroy(SnxVkNotice* vn) {
    if (!vn) return;
    for (uint32_t i = 0; i < vn->slots; ++i) buffer_free(vn, &vn->buffers[i]);
    free(vn);
}

/* slot's buffer, size bytes at least: made again where it holds less, which its slot no longer reads */
static NoticeBuffer* buffer_for(SnxVkNotice* vn, uint32_t slot, VkDeviceSize size, void (*log)(const char* text)) {
    NoticeBuffer* b = &vn->buffers[slot];
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkResult r;
    void* mapped = NULL;
    char text[160];
    if (b->size >= size) return b;
    buffer_free(vn, b);
    r = vkCreateBuffer(vn->dev, &bi, NULL, &b->buffer);
    if (r == VK_SUCCESS) {
        vkGetBufferMemoryRequirements(vn->dev, b->buffer, &req);
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            snx_vk_memory_type(&vn->memory, req.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        r = ai.memoryTypeIndex == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT
                                             : vkAllocateMemory(vn->dev, &ai, NULL, &b->memory);
    }
    if (r == VK_SUCCESS) r = vkBindBufferMemory(vn->dev, b->buffer, b->memory, 0);
    if (r == VK_SUCCESS) r = vkMapMemory(vn->dev, b->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (r != VK_SUCCESS) {
        snprintf(text, sizeof text, "the notice's buffer of %llu bytes could not be made (%s): it is not drawn",
                 (unsigned long long)size, snx_vk_result_str(r));
        if (log) log(text);
        buffer_free(vn, b);
        return NULL;
    }
    b->mapped = mapped;
    b->size = size;
    return b;
}

int snx_vk_notice_record(SnxVkNotice* vn, VkCommandBuffer cb, uint32_t slot, VkImage image, VkFormat format,
                         uint32_t width, uint32_t height, int percent, const SnxNotice* n, uint64_t now_ns,
                         void (*log)(const char* text)) {
    int rect[4], scale, bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    NoticeBuffer* b;
    if (!vn || slot >= vn->slots || !snx_notice_shown(n, now_ns)) return 0;
    if (!bgra && format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB) {
        if (!vn->format_said++ && log) {
            char text[128];
            snprintf(text, sizeof text, "a notice is not drawn on a window of format %d, only on 8-bit RGBA or BGRA",
                     (int)format);
            log(text);
        }
        return 0;
    }
    scale = snx_notice_scale((int)width, (int)height, percent);
    if (!snx_notice_rect(n, (int)width, (int)height, scale, rect)) return 0;
    b = buffer_for(vn, slot, (VkDeviceSize)rect[2] * (VkDeviceSize)rect[3] * 4u, log);
    if (!b) return 0;
    snx_notice_pixels(n, scale, bgra, rect, b->mapped, (size_t)rect[2] * 4u);
    /* after the picture's own copies and blits to the image, whose pixels the band's replace */
    VkImageMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = image,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &after);
    VkBufferImageCopy copy = {.bufferRowLength = (uint32_t)rect[2],
                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageOffset = {rect[0], rect[1], 0},
                              .imageExtent = {(uint32_t)rect[2], (uint32_t)rect[3], 1}};
    vkCmdCopyBufferToImage(cb, b->buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    return 1;
}
