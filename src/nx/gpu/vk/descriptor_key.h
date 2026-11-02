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
/* Space for count writes and elements descriptor elements. No padding, host pointers, or dstSet participate:
   a newly allocated set with the same layout and contents has the same key. */
#define VK_DESCRIPTOR_KEY_WORDS(count, elements) (1u + 2u * (count) + 3u * (elements))
static inline size_t vk_descriptor_key(uint64_t* key, VkDescriptorSetLayout layout,
                                       const VkWriteDescriptorSet* writes, uint32_t count) {
    size_t n = 0;
    key[n++] = SNX_DESCRIPTOR_HANDLE(layout);
    for (uint32_t i = 0; i < count; i++) {
        const VkWriteDescriptorSet* w = &writes[i];
        key[n++] = (uint64_t)w->dstBinding | (uint64_t)(uint32_t)w->descriptorType << 32;
        key[n++] = (uint64_t)w->descriptorCount | (uint64_t)w->dstArrayElement << 32;
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
