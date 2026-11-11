/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen layer's pass (gpu/vk/ui.c, vk_ui_draw) on an image of its own as the swapchain's, each pixel opaque or
   not written: each quad an instance whose corners the vertex shader makes, the atlas sampled in texels, and the
   device's buffer of instances given only the quads that changed since the frame before. Each frame's pixels against
   the CPU's blend of the same quads, the stats box and the debug line (stats.c, hud.c), where they are sure: a pixel
   whose centre or texel falls on an edge, or that a linear sample covers, is not compared. The frames change some of
   the box's letters, none, the line's length, the box's corner, and take the box away and back, so the device's buffer
   holds every kind of change */
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/ui.h"
#include "nx/runtime/platform/vpad.h"
#include "nx/gpu/vk/vk.h"
#include "support/ui_shape_ref.h"
#include <math.h>

#define UI_W 640
#define UI_H 360
static const u8 ui_back[4] = {40, 80, 120, 255};

typedef struct {
    VkImage img;
    VmaAllocation mem;
    u32* atlas; /* the atlas as ui_atlas_make makes it: R G B A bytes, alpha premultiplied */
    int atlas_h;
    SnxUiQuad quads[4096];
    float want[UI_H][UI_W][4]; /* the blend, 0 to 1 */
    u8 sure[UI_H][UI_W];
    u8 last[UI_H * UI_W * 4]; /* the frame before's pixels */
} UiCheck;

/* the layer as vk_ui_draw makes it: the producers in its order, nothing before the game (the controls, showcase and
   the menu off) */
static int ui_quads(UiCheck* c, const int safe[4]) {
    SnxUiList l = {c->quads, 0, 4096};
    int px, py, scale;
    ui_lock();
    vpad_draw(&l, safe);
    vpad_showcase_draw(&l, safe, vpad_showcase_fps(g_vk_present, plat_ns()));
    overlay_place(UI_W, UI_H, &px, &py, &scale);
    stats_draw(&l, safe, scale, plat_ns());
    hud_draw(&l, safe, scale);
    overlay_draw(&l, UI_W, UI_H, plat_ns());
    ui_unlock();
    return l.n;
}
/* whether v is within a thousandth of a whole number */
static int ui_on_edge(float v) { return fabsf(v - roundf(v)) < 1e-3f; }
/* one quad blended into want as it is drawn (ui_drawn: a bar of the ring where it is, from its byte, and the place
   and colour the quad holds), sure where it is */
static void ui_blend_quad(UiCheck* c, const SnxUiQuadView* q) {
    float color[4] = {(float)(q->rgba & 0xFF) / 255.0f, (float)(q->rgba >> 8 & 0xFF) / 255.0f,
                      (float)(q->rgba >> 16 & 0xFF) / 255.0f, (float)(q->rgba >> 24) / 255.0f};
    int x0, y0, x1, y1, x, y, k;
    if (!q->uw) return; /* a blank cell, or no bar */
    x0 = (int)floorf(q->x) - 1;
    y0 = (int)floorf(q->y) - 1;
    x1 = (int)ceilf(q->x + q->w) + 1;
    y1 = (int)ceilf(q->y + q->h) + 1;
    for (y = y0 < 0 ? 0 : y0; y < y1 && y < UI_H; ++y)
        for (x = x0 < 0 ? 0 : x0; x < x1 && x < UI_W; ++x) {
            float cx = (float)x + 0.5f, cy = (float)y + 0.5f, tu, tv, tex[4], src[4];
            const u8* t;
            if (cx < q->x - 1e-3f || cx > q->x + q->w + 1e-3f || cy < q->y - 1e-3f || cy > q->y + q->h + 1e-3f)
                continue;
            if (q->ink == UI_SHAPE) { /* drawn from its place alone, its colour where it is */
                int in = ui_shape_ref(q->u, q->v, q->uw & 0x7FFFu, q->vh, q->w, q->h, cx - q->x, cy - q->y);
                if (in < 0 || fabsf(cx - q->x) < 1e-3f || fabsf(cx - q->x - q->w) < 1e-3f || fabsf(cy - q->y) < 1e-3f ||
                    fabsf(cy - q->y - q->h) < 1e-3f)
                    c->sure[y][x] = 0;
                else if (in)
                    for (k = 0; k < 4; ++k) c->want[y][x][k] = color[k];
                continue;
            }
            tu = (float)q->u + (cx - q->x) / q->w * (float)q->uw;
            tv = (float)q->v + (cy - q->y) / q->h * (float)q->vh;
            if (fabsf(cx - q->x) < 1e-3f || fabsf(cx - q->x - q->w) < 1e-3f || fabsf(cy - q->y) < 1e-3f ||
                fabsf(cy - q->y - q->h) < 1e-3f || q->sample != UI_SHARP || q->turn || ui_on_edge(tu) ||
                ui_on_edge(tv)) {
                c->sure[y][x] = 0;
                continue;
            }
            t = (const u8*)&c->atlas[(size_t)floorf(tv) * SNX_UI_ATLAS_W + (size_t)floorf(tu)];
            for (k = 0; k < 4; ++k) tex[k] = (float)t[k] / 255.0f;
            for (k = 0; k < 3; ++k) src[k] = tex[k] * color[k] * color[3];
            src[3] = tex[3] * color[3];
            /* ui.frag's: opaque, or not written under half */
            if (fabsf(src[3] - 0.5f) < 1e-3f) {
                c->sure[y][x] = 0;
                continue;
            }
            if (src[3] < 0.5f) continue;
            for (k = 0; k < 3; ++k) c->want[y][x][k] = src[k] / src[3];
            c->want[y][x][3] = 1;
        }
}
/* the CPU's blend of n quads over the back colour: want, and sure where it is. A backed letter over its ground */
static void ui_blend(UiCheck* c, int n) {
    int x, y, i, k;
    for (y = 0; y < UI_H; ++y)
        for (x = 0; x < UI_W; ++x) {
            for (k = 0; k < 4; ++k) c->want[y][x][k] = (float)ui_back[k] / 255.0f;
            c->sure[y][x] = 1;
        }
    for (i = 0; i < n; ++i) {
        SnxUiQuadView ground, drawn = ui_drawn(&c->quads[i]);
        if (ui_drawn_ground(&c->quads[i], &ground)) ui_blend_quad(c, &ground);
        ui_blend_quad(c, &drawn);
    }
}
/* a frame of the layer on the image, read back, against the CPU's blend, said when loud: the channels that differ,
   and the pixels that differ from the frame before's into *changed */
static int ui_frame_once(UiCheck* c, const int safe[4], const char* what, int* changed, int loud) {
    VkBuffer rb;
    u8* px = vk_readback_buffer((u64)UI_W * UI_H * 4, &rb);
    VkClearColorValue back = {{ui_back[0] / 255.0f, ui_back[1] / 255.0f, ui_back[2] / 255.0f, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {UI_W, UI_H, 1}};
    VkCommandBuffer cb;
    int n, x, y, k, bad = 0;
    CHECK(px);
    vk_render_end();
    cb = vk_cmd();
    vk_barrier(cb, c->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rec_CmdClearColorImage(cb, c->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &back, 1, &all);
    /* as the frame's blit leaves the swapchain's image */
    vk_barrier(cb, c->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vk_ui_draw(cb, c->img, safe);
    cb = vk_cmd();
    vk_barrier(cb, c->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    rec_CmdCopyImageToBuffer(cb, c->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &copy);
    vk_finish();
    vk_readback_ready();
    n = ui_quads(c, safe);
    ui_blend(c, n);
    *changed = 0;
    for (y = 0; y < UI_H; ++y)
        for (x = 0; x < UI_W; ++x) {
            const u8* p = px + ((size_t)y * UI_W + (size_t)x) * 4;
            *changed += memcmp(p, c->last + (p - px), 4) != 0;
            if (!c->sure[y][x]) continue;
            for (k = 0; k < 4; ++k) {
                int want = (int)lroundf(c->want[y][x][k] * 255.0f);
                if (abs(want - p[k]) > 1 && bad++ < 4 && loud)
                    fprintf(stderr, "vulkan_ui: %s: pixel %d,%d channel %d is %d, not %d\n", what, x, y, k, p[k], want);
            }
        }
    memcpy(c->last, px, sizeof c->last);
    if (bad && loud) fprintf(stderr, "vulkan_ui: %s: %d channels differ (%d quads)\n", what, bad, n);
    return bad;
}
/* ui_frame_once, and once more where it differs: the frame rate's half second can end between the GPU's frame and
   the CPU's, whose first line then differs. A fault differs again; *changed is the first one's */
static int ui_frame(UiCheck* c, const int safe[4], const char* what, int* changed) {
    int again;
    if (!ui_frame_once(c, safe, what, changed, 0)) return 0;
    return ui_frame_once(c, safe, what, &again, 1);
}

static int check_ui(void) {
    static const int safe[4] = {0, 0, UI_W, UI_H};
    UiCheck* c = calloc(1, sizeof *c);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {UI_W, UI_H, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo device = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkExtent2D extent = g_vk.sc_extent;
    VkFormat format = g_vk.sc_format;
    int srgb = g_vk.sc_srgb, changed, stats = g_settings.stats, controls = g_settings.show_controls,
        showcase = g_settings.showcase;
    const IniOpt* corner = ini_find(g_display_ini, "stats_corner");
    CHECK(c && corner);
    CHECK(vmaCreateImage(g_vk.vma, &ii, &device, &c->img, &c->mem, NULL) == VK_SUCCESS);
    /* the atlas: the font, the white block and the icons */
    c->atlas = ui_atlas_make(&c->atlas_h);
    CHECK(c->atlas);
    ui_atlas_build();
    g_vk.sc_extent = (VkExtent2D){UI_W, UI_H};
    g_vk.sc_format = VK_FORMAT_R8G8B8A8_UNORM;
    g_vk.sc_srgb = 0;
    g_settings.show_controls = 0;
    g_settings.showcase = 0;
    g_settings.stats = 1;
    stats_measured(&(SnxStatsMeasure){.cpu = 25, .memory = 512ull << 20, .gpu = -1});
    hud_debug_set("log=2, vk_validation=1");
    /* the frame rate's first half second: after it, 0.0 with no frames, the same on the GPU's frame and the CPU's */
    ui_frame(c, safe, "the first frame", &changed);
    plat_sleep_ns(600000000);
    CHECK(!ui_frame(c, safe, "the box and the line", &changed));
    /* some of the box's letters changed, and some of its cells blank */
    stats_measured(&(SnxStatsMeasure){.cpu = 7, .memory = 65535ull << 20, .memory_total = 31500ull << 20, .gpu = 63,
                                      .gpu_ns = 10450000, .vram = 3120ull << 20, .vram_budget = 7980ull << 20});
    CHECK(!ui_frame(c, safe, "other values", &changed));
    CHECK(changed > 100);
    CHECK(!ui_frame(c, safe, "no change", &changed));
    CHECK(!changed);
    stats_measured(&(SnxStatsMeasure){.cpu = -1, .gpu = -1});
    CHECK(!ui_frame(c, safe, "the values unknown again", &changed));
    /* the line longer and shorter: the quads after the box's more and fewer */
    hud_debug_set("log=2, vk_validation=1, shader_corpus, gdbstub, renderdoc_frames=3");
    CHECK(!ui_frame(c, safe, "a longer debug line", &changed));
    hud_debug_set("gdbstub");
    CHECK(!ui_frame(c, safe, "a shorter debug line", &changed));
    /* the box away, back, and in another corner */
    g_settings.stats = 0;
    CHECK(!ui_frame(c, safe, "no box", &changed));
    g_settings.stats = 1;
    CHECK(!ui_frame(c, safe, "the box back", &changed));
    stats_corner_set(corner, "br");
    CHECK(!ui_frame(c, safe, "the box at the bottom right", &changed));
    stats_corner_set(corner, "tl");
    CHECK(!ui_frame(c, safe, "the box at the top left again", &changed));
    /* the graph's ring: bars for 30 frames, slow ones among them; one more, which writes one bar and scrolls the
       others; and past its slots, round it more than once */
    {
        SnxStatsGpu gpu = {0};
        u64 at = 1000000000ull;
        int k;
        for (k = 0; k < 30; ++k) stats_frame(at += (k % 4 ? 16 : 40) * 1000000ull, &gpu);
        CHECK(!ui_frame(c, safe, "30 bars", &changed));
        stats_frame(at += 25 * 1000000ull, &gpu);
        CHECK(!ui_frame(c, safe, "a bar more", &changed));
        CHECK(changed > 0);
        for (k = 0; k < 2 * STATS_FRAMES + 7; ++k) stats_frame(at += (u64)(5 + k % 45) * 1000000ull, &gpu);
        CHECK(!ui_frame(c, safe, "the ring round twice", &changed));
        for (k = 0; k < 3; ++k) {
            stats_frame(at += (u64)(10 + 15 * k) * 1000000ull, &gpu);
            CHECK(!ui_frame(c, safe, "a bar a frame", &changed));
        }
    }
    /* the on-screen controls and showcase's box: shapes with no texels (ui_shape), each an edge and its label; pressed,
       filled with the label cut out. A, the D-pad's up and the left stick's click, the stick right */
    {
        static const i32 none[4], right[4] = {32767, 0, 0, 0};
        g_settings.show_controls = 1;
        CHECK(!ui_frame(c, safe, "the on-screen controls", &changed));
        g_settings.show_controls = 0;
        g_settings.showcase = 1;
        vpad_showcase_input(1u << 0 | 1u << 13 | 1u << 4, right);
        CHECK(!ui_frame(c, safe, "showcase, pressed", &changed));
        vpad_showcase_input(0, none);
        CHECK(!ui_frame(c, safe, "showcase, let go", &changed));
        CHECK(changed > 0);
        g_settings.showcase = 0;
    }
    /* the same on a GPU that renders in tiles (ui.c's tiler, by the vendor: ARM's here), whose sparse pipeline writes
       a pixel it doesn't draw clear and blends it away in place of a discard: the same pixels */
    {
        static const i32 right[4] = {32767, 0, 0, 0};
        u32 vendor = g_vk.props.vendorID;
        g_vk.props.vendorID = 0x13B5;
        g_settings.show_controls = 1;
        CHECK(!ui_frame(c, safe, "the on-screen controls, blended", &changed));
        g_settings.show_controls = 0;
        g_settings.showcase = 1;
        vpad_showcase_input(1u << 0 | 1u << 13 | 1u << 4, right);
        CHECK(!ui_frame(c, safe, "showcase pressed, blended", &changed));
        g_settings.showcase = 0;
        g_vk.props.vendorID = vendor;
        CHECK(!ui_frame(c, safe, "the box and the line, discarded again", &changed));
    }
    vk_finish();
    vk_ui_swapchain_gone();
    vmaDestroyImage(g_vk.vma, c->img, c->mem);
    hud_debug_set("");
    g_settings.stats = stats;
    g_settings.show_controls = controls;
    g_settings.showcase = showcase;
    g_vk.sc_extent = extent;
    g_vk.sc_format = format;
    g_vk.sc_srgb = srgb;
    free(c->atlas);
    free(c);
    return 0;
}
