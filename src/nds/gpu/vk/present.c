/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The window: the frame image, the upper screen above the lower one as on the console, scaled into the window's
   swapchain image at the frame's shape, and each submission sent with it. */
#include "nds/gpu/vk/vk.h"
#include "nds/gpu/vk/vk_present.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

static struct {
    SnxVkSwapchain swap;
    VkSemaphore acquire[NDS_VK_SLOTS], presented[NDS_VK_SLOTS];
    VkOffset2D rect[2]; /* where the frame is in the window */
    bool up;
} g_present;

static bool swap_make(void) {
    NdsVk* v = nds_vk();
    static const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    SnxVkSwapchainInfo info = {.surface = v->host.surface,
                               .formats = formats,
                               .format_count = 2,
                               .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                               .window = snx_vk_window_extent(&v->host, (VkExtent2D){512, 768}),
                               .unsynced = true}; /* the run paces itself by the console's frame (runtime/host.c) */
    return snx_vk_swapchain_make(v->phys, v->device, &info, &g_present.swap);
}

bool nds_vk_present_init(char* error, size_t error_size) {
    NdsVk* v = nds_vk();
    if (!v->host.surface) return true;
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (unsigned i = 0; i < NDS_VK_SLOTS; ++i)
        if (vkCreateSemaphore(v->device, &si, NULL, &g_present.acquire[i]) != VK_SUCCESS ||
            vkCreateSemaphore(v->device, &si, NULL, &g_present.presented[i]) != VK_SUCCESS) {
            snprintf(error, error_size, "cannot make the window's semaphores");
            return false;
        }
    if (!swap_make()) {
        snprintf(error, error_size, "cannot make the window's swapchain");
        return false;
    }
    g_present.up = true;
    return true;
}

void nds_vk_present_free(void) {
    NdsVk* v = nds_vk();
    if (!v) return;
    vkDeviceWaitIdle(v->device);
    for (unsigned i = 0; i < NDS_VK_SLOTS; ++i) {
        if (g_present.acquire[i]) vkDestroySemaphore(v->device, g_present.acquire[i], NULL);
        if (g_present.presented[i]) vkDestroySemaphore(v->device, g_present.presented[i], NULL);
    }
    if (g_present.swap.swapchain) snx_vk_swapchain_free(v->device, &g_present.swap);
    memset(&g_present, 0, sizeof g_present);
}

bool nds_vk_present_point(float x, float y, int* frame_x, int* frame_y) {
    int w = g_present.rect[1].x - g_present.rect[0].x, h = g_present.rect[1].y - g_present.rect[0].y;
    if (w <= 0 || h <= 0) return false;
    float fx = (x - (float)g_present.rect[0].x) * (float)NDS_SCREEN_WIDTH / (float)w;
    float fy = (y - (float)g_present.rect[0].y) * (float)(NDS_SCREEN_HEIGHT * 2) / (float)h;
    *frame_x = (int)fx, *frame_y = (int)fy;
    return fx >= 0 && fy >= 0 && fx < (float)NDS_SCREEN_WIDTH && fy < (float)(NDS_SCREEN_HEIGHT * 2);
}

void nds_vk_present(void) {
    NdsVk* v = nds_vk();
    if (!v) return;
    if (!g_present.up || !g_present.swap.swapchain) {
        (void)snx_vk_ring_submit(&v->ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        (void)snx_vk_ring_poll(&v->ring);
        return;
    }
    VkCommandBuffer cb = snx_vk_ring_commands(&v->ring);
    unsigned slot = v->ring.slot;
    uint32_t index = 0;
    VkResult got = vkAcquireNextImageKHR(v->device, g_present.swap.swapchain, 0, g_present.acquire[slot], VK_NULL_HANDLE,
                                         &index);
    if (got == VK_ERROR_OUT_OF_DATE_KHR || got == VK_SUBOPTIMAL_KHR) {
        if (got == VK_SUBOPTIMAL_KHR) {
            /* the image is ours all the same: shown as it is, and the swapchain made anew after */
        } else {
            (void)snx_vk_ring_submit(&v->ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
            vkDeviceWaitIdle(v->device);
            (void)snx_vk_ring_poll(&v->ring);
            (void)swap_make();
            return;
        }
    } else if (got != VK_SUCCESS) { /* no image free now: the frame is drawn and not shown */
        (void)snx_vk_ring_submit(&v->ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
        (void)snx_vk_ring_poll(&v->ring);
        return;
    }
    NdsVkImage* frame = nds_vk_frame_image();
    VkExtent2D extent = g_present.swap.extent;
    /* the frame at its shape, 2 to 3, as large as the window takes */
    uint32_t w = extent.width, h = extent.width * 3 / 2;
    if (h > extent.height) h = extent.height, w = extent.height * 2 / 3;
    int32_t x0 = (int32_t)(extent.width - w) / 2, y0 = (int32_t)(extent.height - h) / 2;
    g_present.rect[0] = (VkOffset2D){x0, y0};
    g_present.rect[1] = (VkOffset2D){x0 + (int32_t)w, y0 + (int32_t)h};
    VkImage target = g_present.swap.images[index];
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = target,
                                    .subresourceRange = range};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &barrier);
    VkClearColorValue black = {{0, 0, 0, 1}};
    vkCmdClearColorImage(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    nds_vk_layout(cb, frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT * 2, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{x0, y0, 0}, {x0 + (int32_t)w, y0 + (int32_t)h, 1}}};
    if (w && h)
        vkCmdBlitImage(cb, frame->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
                         &barrier);
    (void)snx_vk_ring_submit(&v->ring, g_present.acquire[slot], g_present.presented[slot]);
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &g_present.presented[slot],
                                .swapchainCount = 1,
                                .pSwapchains = &g_present.swap.swapchain,
                                .pImageIndices = &index};
    VkResult shown = vkQueuePresentKHR(v->queue, &present);
    (void)snx_vk_ring_poll(&v->ring);
    if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR || got == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(v->device);
        (void)snx_vk_ring_poll(&v->ring);
        (void)swap_make();
    }
}
