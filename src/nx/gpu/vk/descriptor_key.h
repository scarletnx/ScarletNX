/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_DESCRIPTOR_KEY_H
#define SNX_VK_DESCRIPTOR_KEY_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#if VK_USE_64_BIT_PTR_DEFINES
#define SNX_DESCRIPTOR_HANDLE(h) ((uint64_t)(uintptr_t)(h))
#else
#define SNX_DESCRIPTOR_HANDLE(h) ((uint64_t)(h))
#endif
/* Space for count writes and elements descriptor elements, including one extra word per address-based texel
   write. No padding, host pointers, or dstSet participate. The high bit of the type word marks that internal
   representation; Vulkan's nonnegative descriptor-type enums fit in31 bits. */
#define VK_DESCRIPTOR_KEY_WORDS(count, elements) (1u + 3u * (size_t)(count) + 3u * (size_t)(elements))
static inline size_t vk_descriptor_key(uint64_t* key, VkDescriptorSetLayout layout,
                                       const VkWriteDescriptorSet* writes, uint32_t count) {
    size_t n = 0;
    key[n++] = SNX_DESCRIPTOR_HANDLE(layout);
    for (uint32_t i = 0; i < count; i++) {
        const VkWriteDescriptorSet* w = &writes[i];
        const VkBufferViewCreateInfo* view = w->pNext;
        int address = w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER && w->descriptorCount == 1 &&
            !w->pBufferInfo && !w->pImageInfo && view && view->sType == VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO &&
            !view->pNext;
        key[n++] = (uint64_t)w->dstBinding | (uint64_t)(uint32_t)w->descriptorType << 32 |
                   (uint64_t)address << 63;
        key[n++] = (uint64_t)w->descriptorCount | (uint64_t)w->dstArrayElement << 32;
        if (address) {
            key[n++] = SNX_DESCRIPTOR_HANDLE(view->buffer);
            key[n++] = view->offset;
            key[n++] = view->range;
            key[n++] = (uint64_t)(uint32_t)view->format | (uint64_t)view->flags << 32;
            continue;
        }
        for (uint32_t j = 0; j < w->descriptorCount; j++) {
            if (w->pBufferInfo) {
                key[n++] = SNX_DESCRIPTOR_HANDLE(w->pBufferInfo[j].buffer);
                key[n++] = w->pBufferInfo[j].offset;
                key[n++] = w->pBufferInfo[j].range;
            } else if (w->pImageInfo) {
                key[n++] = SNX_DESCRIPTOR_HANDLE(w->pImageInfo[j].imageView);
                key[n++] = SNX_DESCRIPTOR_HANDLE(w->pImageInfo[j].sampler);
                key[n++] = (uint64_t)w->pImageInfo[j].imageLayout;
            } else {
                key[n++] = w->pTexelBufferView ? SNX_DESCRIPTOR_HANDLE(w->pTexelBufferView[j]) : 0;
                key[n++] = 0;
                key[n++] = 0;
            }
        }
    }
    return n;
}
#undef SNX_DESCRIPTOR_HANDLE
#endif
