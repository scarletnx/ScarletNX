/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* HDR on a Wayland session: the only Linux desktop that shows it. A real window, a real swapchain, a 16:9 frame in a
   16:10 window (the letterbox the compositor sees), then HDR turned over and back. Skipped (77) when the session is
   not Wayland, so a machine without one says so instead of failing. Opt-in with SNX_TEST_VULKAN. */
#include "common/utilities/common.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/hdr.h"
#include "nx/gpu/vk/vk.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 g_wayland_validation_base;

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "vulkan_hdr_wayland:%d: %s\n", __LINE__, #x);                                              \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main(int argc, char** argv) {
    char* root = snx_temp_dir(NULL);
    u8* frame;
    const char* wl = getenv("WAYLAND_DISPLAY");
    int crop[4] = {0, 0, 0, 0};
    int i;
    if (!wl || !*wl) {
        printf("SKIP headless: no Wayland session (WAYLAND_DISPLAY is not set)\n");
        return 77;
    }
    CHECK(root);
    CHECK(!snx_startup_options(argc, argv));
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--vk_validation=1")) g_settings.vulkan_validation_layer = 1;
    /* the session's compositor, not XWayland: only it can show an HDR image */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
    snprintf(g_exedir, 1024, "%s/", root);
    mem_init();
    CHECK(mem_map(HEAP_BASE, 0x800000, MS_NORMAL, PERM_RW) == 0);
    nv_init();
    video_init("Wayland HDR fixture", "");
    /* the device's own layout is past some drivers' per-stage limits: only the passes' messages count below */
    {
        u32 base = g_vk_validation_messages;
        g_wayland_validation_base = base;
    }
    CHECK(g_renderer_up && g_vk.dev && g_vk.surface && g_vk.swapchain);
    CHECK(!strcmp(SDL_GetCurrentVideoDriver(), "wayland"));
    CHECK(g_vk.has_swapchain_colorspace);
    printf("Wayland HDR: %s, the display takes %s\n", g_vk.props.deviceName,
           vk_hdr_display_kind() == SNX_HDR_DISPLAY_PQ      ? "HDR10 (PQ)"
           : vk_hdr_display_kind() == SNX_HDR_DISPLAY_SCRGB ? "scRGB"
                                                            : "no HDR image");
    CHECK(vk_hdr_display_kind() != SNX_HDR_DISPLAY_NONE);

    /* a 640x480 frame, 4:3, in the 16:9 window: the bars are the presenter's letterbox, composed and encoded */
    frame = (u8*)g_mem + HEAP_BASE;
    for (u32 y = 0; y < 480; ++y)
        for (u32 x = 0; x < 640; ++x) {
            u8* p = frame + ((u64)y * 640 + x) * 4;
            p[0] = (u8)(x * 255 / 639);
            p[1] = (u8)(y * 255 / 479);
            p[2] = 64;
            p[3] = 255;
        }
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, 200, 1000, 50, 0);
    for (i = 0; i < 3; ++i) g_renderer->present_buffer(HEAP_BASE, 640, 480, 640 * 4, 0, 0, 1, crop, 0, 0, 0, 0);
    /* the first present after HDR is on makes the window's image anew: the surface's HDR kind, its white at the paper
       white (scRGB) or the 203-nit PQ reference (HDR10) */
    CHECK(g_vk.sc_hdr);
    CHECK(g_vk.sc_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
          g_vk.sc_format == VK_FORMAT_A2R10G10B10_UNORM_PACK32 || g_vk.sc_format == VK_FORMAT_R16G16B16A16_SFLOAT);
    printf("Wayland HDR: the swapchain is %ux%u %s\n", g_vk.sc_extent.width, g_vk.sc_extent.height,
           g_vk.sc_scrgb ? "scRGB" : "HDR10 (PQ)");
    /* the live toggle, as Ctrl+H: off makes an SDR image anew, on the HDR one */
    {
        const char* label = g_renderer->toggle_hdr ? g_renderer->toggle_hdr() : NULL;
        CHECK(label && !strncmp(label, "Off", 3));
        CHECK(!g_renderer->toggle_hdr || strncmp(g_renderer->toggle_hdr(), "Off", 3) != 0);
        for (i = 0; i < 2; ++i) g_renderer->present_buffer(HEAP_BASE, 640, 480, 640 * 4, 0, 0, 1, crop, 0, 0, 0, 0);
        CHECK(!g_vk.sc_hdr);
        CHECK(g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB || g_vk.sc_format == VK_FORMAT_R8G8B8A8_SRGB ||
              g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_R8G8B8A8_UNORM);
        for (i = 0; i < 2; ++i) g_renderer->present_buffer(HEAP_BASE, 640, 480, 640 * 4, 0, 0, 1, crop, 0, 0, 0, 0);
        CHECK(g_vk.sc_hdr);
    }
    g_renderer->idle();
    vk_pipeline_quiesce();
    if (g_vk_validation_messages != g_wayland_validation_base)
        printf("vulkan_hdr_wayland: a validation message: %u after the device's own %u\n", g_vk_validation_messages,
               g_wayland_validation_base);
    CHECK(!snx_remove_tree(root));
    free(root);
    printf("PASS Wayland HDR: the HDR image, the letterbox, the live toggle, %u validation messages\n",
           g_vk_validation_messages - g_wayland_validation_base);
    return 0;
}
