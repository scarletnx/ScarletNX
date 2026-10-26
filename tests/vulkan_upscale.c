/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The presented frame's spatial upscalers on a real Vulkan device, with no game data and no window: a synthetic
   checkerboard and step edge through FSR 1 at two sharpnesses, NIS and SGSR, read back and checked, and Auto's choice
   and ratio on the device's own GPU. Opt-in with SNX_TEST_VULKAN, as vulkan_headless is. */
#include "common/utilities/common.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/upscale.h"
#include "nx/gpu/vk/vk.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/backend.h"
#include "support/half.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "vulkan_upscale:%d: %s\n", __LINE__, #x);                                                  \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

typedef struct {
    VkImage img;
    VmaAllocation mem;
    VkImageView view;
    u32 w, h;
} TestImage;

static int image_make(TestImage* t, u32 w, u32 h) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!vk_image_create(&ii, &t->img, &t->mem)) return 0;
    vi.image = t->img;
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &t->view))) return 0;
    t->w = w;
    t->h = h;
    return 1;
}

static void image_free(TestImage* t) {
    if (t->img) vk_defer_image(t->img, t->mem, t->view, VK_NULL_HANDLE);
    memset(t, 0, sizeof *t);
}

/* the pattern: a vertical step at half width, a horizontal one at half height, a checker behind them, opaque */
static void pattern(u8* p, u32 w, u32 h) {
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u8* at = p + ((u64)y * w + x) * 4;
            at[0] = x < w / 2 ? 0 : 255;
            at[1] = y < h / 2 ? 0 : 255;
            at[2] = ((x / 8 + y / 8) & 1) ? 255 : 0;
            at[3] = 255;
        }
}

static int upload(TestImage* t) {
    const u64 size = (u64)t->w * t->h * 4;
    VkBuffer buf;
    u64 off;
    u8* p = vk_stage(size, 16, &buf, &off);
    VkBufferImageCopy c = {.bufferOffset = off,
                           .bufferRowLength = t->w,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {t->w, t->h, 1}};
    if (!p) return 0;
    pattern(p, t->w, t->h);
    vk_barrier(vk_cmd(), t->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rec_CmdCopyBufferToImage(vk_cmd(), buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    vk_barrier(vk_cmd(), t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_SAMPLED_STAGES,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return 1;
}

/* the upscaler's image as floats, 0 when it could not run or read */
static int run_read_at(TestImage* src, u32 sx, u32 sy, u32 sw, u32 sh, u32 dw, u32 dh, u32 transform, int filter,
                       int sharpness, float** out, u32* out_w, u32* out_h) {
    VkImage img = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat fmt = vk_upscale_format();
    u32 bpp = fmt == VK_FORMAT_R8G8B8A8_UNORM ? 4 : 8;
    VkBuffer rb;
    u8* data;
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {dw, dh, 1}};
    float* values;
    if (!vk_upscale_process(vk_cmd(), src->view, src->w, src->h, sx, sy, sw, sh, transform, dw, dh, filter, sharpness,
                            &img, &view))
        return 0;
    data = vk_readback_buffer((u64)dw * dh * bpp, &rb);
    if (!data) return 0;
    rec_CmdCopyImageToBuffer(vk_cmd(), img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &copy);
    vk_finish();
    vk_readback_ready();
    values = malloc((size_t)dw * dh * 4 * sizeof(float));
    if (!values) return 0;
    for (u32 i = 0; i < dw * dh; ++i)
        if (bpp == 8) {
            const u16* h = (const u16*)data + (u64)i * 4;
            values[i * 4 + 0] = half_to_float(h[0]);
            values[i * 4 + 1] = half_to_float(h[1]);
            values[i * 4 + 2] = half_to_float(h[2]);
            values[i * 4 + 3] = half_to_float(h[3]);
        } else {
            const u8* b = data + (u64)i * 4;
            values[i * 4 + 0] = b[0] / 255.0f;
            values[i * 4 + 1] = b[1] / 255.0f;
            values[i * 4 + 2] = b[2] / 255.0f;
            values[i * 4 + 3] = b[3] / 255.0f;
        }
    vk_frame_next();
    *out = values;
    *out_w = dw;
    *out_h = dh;
    return 1;
}
static int run_read(TestImage* src, u32 sw, u32 sh, u32 dw, u32 dh, int filter, int sharpness, float** out, u32* out_w,
                    u32* out_h) {
    return run_read_at(src, 0, 0, sw, sh, dw, dh, 0, filter, sharpness, out, out_w, out_h);
}

static float luma(const float* p) { return p[0] * 0.25f + p[1] * 0.5f + p[2] * 0.25f; }

/* the sum of the absolute differences across the vertical edge at the middle */
static double edge_energy(const float* px, u32 w, u32 h) {
    double sum = 0.0;
    u32 x = w / 2;
    for (u32 y = 0; y < h; ++y) {
        const float* a = px + ((u64)y * w + x - 1) * 4;
        const float* b = px + ((u64)y * w + x) * 4;
        sum += fabs(luma(b) - luma(a));
    }
    return sum;
}
/* every adjacent pair's difference: how much detail the image keeps, which sharpening raises */
static double detail_energy(const float* px, u32 w, u32 h) {
    double sum = 0.0;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 1; x < w; ++x) sum += fabs(luma(px + ((u64)y * w + x) * 4) - luma(px + ((u64)y * w + x - 1) * 4));
    return sum;
}

static int check_filter(TestImage* src, int filter, u32 dw, u32 dh, float* sharp0, float* sharp100, u32* n0,
                        u32* n100) {
    float* px0 = NULL;
    float* px1 = NULL;
    u32 w0, h0, w1, h1;
    int ok;
    if (filter == SNX_UPSCALE_FSR1) {
        CHECK(run_read(src, src->w, src->h, dw, dh, filter, 0, &px0, &w0, &h0));
        CHECK(run_read(src, src->w, src->h, dw, dh, filter, 100, &px1, &w1, &h1));
        *sharp0 = (float)detail_energy(px0, w0, h0);
        *sharp100 = (float)detail_energy(px1, w1, h1);
        *n0 = w0;
        *n100 = h0;
        CHECK(w0 == dw && h0 == dh && w1 == dw && h1 == dh);
        /* RCAS at 0 is sharper than at 100: more detail survives at the same extent */
        CHECK(*sharp0 > *sharp100);
        free(px1);
    } else {
        CHECK(run_read(src, src->w, src->h, dw, dh, filter, 25, &px0, &w0, &h0));
        CHECK(w0 == dw && h0 == dh);
        CHECK(edge_energy(px0, w0, h0) > 0.0);
        *n0 = w0;
        *n100 = h0;
    }
    for (u32 i = 0; i < (u64)dw * dh; ++i) {
        CHECK(px0[i * 4 + 3] > 0.9f); /* opaque in, opaque out */
        CHECK(isfinite(px0[i * 4 + 0]) && isfinite(px0[i * 4 + 1]) && isfinite(px0[i * 4 + 2]));
    }
    free(px0);
    return 0;
}

int main(int argc, char** argv) {
    char* root = snx_temp_dir(NULL);
    TestImage src = {0};
    float s0 = 0, s100 = 0;
    u32 n0 = 0, n100 = 0;
    int auto_pick;
    CHECK(root);
    CHECK(!snx_startup_options(argc, argv));
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--vk_validation=1")) g_settings.vulkan_validation_layer = 1;
    snprintf(g_exedir, 1024, "%s/", root);
    mem_init();
    CHECK(mem_map(HEAP_BASE, 0x800000, MS_NORMAL, PERM_RW) == 0);
    nv_init();
    video_init("Headless Vulkan upscaler fixture", "");
    CHECK(g_opt_headless && g_renderer_up && g_vk.dev && !g_vk.surface && !g_vk.swapchain);

    vk_lock();
    CHECK(image_make(&src, 160, 90));
    CHECK(upload(&src));
    vk_finish();
    printf("Headless upscaler device: %s, output %s\n", g_vk.props.deviceName,
           vk_upscale_format() == VK_FORMAT_R8G8B8A8_UNORM ? "RGBA8" : "RGBA16F");

    /* a crop and vi's FlipH: red is dark left of the middle and bright right; 40..120 flipped puts bright on the
       left. NIS's viewport support shows the crop the same way without the flip, and SGSR mirrors its texcoord */
    {
        float* px = NULL;
        u32 w = 0, h = 0;
        CHECK(run_read_at(&src, 0, 0, 160, 90, 320, 180, 1, SNX_UPSCALE_FSR1, 25, &px, &w, &h));
        CHECK(w == 320 && h == 180 && px[((u64)(h / 2) * w + 10) * 4] > 0.8f &&
              px[((u64)(h / 2) * w + w - 10) * 4] < 0.2f);
        free(px);
        px = NULL;
        CHECK(run_read_at(&src, 0, 0, 160, 90, 160, 90, 1, SNX_UPSCALE_FSR1, 25, &px, &w, &h));
        CHECK(w == 160 && h == 90 && px[((u64)(h / 2) * w + 10) * 4] > 0.8f &&
              px[((u64)(h / 2) * w + w - 10) * 4] < 0.2f);
        free(px);
        px = NULL;
        CHECK(run_read_at(&src, 40, 20, 80, 45, 160, 90, 1, SNX_UPSCALE_FSR1, 25, &px, &w, &h));
        CHECK(w == 160 && h == 90 && px[((u64)(h / 2) * w + 10) * 4] > 0.8f &&
              px[((u64)(h / 2) * w + w - 10) * 4] < 0.2f);
        free(px);
        px = NULL;
        CHECK(run_read_at(&src, 40, 20, 80, 45, 160, 90, 0, SNX_UPSCALE_NIS, 25, &px, &w, &h));
        CHECK(w == 160 && h == 90 && px[((u64)(h / 2) * w + 10) * 4] < 0.2f &&
              px[((u64)(h / 2) * w + w - 10) * 4] > 0.8f);
        free(px);
        px = NULL;
        CHECK(run_read_at(&src, 0, 0, 160, 90, 240, 135, 1, SNX_UPSCALE_SGSR, 25, &px, &w, &h));
        CHECK(w == 240 && h == 135 && px[((u64)(h / 2) * w + 10) * 4] > 0.8f &&
              px[((u64)(h / 2) * w + w - 10) * 4] < 0.2f);
        free(px);
        px = NULL;
        CHECK(run_read_at(&src, 40, 20, 80, 45, 160, 90, 0, SNX_UPSCALE_SGSR, 25, &px, &w, &h));
        CHECK(w == 160 && h == 90 && px[((u64)(h / 2) * w + 10) * 4] < 0.2f &&
              px[((u64)(h / 2) * w + w - 10) * 4] > 0.8f);
        free(px);
        px = NULL;
        CHECK(run_read_at(&src, 40, 20, 80, 45, 160, 90, 1, SNX_UPSCALE_SGSR, 25, &px, &w, &h));
        CHECK(w == 160 && h == 90 && px[((u64)(h / 2) * w + 10) * 4] > 0.8f &&
              px[((u64)(h / 2) * w + w - 10) * 4] < 0.2f);
        free(px);
    }

    CHECK(!check_filter(&src, SNX_UPSCALE_FSR1, 320, 180, &s0, &s100, &n0, &n100));
    CHECK(s0 > 0.0f);
    printf("FSR 1: 160x90 -> 320x180, detail %.1f at sharpness 0 and %.1f at 100\n", s0, s100);
    CHECK(!check_filter(&src, SNX_UPSCALE_NIS, 320, 180, &s0, &s100, &n0, &n100));
    printf("NIS: 160x90 -> 320x180\n");
    CHECK(!check_filter(&src, SNX_UPSCALE_SGSR, 240, 135, &s0, &s100, &n0, &n100));
    printf("SGSR: 160x90 -> 240x135\n");

    /* Auto on this device, at 1:1 and at 1.5x; Bilinear and None are themselves */
    auto_pick = snx_upscale_auto_pick(g_vk.props.vendorID, g_vk.props.deviceID, g_vk.props.deviceName);
    vk_upscale_configure(SNX_UPSCALE_AUTO, 25);
    CHECK(vk_upscale_pick(1280, 720, 1280, 720, 0) == SNX_UPSCALE_OFF);
    CHECK(vk_upscale_pick(1280, 720, 1920, 1080, 0) == auto_pick);
    vk_upscale_configure(SNX_UPSCALE_OFF, 25);
    CHECK(vk_upscale_pick(1280, 720, 1920, 1080, 0) == SNX_UPSCALE_OFF);
    vk_upscale_configure(SNX_UPSCALE_BILINEAR, 25);
    CHECK(vk_upscale_pick(1280, 720, 1920, 1080, 0) == SNX_UPSCALE_BILINEAR);
    vk_upscale_configure(SNX_UPSCALE_FSR1, 25);
    CHECK(vk_upscale_pick(1280, 720, 1280, 720, 0) == SNX_UPSCALE_FSR1);
    CHECK(!vk_upscale_filter_uses_sharpness(SNX_UPSCALE_OFF));
    CHECK(!vk_upscale_filter_uses_sharpness(SNX_UPSCALE_BILINEAR));
    CHECK(vk_upscale_filter_uses_sharpness(SNX_UPSCALE_FSR1));

    /* the live cycle: FSR 1, Bilinear, NIS, SGSR, None, and round; Auto enters at FSR 1 */
    vk_upscale_configure(SNX_UPSCALE_AUTO, 25);
    CHECK(vk_upscale_cycle() == SNX_UPSCALE_FSR1);
    CHECK(vk_upscale_pick(1280, 720, 1920, 1080, 0) == SNX_UPSCALE_FSR1);
    CHECK(vk_upscale_cycle() == SNX_UPSCALE_BILINEAR);
    CHECK(vk_upscale_pick(1280, 720, 1920, 1080, 0) == SNX_UPSCALE_BILINEAR);
    CHECK(vk_upscale_cycle() == SNX_UPSCALE_NIS);
    CHECK(vk_upscale_pick(1280, 720, 1920, 1080, 0) == SNX_UPSCALE_NIS);
    CHECK(vk_upscale_cycle() == SNX_UPSCALE_SGSR);
    CHECK(vk_upscale_pick(1280, 720, 1920, 1080, 0) == SNX_UPSCALE_SGSR);
    CHECK(vk_upscale_cycle() == SNX_UPSCALE_OFF);
    CHECK(vk_upscale_pick(1280, 720, 1920, 1080, 0) == SNX_UPSCALE_OFF);
    CHECK(vk_upscale_cycle() == SNX_UPSCALE_FSR1);
    CHECK(vk_upscale_pick(1280, 720, 1920, 1080, 0) == SNX_UPSCALE_FSR1);

    image_free(&src);
    vk_finish();
    vk_unlock();
    CHECK(!snx_remove_tree(root));
    free(root);
    printf("PASS headless Vulkan upscalers: FSR 1, NIS, SGSR, Auto's choice and ratio, %u validation messages\n",
           g_vk_validation_messages);
    return 0;
}
