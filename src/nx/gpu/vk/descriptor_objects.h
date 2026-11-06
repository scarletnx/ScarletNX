/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_DESCRIPTOR_OBJECTS_H
#define SNX_VK_DESCRIPTOR_OBJECTS_H
#include "nx/gpu/vk/vk.h"

/* Enabled heap support retains exact create state, including during per-draw descriptor-buffer fallback.
   Devices without that feature call Vulkan directly.
   The GPU thread holds vk_lock for runtime access; startup is single-threaded. Destruction remains at the caller's
   existing GPU retirement point. The enabled device feature is fixed for the device's lifetime. */
VkResult vk_image_view_create(VkDevice device, const VkImageViewCreateInfo* info,
                               const VkAllocationCallbacks* allocation, VkImageView* view);
void vk_image_view_destroy(VkDevice device, VkImageView view, const VkAllocationCallbacks* allocation);
VkResult vk_sampler_create(VkDevice device, const VkSamplerCreateInfo* info,
                            const VkAllocationCallbacks* allocation, VkSampler* sampler);
void vk_sampler_destroy(VkDevice device, VkSampler sampler, const VkAllocationCallbacks* allocation);
/* Heap replay declarations live in descriptor_heaps.h. chain must have pointer alignment; all copied links point
   into that caller-owned storage and remain valid while the descriptor is encoded. Unknown/unsupported chains
   or insufficient storage return0. The original Vulkan objects remain unaffected. */
#endif
