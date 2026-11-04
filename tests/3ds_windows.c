/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The 3DS's two windows and its per-screen render scales (present.c, vk/device.c). A run whose window the
   host cannot make, as CI's, checks that the setting costs nothing and the dumps stay the framebuffer's own
   size. A run with a window checks that Separate screens gives each screen its own swapchain (--separate), and that
   a window minimized and restored gets its swapchain back (--minimize). */
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

/* an RGBA8 (A, B, G, R) framebuffer of one colour, with its top-left screen pixel white and the rest as given */
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

/* the PPM's two sizes, as it wrote them; the dumps' index counts both screens, so either 00 or 01 */
static int ppm_size(const char* dir, unsigned screen, unsigned* a, unsigned* b)
{
    for (unsigned index = 0; index < 8; ++index) {
        char path[1024];
        snprintf(path, sizeof path, "%s/frame-%u-%02u.ppm", dir, screen, index);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        int read = fscanf(f, "P6 %u %u 255", a, b) == 2;
        fclose(f);
        if (read) return 1;
    }
    return 0;
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

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : ".";
    bool separate = argc > 2 && !strcmp(argv[2], "--separate");
    bool minimized = argc > 2 && !strcmp(argv[2], "--minimize");
    bool interp = argc > 2 && !strcmp(argv[2], "--interp");
    char stale[1024];
    if (snx_mkdirs(dir)) return 1;
    for (unsigned screen = 0; screen < 2; ++screen)
        for (unsigned index = 0; index < 8; ++index) {
            snprintf(stale, sizeof stale, "%s/frame-%u-%02u.ppm", dir, screen, index);
            remove(stale);
        }
    /* two screens at different scales: each setting reaches its own screen's render scale, and no other's */
    if (interp) snprintf(g_settings.frame_interpolation, sizeof g_settings.frame_interpolation, "blend");
    g_settings.ctr_top_resolution = 2;
    g_settings.ctr_bottom_resolution = 1;
    bool scales = snx_ctr_vk_target_scale(240, 400) == 2 && snx_ctr_vk_target_scale(240, 320) == 1;
    g_settings.ctr_top_resolution = 1;
    g_settings.ctr_bottom_resolution = 2;
    scales = scales && snx_ctr_vk_target_scale(240, 400) == 1 && snx_ctr_vk_target_scale(240, 320) == 2;
    if (interp) snprintf(g_settings.frame_interpolation, sizeof g_settings.frame_interpolation, "blend");
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
    if (!snx_ctr_present_init(m, dir)) {
        printf("no Vulkan device: presentation checks skipped\n");
        snx_ctr_memory_free(m);
        return 77;
    }
    if ((separate || minimized) && !snx_ctr_present_windowed()) {
        printf("no window on this host: %s cannot be checked here\n",
               separate ? "the separate windows" : "a minimized window");
        snx_ctr_present_free();
        snx_ctr_memory_free(m);
        return 77;
    }
    if (minimized) {
        int result = minimize_restore();
        snx_ctr_present_free();
        snx_ctr_memory_free(m);
        return result;
    }
    bool ok = snx_ctr_present_separate() == separate;
    ok = ok && snx_ctr_present_frame(0, TOP, 240, 400, 0, 240 * 4, NULL);
    ok = ok && snx_ctr_present_frame(1, BOTTOM, 240, 320, 0, 240 * 4, NULL);
    snx_ctr_present_free();
    unsigned top[2] = {0}, bottom[2] = {0};
    /* the readback stays the framebuffer's own size whatever the render scale (present.comp says so) */
    ok = ok && ppm_size(dir, 0, &top[0], &top[1]) && top[0] == 400 && top[1] == 240;
    ok = ok && ppm_size(dir, 1, &bottom[0], &bottom[1]) && bottom[0] == 320 && bottom[1] == 240;
    printf("%-24s %s: top %ux%u, bottom %ux%u\n", separate ? "separate screens" : "one window",
           ok ? "ok" : "FAIL", top[0], top[1], bottom[0], bottom[1]);
    snx_ctr_memory_free(m);
    return ok ? 0 : 1;
}
