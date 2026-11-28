/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA runner's Vulkan window (agb/runtime/vk.h). The board's 240x160 RGBA frame goes to a device image through a
   host-visible staging buffer, and is copied to the window's image at the whole step of the screen it holds, as SDL's
   renderer shows it (host.c) */
#include "agb/runtime/vk.h"
#include "common/gpu/vk/gpu_pstate.h"
#include "agb/ppu/ppu.h"
#include "common/gpu/vk/device_settings.h"
#include "common/gpu/vk/notice.h"
#include "common/gpu/vk/present_fx.h"
#include "common/gpu/vk/rects.h"
#include "common/gpu/vk/ring.h"
#include "common/gpu/vk/swapchain.h"
#include "common/gpu/vk/marks.h"
#include "common/gpu/vk/pacing.h"
#include "common/gpu/vk/vk_common.h"
#include "common/gpu/vk/window.h"
#include "common/utilities/vpad_sdl.h"
#include "common/utilities/window_icon.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGB_VK_SLOTS 2 /* a frame on the GPU while the next is recorded */

/* a slot's frame: the device image the window's draw samples, and the staging buffer the board's frame goes in */
typedef struct {
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    unsigned char* mapped;
} AgbFrame;

struct AgbVk {
    SnxVkWindow host;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    SnxVkSwapchain swap;
    SnxVkPacing pacing; /* frame_pacing's (common/gpu/vk/pacing.h) */
    bool checkpoints;   /* the device is made with vk_checkpoints' extension (common/gpu/vk/marks.h) */
    SnxVkPaced paced;
    VkSemaphore acquired[AGB_VK_SLOTS], rendered[SNX_VK_SWAP_IMAGES];
    SnxVkRing ring;
    AgbFrame frame[AGB_VK_SLOTS];
    SnxVkNotice* notice;
    SnxVkRects* controls;
    /* the frame drawn into the window through the upscaler and the anti-aliasing the Switch's settings ask for, in
       sharp pixels with both off as SDL's renderer scales it */
    SnxFxConfig fx_config;
    SnxFx* fx;
};

/* the GBA's device on a physical device and queue family (common/gpu/vk/device.h) */
static bool choose(SnxVkChoice* c, void* user) {
    AgbVk* v = user;
    if (!snx_vk_choice_enable(c, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        return snx_vk_choice_refuse(c, "has no VK_KHR_swapchain to draw the window with");
    snx_vk_choice_enable(c, "VK_KHR_portability_subset");
    VkPhysicalDeviceVulkan13Features want13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .synchronization2 = VK_TRUE, /* every 1.3 device has both */
                                               .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceFeatures2 want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &want13};
    v->checkpoints = snx_vk_marks_choose(c, g_settings.gpu_checkpoints);
    want.pNext = snx_vk_pacing_choose(&v->pacing, c, g_settings.frame_pacing, want.pNext);
    return snx_vk_choice_create(c, &want, NULL, NULL, 0);
}

/* the window's swapchain, made anew for its size, its images at the display's refresh or not as vsync says */
static bool swapchain_make(AgbVk* v) {
    static const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    SnxVkSwapchainInfo info = {.surface = v->host.surface,
                               .formats = formats,
                               .format_count = sizeof formats / sizeof *formats,
                               .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                               .window = snx_vk_window_extent(&v->host, (VkExtent2D){960, 640}),
                               .views = true,
                               .unsynced = !g_settings.vsync};
    return snx_vk_swapchain_make(v->phys, v->device, &info, &v->swap);
}

static bool frame_make(AgbVk* v, AgbFrame* f) {
    const VkDeviceSize size = (VkDeviceSize)AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT * 4u;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkMemoryRequirements req;
    VkPhysicalDeviceMemoryProperties mp;
    void* mapped = NULL;
    if (vkCreateImage(v->device, &ii, NULL, &f->image) != VK_SUCCESS ||
        vkCreateBuffer(v->device, &bi, NULL, &f->staging) != VK_SUCCESS)
        return false;
    vkGetPhysicalDeviceMemoryProperties(v->phys, &mp);
    vkGetImageMemoryRequirements(v->device, f->image, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
    ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, 0);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = f->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkAllocateMemory(v->device, &ai, NULL, &f->memory) != VK_SUCCESS ||
        vkBindImageMemory(v->device, f->image, f->memory, 0) != VK_SUCCESS ||
        vkCreateImageView(v->device, &vi, NULL, &f->view) != VK_SUCCESS)
        return false;
    vkGetBufferMemoryRequirements(v->device, f->staging, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(v->device, &ai, NULL, &f->staging_memory) != VK_SUCCESS ||
        vkBindBufferMemory(v->device, f->staging, f->staging_memory, 0) != VK_SUCCESS ||
        vkMapMemory(v->device, f->staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        return false;
    f->mapped = mapped;
    return true;
}

static void frame_free(AgbVk* v, AgbFrame* f) {
    if (f->view) vkDestroyImageView(v->device, f->view, NULL);
    if (f->image) vkDestroyImage(v->device, f->image, NULL);
    if (f->memory) vkFreeMemory(v->device, f->memory, NULL);
    if (f->staging) vkDestroyBuffer(v->device, f->staging, NULL);
    if (f->staging_memory) vkFreeMemory(v->device, f->staging_memory, NULL); /* unmapped with it */
    *f = (AgbFrame){0};
}

static void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout old, VkImageLayout next,
                    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src, VkPipelineStageFlags2 dst_stage,
                    VkAccessFlags2 dst) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = src_stage,
                               .srcAccessMask = src,
                               .dstStageMask = dst_stage,
                               .dstAccessMask = dst,
                               .oldLayout = old,
                               .newLayout = next,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = image,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static void notice_log(const char* text) { fprintf(stderr, "scarletnx-agb: %s\n", text); }

AgbVk* agb_vk_open(const char* title, int width, int height, char* error, size_t error_size) {
    AgbVk* v = calloc(1, sizeof *v);
    if (!v) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    if (!snx_vk_window_open("scarletnx-agb", title, width, height, &v->host, error, error_size)) {
        free(v);
        return NULL;
    }
    if (!v->host.window || !v->host.surface) {
        snprintf(error, error_size, "no Vulkan window on this display");
        agb_vk_close(v);
        return NULL;
    }
    snx_window_icon(v->host.window);
    /* what the GBA can't go without: the window's swapchain, and a portability driver's subset */
    const VkExtensionNeed need[2] = {
        {VK_KHR_SWAPCHAIN_EXTENSION_NAME, "the window is drawn with it"},
        {"VK_KHR_portability_subset", "Vulkan has a portability driver's subset on wherever the driver lists it"}};
    VkExtensionSetting settings[VK_EXTENSION_SETTINGS];
    SnxVkDeviceRequest request = {.who = "scarletnx-agb",
                                  .none = "no Vulkan 1.3 device that presents to the window",
                                  .instance = v->host.instance.instance,
                                  .api = VK_API_VERSION_1_3,
                                  .queues = VK_QUEUE_GRAPHICS_BIT,
                                  .surface = v->host.surface,
                                  .choose = choose,
                                  .user = v,
                                  .need = need,
                                  .needs = 2};
    snx_vk_device_settings(&request, settings);
    SnxVkDevice device;
    if (!snx_vk_device_make(&request, &device, error, error_size)) {
        agb_vk_close(v);
        return NULL;
    }
    v->phys = device.phys;
    v->device = device.device;
    v->family = device.family;
    vkGetDeviceQueue(v->device, v->family, 0, &v->queue);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(v->phys, &properties);
    fprintf(stderr, "scarletnx-agb: the screen through Vulkan on %s\n", properties.deviceName);
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dp = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    bool made = snx_vk_ring_init(&v->ring, v->device, v->queue, v->family, AGB_VK_SLOTS, &dp, false);
    if (made)
        v->ring.marks = snx_vk_marks_new(v->phys, v->device, v->family, g_settings.gpu_profile,
                                         g_settings.gpu_checkpoints, v->checkpoints, "scarletnx-agb", "screen");
    for (unsigned i = 0; i < AGB_VK_SLOTS && made; ++i)
        made = vkCreateSemaphore(v->device, &si, NULL, &v->acquired[i]) == VK_SUCCESS && frame_make(v, &v->frame[i]);
    for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES && made; ++i)
        made = vkCreateSemaphore(v->device, &si, NULL, &v->rendered[i]) == VK_SUCCESS;
    if (!made || !(v->notice = snx_vk_notice_create(v->phys, v->device, AGB_VK_SLOTS)) ||
        !(v->controls = snx_vk_rects_create(v->phys, v->device, AGB_VK_SLOTS)) || !swapchain_make(v)) {
        snprintf(error, error_size, "cannot make the Vulkan window's frames, swapchain or command pool");
        agb_vk_close(v);
        return NULL;
    }
    char said[512];
    snx_vk_present_said(v->swap.mode, g_settings.vsync, said, sizeof said);
    fprintf(stderr, "scarletnx-agb: %s\n", said);
    snx_vk_pacing_said(&v->pacing, g_settings.frame_pacing, said, sizeof said);
    fprintf(stderr, "scarletnx-agb: %s\n", said);
    /* the upscaler and the anti-aliasing the settings ask for, said once */
    if (snx_fx_settings(&v->fx_config, g_settings.upscaler, g_settings.upscaler_sharpness, g_settings.anti_aliasing,
                        said, sizeof said))
        fprintf(stderr, "scarletnx-agb: settings: %s\n", said);
    if (!(v->fx = snx_fx_create(v->phys, v->device, VK_NULL_HANDLE, AGB_VK_SLOTS, notice_log))) {
        snprintf(error, error_size, "cannot make the window's draw of the screen on this device");
        agb_vk_close(v);
        return NULL;
    }
    snx_fx_set_sharpness(v->fx, v->fx_config.sharpness);
    snx_fx_set_off_sharp(v->fx, 1);
    fprintf(stderr, "scarletnx-agb: upscaler %s, anti-aliasing %s\n", snx_fx_upscale_label(v->fx_config.upscaler),
            snx_fx_aa_label(v->fx_config.aa));
    return v;
}

SDL_Window* agb_vk_window(AgbVk* vk) { return vk ? vk->host.window : NULL; }

void agb_vk_close(AgbVk* v) {
    if (!v) return;
    if (v->device) {
        vkDeviceWaitIdle(v->device);
        snx_fx_destroy(v->fx);
        snx_vk_notice_destroy(v->notice);
        snx_vk_rects_destroy(v->controls);
        snx_vk_swapchain_free(v->device, &v->swap);
        for (unsigned i = 0; i < AGB_VK_SLOTS; ++i) {
            frame_free(v, &v->frame[i]);
            if (v->acquired[i]) vkDestroySemaphore(v->device, v->acquired[i], NULL);
        }
        for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
            if (v->rendered[i]) vkDestroySemaphore(v->device, v->rendered[i], NULL);
        snx_vk_marks_free(v->ring.marks, v->device);
        snx_vk_ring_free(&v->ring);
        vkDestroyDevice(v->device, NULL);
    }
    snx_vk_window_close(&v->host);
    free(v);
}

void agb_vk_place(uint32_t w, uint32_t h, int32_t rect[4]) {
    uint32_t step = w / AGB_SCREEN_WIDTH < h / AGB_SCREEN_HEIGHT ? w / AGB_SCREEN_WIDTH : h / AGB_SCREEN_HEIGHT;
    uint32_t sw = AGB_SCREEN_WIDTH * step, sh = AGB_SCREEN_HEIGHT * step;
    if (!step) {
        sw = w, sh = (uint32_t)((uint64_t)w * AGB_SCREEN_HEIGHT / AGB_SCREEN_WIDTH);
        if (sh > h) sh = h, sw = (uint32_t)((uint64_t)h * AGB_SCREEN_WIDTH / AGB_SCREEN_HEIGHT);
    }
    rect[0] = (int32_t)(w - sw) / 2;
    rect[1] = (int32_t)(h - sh) / 2;
    rect[2] = (int32_t)sw;
    rect[3] = (int32_t)sh;
}

bool agb_vk_frame(AgbVk* v, const uint32_t* framebuffer, const SnxNotice* notice) {
    uint32_t slot, index = 0;
    VkResult r;
    /* a window the swapchain was not made for: the driver need not say the surface is out of date, and the screen
       would show stretched until it does */
    int ww = 0, wh = 0;
    if (v->swap.swapchain && SDL_GetWindowSizeInPixels(v->host.window, &ww, &wh) && ww > 0 && wh > 0 &&
        ((uint32_t)ww != v->swap.extent.width || (uint32_t)wh != v->swap.extent.height))
        (void)swapchain_make(v);
    if (!v->swap.swapchain && !swapchain_make(v)) return false; /* a window with no size: none until it has one */
    /* the recording, begun once the slot's last submission is done: its staging buffer and its semaphore are free */
    VkCommandBuffer cb = snx_vk_ring_commands(&v->ring);
    slot = v->ring.slot;
    AgbFrame* f = &v->frame[slot];
    /* an image the window can take, waiting for it as SDL's renderer with vsync does; none, and the recording stays
       open and empty for the next frame */
    r = vkAcquireNextImageKHR(v->device, v->swap.swapchain, UINT64_MAX, v->acquired[slot], VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        (void)swapchain_make(v);
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;
    memcpy(f->mapped, framebuffer, (size_t)AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT * 4u);
    barrier(cb, f->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = {.bufferRowLength = AGB_SCREEN_WIDTH,
                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT, 1}};
    vkCmdCopyBufferToImage(cb, f->staging, f->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    uint32_t w = v->swap.extent.width, h = v->swap.extent.height;
    int32_t at[4];
    agb_vk_place(w, h, at);
    VkRect2D to = {{at[0], at[1]}, {(uint32_t)at[2], (uint32_t)at[3]}};
    /* the upscaler's and the anti-aliasing's passes but their last, which draws in the window's rendering: sharp
       pixels with both off, as SDL's renderer scales the screen's texture (SDL_SCALEMODE_NEAREST) */
    bool drawn = at[2] && at[3] &&
                 snx_fx_prepare(v->fx, cb, slot, f->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, AGB_SCREEN_WIDTH,
                                AGB_SCREEN_HEIGHT, 0, 0, AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT, 0, to,
                                v->swap.format.format, v->fx_config.upscaler, v->fx_config.aa);
    snx_vk_swapchain_begin(cb, &v->swap, index);
    if (drawn) snx_fx_draw(v->fx, cb, slot);
    (void)snx_vk_notice_draw(v->notice, cb, slot, v->swap.format.format, w, h,
                             snx_display_percent(SDL_GetWindowDisplayScale(v->host.window)), notice, SDL_GetTicksNS(),
                             notice_log);
    (void)snx_vk_rects_draw(v->controls, cb, slot, v->swap.format.format, w, h, snx_vpad_window_rects((int)w, (int)h),
                            notice_log);
    snx_vk_swapchain_end(cb, &v->swap, index);
    (void)snx_vk_ring_submit(&v->ring, v->acquired[slot], v->rendered[index]);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &v->rendered[index],
                           .swapchainCount = 1,
                           .pSwapchains = &v->swap.swapchain,
                           .pImageIndices = &index};
    snx_gpu_pstate_present(); /* a kgsl hold lasts while frames are presented */
    snx_vk_pacing_present(&v->pacing, &v->paced, v->device, v->swap.swapchain,
                          g_settings.vsync && snx_vpad_window_frame_ns(1) == 1, &pi, "scarletnx-agb");
    r = vkQueuePresentKHR(v->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) (void)swapchain_make(v);
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}
