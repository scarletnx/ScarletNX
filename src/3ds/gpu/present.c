/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The shown framebuffers, on the 3DS's Vulkan device (vk/device.c): a compute shader (vk/shaders/present.comp)
   reads each one where the guest has it in RAM, decodes it and turns it to the console's landscape screen in an
   image, and the window's swapchain shows both screens, the top one above the bottom one as on the console, each
   keeping its shape (frame_layout). Nothing of the frame passes through the CPU. The frame
   dumps (the first eight, then each screen's latest about once a second, in frame_dir) and the tests' hashes are
   the one readback: the shader writes those frames' RGBA pixels to a host-visible buffer as well. The 3DS framebuffer
   is 240x400/320 portrait, one 240-pixel row per screen column from the left, each row from the bottom of the
   screen up (libctru's console draws pixel (x, y) at x * 240 + 239 - y); the window and the dumps show it that way
   round. */
#include "3ds/gpu/present.h"
#include "3ds/input/input.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/shaders/present_spv.h"
#include "3ds/gpu/vk/vk_transfer.h"
#include "3ds/gpu/vk/vk_y2r.h"
#include "3ds/gpu/vk/vk_render.h"
#include "common/utilities/bits.h"
#include "common/gpu/vk/swapchain.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/window_icon.h"
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
} Frame;
static struct {
    SnxCtrVkCompute decode;
    Frame frames[2];
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    unsigned char* readback_view;
    SnxVkSwapchain swap;
    /* each submission slot's: a slot is free again only once its submission is done, and so are they */
    VkSemaphore acquire[SNX_CTR_VK_SLOTS], presented[SNX_CTR_VK_SLOTS];
    const char* frame_dir;
    unsigned dumps;
    uint64_t presents; /* past the first dumps, two presents (one a screen) of every 120 are kept as the latest */
    unsigned bottom_alone; /* bottom screen frames since the window last showed, the top screen showing none */
    int up, windowed, closed;
} g_present;

static void swap_destroy(void) { snx_vk_swapchain_free(snx_ctr_vk()->device, &g_present.swap); }
/* the window's swapchain, made anew for its size */
static bool swap_create(void) {
    const SnxCtrVk* v = snx_ctr_vk();
    static const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    SnxVkSwapchainInfo info = {.surface = v->host.surface,
                               .formats = formats,
                               .format_count = sizeof formats / sizeof *formats,
                               .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               .window = snx_vk_window_extent(&v->host, (VkExtent2D){600, 720})};
    return snx_vk_swapchain_make(v->phys, v->device, &info, &g_present.swap);
}
static bool frame_make(Frame* f, unsigned width, unsigned height) {
    const SnxCtrVk* v = snx_ctr_vk();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {width, height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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
    for (unsigned i = 0; i < SNX_CTR_VK_SLOTS; ++i)
        if (vkCreateSemaphore(v->device, si, NULL, &g_present.acquire[i]) != VK_SUCCESS ||
            vkCreateSemaphore(v->device, si, NULL, &g_present.presented[i]) != VK_SUCCESS)
            return false;
    return true;
}
bool snx_ctr_present_init(SnxCtrMemory* memory, const char* frame_dir) {
    if (g_present.up) return true;
    char why[256];
    if (!snx_ctr_vk_init(memory, why, sizeof why)) {
        fprintf(stderr, "3ds gpu: %s\n", why);
        return false;
    }
    const SnxCtrVk* v = snx_ctr_vk();
    g_present.frame_dir = frame_dir;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = (VkDeviceSize)MAX_SIDE * MAX_SIDE * 4,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!snx_ctr_vk_compute(snx_ctr_present_spv, snx_ctr_present_spv_size / 4, 2, 1, 24, &g_present.decode) ||
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
    if (v->host.surface) g_present.windowed = swap_create();
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
    SDL_SetWindowTitle(v->host.window, header);
    snx_window_icon(v->host.window);
}
bool snx_ctr_present_ready(void) { return g_present.up; }
bool snx_ctr_present_windowed(void) { return g_present.windowed; }

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
/* The blit rectangle of a screen in swapchain pixels: the pair scaled to fit and centred, each screen centred across
   the layout. False before a layout and a swapchain exist. */
static bool screen_rect(unsigned screen, VkOffset2D rect[2]) {
    const Frame* frames = g_present.frames;
    uint32_t layout_w = snx_max_u32(frames[0].width, frames[1].width);
    uint32_t layout_h = frames[0].height + frames[1].height;
    VkExtent2D e = g_present.swap.extent;
    if (!layout_w || !layout_h || !e.width || !e.height) return false;
    double scale = (double)e.width / layout_w < (double)e.height / layout_h ? (double)e.width / layout_w
                                                                            : (double)e.height / layout_h;
    const Frame* f = &frames[screen];
    double x = (e.width - layout_w * scale) / 2 + (layout_w - f->width) * scale / 2;
    double y = (e.height - layout_h * scale) / 2 + (screen ? frames[0].height * scale : 0);
    rect[0] = (VkOffset2D){(int32_t)x, (int32_t)y};
    rect[1] = (VkOffset2D){(int32_t)(x + f->width * scale), (int32_t)(y + f->height * scale)};
    return true;
}
/* The touch screen's pixel under a pointer at x and y, in the window's points as SDL reports them. The blit's own scale
   maps them, so a swapchain whose pixels differ from the window's points still lands on what is shown. As the Switch
   runtime's finger handling does, a press off the screen starts nothing and a drag off it keeps to its edge. */
static void pointer_touch(SDL_Window* window, float x, float y, bool press) {
    const Frame* bottom = &g_present.frames[1];
    VkExtent2D e = g_present.swap.extent;
    int width = 0, height = 0;
    VkOffset2D rect[2];
    if (!SDL_GetWindowSize(window, &width, &height) || width <= 0 || height <= 0 || !screen_rect(1, rect)) return;
    /* the division below is by the rect's size, so a rect with no pixels has nothing to map */
    if (rect[1].x <= rect[0].x || rect[1].y <= rect[0].y) return;
    double px = (double)x * e.width / width;
    double py = (double)y * e.height / height;
    if (press && (px < rect[0].x || px >= rect[1].x || py < rect[0].y || py >= rect[1].y)) return;
    px = snx_clamp_double(px, rect[0].x, rect[1].x);
    py = snx_clamp_double(py, rect[0].y, rect[1].y);
    uint32_t tx = (uint32_t)((px - rect[0].x) * bottom->width / (rect[1].x - rect[0].x));
    uint32_t ty = (uint32_t)((py - rect[0].y) * bottom->height / (rect[1].y - rect[0].y));
    snx_ctr_input_touch(true, snx_min_u32(tx, bottom->width - 1), snx_min_u32(ty, bottom->height - 1));
}
static void pointer_event(SDL_Window* window, const SDL_Event* event) {
    SDL_WindowID id = SDL_GetWindowID(window);
    switch (event->type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event->button.windowID == id && event->button.button == SDL_BUTTON_LEFT)
                pointer_touch(window, event->button.x, event->button.y, true);
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP: /* a release off the screen still ends the press */
            if (event->button.windowID == id && event->button.button == SDL_BUTTON_LEFT)
                snx_ctr_input_touch(false, 0, 0);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (event->motion.windowID == id && snx_ctr_input_touched())
                pointer_touch(window, event->motion.x, event->motion.y, false);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            /* a focus loss ends the press: this window may not get the release */
            if (event->window.windowID == id) snx_ctr_input_touch(false, 0, 0);
            break;
    }
}
static void pump(void) {
    const SnxCtrVk* v = snx_ctr_vk();
    SDL_Window* window = v ? v->host.window : NULL;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g_present.closed = 1;
        if (window) pointer_event(window, &event);
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
/* The window's picture: the top screen above the bottom one, which is centred under it (400 and 320 pixels wide, 240
   high), the pair scaled as one to fit the swapchain's image and centred in it, the rest black */
static void show_screens(VkCommandBuffer cb, VkImage target) {
    image_barrier(cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    /* the blits below overwrite the clear's pixels: their writes must wait for it */
    image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (unsigned i = 0; i < 2; ++i) {
        Frame* f = &g_present.frames[i];
        VkOffset2D rect[2];
        /* a screen not drawn yet stays black */
        if (!f->image || f->layout == VK_IMAGE_LAYOUT_UNDEFINED || !screen_rect(i, rect)) continue;
        image_barrier(cb, f->image, f->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        f->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .srcOffsets = {{0, 0, 0}, {(int32_t)f->width, (int32_t)f->height, 1}},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .dstOffsets = {{rect[0].x, rect[0].y, 0}, {rect[1].x, rect[1].y, 1}}};
        vkCmdBlitImage(cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_NEAREST);
    }
    image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
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
    if (f->width != height || f->height != width) {
        snx_ctr_vk_finish();
        frame_free(f);
        if (!frame_make(f, height, width)) return false;
    }
    bool dump = dump_due(), readback = dump || hash;
    snx_ctr_vk_sync(pa, pitch * (height - 1) + width * bytes, false); /* the draws over it stored first, on the GPU */
    VkCommandBuffer cb = snx_ctr_vk_commands();
    unsigned slot = snx_ctr_vk_slot(); /* the open recording's, whose semaphores it takes */
    snx_ctr_vk_compute_barrier(cb);    /* after what the GPU wrote to the framebuffer */
    image_barrier(cb, f->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL); /* the shader writes all of it */
    f->layout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet set = snx_ctr_vk_set(g_present.decode.set);
    VkDescriptorBufferInfo in = {w.buffer, w.offset, w.range}, out = {g_present.readback, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo image = {VK_NULL_HANDLE, f->view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[3] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
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
                                       .pImageInfo = &image}};
    vkUpdateDescriptorSets(v->device, 3, writes, 0, NULL);
    uint32_t push[6] = {inside, format, pitch, width, height, readback};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_present.decode.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_present.decode.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, g_present.decode.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    vkCmdDispatch(cb, (width + 7) / 8, (height + 7) / 8, 1);
    /* both screens in the window, at each top screen frame: a game draws both every frame, and showing at each
       screen's would wait for two vblanks a frame. A bottom screen that changes while the top one stays still shows
       at its second frame */
    uint32_t index = 0;
    g_present.bottom_alone = screen ? g_present.bottom_alone + 1 : 0;
    bool show = (screen == 0 || g_present.bottom_alone >= 2) && g_present.windowed;
    if (show) {
        VkResult acquired = vkAcquireNextImageKHR(v->device, g_present.swap.swapchain, UINT64_MAX,
                                                  g_present.acquire[slot],
                                                  VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            g_present.windowed = swap_create();
            show = false;
        } else if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            show = false;
    }
    if (show) {
        g_present.bottom_alone = 0;
        show_screens(cb, g_present.swap.images[index]);
    }
    /* submitted, waited for only when the host reads the frame (a dump, a test's hash) */
    bool ok = true;
    if (show) {
        uint64_t serial = snx_ctr_vk_submit_with(g_present.acquire[slot], g_present.presented[slot]);
        ok = !readback || snx_ctr_vk_wait(serial, UINT64_MAX);
    } else if (readback)
        ok = snx_ctr_vk_finish();
    if (show && ok) {
        VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                    .waitSemaphoreCount = 1,
                                    .pWaitSemaphores = &g_present.presented[slot],
                                    .swapchainCount = 1,
                                    .pSwapchains = &g_present.swap.swapchain,
                                    .pImageIndices = &index};
        VkResult shown = vkQueuePresentKHR(v->queue, &present);
        if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR) g_present.windowed = swap_create();
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
    swap_destroy();
    for (unsigned i = 0; i < 2; ++i) frame_free(&g_present.frames[i]);
    for (unsigned i = 0; i < SNX_CTR_VK_SLOTS; ++i) {
        if (g_present.acquire[i]) vkDestroySemaphore(v->device, g_present.acquire[i], NULL);
        if (g_present.presented[i]) vkDestroySemaphore(v->device, g_present.presented[i], NULL);
    }
    if (g_present.readback) vkDestroyBuffer(v->device, g_present.readback, NULL);
    if (g_present.readback_memory) vkFreeMemory(v->device, g_present.readback_memory, NULL);
    snx_ctr_vk_compute_free(&g_present.decode);
    snx_ctr_vk_transfer_free();
    snx_ctr_vk_y2r_free();
    memset(&g_present, 0, sizeof g_present);
    snx_ctr_vk_free();
}
