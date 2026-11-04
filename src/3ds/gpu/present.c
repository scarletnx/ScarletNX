/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The shown framebuffers, on the 3DS's Vulkan device (vk/device.c): a compute shader (vk/shaders/present.comp)
   reads each one where the guest has it in RAM, decodes it and turns it to the console's landscape screen in an
   image, and the window's swapchain shows both screens, the top one above the bottom one as on the console, each
   keeping its shape (frame_layout). With separate screens each one has its own window and fills it. Nothing of the
   frame passes through the CPU. The frame dumps (the first eight, then each screen's latest about once a second, in
   frame_dir) and the tests' hashes are the one readback: the shader writes those frames' RGBA pixels to a
   host-visible buffer as well. The 3DS framebuffer is 240x400/320 portrait, one 240-pixel row per screen column from
   the left, each row from the bottom of the screen up (libctru's console draws pixel (x, y) at x * 240 + 239 - y);
   the window and the dumps show it that way round. A 3D render scaled by its screen's resolution setting is drawn
   from its render target while the GPU keeps that newer than memory. */
#include "3ds/gpu/present.h"
#include "3ds/input/input.h"
#include "common/gpu/vk/interp_auto.h"
#include "common/gpu/vk/present_fx.h"
#include "common/gpu/vk/stats.h"
#include "common/gpu/vk/toast.h"
#include "common/gpu/vk/hotkeys.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/shaders/present_spv.h"
#include "3ds/gpu/vk/vk_transfer.h"
#include "3ds/gpu/vk/vk_y2r.h"
#include "3ds/gpu/vk/vk_render.h"
#include "common/utilities/bits.h"
#include "common/utilities/keybind.h"
#include "common/gpu/vk/swapchain.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/window_icon.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SIDE 1024u /* the largest framebuffer side presented */

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkImageLayout layout;   /* where the last command recorded on it left it */
    unsigned width, height; /* landscape */
    unsigned scale;         /* the render scale it shows: 1 is the console's */
    SnxFxImage fx;          /* the upscaled and anti aliased image of this screen, when the effects run */
} Frame;
static struct {
    SnxCtrVkCompute decode;
    Frame frames[2];
    Frame source_dummy; /* bound where a frame is decoded from the guest, which the shader does not read */
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    unsigned char* readback_view;
    SnxVkSwapchain swap[2]; /* the combined window's, or each screen's own */
    VkImageView swap_views[2][SNX_VK_SWAP_IMAGES];
    VkSwapchainKHR swap_views_for[2];
    SnxFx* fx[2];     /* each screen's effects, a context of its own so their frame slots stay apart */
    SnxToast* toast;  /* the hotkeys' messages, over both windows */
    SnxText* overlay; /* the frame rate and the held buttons, on F6, over both windows */
    SnxStats stats;
    int paper; /* the SDR white in nits: the setting, or the display's own */
    int peak;  /* the peak in nits: the setting, or the display's own */
    int display_kind[2];
    int said_kind[2];  /* the display kind last said in the console, -1 before the first */
    int auto_tried[2]; /* Auto has looked for the display's headroom once, which SDL reports after the first frame */
    SnxInterpAuto interp_auto; /* Auto's 1 % figures and its on and off delays */
    int headroom_pct[2];
    /* the SDR window image the pair is composed in before the HDR passes (source/upscale first, then the whole pair) */
    VkImage mix[2];
    VkDeviceMemory mix_memory[2];
    VkImageView mix_view[2];
    uint32_t mix_width[2], mix_height[2];
    VkImageLayout mix_layout[2];
    /* each submission slot's: a slot is free again only once its submission is done, and so are they */
    VkSemaphore acquire[2][SNX_CTR_VK_SLOTS], presented[2][SNX_CTR_VK_SLOTS];
    VkFence acquire_fence[2]; /* the second image of an interpolated pair, waited for on the CPU */
    const char* frame_dir;
    unsigned dumps;
    uint64_t presents;     /* past the first dumps, two presents (one a screen) of every 120 are kept as the latest */
    uint64_t quit_armed_ns; /* the second Esc, before this, quits: a first one only warns */
    unsigned bottom_alone; /* bottom screen frames since the window last showed, the top screen showing none */
    unsigned up : 1, closed : 1, separate : 1;
    int have_mix[2];
} g_present;

static void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout old, VkImageLayout next);
/* the effects' short messages: the console, where this frontend has no log file */
static void present_fx_log(const char* text) { fprintf(stderr, "%s\n", text); }

static void swap_destroy(unsigned screen) { snx_vk_swapchain_free(snx_ctr_vk()->device, &g_present.swap[screen]); }
static SDL_Window* screen_window(unsigned screen) {
    const SnxCtrVk* v = snx_ctr_vk();
    return g_present.separate ? (screen ? v->second : v->host.window) : v->host.window;
}
static VkSurfaceKHR screen_surface(unsigned screen) {
    const SnxCtrVk* v = snx_ctr_vk();
    return g_present.separate ? (screen ? v->second_surface : v->host.surface) : v->host.surface;
}
/* the image the window takes: an HDR one where the settings and the surface allow it, else the SDR formats */
static int choose_display(unsigned window, VkFormat* format, VkColorSpaceKHR* space) {
    const SnxCtrVk* v = snx_ctr_vk();
    VkSurfaceKHR surface = screen_surface(window);
    static const VkFormat hdr_formats[] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32};
    static const VkFormat scrgb_formats[] = {VK_FORMAT_R16G16B16A16_SFLOAT};
    VkSurfaceFormatKHR f = {0};
    int mode = g_present.fx[window] ? snx_fx_hdr_mode(g_present.fx[window]) : snx_fx_hdr_parse(g_settings.hdr);
    int cs = snx_fx_colorspace_parse(g_settings.hdr_colours);
    int kind = SNX_FX_DISPLAY_NONE;
    if (!surface || mode == SNX_FX_HDR_OFF) return SNX_FX_DISPLAY_NONE;
    if (cs == SNX_FX_CS_AUTO || cs == SNX_FX_CS_P3) {
        if (snx_vk_surface_format_cs(v->phys, surface, scrgb_formats, 1, VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT, &f))
            kind = SNX_FX_DISPLAY_P3;
        else if (cs == SNX_FX_CS_P3)
            return SNX_FX_DISPLAY_NONE;
    }
    if (kind == SNX_FX_DISPLAY_NONE && (cs == SNX_FX_CS_AUTO || cs == SNX_FX_CS_SCRGB)) {
        if (snx_vk_surface_format_cs(v->phys, surface, scrgb_formats, 1, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, &f))
            kind = SNX_FX_DISPLAY_SCRGB;
        else if (cs == SNX_FX_CS_SCRGB)
            return SNX_FX_DISPLAY_NONE;
    }
    if (kind == SNX_FX_DISPLAY_NONE &&
        snx_vk_surface_format_cs(v->phys, surface, hdr_formats, 2, VK_COLOR_SPACE_HDR10_ST2084_EXT, &f))
        kind = SNX_FX_DISPLAY_PQ;
    if (kind == SNX_FX_DISPLAY_NONE) return SNX_FX_DISPLAY_NONE;
    /* Auto takes an extended kind only where the display reports headroom over its white, as the Switch does: an
       HDR10 target's signal is absolute and the surface taking it is the test */
    if (mode == SNX_FX_HDR_AUTO && kind != SNX_FX_DISPLAY_PQ &&
        snx_vk_window_headroom_pct(screen_window(window)) <= 100)
        return SNX_FX_DISPLAY_NONE;
    *format = f.format;
    *space = f.colorSpace;
    return kind;
}
/* the kind, and the light over its white when it takes an HDR image */
static const char* display_said(unsigned window, int kind) {
    static char said[2][48];
    if (kind == SNX_FX_DISPLAY_NONE) return snx_fx_display_label(kind);
    snprintf(said[window], sizeof said[window], "%s, %d%% headroom", snx_fx_display_label(kind),
             g_present.headroom_pct[window]);
    return said[window];
}
static void swap_views_free(unsigned window) {
    const SnxCtrVk* v = snx_ctr_vk();
    for (uint32_t i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
        if (g_present.swap_views[window][i]) vkDestroyImageView(v->device, g_present.swap_views[window][i], NULL);
    memset(g_present.swap_views[window], 0, sizeof g_present.swap_views[window]);
    g_present.swap_views_for[window] = VK_NULL_HANDLE;
}
/* a window's swapchain, made anew for its size */
static bool swap_create(unsigned screen) {
    const SnxCtrVk* v = snx_ctr_vk();
    SDL_Window* window = screen_window(screen);
    VkSurfaceKHR surface = screen_surface(screen);
    VkFormat hdr_format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR hdr_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (!window || !surface) return false;
    static const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    int kind = choose_display(screen, &hdr_format, &hdr_space);
    VkExtent2D fallback =
        g_present.separate ? (screen ? (VkExtent2D){640, 480} : (VkExtent2D){800, 480}) : (VkExtent2D){600, 720};
    SnxVkSwapchainInfo info = {
        .surface = surface,
        .formats = kind != SNX_FX_DISPLAY_NONE ? &hdr_format : formats,
        .format_count = kind != SNX_FX_DISPLAY_NONE ? 1 : (uint32_t)(sizeof formats / sizeof *formats),
        .color_space = kind != SNX_FX_DISPLAY_NONE ? hdr_space : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .window = snx_vk_window_extent(&v->host, fallback)};
    if (window != v->host.window) { /* the second window's own size */
        int width = 0, height = 0;
        info.window = SDL_GetWindowSizeInPixels(window, &width, &height) && width > 0 && height > 0
                          ? (VkExtent2D){(uint32_t)width, (uint32_t)height}
                          : fallback;
    }
    g_present.display_kind[screen] = kind;
    {
        /* the display's own headroom where SDL reports it (Wayland's color management, Windows' Advanced Color),
           else the configured peak: the SDR frame is mapped into it when HDR is on */
        int reported = snx_vk_window_headroom_pct(window);
        g_present.headroom_pct[screen] =
            kind == SNX_FX_DISPLAY_PQ
                ? snx_clamp_int(g_present.peak * 100 / g_present.paper, 100, 1000)
                : (reported > 100 ? snx_clamp_int(reported, 100, 1000) : 100);
    }
    if (kind != g_present.said_kind[screen]) {
        g_present.said_kind[screen] = kind;
        if (g_present.separate)
            printf("3ds: the %s window takes %s\n", screen ? "bottom" : "top", display_said(screen, kind));
        else
            printf("3ds: the window takes %s\n", display_said(screen, kind));
    }
    if (g_present.fx[screen]) snx_fx_set_display(g_present.fx[screen], kind, g_present.headroom_pct[screen]);
    if (g_present.swap_views_for[screen]) {
        vkDeviceWaitIdle(v->device);
        swap_views_free(screen);
    }
    if (!snx_vk_swapchain_make(v->phys, v->device, &info, &g_present.swap[screen])) return false;
    if (v->hdr_metadata)
        snx_vk_swapchain_metadata(v->device, g_present.swap[screen].swapchain, g_present.swap[screen].format.format,
                                  g_present.swap[screen].format.colorSpace, g_present.peak, g_present.paper);
    /* a view of every window image: the HDR encode draws into it, and the toast draws into the SDR one too */
    for (uint32_t i = 0; i < g_present.swap[screen].count; ++i) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .image = g_present.swap[screen].images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = g_present.swap[screen].format.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(v->device, &vi, NULL, &g_present.swap_views[screen][i]) != VK_SUCCESS) {
            swap_views_free(screen);
            return false;
        }
    }
    g_present.swap_views_for[screen] = g_present.swap[screen].swapchain;
    return true;
}
/* the SDR image the pair is composed in before the HDR passes, at the window's image size */
static bool mix_image(unsigned window, uint32_t width, uint32_t height) {
    const SnxCtrVk* v = snx_ctr_vk();
    if (g_present.mix[window] && g_present.mix_width[window] == width && g_present.mix_height[window] == height)
        return true;
    vkDeviceWaitIdle(v->device);
    if (g_present.mix_view[window]) vkDestroyImageView(v->device, g_present.mix_view[window], NULL);
    if (g_present.mix[window]) vkDestroyImage(v->device, g_present.mix[window], NULL);
    if (g_present.mix_memory[window]) vkFreeMemory(v->device, g_present.mix_memory[window], NULL);
    g_present.mix[window] = VK_NULL_HANDLE, g_present.mix_memory[window] = VK_NULL_HANDLE,
    g_present.mix_view[window] = VK_NULL_HANDLE;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {width, height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT};
    if (vkCreateImage(v->device, &ii, NULL, &g_present.mix[window]) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->device, g_present.mix[window], &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex =
                                   snx_ctr_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = snx_ctr_vk_memory_type(req.memoryTypeBits, 0);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = g_present.mix[window],
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(v->device, &ai, NULL, &g_present.mix_memory[window]) != VK_SUCCESS ||
        vkBindImageMemory(v->device, g_present.mix[window], g_present.mix_memory[window], 0) != VK_SUCCESS ||
        vkCreateImageView(v->device, &vi, NULL, &g_present.mix_view[window]) != VK_SUCCESS)
        return false;
    g_present.mix_width[window] = width, g_present.mix_height[window] = height;
    return true;
}
static bool frame_make(Frame* f, unsigned width, unsigned height, unsigned scale) {
    const SnxCtrVk* v = snx_ctr_vk();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {width, height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(v->device, &ii, NULL, &f->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->device, f->image, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex =
                                   snx_ctr_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = snx_ctr_vk_memory_type(req.memoryTypeBits, 0);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = f->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkAllocateMemory(v->device, &ai, NULL, &f->memory) != VK_SUCCESS ||
        vkBindImageMemory(v->device, f->image, f->memory, 0) != VK_SUCCESS ||
        vkCreateImageView(v->device, &vi, NULL, &f->view) != VK_SUCCESS)
        return false;
    f->width = width;
    f->height = height;
    f->scale = scale ? scale : 1;
    f->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}
static void frame_free(Frame* f) {
    const SnxCtrVk* v = snx_ctr_vk();
    if (f->view) vkDestroyImageView(v->device, f->view, NULL);
    if (f->image) vkDestroyImage(v->device, f->image, NULL);
    if (f->memory) vkFreeMemory(v->device, f->memory, NULL);
    *f = (Frame){0};
}

static bool semaphores(const SnxCtrVk* v, const VkSemaphoreCreateInfo* si) {
    for (unsigned screen = 0; screen < 2; ++screen)
        for (unsigned i = 0; i < SNX_CTR_VK_SLOTS; ++i)
            if (vkCreateSemaphore(v->device, si, NULL, &g_present.acquire[screen][i]) != VK_SUCCESS ||
                vkCreateSemaphore(v->device, si, NULL, &g_present.presented[screen][i]) != VK_SUCCESS)
                return false;
    return true;
}
bool snx_ctr_present_init(SnxCtrMemory* memory, const char* frame_dir) {
    if (g_present.up) return true;
    char why[1024];
    if (!snx_ctr_vk_init(memory, why, sizeof why)) {
        fprintf(stderr, "3ds gpu: %s\n", why);
        return false;
    }
    const SnxCtrVk* v = snx_ctr_vk();
    g_present.frame_dir = frame_dir;
    g_present.said_kind[0] = g_present.said_kind[1] = -1;
    g_present.toast = snx_toast_create(v->phys, v->device, v->queue, v->family, VK_NULL_HANDLE, SNX_CTR_VK_SLOTS);
    g_present.overlay = snx_text_create(v->phys, v->device, v->queue, v->family, VK_NULL_HANDLE, SNX_CTR_VK_SLOTS);
    snx_stats_init(&g_present.stats);
    g_present.paper = snx_vk_paper_nits(g_settings.hdr_paper_white);
    g_present.peak = snx_vk_peak_nits(g_settings.hdr_peak);
    snx_interp_auto_init(&g_present.interp_auto);
    g_present.stats.on = g_settings.stats != 0; /* the setting starts it, F6 turns it over for the run */
    g_present.separate = g_settings.ctr_separate_screens && v->second && v->second_surface;
    for (unsigned i = 0; i < 2; ++i) {
        g_present.fx[i] = snx_fx_create(v->phys, v->device, v->queue, v->family, VK_NULL_HANDLE, SNX_CTR_VK_SLOTS,
                                        snx_ctr_vk_slot, NULL, present_fx_log);
        if (g_present.fx[i]) {
            SnxFxConfig cfg = {snx_fx_upscale_parse(g_settings.upscaler),
                               g_settings.sharpness,
                               snx_fx_aa_parse(g_settings.anti_aliasing),
                               snx_fx_hdr_parse(g_settings.hdr),
                               snx_fx_colorspace_parse(g_settings.hdr_colours),
                               snx_fx_source_parse(g_settings.hdr_source),
                               g_present.paper,
                               g_present.peak,
                               g_settings.hdr_expansion,
                               g_settings.hdr_test_pattern};
            snx_fx_configure(g_present.fx[i], &cfg);
            snx_fx_set_fp16(g_present.fx[i], 0);
        }
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = (VkDeviceSize)MAX_SIDE * MAX_SIDE * 4,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (unsigned i = 0; i < 2; ++i) {
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(v->device, &fi, NULL, &g_present.acquire_fence[i]) != VK_SUCCESS) return false;
    }
    if (!snx_ctr_vk_compute(snx_ctr_present_spv, snx_ctr_present_spv_size / 4, 2, 2, 36, &g_present.decode) ||
        !frame_make(&g_present.source_dummy, 1, 1, 1) ||
        vkCreateBuffer(v->device, &bi, NULL, &g_present.readback) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->device, g_present.readback, &req);
    /* host-cached where the device has it: the host reads these frames */
    uint32_t type = snx_ctr_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX)
        type = snx_ctr_vk_memory_type(req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    void* mapped = NULL;
    if (type == UINT32_MAX || vkAllocateMemory(v->device, &ai, NULL, &g_present.readback_memory) != VK_SUCCESS ||
        vkBindBufferMemory(v->device, g_present.readback, g_present.readback_memory, 0) != VK_SUCCESS ||
        vkMapMemory(v->device, g_present.readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS ||
        !semaphores(v, &si))
        return false;
    g_present.readback_view = mapped;
    image_barrier(snx_ctr_vk_commands(), g_present.source_dummy.image, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_GENERAL);
    g_present.source_dummy.layout = VK_IMAGE_LAYOUT_GENERAL;
    (void)swap_create(0);
    if (g_present.separate) (void)swap_create(1);
    g_present.up = 1;
    return true;
}
void snx_ctr_present_title(const char* name, const char* addons) {
    const SnxCtrVk* v = snx_ctr_vk();
    if (!g_present.up || !v->host.window) return;
    char header[768];
    if (addons && addons[0])
        snprintf(header, sizeof header, "%s (%s) - %s", name, addons, snx_build_info());
    else
        snprintf(header, sizeof header, "%s - %s", name, snx_build_info());
    if (v->second) {
        char top[800], bottom[800];
        snprintf(top, sizeof top, "%s (top)", header);
        snprintf(bottom, sizeof bottom, "%s (bottom)", header);
        SDL_SetWindowTitle(v->host.window, top);
        SDL_SetWindowTitle(v->second, bottom);
        snx_window_icon(v->second);
    } else
        SDL_SetWindowTitle(v->host.window, header);
    snx_window_icon(v->host.window);
}
bool snx_ctr_present_ready(void) { return g_present.up; }
bool snx_ctr_present_windowed(void) { return g_present.swap[0].swapchain || g_present.swap[1].swapchain; }
bool snx_ctr_present_separate(void) {
    return g_present.up && g_present.separate && g_present.swap[0].swapchain && g_present.swap[1].swapchain;
}

/* whether this present is one the dumps keep: the first eight frames, then about one a second as
   frame-<screen>-latest.ppm */
static bool dump_due(void) {
    if (!g_present.frame_dir) return false;
    if (g_present.dumps < 8) return true;
    return g_present.presents++ % 120 < 2;
}
/* the readback's frame (framebuffer order, RGBA) as a landscape PPM, a latest one written beside its file and renamed
   over it so a reader never sees half of one */
static void write_ppm(unsigned screen, unsigned width, unsigned height, const unsigned char* rgba) {
    bool latest = g_present.dumps >= 8;
    char path[1024], staged[1040];
    if (latest)
        snprintf(path, sizeof path, "%s/frame-%u-latest.ppm", g_present.frame_dir, screen);
    else
        snprintf(path, sizeof path, "%s/frame-%u-%02u.ppm", g_present.frame_dir, screen, g_present.dumps);
    snx_path_native(path);
    snprintf(staged, sizeof staged, "%s.partial", path);
    FILE* file = fopen(latest ? staged : path, "wb");
    if (!file) return;
    fprintf(file, "P6\n%u %u\n255\n", height, width);
    for (uint32_t y = 0; y < width; ++y)        /* the landscape rows */
        for (uint32_t x = 0; x < height; ++x) { /* the landscape columns */
            const unsigned char* p = rgba + ((uint64_t)x * width + (width - 1 - y)) * 4;
            if (fwrite(p, 1, 3, file) != 3) break;
        }
    bool closed = !fclose(file);
    if (!latest)
        ++g_present.dumps;
    else if (closed)
        (void)snx_replace(staged, path);
}
/* The blit rectangle of a screen in swapchain pixels: in one window the pair is laid out in the window, each screen
   centred across the layout; in its own window the screen fills it. A window of another shape stretches the picture up
   to half as much again, and fullscreen a quarter, bars covering the rest. False before a layout and a swapchain
   exist. */
static bool screen_rect(unsigned screen, VkOffset2D rect[2]) {
    const Frame* frames = g_present.frames;
    unsigned window = g_present.separate ? screen : 0;
    VkExtent2D e = g_present.swap[window].extent;
    SDL_Window* win = screen_window(window);
    /* fullscreen stretches the picture at most a quarter of the way, a window half as much again; bars cover the rest */
    double limit = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) ? 1.25 : 1.5;
    double sx, sy, scale, top;
    if (g_present.separate) {
        const Frame* f = &frames[screen];
        if (!f->width || !f->height || !e.width || !e.height) return false;
        sx = (double)e.width / f->width, sy = (double)e.height / f->height;
        scale = sx < sy ? sx : sy;
        top = scale * limit;
        sx = sx < top ? sx : top;
        sy = sy < top ? sy : top;
        rect[0] = (VkOffset2D){(int32_t)((e.width - f->width * sx) / 2), (int32_t)((e.height - f->height * sy) / 2)};
        rect[1] = (VkOffset2D){(int32_t)(rect[0].x + f->width * sx), (int32_t)(rect[0].y + f->height * sy)};
        return true;
    }
    uint32_t layout_w = snx_max_u32(frames[0].width, frames[1].width);
    uint32_t layout_h = frames[0].height + frames[1].height;
    if (!layout_w || !layout_h || !e.width || !e.height) return false;
    sx = (double)e.width / layout_w, sy = (double)e.height / layout_h;
    scale = sx < sy ? sx : sy;
    top = scale * limit;
    sx = sx < top ? sx : top;
    sy = sy < top ? sy : top;
    const Frame* f = &frames[screen];
    double x = (e.width - layout_w * sx) / 2 + (layout_w - f->width) * sx / 2;
    double y = (e.height - layout_h * sy) / 2 + (screen ? frames[0].height * sy : 0);
    rect[0] = (VkOffset2D){(int32_t)x, (int32_t)y};
    rect[1] = (VkOffset2D){(int32_t)(x + f->width * sx), (int32_t)(y + f->height * sy)};
    return true;
}
/* The touch screen's pixel under a pointer at x and y, in the window's points as SDL reports them. The blit's own scale
   maps them, so a swapchain whose pixels differ from the window's points still lands on what is shown. As the Switch
   runtime's finger handling does, a press off the screen starts nothing and a drag off it keeps to its edge. */
static void pointer_touch(SDL_Window* window, float x, float y, bool press) {
    const Frame* bottom = &g_present.frames[1];
    unsigned screen = 1, window_index = g_present.separate ? screen : 0;
    VkExtent2D e = g_present.swap[window_index].extent;
    int width = 0, height = 0;
    VkOffset2D rect[2];
    if (!SDL_GetWindowSize(window, &width, &height) || width <= 0 || height <= 0 || !screen_rect(screen, rect)) return;
    /* the division below is by the rect's size, so a rect with no pixels has nothing to map */
    if (rect[1].x <= rect[0].x || rect[1].y <= rect[0].y) return;
    double px = (double)x * e.width / width;
    double py = (double)y * e.height / height;
    if (press && (px < rect[0].x || px >= rect[1].x || py < rect[0].y || py >= rect[1].y)) return;
    px = snx_clamp_double(px, rect[0].x, rect[1].x);
    py = snx_clamp_double(py, rect[0].y, rect[1].y);
    unsigned native_width = bottom->width / bottom->scale, native_height = bottom->height / bottom->scale;
    uint32_t tx = (uint32_t)((px - rect[0].x) * native_width / (rect[1].x - rect[0].x));
    uint32_t ty = (uint32_t)((py - rect[0].y) * native_height / (rect[1].y - rect[0].y));
    snx_ctr_input_touch(true, snx_min_u32(tx, native_width - 1), snx_min_u32(ty, native_height - 1));
}
static void pointer_event(const SDL_Event* event) {
    const SnxCtrVk* v = snx_ctr_vk();
    if (!v || !v->host.window) return;
    SDL_Window* bottom = g_present.separate && v->second ? v->second : v->host.window;
    SDL_WindowID id = SDL_GetWindowID(bottom);
    switch (event->type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event->button.windowID == id && event->button.button == SDL_BUTTON_LEFT)
                pointer_touch(bottom, event->button.x, event->button.y, true);
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP: /* a release off the screen still ends the press */
            if (event->button.windowID == id && event->button.button == SDL_BUTTON_LEFT)
                snx_ctr_input_touch(false, 0, 0);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (event->motion.windowID == id && snx_ctr_input_touched())
                pointer_touch(bottom, event->motion.x, event->motion.y, false);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            /* a focus loss ends the press: this window may not get the release */
            if (event->window.windowID == id) snx_ctr_input_touch(false, 0, 0);
            break;
    }
}
/* The runtime's live keys, the same as the other frontends': the upscaler, the anti aliasing, HDR and frame
   interpolation change the effects, with a toast and a log line. A press is taken, a repeat included, so the game
   never sees it. */
static int hotkey_event(const SDL_KeyboardEvent* e) {
    int action = snx_hotkey_match(e);
    char text[64];
    int value = -1;
    if (action == SNX_HOTKEY_NONE) return 0;
    if (e->repeat || !g_present.fx[0]) return 1;
    switch (action) {
        case SNX_HOTKEY_UPSCALER:
            value = snx_fx_upscale_cycle(g_present.fx[0]);
            if (g_present.fx[1]) snx_fx_upscale_cycle(g_present.fx[1]);
            break;
        case SNX_HOTKEY_AA:
            value = snx_fx_aa_cycle(g_present.fx[0]);
            if (g_present.fx[1]) snx_fx_aa_cycle(g_present.fx[1]);
            break;
        case SNX_HOTKEY_HDR:
            value = snx_fx_hdr_toggle(g_present.fx[0]);
            if (g_present.fx[1]) snx_fx_hdr_toggle(g_present.fx[1]);
            break;
        case SNX_HOTKEY_INTERP: value = snx_hotkey_next_interp(); break;
    }
    if (value < 0) return 1;
    snx_hotkey_text(action, value, text, sizeof text);
    if (g_present.toast) snx_toast_show(g_present.toast, text, SDL_GetTicksNS());
    printf("%s (%s)\n", text, snx_hotkey_key_name(action));
    return 1;
}
/* F11 and Alt+Enter: the window the key came from, fullscreen or back, as every other frontend's */
static void toggle_fullscreen(const SDL_KeyboardEvent* key) {
    const SnxCtrVk* v = snx_ctr_vk();
    SDL_Window* window = NULL;
    if (v->host.window && SDL_GetWindowID(v->host.window) == key->windowID)
        window = v->host.window;
    else if (v->second && SDL_GetWindowID(v->second) == key->windowID)
        window = v->second;
    if (!window) window = v->host.window;
    if (window) SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
}
static void pump(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g_present.closed = 1;
        if (event.type == SDL_EVENT_KEY_DOWN &&
            (event.key.key == SDLK_F11 || (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)))) {
            if (!event.key.repeat) toggle_fullscreen(&event.key);
            continue;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !event.key.repeat) {
            uint64_t now = SDL_GetTicksNS();
            if (g_present.quit_armed_ns && now < g_present.quit_armed_ns) {
                g_present.closed = 1;
            } else {
                g_present.quit_armed_ns = now + 3000000000ull;
                if (g_present.toast)
                    snx_toast_show_ms(g_present.toast, "Press Esc again to quit", now, 3000);
                printf("Press Esc again to quit\n");
            }
            continue;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F6 && !event.key.repeat) {
            printf("showcase: %s\n", snx_stats_toggle(&g_present.stats, 1) ? "on" : "off");
            continue;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F12 && !event.key.repeat) {
            printf("stats: %s\n", snx_stats_toggle(&g_present.stats, 0) ? "on" : "off");
            continue;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && hotkey_event(&event.key)) continue;
        pointer_event(&event);
        snx_ctr_input_event(&event); /* the gamepads connected and gone */
    }
}
/* SDL turns SIGINT and SIGTERM into a quit event too, so this is also how Ctrl+C ends a run, with or without a window
   (the audio output initializes SDL's events) */
bool snx_ctr_present_closed(void) {
    if (SDL_WasInit(SDL_INIT_EVENTS)) pump();
    return g_present.closed;
}
static void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout old, VkImageLayout next) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = old,
                              .newLayout = next,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
}
/* The window's picture: in one window the top screen above the bottom one, which is centred under it (400 and 320
   pixels wide, 240 high), the pair scaled as one to fit the swapchain's image and centred in it, the rest black; in
   separate windows this screen alone, scaled to fit and centred. */
/* The pair of screens in one image of the window's size: the top above the bottom, or this screen alone. The image
   is left in TRANSFER_DST_OPTIMAL. */
static int build_mix(VkCommandBuffer cb, unsigned screen) {
    const SnxCtrVk* v = snx_ctr_vk();
    unsigned window = g_present.separate ? screen : 0;
    VkExtent2D extent = g_present.swap[window].extent;
    if (!mix_image(window, extent.width, extent.height)) return 0;
    VkImage dst = g_present.mix[window];
    image_barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    image_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    g_present.mix_layout[window] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    for (unsigned i = 0; i < 2; ++i) {
        if (g_present.separate && i != screen) continue;
        Frame* f = &g_present.frames[i];
        VkOffset2D rect[2];
        VkImage src_image;
        VkImageView src_view;
        uint32_t src_width, src_height;
        if (!f->image || f->layout == VK_IMAGE_LAYOUT_UNDEFINED || !screen_rect(i, rect)) continue;
        uint32_t tw = (uint32_t)(rect[1].x - rect[0].x), th = (uint32_t)(rect[1].y - rect[0].y);
        src_image = f->image, src_view = f->view, src_width = f->width, src_height = f->height;
        if (g_present.fx[i] && tw && th && f->width && f->height) {
            int filter, aa;
            image_barrier(cb, f->image, f->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            f->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            filter = snx_fx_pick_filter(g_present.fx[i], f->width, f->height, tw, th, 0);
            aa = snx_fx_pick_aa(g_present.fx[i], snx_fx_aa_parse(g_settings.anti_aliasing), f->width, f->height, tw, th,
                                v->properties.limits.maxImageDimension2D);
            if (snx_fx_render(g_present.fx[i], cb, f->view, f->width, f->height, 0, 0, f->width, f->height, 0, tw, th,
                              filter, aa, &f->fx)) {
                src_image = f->fx.image;
                src_view = f->fx.view;
                src_width = f->fx.width;
                src_height = f->fx.height;
            } else {
                image_barrier(cb, f->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                f->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            }
        } else {
            image_barrier(cb, f->image, f->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            f->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        {
            VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .srcOffsets = {{0, 0, 0}, {(int32_t)src_width, (int32_t)src_height, 1}},
                                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .dstOffsets = {{rect[0].x, rect[0].y, 0}, {rect[1].x, rect[1].y, 1}}};
            vkCmdBlitImage(cb, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        }
    }
    return 1;
}

/* the buttons the player holds, in the overlay's words, with the circle pad where it is off its center */
static const SnxStatsButton pad_buttons[] = {
    {SNX_CTR_PAD_A, "A"},  {SNX_CTR_PAD_B, "B"},    {SNX_CTR_PAD_X, "X"},         {SNX_CTR_PAD_Y, "Y"},
    {SNX_CTR_PAD_L, "L"},  {SNX_CTR_PAD_R, "R"},    {SNX_CTR_PAD_START, "Start"}, {SNX_CTR_PAD_SELECT, "Select"},
    {SNX_CTR_PAD_UP, "^"}, {SNX_CTR_PAD_DOWN, "v"}, {SNX_CTR_PAD_LEFT, "<"},      {SNX_CTR_PAD_RIGHT, ">"}};
static void stats_inputs(char* out, size_t size) {
    SnxCtrInput in;
    size_t at;
    snx_ctr_input_read(&in);
    snx_stats_inputs(out, size, in.buttons, pad_buttons, (unsigned)(sizeof pad_buttons / sizeof *pad_buttons));
    at = strlen(out);
    if ((in.circle[0] > 0.15f || in.circle[0] < -0.15f || in.circle[1] > 0.15f || in.circle[1] < -0.15f) &&
        at + 24 < size) {
        if (!strcmp(out, "-")) out[at = 0] = 0;
        snprintf(out + at, size - at, "%spad %+.1f,%+.1f", at ? "  " : "", (double)in.circle[0], (double)in.circle[1]);
    }
}
/* the frame rate and the held buttons, at the window's bottom left */
static void stats_overlay(VkCommandBuffer cb, VkImageView view, VkFormat format, float white, VkExtent2D extent) {
    char inputs[96];
    if (!g_present.overlay || !snx_stats_visible(&g_present.stats)) return;
    if (g_present.stats.showcase) stats_inputs(inputs, sizeof inputs);
    else inputs[0] = 0;
    snx_stats_draw(g_present.overlay, &g_present.stats, cb, view, format, extent.width, extent.height, white,
                   snx_ctr_vk_slot(), SDL_GetTicksNS(), inputs);
}

/* A composited pair (or a blend) into the window's image: the HDR passes, or the plain blit. The target comes back in
   PRESENT_SRC_KHR. */
static void draw_mix(VkCommandBuffer cb, VkImage mix, VkImageView mix_view, VkImage target, VkImageView target_view,
                     unsigned window) {
    VkExtent2D extent = g_present.swap[window].extent;
    int hdr = g_present.fx[window] && g_present.display_kind[window] != SNX_FX_DISPLAY_NONE &&
              snx_fx_hdr_active(g_present.fx[window]);
    if (hdr) {
        SnxFxImage comp = {0};
        image_barrier(cb, mix, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (snx_fx_hdr_compose(g_present.fx[window], cb, mix_view, VK_FORMAT_R8G8B8A8_UNORM, extent.width,
                               extent.height, 0, 0, extent.width, extent.height, 0, extent.width, extent.height, 0, 0,
                               extent.width, extent.height, &comp)) {
            if (g_settings.hdr_test_pattern) snx_fx_hdr_test(g_present.fx[window], cb, &comp);
            if (g_present.toast)
                snx_toast_draw(g_present.toast, cb, comp.view, snx_fx_compose_format(), extent.width, extent.height,
                               (float)g_present.paper, snx_ctr_vk_slot(), SDL_GetTicksNS());
            stats_overlay(cb, comp.view, snx_fx_compose_format(), (float)g_present.paper, extent);
            if (snx_fx_hdr_encode(g_present.fx[window], cb, &comp, target, target_view,
                                  g_present.swap[window].format.format, extent.width, extent.height,
                                  snx_fx_encode_kind(g_present.fx[window]))) {
                image_barrier(cb, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                g_present.mix_layout[window] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                return;
            }
        }
        image_barrier(cb, mix, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    } else {
        image_barrier(cb, mix, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    g_present.mix_layout[window] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    image_barrier(cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearColorImage(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit full = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {(int32_t)extent.width, (int32_t)extent.height, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{0, 0, 0}, {(int32_t)extent.width, (int32_t)extent.height, 1}}};
    vkCmdBlitImage(cb, mix, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &full, VK_FILTER_NEAREST);
    image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (g_present.toast)
        snx_toast_draw(g_present.toast, cb, target_view, g_present.swap[window].format.format, extent.width,
                       extent.height, 1.0f, snx_ctr_vk_slot(), SDL_GetTicksNS());
    stats_overlay(cb, target_view, g_present.swap[window].format.format, 1.0f, extent);
    image_barrier(cb, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

/* The shown picture without interpolation. */
static void show_screens(VkCommandBuffer cb, VkImage target, VkImageView target_view, unsigned screen) {
    unsigned window = g_present.separate ? screen : 0;
    if (!build_mix(cb, screen)) return;
    draw_mix(cb, g_present.mix[window], g_present.mix_view[window], target, target_view, window);
    g_present.have_mix[window] = 1; /* the pair is there for the next frame's blend */
}

/* The blend of the previous pair and this one, then this pair, into two window images. */
static void show_screens_interp(VkCommandBuffer cb, VkImage target, VkImageView target_view, VkImage target2,
                                VkImageView target_view2, unsigned screen) {
    unsigned window = g_present.separate ? screen : 0;
    VkExtent2D extent = g_present.swap[window].extent;
    SnxFxImage prev = {0};
    int have_prev;
    if (g_present.have_mix[window] && g_present.fx[window]) {
        image_barrier(cb, g_present.mix[window], g_present.mix_layout[window],
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        g_present.mix_layout[window] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    have_prev = g_present.have_mix[window] && g_present.fx[window] &&
                snx_fx_keep(g_present.fx[window], cb, g_present.mix_view[window], extent.width, extent.height, &prev);
    if (have_prev)
        image_barrier(cb, prev.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!build_mix(cb, screen)) return;
    {
        int blended = 0;
        if (have_prev) {
            SnxFxImage blend = {0};
            /* the blend samples the new pair, and draw_mix expects a mix in TRANSFER_DST after it */
            image_barrier(cb, g_present.mix[window], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (snx_fx_blend(g_present.fx[window], cb, prev.view, g_present.mix_view[window], extent.width,
                             extent.height, 0.5f, &blend)) {
                image_barrier(cb, blend.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                draw_mix(cb, blend.image, blend.view, target, target_view, window);
                blended = 1;
            }
            image_barrier(cb, g_present.mix[window], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            g_present.mix_layout[window] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }
        if (!blended) {
            /* the first frame, or a blend that could not run: the pair itself in the first image too, so both the
               images the frame presents are drawn */
            draw_mix(cb, g_present.mix[window], g_present.mix_view[window], target, target_view, window);
            if (!build_mix(cb, screen)) return;
        }
    }
    draw_mix(cb, g_present.mix[window], g_present.mix_view[window], target2, target_view2, window);
    g_present.have_mix[window] = 1;
}

static int ctr_display_hz(SDL_Window* window) {
    SDL_DisplayID id = window ? SDL_GetDisplayForWindow(window) : 0;
    const SDL_DisplayMode* mode = id ? SDL_GetCurrentDisplayMode(id) : NULL;
    if (!mode || !(mode->refresh_rate > 0)) return 0;
    return (int)(mode->refresh_rate + 0.5f);
}

bool snx_ctr_present_frame(unsigned screen, uint32_t pa, unsigned width, unsigned height, unsigned format,
                           unsigned pitch, uint64_t* hash) {
    if (!g_present.up || screen > 1 || !width || !height || width > MAX_SIDE || height > MAX_SIDE || format > 4)
        return false;
    uint32_t bytes = snx_ctr_pixel_bytes(format);
    SnxCtrVkWindow w;
    uint32_t inside;
    if (pitch < width * bytes || !snx_ctr_vk_window(pa, (uint64_t)pitch * (height - 1) + width * bytes, &w, &inside))
        return false;
    const SnxCtrVk* v = snx_ctr_vk();
    Frame* f = &g_present.frames[screen];
    {
        unsigned window = g_present.separate ? screen : 0;
        /* Auto follows the display's headroom, which SDL reports only once the window is on screen: the first frame
           that sees it takes the HDR swapchain, and no more than once where the surface offers none */
        if (g_present.fx[window] && snx_fx_hdr_mode(g_present.fx[window]) == SNX_FX_HDR_AUTO &&
            !g_present.auto_tried[window] && g_present.display_kind[window] == SNX_FX_DISPLAY_NONE &&
            snx_vk_window_headroom_pct(screen_window(window)) > 100) {
            g_present.auto_tried[window] = 1;
            swap_create(window);
        }
        if (g_present.swap_views_for[window] && g_present.fx[window] &&
            snx_fx_hdr_active(g_present.fx[window]) != (g_present.display_kind[window] != SNX_FX_DISPLAY_NONE))
            swap_create(window);
    }
    /* A render target the GPU has not stored is the frame at its render scale: the shader reads it instead of the
       framebuffer, whose pixels it has not written yet */
    VkImageView source = g_present.source_dummy.view;
    unsigned scale = 1, flip = 1;
    static const unsigned target_format[5] = {0, 1, 3, 2, 4}; /* GSP's colour formats to transfer.h's */
    bool from_target = pitch == width * bytes &&
                       snx_ctr_vk_target_image(pa, width, height, target_format[format], &source, &scale, &flip);
    unsigned frame_width = height * scale, frame_height = width * scale;
    if (f->width != frame_width || f->height != frame_height) {
        snx_ctr_vk_finish();
        frame_free(f);
        if (!frame_make(f, frame_width, frame_height, scale)) return false;
    }
    bool dump = dump_due(), readback = dump || hash;
    if (!from_target) snx_ctr_vk_sync(pa, pitch * (height - 1) + width * bytes, false); /* the draws over it stored */
    VkCommandBuffer cb = snx_ctr_vk_commands();
    unsigned slot = snx_ctr_vk_slot(); /* the open recording's, whose semaphores it takes */
    snx_ctr_vk_compute_barrier(cb);    /* after what the GPU wrote to the framebuffer */
    image_barrier(cb, f->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL); /* the shader writes all of it */
    f->layout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet set = snx_ctr_vk_set(g_present.decode.set);
    VkDescriptorBufferInfo in = {w.buffer, w.offset, w.range}, out = {g_present.readback, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo image = {VK_NULL_HANDLE, f->view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo from = {VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[4] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 0,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &in},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 1,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &out},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 2,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &image},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 3,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &from}};
    vkUpdateDescriptorSets(v->device, 4, writes, 0, NULL);
    uint32_t push[9] = {inside, format, pitch, width, height, readback, from_target, flip, scale};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_present.decode.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_present.decode.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, g_present.decode.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    unsigned columns = from_target ? width * scale : width, rows = from_target ? height * scale : height;
    vkCmdDispatch(cb, (columns + 7) / 8, (rows + 7) / 8, 1);
    /* both screens in one window, at each top screen frame: a game draws both every frame, and showing at each
       screen's would wait for two vblanks a frame. A bottom screen that changes while the top one stays still shows
       at its second frame. In separate windows each screen shows at its own */
    unsigned window_index = g_present.separate ? screen : 0;
    uint32_t index = 0;
    g_present.bottom_alone = screen ? g_present.bottom_alone + 1 : 0;
    /* a window without a swapchain gets one again when its surface has a size: on Windows a minimized window's
       surface is 0x0, and no swapchain can be made at that size */
    bool show = g_present.swap[window_index].swapchain || swap_create(window_index);
    if (!g_present.separate && screen == 1 && g_present.bottom_alone < 2) show = false;
    if (show) {
        VkResult acquired = vkAcquireNextImageKHR(v->device, g_present.swap[window_index].swapchain, UINT64_MAX,
                                                  g_present.acquire[window_index][slot], VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            (void)swap_create(window_index);
            show = false;
        } else if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            show = false;
    }
    int interp = show && g_present.fx[window_index] &&
                 (snx_fx_interp_parse(g_settings.frame_interpolation) == SNX_FX_INTERP_BLEND ||
                  (snx_fx_interp_parse(g_settings.frame_interpolation) == SNX_FX_INTERP_AUTO &&
                   snx_interp_auto_frame(&g_present.interp_auto, SDL_GetTicksNS(),
                                         ctr_display_hz(screen_window(window_index)))));
    uint32_t index2 = 0;
    int show2 = 0;
    /* two images only where a pair is already kept: the first frame shows one and keeps its pair for the next */
    if (interp && g_present.have_mix[window_index]) {
        VkFence fence = g_present.acquire_fence[window_index];
        VkResult r2;
        vkResetFences(v->device, 1, &fence);
        r2 = vkAcquireNextImageKHR(v->device, g_present.swap[window_index].swapchain, UINT64_MAX, VK_NULL_HANDLE, fence,
                                   &index2);
        if (r2 == VK_SUCCESS || r2 == VK_SUBOPTIMAL_KHR) {
            vkWaitForFences(v->device, 1, &fence, VK_TRUE, UINT64_MAX);
            show2 = 1;
        }
    }
    if (show) {
        g_present.bottom_alone = 0;
        if (show2)
            show_screens_interp(cb, g_present.swap[window_index].images[index],
                                g_present.swap_views[window_index][index], g_present.swap[window_index].images[index2],
                                g_present.swap_views[window_index][index2], screen);
        else
            show_screens(cb, g_present.swap[window_index].images[index], g_present.swap_views[window_index][index],
                         screen);
    }
    /* submitted, waited for only when the host reads the frame (a dump, a test's hash) */
    bool ok = true;
    if (show) {
        uint64_t serial =
            snx_ctr_vk_submit_with(g_present.acquire[window_index][slot], g_present.presented[window_index][slot]);
        ok = (!readback && !show2) || snx_ctr_vk_wait(serial, UINT64_MAX);
        if (show2 && ok) {
            VkPresentInfoKHR p[2] = {{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                      .swapchainCount = 1,
                                      .pSwapchains = &g_present.swap[window_index].swapchain,
                                      .pImageIndices = &index},
                                     {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                      .swapchainCount = 1,
                                      .pSwapchains = &g_present.swap[window_index].swapchain,
                                      .pImageIndices = &index2}};
            VkResult shown = vkQueuePresentKHR(v->queue, &p[0]);
            if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR)
                g_present.windowed[window_index] = swap_create(window_index);
            if (shown == VK_SUCCESS || shown == VK_SUBOPTIMAL_KHR) {
                shown = vkQueuePresentKHR(v->queue, &p[1]);
                if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR)
                    g_present.windowed[window_index] = swap_create(window_index);
            }
            index = index2;
            goto presented;
        }
    } else if (readback)
        ok = snx_ctr_vk_finish();
    if (show && ok) {
        VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                    .waitSemaphoreCount = 1,
                                    .pWaitSemaphores = &g_present.presented[window_index][slot],
                                    .swapchainCount = 1,
                                    .pSwapchains = &g_present.swap[window_index].swapchain,
                                    .pImageIndices = &index};
        VkResult shown = vkQueuePresentKHR(v->queue, &present);
        if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR) (void)swap_create(window_index);
    }
presented:;
    if (show && !screen) { /* the top screen's frames are the game's */
        snx_stats_frame(&g_present.stats, SDL_GetTicksNS());
        if (show2) snx_stats_generated(&g_present.stats);
    }
    pump();
    if (!ok) return false;
    if (readback) {
        const unsigned char* rgba = g_present.readback_view;
        if (hash) {
            /* not SNX_FNV_BASIS (a digit short of it), kept so the frame hashes stay what they were */
            *hash = snx_fnv1a(1469598103934665603ull, rgba, (size_t)width * height * 4);
        }
        if (dump) write_ppm(screen, width, height, rgba);
    }
    return true;
}
void snx_ctr_present_free(void) {
    if (!g_present.up) return;
    const SnxCtrVk* v = snx_ctr_vk();
    vkDeviceWaitIdle(v->device);
    if (g_present.toast) snx_toast_destroy(g_present.toast);
    g_present.toast = NULL;
    if (g_present.overlay) snx_text_destroy(g_present.overlay);
    g_present.overlay = NULL;
    for (unsigned screen = 0; screen < 2; ++screen) {
        swap_views_free(screen);
        if (g_present.swap[screen].swapchain) swap_destroy(screen);
        if (g_present.fx[screen]) snx_fx_destroy(g_present.fx[screen]);
        g_present.fx[screen] = NULL;
        if (g_present.mix_view[screen]) vkDestroyImageView(v->device, g_present.mix_view[screen], NULL);
        if (g_present.mix[screen]) vkDestroyImage(v->device, g_present.mix[screen], NULL);
        if (g_present.mix_memory[screen]) vkFreeMemory(v->device, g_present.mix_memory[screen], NULL);
    }
    for (unsigned i = 0; i < 2; ++i) frame_free(&g_present.frames[i]);
    frame_free(&g_present.source_dummy);
    for (unsigned screen = 0; screen < 2; ++screen)
        for (unsigned i = 0; i < SNX_CTR_VK_SLOTS; ++i) {
            if (g_present.acquire[screen][i]) vkDestroySemaphore(v->device, g_present.acquire[screen][i], NULL);
            if (g_present.presented[screen][i]) vkDestroySemaphore(v->device, g_present.presented[screen][i], NULL);
        }
    for (unsigned i = 0; i < 2; ++i)
        if (g_present.acquire_fence[i]) vkDestroyFence(v->device, g_present.acquire_fence[i], NULL);
    if (g_present.readback) vkDestroyBuffer(v->device, g_present.readback, NULL);
    if (g_present.readback_memory) vkFreeMemory(v->device, g_present.readback_memory, NULL);
    snx_ctr_vk_compute_free(&g_present.decode);
    snx_ctr_vk_transfer_free();
    snx_ctr_vk_y2r_free();
    memset(&g_present, 0, sizeof g_present);
    snx_ctr_vk_free();
}
