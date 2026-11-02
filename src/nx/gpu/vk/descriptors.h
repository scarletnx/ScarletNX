/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_DESCRIPTORS_H
#define SNX_VK_DESCRIPTORS_H
#include "nx/gpu/vk/vk.h"
/* Graphics descriptor buffers. Compute keeps its independently retired descriptor pools. */
void vk_descriptors_layout(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutBinding* bindings, u32 count);
VkDescriptorSet vk_descriptors_write(VkDescriptorSetLayout layout, u32 count, const VkWriteDescriptorSet* writes);
void vk_descriptors_bind(VkCommandBuffer cb, VkPipelineLayout layout, u32 first, u32 count, const VkDescriptorSet* sets);
void vk_descriptors_reset(u32 slot);
/* whether the device has the memory the frames' descriptor tables live in (host-visible, coherent and cached) */
int vk_descriptors_memory(VkPhysicalDevice phys);
/* whether the frame slot's descriptor memory holds the tables of one more draw: else vk_frame_check moves on to the
   next frame before the draw's setup starts, which must not submit */
int vk_descriptors_room(u32 slot);
void vk_descriptors_stats(void);
VkResult vk_buffer_create(VmaAllocator allocator, const VkBufferCreateInfo* info,
                           const VmaAllocationCreateInfo* allocation, VkBuffer* buffer,
                           VmaAllocation* memory, VmaAllocationInfo* details);
void rec_CmdBindDescriptorBuffersEXT(VkCommandBuffer cb, VkDeviceAddress address);
void rec_CmdSetDescriptorBufferOffsetsEXT(VkCommandBuffer cb, VkPipelineLayout layout, u32 first, u32 count,
                                         const VkDeviceSize* offsets);
#endif
