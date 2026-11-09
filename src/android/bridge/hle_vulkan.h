/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_HLE_VULKAN_H
#define SNX_ANDROID_HLE_VULKAN_H
#include <stdbool.h>
#include <stdint.h>

/* libvulkan as a pass-through to the host's Vulkan (hle_vulkan.c) */

/* The renderer's windows over the guest's frames, as Android's compositor puts an app's windows over its SurfaceView
   (docs/platforms/android-apps/hwui.md, "Over an app's own Vulkan"): a ring of images on the renderer's device, which the guest's
   device imports through external memory and blends over each present */
enum { HLE_LAYER_IMAGES = 3 };
/* the memory handle type of the layer images (a VkExternalMemoryHandleTypeFlagBits) and its device extension: an
   opaque fd on Linux and FreeBSD, an opaque Win32 handle on Windows. macOS has none (HLE_LAYER_MEMORY 0): no layer */
#if defined(_WIN32)
#define HLE_LAYER_MEMORY 0x2u /* VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT */
#define HLE_LAYER_EXTENSION "VK_KHR_external_memory_win32"
#elif defined(__APPLE__)
#define HLE_LAYER_MEMORY 0u
#define HLE_LAYER_EXTENSION ""
#else
#define HLE_LAYER_MEMORY 0x1u /* VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT */
#define HLE_LAYER_EXTENSION "VK_KHR_external_memory_fd"
#endif
/* the layer image to blend over a present. The guest's device makes its image with the same parameters: 2D, one mip
   and one layer, optimal tiling, MUTABLE_FORMAT, usage COLOR_ATTACHMENT, SAMPLED and TRANSFER_SRC, a dedicated
   allocation. The image's contents are premultiplied, in layout GENERAL, released to VK_QUEUE_FAMILY_EXTERNAL */
typedef struct {
    unsigned index;      /* in the ring */
    uint64_t generation; /* the image's: a new one each time the renderer makes the image */
    uint32_t width, height;
    int32_t format;      /* a VkFormat, 8 bits UNORM for each channel */
    uint64_t size;       /* the allocation's, for the import */
    uint8_t device_uuid[16], driver_uuid[16]; /* the renderer's physical device: an import needs the same */
} HleLayer;

/* the host window for the guest's surface, from ScarletNX's renderer (hwui/render.c): the SDL window (NULL without a
   display), the renderer giving the window up for the guest's swapchain (it then draws its frames into the layer),
   the layer image to blend over a present (false when no window shows over the guest's surface), and a new handle
   of an image's memory for an import (an fd that the import owns, a Win32 handle that the importer closes; -1 when
   that generation of the image is gone). layer and layer_handle are called on the presenting thread */
typedef struct {
    void* (*window)(void);
    void (*yield)(void);
    bool (*layer)(HleLayer* out);
    intptr_t (*layer_handle)(unsigned index, uint64_t generation);
} HleWindowHost;
void hle_vulkan_set_host(const HleWindowHost* host);
/* whether a blend of the guest's presents still reads the layer image index: the renderer does not draw into it then.
   Called on the renderer's thread */
bool hle_vulkan_layer_reading(unsigned index);
/* a vk* command's export for the guest (dlsym of libvulkan.so, vkGet*ProcAddr); 0 for a name that isn't one */
uint64_t hle_vulkan_symbol(const char* name);
/* the guest's vkQueuePresentKHR calls so far (--sample) */
uint64_t hle_vulkan_presents(void);
/* --shot and --shot-frame for the guest's own Vulkan swapchain: the image of each listed present (1 the first),
   saved as binary PPM at path (%ld the present's number). A test's readback: it waits for the GPU at those presents.
   Before the guest makes its device */
void hle_vulkan_set_shot(const char* path, const char* frames);
#endif
