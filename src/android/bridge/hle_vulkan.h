/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_HLE_VULKAN_H
#define SNX_ANDROID_HLE_VULKAN_H
#include <stdint.h>

/* libvulkan as a pass-through to the host's Vulkan (hle_vulkan.c) */

/* the host window for the guest's surface, from ScarletNX's renderer (hwui/render.c): the SDL window (NULL without a
   display), and the renderer giving it up for the guest's swapchain */
typedef struct {
    void* (*window)(void);
    void (*yield)(void);
} HleWindowHost;
void hle_vulkan_set_host(const HleWindowHost* host);
/* a vk* command's export for the guest (dlsym of libvulkan.so, vkGet*ProcAddr); 0 for a name that isn't one */
uint64_t hle_vulkan_symbol(const char* name);
/* the guest's vkQueuePresentKHR calls so far (--sample) */
uint64_t hle_vulkan_presents(void);
/* --shot and --shot-frame for the guest's own Vulkan swapchain: the image of each listed present (1 the first),
   saved as binary PPM at path (%ld the present's number). A test's readback: it waits for the GPU at those presents.
   Before the guest makes its device */
void hle_vulkan_set_shot(const char* path, const char* frames);
#endif
