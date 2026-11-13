/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The pre-draw capture's images (ShaderInfo.predraw_slots, shader/spirv_translate.c predraw_prologue).

   A draw that samples a color target it renders to reads, on the console, what the target held before the draw: the
   texture cache keeps the lines it read until the game's texture barrier, so two overlapping primitives of one draw
   both read the old pixel. In place on the host (VK_EXT_attachment_feedback_loop_layout), the second may read what
   the first just wrote, in whatever order the GPU's tiles take, and no barrier orders primitives inside a draw. A
   terrain draw that blends layers over the G-buffer it renders to then flickered in specks of a few pixels.

   The capture reads the old pixel without a copy of the target. In a fragment shader interlock at the program's start,
   a fragment compares the pixel's stamp with its draw's number. The first fragment of the draw at the pixel fetches
   each captured slot there, which no fragment of the draw has written yet (the interlock orders their sections in
   primitive order, and a fragment's color writes come after its section), puts the texels in the side images and
   stamps the pixel. The first fragment keeps what it fetched, and the others read the side images. A fragment that a
   later test discards captures only what the pixel held before the draw, which the next fragment wants too.

   The stores keep the GPU from culling hidden fragments before the program runs. Where the guest's program can't
   discard or write depth, the variant declares early fragment tests (render.c), which give the same result: in the
   test scene this took the G-buffer pass from 2.80 ms to 2.28 ms a frame (1.95 ms without the capture, RTX 4090).
   The interlock costs about 0.3 ms of it, and the side images' traffic little (4-byte sides took 0.05 ms off).

   The stamp image (R32_UINT) and one side image a slot (R32G32B32A32_UINT, the texel's bits whatever its type) are as
   large as the largest pass drawn with them, in GENERAL from their making. Within a pass the interlock orders their
   accesses, across draws too; between passes a memory barrier at the next pass's beginning does
   (vk_predraw_pass_begin). Draw numbers count up from 1; before they wrap the stamps are cleared again */
#include "nx/gpu/vk/predraw.h"
#include "nx/gpu/vk/descriptor_objects.h"
#include "nx/runtime/platform/log.h"

static struct {
    Surf stamp;                /* the capture number of the last draw that captured each pixel */
    Surf side[SH_PREDRAW_MAX]; /* each captured slot's texel, as it was before that capture */
    u32 draw;                  /* the last capture number */
    int written;               /* a draw stored to them since the last barrier */
    u64 epoch;                 /* g_vk_texture_epoch when the number was taken */
    u32 nslot;                 /* the targets the number's draws capture, by side image */
    const Surf* slot[SH_PREDRAW_MAX];
} g_predraw;

static void image_drop(Surf* s) {
    if (s->img) vk_defer_image(s->img, s->mem, s->att, VK_NULL_HANDLE);
    *s = (Surf){0};
}
static int image_make(Surf* s, u32 sw, u32 sh, VkFormat format, u32 bytes) {
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {sw, sh, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    *s = (Surf){0};
    s->w = s->sw = sw;
    s->h = s->sh = sh;
    s->layers = s->levels = s->depth3d = 1;
    s->storage_capable = 1;
    s->type = VK_IMAGE_VIEW_TYPE_2D;
    s->f = (VkFmt){.vk = format, .bpp = (u8)bytes, .bw = 1, .bh = 1};
    if (!vk_image_create(&info, &s->img, &s->mem)) {
        *s = (Surf){0};
        return 0;
    }
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .image = s->img,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                  .format = format,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!VK_OK(vk_image_view_create(g_vk.dev, &view, NULL, &s->att))) {
        vk_image_destroy(s->img, s->mem);
        *s = (Surf){0};
        return 0;
    }
    return 1;
}
static void drop_all(void) {
    image_drop(&g_predraw.stamp);
    for (u32 k = 0; k < SH_PREDRAW_MAX; k++) image_drop(&g_predraw.side[k]);
}
/* outside a pass: the stamps at 0, which no draw's number is, and visible to the fragment shaders that follow */
static void stamps_clear(void) {
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    Surf* stamp = &g_predraw.stamp;
    vk_surf_use(cb, stamp, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkClearColorValue zero = {.uint32 = {0, 0, 0, 0}};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    rec_CmdClearColorImage(cb, stamp->img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    vk_surf_use(cb, stamp, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    g_predraw.draw = 0;
    g_predraw.written = 0;
}
int vk_predraw_reserve(u32 sw, u32 sh, u32 sides) {
    if (!sw || !sh || !sides || sides > SH_PREDRAW_MAX) return 0;
    Surf* stamp = &g_predraw.stamp;
    if (!stamp->img || stamp->sw < sw || stamp->sh < sh) {
        u32 w = stamp->sw > sw ? stamp->sw : sw, h = stamp->sh > sh ? stamp->sh : sh;
        vk_render_end(); /* the old images' last draws are recorded: they go once those are done */
        drop_all();
        if (!image_make(stamp, w, h, VK_FORMAT_R32_UINT, 4)) return 0;
        LOG("vulkan: pre-draw capture images of %ux%u\n", w, h);
        stamps_clear();
    } else if (g_predraw.draw == ~0u)
        stamps_clear();
    for (u32 k = 0; k < sides; k++) {
        Surf* side = &g_predraw.side[k];
        if (side->img) continue;
        if (!image_make(side, stamp->sw, stamp->sh, VK_FORMAT_R32G32B32A32_UINT, 16)) return 0;
        /* never read before a stamp says this draw wrote it: no contents to keep */
        vk_render_end();
        vk_surf_use(vk_cmd(), side, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    return 1;
}
/* The draws between two of the game's texture barriers share one number. On the console each of them reads what the
   targets held at the barrier: the texture cache keeps the lines it read, and the ROP's writes of the draws before
   reach texture reads only after the next barrier (render.c, feedback loops). Here a later draw's capture read the
   target in place, where the writes of the draws just before it may or may not have landed: two crossing rock props
   that blend into what lies under them read each other in one frame and the cliff in the next, and whole patches
   bounded by the first prop's triangles flickered while the camera turned. With one number the first fragment at a
   pixel in the interval captures it, before any draw of the interval wrote it, and every later draw there reads that.
   A draw that captures other targets, or in another order, takes a number of its own: the side images are by slot */
u32 vk_predraw_next(const Surf* const* targets, u32 n) {
    u64 epoch = __atomic_load_n(&g_vk_texture_epoch, __ATOMIC_RELAXED);
    int same = g_predraw.draw && epoch == g_predraw.epoch && n == g_predraw.nslot;
    for (u32 k = 0; same && k < n; k++) same = targets[k] == g_predraw.slot[k];
    if (same) return g_predraw.draw;
    g_predraw.epoch = epoch;
    g_predraw.nslot = n;
    for (u32 k = 0; k < n; k++) g_predraw.slot[k] = targets[k];
    return ++g_predraw.draw;
}
VkImageView vk_predraw_view(u32 k) {
    if (k > SH_PREDRAW_MAX) return VK_NULL_HANDLE;
    return k ? g_predraw.side[k - 1].att : g_predraw.stamp.att;
}
void vk_predraw_drawn(void) { g_predraw.written = 1; }
void vk_predraw_pass_begin(VkCommandBuffer cb) {
    if (!g_predraw.written) return;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    g_predraw.written = 0;
}
