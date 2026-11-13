/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_VK_H
#define SNX_VITA_VK_H
#include "common/gpu/vk/ring.h"
#include "common/gpu/vk/swapchain.h"
#include "common/gpu/vk/window.h"
#include <stdbool.h>
#include <stdint.h>

/* The Vita's Vulkan device (vk.c), which the renderer and the presenter share, as the 3DS driver's (3ds/gpu/vk): the
   guest's memory the GPU uses is imported as buffers (VK_EXT_external_memory_host), so surfaces, vertices, textures and
   the shown framebuffer are read and written in place on the GPU and nothing of them is copied by the CPU. Work is
   recorded into a command buffer until it is submitted, a few submissions in flight, each with a serial */
/* a scene is one submission and the display's copy another, so a game with several passes a frame fills the slots
   within one: the host thread waits for the oldest submission when it begins the next recording, and a slot more lets
   it run that much further ahead of the GPU (the ring's most, SNX_VK_RING_SLOTS). Each slot costs the renderer's
   staging and linear rings (render.c) */
#define VITA_VK_SLOTS 4
typedef struct {
    SnxVkWindow host; /* the window (none headless), the instance and the surface */
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkDeviceSize host_alignment; /* minImportedHostPointerAlignment */
    bool preserve_infnan; /* shaderSignedZeroInfNanPreserveFloat32: the shaders declare SignedZeroInfNanPreserve */
    bool bc; /* the device samples BC1 to BC3 (textureCompressionBC) */
    bool depth16_blit; /* D16 depth images blit both ways (a depth surface scaled to and from its samples) */
    bool depth_clamp;  /* the device clamps depth rather than clipping it (depthClamp), as the console does */
    VkFormat stencil_format; /* a depth and stencil format it renders to (D24S8, else D32S8), UNDEFINED for none */
    /* the color attachment read as an input attachment while rendered to (VK_KHR_dynamic_rendering_local_read), in
       primitive order within a draw (VK_EXT_rasterization_order_attachment_access) */
    bool local_read, raster_order;
    SnxVkSwapchain swap;
    /* a submission slot's acquired image, a swapchain image's rendering done */
    VkSemaphore acquired[VITA_VK_SLOTS], rendered[SNX_VK_SWAP_IMAGES];
    SnxVkRing ring; /* the submissions in flight, VITA_VK_SLOTS of them */
} VitaVk;

/* the device and the window (none headless, or without a display): false with the reason when the host has no
   Vulkan 1.3 device with VK_EXT_external_memory_host */
bool vita_vk_init(const char* title, char* error, size_t error_size);
VitaVk* vita_vk(void); /* NULL before init */
void vita_vk_free(void);
/* the open command buffer, begun if none was (in a slot free again, which may wait for its submission) */
VkCommandBuffer vita_vk_commands(void);
/* the recording submitted: its serial (the last one's when nothing was recorded) */
uint64_t vita_vk_submit(void);
/* the submissions done so far, without waiting: their slots free again; the last serial done */
uint64_t vita_vk_poll(void);
/* until serial is done, the recording submitted first when serial is its */
bool vita_vk_wait(uint64_t serial);
unsigned vita_vk_slot(void);
VkDescriptorSet vita_vk_set(VkDescriptorSetLayout layout);
uint32_t vita_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags properties);
/* a line over the game for a few seconds, as a Switch game shows one: Esc's first press */
void vita_vk_notice(const char* text);
/* that line while it shows, "" when none does */
const char* vita_vk_notice_text(void);
/* the upscaler's step (which 0) or the anti-aliasing's (1) for this run, as a Switch game's shortcuts step them, said
   in the log and over the game */
void vita_vk_shortcut(int which);
/* the renderer's per-submission resources, freed when the slot's submission is done */
void vita_vk_on_retire(void (*retire)(unsigned slot, uint64_t serial));
/* Shows the framebuffer: width by height pixels, pitch pixels a row, R8G8B8A8 bytes at offset in buffer, scaled into
   the window. False when nothing was shown: no window, or no swapchain image free this time */
bool vita_vk_present(VkBuffer buffer, VkDeviceSize offset, uint32_t pitch, uint32_t width, uint32_t height);
#endif
