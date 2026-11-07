/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_HLE_VULKAN_HOST_H
#define SNX_ANDROID_HLE_VULKAN_HOST_H
#include <volk.h>
/* The pass-through's commands for host code (hle_vulkan.c): the GLES library (hle_gles.c) is a Vulkan app of its
   own on the host window. It makes its instance, surface, device, queue and swapchain, and presents, through the
   same commands that the guest's calls reach, so its frames get the renderer's layer and the --shot copies as a
   guest's do. Each is the guest's command of that name with the guest's allocation callbacks left out:
   VK_KHR_android_surface in an instance's extensions becomes the host window's */
VkResult hle_vulkan_create_instance(const VkInstanceCreateInfo* info, VkInstance* out);
/* vkCreateAndroidSurfaceKHR: the host window's surface, or a headless one without a display */
VkResult hle_vulkan_create_surface(VkInstance instance, VkSurfaceKHR* out);
VkResult hle_vulkan_surface_capabilities(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                         VkSurfaceCapabilitiesKHR* caps);
VkResult hle_vulkan_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo* info, VkDevice* out);
void hle_vulkan_get_queue(VkDevice device, uint32_t family, uint32_t index, VkQueue* out);
VkResult hle_vulkan_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info, VkSwapchainKHR* out);
VkResult hle_vulkan_swapchain_images(VkDevice device, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images);
void hle_vulkan_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain);
void hle_vulkan_destroy_device(VkDevice device);
/* vkDestroyInstance, after its devices: the commands resolved for it resolve again for the next */
void hle_vulkan_destroy_instance(VkInstance instance);
VkResult hle_vulkan_present(VkQueue queue, const VkPresentInfoKHR* info);
#endif
