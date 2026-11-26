/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The window: the frame image, the upper screen above the lower one as on the console, scaled into the window's
   swapchain image at the frame's shape through the upscaler and the anti-aliasing the Switch's settings ask for (sharp
   pixels with both off), the on-screen controls over it, and each submission sent with it. */
#include "nds/gpu/vk/vk.h"
#include "common/gpu/vk/gpu_pstate.h"
#include "nds/gpu/vk/vk_present.h"
#include "common/gpu/vk/present_fx.h"
#include "common/gpu/vk/rects.h"
#include "common/gpu/vk/swapchain.h"
#include "common/utilities/vpad_sdl.h"
#include "snx_settings.h"
#include <SDL3/SDL.h>
#include "common/gpu/vk/notice.h"
#include "common/utilities/notice.h"
#include <stdio.h>
#include <string.h>

static struct {
    SnxVkSwapchain swap;
    SnxVkPaced paced; /* its presents' ids, for frame_pacing */
    VkSemaphore acquire[NDS_VK_SLOTS], presented[NDS_VK_SLOTS];
    SnxVkRects* controls;
    SnxNotice notice;
    SnxVkNotice* notice_vk;
    SnxFxConfig fx_config;
    SnxFx* fx;
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
                               .views = true,
                               .unsynced = !g_settings.vsync}; /* paced by the console's frame (runtime/host.c) too */
    return snx_vk_swapchain_make(v->phys, v->device, &info, &g_present.swap);
}

static void controls_log(const char* text) { fprintf(stderr, "nds gpu: %s\n", text); }

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
    g_present.controls = snx_vk_rects_create(v->phys, v->device, NDS_VK_SLOTS);
    g_present.notice_vk = snx_vk_notice_create(v->phys, v->device, NDS_VK_SLOTS);
    if (!g_present.controls || !g_present.notice_vk || !swap_make()) {
        snprintf(error, error_size, "cannot make the window's swapchain");
        return false;
    }
    char said[512];
    snx_vk_present_said(g_present.swap.mode, g_settings.vsync, said, sizeof said);
    fprintf(stderr, "scarletnx-nds: %s\n", said);
    snx_vk_pacing_said(&v->pacing, g_settings.frame_pacing, said, sizeof said);
    fprintf(stderr, "scarletnx-nds: %s\n", said);
    /* the upscaler and the anti-aliasing the settings ask for, said once */
    if (snx_fx_settings(&g_present.fx_config, g_settings.upscaler, g_settings.upscaler_sharpness,
                        g_settings.anti_aliasing, said, sizeof said))
        fprintf(stderr, "scarletnx-nds: settings: %s\n", said);
    if (!(g_present.fx = snx_fx_create(v->phys, v->device, VK_NULL_HANDLE, NDS_VK_SLOTS, controls_log))) {
        snprintf(error, error_size, "cannot make the window's draw of the screens on this device");
        return false;
    }
    snx_fx_set_sharpness(g_present.fx, g_present.fx_config.sharpness);
    snx_fx_set_off_sharp(g_present.fx, 1);
    fprintf(stderr, "scarletnx-nds: upscaler %s, anti-aliasing %s\n",
            snx_fx_upscale_label(g_present.fx_config.upscaler), snx_fx_aa_label(g_present.fx_config.aa));
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
    snx_vk_rects_destroy(g_present.controls);
    snx_vk_notice_destroy(g_present.notice_vk);
    snx_fx_destroy(g_present.fx);
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

void nds_vk_notice(const char* text) { snx_notice_show(&g_present.notice, text, SDL_GetTicksNS()); }
const char* nds_vk_notice_text(void) {
    return snx_notice_shown(&g_present.notice, SDL_GetTicksNS()) ? g_present.notice.text : "";
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
    /* the upscaler's and the anti-aliasing's passes but their last, which draws in the window's rendering */
    nds_vk_layout(cb, frame, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkRect2D to = {{x0, y0}, {w, h}};
    bool drawn = w && h &&
                 snx_fx_prepare(g_present.fx, cb, slot, frame->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                NDS_SCREEN_WIDTH * v->scale, NDS_SCREEN_HEIGHT * 2 * v->scale, 0, 0,
                                NDS_SCREEN_WIDTH * v->scale, NDS_SCREEN_HEIGHT * 2 * v->scale, 0, to,
                                g_present.swap.format.format, g_present.fx_config.upscaler, g_present.fx_config.aa);
    snx_vk_swapchain_begin(cb, &g_present.swap, index);
    if (drawn) snx_fx_draw(g_present.fx, cb, slot);
    (void)snx_vk_notice_draw(g_present.notice_vk, cb, slot, g_present.swap.format.format, extent.width, extent.height,
                             snx_display_percent(SDL_GetWindowDisplayScale(v->host.window)), &g_present.notice,
                             SDL_GetTicksNS(), controls_log);
    (void)snx_vk_rects_draw(g_present.controls, cb, slot, g_present.swap.format.format, extent.width, extent.height,
                            snx_vpad_window_rects((int)extent.width, (int)extent.height), controls_log);
    snx_vk_swapchain_end(cb, &g_present.swap, index);
    (void)snx_vk_ring_submit(&v->ring, g_present.acquire[slot], g_present.presented[slot]);
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &g_present.presented[slot],
                                .swapchainCount = 1,
                                .pSwapchains = &g_present.swap.swapchain,
                                .pImageIndices = &index};
    snx_gpu_pstate_present(); /* a kgsl hold lasts while frames are presented */
    snx_vk_pacing_present(&v->pacing, &g_present.paced, v->device, g_present.swap.swapchain,
                          g_settings.vsync && snx_vpad_window_frame_ns(1) == 1, &present, "scarletnx-nds");
    VkResult shown = vkQueuePresentKHR(v->queue, &present);
    (void)snx_vk_ring_poll(&v->ring);
    if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR || got == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(v->device);
        (void)snx_vk_ring_poll(&v->ring);
        (void)swap_make();
    }
}
