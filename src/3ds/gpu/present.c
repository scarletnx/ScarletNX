/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The shown framebuffers, on the 3DS's Vulkan device (vk/device.c): a compute shader (vk/shaders/present.comp)
   reads each one where the guest has it in RAM, decodes it and turns it to the console's landscape screen in an
   image, and the window's swapchain shows both screens as ctr_screen_layout arranges them, each keeping its shape
   (snx_ctr_layout_rects). With separate screens each one has its own window and fills it. Nothing of the
   frame passes through the CPU. The frame dumps (the first eight, then each screen's latest about once a second, in
   frame_dir), the tests' hashes and --shot read a frame back: the shader writes those frames' RGBA pixels to a
   host-visible buffer as well, and a latest dump is written once the GPU is done, without a wait for it. --shot copies
   each screen's image at its render scale into a buffer of its own. The 3DS framebuffer is 240x400/320 portrait, one 240-pixel row per screen column from
   the left, each row from the bottom of the screen up (libctru's console draws pixel (x, y) at x * 240 + 239 - y);
   the window and the dumps show it that way round. A 3D render scaled by its screen's resolution setting is drawn
   from its render target while the GPU keeps that newer than memory, or from the copy that a display transfer made
   of it. */
#include "3ds/gpu/present.h"
#include "common/utilities/tracy.h"
#include "common/gpu/vk/gpu_pstate.h"
#include "3ds/board/clock.h"
#include "3ds/input/input.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/shaders/present_spv.h"
#include "3ds/gpu/vk/vk_transfer.h"
#include "3ds/gpu/vk/vk_y2r.h"
#include "3ds/gpu/vk/vk_render.h"
#include "common/utilities/bits.h"
#include "common/gpu/vk/notice.h"
#include "common/gpu/vk/rects.h"
#include "common/gpu/vk/renderdoc.h"
#include "common/gpu/vk/present_fx.h"
#include "common/gpu/vk/swapchain.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/quit_key.h"
#include "common/utilities/hotkeys.h"
#include "common/utilities/vpad_sdl.h"
#include "common/utilities/state_header.h"
#include "common/platform/file.h"
#include "common/utilities/window_fullscreen.h"
#include "common/utilities/window_icon.h"
#include "snx_settings.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SIDE 1024u /* the largest framebuffer side presented */
/* The large layout's small screen is its size over 2.25 = 9/4: Azahar's Android large_screen_proportion, the one
   proportion on every platform (FloatSetting.kt at 11eceda5). The steps run in integers, so each rectangle is exact
   and the same on every host. The small screen's sides take Azahar's floors (320 / 2.25 = 142, 240 / 2.25 = 106) */
#define LARGE_SCREEN_NUM 4u
#define LARGE_SCREEN_DEN 9u

static volatile sig_atomic_t g_quit; /* a headless run's Ctrl+C or SIGTERM */
static void quit_signal(int signal_number) {
    (void)signal_number;
    g_quit = 1;
}

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkImageLayout layout;   /* where the last command recorded on it left it */
    unsigned width, height; /* landscape */
    unsigned scale;         /* the render scale it shows: 1 is the console's */
} Frame;
static struct {
    char why[1024]; /* the last init's failure */
    SnxCtrVkCompute decode;
    Frame frames[2];
    Frame source_dummy; /* bound where a frame is decoded from the guest, which the shader does not read */
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    unsigned char* readback_view;
    SnxVkSwapchain swap[2]; /* the combined window's, or each screen's own */
    SnxVkPaced paced[2];    /* their presents' ids, for frame_pacing */
    /* each screen drawn into the window by the effects (common/gpu/vk/present_fx.h): the upscaler and the
       anti-aliasing as the settings say, or the screen sampled in sharp pixels with both Off. A screen's own, as both
       are drawn in one recording, whose frame slot each keeps its images by */
    SnxFxConfig fx_config;
    SnxFx* fx[2];
    /* each submission slot's: a slot is free again only once its submission is done, and so are they */
    VkSemaphore acquire[2][SNX_CTR_VK_SLOTS];
    /* each swapchain image's: free again only once that image is acquired again (a present's wait has no fence) */
    VkSemaphore presented[2][SNX_VK_SWAP_IMAGES];
    /* a line over the game for a few seconds, as a Switch game shows one: Esc's first press. In the window it was
       asked in, each window's band drawn by its own (common/gpu/vk/notice.h) */
    SnxNotice notice;
    SDL_WindowID notice_window;
    SnxVkNotice* notice_vk[2];
    SnxVkRects* controls; /* the on-screen controls over the window with both screens, or the top's own */
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
    uint64_t
        console_frames; /* the console's frames: the top screen's, or the bottom's until the first top screen frame */
    uint64_t unshown;      /* the shows that found every image of their window on the display */
    int state_ask;         /* a save states' key (HOTKEY_SAVE_STATE...) plus 1, done between two dispatches; 0 none */
    bool dump;             /* the thread dump's key (F5): the threads listed between two dispatches */
    int pace;              /* the clock's pace as the keys last left it (common/utilities/vpad_sdl.h) */
    char* game;            /* the game's folder with its separator, whose captures/ a screenshot goes in */
    SDL_FingerID finger;   /* the finger holding the touch screen, while one does (by_finger) */
    unsigned up : 1, closed : 1, separate : 1, by_finger : 1, keys : 1;
    uint64_t presents; /* the frames shown in the first window, RenderDoc's captures counting by them */
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
                               .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                               .window = snx_vk_window_extent(&v->host, fallback),
                               .unsynced = !g_settings.vsync,
                               .views = true};
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
    for (unsigned screen = 0; screen < 2; ++screen) {
        for (unsigned i = 0; i < SNX_CTR_VK_SLOTS; ++i)
            if (vkCreateSemaphore(v->device, si, NULL, &g_present.acquire[screen][i]) != VK_SUCCESS) return false;
        for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
            if (vkCreateSemaphore(v->device, si, NULL, &g_present.presented[screen][i]) != VK_SUCCESS) return false;
    }
    return true;
}
static void fx_log(const char* text) { fprintf(stderr, "3ds gpu: %s\n", text); }
/* the upscaler and the anti-aliasing the settings ask for, said once, and each screen's draw: false, said, where one
   could not be made */
static bool fx_init(const SnxCtrVk* v) {
    char said[512];
    SnxFxConfig* c = &g_present.fx_config;
    if (snx_fx_settings(c, g_settings.upscaler, g_settings.upscaler_sharpness, g_settings.anti_aliasing, said,
                        sizeof said))
        fprintf(stderr, "3ds gpu: settings: %s\n", said);
    for (unsigned i = 0; i < 2; ++i) {
        if (!(g_present.fx[i] = snx_fx_create(v->phys, v->device, VK_NULL_HANDLE, SNX_CTR_VK_SLOTS, fx_log))) {
            fprintf(stderr, "3ds gpu: the %s screen's draw into the window could not be made on this device\n",
                    i ? "bottom" : "top");
            return false;
        }
        snx_fx_set_sharpness(g_present.fx[i], c->sharpness);
        snx_fx_set_off_sharp(g_present.fx[i], 1); /* the window's own scale, in sharp pixels */
    }
    fprintf(stderr, "3ds gpu: upscaler %s, anti-aliasing %s\n", snx_fx_upscale_label(c->upscaler),
            snx_fx_aa_label(c->aa));
    return true;
}
bool snx_ctr_present_init(SnxCtrMemory* memory, const char* frame_dir, bool headless) {
    if (g_present.up) return true;
    /* a finger and the mouse each reach the touch screen once, as on the Switch (backend/sdl3.c): SDL would also send
       each as the other, and a pen as both (it stays a finger) */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "0");
    /* Headless starts no SDL video, audio or gamepad, so it installs the signal handler itself: Ctrl+C and SIGTERM
       end the run with its report, --shot and a finalised WAV */
    if (headless) {
        g_quit = 0;
        signal(SIGINT, quit_signal);
        signal(SIGTERM, quit_signal);
    }
    char why[1024];
    if (!snx_ctr_vk_init(memory, headless, why, sizeof why)) {
        fprintf(stderr, "3ds gpu: %s\n", why);
        memset(&g_present, 0, sizeof g_present);
        snprintf(g_present.why, sizeof g_present.why, "%s", why);
        return false; /* the signal handlers stay for a headless run with no Vulkan device */
    }
    /* the device is up: from here the presenter's free unwinds a partial start too */
    g_present.up = 1;
    const SnxCtrVk* v = snx_ctr_vk();
    g_present.frame_dir = frame_dir;
    g_present.separate = g_settings.ctr_screen_layout == SNX_CTR_LAYOUT_SEPARATE && v->second && v->second_surface;
    if (!fx_init(v)) {
        snx_ctr_present_free();
        return false;
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = (VkDeviceSize)MAX_SIDE * MAX_SIDE * 4,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!snx_ctr_vk_compute(snx_ctr_present_spv, snx_ctr_present_spv_size / 4, 3, 2, 52, &g_present.decode) ||
        !frame_make(&g_present.source_dummy, 1, 1, 1) ||
        vkCreateBuffer(v->device, &bi, NULL, &g_present.readback) != VK_SUCCESS) {
        snx_ctr_present_free();
        return false;
    }
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
        !semaphores(v, &si)) {
        snx_ctr_present_free();
        return false;
    }
    g_present.readback_view = mapped;
    for (unsigned screen = 0; screen < 2; ++screen)
        if (!(g_present.notice_vk[screen] = snx_vk_notice_create(v->phys, v->device, SNX_CTR_VK_SLOTS))) {
            snx_ctr_present_free();
            return false;
        }
    image_barrier(snx_ctr_vk_commands(), g_present.source_dummy.image, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_GENERAL);
    g_present.source_dummy.layout = VK_IMAGE_LAYOUT_GENERAL;
    (void)swap_create(0);
    if (g_present.separate) (void)swap_create(1);
    /* vsync's effect: FIFO waits for the display's refresh, mailbox and immediate do not */
    if (g_present.swap[0].swapchain) {
        char said[160];
        snx_vk_present_said(g_present.swap[0].mode, g_settings.vsync, said, sizeof said);
        fprintf(stderr, "3ds gpu: %s\n", said);
        snx_vk_pacing_said(&v->pacing, g_settings.frame_pacing, said, sizeof said);
        fprintf(stderr, "3ds gpu: %s\n", said);
    }
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
const char* snx_ctr_present_why(void) { return g_present.why; }
bool snx_ctr_present_lost(char* why, size_t why_size) {
    const SnxCtrVk* v = g_present.up ? snx_ctr_vk() : NULL;
    if (!v || v->ring.lost == VK_SUCCESS) return false;
    snprintf(why, why_size, "the GPU stopped answering (%s): no frame is drawn after it",
             snx_vk_result_str(v->ring.lost));
    return true;
}
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
    if (latest) {
        if (snx_path_format(path, sizeof path, "%s/frame-%u-latest.ppm", g_present.frame_dir, screen)) return;
        if (snx_path_format(staged, sizeof staged, "%s.partial", path)) return;
    } else {
        if (snx_path_format(path, sizeof path, "%s/frame-%u-%02u.ppm", g_present.frame_dir, screen, g_present.dumps))
            return;
    }
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
/* a screen's rectangle in the window's pixels, each edge truncated: the right and bottom edges from the position plus
   the size, so the size is their difference from the left and top edges */
static void set_rect(SnxCtrRect* r, double x, double y, double width, double height) {
    r->x = (int32_t)x;
    r->y = (int32_t)y;
    r->width = (int32_t)(x + width) - r->x;
    r->height = (int32_t)(y + height) - r->y;
}
/* no rectangle for a screen that has not presented (present.h's contract) */
static void shown_rects(SnxCtrRects* rects, const bool shown[2]) {
    if (!shown[0]) rects->top = (SnxCtrRect){0};
    if (!shown[1]) rects->bottom = (SnxCtrRect){0};
}
bool snx_ctr_layout_rects(SnxCtrLayout layout, bool swap, uint32_t window_width, uint32_t window_height,
                          SnxCtrScreens screens, SnxCtrRects* rects) {
    if (!window_width || !window_height) return false;
    const SnxCtrScreens given = screens;
    rects->top = rects->bottom = (SnxCtrRect){0};
    /* A screen with no frame has no size. In the one-screen layout the missing screen gets no rectangle, so that
       window shows black until it has a frame; the console's size only keeps the arithmetic defined there. In the
       large and side layouts the other screen keeps its place, the console's size standing in for the missing one
       in the box. In the stacked layout the missing screen adds no width and no height instead, so the other one fills
       the box alone */
    const SnxCtrScreens console = {400, 240, 320, 240};
    bool shown[2] = {screens.top_width && screens.top_height, screens.bottom_width && screens.bottom_height};
    if (!shown[0]) screens.top_width = console.top_width;
    if (!shown[0]) screens.top_height = console.top_height;
    if (!shown[1]) screens.bottom_width = console.bottom_width;
    if (!shown[1]) screens.bottom_height = console.bottom_height;
    /* the settings' range keeps to the five (ini_read clamps); another caller's value is shown stacked */
    /* separate reaches here only when no second window was made: the one window then shows the screens stacked */
    if ((unsigned)layout >= SNX_CTR_LAYOUT_COUNT || layout == SNX_CTR_LAYOUT_SEPARATE) layout = SNX_CTR_LAYOUT_STACKED;
    if (layout == SNX_CTR_LAYOUT_SINGLE) {
        /* the one screen, the top or with the swap the bottom, scaled to fit and centred */
        uint32_t width = swap ? screens.bottom_width : screens.top_width;
        uint32_t height = swap ? screens.bottom_height : screens.top_height;
        double scale = (double)window_width / width < (double)window_height / height ? (double)window_width / width
                                                                                     : (double)window_height / height;
        set_rect(swap ? &rects->bottom : &rects->top, (window_width - width * scale) / 2,
                 (window_height - height * scale) / 2, width * scale, height * scale);
        shown_rects(rects, shown);
        return true;
    }
    if (layout == SNX_CTR_LAYOUT_STACKED) {
        /* one above the other in a box the wider screen's width by both heights, the box scaled to fit and centred,
           each screen centred across it */
        uint32_t width = snx_max_u32(given.top_width, given.bottom_width);
        uint32_t height = given.top_height + given.bottom_height;
        /* no frame in either screen, nothing to place: the scale below would divide by zero */
        if (!width || !height) return true;
        double scale = (double)window_width / width < (double)window_height / height ? (double)window_width / width
                                                                                     : (double)window_height / height;
        double x = (window_width - width * scale) / 2, y = (window_height - height * scale) / 2;
        set_rect(&rects->top, x + (width - given.top_width) * scale / 2, swap ? y + given.bottom_height * scale : y,
                 given.top_width * scale, given.top_height * scale);
        set_rect(&rects->bottom, x + (width - given.bottom_width) * scale / 2, swap ? y : y + given.top_height * scale,
                 given.bottom_width * scale, given.bottom_height * scale);
        shown_rects(rects, shown);
        return true;
    }
    /* Azahar's LargeFrameLayout at 11eceda5 (src/core/frontend/framebuffer_layout.cpp, GPL-3.0-or-later), step by step:
       the large screen at the console's size and the small one at its own size over 2.25 (Azahar's Android default),
       centred vertically at the large one's right (Azahar's MiddleRight), make one box, scaled to fit the window and
       centred, the large one at the box's left. Side by side gives the small one its whole size. A narrow or a tall
       window still leaves a rectangle for each, but a window of a few pixels, too small for the box, leaves none */
    const uint64_t num = layout == SNX_CTR_LAYOUT_LARGE ? LARGE_SCREEN_NUM : 1;
    const uint64_t den = layout == SNX_CTR_LAYOUT_LARGE ? LARGE_SCREEN_DEN : 1;
    const uint32_t large_w = swap ? screens.bottom_width : screens.top_width;
    const uint32_t large_h = swap ? screens.bottom_height : screens.top_height;
    const uint32_t small_w = swap ? screens.top_width : screens.bottom_width;
    const uint32_t small_h = swap ? screens.top_height : screens.bottom_height;
    const uint64_t sw = small_w * num / den, sh = small_h * num / den;
    const uint64_t box_w = large_w + sw, box_h = sh > large_h ? sh : large_h;
    uint64_t fit_w = window_width, fit_h = window_height;
    if (window_width * box_h <= window_height * box_w)
        fit_h = window_width * box_h / box_w;
    else
        fit_w = window_height * box_w / box_h;
    const uint64_t x = (window_width - fit_w) / 2, y = (window_height - fit_h) / 2;
    const uint64_t large_pw = large_w * fit_h / box_h, large_ph = large_h * fit_h / box_h;
    const uint64_t small_pw = sw * fit_h / box_h, small_ph = sh * fit_h / box_h;
    const SnxCtrRect large = {(int32_t)x, (int32_t)y, (int32_t)large_pw, (int32_t)large_ph};
    const int64_t small_y = (int64_t)y + ((int64_t)large_ph - (int64_t)small_ph) / 2;
    const SnxCtrRect small = {(int32_t)(x + large_pw), (int32_t)small_y, (int32_t)small_pw, (int32_t)small_ph};
    rects->top = swap ? small : large;
    rects->bottom = swap ? large : small;
    shown_rects(rects, shown);
    return true;
}
/* The blit rectangle of a screen in swapchain pixels: in one window the layout's (snx_ctr_layout_rects), in its own
   window the screen scaled to fit and centred. The layout takes each screen at the console's size, its frame's size
   over its render scale. False before a layout and a swapchain exist, and for a screen the layout leaves no rectangle:
   the hidden one, or one with no frame yet */
static bool screen_rect(unsigned screen, VkOffset2D rect[2]) {
    const Frame* frames = g_present.frames;
    uint32_t width[2], height[2];
    for (unsigned i = 0; i < 2; ++i) {
        width[i] = frames[i].scale ? frames[i].width / frames[i].scale : 0;
        height[i] = frames[i].scale ? frames[i].height / frames[i].scale : 0;
    }
    unsigned window = g_present.separate ? screen : 0;
    VkExtent2D e = g_present.swap[window].extent;
    if (g_present.separate) {
        if (!width[screen] || !height[screen] || !e.width || !e.height) return false;
        double scale = (double)e.width / width[screen] < (double)e.height / height[screen]
                           ? (double)e.width / width[screen]
                           : (double)e.height / height[screen];
        double x = (e.width - width[screen] * scale) / 2, y = (e.height - height[screen] * scale) / 2;
        rect[0] = (VkOffset2D){(int32_t)x, (int32_t)y};
        rect[1] = (VkOffset2D){(int32_t)(x + width[screen] * scale), (int32_t)(y + height[screen] * scale)};
        return true;
    }
    SnxCtrScreens screens = {width[0], height[0], width[1], height[1]};
    SnxCtrRects rects = {0};
    if (!snx_ctr_layout_rects((SnxCtrLayout)g_settings.ctr_screen_layout, g_settings.ctr_screen_swap != 0, e.width,
                              e.height, screens, &rects))
        return false;
    const SnxCtrRect* r = screen ? &rects.bottom : &rects.top;
    if (r->width <= 0 || r->height <= 0) return false;
    rect[0] = (VkOffset2D){r->x, r->y};
    rect[1] = (VkOffset2D){r->x + r->width, r->y + r->height};
    return true;
}
/* The touch screen's pixel under a pointer at x and y, in the window's points as SDL reports them. The draw's own scale
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
    char why[256];
    if (!snx_window_fullscreen(window, on, why, sizeof why)) fprintf(stderr, "3ds gpu: %s\n", why);
}
void snx_ctr_present_fullscreen(void) {
    const SnxCtrVk* v = snx_ctr_vk();
    if (g_present.up && v && v->host.window) fullscreen(v->host.window, true);
}
/* a line over the game in window (0: the top screen's) for a few seconds */
static void notice(const char* text, SDL_WindowID window) {
    snx_notice_show(&g_present.notice, text, SDL_GetTicksNS());
    g_present.notice_window = window || !g_present.up ? window : SDL_GetWindowID(screen_window(0));
}
static void say(const char* line) { fprintf(stderr, "%s\n", line); }
/* a key's line over the window it was pressed in, of the two the screens may have */
static void show(const char* line) { notice(line, snx_vpad_window_key_window()); }
/* the runtime's keys of the window, as a Switch game's and every console's (common/utilities/vpad_sdl.h): the quit key
   twice, fullscreen in the window pressed, Pause, F4's pace, which the clock follows (board/clock.h), F3, F7 to F9
   between two dispatches; a key the 3DS has nothing for said so */
static void key_action(int action) {
    char path[1024];
    if (action == HOTKEY_SAVE_STATE || action == HOTKEY_LOAD_STATE || action == HOTKEY_NEXT_SLOT)
        g_present.state_ask = action + 1;
    else if (action == HOTKEY_THREAD_DUMP)
        g_present.dump = true;
    else if (action == HOTKEY_CAPTURE)
        (void)snx_renderdoc_capture(1); /* the frame after the next present, said in the log and over the game */
    else if (action == HOTKEY_UPSCALER || action == HOTKEY_ANTI_ALIASING) { /* both screens', for the run */
        int* at = action == HOTKEY_ANTI_ALIASING ? &g_present.fx_config.aa : &g_present.fx_config.upscaler;
        *at = action == HOTKEY_ANTI_ALIASING ? snx_fx_aa_next(*at) : snx_fx_upscale_next(*at);
        snx_vpad_window_effect(action,
                               action == HOTKEY_ANTI_ALIASING ? snx_fx_aa_label(*at) : snx_fx_upscale_label(*at));
    } else if (action == HOTKEY_SCREENSHOT) {
        snx_capture_path(path, sizeof path - 4, g_present.game ? g_present.game : "./");
        strcat(path, ".ppm");
        char line[1100];
        bool saved = snx_ctr_present_shot(path); /* both screens, the top above the bottom, as --shot writes them */
        snprintf(line, sizeof line, saved ? "screenshot: %s" : "the screenshot could not be written to %s", path);
        say(line);
        show(saved ? "Screenshot saved in captures" : "Screenshot not saved: the log says why");
    } else
        snx_vpad_window_none(action, "its window has none of it yet");
}
static void pump(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g_present.closed = 1;
        int action = -1;
        SnxVpadEvent took = g_present.keys ? snx_vpad_window_event(&event, &action) : SNX_VPAD_PASS;
        if (took == SNX_VPAD_QUIT) g_present.closed = 1;
        if (took == SNX_VPAD_ACTION) key_action(action);
        if (took != SNX_VPAD_PASS) continue;
        pointer_event(&event);
        snx_ctr_input_event(&event); /* the gamepads connected and gone */
    }
    int pace = snx_vpad_window_pace();
    if (g_present.keys && pace != g_present.pace) snx_ctr_clock_set_pace(g_present.pace = pace); /* F4 */
}
bool snx_ctr_present_dump_asked(void) {
    bool asked = g_present.dump;
    g_present.dump = false;
    return asked;
}
int snx_ctr_present_state_ask(void) {
    int ask = g_present.state_ask - 1;
    g_present.state_ask = 0;
    return ask;
}
void snx_ctr_present_keys(SDL_Window* window, const char* ini) {
    if (g_present.keys || !window) return;
    snx_vpad_window_open(window, ini, say, show);
    g_present.pace = snx_ctr_clock_pace();
    snx_vpad_window_set_pace(g_present.pace);
    g_present.keys = 1;
}
void snx_ctr_present_game(const char* folder, const char* ini) {
    free(g_present.game);
    g_present.game = folder ? snx_format("%s/", folder) : NULL;
    const SnxCtrVk* v = snx_ctr_vk();
    if (!g_present.up || !v || !v->host.window || g_present.keys) return;
    if (!(g_present.controls = snx_vk_rects_create(v->phys, v->device, SNX_CTR_VK_SLOTS)))
        fprintf(stderr, "3ds gpu: out of memory for the on-screen controls: they are not drawn\n");
    snx_ctr_present_keys(v->host.window, ini);
}
const char* snx_ctr_present_notice(void) {
    return snx_notice_shown(&g_present.notice, SDL_GetTicksNS()) ? g_present.notice.text : "";
}
/* SDL turns SIGINT and SIGTERM into a quit event in a windowed run. A headless run installs its own handler, since it
   starts no SDL subsystem, and this is how Ctrl+C ends it with its report, --shot and a finalised WAV */
bool snx_ctr_present_closed(void) {
    if (g_quit) g_present.closed = 1;
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
static void notice_log(const char* text) { fprintf(stderr, "3ds gpu: %s\n", text); }
/* The window's picture: in one window both screens where the layout puts them (snx_ctr_layout_rects), the rest
   black; in separate windows this screen alone, scaled to fit and centred. Each screen drawn into the window's
   rendering by its effects (slot: the recording's), the upscaler's and the anti-aliasing's last pass or, with both
   off, the screen in sharp pixels. Over it the notice while one shows in that window, its band in slot's buffer */
static void show_screens(VkCommandBuffer cb, unsigned window, uint32_t index, unsigned screen, unsigned slot) {
    const SnxVkSwapchain* sc = &g_present.swap[window];
    const SnxFxConfig* c = &g_present.fx_config;
    bool drawn[2] = {false, false};
    /* every pass but each screen's last, outside the window's rendering */
    for (unsigned i = 0; i < 2; ++i) {
        if (g_present.separate && i != screen) continue;
        Frame* f = &g_present.frames[i];
        VkOffset2D rect[2];
        /* a screen not drawn yet stays black */
        if (!f->image || f->layout == VK_IMAGE_LAYOUT_UNDEFINED || !screen_rect(i, rect)) continue;
        VkRect2D to = {rect[0], {(uint32_t)(rect[1].x - rect[0].x), (uint32_t)(rect[1].y - rect[0].y)}};
        image_barrier(cb, f->image, f->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        f->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        drawn[i] = snx_fx_prepare(g_present.fx[i], cb, slot, f->view, f->layout, f->width, f->height, 0, 0, f->width,
                                  f->height, 0, to, sc->format.format, c->upscaler, c->aa);
    }
    snx_vk_swapchain_begin(cb, sc, index);
    for (unsigned i = 0; i < 2; ++i)
        if (drawn[i]) snx_fx_draw(g_present.fx[i], cb, slot);
    /* the notice over the window it was asked in: the screen's own, or the one both share */
    SDL_Window* shown = screen_window(window);
    if (shown && SDL_GetWindowID(shown) == g_present.notice_window)
        (void)snx_vk_notice_draw(g_present.notice_vk[window], cb, slot, sc->format.format, sc->extent.width,
                                 sc->extent.height, snx_display_percent(SDL_GetWindowDisplayScale(shown)),
                                 &g_present.notice, SDL_GetTicksNS(), notice_log);
    if (!window && g_present.keys) /* the window the keys and the controls belong to */
        (void)snx_vk_rects_draw(g_present.controls, cb, slot, sc->format.format, sc->extent.width, sc->extent.height,
                                snx_vpad_window_rects((int)sc->extent.width, (int)sc->extent.height), notice_log);
    snx_vk_swapchain_end(cb, sc, index);
}
bool snx_ctr_present_frame(unsigned screen, uint32_t pa, unsigned width, unsigned height, unsigned format,
                           unsigned pitch, uint64_t* hash) {
    SNX_ZONE(z_present, "3ds present"); /* a screen's drawing and present, in a Tracy capture (F10) */
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
    /* A render target the GPU has not stored, or a display transfer's copy of one, is the frame at its render scale:
       the shader reads it instead of the framebuffer, whose pixels it has not written yet, and a copy's bytes it
       compares with the framebuffer's memory. The GSP's colour format is transfer.h's, as a target's is */
    SnxCtrVkScaled scaled = {.view = g_present.source_dummy.view, .scale = 1, .flip = 1};
    bool from_target = snx_ctr_vk_target_image(pa, width, height, format, pitch, &scaled);
    unsigned scale = scaled.scale;
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
    if (!from_target || scaled.kept) /* the draws over it stored, and its stale pages in the copy */
        snx_ctr_vk_sync(pa, pitch * (height - 1) + width * bytes, false);
    VkCommandBuffer cb = snx_ctr_vk_commands();
    unsigned slot = snx_ctr_vk_slot(); /* the open recording's, whose acquire semaphore it takes */
    uint64_t recording = snx_ctr_vk_serial();
    snx_ctr_vk_compute_barrier(cb);    /* after what the GPU wrote to the framebuffer */
    image_barrier(cb, f->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL); /* the shader writes all of it */
    f->layout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet set = snx_ctr_vk_set(g_present.decode.set);
    VkDescriptorBufferInfo in = {w.buffer, w.offset, w.range}, out = {g_present.readback, 0, VK_WHOLE_SIZE},
                           kept = {scaled.kept ? scaled.kept : g_present.readback, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo image = {VK_NULL_HANDLE, f->view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo from = {VK_NULL_HANDLE, scaled.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[5] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
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
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &kept},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 3,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &image},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 4,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &from}};
    vkUpdateDescriptorSets(v->device, 5, writes, 0, NULL);
    uint32_t push[13] = {inside,        format,      pitch, width,    height,   readback,
                         from_target,   scaled.flip, scale, scaled.x, scaled.y, scaled.kept ? 1u : 0u,
                         scaled.kept_at};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_present.decode.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_present.decode.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, g_present.decode.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    /* the shader writes a block per console pixel, or one image pixel of a direct target */
    unsigned columns = from_target && !scaled.kept ? width * scale : width;
    unsigned rows = from_target && !scaled.kept ? height * scale : height;
    vkCmdDispatch(cb, (columns + 7) / 8, (rows + 7) / 8, 1);
    ++g_present.decoded[screen];
    /* both screens in one window, at each top screen frame: a game draws both every frame, and showing at each
       screen's would wait for two vblanks a frame. A bottom screen that changes while the top one stays still shows
       at its second frame. In separate windows each screen shows at its own */
    unsigned window_index = g_present.separate ? screen : 0;
    /* a window the swapchain was not made for (a phone turned tall, a window dragged): the frames show turned or
       stretched, and the driver need not say the surface is out of date. Made again until the surface reports the
       window's size, which it does a frame or two after a turn */
    {
        SDL_Window* window = screen_window(screen);
        int ww = 0, wh = 0;
        if (window && SDL_GetWindowSizeInPixels(window, &ww, &wh) && ww > 0 && wh > 0 &&
            g_present.swap[window_index].swapchain &&
            ((uint32_t)ww != g_present.swap[window_index].extent.width ||
             (uint32_t)wh != g_present.swap[window_index].extent.height))
            (void)swap_create(window_index);
    }
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
        show_screens(cb, window_index, index, screen, slot);
    }
    /* submitted, waited for only when the host reads the frame (a dump, a test's hash) */
    bool ok = true;
    if (show) {
        uint64_t serial =
            snx_ctr_vk_submit_with(g_present.acquire[window_index][slot], g_present.presented[window_index][index]);
        ok = !waited || snx_ctr_vk_wait(serial, UINT64_MAX);
    } else if (waited)
        ok = snx_ctr_vk_finish();
    if (show && ok) {
        VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                    .waitSemaphoreCount = 1,
                                    .pWaitSemaphores = &g_present.presented[window_index][index],
                                    .swapchainCount = 1,
                                    .pSwapchains = &g_present.swap[window_index].swapchain,
                                    .pImageIndices = &index};
        if (!window_index) snx_renderdoc_present(v->host.instance.instance, ++g_present.presents);
        snx_gpu_pstate_present(); /* a kgsl hold lasts while frames are presented */
        snx_vk_pacing_present(&v->pacing, &g_present.paced[window_index], v->device,
                              g_present.swap[window_index].swapchain,
                              g_settings.vsync && g_present.pace == SNX_PACE_CONSOLE, &present, "3ds gpu");
        VkResult shown = vkQueuePresentKHR(v->queue, &present);
        if (!window_index) SNX_FRAME_MARK(); /* a frame of the capture's timeline, at the top screen's */
        if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR) (void)swap_create(window_index);
    }
    pump();
    if (!ok) return false;
    if (screen == 0 || !g_present.decoded[0]) ++g_present.console_frames;
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
uint64_t snx_ctr_present_frames(void) { return g_present.up ? g_present.console_frames : 0; }
/* a host-visible buffer a shot copies a frame into, sized for that frame */
typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char* map;
} ShotBuffer;
static bool shot_buffer_make(SnxCtrVk* v, VkDeviceSize size, ShotBuffer* b) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    *b = (ShotBuffer){0};
    if (vkCreateBuffer(v->device, &bi, NULL, &b->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->device, b->buffer, &req);
    uint32_t type = snx_ctr_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX)
        type = snx_ctr_vk_memory_type(req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(v->device, &ai, NULL, &b->memory) != VK_SUCCESS ||
        vkBindBufferMemory(v->device, b->buffer, b->memory, 0) != VK_SUCCESS ||
        vkMapMemory(v->device, b->memory, 0, VK_WHOLE_SIZE, 0, (void**)&b->map) != VK_SUCCESS) {
        if (b->memory) vkFreeMemory(v->device, b->memory, NULL);
        if (b->buffer) vkDestroyBuffer(v->device, b->buffer, NULL);
        *b = (ShotBuffer){0};
        return false;
    }
    return true;
}
static void shot_buffer_free(SnxCtrVk* v, ShotBuffer* b) {
    if (b->map) vkUnmapMemory(v->device, b->memory);
    if (b->memory) vkFreeMemory(v->device, b->memory, NULL);
    if (b->buffer) vkDestroyBuffer(v->device, b->buffer, NULL);
    *b = (ShotBuffer){0};
}
/* A screen's decoded frame (an RGBA image on the device) into rgba, width * height * 4 bytes; false when the screen
   has no frame or the GPU lost it */
static bool read_frame(unsigned screen, VkBuffer target, unsigned char* map, unsigned char* rgba, unsigned* width,
                       unsigned* height) {
    Frame* f = &g_present.frames[screen];
    *width = *height = 0;
    if (!f->image || !f->width || !f->height) return false;
    VkCommandBuffer cb = snx_ctr_vk_commands();
    if (!cb) return false;
    snx_ctr_vk_compute_barrier(cb); /* after the GPU's last work, and any write to this buffer */
    image_barrier(cb, f->image, f->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    f->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkBufferImageCopy region = {.bufferOffset = 0,
                                .bufferRowLength = 0,
                                .bufferImageHeight = 0,
                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageOffset = {0, 0, 0},
                                .imageExtent = {f->width, f->height, 1}};
    vkCmdCopyImageToBuffer(cb, f->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, 1, &region);
    if (!snx_ctr_vk_finish()) return false;
    memcpy(rgba, map, (size_t)f->width * f->height * 4);
    *width = f->width;
    *height = f->height;
    return true;
}
/* both screens' last frames, the top above the bottom on black, read back from the GPU into one RGBA picture, owned,
   its size into w and h; NULL where none was decoded or memory runs out */
static unsigned char* shot_canvas(unsigned* w, unsigned* h) {
    *w = *h = 0;
    if (!g_present.up) return NULL;
    write_pending(true);
    SnxCtrVk* v = snx_ctr_vk();
    unsigned char *screens[2] = {NULL, NULL}, *canvas = NULL;
    unsigned width[2] = {0, 0}, height[2] = {0, 0};
    VkDeviceSize bytes = 0;
    for (unsigned screen = 0; screen < 2; ++screen) {
        Frame* f = &g_present.frames[screen];
        if (f->image && (VkDeviceSize)f->width * f->height * 4 > bytes) bytes = (VkDeviceSize)f->width * f->height * 4;
    }
    ShotBuffer shot = {0};
    bool ok = bytes && shot_buffer_make(v, bytes, &shot);
    if (ok) snx_ctr_vk_render_close(); /* a draw may have left its rendering pass open */
    for (unsigned screen = 0; screen < 2 && ok; ++screen) {
        Frame* f = &g_present.frames[screen];
        if (!f->image) continue;
        screens[screen] = malloc((size_t)f->width * f->height * 4);
        ok = screens[screen] &&
             read_frame(screen, shot.buffer, shot.map, screens[screen], &width[screen], &height[screen]);
    }
    shot_buffer_free(v, &shot);
    unsigned layout_w = width[0] > width[1] ? width[0] : width[1];
    unsigned layout_h = height[0] + height[1];
    if (ok && layout_w && layout_h && (canvas = calloc((size_t)layout_w * layout_h, 4))) {
        unsigned y = 0;
        for (unsigned screen = 0; screen < 2; ++screen) {
            if (!screens[screen]) continue;
            unsigned x = (layout_w - width[screen]) / 2;
            for (unsigned row = 0; row < height[screen]; ++row)
                memcpy(canvas + ((size_t)(y + row) * layout_w + x) * 4,
                       screens[screen] + (size_t)row * width[screen] * 4, (size_t)width[screen] * 4);
            y += height[screen];
        }
        *w = layout_w, *h = layout_h;
    }
    for (unsigned screen = 0; screen < 2; ++screen) free(screens[screen]);
    return canvas;
}
bool snx_ctr_present_shot(const char* path) {
    if (!g_present.up || !path || !path[0]) return false;
    unsigned layout_w, layout_h;
    unsigned char* canvas = shot_canvas(&layout_w, &layout_h);
    bool ok = canvas != NULL;
    if (ok) {
        char staged[1040], native[1024];
        if (snx_path_format(native, sizeof native, "%s", path) ||
            snx_path_format(staged, sizeof staged, "%s.partial", native))
            ok = false;
        else {
            FILE* file = fopen(staged, "wb");
            ok = file && !snx_ppm_write(file, layout_w, layout_h, canvas, 4, (ptrdiff_t)layout_w * 4, SNX_PPM_RGBA);
            if (file && fclose(file)) ok = false;
            if (ok)
                ok = !plat_replace_file(staged, native);
            else
                remove(staged);
        }
    }
    free(canvas);
    return ok;
}
bool snx_ctr_present_thumb(uint8_t* thumb) {
    unsigned w, h;
    unsigned char* canvas = shot_canvas(&w, &h);
    if (canvas) snx_state_thumb(canvas, (int)w, (int)h, (size_t)w * 4, thumb);
    free(canvas);
    return canvas != NULL;
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
    if (g_present.keys) snx_vpad_window_close();
    free(g_present.game);
    g_present.game = NULL, g_present.keys = 0;
    if (!g_present.up) {
        memset(&g_present, 0, sizeof g_present);
        return; /* a headless run keeps its signal handlers until the process ends */
    }
    write_pending(true);
    /* The open recording is handed to the GPU before anything its commands name is freed: vkDeviceWaitIdle below
       waits for what was submitted, not for that recording, and the buffers, pipelines and layouts freed under it
       would leave the GPU reading memory that is gone (a gfxhub page fault and a ring reset at the run's end) */
    (void)snx_ctr_vk_finish();
    const SnxCtrVk* v = snx_ctr_vk();
    vkDeviceWaitIdle(v->device);
    for (unsigned screen = 0; screen < 2; ++screen)
        if (g_present.swap[screen].swapchain) swap_destroy(screen);
    for (unsigned i = 0; i < 2; ++i) frame_free(&g_present.frames[i]);
    frame_free(&g_present.source_dummy);
    for (unsigned i = 0; i < 2; ++i) snx_fx_destroy(g_present.fx[i]);
    for (unsigned screen = 0; screen < 2; ++screen) snx_vk_notice_destroy(g_present.notice_vk[screen]);
    snx_vk_rects_destroy(g_present.controls);
    for (unsigned screen = 0; screen < 2; ++screen) {
        for (unsigned i = 0; i < SNX_CTR_VK_SLOTS; ++i)
            if (g_present.acquire[screen][i]) vkDestroySemaphore(v->device, g_present.acquire[screen][i], NULL);
        for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
            if (g_present.presented[screen][i]) vkDestroySemaphore(v->device, g_present.presented[screen][i], NULL);
    }
    if (g_present.readback_view) vkUnmapMemory(v->device, g_present.readback_memory);
    if (g_present.readback) vkDestroyBuffer(v->device, g_present.readback, NULL);
    if (g_present.readback_memory) vkFreeMemory(v->device, g_present.readback_memory, NULL);
    snx_ctr_vk_compute_free(&g_present.decode);
    snx_ctr_vk_transfer_free();
    snx_ctr_vk_y2r_free();
    memset(&g_present, 0, sizeof g_present);
    snx_ctr_vk_free();
}
