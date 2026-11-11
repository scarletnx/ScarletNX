/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's own HDR output, each step on values of the test's own: what a window's frames hold by the format of its
   textures, as the Ounce's NVN driver decides it (gpu/nvn/nvn_obj.h nvn_window_space), and the swapchain image that
   takes each kind as it is from the formats a surface lists, and when a game is told the display is HDR
   (gpu/vk/hdr_swapchain.h) */
#include "nx/gpu/nvn/nvn_obj.h"
#include "nx/gpu/vk/hdr_swapchain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "hdr_output.c:%d: check failed: %s\n", __LINE__, #cond);                                   \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

#define API_OUNCE (231u << 16)
/* every format the Ounce's driver makes a window of, with the color space it gives the native window (its
   NativeWindow::Initialize: 0 for the 8-bit ones, 1 for RGB10A2, 2 with a unit of 80 nits for the half floats) */
static void window_spaces(void) {
    static const struct {
        i32 format;
        u32 space;
    } rows[] = {{NVN_FORMAT_RGBA8, SNX_HDR_SPACE_SRGB},   {NVN_FORMAT_RGBA8_SRGB, SNX_HDR_SPACE_SRGB},
                {NVN_FORMAT_RGBX8, SNX_HDR_SPACE_SRGB},   {NVN_FORMAT_RGBX8_SRGB, SNX_HDR_SPACE_SRGB},
                {NVN_FORMAT_BGRA8, SNX_HDR_SPACE_SRGB},   {NVN_FORMAT_BGRA8_SRGB, SNX_HDR_SPACE_SRGB},
                {NVN_FORMAT_RGB10A2, SNX_HDR_SPACE_PQ},   {NVN_FORMAT_RGBA16F, SNX_HDR_SPACE_SCRGB},
                {NVN_FORMAT_RGBX16F, SNX_HDR_SPACE_SCRGB}};
    for (u32 i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        CHECK(nvn_window_space(API_OUNCE, rows[i].format) == rows[i].space);
        CHECK(nvn_window_space(API_OUNCE + 7, rows[i].format) == rows[i].space);
        /* an older driver has no HDR window: its RGB10A2 and half float textures are not PQ or scRGB */
        CHECK(nvn_window_space(API_OUNCE - 1, rows[i].format) == SNX_HDR_SPACE_SRGB);
        CHECK(nvn_window_space(55u << 16 | 12, rows[i].format) == SNX_HDR_SPACE_SRGB);
    }
    /* the numbers the driver's table is indexed by */
    CHECK(NVN_FORMAT_RGB10A2 == 0x3D && NVN_FORMAT_RGBA16F == 0x29 && NVN_FORMAT_RGBX16F == 0x71);
    /* formats it makes no window of, and the 10-bit ones beside RGB10A2 */
    CHECK(nvn_window_space(API_OUNCE, NVN_FORMAT_RGB10A2UI) == SNX_HDR_SPACE_SRGB);
    CHECK(nvn_window_space(API_OUNCE, NVN_FORMAT_RGBA32F) == SNX_HDR_SPACE_SRGB);
    CHECK(nvn_window_space(API_OUNCE, NVN_FORMAT_R11G11B10F) == SNX_HDR_SPACE_SRGB);
    CHECK(nvn_window_space(API_OUNCE, 0) == SNX_HDR_SPACE_SRGB);
    CHECK(SNX_HDR_SCRGB_UNIT == 80);
}

#define SDR   VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
#define PQ    VK_COLOR_SPACE_HDR10_ST2084_EXT
#define SCRGB VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
static int pick(u32 space, const VkSurfaceFormatKHR* formats, u32 n, VkFormat format, VkColorSpaceKHR color_space) {
    VkSurfaceFormatKHR got = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_MAX_ENUM_KHR};
    if (!vk_hdr_surface_format(space, formats, n, &got))
        return format == VK_FORMAT_UNDEFINED && got.format == VK_FORMAT_UNDEFINED; /* refused: *out untouched */
    return got.format == format && got.colorSpace == color_space;
}
/* each space's Vulkan color space and formats, on the lists of an HDR desktop, of surfaces with one kind alone, and of
   an SDR one */
static void surface_formats(void) {
    static const VkSurfaceFormatKHR desktop[] = {{VK_FORMAT_B8G8R8A8_UNORM, SDR},
                                                 {VK_FORMAT_B8G8R8A8_SRGB, SDR},
                                                 {VK_FORMAT_R16G16B16A16_SFLOAT, PQ},
                                                 {VK_FORMAT_A2R10G10B10_UNORM_PACK32, PQ},
                                                 {VK_FORMAT_A2B10G10R10_UNORM_PACK32, PQ},
                                                 {VK_FORMAT_A2B10G10R10_UNORM_PACK32, SDR},
                                                 {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_BT2020_LINEAR_EXT},
                                                 {VK_FORMAT_R16G16B16A16_SFLOAT, SCRGB},
                                                 {VK_FORMAT_R16G16B16A16_SFLOAT, SDR}};
    static const VkSurfaceFormatKHR bgr10[] = {{VK_FORMAT_B8G8R8A8_UNORM, SDR},
                                               {VK_FORMAT_A2R10G10B10_UNORM_PACK32, PQ}};
    static const VkSurfaceFormatKHR floats[] = {{VK_FORMAT_R16G16B16A16_SFLOAT, PQ},
                                                {VK_FORMAT_R16G16B16A16_SFLOAT, SCRGB}};
    static const VkSurfaceFormatKHR sdr[] = {{VK_FORMAT_B8G8R8A8_UNORM, SDR},
                                             {VK_FORMAT_A2B10G10R10_UNORM_PACK32, SDR},
                                             {VK_FORMAT_R16G16B16A16_SFLOAT, SDR}};
    static const VkSurfaceFormatKHR wide[] = {
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_HLG_EXT},
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT}};
    /* PQ: the frame's own 10 bits before the other channel order, and half floats last */
    CHECK(pick(SNX_HDR_SPACE_PQ, desktop, 9, VK_FORMAT_A2B10G10R10_UNORM_PACK32, PQ));
    CHECK(pick(SNX_HDR_SPACE_PQ, bgr10, 2, VK_FORMAT_A2R10G10B10_UNORM_PACK32, PQ));
    CHECK(pick(SNX_HDR_SPACE_PQ, floats, 2, VK_FORMAT_R16G16B16A16_SFLOAT, PQ));
    /* scRGB: half floats in the extended linear sRGB space alone, never BT.2020's or sRGB's own */
    CHECK(pick(SNX_HDR_SPACE_SCRGB, desktop, 9, VK_FORMAT_R16G16B16A16_SFLOAT, SCRGB));
    CHECK(pick(SNX_HDR_SPACE_SCRGB, floats, 2, VK_FORMAT_R16G16B16A16_SFLOAT, SCRGB));
    CHECK(pick(SNX_HDR_SPACE_SCRGB, bgr10, 2, VK_FORMAT_UNDEFINED, 0));
    /* a 10-bit or float format in sRGB's space is no HDR image, nor is another HDR space the frame's */
    CHECK(pick(SNX_HDR_SPACE_PQ, sdr, 3, VK_FORMAT_UNDEFINED, 0));
    CHECK(pick(SNX_HDR_SPACE_SCRGB, sdr, 3, VK_FORMAT_UNDEFINED, 0));
    CHECK(pick(SNX_HDR_SPACE_PQ, wide, 3, VK_FORMAT_UNDEFINED, 0));
    CHECK(pick(SNX_HDR_SPACE_SCRGB, wide, 3, VK_FORMAT_UNDEFINED, 0));
    CHECK(pick(SNX_HDR_SPACE_PQ, NULL, 0, VK_FORMAT_UNDEFINED, 0));
    /* an SDR frame's image is not this choice */
    CHECK(pick(SNX_HDR_SPACE_SRGB, desktop, 9, VK_FORMAT_UNDEFINED, 0));
    CHECK(pick(SNX_HDR_SPACES, desktop, 9, VK_FORMAT_UNDEFINED, 0));
    /* the game is told of HDR before it picks a kind: the surface has to take both */
    CHECK(vk_hdr_surface_takes(desktop, 9) && vk_hdr_surface_takes(floats, 2));
    CHECK(!vk_hdr_surface_takes(bgr10, 2) && !vk_hdr_surface_takes(sdr, 3) && !vk_hdr_surface_takes(wide, 3));
    CHECK(!vk_hdr_surface_takes(desktop, 7)); /* without its scRGB row */
    CHECK(!vk_hdr_surface_takes(NULL, 0));
}

/* a game is told the display is HDR with the setting on, both extensions and a surface of both kinds, and only then:
   each one missing is named */
static void display_rule(void) {
    static const VkSurfaceFormatKHR hdr[] = {{VK_FORMAT_B8G8R8A8_UNORM, SDR},
                                             {VK_FORMAT_A2B10G10R10_UNORM_PACK32, PQ},
                                             {VK_FORMAT_R16G16B16A16_SFLOAT, SCRGB}};
    const char* why = "unset";
    CHECK(vk_hdr_display(1, 1, 1, hdr, 3, &why) && !why);
    CHECK(!vk_hdr_display(0, 1, 1, hdr, 3, &why) && !strcmp(why, "hdr_game=0"));
    CHECK(!vk_hdr_display(1, 0, 1, hdr, 3, &why) && strstr(why, "VK_EXT_swapchain_colorspace"));
    CHECK(!vk_hdr_display(1, 1, 0, hdr, 3, &why) && strstr(why, "VK_EXT_hdr_metadata"));
    CHECK(!vk_hdr_display(1, 1, 1, hdr, 1, &why) && strstr(why, "HDR10"));
    CHECK(!vk_hdr_display(1, 1, 1, hdr, 2, &why) && strstr(why, "scRGB"));
    CHECK(!vk_hdr_display(1, 1, 1, NULL, 0, &why) && strstr(why, "HDR10"));
    /* off whatever the display has: the default */
    CHECK(!vk_hdr_display(0, 0, 0, NULL, 0, &why) && !strcmp(why, "hdr_game=0"));
}

int main(void) {
    window_spaces();
    surface_formats();
    display_rule();
    printf("hdr output: %d checks\n", checks);
    return 0;
}
