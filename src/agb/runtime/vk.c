/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA runner's Vulkan window (agb/runtime/vk.h). The board's 240x160 RGBA frame goes to a device image through a
   host-visible staging buffer, then the upscaler and the anti-aliasing run where either is on, and the frame is
   copied to the window's image at the whole step of the screen it holds, as SDL's renderer shows it (host.c) */
#include "agb/runtime/vk.h"
#include "agb/ppu/ppu.h"
#include "common/gpu/vk/notice.h"
#include "common/gpu/vk/present_fx.h"
#include "common/gpu/vk/ring.h"
#include "common/gpu/vk/swapchain.h"
#include "common/gpu/vk/vk_common.h"
#include "common/gpu/vk/window.h"
#include "common/utilities/window_icon.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGB_VK_SLOTS 2 /* a frame on the GPU while the next is recorded */

/* a slot's frame: the device image the effects and the copy read, and the staging buffer the board's frame goes in */
typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
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
    bool fp16; /* shaderFloat16: FSR 1's half precision passes */
    SnxVkSwapchain swap;
    VkSemaphore acquired[AGB_VK_SLOTS], rendered[SNX_VK_SWAP_IMAGES];
    SnxVkRing ring;
    AgbFrame frame[AGB_VK_SLOTS];
    /* the upscaler and the anti-aliasing as the settings say, stepped by the shortcuts; none made while both are Off */
    SnxFxConfig fx_config;
    SnxFx* fx;
    SnxVkNotice* notice;
};

static bool make_device(VkPhysicalDevice phys, uint32_t family, void* user) {
    AgbVk* v = user;
    SnxVkExtensions list;
    const char* extensions[2];
    uint32_t n = 0;
    if (!snx_vk_device_extensions(phys, &list)) return false;
    bool ok = snx_vk_extensions_has(&list, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    extensions[n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (snx_vk_extensions_has(&list, "VK_KHR_portability_subset")) extensions[n++] = "VK_KHR_portability_subset";
    snx_vk_extensions_free(&list);
    if (!ok) return false;
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    void* const query[] = {&features, &v12, &v13};
    snx_vk_chain(query, sizeof query / sizeof *query);
    vkGetPhysicalDeviceFeatures2(phys, &features);
    if (!v13.dynamicRendering || !v13.synchronization2) return false;
    VkPhysicalDeviceVulkan13Features want13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .dynamicRendering = VK_TRUE,
                                               .synchronization2 = VK_TRUE};
    VkPhysicalDeviceVulkan12Features want12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                               .shaderFloat16 = v12.shaderFloat16};
    VkPhysicalDeviceFeatures2 want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    void* const enable[] = {&want, &want12, &want13};
    snx_vk_chain(enable, sizeof enable / sizeof *enable);
    if (snx_vk_device_create(phys, family, &want, extensions, n, &v->device) != VK_SUCCESS) return false;
    v->fp16 = v12.shaderFloat16 != 0;
    v->phys = phys;
    v->family = family;
    return true;
}

/* the window's swapchain, made anew for its size, its images at the display's refresh as SDL's renderer has them */
static bool swapchain_make(AgbVk* v) {
    static const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    SnxVkSwapchainInfo info = {.surface = v->host.surface,
                               .formats = formats,
                               .format_count = sizeof formats / sizeof *formats,
                               .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                               .window = snx_vk_window_extent(&v->host, (VkExtent2D){960, 640})};
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
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT};
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
    if (vkAllocateMemory(v->device, &ai, NULL, &f->memory) != VK_SUCCESS ||
        vkBindImageMemory(v->device, f->image, f->memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = f->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(v->device, &vi, NULL, &f->view) != VK_SUCCESS) return false;
    vkGetBufferMemoryRequirements(v->device, f->staging, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = snx_vk_memory_type(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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

static void fx_log(const char* text) { fprintf(stderr, "scarletnx-agb: %s\n", text); }
/* the effects, where either is on and they are not made yet: both Off when they could not be, said */
static void fx_make(AgbVk* v) {
    SnxFxConfig* c = &v->fx_config;
    if (v->fx || (c->upscaler == SNX_FX_UPSCALE_OFF && c->aa == SNX_FX_AA_OFF)) return;
    v->fx = snx_fx_create(v->phys, v->device, VK_NULL_HANDLE, AGB_VK_SLOTS, fx_log);
    if (v->fx) {
        snx_fx_set_sharpness(v->fx, c->sharpness);
        snx_fx_set_fp16(v->fx, v->fp16);
        snx_fx_set_off_sharp(v->fx, 1); /* the window's own scale, in sharp pixels */
        return;
    }
    fprintf(stderr,
            "scarletnx-agb: upscaler=%s and anti_aliasing=%s: their passes could not be made on this device, so the "
            "screen shows with both off\n",
            snx_fx_upscale_key(c->upscaler), snx_fx_aa_key(c->aa));
    c->upscaler = SNX_FX_UPSCALE_OFF;
    c->aa = SNX_FX_AA_OFF;
}

AgbVk* agb_vk_open(const char* title, bool fullscreen, char* error, size_t error_size) {
    AgbVk* v = calloc(1, sizeof *v);
    char said[512];
    if (!v) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    if (!snx_vk_window_open("scarletnx-agb", title, AGB_SCREEN_WIDTH * 4, AGB_SCREEN_HEIGHT * 4, &v->host, error,
                            error_size)) {
        free(v);
        return NULL;
    }
    if (!v->host.window || !v->host.surface) {
        snprintf(error, error_size, "no Vulkan window on this display");
        agb_vk_close(v);
        return NULL;
    }
    if (fullscreen && !SDL_SetWindowFullscreen(v->host.window, true))
        fprintf(stderr, "scarletnx-agb: the window cannot go fullscreen: %s\n", SDL_GetError());
    snx_window_icon(v->host.window);
    if (!snx_vk_first_device(v->host.instance.instance, VK_API_VERSION_1_3, VK_QUEUE_GRAPHICS_BIT, v->host.surface,
                             make_device, v)) {
        snprintf(error, error_size, "no Vulkan 1.3 device with dynamic rendering, synchronization2 and a swapchain");
        agb_vk_close(v);
        return NULL;
    }
    volkLoadDevice(v->device);
    vkGetDeviceQueue(v->device, v->family, 0, &v->queue);
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo dp = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8, .poolSizeCount = 1, .pPoolSizes = &size};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    bool made = snx_vk_ring_init(&v->ring, v->device, v->queue, v->family, AGB_VK_SLOTS, &dp);
    for (unsigned i = 0; i < AGB_VK_SLOTS && made; ++i)
        made = vkCreateSemaphore(v->device, &si, NULL, &v->acquired[i]) == VK_SUCCESS && frame_make(v, &v->frame[i]);
    for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES && made; ++i)
        made = vkCreateSemaphore(v->device, &si, NULL, &v->rendered[i]) == VK_SUCCESS;
    if (!made || !(v->notice = snx_vk_notice_create(v->phys, v->device, AGB_VK_SLOTS)) || !swapchain_make(v)) {
        snprintf(error, error_size, "cannot make the Vulkan window's frames, swapchain or command pool");
        agb_vk_close(v);
        return NULL;
    }
    if (snx_fx_settings(&v->fx_config, g_settings.upscaler, g_settings.upscaler_sharpness, g_settings.anti_aliasing,
                        said, sizeof said))
        fprintf(stderr, "scarletnx-agb: settings: %s\n", said);
    fx_make(v);
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
        snx_vk_swapchain_free(v->device, &v->swap);
        for (unsigned i = 0; i < AGB_VK_SLOTS; ++i) {
            frame_free(v, &v->frame[i]);
            if (v->acquired[i]) vkDestroySemaphore(v->device, v->acquired[i], NULL);
        }
        for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
            if (v->rendered[i]) vkDestroySemaphore(v->device, v->rendered[i], NULL);
        snx_vk_ring_free(&v->ring);
        vkDestroyDevice(v->device, NULL);
    }
    snx_vk_window_close(&v->host);
    free(v);
}

void agb_vk_step(AgbVk* v, int which, char* text, size_t n) {
    SnxFxConfig* c = &v->fx_config;
    if (which)
        c->aa = snx_fx_aa_next(c->aa);
    else
        c->upscaler = snx_fx_upscale_next(c->upscaler);
    fx_make(v);
    snx_fx_step_text(which, which ? c->aa : c->upscaler, text, n);
    fprintf(stderr, "scarletnx-agb: %s, for this run\n", text);
}

bool agb_vk_frame(AgbVk* v, const uint32_t* framebuffer, const SnxNotice* notice) {
    uint32_t slot, index = 0;
    VkResult r;
    if (!v->swap.swapchain && !swapchain_make(v)) return false;
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
    /* the screen at the largest whole step the window holds, as SDL's renderer has it (integer scale), or shrunk to
       fit a window smaller than the screen */
    uint32_t w = v->swap.extent.width, h = v->swap.extent.height;
    uint32_t step = w / AGB_SCREEN_WIDTH < h / AGB_SCREEN_HEIGHT ? w / AGB_SCREEN_WIDTH : h / AGB_SCREEN_HEIGHT;
    uint32_t sw = AGB_SCREEN_WIDTH * step, sh = AGB_SCREEN_HEIGHT * step;
    if (!step) {
        sw = w, sh = (uint32_t)((uint64_t)w * AGB_SCREEN_HEIGHT / AGB_SCREEN_WIDTH);
        if (sh > h) sh = h, sw = (uint32_t)((uint64_t)h * AGB_SCREEN_WIDTH / AGB_SCREEN_HEIGHT);
    }
    int32_t x = (int32_t)(w - sw) / 2, y = (int32_t)(h - sh) / 2;
    SnxFxImage fx = {0};
    bool effects = false;
    if (v->fx && sw && sh) {
        barrier(cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        effects = snx_fx_render(v->fx, cb, slot, f->view, AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT, 0, 0, AGB_SCREEN_WIDTH,
                                AGB_SCREEN_HEIGHT, 0, sw, sh, v->fx_config.upscaler, v->fx_config.aa, &fx);
        if (!effects)
            barrier(cb, f->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    } else
        barrier(cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkImage target = v->swap.images[index];
    barrier(cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0},
                                       {(int32_t)(effects ? fx.width : AGB_SCREEN_WIDTH),
                                        (int32_t)(effects ? fx.height : AGB_SCREEN_HEIGHT), 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{x, y, 0}, {x + (int32_t)sw, y + (int32_t)sh, 1}}};
    /* the effects' image is the rectangle's size, copied; Off in sharp pixels; an upscaler that did not run leaves
       the smooth scale */
    vkCmdBlitImage(cb, effects ? fx.image : f->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   (effects && fx.width == sw && fx.height == sh) || v->fx_config.upscaler == SNX_FX_UPSCALE_OFF
                       ? VK_FILTER_NEAREST
                       : VK_FILTER_LINEAR);
    (void)snx_vk_notice_record(v->notice, cb, slot, target, v->swap.format.format, w, h,
                               snx_display_percent(SDL_GetWindowDisplayScale(v->host.window)), notice, SDL_GetTicksNS(),
                               fx_log);
    barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    (void)snx_vk_ring_submit(&v->ring, v->acquired[slot], v->rendered[index]);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &v->rendered[index],
                           .swapchainCount = 1,
                           .pSwapchains = &v->swap.swapchain,
                           .pImageIndices = &index};
    r = vkQueuePresentKHR(v->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) (void)swapchain_make(v);
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}
