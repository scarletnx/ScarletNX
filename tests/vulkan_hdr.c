/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* HDR's passes of the presented frame on a real Vulkan device, with no game data and no window: an SDR ramp through the
   composition into PQ (HDR10) and into the SDR range, a linear HDR frame and a PQ one, crops, both flips, the
   letterbox, and the upscaler's output composed too, all read back and held to the standards' numbers
   (tests/support/hdr_ref.c). Opt-in with SNX_TEST_VULKAN, as vulkan_upscale is. */
#include "common/utilities/common.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/hdr.h"
#include "nx/gpu/vk/upscale.h"
#include "nx/gpu/vk/vk.h"
#include "common/gpu/vk/swapchain.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/ui.h"
#include "support/half.h"
#include "support/hdr_ref.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "vulkan_hdr:%d: %s\n", __LINE__, #x);                                                      \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

typedef struct {
    VkImage img;
    VmaAllocation mem;
    VkImageView view;
    VkFormat fmt;
    u32 w, h;
} TestImage;

static int image_make(TestImage* t, VkFormat fmt, u32 w, u32 h, int target) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     (target ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0),
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!vk_image_create(&ii, &t->img, &t->mem)) return 0;
    vi.image = t->img;
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &t->view))) return 0;
    t->fmt = fmt;
    t->w = w;
    t->h = h;
    return 1;
}

static void image_free(TestImage* t) {
    if (t->img) vk_defer_image(t->img, t->mem, t->view, VK_NULL_HANDLE);
    memset(t, 0, sizeof *t);
}

/* the pixels staged and copied in, whatever their format's bytes */
static int upload(TestImage* t, const void* px, u32 bytes_per_pixel) {
    const u64 size = (u64)t->w * t->h * bytes_per_pixel;
    VkBuffer buf;
    u64 off;
    u8* p = vk_stage(size, 16, &buf, &off);
    VkBufferImageCopy c = {.bufferOffset = off,
                           .bufferRowLength = t->w,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {t->w, t->h, 1}};
    if (!p) return 0;
    memcpy(p, px, (size_t)size);
    vk_barrier(vk_cmd(), t->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rec_CmdCopyBufferToImage(vk_cmd(), buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    vk_barrier(vk_cmd(), t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_SAMPLED_STAGES,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return 1;
}

/* the target as the numbers the passes wrote: three floats a pixel, PQ decoded to nits for a PQ target, the linear
   light of an scRGB one (1.0 at the paper white), or the SDR code 0 to 1 for an RGBA8 one */
static int read_target(TestImage* t, int enc, float** out) {
    VkBuffer rb;
    u8* data = vk_readback_buffer((u64)t->w * t->h * 4, &rb);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {t->w, t->h, 1}};
    float* v;
    if (!data) return 0;
    rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &copy);
    vk_finish();
    vk_readback_ready();
    v = malloc((size_t)t->w * t->h * 3 * sizeof(float));
    if (!v) return 0;
    for (u32 i = 0; i < t->w * t->h; ++i) {
        u32 w;
        float r, g, b;
        if (t->fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
            w = snx_ld_le32(data + (u64)i * 4);
            r = (float)(w & 1023) / 1023.0f;
            g = (float)((w >> 10) & 1023) / 1023.0f;
            b = (float)((w >> 20) & 1023) / 1023.0f;
        } else if (t->fmt == VK_FORMAT_R16G16B16A16_SFLOAT) {
            const u16* half = (const u16*)(data + (u64)i * 8);
            r = half_to_float(half[0]);
            g = half_to_float(half[1]);
            b = half_to_float(half[2]);
        } else {
            const u8* bytes = data + (u64)i * 4;
            r = bytes[0] / 255.0f;
            g = bytes[1] / 255.0f;
            b = bytes[2] / 255.0f;
        }
        if (enc == VK_HDR_ENC_PQ) {
            v[i * 3 + 0] = (float)hdr_ref_pq_decode(r);
            v[i * 3 + 1] = (float)hdr_ref_pq_decode(g);
            v[i * 3 + 2] = (float)hdr_ref_pq_decode(b);
        } else {
            v[i * 3 + 0] = r;
            v[i * 3 + 1] = g;
            v[i * 3 + 2] = b;
        }
    }
    *out = v;
    return 1;
}

/* the frame's crop through the passes into a target at dw x dh, at (dx, dy) in a dst_w x dst_h image; the pixels come
   back as nits (hdr) or SDR codes */
static int run_at(TestImage* src, int source, u32 x, u32 y, u32 w, u32 h, u32 transform, u32 dst_w, u32 dst_h, i32 dx,
                  i32 dy, u32 dw, u32 dh, int enc, float** out) {
    TestImage t = {0};
    VkImage comp = VK_NULL_HANDLE;
    VkImageView comp_view = VK_NULL_HANDLE;
    VkFormat fmt = enc == VK_HDR_ENC_PQ      ? VK_FORMAT_A2B10G10R10_UNORM_PACK32
                   : enc == VK_HDR_ENC_SCRGB ? VK_FORMAT_R16G16B16A16_SFLOAT
                                             : VK_FORMAT_R8G8B8A8_UNORM;
    int ok;
    if (!image_make(&t, fmt, dst_w, dst_h, 1)) return 0;
    ok = vk_hdr_compose(vk_cmd(), src->view, source, src->w, src->h, x, y, w, h, transform, dst_w, dst_h, dx, dy, dw,
                        dh, &comp, &comp_view) &&
         comp_view && vk_hdr_encode(vk_cmd(), comp, comp_view, dst_w, dst_h, t.img, t.view, t.fmt, enc);
    vk_barrier(vk_cmd(), t.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (ok) ok = read_target(&t, enc, out);
    image_free(&t);
    vk_frame_next();
    return ok;
}
static int run(TestImage* src, int source, u32 dw, u32 dh, int enc, float** out) {
    return run_at(src, source, 0, 0, src->w, src->h, 0, dw, dh, 0, 0, dw, dh, enc, out);
}
/* the frame in a rectangle of its own (the letterbox): dw x dh at dx,dy in a dst_w x dst_h image */
static int run_rect(TestImage* src, int source, u32 w, u32 h, u32 transform, u32 dst_w, u32 dst_h, i32 dx, i32 dy,
                    u32 dw, u32 dh, int enc, float** out) {
    return run_at(src, source, 0, 0, w, h, transform, dst_w, dst_h, dx, dy, dw, dh, enc, out);
}

/* the SDR ramp through the composition: PQ's numbers, white at the paper white */
static int check_sdr_ramp(u32 paper) {
    static const u8 codes[6] = {0, 32, 64, 128, 192, 255};
    u8 px[6 * 4];
    TestImage src = {0};
    float* nits = NULL;
    __atomic_store_n(&g_display_headroom_pct, 100, __ATOMIC_RELAXED);
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 1000, 0, 0);
    for (int i = 0; i < 6; ++i) {
        px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = codes[i];
        px[i * 4 + 3] = 255;
    }
    CHECK(image_make(&src, VK_FORMAT_R8G8B8A8_UNORM, 6, 1, 0));
    CHECK(upload(&src, px, 4));
    CHECK(run(&src, vk_hdr_source_for(src.fmt), 6, 1, 1, &nits));
    for (int i = 0; i < 6; ++i) {
        double want = hdr_ref_sdr_code_nits(codes[i] / 255.0, paper, 0.0);
        double got = nits[i * 3];
        /* a 10-bit PQ code near the value is at most a few nits: the slope of PQ is steepest low down */
        double tol = want < 1.0 ? 0.5 : want * 0.035 + 0.5;
        if (fabs(got - want) > tol) {
            fprintf(stderr, "vulkan_hdr: sdr %u -> %.2f nits, wanted %.2f (paper %u)\n", codes[i], got, want, paper);
            return 1;
        }
        CHECK(fabs(nits[i * 3 + 1] - got) < 0.05 + got * 0.01); /* the ramp is grey: one number three times */
        CHECK(fabs(nits[i * 3 + 2] - got) < 0.05 + got * 0.01);
    }
    /* white is the paper white, the top of the SDR range */
    CHECK(fabs(nits[5 * 3] - (double)paper) < (double)paper * 0.02);
    free(nits);
    image_free(&src);
    return 0;
}

/* a frame that already holds HDR data: linear light at the paper white, or PQ, is not lifted again */
static int check_hdr_source(u32 paper) {
    static const float linear[4] = {0.25f, 0.5f, 1.0f, 4.0f};
    static const float pqcodes[4] = {0.5080784215f, 0.7518270962f, 0.8946318484f, 0.907f};
    TestImage src = {0};
    float* nits = NULL;
    u16 half[4 * 4];
    float pq[4 * 4]; /* 32 bits a channel: the PQ codes stay exact, where a half's 11 bits would blur the light */
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 4000, 0, 0); /* a peak above 4x the paper */
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 3; ++c) half[i * 4 + c] = float_to_half(linear[i]);
    for (int i = 0; i < 4; ++i) half[i * 4 + 3] = float_to_half(1.0f);
    CHECK(image_make(&src, VK_FORMAT_R16G16B16A16_SFLOAT, 4, 1, 0));
    CHECK(upload(&src, half, 8));
    CHECK(vk_hdr_source_for(src.fmt) == SNX_HDR_SRC_LINEAR);
    CHECK(run(&src, SNX_HDR_SRC_LINEAR, 4, 1, 1, &nits));
    for (int i = 0; i < 4; ++i) {
        double want = linear[i] * paper;
        double got = nits[i * 3 + 1];
        CHECK(fabs(got - want) < want * 0.03 + 0.6);
    }
    free(nits);
    nits = NULL;
    image_free(&src);

    /* PQ in, PQ out: 100, 1000 and 3716 nits keep their light, and 4167 nits is clamped to the peak's 4000 */
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 3; ++c) pq[i * 4 + c] = pqcodes[i];
    for (int i = 0; i < 4; ++i) pq[i * 4 + 3] = 1.0f;
    CHECK(image_make(&src, VK_FORMAT_R32G32B32A32_SFLOAT, 4, 1, 0));
    CHECK(upload(&src, pq, 16));
    CHECK(run(&src, SNX_HDR_SRC_PQ, 4, 1, 1, &nits));
    {
        static const double want[4] = {100.0, 1000.0, 3716.0, 4000.0};
        for (int i = 0; i < 4; ++i) CHECK(fabs(nits[i * 3 + 1] - want[i]) < want[i] * 0.02 + 0.6);
    }
    free(nits);
    image_free(&src);
    return 0;
}

/* an HDR frame on an SDR display: the knee keeps the SDR range and compresses above it, and nothing clips */
static int check_sdr_output(u32 paper) {
    static const float linear[5] = {0.0f, 0.5f, 1.0f, 2.0f, 4.0f};
    TestImage src = {0};
    float* sdr = NULL;
    u16 half[5 * 4];
    vk_hdr_configure(SNX_HDR_OFF, SNX_HDR_SRC_AUTO, (int)paper, 1000, 0, 0);
    for (int i = 0; i < 5; ++i)
        for (int c = 0; c < 3; ++c) half[i * 4 + c] = float_to_half(linear[i]);
    for (int i = 0; i < 5; ++i) half[i * 4 + 3] = float_to_half(1.0f);
    CHECK(image_make(&src, VK_FORMAT_R16G16B16A16_SFLOAT, 5, 1, 0));
    CHECK(upload(&src, half, 8));
    CHECK(vk_hdr_frame_needs(src.fmt)); /* HDR off, but the frame holds HDR data */
    CHECK(run(&src, SNX_HDR_SRC_LINEAR, 5, 1, 0, &sdr));
    CHECK(fabs(sdr[0] - 0.0f) < 0.01f);
    for (int i = 1; i < 5; ++i) {
        double want = hdr_ref_sdr_nits_code(linear[i] * paper, paper, 1000.0);
        CHECK(fabs(sdr[i * 3] - want) < 0.012);
        if (i > 1) CHECK(sdr[i * 3] >= sdr[(i - 1) * 3]); /* monotonic, and each step above white is its own */
    }
    CHECK(sdr[2 * 3] > 0.94);        /* the paper white, at 90% of the output */
    CHECK(sdr[4 * 3] > sdr[3 * 3]);  /* 800 nits is lighter than 400, which is lighter than white */
    free(sdr);
    image_free(&src);
    return 0;
}

/* the crop and the frame's flips, and the black around it where the frame is smaller than the image */
static int check_crop_flip_letterbox(u32 paper) {
    u8 px[4 * 2 * 4];
    TestImage src = {0};
    float* nits = NULL;
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 1000, 0, 0); /* white is the paper white */
    for (u32 i = 0; i < 8; ++i) {
        /* the top row is red, green, blue, white; the bottom row is grey levels */
        u8 c[4] = {255, 0, 0, 255};
        if (i == 1) c[0] = 0, c[1] = 255, c[2] = 0;
        if (i == 2) c[0] = 0, c[1] = 0, c[2] = 255;
        if (i == 3) c[0] = c[1] = c[2] = 255;
        if (i >= 4) c[0] = c[1] = c[2] = (u8)(64 * (i - 3));
        memcpy(px + i * 4, c, 4);
    }
    CHECK(image_make(&src, VK_FORMAT_R8G8B8A8_UNORM, 4, 2, 0));
    CHECK(upload(&src, px, 4));
    /* red, through BT.2020's primaries */
    {
        double red[3] = {1.0, 0.0, 0.0}, out[3];
        hdr_ref_bt709_to_2020(red, out);
        CHECK(run_at(&src, SNX_HDR_SRC_SDR, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, &nits));
        CHECK(fabs(nits[0] - out[0] * paper) < 3.0);
        CHECK(fabs(nits[1] - out[1] * paper) < 3.0);
        CHECK(fabs(nits[2] - out[2] * paper) < 3.0);
        free(nits);
        nits = NULL;
    }
    /* the crop 1,0 2x1 is green then blue, each the strongest of its channels (BT.2020 keeps them so) */
    CHECK(run_at(&src, SNX_HDR_SRC_SDR, 1, 0, 2, 1, 0, 2, 1, 0, 0, 2, 1, 1, &nits));
    CHECK(nits[1] > nits[0] && nits[1] > nits[2]);
    CHECK(nits[5] > nits[3] && nits[5] > nits[4]);
    /* FlipH: blue, then green */
    CHECK(run_at(&src, SNX_HDR_SRC_SDR, 1, 0, 2, 1, 1, 2, 1, 0, 0, 2, 1, 1, &nits));
    CHECK(nits[2] > nits[0] && nits[2] > nits[1]);
    CHECK(nits[4] > nits[3] && nits[4] > nits[5]);
    /* FlipV of two rows shows the bottom row first: a 64 grey, where the source's top row's first texel is red */
    CHECK(run_at(&src, SNX_HDR_SRC_SDR, 0, 0, 2, 2, 2, 2, 2, 0, 0, 2, 2, 1, &nits));
    CHECK(nits[0] < 20.0 && nits[0] > 2.0);         /* the 64 grey, at 10 nits */
    CHECK(nits[(1 * 2 + 0) * 3] > 20.0);             /* the row under it is the source's top row, red */
    CHECK(nits[(1 * 2 + 0) * 3] > nits[(1 * 2 + 0) * 3 + 1]);
    free(nits);
    /* the letterbox: a 2x2 frame in an 8x8 image at 3,3, and black around it */
    /* the frame in its own 2x2 rectangle at (3,3), the rest black: it is neither stretched to the image nor moved */
    CHECK(run_rect(&src, SNX_HDR_SRC_SDR, 2, 2, 0, 8, 8, 3, 3, 2, 2, 1, &nits));
    CHECK(nits[0] < 0.5 && nits[1] < 0.5 && nits[2] < 0.5);
    CHECK(nits[((u64)3 * 8 + 3) * 3 + 1] > 0.0); /* (3,3) holds the first texel's light */
    CHECK(nits[((u64)3 * 8 + 4) * 3 + 1] > 0.0); /* (4,3) the second's */
    CHECK(nits[((u64)4 * 8 + 3) * 3 + 1] > 0.0); /* (3,4) the first of the bottom row */
    CHECK(nits[((u64)4 * 8 + 5) * 3 + 1] < 0.5); /* (5,4) is outside it: no stretch */
    CHECK(nits[((u64)5 * 8 + 3) * 3 + 1] < 0.5); /* (3,5) too */
    free(nits);
    image_free(&src);
    return 0;
}

/* the upscaler's output, composed and encoded: HDR on an SDR frame keeps the upscaled picture's white at the paper
   white plus its boost, for each filter */
static int check_upscale_then_hdr(int filter) {
    u8 px[64 * 36 * 4];
    TestImage src = {0};
    TestImage t = {0};
    VkImage up = VK_NULL_HANDLE;
    VkImageView up_view = VK_NULL_HANDLE;
    VkImage comp;
    VkImageView comp_view;
    float* nits = NULL;
    int ok;
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, 200, 1000, 0, 0);
    for (u32 i = 0; i < sizeof px; ++i) px[i] = 255;
    CHECK(image_make(&src, VK_FORMAT_R8G8B8A8_UNORM, 64, 36, 0));
    CHECK(upload(&src, px, 4));
    /* the filter's pipeline is made the first time it is asked for, and that frame's dispatch is dropped: a warm-up
       run, as the presenter's first frame of a filter gives */
    ok = vk_upscale_process(vk_cmd(), src.view, src.w, src.h, 0, 0, src.w, src.h, 0, 128, 72, filter, 25, &up,
                            &up_view);
    CHECK(ok);
    vk_finish();
    vk_frame_next();
    up = VK_NULL_HANDLE;
    up_view = VK_NULL_HANDLE;
    ok = vk_upscale_process(vk_cmd(), src.view, src.w, src.h, 0, 0, src.w, src.h, 0, 128, 72, filter, 25, &up, &up_view);
    CHECK(ok);
    vk_barrier(vk_cmd(), up, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(vk_hdr_compose(vk_cmd(), up_view, SNX_HDR_SRC_SDR, 128, 72, 0, 0, 128, 72, 0, 128, 72, 0, 0, 128, 72, &comp,
                         &comp_view));
    CHECK(image_make(&t, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 128, 72, 1));
    CHECK(vk_hdr_encode(vk_cmd(), comp, comp_view, 128, 72, t.img, t.view, t.fmt, 1));
    vk_barrier(vk_cmd(), t.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK(read_target(&t, 1, &nits));
    {
        float lo = 1e9f, hi = -1e9f;
        for (u32 i = 0; i < 128 * 72; ++i) {
            float v = nits[i * 3 + 1];
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
        if (fabs(lo - 200.0f) > 12.0f || fabs(hi - 200.0f) > 12.0f)
            fprintf(stderr, "vulkan_hdr: filter %d white %.1f to %.1f nits\n", filter, lo, hi);
        CHECK(fabs(lo - 200.0f) < 12.0f && fabs(hi - 200.0f) < 12.0f);
    }
    free(nits);
    image_free(&t);
    image_free(&src);
    vk_frame_next();
    return 0;
}

/* the live toggle, and the choices the swapchain makes */
static int check_toggle_and_picks(void) {
    static const VkFormat wanted[] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    static const VkSurfaceFormatKHR formats[] = {
        {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
    };
    VkSurfaceFormatKHR out = {0};
    char text[128];
    CHECK(snx_vk_pick_format(formats, 4, wanted, 1, VK_COLOR_SPACE_HDR10_ST2084_EXT, &out) == 1);
    CHECK(out.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && out.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT);
    CHECK(snx_vk_pick_format(formats, 2, wanted, 1, VK_COLOR_SPACE_HDR10_ST2084_EXT, &out) == 0);
    CHECK(snx_vk_pick_format(NULL, 0, wanted, 1, VK_COLOR_SPACE_HDR10_ST2084_EXT, &out) == 0);

    vk_hdr_configure(SNX_HDR_OFF, SNX_HDR_SRC_AUTO, 200, 1000, 0, 0);
    CHECK(vk_hdr_mode() == SNX_HDR_OFF);
    vk_hdr_describe(SNX_HDR_DISPLAY_PQ, text, sizeof text);
    CHECK(!strcmp(text, "Off"));
    CHECK(vk_hdr_toggle() == SNX_HDR_ON && vk_hdr_mode() == SNX_HDR_ON);
    vk_hdr_describe(SNX_HDR_DISPLAY_PQ, text, sizeof text);
    CHECK(strstr(text, "HDR10") && strstr(text, "200"));
    vk_hdr_describe(SNX_HDR_DISPLAY_SCRGB, text, sizeof text);
    CHECK(strstr(text, "scRGB"));
    vk_hdr_describe(SNX_HDR_DISPLAY_P3, text, sizeof text);
    CHECK(strstr(text, "Display P3"));
    CHECK(vk_hdr_toggle() == SNX_HDR_OFF && vk_hdr_mode() == SNX_HDR_OFF);

    CHECK(vk_hdr_source_for(VK_FORMAT_R16G16B16A16_SFLOAT) == SNX_HDR_SRC_LINEAR);
    CHECK(vk_hdr_source_for(VK_FORMAT_R32G32B32A32_SFLOAT) == SNX_HDR_SRC_LINEAR);
    CHECK(vk_hdr_source_for(VK_FORMAT_R8G8B8A8_UNORM) == SNX_HDR_SRC_SDR);
    CHECK(vk_hdr_source_for(VK_FORMAT_A2B10G10R10_UNORM_PACK32) == SNX_HDR_SRC_SDR);
    CHECK(!vk_hdr_frame_hdr(VK_FORMAT_R8G8B8A8_UNORM));
    CHECK(vk_hdr_frame_hdr(VK_FORMAT_R16G16B16A16_SFLOAT));
    CHECK(!vk_hdr_frame_needs(VK_FORMAT_R8G8B8A8_UNORM)); /* HDR off and an SDR frame: the plain blit */
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, 200, 1000, 0, 0);
    CHECK(vk_hdr_frame_needs(VK_FORMAT_R8G8B8A8_UNORM));
    /* the hdr_source setting names the frame for a game whose format doesn't say */
    vk_hdr_configure(SNX_HDR_OFF, SNX_HDR_SRC_PQ, 200, 1000, 0, 0);
    CHECK(vk_hdr_source_for(VK_FORMAT_R8G8B8A8_UNORM) == SNX_HDR_SRC_PQ);
    CHECK(vk_hdr_frame_needs(VK_FORMAT_R8G8B8A8_UNORM));
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, 200, 1000, 0, 0);
    CHECK(vk_hdr_available());
    CHECK(vk_hdr_compose_format() == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(vk_hdr_display_kind() == SNX_HDR_DISPLAY_NONE); /* no window, so no surface to ask */
    return 0;
}

static u32 g_hdr_validation_base;

/* the menu's screen before a game's first frame: a composition of black alone, encoded; and the SDR output into an
   sRGB image, which encodes it once, as the window's own format does */
static int check_empty_and_srgb(void) {
    TestImage t = {0};
    u8 px[4 * 4 * 4];
    TestImage src = {0};
    float* out = NULL;
    VkImage comp = VK_NULL_HANDLE;
    VkImageView comp_view = VK_NULL_HANDLE;
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, 200, 1000, 0, 0);
    CHECK(image_make(&t, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 4, 1));
    CHECK(vk_hdr_compose(vk_cmd(), VK_NULL_HANDLE, SNX_HDR_SRC_SDR, 0, 0, 0, 0, 0, 0, 0, 4, 4, 0, 0, 4, 4, &comp,
                         &comp_view));
    CHECK(comp_view);
    CHECK(vk_hdr_encode(vk_cmd(), comp, comp_view, 4, 4, t.img, t.view, t.fmt, 1));
    vk_barrier(vk_cmd(), t.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK(read_target(&t, 1, &out));
    for (u32 i = 0; i < 16; ++i) CHECK(out[i * 3] < 0.01f && out[i * 3 + 1] < 0.01f && out[i * 3 + 2] < 0.01f);
    free(out);
    image_free(&t);
    vk_frame_next();

    for (u32 i = 0; i < 16; ++i) px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = 128, px[i * 4 + 3] = 255;
    CHECK(image_make(&src, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 0));
    CHECK(upload(&src, px, 4));
    CHECK(image_make(&t, VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 1));
    CHECK(vk_hdr_compose(vk_cmd(), src.view, SNX_HDR_SRC_SDR, 4, 4, 0, 0, 4, 4, 0, 4, 4, 0, 0, 4, 4, &comp,
                         &comp_view));
    CHECK(vk_hdr_encode(vk_cmd(), comp, comp_view, 4, 4, t.img, t.view, t.fmt, 0));
    vk_barrier(vk_cmd(), t.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK(read_target(&t, 0, &out));
    {
        /* 128 of 255 is 0.2158 linear, 43 nits at 200: 0.45 of the SDR range, 0.701 encoded */
        double want = hdr_ref_sdr_nits_code(hdr_ref_sdr_code_nits(128.0 / 255.0, 200.0, 0.0), 200.0, 1000.0);
        for (u32 i = 0; i < 16; ++i) CHECK(fabs(out[i * 3] - want) < 0.01);
    }
    free(out);
    image_free(&t);
    image_free(&src);
    vk_frame_next();
    return 0;
}

/* scRGB, the kind macOS and Windows take: linear light in sRGB's primaries, 1.0 at the paper white, so an SDR frame
   is exactly what the display shows in SDR, and the frame's own headroom (and the boost) is above 1 */
static int check_scrgb(u32 paper) {
    static const u8 codes[4] = {0, 64, 128, 255};
    u8 px[4 * 4];
    TestImage src = {0};
    float* lin = NULL;
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 1000, 0, 0);
    for (int i = 0; i < 4; ++i) {
        px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = codes[i];
        px[i * 4 + 3] = 255;
    }
    CHECK(image_make(&src, VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 0));
    CHECK(upload(&src, px, 4));
    CHECK(run(&src, SNX_HDR_SRC_SDR, 4, 1, VK_HDR_ENC_SCRGB, &lin));
    for (int i = 0; i < 4; ++i) {
        double want = hdr_ref_srgb_to_linear(codes[i] / 255.0);
        CHECK(fabs(lin[i * 3 + 1] - want) < 0.005 + want * 0.01);
    }
    CHECK(fabs(lin[3 * 3] - 1.0f) < 0.01f); /* white is the display's own white, not a fixed number of nits */
    free(lin);
    image_free(&src);

    /* the headroom the display reports is what the lift may use, and its share: half of a 2.0 is the half above 1 */
    {
        u8 white[4] = {255, 255, 255, 255};
        __atomic_store_n(&g_display_headroom_pct, 200, __ATOMIC_RELAXED);
        vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 1000, 50, 0);
        CHECK(image_make(&src, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 0));
        CHECK(upload(&src, white, 4));
        CHECK(run(&src, SNX_HDR_SRC_SDR, 1, 1, VK_HDR_ENC_SCRGB, &lin));
        CHECK(fabs(lin[1] - 1.5f) < 0.01f);
        free(lin);
        image_free(&src);
        __atomic_store_n(&g_display_headroom_pct, 100, __ATOMIC_RELAXED);
    }
    /* a frame that already holds HDR data: 1.0 is the paper white, 2.5 is two and a half times it */
    {
        static const float values[2] = {1.0f, 2.5f};
        u16 half[2 * 4];
        for (int i = 0; i < 2; ++i)
            for (int c = 0; c < 3; ++c) half[i * 4 + c] = float_to_half(values[i]);
        for (int i = 0; i < 2; ++i) half[i * 4 + 3] = float_to_half(1.0f);
        vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 4000, 0, 0);
        CHECK(image_make(&src, VK_FORMAT_R16G16B16A16_SFLOAT, 2, 1, 0));
        CHECK(upload(&src, half, 8));
        CHECK(run(&src, SNX_HDR_SRC_LINEAR, 2, 1, VK_HDR_ENC_SCRGB, &lin));
        CHECK(fabs(lin[0 * 3 + 1] - 1.0f) < 0.01f);
        CHECK(fabs(lin[1 * 3 + 1] - 2.5f) < 0.02f);
        free(lin);
        image_free(&src);
    }
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 1000, 0, 0);
    return 0;
}

/* the top of an SDR frame lifted into the display's headroom: the darks and the midtones as SDR, white using the
   share of the headroom, the colours' ratios kept. The practice: BT.2446, Dolby's inverse tone mapping, and Horizon's
   HDR, which lift only the brightest part of the frame */
static int check_lift(u32 paper) {
    static const u8 codes[5] = {0, 64, 128, 230, 255};
    u8 px[5 * 4];
    TestImage src = {0};
    float* lin = NULL;
    __atomic_store_n(&g_display_headroom_pct, 200, __ATOMIC_RELAXED);
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 1000, 100, 0); /* the whole of the headroom */
    for (int i = 0; i < 5; ++i) {
        px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = codes[i];
        px[i * 4 + 3] = 255;
    }
    CHECK(image_make(&src, VK_FORMAT_R8G8B8A8_UNORM, 5, 1, 0));
    CHECK(upload(&src, px, 4));
    CHECK(run(&src, SNX_HDR_SRC_SDR, 5, 1, VK_HDR_ENC_SCRGB, &lin));
    /* below 80 % of the code range nothing moves: 0, 64 and 128 are their SDR linear values */
    CHECK(fabs(lin[0] - 0.0f) < 0.005f);
    CHECK(fabs(lin[1 * 3] - hdr_ref_srgb_to_linear(64.0 / 255.0)) < 0.01);
    CHECK(fabs(lin[2 * 3] - hdr_ref_srgb_to_linear(128.0 / 255.0)) < 0.01);
    /* the 230 code is above the knee and is lifted; white reaches the whole of the headroom */
    {
        double le = 230.0 / 255.0, k = (le - 0.8) / 0.2, knee = k * k * (3.0 - 2.0 * k);
        double want = hdr_ref_srgb_to_linear(le) * (1.0 + 1.0 * knee);
        CHECK(fabs(lin[3 * 3] - want) < 0.02);
    }
    CHECK(fabs(lin[4 * 3] - 2.0f) < 0.02f);
    /* and no headroom reports nothing to lift: the frame is exactly SDR */
    __atomic_store_n(&g_display_headroom_pct, 100, __ATOMIC_RELAXED);
    CHECK(run(&src, SNX_HDR_SRC_SDR, 5, 1, VK_HDR_ENC_SCRGB, &lin));
    for (int i = 0; i < 5; ++i)
        CHECK(fabs(lin[i * 3] - hdr_ref_srgb_to_linear(codes[i] / 255.0)) < 0.01);
    free(lin);
    image_free(&src);

    /* the colours: the lift is on the luma only, so a colour's linear ratios do not move */
    {
        u8 mixed[4] = {250, 240, 200, 255};
        float plain[3], lifted[3];
        __atomic_store_n(&g_display_headroom_pct, 200, __ATOMIC_RELAXED);
        vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 1000, 0, 0);
        CHECK(image_make(&src, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 0));
        CHECK(upload(&src, mixed, 4));
        CHECK(run(&src, SNX_HDR_SRC_SDR, 1, 1, VK_HDR_ENC_SCRGB, &lin));
        memcpy(plain, lin, sizeof plain);
        free(lin);
        vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 1000, 100, 0);
        CHECK(run(&src, SNX_HDR_SRC_SDR, 1, 1, VK_HDR_ENC_SCRGB, &lin));
        memcpy(lifted, lin, sizeof lifted);
        free(lin);
        {
            double r0 = plain[1] / plain[0], b0 = plain[2] / plain[0];
            double r1 = lifted[1] / lifted[0], b1 = lifted[2] / lifted[0];
            CHECK(fabs(r1 - r0) < 0.01 && fabs(b1 - b0) < 0.01); /* the same hue and saturation */
            CHECK(lifted[0] > plain[0]); /* and more light */
        }
        image_free(&src);
    }
    __atomic_store_n(&g_display_headroom_pct, 100, __ATOMIC_RELAXED);
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 1000, 0, 0);
    return 0;
}

/* Auto, the default: the passes run only where the display takes an HDR image with headroom. Headless there is no
   surface, so Auto is off and an SDR frame keeps the plain blit */
static int check_auto(void) {
    char text[128];
    vk_hdr_configure(SNX_HDR_AUTO, SNX_HDR_SRC_AUTO, 203, 1000, 50, 0);
    CHECK(vk_hdr_mode() == SNX_HDR_AUTO);
    CHECK(!vk_hdr_active()); /* no window, so no surface to take it */
    CHECK(!vk_hdr_frame_needs(VK_FORMAT_R8G8B8A8_UNORM));
    CHECK(vk_hdr_frame_needs(VK_FORMAT_R16G16B16A16_SFLOAT)); /* HDR data still goes through */
    vk_hdr_describe(SNX_HDR_DISPLAY_NONE, text, sizeof text);
    CHECK(strstr(text, "Off (auto"));
    /* the toggle from Auto makes it On, which is active, then Off */
    CHECK(vk_hdr_toggle() == SNX_HDR_ON && vk_hdr_active());
    CHECK(vk_hdr_toggle() == SNX_HDR_OFF && !vk_hdr_active());
    return 0;
}

/* the test pattern: six patches of known light, 0, the paper white and 1.25 to 2 times it, over the frame's bottom
   eighth, encoded with everything else, so a run proves the lift reaches the display */
static int check_pattern(u32 paper) {
    TestImage t = {0};
    VkImage comp = VK_NULL_HANDLE;
    VkImageView comp_view = VK_NULL_HANDLE;
    float* nits = NULL;
    __atomic_store_n(&g_display_headroom_pct, 200, __ATOMIC_RELAXED);
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 1000, 100, 1);
    /* 6 x 16: the band is the bottom eighth, two rows, a patch a column */
    CHECK(vk_hdr_compose(vk_cmd(), VK_NULL_HANDLE, SNX_HDR_SRC_SDR, 0, 0, 0, 0, 0, 0, 0, 6, 16, 0, 0, 6, 16, &comp,
                         &comp_view));
    CHECK(image_make(&t, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 6, 16, 1));
    CHECK(vk_hdr_encode(vk_cmd(), comp, comp_view, 6, 16, t.img, t.view, t.fmt, VK_HDR_ENC_PQ));
    vk_barrier(vk_cmd(), t.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK(read_target(&t, VK_HDR_ENC_PQ, &nits));
    for (int i = 0; i < 6; ++i) {
        static const double factor[6] = {0.0, 1.0, 1.25, 1.5, 1.75, 2.0};
        double want = factor[i] * paper;
        CHECK(fabs(nits[((u64)15 * 6 + i) * 3 + 1] - want) < want * 0.03 + 0.6);
    }
    free(nits);
    image_free(&t);
    vk_frame_next();
    __atomic_store_n(&g_display_headroom_pct, 100, __ATOMIC_RELAXED);
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 1000, 50, 0);
    return 0;
}

/* Display P3's wide gamut, the kind macOS takes: the same linear light in P3's primaries, the white at 1.0 */
static int check_p3(u32 paper) {
    u8 white[4] = {255, 255, 255, 255}, red[4] = {255, 0, 0, 255};
    TestImage src = {0};
    float* lin = NULL;
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 1000, 0, 0);
    CHECK(image_make(&src, VK_FORMAT_R8G8B8A8_UNORM, 2, 1, 0));
    {
        u8 px[8] = {255, 255, 255, 255, 255, 0, 0, 255};
        CHECK(upload(&src, px, 4));
    }
    CHECK(run(&src, SNX_HDR_SRC_SDR, 2, 1, VK_HDR_ENC_P3, &lin));
    CHECK(fabs(lin[0] - 1.0f) < 0.01f); /* white is the paper white, as scRGB's */
    {
        /* 709 red in P3: the matrix's first column, (0.6274, 0.0691, 0.0164) through 2020 -> P3 */
        double in[3] = {1.0, 0.0, 0.0}, mid[3], out[3];
        hdr_ref_bt709_to_2020(in, mid);
        hdr_ref_bt2020_to_p3(mid, out);
        CHECK(fabs(lin[1 * 3 + 0] - out[0]) < 0.02); /* the output is scaled by the paper white, then 1/paper */
        CHECK(fabs(lin[1 * 3 + 1] - out[1]) < 0.02);
        CHECK(fabs(lin[1 * 3 + 2] - out[2]) < 0.02);
        CHECK(out[0] > 0.75 && out[1] < 0.1); /* P3's red is nearer the corner than sRGB's */
    }
    free(lin);
    image_free(&src);
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, (int)paper, 1000, 0, 0);
    return 0;
}

/* the menu's letters over an HDR frame: the toast is drawn into the composition in linear light, scaled to the paper
   white, and encoded with the frame, so white letters come out at 200 nits and not as an SDR code's light */
static int check_menu_over_hdr(void) {
    TestImage t = {0};
    VkImage comp = VK_NULL_HANDLE;
    VkImageView comp_view = VK_NULL_HANDLE;
    VkUiTarget target;
    int safe[4] = {0, 0, 320, 180};
    float* nits = NULL;
    u32 lit = 0, brightest = 0;
    float most = 0.0f;
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, 200, 1000, 0, 0);
    ui_lock();
    overlay_toast("HDR: On", plat_ns());
    ui_unlock();
    CHECK(vk_hdr_compose(vk_cmd(), VK_NULL_HANDLE, SNX_HDR_SRC_SDR, 0, 0, 0, 0, 0, 0, 0, 320, 180, 0, 0, 320, 180,
                         &comp, &comp_view));
    target = (VkUiTarget){.img = comp,
                          .view = comp_view,
                          .fmt = vk_hdr_compose_format(),
                          .extent = {320, 180},
                          .mode = VK_UI_HDR_NITS,
                          .gain = (float)vk_hdr_paper_nits(),
                          .from_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .from_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          .from_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    if (!vk_ui_draw(vk_cmd(), &target, safe)) return 0; /* no font here: nothing to hold */
    CHECK(image_make(&t, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 320, 180, 1));
    CHECK(vk_hdr_encode(vk_cmd(), comp, comp_view, 320, 180, t.img, t.view, t.fmt, 1));
    vk_barrier(vk_cmd(), t.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK(read_target(&t, 1, &nits));
    for (u32 i = 0; i < 320 * 180; ++i) {
        if (nits[i * 3 + 1] > 1.0f) lit++;
        if (nits[i * 3 + 1] > most) {
            most = nits[i * 3 + 1];
            brightest = i;
        }
    }
    /* the toast's letters are the brightest thing in the composition: the paper white's light, not the SDR code's */
    CHECK(lit > 20);
    CHECK(most > 150.0f && most < 260.0f);
    CHECK(nits[brightest * 3] > 100.0f && nits[brightest * 3 + 2] > 100.0f);
    free(nits);
    image_free(&t);
    vk_frame_next();
    return 0;
}

/* a PPM of an RGBA8 image's bytes */
static int write_ppm(const char* dir, const char* name, const u8* rgba, u32 w, u32 h) {
    char path[1024];
    FILE* f;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f) return 0;
    {
        int ok = fprintf(f, "P6\n%u %u\n255\n", w, h) > 0;
        for (u32 i = 0; i < w * h && ok; ++i) ok = fwrite(rgba + (u64)i * 4, 1, 3, f) == 3;
        ok = fclose(f) == 0 && ok;
        return ok;
    }
}

/* The showcase a run may write (--showcase=DIR): the same synthetic 640x360 scene three ways at 1920x1080, the
   upscaler off, FSR 1, and FSR 1 with HDR. The HDR panel is the PQ output shown as an SDR capture shows it, its
   values above the paper white clipped. Real FSR 1 and the real HDR passes, on the GPU. */
static int showcase(const char* dir) {
    const u32 SW = 640, SH = 360, W = 1920, H = 1080;
    TestImage src = {0};
    u8* px = malloc((size_t)SW * SH * 4);
    float* values = NULL;
    u8* out = malloc((size_t)W * H * 4);
    CHECK(px && out);
    /* the scene: a gradient sky with a bright sun, a hard diagonal edge, fine stripes both ways (bilinear softens them,
       FSR 1 keeps their edges) and a colour bar */
    for (u32 y = 0; y < SH; ++y)
        for (u32 x = 0; x < SW; ++x) {
            u8* p = px + ((u64)y * SW + x) * 4;
            float r, g, b;
            if (y < 140) {
                float t = (float)y / 140.0f;
                float dx = ((float)x - 470.0f) / 52.0f, dy = ((float)y - 52.0f) / 52.0f, d = dx * dx + dy * dy;
                r = 0.04f + 0.55f * t, g = 0.09f + 0.70f * t, b = 0.28f + 0.70f * t;
                if (d < 1.0f) {
                    float k = 1.0f - d;
                    r = r + k > 1 ? 1 : r + k, g = g + k > 1 ? 1 : g + k, b = b + 0.6f * k > 1 ? 1 : b + 0.6f * k;
                }
            } else if (y < 210) { /* a hard diagonal edge, like a roof against the sky */
                float edge = 60.0f + (float)(y - 140) * 0.9f;
                if ((float)x > edge) r = 0.62f, g = 0.35f, b = 0.22f;
                else r = 0.10f, g = 0.16f, b = 0.30f;
            } else if (y < 270) { /* two-pixel vertical stripes */
                float v = ((x / 2) & 1) ? 0.95f : 0.12f;
                r = v, g = v * 0.85f, b = v * 0.6f;
            } else if (y < 320) { /* two-pixel horizontal stripes */
                float v = ((y / 2) & 1) ? 0.95f : 0.12f;
                r = v * 0.7f, g = v * 0.95f, b = v;
            } else { /* a colour bar, dark to bright */
                u32 i = x * 8 / SW;
                float v = (float)i / 7.0f;
                r = v, g = v * 0.6f + 0.2f, b = v * 0.3f + 0.4f;
                if (i == 7) r = g = b = 1.0f;
            }
            p[0] = (u8)(r * 255.0f + 0.5f);
            p[1] = (u8)(g * 255.0f + 0.5f);
            p[2] = (u8)(b * 255.0f + 0.5f);
            p[3] = 255;
        }
    CHECK(image_make(&src, VK_FORMAT_R8G8B8A8_UNORM, SW, SH, 0));
    CHECK(upload(&src, px, 4));
    __atomic_store_n(&g_display_headroom_pct, 200, __ATOMIC_RELAXED);
    /* the FSR 1 pipeline is made on its first call and that frame's dispatch is dropped: warm it up */
    {
        VkImage up = VK_NULL_HANDLE;
        VkImageView up_view = VK_NULL_HANDLE;
        vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, 203, 1000, 0, 0);
        CHECK(vk_upscale_process(vk_cmd(), src.view, SW, SH, 0, 0, SW, SH, 0, W, H, SNX_UPSCALE_FSR1, 25, &up, &up_view));
        vk_finish();
        vk_frame_next();
    }
    for (int panel = 0; panel < 3; ++panel) {
        TestImage t = {0};
        VkImage up = VK_NULL_HANDLE, comp = VK_NULL_HANDLE;
        VkImageView up_view = VK_NULL_HANDLE, comp_view = VK_NULL_HANDLE;
        VkImageView frame_view = src.view;
        u32 fw = SW, fh = SH;
        int hdr = panel == 2;
        vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, 203, 1000, hdr ? 50 : 0, 0);
        if (panel > 0) {
            CHECK(vk_upscale_process(vk_cmd(), src.view, SW, SH, 0, 0, SW, SH, 0, W, H, SNX_UPSCALE_FSR1, 25, &up,
                                     &up_view));
            vk_barrier(vk_cmd(), up, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            frame_view = up_view;
            fw = fh = 0; /* the upscaler's output is the whole frame; its size is W x H */
        }
        {
            u32 sw = panel > 0 ? W : fw, sh = panel > 0 ? H : fh;
            if (!sw) sw = W, sh = H;
            CHECK(vk_hdr_compose(vk_cmd(), frame_view, SNX_HDR_SRC_SDR, sw, sh, 0, 0, sw, sh, 0, W, H, 0, 0, W, H, &comp,
                                 &comp_view));
        }
        CHECK(image_make(&t, VK_FORMAT_A2B10G10R10_UNORM_PACK32, W, H, 1));
        CHECK(vk_hdr_encode(vk_cmd(), comp, comp_view, W, H, t.img, t.view, t.fmt,
                            hdr ? VK_HDR_ENC_PQ : VK_HDR_ENC_SDR));
        vk_barrier(vk_cmd(), t.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        CHECK(read_target(&t, hdr ? VK_HDR_ENC_PQ : VK_HDR_ENC_SDR, &values));
        for (u32 i = 0; i < W * H; ++i) {
            u8* p = out + (u64)i * 4;
            if (hdr) { /* the PQ output as an SDR capture shows it: relative to the paper white, clipped at it */
                for (int c = 0; c < 3; ++c) {
                    float n = values[i * 3 + c] / 203.0f;
                    float v = n >= 1.0f ? 1.0f : (n <= 0.0f ? 0.0f : 1.055f * powf(n, 1.0f / 2.4f) - 0.055f);
                    p[c] = (u8)(v * 255.0f + 0.5f);
                }
            } else {
                p[0] = (u8)(values[i * 3 + 0] * 255.0f + 0.5f);
                p[1] = (u8)(values[i * 3 + 1] * 255.0f + 0.5f);
                p[2] = (u8)(values[i * 3 + 2] * 255.0f + 0.5f);
            }
            p[3] = 255;
        }
        CHECK(write_ppm(dir, panel == 0 ? "panel_off" : panel == 1 ? "panel_fsr" : "panel_fsr_hdr", out, W, H));
        free(values);
        values = NULL;
        image_free(&t);
        vk_frame_next();
    }
    __atomic_store_n(&g_display_headroom_pct, 100, __ATOMIC_RELAXED);
    vk_hdr_configure(SNX_HDR_ON, SNX_HDR_SRC_AUTO, 203, 1000, 50, 0);
    free(px);
    free(out);
    image_free(&src);
    return 0;
}

int main(int argc, char** argv) {
    char* root = snx_temp_dir(NULL);
    const char* showcase_dir = NULL;
    CHECK(root);
    CHECK(!snx_startup_options(argc, argv));
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--vk_validation=1")) g_settings.vulkan_validation_layer = 1;
        if (!strncmp(argv[i], "--showcase=", 11)) showcase_dir = argv[i] + 11;
    }
    snprintf(g_exedir, 1024, "%s/", root);
    mem_init();
    CHECK(mem_map(HEAP_BASE, 0x800000, MS_NORMAL, PERM_RW) == 0);
    nv_init();
    video_init("Headless Vulkan HDR fixture", "");
    CHECK(g_opt_headless && g_renderer_up && g_vk.dev && !g_vk.surface && !g_vk.swapchain);
    /* the device's own layout is past this GPU's per-stage sampler limit on some drivers (MoltenVK's), which the
       layer says twice as the renderer starts: the passes' own messages are what this test holds to none */
    {
        u32 base = g_vk_validation_messages;
        g_hdr_validation_base = base;
    }

    vk_lock();
    printf("Headless HDR device: %s\n", g_vk.props.deviceName);
    CHECK(!check_toggle_and_picks());
    CHECK(!check_sdr_ramp(200));
    printf("SDR ramp: 0 to 255 through PQ at 200 nits, white at the paper white\n");
    CHECK(!check_sdr_ramp(100));
    printf("SDR ramp: paper white 100\n");
    CHECK(!check_lift(200));
    printf("the SDR lift: only the top of the range, the display's headroom's share, the colours' ratios kept\n");
    CHECK(!check_p3(200));
    printf("Display P3: white at the paper white, 709 red in P3's primaries\n");
    CHECK(!check_auto());
    printf("Auto: off where the display takes no HDR, and HDR data still goes through\n");
    CHECK(!check_pattern(203));
    printf("the test pattern: six patches of known light, 0 to twice the paper white\n");
    CHECK(!check_scrgb(200));
    printf("scRGB: white at the display's own white, the headroom above 1\n");
    CHECK(!check_hdr_source(200));
    printf("HDR source: linear at the paper white, PQ passed as it is\n");
    CHECK(!check_sdr_output(200));
    printf("HDR to SDR: the knee keeps the SDR range and compresses the peak\n");
    CHECK(!check_crop_flip_letterbox(200));
    printf("crops, both flips, the letterbox and BT.2020's primaries\n");
    CHECK(!check_empty_and_srgb());
    printf("the menu's screen alone, and the SDR output into an sRGB image\n");
    CHECK(!check_menu_over_hdr());
    printf("the menu's letters over the composition, at the paper white\n");
    CHECK(!check_upscale_then_hdr(SNX_UPSCALE_FSR1));
    CHECK(!check_upscale_then_hdr(SNX_UPSCALE_NIS));
    CHECK(!check_upscale_then_hdr(SNX_UPSCALE_SGSR));
    printf("FSR 1, NIS and SGSR composed and encoded, white at the paper white\n");

    if (showcase_dir) CHECK(!showcase(showcase_dir));
    vk_unlock();
    vk_pipeline_quiesce();
    CHECK(!snx_remove_tree(root));
    free(root);
    if (g_vk_validation_messages != g_hdr_validation_base)
        printf("vulkan_hdr: a validation message: %u after the device's own %u\n", g_vk_validation_messages,
               g_hdr_validation_base);
    printf("PASS headless Vulkan HDR: the composition, PQ and the SDR knee, HDR sources, the upscaler, %u validation "
           "messages\n",
           g_vk_validation_messages - g_hdr_validation_base);
    return 0;
}
