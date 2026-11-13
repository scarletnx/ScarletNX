/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The NDS's Vulkan device, which draws everything the console shows: the 3D engine's polygons (render3d.c), the two 2D
   engines' frame composed over them (frame.c) and the window (present.c). The CPU's part is the copy of the video
   memory and of the lines' registers into the device's buffers, a frame at a time, in whole ranges; every texel,
   tile, object and blend is decoded in a shader from those bytes. */
#ifndef SNX_NDS_VK_H
#define SNX_NDS_VK_H
#include "common/gpu/vk/ring.h"
#include "common/gpu/vk/swapchain.h"
#include "common/gpu/vk/vk_common.h"
#include "common/gpu/vk/window.h"
#include "nds/board/nds.h"

/* Where each of the video memory's spaces lies in the frame's copy, in bytes (shaders/compose.comp has the same) */
#define NDS_VK_BG_A      0x000000u
#define NDS_VK_BG_B      0x080000u
#define NDS_VK_OBJ_A     0x0A0000u
#define NDS_VK_OBJ_B     0x0E0000u
#define NDS_VK_BGEXT_A   0x100000u
#define NDS_VK_BGEXT_B   0x108000u
#define NDS_VK_OBJEXT_A  0x110000u
#define NDS_VK_OBJEXT_B  0x112000u
#define NDS_VK_PALETTE   0x114000u
#define NDS_VK_OAM       0x114800u
#define NDS_VK_LCDC      0x120000u
/* the main memory display's frame, 256 by 192 pixels of 16 bits */
#define NDS_VK_MAIN      0x1A0000u
#define NDS_VK_2D_BYTES  0x1B8000u
#define NDS_VK_LINES_BYTES (2u * NDS_SCREEN_HEIGHT * NDS_LINE_WORDS * 4u)
/* the 3D engine's copy: the texture space, then the texture palettes (shaders/texture.glsl) */
#define NDS_VK_TEXPAL    0x080000u
#define NDS_VK_3D_BYTES  0x098000u

#define NDS_VK_SLOTS 4

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char* map; /* NULL for the device's own memory */
    VkDeviceSize size;
} NdsVkBuffer;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkImageLayout layout; /* where the last command recorded on it left it */
    VkFormat format;
    uint32_t width, height;
} NdsVkImage;

typedef struct {
    SnxVkWindow host;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory_properties;
    SnxVkRing ring;
    VkFormat depth_format;
    bool headless;
} NdsVk;

/* the device, once nds_vk_init made it; NULL before, and after nds_vk_free */
NdsVk* nds_vk(void);
/* The device and, unless headless, the window. False with the reason in error when the host has no Vulkan 1.3 device */
bool nds_vk_init(const char* title, bool headless, char* error, size_t error_size);
void nds_vk_free(void);
bool nds_vk_buffer(NdsVkBuffer* out, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
void nds_vk_buffer_free(NdsVkBuffer* buffer);
bool nds_vk_image(NdsVkImage* out, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                  VkImageAspectFlags aspect);
void nds_vk_image_free(NdsVkImage* image);
/* the image to another layout, for the commands recorded next */
void nds_vk_layout(VkCommandBuffer cb, NdsVkImage* image, VkImageLayout layout);
/* what was written to buffers so far is there for the commands recorded next */
void nds_vk_buffer_barrier(VkCommandBuffer cb);

/* ---- frame.c: the 2D engines' frame ---- */
bool nds_vk_frame_init(char* error, size_t error_size);
void nds_vk_frame_free(void);
/* the frame composed from the board's lines and video memory into the frame image (256 by 384, the upper screen
   first), recorded into the open submission */
void nds_vk_frame_compose(NdsBoard* board);
NdsVkImage* nds_vk_frame_image(void);
/* a display capture the GPU has not given back yet, put into its VRAM bank now (the GPU is waited for): before
   anything reads the bank */
void nds_vk_capture_land(NdsBoard* board);
/* the frame's pixels read back, RGBA, 256 by 384: waits for the GPU. False when it cannot */
bool nds_vk_frame_read(unsigned char* rgba);

/* ---- render3d.c: the 3D engine's picture ---- */
bool nds_vk_render3d_init(char* error, size_t error_size);
void nds_vk_render3d_free(void);
/* the polygons the 3D engine swapped in, drawn into the 3D picture */
void nds_vk_render3d(NdsBoard* board);
/* the picture the 2D engines put on BG0 (256 by 192, its alpha a pixel's own) */
NdsVkImage* nds_vk_render3d_image(void);

/* ---- present.c ---- */
bool nds_vk_present_init(char* error, size_t error_size);
void nds_vk_present_free(void);
/* the frame image to the window, both screens, and the submission sent */
void nds_vk_present(void);
#endif
