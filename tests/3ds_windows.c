/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The 3DS's two windows and its per-screen render scales (present.c, vk/device.c). A run whose window the
   host cannot make, as CI's, checks that the setting costs nothing and the dumps stay the framebuffer's own
   size. A run with a window checks that Separate screens gives each screen its own swapchain (--separate), that
   a window minimized and restored gets its swapchain back (--minimize), that a frame never waits for an image of
   the window while the display holds them all (--no-image), and that --headless starts no SDL video and still decodes,
   dumps and saves both screens (--headless). */
#include "3ds/gpu/present.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/memory/ctr_memory.h"
#include "common/utilities/common.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FCRAM 0x20000000u
#define TOP (FCRAM + 0x00100000u)
#define BOTTOM (FCRAM + 0x00400000u)
#define PHASE_FRAMES 4 /* each screen's frames while the window is minimized, and again once it is restored */

/* --no-image's stand-ins for the driver's acquire and present, and what they saw */
static struct {
    PFN_vkAcquireNextImageKHR acquire;
    PFN_vkQueuePresentKHR present;
    uint64_t longest; /* the longest wait for an image asked, in ns */
    unsigned acquires, presents;
} g_windows;

/* an RGBA8 (A, B, G, R) framebuffer of one colour, with its first framebuffer pixel white and the rest as given */
static void fill(SnxCtrMemory* m, uint32_t at, unsigned width, unsigned height, unsigned char alpha)
{
    unsigned char* pixels = malloc((size_t)width * height * 4);
    if (!pixels) exit(1);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            unsigned char* px = pixels + ((size_t)y * width + x) * 4;
            px[0] = 255;
            px[1] = (unsigned char)(y + 0x40);
            px[2] = (unsigned char)(x + 0x20);
            px[3] = (unsigned char)(x == 0 && y == 0 ? 0xff : alpha); /* A, then B, G */
        }
    if (snx_ctr_phys_write(m, at, pixels, (size_t)width * height * 4) != SNX_CTR_MEM_OK) exit(1);
    free(pixels);
}

/* the PPM's two sizes and its maximum, which must be 255 */
static int ppm_file_size(const char* path, unsigned* a, unsigned* b) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned maxval = 0;
    int read = fscanf(f, "P6 %u %u %u", a, b, &maxval) == 3 && maxval == 255;
    fclose(f);
    return read;
}

/* the dumps' index counts both screens, so either 00 or 01 */
static int ppm_size(const char* dir, unsigned screen, unsigned* a, unsigned* b)
{
    for (unsigned index = 0; index < 8; ++index) {
        char path[1024];
        snprintf(path, sizeof path, "%s/frame-%u-%02u.ppm", dir, screen, index);
        if (ppm_file_size(path, a, b)) return 1;
    }
    return 0;
}

/* one pixel of a P6 PPM, after its header */
static int ppm_pixel(const char* path, unsigned x, unsigned y, unsigned char rgb[3]) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned width = 0, height = 0, maxval = 0;
    int read = fscanf(f, "P6 %u %u %u", &width, &height, &maxval) == 3 && maxval == 255 && x < width && y < height &&
               fgetc(f) != EOF && fseek(f, (long)(((size_t)y * width + x) * 3), SEEK_CUR) == 0 &&
               fread(rgb, 1, 3, f) == 3;
    fclose(f);
    return read;
}

/* one frame of each screen */
static bool present_pair(void) {
    return snx_ctr_present_frame(0, TOP, 240, 400, 0, 240 * 4, NULL) &&
           snx_ctr_present_frame(1, BOTTOM, 240, 320, 0, 240 * 4, NULL);
}

/* Frames go on while the window is minimized, then it is restored: it must have a swapchain again. The surface's
   size and smallest size while minimized are printed: 0x0 on Windows, where no swapchain can be made for it. A host
   that does not minimize the window, or keeps its swapchain while it is minimized, has nothing to check here. */
static int minimize_restore(void) {
    const SnxCtrVk* v = snx_ctr_vk();
    SDL_Window* window = v->host.window;
    bool ok = present_pair();
    SDL_MinimizeWindow(window);
    SDL_SyncWindow(window);
    for (unsigned i = 0; i < PHASE_FRAMES; ++i) ok = ok && present_pair();
    VkSurfaceCapabilitiesKHR caps = {0};
    ok = ok && vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v->phys, v->host.surface, &caps) == VK_SUCCESS;
    if (ok && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
        printf("the window did not minimize on this host: nothing to check\n");
        return 77;
    }
    if (ok && snx_ctr_present_windowed()) {
        printf("the swapchain outlived the minimize on this host (surface %ux%u): nothing to check\n",
               caps.currentExtent.width, caps.currentExtent.height);
        return 77;
    }
    SDL_RestoreWindow(window);
    SDL_SyncWindow(window);
    for (unsigned i = 0; i < PHASE_FRAMES; ++i) ok = ok && present_pair();
    ok = ok && snx_ctr_present_windowed();
    printf("%-24s %s: the surface was %ux%u, at least %ux%u, while minimized\n", "minimized and restored",
           ok ? "ok" : "FAIL", caps.currentExtent.width, caps.currentExtent.height, caps.minImageExtent.width,
           caps.minImageExtent.height);
    return ok ? 0 : 1;
}

/* A driver's acquire while the display holds every image of the window (FIFO behind, a locked or remote session):
   none now, at once to a call that asks not to wait, and to one that waits once its time is out. A real one stops the
   game that long, for as long as the display keeps them with no limit */
static VKAPI_ATTR VkResult VKAPI_CALL held_acquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                   VkSemaphore semaphore, VkFence fence, uint32_t* index) {
    (void)device, (void)swapchain, (void)semaphore, (void)fence, (void)index;
    ++g_windows.acquires;
    if (timeout > g_windows.longest) g_windows.longest = timeout;
    return timeout ? VK_TIMEOUT : VK_NOT_READY;
}
static VKAPI_ATTR VkResult VKAPI_CALL counted_present(VkQueue queue, const VkPresentInfoKHR* info) {
    ++g_windows.presents;
    return g_windows.present(queue, info);
}

/* Frames while the display holds every image of the window: none waits for one or shows, and each is still decoded,
   its hash the one it has when shown. Once an image is free the frames show again */
static int no_image(SnxCtrMemory* m) {
    uint64_t shown = 0, held = 0;
    g_windows.present = vkQueuePresentKHR;
    vkQueuePresentKHR = counted_present;
    bool ok = snx_ctr_present_frame(0, TOP, 240, 400, 0, 240 * 4, &shown) && g_windows.presents == 1;
    g_windows.acquire = vkAcquireNextImageKHR;
    vkAcquireNextImageKHR = held_acquire;
    for (unsigned i = 0; i < PHASE_FRAMES; ++i) ok = ok && present_pair();
    fill(m, TOP, 240, 400, 0x40); /* a new frame, so a decode that is skipped cannot keep the old hash */
    ok = ok && snx_ctr_present_frame(0, TOP, 240, 400, 0, 240 * 4, &held);
    unsigned acquires = g_windows.acquires, before = g_windows.presents;
    vkAcquireNextImageKHR = g_windows.acquire;
    ok = ok && present_pair() && g_windows.presents == before + 1;
    vkQueuePresentKHR = g_windows.present;
    bool waited = g_windows.longest != 0;
    ok = ok && acquires == PHASE_FRAMES + 1 && before == 1 && held != shown && !waited;
    printf("%-24s %s: %u frames found no image, asked to wait %llu ns at most, %u presented, hash %s, %u presented "
           "after\n",
           "no image free", ok ? "ok" : "FAIL", acquires, (unsigned long long)g_windows.longest, before - 1,
           held != shown ? "a new frame's" : "the shown frame's", g_windows.presents - before);
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : ".";
    bool separate = argc > 2 && !strcmp(argv[2], "--separate");
    bool minimized = argc > 2 && !strcmp(argv[2], "--minimize");
    bool held = argc > 2 && !strcmp(argv[2], "--no-image");
    bool headless = argc > 2 && !strcmp(argv[2], "--headless");
    if (argc > 2 && !separate && !minimized && !held && !headless) {
        fprintf(stderr, "snx-test-3ds-windows: %s is not a mode\n", argv[2]);
        return 2;
    }
    char stale[1024];
    if (snx_mkdirs(dir)) return 1;
    for (unsigned screen = 0; screen < 2; ++screen)
        for (unsigned index = 0; index < 8; ++index) {
            snprintf(stale, sizeof stale, "%s/frame-%u-%02u.ppm", dir, screen, index);
            remove(stale);
        }
    /* two screens at different scales: each setting reaches its own screen's render scale, and no other's */
    g_settings.ctr_top_resolution = 2;
    g_settings.ctr_bottom_resolution = 1;
    bool scales = snx_ctr_vk_target_scale(240, 400) == 2 && snx_ctr_vk_target_scale(240, 320) == 1;
    g_settings.ctr_top_resolution = 1;
    g_settings.ctr_bottom_resolution = 2;
    scales = scales && snx_ctr_vk_target_scale(240, 400) == 1 && snx_ctr_vk_target_scale(240, 320) == 2;
    g_settings.ctr_top_resolution = 2;
    g_settings.ctr_bottom_resolution = 1;
    if (!scales) {
        printf("the per-screen render scales do not follow their settings\n");
        return 1;
    }
    SnxCtrMemory* m = snx_ctr_memory_new(SNX_CTR_OLD);
    if (!m) return 1;
    fill(m, TOP, 240, 400, 0x80);
    fill(m, BOTTOM, 240, 320, 0x80);
    /* the frame the window shows is the target's, the dumps the framebuffer's */
    g_settings.ctr_separate_screens = separate ? 1 : 0;
    if (!snx_ctr_present_init(m, dir, headless)) {
        printf("no Vulkan device: presentation checks skipped\n");
        snx_ctr_memory_free(m);
        return 77;
    }
    if ((separate || minimized || held) && !snx_ctr_present_windowed()) {
        printf("no window on this host: %s cannot be checked here\n", separate    ? "the separate windows"
                                                                      : minimized ? "a minimized window"
                                                                                  : "a window with no image free");
        snx_ctr_present_free();
        snx_ctr_memory_free(m);
        return 77;
    }
    if (separate && !(snx_ctr_vk()->second && snx_ctr_vk()->second_surface)) {
        printf("the device presents to one window only: the separate windows cannot be checked here\n");
        snx_ctr_present_free();
        snx_ctr_memory_free(m);
        return 77;
    }
    if (minimized || held) {
        int result = minimized ? minimize_restore() : no_image(m);
        snx_ctr_present_free();
        snx_ctr_memory_free(m);
        return result;
    }
    bool ok = snx_ctr_present_separate() == separate;
    if (headless)
        ok = ok && !snx_ctr_present_windowed() && !snx_ctr_vk()->host.window && !snx_ctr_vk()->host.surface &&
             !snx_ctr_vk()->second && !snx_ctr_vk()->second_surface && !SDL_WasInit(SDL_INIT_VIDEO);
    ok = ok && snx_ctr_present_frame(0, TOP, 240, 400, 0, 240 * 4, NULL);
    ok = ok && snx_ctr_present_frame(1, BOTTOM, 240, 320, 0, 240 * 4, NULL);
    uint64_t frames = snx_ctr_present_frames();
    char shot[1024] = {0};
    unsigned shot_w = 0, shot_h = 0;
    if (headless) {
        snprintf(shot, sizeof shot, "%s/shot.ppm", dir);
        remove(shot);
        ok = ok && snx_ctr_present_shot(shot);
    }
    snx_ctr_present_free();
    if (headless) {
        unsigned char top_px[3] = {0}, bottom_px[3] = {0}, margin[3] = {0};
        unsigned top[2] = {0}, bottom[2] = {0};
        ok = ok && frames == 1 && ppm_file_size(shot, &shot_w, &shot_h) && shot_w == 400 && shot_h == 480;
        ok = ok && ppm_pixel(shot, 200, 120, top_px) && (top_px[0] || top_px[1] || top_px[2]);
        ok = ok && ppm_pixel(shot, 200, 360, bottom_px) && (bottom_px[0] || bottom_px[1] || bottom_px[2]);
        ok = ok && ppm_pixel(shot, 0, 360, margin) && !margin[0] && !margin[1] && !margin[2];
        ok = ok && ppm_size(dir, 0, &top[0], &top[1]) && top[0] == 400 && top[1] == 240;
        ok = ok && ppm_size(dir, 1, &bottom[0], &bottom[1]) && bottom[0] == 320 && bottom[1] == 240;
        printf("%-24s %s: %llu frame, shot %ux%u, dumps %ux%u and %ux%u\n", "headless", ok ? "ok" : "FAIL",
               (unsigned long long)frames, shot_w, shot_h, top[0], top[1], bottom[0], bottom[1]);
        snx_ctr_memory_free(m);
        return ok ? 0 : 1;
    }
    unsigned top[2] = {0}, bottom[2] = {0};
    /* the readback stays the framebuffer's own size whatever the render scale (present.comp says so) */
    ok = ok && ppm_size(dir, 0, &top[0], &top[1]) && top[0] == 400 && top[1] == 240;
    ok = ok && ppm_size(dir, 1, &bottom[0], &bottom[1]) && bottom[0] == 320 && bottom[1] == 240;
    printf("%-24s %s: top %ux%u, bottom %ux%u\n", separate ? "separate screens" : "one window",
           ok ? "ok" : "FAIL", top[0], top[1], bottom[0], bottom[1]);
    snx_ctr_memory_free(m);
    return ok ? 0 : 1;
}
