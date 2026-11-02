/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The shown framebuffers, on the 3DS's Vulkan device (vk/device.c): a compute shader (vk/shaders/present.comp)
   reads each one where the guest has it in RAM, decodes it and turns it to the console's landscape screen in an
   image, and the window's swapchain shows both screens (gpu/screens.c, by default the top one above the bottom one
   as on the console), each keeping its shape (frame_layout). Nothing of the frame passes through the CPU. The frame
   dumps (the first eight, then each screen's latest about once a second, in frame_dir) and the tests' hashes are
   the one readback: the shader writes those frames' RGBA pixels to a host-visible buffer as well. The 3DS framebuffer
   is 240x400/320 portrait, one 240-pixel row per screen column from the left, each row from the bottom of the
   screen up (libctru's console draws pixel (x, y) at x * 240 + 239 - y); the window and the dumps show it that way
   round. */
#include "3ds/gpu/present.h"
#include "3ds/gpu/screens.h"
#include "3ds/input/input.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/shaders/fxaa_spv.h"
#include "3ds/gpu/vk/shaders/present_spv.h"
#include "3ds/gpu/vk/vk_transfer.h"
#include "3ds/gpu/vk/vk_y2r.h"
#include "3ds/gpu/vk/vk_render.h"
#include "common/utilities/bits.h"
#include "common/gpu/vk/swapchain.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/window_icon.h"
#include "snx_settings.h"
#include <math.h>
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
    uint32_t hires_pa;      /* the framebuffer it was decoded from, and its size, to look for it at the resolution scale */
    unsigned hires_width, hires_height;
} Frame;
typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char* view;
    VkDeviceSize size;
} DumpCopy;
static struct {
    SnxCtrVkCompute decode, fxaa;
    Frame frames[2];
    Frame post[2]; /* the window's picture before the filter and after it, at the swapchain's size */
    bool post_ready; /* this frame is filtered: post is at the swapchain's size */
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
    int touching; /* the left mouse button went down on the bottom screen and is still down */
    /* a frame dump's copies of a picture made at the resolution scale (0), and of the window's filtered one (1), each
       grown to the picture */
    DumpCopy copies[2];
} g_present;

static void swap_destroy(void) { snx_vk_swapchain_free(snx_ctr_vk()->device, &g_present.swap); }
/* the window's swapchain, made anew for its size */
/* a locked workstation or a remote session can leave the window with no image to acquire for a long time: a frame
   waits this long for one, then goes unshown, in place of stopping the emulation thread */
enum { ACQUIRE_NS = 100000000 };
static bool swap_create(void) {
    const SnxCtrVk* v = snx_ctr_vk();
    static const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    SnxVkSwapchainInfo info = {.surface = v->host.surface,
                               .formats = formats,
                               .format_count = sizeof formats / sizeof *formats,
                               .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               .window = snx_vk_window_extent(&v->host, (VkExtent2D){600, 720}),
                               .unsynced = !g_settings.ctr_vsync};
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
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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
    if (!snx_ctr_vk_compute(snx_ctr_fxaa_spv, snx_ctr_fxaa_spv_size / 4, 0, 2, 8, &g_present.fxaa) ||
        !snx_ctr_vk_compute(snx_ctr_present_spv, snx_ctr_present_spv_size / 4, 2, 1, 24, &g_present.decode) ||
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
    if (v->host.window && g_settings.ctr_fullscreen) (void)SDL_SetWindowFullscreen(v->host.window, true);
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
/* The window's picture as show_screens draws it. A screen not made yet has the console's size */
static SnxCtrWindowLayout window_layout(void) {
    const Frame* top = &g_present.frames[0];
    const Frame* bottom = &g_present.frames[1];
    return (SnxCtrWindowLayout){.image_w = g_present.swap.extent.width,
                                .image_h = g_present.swap.extent.height,
                                .top_w = top->width ? top->width : 400,
                                .top_h = top->height ? top->height : SNX_CTR_TOUCH_HEIGHT,
                                .bottom_w = bottom->width ? bottom->width : SNX_CTR_TOUCH_WIDTH,
                                .bottom_h = bottom->height ? bottom->height : SNX_CTR_TOUCH_HEIGHT,
                                .mode = (unsigned)g_settings.ctr_screen_layout,
                                .integer = g_settings.ctr_integer_scale != 0};
}
/* The left mouse button as the stylus: a press on the bottom screen touches it, the drag moves the touch (held inside
   the screen, as the stylus can't leave it) and the release lets go. A finger is the same (SDL sends it as the mouse
   SDL_TOUCH_MOUSEID). The touch_screen setting is a Switch game's: 0 nothing, 1 the fingers, 2 the fingers and the
   mouse */
static void touch_event(const SDL_Event* event) {
    SDL_Window* window = snx_ctr_vk()->host.window;
    /* only this window's events: another window's press, drag and release change nothing */
    if (event->type != SDL_EVENT_MOUSE_MOTION && event->type != SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event->type != SDL_EVENT_MOUSE_BUTTON_UP && event->type != SDL_EVENT_WINDOW_FOCUS_LOST)
        return;
    SDL_WindowID from = event->type == SDL_EVENT_MOUSE_MOTION ? event->motion.windowID
                        : event->type == SDL_EVENT_WINDOW_FOCUS_LOST ? event->window.windowID
                                                                      : event->button.windowID;
    if (window && from != SDL_GetWindowID(window)) return;
    bool pointer = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP ||
                   event->type == SDL_EVENT_MOUSE_MOTION;
    if (pointer) {
        SDL_MouseID source = event->type == SDL_EVENT_MOUSE_MOTION ? event->motion.which : event->button.which;
        if (g_settings.touch_screen != 2 && !(g_settings.touch_screen == 1 && source == SDL_TOUCH_MOUSEID)) return;
    }
    bool press = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT;
    bool release = (event->type == SDL_EVENT_MOUSE_BUTTON_UP && event->button.button == SDL_BUTTON_LEFT) ||
                   event->type == SDL_EVENT_WINDOW_FOCUS_LOST;
    bool drag = event->type == SDL_EVENT_MOUSE_MOTION && g_present.touching;
    if (release) {
        g_present.touching = 0;
        snx_ctr_input_touch(false, 0, 0);
    } else if ((press || drag) && window && g_present.frames[1].width) { /* no bottom screen drawn yet: nothing to touch */
        /* the event's point is in the window's units, the swapchain image in pixels */
        float density = SDL_GetWindowPixelDensity(window);
        float x = press ? event->button.x : event->motion.x, y = press ? event->button.y : event->motion.y;
        SnxCtrWindowLayout layout = window_layout();
        unsigned px, py;
        bool inside = snx_ctr_touch_locate(&layout, x * density, y * density, &px, &py);
        if (press) g_present.touching = inside;
        if (g_present.touching) snx_ctr_input_touch(true, px, py);
    }
}
static void pump(void) {
    const SnxCtrVk* v = snx_ctr_vk();
    SDL_Window* window = v ? v->host.window : NULL;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g_present.closed = 1;
        /* F10 goes to the next screen layout */
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F10)
            g_settings.ctr_screen_layout = (g_settings.ctr_screen_layout + 1) % SNX_CTR_SCREENS_LAYOUTS;
        /* F11, or Alt and Enter, takes the window in and out of fullscreen */
        SDL_Window* window = snx_ctr_vk()->host.window;
        if (window && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            (event.key.scancode == SDL_SCANCODE_F11 ||
             (event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT))))
            (void)SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
        snx_ctr_input_event(&event); /* the gamepads connected and gone */
        touch_event(&event);
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
/* The window's picture: the two screens where ctr_screen_layout puts them (gpu/screens.c), the rest black, each
   stretched with ctr_scaling_filter */
/* The picture's two images at the swapchain's size for the filter, made anew when the size changed. False when they
   cannot be made: the picture goes unfiltered */
static bool post_images(void) {
    VkExtent2D e = g_present.swap.extent;
    for (unsigned i = 0; i < 2; ++i)
        if (g_present.post[i].width != e.width || g_present.post[i].height != e.height) {
            snx_ctr_vk_finish(); /* the old ones may be in the recordings in flight */
            for (unsigned k = 0; k < 2; ++k) frame_free(&g_present.post[k]);
            return frame_make(&g_present.post[0], e.width, e.height) && frame_make(&g_present.post[1], e.width, e.height);
        }
    return true;
}
/* the picture in post[0] filtered into post[1] and that blitted, whole, to the swapchain image */
static void filter_picture(VkCommandBuffer cb, VkImage target) {
    const SnxCtrVk* v = snx_ctr_vk();
    Frame *before = &g_present.post[0], *after = &g_present.post[1];
    image_barrier(cb, before->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    image_barrier(cb, after->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkDescriptorSet set = snx_ctr_vk_set(g_present.fxaa.set);
    VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, before->view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, after->view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2];
    for (uint32_t i = 0; i < 2; ++i)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                           .pImageInfo = &images[i]};
    vkUpdateDescriptorSets(v->device, 2, writes, 0, NULL);
    uint32_t size[2] = {before->width, before->height};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_present.fxaa.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_present.fxaa.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, g_present.fxaa.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof size, size);
    vkCmdDispatch(cb, (size[0] + 7) / 8, (size[1] + 7) / 8, 1);
    image_barrier(cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    image_barrier(cb, after->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {(int32_t)size[0], (int32_t)size[1], 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{0, 0, 0}, {(int32_t)size[0], (int32_t)size[1], 1}}};
    vkCmdBlitImage(cb, after->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);
    image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}
static void show_screens(VkCommandBuffer cb, VkImage target) {
    SnxCtrWindowLayout layout = window_layout();
    SnxCtrScreenRect rects[2];
    snx_ctr_screens_place(&layout, rects);
    VkFilter filter = g_settings.ctr_scaling_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    /* the screens go to the picture the filter reads, or straight to the swapchain's image */
    bool filtered = g_present.post_ready;
    VkImage canvas = filtered ? g_present.post[0].image : target;
    image_barrier(cb, canvas, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, canvas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    /* the blits below overwrite the clear's pixels: their writes must wait for it */
    image_barrier(cb, canvas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    snx_ctr_vk_barrier(cb); /* what the transfers made at the resolution scale */
    for (unsigned i = 0; i < 2; ++i) {
        Frame* f = &g_present.frames[i];
        /* a screen not drawn yet stays black, one the layout leaves out is not shown */
        if (!rects[i].w || !f->image || f->layout == VK_IMAGE_LAYOUT_UNDEFINED) continue;
        /* the picture a display transfer made at the resolution scale, while the framebuffer is what it wrote */
        VkImage source = f->image;
        VkImageLayout source_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkExtent2D source_extent = {f->width, f->height};
        if (!snx_ctr_vk_hires(f->hires_pa, f->hires_width, f->hires_height, &source, &source_extent))
            image_barrier(cb, f->image, f->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        else
            source_layout = VK_IMAGE_LAYOUT_GENERAL;
        f->layout = source == f->image ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : f->layout;
        VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .srcOffsets = {{0, 0, 0}, {(int32_t)source_extent.width, (int32_t)source_extent.height, 1}},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .dstOffsets = {{(int32_t)lround(rects[i].x), (int32_t)lround(rects[i].y), 0},
                                           {(int32_t)lround(rects[i].x + rects[i].w),
                                            (int32_t)lround(rects[i].y + rects[i].h), 1}}};
        vkCmdBlitImage(cb, source, source_layout, canvas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
    }
    if (filtered)
        filter_picture(cb, target);
    else
        image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}
/* a picture's RGBA pixels copied to the host's copy slot for the dump, recorded in cb: its size, 0 when it cannot be */
static VkExtent2D image_copy(VkCommandBuffer cb, unsigned slot, VkImage image, VkExtent2D extent) {
    const SnxCtrVk* v = snx_ctr_vk();
    DumpCopy* c = &g_present.copies[slot];
    VkDeviceSize size = (VkDeviceSize)extent.width * extent.height * 4;
    if (size > c->size) {
        vkDeviceWaitIdle(v->device);
        if (c->view) vkUnmapMemory(v->device, c->memory);
        if (c->buffer) vkDestroyBuffer(v->device, c->buffer, NULL);
        if (c->memory) vkFreeMemory(v->device, c->memory, NULL);
        *c = (DumpCopy){0};
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = size,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkMemoryRequirements req;
        void* mapped = NULL;
        if (vkCreateBuffer(v->device, &bi, NULL, &c->buffer) != VK_SUCCESS) return (VkExtent2D){0, 0};
        vkGetBufferMemoryRequirements(v->device, c->buffer, &req);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = req.size,
                                   .memoryTypeIndex = snx_ctr_vk_memory_type(
                                       req.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(v->device, &ai, NULL, &c->memory) != VK_SUCCESS ||
            vkBindBufferMemory(v->device, c->buffer, c->memory, 0) != VK_SUCCESS ||
            vkMapMemory(v->device, c->memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
            return (VkExtent2D){0, 0};
        c->view = mapped;
        c->size = size;
    }
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {extent.width, extent.height, 1}};
    snx_ctr_vk_barrier(cb);
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, c->buffer, 1, &copy);
    return extent;
}
/* the picture a display transfer made at the resolution scale for this screen, copied for the dump: its size, 0 for
   none (the window shows the frame as the guest has it) */
static VkExtent2D hires_copy(VkCommandBuffer cb, const Frame* f) {
    VkImage image;
    VkExtent2D extent;
    if (!snx_ctr_vk_hires(f->hires_pa, f->hires_width, f->hires_height, &image, &extent)) return (VkExtent2D){0, 0};
    return image_copy(cb, 0, image, extent);
}
/* a copy's pixels (landscape RGBA) as a PPM named name in the frame folder, written beside it and renamed over the one
   before */
static void write_copy(unsigned slot, const char* name, VkExtent2D extent) {
    char path[1024], staged[1040];
    snprintf(path, sizeof path, "%s/%s.ppm", g_present.frame_dir, name);
    snx_path_native(path);
    snprintf(staged, sizeof staged, "%s.partial", path);
    FILE* file = fopen(staged, "wb");
    if (!file) return;
    fprintf(file, "P6\n%u %u\n255\n", extent.width, extent.height);
    for (uint64_t i = 0; i < (uint64_t)extent.width * extent.height; ++i)
        if (fwrite(g_present.copies[slot].view + i * 4, 1, 3, file) != 3) break;
    if (!fclose(file)) (void)snx_replace(staged, path);
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
    f->hires_pa = pa; /* looked for again when the window shows it */
    f->hires_width = width;
    f->hires_height = height;
    bool dump = dump_due(), readback = dump || hash;
    snx_ctr_vk_sync(pa, pitch * (height - 1) + width * bytes, false); /* the draws over it stored first, on the GPU */
    g_present.post_ready = g_settings.ctr_anti_aliasing == 1 && g_present.windowed && post_images();
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
        VkResult acquired = vkAcquireNextImageKHR(v->device, g_present.swap.swapchain, ACQUIRE_NS,
                                                  g_present.acquire[slot],
                                                  VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR || acquired == VK_ERROR_SURFACE_LOST_KHR) {
            g_present.windowed = swap_create();
            show = false;
        } else if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            show = false;
    }
    if (show) {
        g_present.bottom_alone = 0;
        show_screens(cb, g_present.swap.images[index]);
    }
    VkExtent2D hires = dump ? hires_copy(cb, f) : (VkExtent2D){0, 0}, window = {0, 0};
    if (dump && show && g_present.post_ready && screen == 0)
        window = image_copy(cb, 1, g_present.post[1].image, (VkExtent2D){g_present.post[1].width, g_present.post[1].height});
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
        if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR || shown == VK_ERROR_SURFACE_LOST_KHR)
            g_present.windowed = swap_create();
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
        if (hires.width) {
            char name[32];
            snprintf(name, sizeof name, "frame-%u-hires", screen);
            write_copy(0, name, hires);
        }
        if (window.width) write_copy(1, "frame-window", window);
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
    for (unsigned i = 0; i < 2; ++i) {
        if (g_present.copies[i].view) vkUnmapMemory(v->device, g_present.copies[i].memory);
        if (g_present.copies[i].buffer) vkDestroyBuffer(v->device, g_present.copies[i].buffer, NULL);
        if (g_present.copies[i].memory) vkFreeMemory(v->device, g_present.copies[i].memory, NULL);
    }
    if (g_present.readback) vkDestroyBuffer(v->device, g_present.readback, NULL);
    if (g_present.readback_memory) vkFreeMemory(v->device, g_present.readback_memory, NULL);
    snx_ctr_vk_compute_free(&g_present.decode);
    snx_ctr_vk_compute_free(&g_present.fxaa);
    for (unsigned i = 0; i < 2; ++i) frame_free(&g_present.post[i]);
    snx_ctr_vk_transfer_free();
    snx_ctr_vk_y2r_free();
    memset(&g_present, 0, sizeof g_present);
    snx_ctr_vk_free();
}
