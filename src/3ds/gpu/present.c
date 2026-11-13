/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The shown framebuffers, on the 3DS's Vulkan device (vk/device.c): a compute shader (vk/shaders/present.comp)
   reads each one where the guest has it in RAM, decodes it and turns it to the console's landscape screen in an
   image, and the window's swapchain shows both screens, the top one above the bottom one as on the console, each
   keeping its shape (frame_layout). With separate screens each one has its own window and fills it. Nothing of the
   frame passes through the CPU. The frame dumps (the first eight, then each screen's latest about once a second, in
   frame_dir) and the tests' hashes are the one readback: the shader writes those frames' RGBA pixels to a
   host-visible buffer as well, and a latest dump is written once the GPU is done, without a wait for it. The 3DS framebuffer is 240x400/320 portrait, one 240-pixel row per screen column from
   the left, each row from the bottom of the screen up (libctru's console draws pixel (x, y) at x * 240 + 239 - y);
   the window and the dumps show it that way round. A 3D render scaled by its screen's resolution setting is drawn
   from its render target while the GPU keeps that newer than memory. */
#include "3ds/gpu/present.h"
#include "3ds/board/clock.h"
#include "3ds/input/input.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/shaders/present_spv.h"
#include "3ds/gpu/vk/vk_transfer.h"
#include "3ds/gpu/vk/vk_y2r.h"
#include "3ds/gpu/vk/vk_render.h"
#include "common/utilities/bits.h"
#include "common/gpu/vk/present_fx.h"
#include "common/gpu/vk/swapchain.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/quit_key.h"
#include "common/platform/file.h"
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
} Frame;
static struct {
    SnxCtrVkCompute decode;
    Frame frames[2];
    Frame source_dummy; /* bound where a frame is decoded from the guest, which the shader does not read */
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    unsigned char* readback_view;
    SnxVkSwapchain swap[2]; /* the combined window's, or each screen's own */
    /* the upscaler and the anti-aliasing (common/gpu/vk/present_fx.h) as the settings say, each screen's effects of
       its own: both screens are drawn in one recording, whose frame slot each keeps its images by. None while both
       are Off */
    SnxFxConfig fx_config;
    SnxFx* fx[2];
    /* each submission slot's: a slot is free again only once its submission is done, and so are they */
    VkSemaphore acquire[2][SNX_CTR_VK_SLOTS], presented[2][SNX_CTR_VK_SLOTS];
    const char* frame_dir;
    unsigned dumps;
    double latest_at[2];   /* past the first dumps, when each screen's latest was last kept (host seconds) */
    /* a latest dump the GPU has not finished: its recording's serial and its frame, written at a present once that is
       done, or when the presenter goes. Nothing else reads the readback back meanwhile, and no other dump is taken */
    struct {
        bool active;
        uint64_t serial;
        unsigned screen, width, height;
    } pending;
    unsigned bottom_alone; /* bottom screen frames since the window last showed, the top screen showing none */
    uint64_t decoded[2];   /* each screen's frames decoded */
    uint64_t unshown;      /* the shows that found every image of their window on the display */
    uint64_t quit_by;      /* a second Esc before this closes the window (common/utilities/quit_key.h) */
    SDL_FingerID finger;   /* the finger holding the touch screen, while one does (by_finger) */
    unsigned up : 1, closed : 1, separate : 1, by_finger : 1;
} g_present;

static void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout old, VkImageLayout next);

static void swap_destroy(unsigned screen) { snx_vk_swapchain_free(snx_ctr_vk()->device, &g_present.swap[screen]); }
static SDL_Window* screen_window(unsigned screen) {
    const SnxCtrVk* v = snx_ctr_vk();
    return g_present.separate ? (screen ? v->second : v->host.window) : v->host.window;
}
static VkSurfaceKHR screen_surface(unsigned screen) {
    const SnxCtrVk* v = snx_ctr_vk();
    return g_present.separate ? (screen ? v->second_surface : v->host.surface) : v->host.surface;
}
/* a window's swapchain, made anew for its size */
static bool swap_create(unsigned screen) {
    const SnxCtrVk* v = snx_ctr_vk();
    SDL_Window* window = screen_window(screen);
    VkSurfaceKHR surface = screen_surface(screen);
    if (!window || !surface) return false;
    static const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    VkExtent2D fallback =
        g_present.separate ? (screen ? (VkExtent2D){640, 480} : (VkExtent2D){800, 480}) : (VkExtent2D){600, 720};
    SnxVkSwapchainInfo info = {.surface = surface,
                               .formats = formats,
                               .format_count = sizeof formats / sizeof *formats,
                               .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               .window = snx_vk_window_extent(&v->host, fallback),
                               .unsynced = !g_settings.vsync};
    if (window != v->host.window) { /* the second window's own size */
        int width = 0, height = 0;
        info.window = SDL_GetWindowSizeInPixels(window, &width, &height) && width > 0 && height > 0
                          ? (VkExtent2D){(uint32_t)width, (uint32_t)height}
                          : fallback;
    }
    return snx_vk_swapchain_make(v->phys, v->device, &info, &g_present.swap[screen]);
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
                                     VK_IMAGE_USAGE_SAMPLED_BIT, /* the upscaler's and anti-aliasing's source */
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
static void fx_log(const char* text) { fprintf(stderr, "3ds gpu: %s\n", text); }
/* the upscaler and the anti-aliasing the settings ask for, said once, and their effects made where either is on */
static void fx_init(const SnxCtrVk* v) {
    char said[512];
    SnxFxConfig* c = &g_present.fx_config;
    if (snx_fx_settings(c, g_settings.upscaler, g_settings.upscaler_sharpness, g_settings.anti_aliasing, said,
                        sizeof said))
        fprintf(stderr, "3ds gpu: settings: %s\n", said);
    if (c->upscaler != SNX_FX_UPSCALE_OFF || c->aa != SNX_FX_AA_OFF)
        for (unsigned i = 0; i < 2; ++i) {
            g_present.fx[i] = snx_fx_create(v->phys, v->device, VK_NULL_HANDLE, SNX_CTR_VK_SLOTS, fx_log);
            if (g_present.fx[i]) {
                snx_fx_set_sharpness(g_present.fx[i], c->sharpness);
                continue;
            }
            fprintf(stderr,
                    "3ds gpu: upscaler=%s and anti_aliasing=%s: their passes could not be made on this device, "
                    "so the screens show with both off\n",
                    snx_fx_upscale_key(c->upscaler), snx_fx_aa_key(c->aa));
            c->upscaler = SNX_FX_UPSCALE_OFF;
            c->aa = SNX_FX_AA_OFF;
            if (i) snx_fx_destroy(g_present.fx[0]);
            g_present.fx[0] = NULL;
            break;
        }
    fprintf(stderr, "3ds gpu: upscaler %s, anti-aliasing %s\n", snx_fx_upscale_label(c->upscaler),
            snx_fx_aa_label(c->aa));
}
bool snx_ctr_present_init(SnxCtrMemory* memory, const char* frame_dir) {
    if (g_present.up) return true;
    /* a finger and the mouse each reach the touch screen once, as on the Switch (backend/sdl3.c): SDL would also send
       each as the other, and a pen as both (it stays a finger) */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "0");
    char why[1024];
    if (!snx_ctr_vk_init(memory, why, sizeof why)) {
        fprintf(stderr, "3ds gpu: %s\n", why);
        return false;
    }
    const SnxCtrVk* v = snx_ctr_vk();
    g_present.frame_dir = frame_dir;
    g_present.separate = g_settings.ctr_separate_screens && v->second && v->second_surface;
    fx_init(v);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = (VkDeviceSize)MAX_SIDE * MAX_SIDE * 4,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
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
    /* vsync's effect: FIFO waits for the display's refresh, mailbox and immediate do not */
    if (g_present.swap[0].swapchain) {
        VkPresentModeKHR mode = g_present.swap[0].mode;
        fprintf(stderr, "3ds gpu: the window shows its images %s (vsync=%d)\n",
                mode == VK_PRESENT_MODE_FIFO_KHR      ? "at the display's refresh, in order (fifo)"
                : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "the newest at the next refresh (mailbox)"
                                                      : "at once, tearing (immediate)",
                g_settings.vsync);
    }
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

/* whether this present is one the dumps keep: the first eight frames, then each screen's about once a second (of the
   host's time, however fast the game runs) as frame-<screen>-latest.ppm */
static bool dump_due(unsigned screen) {
    if (!g_present.frame_dir) return false;
    if (g_present.dumps < 8) return true;
    if (g_present.pending.active) return false;
    double now = snx_seconds();
    if (g_present.latest_at[screen] > 0 && now - g_present.latest_at[screen] < 1.0) return false;
    g_present.latest_at[screen] = now;
    return true;
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
    /* the landscape rows are the framebuffer's columns from its last, each column's pixels a framebuffer row apart */
    (void)snx_ppm_write(file, height, width, rgba + (size_t)(width - 1) * 4, (ptrdiff_t)width * 4, -4, SNX_PPM_RGBA);
    bool closed = !fclose(file);
    if (!latest)
        ++g_present.dumps;
    else if (closed)
        (void)plat_replace_file(staged, path);
}
/* The pending latest dump written once the GPU has done it: at once when it has (a present looks, without waiting), or
   waited for (wait) */
static void write_pending(bool wait) {
    if (!g_present.pending.active) return;
    uint64_t serial = g_present.pending.serial;
    if (!wait && serial >= snx_ctr_vk_serial()) return; /* its recording is still open */
    if (!snx_ctr_vk_wait(serial, wait ? UINT64_MAX : 0)) {
        if (wait) g_present.pending.active = false; /* a lost device: the dump goes */
        return;
    }
    g_present.pending.active = false;
    write_ppm(g_present.pending.screen, g_present.pending.width, g_present.pending.height, g_present.readback_view);
}
/* The blit rectangle of a screen in swapchain pixels: in one window the pair scaled to fit and centred, each screen
   centred across the layout; in its own window the screen scaled to fit and centred. False before a layout and a
   swapchain exist. */
static bool screen_rect(unsigned screen, VkOffset2D rect[2]) {
    const Frame* frames = g_present.frames;
    unsigned window = g_present.separate ? screen : 0;
    VkExtent2D e = g_present.swap[window].extent;
    if (g_present.separate) {
        const Frame* f = &frames[screen];
        if (!f->width || !f->height || !e.width || !e.height) return false;
        double scale = (double)e.width / f->width < (double)e.height / f->height ? (double)e.width / f->width
                                                                                 : (double)e.height / f->height;
        double x = (e.width - f->width * scale) / 2, y = (e.height - f->height * scale) / 2;
        rect[0] = (VkOffset2D){(int32_t)x, (int32_t)y};
        rect[1] = (VkOffset2D){(int32_t)(x + f->width * scale), (int32_t)(y + f->height * scale)};
        return true;
    }
    uint32_t layout_w = snx_max_u32(frames[0].width, frames[1].width);
    uint32_t layout_h = frames[0].height + frames[1].height;
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
   runtime's finger handling does, a press off the screen starts nothing and a drag off it keeps to its edge. Whether
   it touched the screen */
static bool pointer_touch(SDL_Window* window, float x, float y, bool press) {
    const Frame* bottom = &g_present.frames[1];
    unsigned screen = 1, window_index = g_present.separate ? screen : 0;
    VkExtent2D e = g_present.swap[window_index].extent;
    int width = 0, height = 0;
    VkOffset2D rect[2];
    if (!SDL_GetWindowSize(window, &width, &height) || width <= 0 || height <= 0 || !screen_rect(screen, rect))
        return false;
    /* the division below is by the rect's size, so a rect with no pixels has nothing to map */
    if (rect[1].x <= rect[0].x || rect[1].y <= rect[0].y) return false;
    double px = (double)x * e.width / width;
    double py = (double)y * e.height / height;
    if (press && (px < rect[0].x || px >= rect[1].x || py < rect[0].y || py >= rect[1].y)) return false;
    px = snx_clamp_double(px, rect[0].x, rect[1].x);
    py = snx_clamp_double(py, rect[0].y, rect[1].y);
    unsigned native_width = bottom->width / bottom->scale, native_height = bottom->height / bottom->scale;
    uint32_t tx = (uint32_t)((px - rect[0].x) * native_width / (rect[1].x - rect[0].x));
    uint32_t ty = (uint32_t)((py - rect[0].y) * native_height / (rect[1].y - rect[0].y));
    snx_ctr_input_touch(true, snx_min_u32(tx, native_width - 1), snx_min_u32(ty, native_height - 1));
    return true;
}
/* The touch screen takes one pointer at a time, as the console's: the first finger, or the mouse's left button, pressed
   on it holds it until that one lets go. The Switch's touch setting says which reach it: 0 none, 1 the fingers, 2 the
   fingers and the mouse. A mouse event SDL made of a finger is the finger's */
static void pointer_event(const SDL_Event* event) {
    const SnxCtrVk* v = snx_ctr_vk();
    if (!v || !v->host.window) return;
    SDL_Window* bottom = g_present.separate && v->second ? v->second : v->host.window;
    SDL_WindowID id = SDL_GetWindowID(bottom);
    bool held = snx_ctr_input_touched(), by_mouse = held && !g_present.by_finger;
    int width = 0, height = 0;
    switch (event->type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (g_settings.touch_screen == 2 && !held && event->button.windowID == id &&
                event->button.which != SDL_TOUCH_MOUSEID && event->button.button == SDL_BUTTON_LEFT &&
                pointer_touch(bottom, event->button.x, event->button.y, true))
                g_present.by_finger = 0;
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP: /* a release off the screen still ends the press */
            if (by_mouse && event->button.windowID == id && event->button.which != SDL_TOUCH_MOUSEID &&
                event->button.button == SDL_BUTTON_LEFT)
                snx_ctr_input_touch(false, 0, 0);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (by_mouse && event->motion.windowID == id && event->motion.which != SDL_TOUCH_MOUSEID)
                pointer_touch(bottom, event->motion.x, event->motion.y, false);
            break;
        case SDL_EVENT_FINGER_DOWN: /* its point is a fraction of the window */
            if (g_settings.touch_screen >= 1 && !held && event->tfinger.windowID == id &&
                SDL_GetWindowSize(bottom, &width, &height) &&
                pointer_touch(bottom, event->tfinger.x * (float)width, event->tfinger.y * (float)height, true)) {
                g_present.by_finger = 1;
                g_present.finger = event->tfinger.fingerID;
            }
            break;
        case SDL_EVENT_FINGER_MOTION:
            if (held && g_present.by_finger && event->tfinger.fingerID == g_present.finger &&
                event->tfinger.windowID == id && SDL_GetWindowSize(bottom, &width, &height))
                pointer_touch(bottom, event->tfinger.x * (float)width, event->tfinger.y * (float)height, false);
            break;
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            if (held && g_present.by_finger && event->tfinger.fingerID == g_present.finger)
                snx_ctr_input_touch(false, 0, 0);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            /* a focus loss ends the press: this window may not get the release */
            if (event->window.windowID == id) snx_ctr_input_touch(false, 0, 0);
            break;
    }
}
/* a window to fullscreen and back, said when the system refuses */
static void fullscreen(SDL_Window* window, bool on) {
    if (!SDL_SetWindowFullscreen(window, on))
        fprintf(stderr, "3ds gpu: the window cannot go %s: %s\n", on ? "fullscreen" : "back from fullscreen",
                SDL_GetError());
}
void snx_ctr_present_fullscreen(void) {
    const SnxCtrVk* v = snx_ctr_vk();
    if (g_present.up && v && v->host.window) fullscreen(v->host.window, true);
}
/* The window's own keys, as a Switch game's (backend/sdl3.c): F11, or Alt+Enter, switches the window it is pressed in
   to fullscreen and back, F4 turns the FPS limit off and back on (board/clock.h), and Esc twice closes it, a first one
   warning on the terminal; not with Ctrl or the system key, the desktop's, nor F4 or Esc with Alt */
static void key_event(const SDL_Event* event) {
    if (event->type != SDL_EVENT_KEY_DOWN || event->key.repeat || (event->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)))
        return;
    bool alt = event->key.mod & SDL_KMOD_ALT;
    SDL_Window* window = SDL_GetWindowFromID(event->key.windowID);
    if (event->key.key == SDLK_ESCAPE && !alt) {
        if (snx_quit_press(&g_present.quit_by, SDL_GetTicksNS()))
            g_present.closed = 1;
        else {
            char warning[64];
            snx_quit_warning(warning, sizeof warning, "Esc");
            fprintf(stderr, "%s\n", warning);
        }
    } else if (window && ((event->key.key == SDLK_F11 && !alt) || (event->key.key == SDLK_RETURN && alt)))
        fullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
    else if (event->key.key == SDLK_F4 && !alt) {
        int pace = snx_ctr_clock_toggle();
        if (pace == SNX_CTR_PACE_NONE)
            fprintf(stderr, "FPS limit: off\n");
        else if (pace == SNX_CTR_PACE_CONSOLE)
            fprintf(stderr, "FPS limit: the game's pace\n");
        else
            fprintf(stderr, "FPS limit: %d\n", pace);
    }
}
static void pump(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g_present.closed = 1;
        pointer_event(&event);
        key_event(&event);
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
   separate windows this screen alone, scaled to fit and centred. Scaled through the upscaler and the anti-aliasing
   where either is on (slot: the recording's), else in sharp pixels */
static void show_screens(VkCommandBuffer cb, VkImage target, unsigned screen, unsigned slot) {
    image_barrier(cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    /* the blits below overwrite the clear's pixels: their writes must wait for it */
    image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (unsigned i = 0; i < 2; ++i) {
        if (g_present.separate && i != screen) continue;
        Frame* f = &g_present.frames[i];
        VkOffset2D rect[2];
        /* a screen not drawn yet stays black */
        if (!f->image || f->layout == VK_IMAGE_LAYOUT_UNDEFINED || !screen_rect(i, rect)) continue;
        const SnxFxConfig* c = &g_present.fx_config;
        uint32_t shown_w = (uint32_t)(rect[1].x - rect[0].x), shown_h = (uint32_t)(rect[1].y - rect[0].y);
        SnxFxImage fx = {0};
        if (g_present.fx[i]) {
            image_barrier(cb, f->image, f->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            f->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        bool effects =
            g_present.fx[i] && snx_fx_render(g_present.fx[i], cb, slot, f->view, f->width, f->height, 0, 0, f->width,
                                             f->height, 0, shown_w, shown_h, c->upscaler, c->aa, &fx);
        if (!effects) {
            image_barrier(cb, f->image, f->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            f->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        /* the effects' image at the rectangle's size is copied; at the frame's own (anti-aliasing alone, the upscaler
           off) it is scaled in sharp pixels, as the frame is; an upscaler that did not run leaves the smooth scale */
        VkImageBlit blit = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{0, 0, 0},
                           {(int32_t)(effects ? fx.width : f->width), (int32_t)(effects ? fx.height : f->height), 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{rect[0].x, rect[0].y, 0}, {rect[1].x, rect[1].y, 1}}};
        vkCmdBlitImage(cb, effects ? fx.image : f->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       (effects && fx.width == shown_w && fx.height == shown_h) || c->upscaler == SNX_FX_UPSCALE_OFF
                           ? VK_FILTER_NEAREST
                           : VK_FILTER_LINEAR);
    }
    image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}
bool snx_ctr_present_frame(unsigned screen, uint32_t pa, unsigned width, unsigned height, unsigned format,
                           unsigned pitch, uint64_t* hash) {
    if (!g_present.up || screen > 1 || !width || !height || width > MAX_SIDE || height > MAX_SIDE || format > 4)
        return false;
    if (!screen) snx_ctr_vk_pipeline_cache_save(false); /* once the game made no pipeline for a while */
    uint32_t bytes = snx_ctr_pixel_bytes(format);
    SnxCtrVkWindow w;
    uint32_t inside;
    if (pitch < width * bytes || !snx_ctr_vk_window(pa, (uint64_t)pitch * (height - 1) + width * bytes, &w, &inside))
        return false;
    const SnxCtrVk* v = snx_ctr_vk();
    Frame* f = &g_present.frames[screen];
    /* A render target the GPU has not stored is the frame at its render scale: the shader reads it instead of the
       framebuffer, whose pixels it has not written yet */
    VkImageView source = g_present.source_dummy.view;
    unsigned scale = 1, flip = 1;
    static const unsigned target_format[5] = {0, 1, 3, 2, 4}; /* GSP's colour formats to transfer.h's */
    bool from_target = pitch == width * bytes && snx_ctr_vk_target_image(pa, width, height, target_format[format],
                                                                        &source, &scale, &flip);
    unsigned frame_width = height * scale, frame_height = width * scale;
    if (f->width != frame_width || f->height != frame_height) {
        snx_ctr_vk_finish();
        frame_free(f);
        if (!frame_make(f, frame_width, frame_height, scale)) return false;
    }
    /* A frame the host reads: a test's hash and the first dumps are waited for, a latest dump is written once the GPU
       is done with it, at a later present, so the game's thread does not wait for the GPU's queue to drain for it */
    write_pending(false);
    if (hash) write_pending(true); /* the readback is about to be written again */
    bool dump = dump_due(screen), readback = dump || hash;
    bool latest = dump && g_present.dumps >= 8 && !hash, waited = readback && !latest;
    if (!from_target) snx_ctr_vk_sync(pa, pitch * (height - 1) + width * bytes, false); /* the draws over it stored */
    VkCommandBuffer cb = snx_ctr_vk_commands();
    unsigned slot = snx_ctr_vk_slot(); /* the open recording's, whose semaphores it takes */
    uint64_t recording = snx_ctr_vk_serial();
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
    ++g_present.decoded[screen];
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
        /* an image the window can take now, or none this frame, as the Switch's and the Vita's presenters ask: the
           game's threads run on this one, and a wait stops them for as long as the display holds every image (FIFO
           behind, or a locked or remote session that shows none). The screens keep the frame, and the next shown
           has their latest */
        VkResult acquired = vkAcquireNextImageKHR(v->device, g_present.swap[window_index].swapchain, 0,
                                                  g_present.acquire[window_index][slot], VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            (void)swap_create(window_index);
            show = false;
        } else if (acquired == VK_NOT_READY || acquired == VK_TIMEOUT) {
            ++g_present.unshown;
            show = false;
        } else if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            show = false;
    }
    if (show) {
        g_present.bottom_alone = 0;
        show_screens(cb, g_present.swap[window_index].images[index], screen, slot);
    }
    /* submitted, waited for only when the host reads the frame (a dump, a test's hash) */
    bool ok = true;
    if (show) {
        uint64_t serial =
            snx_ctr_vk_submit_with(g_present.acquire[window_index][slot], g_present.presented[window_index][slot]);
        ok = !waited || snx_ctr_vk_wait(serial, UINT64_MAX);
    } else if (waited)
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
    pump();
    if (!ok) return false;
    if (latest) {
        g_present.pending.active = true;
        g_present.pending.serial = recording;
        g_present.pending.screen = screen;
        g_present.pending.width = width;
        g_present.pending.height = height;
    } else if (readback) {
        const unsigned char* rgba = g_present.readback_view;
        if (hash) {
            /* not SNX_FNV_BASIS (a digit short of it), kept so the frame hashes stay what they were */
            *hash = snx_fnv1a(1469598103934665603ull, rgba, (size_t)width * height * 4);
        }
        if (dump) write_ppm(screen, width, height, rgba);
    }
    return true;
}
void snx_ctr_present_report(void) {
    if (!g_present.up) return;
    SnxCtrVkTraffic t = snx_ctr_vk_traffic();
    char copies[96] = "", unshown[80] = "";
    bool copied = snx_ctr_vk_copies() || snx_ctr_vk_shadow(); /* the shadow copies each page too */
    if (copied)
        snprintf(copies, sizeof copies, "; %.1f MiB copied in from the RAM and %.1f MiB back",
                 (double)t.copied_in / 1048576.0, (double)t.copied_out / 1048576.0);
    if (g_present.unshown)
        snprintf(unshown, sizeof unshown, ", %llu not shown with every window image on the display",
                 (unsigned long long)g_present.unshown);
    printf("  gpu: %llu top and %llu bottom screen frames%s; its work read %.1f MiB and wrote %.1f MiB of %s%s; "
           "%llu CPU accesses to its pages trapped\n",
           (unsigned long long)g_present.decoded[0], (unsigned long long)g_present.decoded[1], unshown,
           (double)t.read / 1048576.0, (double)t.written / 1048576.0, copied ? "its copy of the RAM" : "the RAM",
           copies, (unsigned long long)t.traps);
}
void snx_ctr_present_free(void) {
    if (!g_present.up) return;
    write_pending(true);
    const SnxCtrVk* v = snx_ctr_vk();
    vkDeviceWaitIdle(v->device);
    for (unsigned screen = 0; screen < 2; ++screen)
        if (g_present.swap[screen].swapchain) swap_destroy(screen);
    for (unsigned i = 0; i < 2; ++i) frame_free(&g_present.frames[i]);
    frame_free(&g_present.source_dummy);
    for (unsigned i = 0; i < 2; ++i) snx_fx_destroy(g_present.fx[i]);
    for (unsigned screen = 0; screen < 2; ++screen)
        for (unsigned i = 0; i < SNX_CTR_VK_SLOTS; ++i) {
            if (g_present.acquire[screen][i]) vkDestroySemaphore(v->device, g_present.acquire[screen][i], NULL);
            if (g_present.presented[screen][i]) vkDestroySemaphore(v->device, g_present.presented[screen][i], NULL);
        }
    if (g_present.readback) vkDestroyBuffer(v->device, g_present.readback, NULL);
    if (g_present.readback_memory) vkFreeMemory(v->device, g_present.readback_memory, NULL);
    snx_ctr_vk_compute_free(&g_present.decode);
    snx_ctr_vk_transfer_free();
    snx_ctr_vk_y2r_free();
    memset(&g_present, 0, sizeof g_present);
    snx_ctr_vk_free();
}
