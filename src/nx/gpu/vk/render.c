/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The render target set, clears and draws. The 3D registers name up to 8 color targets (RT_CONTROL at 0x487 maps
   the slots, each target's address, size, format and tiling at 0x200 + 0x10 * index) and a depth target (enabled
   at 0x54E, address at 0x3F8, size at 0x48A, format and tiling at 0x3FA). A dynamic rendering pass stays open while
   draws and clears keep the same set, and ends before anything that copies or transitions images.

   A draw reads the 3D engine's registers for its state. Signed viewport scales and coordinate swizzles map the
   guest's position. Window-origin bit 4 controls triangle facing independently of that mapping. */
#include "common/utilities/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "nx/runtime/options.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/maxwell/draw_state.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/shader/sm86.h"
#include "nx/gpu/shader/advanced_blend_spirv.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/pagespace.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/pipeline_parts.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/gpu/vk/capture.h"
#include "nx/gpu/vk/depth_shadow.h"
#include "nx/gpu/nvn/nvn_blend.h"
#include "nx/gpu/vk/descriptor_key.h"
#include "nx/gpu/vk/indexed_handles.h"
#include "nx/gpu/texture/texconv_modes.glsl"
#include "nx/gpu/texture/descriptor.h"
#include "nx/gpu/vk/draw_texture.h"
#include "nx/gpu/vk/draw_texture_shader.h"
#include "nx/gpu/vk/host_program.h"
#include "nx/gpu/vk/lookup_ways.h"
#include "nx/gpu/vk/pending_pages.h"
#include "nx/gpu/vk/tess_passthrough.h"
#include "nx/gpu/vk/varying_geometry.h"
#include "nx/gpu/vk/quad_indices.h"
#include "nx/gpu/vk/polygon_edges.h"
#include "nx/gpu/vk/xfb_stores.h"
#include "nx/memory/own.h"
#include "nx/gpu/vk/internal_spv.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/gpu/vk/predraw.h"
#include "nx/gpu/vk/shading_rate.h"

static struct {
    Surf* rt[8];
    Surf* zt;
    u32 n; /* color slots up to the last bound one */
    VkExtent2D ext;
    int open;
    int srgb_write;
    int zt_ro; /* the depth target is read-only in this pass: draws also sample it */
    /* feedback loops (feedback_order): the color slots in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL, those of them written
       since the pass began or its last barrier, and those sampled since then. due_*: those of them written or sampled
       before the guest's last fragment barrier (vk_render_fragment_barrier), which a draw must see ordered */
    u32 fb, fb_written, fb_read, due_written, due_read;
    /* A pass is begun (vkCmdBeginRendering) only when its first command is recorded (rp_record): a clear of whole
       attachments before that is the pass's load op (vk_render_clear), as a discard is (vk_render_discard). Its
       attachments, with the load ops it begins with, and its layers */
    int begun;
    int clears; /* a load op clears: the pass is begun even with nothing drawn */
    VkRenderingAttachmentInfo ca[8], za, zs;
    u32 layers;
    /* the shading rate attachment (shading_rate.c, VK_VRS_KHR), VK_NULL_HANDLE for none. rate_want: what the next
       render_begin gives the pass, a draw's id (DrawCall.shading_rate), or RATE_KEEP for the open pass's own */
    VkImageView rate;
    u32 rate_want;
} g_rp = {.rate_want = ~0u};
#define RATE_KEEP ~0u
u64 g_vk_fb_draws, g_vk_fb_barriers, g_vk_fb_restarts, g_vk_rt_copies;
u64 g_vk_passes, g_vk_clear_cmds, g_vk_clear_loads, g_vk_dontcare_loads;

/* the open pass begun, for a command recorded in it */
static void rp_record(void) {
    if (!g_rp.open || g_rp.begun) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, g_rp.ext},
                          .layerCount = g_rp.layers,
                          .colorAttachmentCount = g_rp.n,
                          .pColorAttachments = g_rp.ca,
                          .pDepthAttachment = g_rp.zt ? &g_rp.za : NULL,
                          .pStencilAttachment = g_rp.zt && g_rp.zt->f.stencil ? &g_rp.zs : NULL};
    VkRenderingFragmentShadingRateAttachmentInfoKHR rate = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR,
        .imageView = g_rp.rate,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .shadingRateAttachmentTexelSize = g_vk.vrs_texel};
    if (g_rp.rate) ri.pNext = &rate;
    vk_predraw_pass_begin(cb);
    vk_gpuprof_pass_begin(cb, g_rp.n ? g_rp.rt[0] : NULL, g_rp.zt);
    rec_CmdBeginRendering(cb, &ri);
    g_rp.begun = 1;
    g_vk_passes++;
    const VkRenderingAttachmentInfo* att[10] = {ri.pDepthAttachment, ri.pStencilAttachment};
    for (u32 i = 0; i < g_rp.n; i++) att[2 + i] = g_rp.rt[i] ? &g_rp.ca[i] : NULL;
    for (u32 i = 0; i < 2 + g_rp.n; i++) {
        g_vk_clear_loads += att[i] && att[i]->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
        g_vk_dontcare_loads += att[i] && att[i]->loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
}

/* the passes ended since the last stats, by the call site that ended them (a site is its file's literal and a line),
   under the GPU thread as vk_render_end_at is */
#define PASS_SITES 64
static struct {
    const char* file;
    int line;
    u64 n;
} g_pass_ends[PASS_SITES];
static void pass_end_count(const char* file, int line) {
    for (u32 i = 0; i < PASS_SITES; i++) {
        if (!g_pass_ends[i].file) {
            g_pass_ends[i].file = file;
            g_pass_ends[i].line = line;
        }
        if (g_pass_ends[i].file == file && g_pass_ends[i].line == line) {
            g_pass_ends[i].n++;
            return;
        }
    }
}
void vk_pass_ends_log(void) {
    u64 total = 0;
    for (u32 i = 0; i < PASS_SITES && g_pass_ends[i].file; i++) total += g_pass_ends[i].n;
    if (!total) return;
    for (u32 k = 0; k < 8; k++) {
        u32 best = PASS_SITES;
        for (u32 i = 0; i < PASS_SITES && g_pass_ends[i].file; i++)
            if (g_pass_ends[i].n && (best == PASS_SITES || g_pass_ends[i].n > g_pass_ends[best].n)) best = i;
        if (best == PASS_SITES) break;
        const char* f = strrchr(g_pass_ends[best].file, '/');
        LOG("vulkan:   %llu passes (%.0f%%) ended at %s:%d\n", (unsigned long long)g_pass_ends[best].n,
            100.0 * (double)g_pass_ends[best].n / (double)total, f ? f + 1 : g_pass_ends[best].file,
            g_pass_ends[best].line);
        g_pass_ends[best].n = 0;
    }
    for (u32 i = 0; i < PASS_SITES && g_pass_ends[i].file; i++) g_pass_ends[i].n = 0;
}

int vk_render_begun(void) { return g_rp.open && (g_rp.begun || g_rp.clears); }
void vk_render_end_at(const char* file, int line) {
    if (!g_rp.open) return;
    if (g_rp.clears) rp_record(); /* clears with nothing drawn after them still clear */
    g_rp.open = 0;
    if (!g_rp.begun) return; /* nothing was recorded in it: its load and store would change nothing */
    g_rp.begun = 0;
    pass_end_count(file, line);
    VkCommandBuffer cb = vk_cmd();
    vk_query_pass_end(cb); /* a query begun in the pass ends in it */
    rec_CmdEndRendering(cb);
    vk_gpuprof_pass_end(cb);
}
/* the load op of an attachment s: its contents don't matter when they are undefined (never drawn, or new memory
   over it: surface.c vk_rt_reload) or discarded with nothing written since. undefined: s's layout before the pass */
static VkAttachmentLoadOp load_op(Surf* s, int undefined) {
    int discarded = s->discarded && s->discard_wseq == s->wseq;
    s->discarded = 0;
    return undefined || discarded ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
}

/* The targets read_targets found last, kept while their group's generation (DsState.gen[DS_TARGETS], a new one
   whenever DsTargets' bytes change, from one count for every owner) and the surface table (g_vk_surf_epoch) stay the
   same: most draws draw where the one before did, and each found its targets again through the surface table. Each
   kept target still gets the read check its lookup makes (vk_rt_note_read). A set where a lookup found nothing isn't
   kept, so it is tried again */
static struct {
    u64 gen; /* 0: nothing kept */
    u32 epoch, n;
    Surf *rt[8], *zt;
} g_targets;
u64 g_vk_targets_kept;
/* the surfaces of the targets ds names, without opening anything: a multisampled one's pixels are its samples
   (DsSurf's sizes) by its sample layout's block */
static void read_targets(const DsState* ds, Surf** rt, Surf** zt, u32* n) {
    const DsTargets* tg = &ds->tgt;
    u64 gen = ds->gen[DS_TARGETS];
    if (gen && g_targets.gen == gen &&
        g_targets.epoch == atomic_load_explicit(&g_vk_surf_epoch, memory_order_relaxed)) {
        memcpy(rt, g_targets.rt, sizeof g_targets.rt);
        *zt = g_targets.zt;
        *n = g_targets.n;
        for (u32 i = 0; i < 8; i++)
            if (rt[i]) vk_rt_note_read(rt[i]);
        if (*zt) vk_rt_note_read(*zt);
        g_vk_targets_kept++;
        return;
    }
    int found = 1; /* every lookup made found its target */
    u32 lx = ds_msaa_log2_x(tg->msaa), ly = ds_msaa_log2_y(tg->msaa);
    *n = 0;
    for (u32 i = 0; i < 8; i++) {
        rt[i] = NULL;
        if (i >= tg->count) continue;
        const DsSurf* t = &tg->color[i];
        u64 a = t->gva;
        if (a && t->width && t->height && t->format) {
            /* a pitch-linear target (bit 12 of the memory layout) holds its row pitch in bytes where the width is */
            u32 bpp = vk_rt_format(t->format).bpp, linear = (t->mem >> 12) & 1, w = linear ? t->width / bpp : t->width;
            /* COLOR_TARGET_THIRD_DIMENSION names the number of renderable array slices, or a volume's depth when the
               memory word's THIRD_DIMENSION_CONTROL (bit 16, NVIDIA's clb197.h) says so: its slices are then GOB
               slices of a 3D surface, and the array pitch says nothing. One game's 64x64x64 light volume taken as 64
               layers its 1 MiB array pitch apart covered 63 MiB of other memory. Vertex shaders may select one with
               their layer output (the color LUT passes render all slices in one draw). */
            u32 layers = t->layers;
            if (!layers || layers > g_vk.props.limits.maxImageArrayLayers) layers = 1;
            if (w >> lx && t->height >> ly) {
                rt[i] = vk_rt_array(a, w >> lx, t->height >> ly, t->format, t->mem, linear ? t->width : w * bpp, layers,
                                    t->layer_stride, (t->mem >> 16) & 1, tg->msaa);
                if (!rt[i]) found = 0;
            }
            if (rt[i]) *n = i + 1;
        }
    }
    *zt = NULL;
    const DsSurf* z = &tg->depth;
    u32 depth_msaa = ds_depth_msaa(tg), zx = ds_msaa_log2_x(depth_msaa), zy = ds_msaa_log2_y(depth_msaa);
    if (z->gva && z->width >> zx && z->height >> zy) {
        u32 layers = z->layers;
        if (!layers || layers > g_vk.props.limits.maxImageArrayLayers) layers = 1;
        *zt =
            vk_zt_array(z->gva, z->width >> zx, z->height >> zy, z->format, z->mem, layers, z->layer_stride, depth_msaa);
        if (!*zt) found = 0;
    }
    /* the table as the lookups left it: one that made a target changed it */
    g_targets.gen = found ? gen : 0;
    g_targets.epoch = atomic_load_explicit(&g_vk_surf_epoch, memory_order_relaxed);
    g_targets.n = *n;
    memcpy(g_targets.rt, rt, sizeof g_targets.rt);
    g_targets.zt = *zt;
}

/* the size the targets rt (n of them) and zt have in common */
static VkExtent2D target_extent(Surf* const* rt, const Surf* zt, u32 n) {
    VkExtent2D ext = {~0u, ~0u};
    for (u32 i = 0; i < n; i++)
        if (rt[i]) {
            if (rt[i]->sw < ext.width) ext.width = rt[i]->sw;
            if (rt[i]->sh < ext.height) ext.height = rt[i]->sh;
        }
    if (zt) {
        if (zt->sw < ext.width) ext.width = zt->sw;
        if (zt->sh < ext.height) ext.height = zt->sh;
    }
    return ext;
}
/* opens a rendering pass on these targets (begun at its first command: rp_record), or keeps the open one if it is the
   same set. zt_ro: the depth target is
   bound read-only, so draws can sample it too (soft particles test against depth they also read). fb: color slots the
   draw samples, which the pass needs in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL (feedback_order). area: the render area when no
   attachment has a surface (a draw's color targets all sampled and none written, render_draw), the targets' the guest
   bound */
static int render_begin(Surf** rt, Surf* zt, u32 n, int zt_ro, int srgb_write, u32 fb, VkExtent2D area) {
    srgb_write = !!srgb_write;
    if (!n && !zt) return 0;
    if (!zt) zt_ro = 0;
    int same = g_rp.open && g_rp.n == n && g_rp.zt == zt && g_rp.zt_ro == zt_ro && g_rp.srgb_write == srgb_write &&
               !memcmp(g_rp.rt, rt, sizeof g_rp.rt);
    /* a draw's shading rate attachment: one the pass doesn't have begins another. It must cover the open pass's size
       (the same targets), or else the area, which the new pass's size is no larger than */
    VkImageView rate = g_rp.rate_want == RATE_KEEP ? (same ? g_rp.rate : VK_NULL_HANDLE)
                       : vk_vrs_attachment(g_rp.rate_want, same ? g_rp.ext : area);
    if (same && rate != g_rp.rate) same = 0;
    if (same && !(fb & ~g_rp.fb)) return 1;
    if (same) g_vk_fb_restarts++; /* only a target's layout changes */
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    u32 loops = 0;
    VkRenderingAttachmentInfo ca[8];
    VkExtent2D ext = {~0u, ~0u};
    vk_surf_batch_begin(); /* the targets' barriers in one call */
    for (u32 i = 0; i < n; i++) {
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                            .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        if (!rt[i]) continue;
        int undefined = rt[i]->layout == VK_IMAGE_LAYOUT_UNDEFINED;
        /* a target left in the feedback-loop layout stays in it: draws keep sampling the same targets pass after pass,
           and moving it back would end the next such pass at its first feedback draw */
        int loop = g_vk.has_feedback &&
                   ((fb >> i & 1) || rt[i]->layout == VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT);
        VkImageLayout layout =
            loop ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_LAYOUT(COLOR_ATTACHMENT_OPTIMAL);
        /* the fragment shader's reads are in the barrier's second scope, so the pass begins with every earlier write
           visible to them */
        vk_surf_use(cb, rt[i], layout,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                        (loop ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : 0),
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                        (loop ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0));
        ca[i].imageView = vk_rt_attachment(rt[i], srgb_write);
        if (!ca[i].imageView) {
            vk_surf_batch_end();
            return 0;
        }
        ca[i].imageLayout = layout;
        ca[i].loadOp = load_op(rt[i], undefined);
        if (loop) loops |= 1u << i;
        if (rt[i]->sw < ext.width) ext.width = rt[i]->sw;
        if (rt[i]->sh < ext.height) ext.height = rt[i]->sh;
    }
    VkRenderingAttachmentInfo za = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkImageLayout zl =
        zt_ro ? VK_LAYOUT(DEPTH_STENCIL_READ_ONLY_OPTIMAL) : VK_LAYOUT(DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    /* a read-only depth target keeps its contents: a store would write it at the end of the pass, which the read
       access vk_surf_use records doesn't cover, and the next barrier wouldn't wait for it */
    if (zt_ro) za.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    if (zt) {
        int undefined = zt->layout == VK_IMAGE_LAYOUT_UNDEFINED;
        if (zt_ro)
            vk_surf_use(cb, zt, zl,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                            VK_SAMPLED_STAGES,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        else
            vk_surf_use(cb, zt, zl,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        za.imageView = zt->att;
        za.imageLayout = zl;
        za.loadOp = load_op(zt, undefined);
        if (zt->sw < ext.width) ext.width = zt->sw;
        if (zt->sh < ext.height) ext.height = zt->sh;
    }
    vk_surf_batch_end();
    if (ext.width == ~0u) ext = area; /* nothing bound with a surface: the bound targets' size */
    if (ext.width > area.width) ext.width = area.width;
    if (ext.height > area.height) ext.height = area.height;
    u32 layers = ~0u;
    for (u32 i = 0; i < n; i++)
        if (rt[i] && rt[i]->layers < layers) layers = rt[i]->layers;
    if (zt && zt->layers < layers) layers = zt->layers;
    if (layers == ~0u) layers = 1;
    memcpy(g_rp.ca, ca, n * sizeof *ca);
    g_rp.za = g_rp.zs = za; /* depth and stencil: one view, a load op each */
    g_rp.layers = layers;
    g_rp.rate = rate;
    g_rp.begun = g_rp.clears = 0;
    memcpy(g_rp.rt, rt, sizeof g_rp.rt);
    g_rp.zt = zt;
    g_rp.n = n;
    g_rp.ext = ext;
    g_rp.zt_ro = zt_ro;
    g_rp.srgb_write = srgb_write;
    g_rp.fb = loops;
    g_rp.fb_written = g_rp.fb_read = g_rp.due_written = g_rp.due_read = 0;
    g_rp.open = 1;
    return 1;
}
static void target_written(Surf* s) {
    vk_surf_wrote(s);
    vk_rt_drawn(s);
}
/* a draw or clear wrote the depth target, which was undefined (its layout, before the pass, or still) when that is
   pred_undefined. One predicated on the GPU may not have run, and depth is never loaded from memory: until a write
   that surely runs, the target holds nothing a texture may take (tex_take_drawn), as before its first pass. 1 when
   it counts as written */
static int zt_written(Surf* zt, u32 pred, int undefined) {
    if (pred && (undefined || zt->pred_undefined)) {
        zt->pred_undefined = 1;
        return 0;
    }
    zt->pred_undefined = 0;
    return 1;
}

/* A pass with no attachments, for a draw that only captures its vertices (transform feedback with no target): its
   area is as large as a framebuffer may be, so the draw's viewport and scissor fit in it */
static int render_begin_empty_area(VkExtent2D ext, u32 layers) {
    if (g_rp.open && !g_rp.n && !g_rp.zt && g_rp.ext.width == ext.width && g_rp.ext.height == ext.height &&
        g_rp.layers == layers)
        return 1;
    vk_render_end();
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, ext}, .layerCount = layers};
    vk_predraw_pass_begin(vk_cmd());
    vk_gpuprof_pass_begin(vk_cmd(), NULL, NULL);
    rec_CmdBeginRendering(vk_cmd(), &ri);
    g_rp.begun = 1;
    g_rp.clears = 0;
    g_vk_passes++;
    memset(g_rp.rt, 0, sizeof g_rp.rt);
    g_rp.rate = VK_NULL_HANDLE;
    g_rp.zt = NULL;
    g_rp.n = 0;
    g_rp.layers = layers;
    g_rp.ext = ext;
    g_rp.zt_ro = 0;
    g_rp.srgb_write = 0;
    g_rp.fb = g_rp.fb_written = g_rp.fb_read = g_rp.due_written = g_rp.due_read = 0;
    g_rp.open = 1;
    return 1;
}
static int render_begin_empty(void) {
    return render_begin_empty_area(
        (VkExtent2D){g_vk.props.limits.maxFramebufferWidth, g_vk.props.limits.maxFramebufferHeight}, 1);
}

/* Transform feedback's byte counters, a word for each of the 4 buffers: where a capture's end left each buffer, which
   the next draw's capture resumes from (3D 0x1D1; made at the first capture). Without the extension the CPU keeps
   them (end, in bytes from each buffer's start offset, an owner's at its counters' place): a draw in vertex order
   captures as many vertices as it has, in whole primitives, while they fit in every buffer */
static struct {
    VkBuffer buf;
    DevMem mem;
    u32 end[GPU_CHANNELS * 2][4];
} g_xfb;
static int xfb_counters(void) {
    if (g_xfb.buf) return 1;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = GPU_CHANNELS * 2 * 16,
                              .usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    DevMemWant aci = {.at = DM_DEVICE};
    if (!VK_OK(vk_buffer_create(&bci, &aci, &g_xfb.buf, &g_xfb.mem, NULL))) return 0;
    vk_mem_track(g_xfb.mem, VK_OWN_XFB);
    return 1;
}
static u64 xfb_counter_offset(u32 owner) { return ((u64)(owner & 0x7fffffff) + (owner >> 31 ? GPU_CHANNELS : 0)) * 16; }
/* by stores the CPU keeps the ends, so it reads and writes the control block (xfb.c's layout) itself */
static int xfb_control_stores(u32* end, u64 control, u32 what, u32 stride) {
    if (what > 3 || (control & 3) || (!control && what == 0)) return 0;
    if (!control) return 1; /* temporary pause/resume keeps the current ends */
    u8 block[16];
    if (what == 3) {
        if (gmem_read(control + 4, block, sizeof block)) return 0;
        for (u32 b = 0; b < 4; b++) end[b] = snx_ld_le32(block + 4 * b);
        return 1;
    }
    if (!what) {
        for (u32 b = 0; b < 4; b++) end[b] = 0;
        snx_st_le32(block, stride);
        return !gmem_write(control, block, 4);
    }
    for (u32 b = 0; b < 4; b++) snx_st_le32(block + 4 * b, end[b]);
    return !gmem_write(control + 4, block, sizeof block);
}
int vk_render_xfb_control(u32 owner_id, u64 control, u32 what, u32 stride, DrawOwner* owner) {
    if ((owner_id & 0x7fffffff) >= GPU_CHANNELS) return 0;
    if (g_spirv_translate.xfb_stores) {
        if (!xfb_control_stores(g_xfb.end[xfb_counter_offset(owner_id) / 16], control, what, stride)) return 0;
    } else {
        if (!g_vk.has_xfb || !xfb_counters()) return 0;
        vk_render_end(); /* counter transfers occur outside rendering */
        if (!vk_xfb_control_buffer(g_xfb.buf, xfb_counter_offset(owner_id), control, what, stride)) return 0;
    }
    if (!what) {
        owner->xfb_started = 0;
        owner->xfb_reload = 15;
    } else if (what == 3 && control) {
        owner->xfb_started = 15;
        owner->xfb_reload = 0;
    }
    return 1;
}
/* whether a draw captures its vertices (DsPrograms.xfb_on): by the extension once its counters exist, or by stores */
static int xfb_wanted(int on) {
    if (!on) return 0;
    return g_vk.has_xfb ? xfb_counters() : g_spirv_translate.xfb_stores;
}

/* a draw's capture by stores: the vertex stage's bindings and where each buffer's capture ends */
typedef struct {
    VkBuffer buf[5]; /* [4] the parameters */
    u64 off[5], size[5];
    SnxXfbRange range;
    u32 slot; /* the owner's place in g_xfb.end */
} XfbBind;
/* x for a capture by stores, in the hardware's order: direct point, line and triangle lists only (others are logged
   and skipped). A draw predicated on the GPU counts as drawn. 0 when a buffer can't be had */
static int xfb_bind(const DsState* ds, const DrawOwner* o, const VkSh* vs, int indexed, int indirect, u32 topology,
                    u32 first, u32 count, u32 base_instance, u32 instances, XfbBind* x) {
    const ShXfb* key = &vs->si.xfb;
    u32 per = topology == 0 ? 1 : topology == 1 ? 2 : topology == 4 ? 3 : 0; /* vertices a primitive */
    u32 n = per && !indexed && !indirect ? count - count % per : 0, stride[4], words[4], size[4], from[4], off = 0;
    u64 limit = (u64)n * instances;
    *x = (XfbBind){0};
    x->slot = (u32)(xfb_counter_offset(ds->owner) / 16);
    if (vs->si.ngbuf > SNX_XFB_GBUF_MAX) /* the translator left the stores out, and it has no log */
        LOG_LIMIT(1, "vulkan: a vertex program with %d global buffers can't store what transform feedback captures\n",
                  vs->si.ngbuf);
    if (!per || indexed || indirect)
        LOG_LIMIT(4, "vulkan: transform feedback of %s topology %u isn't captured without the extension\n",
                  indexed ? "an indexed" : indirect ? "an indirect" : "a", topology);
    if (limit > UINT32_MAX) { /* the program's vertex places are 32-bit */
        LOG_LIMIT(1, "vulkan: a transform feedback draw of %llu vertices isn't captured\n", (unsigned long long)limit);
        limit = 0;
    }
    if (x->slot >= GPU_CHANNELS * 2) { /* no place for its counters */
        LOG_LIMIT(1, "vulkan: transform feedback of draw owner %08x isn't captured\n", ds->owner);
        limit = 0;
    }
    for (u32 b = 0; b < 4; b++) {
        int on = ds->bind.xfb[b].on && (i32)ds->bind.xfb[b].size > 0;
        stride[b] = key->stride[b];
        words[b] = key->count[b];
        size[b] = on ? ds->bind.xfb[b].size : 0;
        from[b] = limit && (o->xfb_started & ~o->xfb_reload & (1u << b)) ? g_xfb.end[x->slot][b] : 0;
        off |= on ? 0 : 1u << b;
    }
    SnxXfbRange* r = &x->range;
    snx_xfb_range((u32)limit, per, stride, words, size, from, r);
    if (r->declared & off) /* not written, as the extension's path leaves it: the others still capture */
        LOG_LIMIT(1, "vulkan: transform feedback into a buffer that isn't enabled (that buffer gets nothing)\n");
    if (!r->declared) return 1;
    /* the bound mask: the program stores into a declared buffer only when the guest enabled it, as the hardware does */
    u32 params[5] = {first, base_instance, n, r->limit, r->bound};
    u8* p = vk_stage(sizeof params, vk_stage_align(), &x->buf[4], &x->off[4]);
    if (!p) return 0;
    memcpy(p, params, sizeof params);
    snx_le32_array(p, 5);
    x->size[4] = sizeof params;
    for (u32 b = 0; b < 4; b++) {
        if (!(r->declared >> b & 1)) continue;
        if (!(r->bound >> b & 1)) { /* nothing stored: a null buffer, or a buffer for the binding */
            if (vk_null_descriptors()) {
                x->buf[b] = VK_NULL_HANDLE;
                x->off[b] = 0;
                x->size[b] = VK_WHOLE_SIZE;
                continue;
            }
            if (!vk_buf_copy(0, 16, &x->buf[b], &x->off[b])) return 0;
            x->size[b] = 16;
            continue;
        }
        /* a writable copy of guest memory from the capture's start, which later draws read (vk_buf_copy_writable) */
        if (!vk_buf_copy_writable(ds->bind.xfb[b].addr + r->start[b], r->used[b], &x->buf[b], &x->off[b], 1)) return 0;
        x->size[b] = r->used[b];
    }
    return 1;
}
/* after the draw: each buffer's capture resumes from where it ended, until its write pointer is loaded again */
static void xfb_bind_done(DrawOwner* o, const XfbBind* x) {
    u32 bound = x->range.bound;
    for (u32 b = 0; b < 4; b++)
        if (bound >> b & 1) g_xfb.end[x->slot][b] = x->range.end[b];
    o->xfb_started = (u8)((o->xfb_started | bound) & 15);
    o->xfb_reload = (u8)(o->xfb_reload & ~bound);
}

/* Feedback loops. A Maxwell draw may sample a color target it renders to: a game may blend in its shaders,
   reading the pixel under the fragment and writing it back, in hundreds of draws a frame. With
   VK_EXT_attachment_feedback_loop_layout the target stays the pass's attachment, in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL,
   which the draw's descriptor names too, and the draw enables the loop for color (dynamic state, or a pipeline flag).
   A barrier orders the color attachment writes and fragment shader reads before it against those after it, both ways.
   A draw gets one where the guest ordered it: NVN's ORDER_FRAGMENTS or INVALIDATE_TEXTURE, the 3D engine's fragment
   barrier or texture cache invalidation (vk_render_fragment_barrier), came after a draw that wrote a slot this one
   samples, or sampled a slot this one writes. Between those, the console doesn't order a texture read of the target
   against the writes of earlier draws either: the texture cache is not coherent with the ROP's writes until the
   invalidation, and the game asks for one where it needs the result. In one game's open world, 2,679 draws a frame
   sampled the G-buffer targets they drew to with 134 fragment barriers a frame, none between them: a barrier before
   each one, as before, took 4.1 of the GPU's 10.9 ms a frame. A clear (vkCmdClearAttachments) of a slot that a draw
   sampled keeps the barrier: strict.

   Within a vkCmdBeginRendering pass a barrier is allowed only with dynamicRenderingLocalRead
   (VUID-vkCmdPipelineBarrier2-None-09553), with framebuffer-space stages only (-srcStageMask-09556), by region
   (-dependencyFlags-07891), and with no image barrier on an attachment outside RENDERING_LOCAL_READ or GENERAL
   (-image-09555): so it is a memory barrier, and the layout stays. By region, the specification orders a write at
   (x, y, layer) only before reads by fragments at the same (x, y, layer) ("Framebuffer Region Dependencies"). That is
   all a fragment reading the pixel it covers needs, which is what blending in a shader does. A fragment reading other
   pixels (a blur of the target) isn't ordered by it in the specification: an immediate-mode desktop GPU drains and
   flushes for the barrier as a whole and orders those reads too in practice, while a tiler may order only within its
   tile. Without dynamicRenderingLocalRead, the pass ends and begins again instead, and render_begin's image barrier,
   outside the pass, orders every pixel.

   Eden ends its render pass for each such draw and samples the image in GENERAL (TextureCache::CheckFeedbackLoop,
   BarrierFeedbackLoop). Before this, the target was copied for each draw (vk_rt_snapshot), which stays for devices
   without the extension, for stages before the fragment shader, and for a depth target the draw writes.

   Orders a command of the open pass that samples the color slots in read and writes those in write: against every
   earlier command of the pass where strict, else against those before the guest's last fragment barrier. 0 when the
   pass could not be begun again */
static int feedback_order(u32 read, u32 write, int strict) {
    read &= g_rp.fb;
    write &= g_rp.fb;
    /* a save attachment of a feedback draw drawn two times (predraw_run) is ordered at each use: its first draw
       writes it and its second draw reads it, with no guest barrier between them */
    u32 saves = 0;
    for (u32 i = 0; i < g_rp.n; i++)
        if (g_rp.rt[i] && g_rp.rt[i]->predraw) saves |= 1u << i;
    u32 written = (strict ? g_rp.fb_written : g_rp.due_written) | (g_rp.fb_written & saves),
        sampled = (strict ? g_rp.fb_read : g_rp.due_read) | (g_rp.fb_read & saves);
    if (!(read & written) && !(write & sampled)) return 1;
    if (!g_vk.has_local_read) {
        Surf* rt[8];
        SNX_ZONE(z, "vk pass restart");
        memcpy(rt, g_rp.rt, sizeof rt);
        vk_render_end();
        g_vk_fb_restarts++;
        return render_begin(rt, g_rp.zt, g_rp.n, g_rp.zt_ro, g_rp.srgb_write, g_rp.fb, g_rp.ext);
    }
    VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    /* FEEDBACK_LOOP says what the barrier is for, to drivers that flush differently for it */
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                           .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT,
                           .memoryBarrierCount = 1,
                           .pMemoryBarriers = &mb};
    rp_record(); /* the barrier is the pass's, after what began it */
    rec_CmdPipelineBarrier2(vk_cmd(), &di);
    g_rp.fb_written = g_rp.fb_read = g_rp.due_written = g_rp.due_read = 0;
    g_vk_fb_barriers++;
    return 1;
}
void vk_render_fragment_barrier(void) {
    if (!g_rp.open) return;
    g_rp.due_written |= g_rp.fb_written;
    g_rp.due_read |= g_rp.fb_read;
}
/* a command of the open pass sampled the color slots in read and wrote those in write */
static void feedback_done(u32 read, u32 write) {
    g_rp.fb_read |= read & g_rp.fb;
    g_rp.fb_written |= write & g_rp.fb;
}

/* every dynamic state at a neutral value, for draws of our own */
static void pr_dynamic(VkCommandBuffer cb, const PipeKey* k, int minus_one, int fresh);
static void plain_state(VkCommandBuffer cb, VkRect2D sc) {
    vk_render_forget_state(); /* every state set here, differently from a draw */
    VkViewport vp = {0, 0, (float)g_rp.ext.width, (float)g_rp.ext.height, 0, 1};
    rec_CmdSetViewportWithCount(cb, 1, &vp);
    rec_CmdSetScissorWithCount(cb, 1, &sc);
    rec_CmdSetCullMode(cb, VK_CULL_MODE_NONE);
    rec_CmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    rec_CmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    rec_CmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    rec_CmdSetRasterizerDiscardEnable(cb, VK_FALSE);
    rec_CmdSetLineWidth(cb, 1.0f);
    rec_CmdSetDepthBiasEnable(cb, VK_FALSE);
    rec_CmdSetDepthBias(cb, 0, 0, 0);
    float bc[4] = {0, 0, 0, 0};
    rec_CmdSetBlendConstants(cb, bc);
    rec_CmdSetDepthTestEnable(cb, VK_FALSE);
    rec_CmdSetDepthWriteEnable(cb, VK_FALSE);
    rec_CmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    rec_CmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    rec_CmdSetDepthBounds(cb, 0.0f, 1.0f);
    rec_CmdSetStencilTestEnable(cb, VK_FALSE);
    rec_CmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                        VK_COMPARE_OP_ALWAYS);
    rec_CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    rec_CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    rec_CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    if (g_vk.has_feedback_dynamic) rec_CmdSetAttachmentFeedbackLoopEnableEXT(cb, VK_IMAGE_ASPECT_NONE);
    if (g_vk.vertex_input_dynamic) rec_CmdSetVertexInputEXT(cb, 0, NULL, 0, NULL); /* our draws read no vertices */
    const PipeKey plain = {.polygon = VK_POLYGON_MODE_FILL}; /* the clears' keys: fill, no clamp, no tessellation */
    pr_dynamic(cb, &plain, 0, 1);                            /* and their depths, from 0 to 1 */
}

/* a channel's render enable predicate (gpu.h render_pred, 0 none) around one of its draws or clears, inside the
   rendering pass they are in: on the GPU, the comparison it holds decides whether they run */
static void pred_begin(VkCommandBuffer cb, u32 pred) {
    if (pred)
        rec_CmdBeginConditionalRenderingEXT(cb, vk_query_pred_buffer(), (VkDeviceSize)((pred & ~VK_PRED_EQUAL) - 1) * 4,
                                            (pred & VK_PRED_EQUAL) ? VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT : 0);
}
static void pred_end(VkCommandBuffer cb, u32 pred) {
    if (pred) rec_CmdEndConditionalRenderingEXT(cb);
}

static struct {
    VkShaderModule vs, fs[8][3];
} g_clear;

/* a clear of some color components only: a full-screen triangle with that write mask, whose fragment shader writes
   the cleared slot alone, in the target's number type, run on pred */
static int clear_masked(u32 slot, u32 mask, const float* cc, VkRect2D rect, u32 layer, u32 pred) {
    rp_record();
    const VkFmt* f = &g_rp.rt[slot]->f;
    int num = f->is_int ? (f->sint ? 2 : 1) : 0;
    if (!g_clear.vs) g_clear.vs = vk_module_spv(snx_clear_vs_spv, snx_clear_vs_spv_size / sizeof *snx_clear_vs_spv);
    if (!g_clear.fs[slot][num]) {
        static const u32* words[3] = {snx_clear_fs_float_spv, snx_clear_fs_uint_spv, snx_clear_fs_sint_spv};
        const u64 bytes[3] = {snx_clear_fs_float_spv_size, snx_clear_fs_uint_spv_size, snx_clear_fs_sint_spv_size};
        g_clear.fs[slot][num] = vk_module_spv_location(words[num], bytes[num] / sizeof(u32), slot);
    }
    if (!g_clear.vs || !g_clear.fs[slot][num]) return 0;
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.vs = g_clear.vs;
    k.fs = g_clear.fs[slot][num];
    k.topo_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    k.polygon = VK_POLYGON_MODE_FILL;
    k.ncolor = g_rp.n;
    for (u32 i = 0; i < g_rp.n; i++)
        k.color[i] =
            g_rp.rt[i] ? (g_rp.srgb_write ? g_rp.rt[i]->f.vk : vk_srgb_as(g_rp.rt[i]->f.vk, 0)) : VK_FORMAT_UNDEFINED;
    k.samples = vk_surf_samples(g_rp.rt[slot]); /* every sample of the pass's targets */
    k.blend[slot].mask = ((mask & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((mask & 2) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                         ((mask & 4) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((mask & 8) ? VK_COLOR_COMPONENT_A_BIT : 0);
    if (g_rp.zt) {
        k.depth = g_rp.zt->f.vk;
        k.stencil = g_rp.zt->f.stencil ? g_rp.zt->f.vk : VK_FORMAT_UNDEFINED;
    }
    VkPipeline p = vk_pipeline_get(&k);
    if (!p) return 0;
    VkCommandBuffer cb = vk_cmd();
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vk_vrs_keep(cb);
    plain_state(cb, rect);
    u32 pc[8] = {0};
    pc[0] = layer;
    memcpy(&pc[4], cc, 16); /* the register bits, read as the shader's vec4, uvec4 or ivec4 */
    rec_CmdPushConstants(cb, g_vk_playout, VK_PUSH_STAGES, 0, 32, pc);
    vk_query_pass_end(cb); /* a clear passes no samples on the console: its draw here isn't counted */
    pred_begin(cb, pred);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    pred_end(cb, pred);
    return 1;
}

/* Preserve unselected stencil bits with fixed-function REPLACE and a write mask. The triangle has no fragment
   output, color writes or depth writes. It covers only the requested rectangle and layer, under the GPU predicate. */
static int clear_stencil_masked(u32 mask, u32 value, VkRect2D rect, u32 layer, u32 pred) {
    rp_record();
    if (!g_clear.vs) g_clear.vs = vk_module_spv(snx_clear_vs_spv, snx_clear_vs_spv_size / sizeof *snx_clear_vs_spv);
    if (!g_clear.vs) return 0;
    PipeKey k;
    memset(&k, 0, sizeof k); /* pipeline keys compare every byte */
    k.vs = g_clear.vs;
    k.topo_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    k.polygon = VK_POLYGON_MODE_FILL;
    k.ncolor = g_rp.n;
    for (u32 i = 0; i < g_rp.n; i++)
        k.color[i] =
            g_rp.rt[i] ? (g_rp.srgb_write ? g_rp.rt[i]->f.vk : vk_srgb_as(g_rp.rt[i]->f.vk, 0)) : VK_FORMAT_UNDEFINED;
    k.samples = vk_surf_samples(g_rp.zt);
    k.depth = k.stencil = g_rp.zt->f.vk;
    VkPipeline pipeline = vk_pipeline_get(&k);
    if (!pipeline) return 0;
    VkCommandBuffer cb = vk_cmd();
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vk_vrs_keep(cb);
    plain_state(cb, rect);
    rec_CmdSetStencilTestEnable(cb, VK_TRUE);
    rec_CmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE,
                        VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
    rec_CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, value & 0xff);
    rec_CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    rec_CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, mask & 0xff);
    u32 pc[8] = {0};
    pc[0] = layer;
    rec_CmdPushConstants(cb, g_vk_playout, VK_PUSH_STAGES, 0, sizeof pc, pc);
    vk_query_pass_end(cb);
    pred_begin(cb, pred);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    pred_end(cb, pred);
    return 1;
}

/* gpu_trace_target=address[:bytes]: log the clears and draws that write a render target starting in [address,
   address + bytes) (one byte without a size), to find what produced a wrong target. Read on first use: draws and
   clears run on the GPU thread */
static int trace_target(const Surf* s) {
    static int read;
    static u64 lo, bytes;
    if (!read) {
        read = 1;
        const char* v = g_settings.gpu_trace_target;
        if (*v) {
            char *end, *size_end = NULL;
            lo = strtoull(v, &end, 0);
            bytes = *end == ':' ? strtoull(end + 1, &size_end, 0) : 1;
            if (end == v || (*end && *end != ':') || (size_end && (*size_end || size_end == end + 1)) || !bytes) {
                WARN("gpu_trace_target=%s isn't address[:bytes] with a nonzero size (0x for hex): no trace\n", v);
                bytes = 0;
            }
        }
    }
    return bytes && s && s->gva >= lo && s->gva - lo < bytes;
}

/* the clear c of the targets ds names, under the owner's render predicate */
static void clear_targets(const DsState* ds, const DrawClear* c, const DrawOwner* o, Surf** rt, Surf* zt,
                            u32 n, VkExtent2D area) {
    u32 slot = c->slot, mask = c->color_mask, layer = c->layer, v = c->aspects;
    float cc[4], dz = c->depth;
    memcpy(cc, c->color, 16);
    dz = dz > 1 ? 1 : dz >= 0 ? dz : 0; /* Vulkan takes 0..1 only */
    for (u32 i = 0; mask && i < n; i++) /* the slot the clear takes is resolved below: every bound one is listed */
        if (trace_target(rt[i]))
            LOG_LIMIT(64,
                      "gpu-trace-target: color clear of slot %u, bound slot %u target=%llx %ux%u mask=%x "
                      "value=%08x,%08x,%08x,%08x\n",
                      slot, i, (unsigned long long)rt[i]->gva, rt[i]->w, rt[i]->h, mask, c->color[0], c->color[1],
                      c->color[2], c->color[3]);
    if ((v & 3) && trace_target(zt))
        LOG_LIMIT(64, "gpu-trace-target: clear depth target=%llx %ux%u depth=%d stencil=%d value=%g\n",
                  (unsigned long long)zt->gva, zt->w, zt->h, v & 1, (v >> 1) & 1, dz);
    int zt_undefined = zt && zt->layout == VK_IMAGE_LAYOUT_UNDEFINED;
    if (!render_begin(rt, zt, n, 0, ds->tgt.srgb_write, 0, area)) return;
    u32 layers = ~0u;
    for (u32 i = 0; i < g_rp.n; i++)
        if (g_rp.rt[i] && g_rp.rt[i]->layers < layers) layers = g_rp.rt[i]->layers;
    if (g_rp.zt && g_rp.zt->layers < layers) layers = g_rp.zt->layers;
    if (layer >= layers) return;
    VkClearRect rect = {{{0, 0}, g_rp.ext}, layer, 1};
    if (c->scissored) {
        u32 xs = ds->vp.scissor_x[0], ys = ds->vp.scissor_y[0];
        i32 x0 = (i32)vk_scf(xs & 0xFFFF), x1 = (i32)vk_scf(xs >> 16), y0 = (i32)vk_scf(ys & 0xFFFF),
            y1 = (i32)vk_scf(ys >> 16);
        if (ds->dyn.window_origin & DS_ORIGIN_LOWER_LEFT) {
            i32 clip_height = (i32)vk_scf((float)ds->dyn.clip_height);
            i32 old_y0 = y0;
            y0 = clip_height - y1;
            y1 = clip_height - old_y0;
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (i32)g_rp.ext.width) x1 = (i32)g_rp.ext.width;
        if (y1 > (i32)g_rp.ext.height) y1 = (i32)g_rp.ext.height;
        if (x1 <= x0 || y1 <= y0) return;
        rect.rect = (VkRect2D){{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
    }
    /* the color slot it writes, after the pass's draws that sampled it */
    u32 written = mask && slot < g_rp.n && g_rp.rt[slot] ? 1u << slot : 0;
    if (!feedback_order(0, written, 1)) return;
    feedback_done(0, written);
    int color = mask && slot < g_rp.n && g_rp.rt[slot], color_clear = color && mask == 15;
    VkImageAspectFlags za = 0; /* the depth target's aspects it clears */
    if ((v & 3) && zt)
        za = ((v & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((v & 2) && zt->f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    u32 stencil_mask = ~c->stencil_preserve & 0xff;
    int masked_stencil = (za & VK_IMAGE_ASPECT_STENCIL_BIT) && stencil_mask && stencil_mask != 0xff;
    if (!stencil_mask || masked_stencil) za &= ~VK_IMAGE_ASPECT_STENCIL_BIT;
    /* Whole attachments before anything else of the pass: the load ops it begins with clear them (rp_record), in place
       of a load and vkCmdClearAttachments. Not a clear of one layer of several (a load op clears every layer the pass
       renders), of less than the render area (its scissor: one game scissors every clear, to the whole target in most)
       or a predicated one (a load op clears the render area, always), or a color clear of some components only */
    int whole = rect.rect.offset.x == 0 && rect.rect.offset.y == 0 && rect.rect.extent.width == g_rp.ext.width &&
                rect.rect.extent.height == g_rp.ext.height;
    if (!g_rp.begun && !o->pred && layers == 1 && whole && (!color || color_clear) && (color_clear || za)) {
        if (color_clear) {
            VkRenderingAttachmentInfo* a = &g_rp.ca[slot];
            a->loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            if (g_rp.rt[slot]->f.is_int)
                memcpy(a->clearValue.color.uint32, c->color, 16);
            else
                memcpy(a->clearValue.color.float32, cc, 16);
        }
        if (za & VK_IMAGE_ASPECT_DEPTH_BIT) {
            g_rp.za.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            g_rp.za.clearValue.depthStencil.depth = dz;
        }
        if (za & VK_IMAGE_ASPECT_STENCIL_BIT) {
            g_rp.zs.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            g_rp.zs.clearValue.depthStencil.stencil = c->stencil & 0xFF;
        }
        g_rp.clears = 1;
    } else {
        rp_record();
        VkClearAttachment at[2];
        u32 na = 0;
        if (color_clear) {
            at[na] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = slot};
            /* an integer target's clear color registers hold the integers themselves, not floats to convert: an
               R8_Uint clear of 185 arrives as 000000b9 */
            if (g_rp.rt[slot]->f.is_int)
                memcpy(at[na].clearValue.color.uint32, c->color, 16);
            else
                memcpy(at[na].clearValue.color.float32, cc, 16);
            na++;
        } else if (color) {
            if (clear_masked(slot, mask, cc, rect.rect, layer, o->pred)) target_written(g_rp.rt[slot]);
        }
        if (za) {
            at[na] = (VkClearAttachment){.aspectMask = za};
            at[na].clearValue.depthStencil = (VkClearDepthStencilValue){dz, c->stencil & 0xFF};
            na++;
        }
        if (na) {
            pred_begin(vk_cmd(), o->pred);
            rec_CmdClearAttachments(vk_cmd(), na, at, 1, &rect);
            pred_end(vk_cmd(), o->pred);
            g_vk_clear_cmds++;
        }
    }
    /* a predicated clear may not run: it covers no target whole */
    if (color_clear) {
        Surf* s = g_rp.rt[slot];
        vk_surf_wrote(s);
        if (!o->pred && s->layers == 1 && rect.rect.offset.x == 0 && rect.rect.offset.y == 0 &&
            rect.rect.extent.width == s->sw && rect.rect.extent.height == s->sh)
            vk_rt_drawn_full(s);
        else
            vk_rt_drawn(s);
    }
    if (masked_stencil && clear_stencil_masked(stencil_mask, c->stencil, rect.rect, layer, o->pred))
        za |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (za && zt_written(zt, o->pred, zt_undefined)) {
        vk_surf_wrote(zt);
        int all_aspects = (v & 1) && (!zt->f.stencil || ((v & 2) && stencil_mask == 0xff));
        if (!o->pred && all_aspects && zt->layers == 1 && rect.rect.offset.x == 0 && rect.rect.offset.y == 0 &&
            rect.rect.extent.width == zt->sw && rect.rect.extent.height == zt->sh)
            vk_rt_drawn_full(zt);
        else
            vk_rt_drawn(zt);
    }
}

void vk_render_clear(const DsState* ds, const DrawClear* c, const DrawOwner* o) {
    Surf *rt[8], *zt;
    u32 n;
    read_targets(ds, rt, &zt, &n);
    VkExtent2D area = target_extent(rt, zt, n);
    int mixed = 0;
    for (u32 i = 0; zt && i < n; i++)
        if (rt[i] && vk_surf_samples(rt[i]) != vk_surf_samples(zt)) mixed = 1;
    u32 stencil_mask = ~c->stencil_preserve & 255;
    int masked = (c->color_mask && c->color_mask != 15) ||
                 ((c->aspects & 2) && stencil_mask && stencil_mask != 255);
    /* Native attachment clears need no draw pipeline. Masked clears do, so separate unlike attachments rather
       than bind a pipeline whose single sample count cannot describe both. Other hosts also require equal
       samples at BeginRendering; their color/depth clears always use separate scopes. */
    if (mixed && (!g_vk.has_mixed_samples || masked)) {
        u32 layers = zt->layers;
        for (u32 i = 0; i < n; i++)
            if (rt[i] && rt[i]->layers < layers) layers = rt[i]->layers;
        if (c->layer >= layers) return;
        if (c->color_mask) {
            DrawClear color = *c;
            color.aspects = 0;
            clear_targets(ds, &color, o, rt, NULL, n, area);
        }
        if (c->aspects & 3) {
            DrawClear depth = *c;
            Surf* none[8] = {0};
            depth.color_mask = 0;
            clear_targets(ds, &depth, o, none, zt, 0, area);
        }
    } else
        clear_targets(ds, c, o, rt, zt, n, area);
}

/* targets of ds (bit i color slot i, bit 8 the depth target) whose contents no longer matter: the open pass, before
   anything of it, loads them as DONT_CARE; otherwise their next pass does, unless something writes them first */
void vk_render_discard(const DsState* ds, u32 targets) {
    Surf* rt[8];
    Surf* zt;
    u32 n;
    read_targets(ds, rt, &zt, &n);
    for (u32 i = 0; i < 9; i++) {
        Surf* s = i < 8 ? (i < n && targets >> i & 1 ? rt[i] : NULL) : (targets >> 8 & 1 ? zt : NULL);
        if (!s) continue;
        if (g_rp.open && !g_rp.begun) { /* the pass of these targets hasn't begun: its load ops discard them */
            int found = 0;
            for (u32 j = 0; j < g_rp.n; j++)
                if (g_rp.rt[j] == s) {
                    if (g_rp.ca[j].loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR)
                        g_rp.ca[j].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    found = 1;
                }
            if (g_rp.zt == s) {
                if (g_rp.za.loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR) g_rp.za.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                if (g_rp.zs.loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR) g_rp.zs.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                found = 1;
            }
            if (found) continue;
        }
        s->discarded = 1;
        s->discard_wseq = s->wseq;
    }
}

/* ------------------------------------------------------------ register values -> Vulkan */
/* the draw state's enumerants are Vulkan's (gpu/draw_state.h) */
typedef char ds_blend_vk[DS_BLEND_ADD == VK_BLEND_OP_ADD && DS_BLEND_SUBTRACT == VK_BLEND_OP_SUBTRACT &&
                                 DS_BLEND_REVERSE_SUBTRACT == VK_BLEND_OP_REVERSE_SUBTRACT &&
                                 DS_BLEND_MIN == VK_BLEND_OP_MIN && DS_BLEND_MAX == VK_BLEND_OP_MAX
                             ? 1
                             : -1];
typedef char ds_factor_vk[DS_FACTOR_ZERO == VK_BLEND_FACTOR_ZERO && DS_FACTOR_ONE == VK_BLEND_FACTOR_ONE &&
                                  DS_FACTOR_SRC_COLOR == VK_BLEND_FACTOR_SRC_COLOR &&
                                  DS_FACTOR_ONE_MINUS_SRC_COLOR == VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR &&
                                  DS_FACTOR_DST_COLOR == VK_BLEND_FACTOR_DST_COLOR &&
                                  DS_FACTOR_ONE_MINUS_DST_COLOR == VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR &&
                                  DS_FACTOR_SRC_ALPHA == VK_BLEND_FACTOR_SRC_ALPHA &&
                                  DS_FACTOR_ONE_MINUS_SRC_ALPHA == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA &&
                                  DS_FACTOR_DST_ALPHA == VK_BLEND_FACTOR_DST_ALPHA &&
                                  DS_FACTOR_ONE_MINUS_DST_ALPHA == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA &&
                                  DS_FACTOR_CONSTANT_COLOR == VK_BLEND_FACTOR_CONSTANT_COLOR &&
                                  DS_FACTOR_ONE_MINUS_CONSTANT_COLOR == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR &&
                                  DS_FACTOR_CONSTANT_ALPHA == VK_BLEND_FACTOR_CONSTANT_ALPHA &&
                                  DS_FACTOR_ONE_MINUS_CONSTANT_ALPHA == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA &&
                                  DS_FACTOR_SRC_ALPHA_SATURATE == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE &&
                                  DS_FACTOR_SRC1_COLOR == VK_BLEND_FACTOR_SRC1_COLOR &&
                                  DS_FACTOR_ONE_MINUS_SRC1_COLOR == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR &&
                                  DS_FACTOR_SRC1_ALPHA == VK_BLEND_FACTOR_SRC1_ALPHA &&
                                  DS_FACTOR_ONE_MINUS_SRC1_ALPHA == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA
                              ? 1
                              : -1];
typedef char ds_component_vk[DS_COMPONENT_R == VK_COLOR_COMPONENT_R_BIT && DS_COMPONENT_G == VK_COLOR_COMPONENT_G_BIT &&
                                     DS_COMPONENT_B == VK_COLOR_COMPONENT_B_BIT &&
                                     DS_COMPONENT_A == VK_COLOR_COMPONENT_A_BIT
                                 ? 1
                                 : -1];
typedef char ds_polygon_vk[DS_POLYGON_FILL == VK_POLYGON_MODE_FILL && DS_POLYGON_LINE == VK_POLYGON_MODE_LINE &&
                                   DS_POLYGON_POINT == VK_POLYGON_MODE_POINT
                               ? 1
                               : -1];
typedef char ds_cull_vk[DS_CULL_NONE == VK_CULL_MODE_NONE && DS_CULL_FRONT == VK_CULL_MODE_FRONT_BIT &&
                                DS_CULL_BACK == VK_CULL_MODE_BACK_BIT &&
                                DS_CULL_FRONT_AND_BACK == VK_CULL_MODE_FRONT_AND_BACK
                            ? 1
                            : -1];
typedef char
    ds_cmp_vk[DS_CMP_NEVER == VK_COMPARE_OP_NEVER && DS_CMP_LESS == VK_COMPARE_OP_LESS &&
                      DS_CMP_EQUAL == VK_COMPARE_OP_EQUAL && DS_CMP_LESS_OR_EQUAL == VK_COMPARE_OP_LESS_OR_EQUAL &&
                      DS_CMP_GREATER == VK_COMPARE_OP_GREATER && DS_CMP_NOT_EQUAL == VK_COMPARE_OP_NOT_EQUAL &&
                      DS_CMP_GREATER_OR_EQUAL == VK_COMPARE_OP_GREATER_OR_EQUAL && DS_CMP_ALWAYS == VK_COMPARE_OP_ALWAYS
                  ? 1
                  : -1];
typedef char ds_stencil_vk[DS_STENCIL_KEEP == VK_STENCIL_OP_KEEP && DS_STENCIL_ZERO == VK_STENCIL_OP_ZERO &&
                                   DS_STENCIL_REPLACE == VK_STENCIL_OP_REPLACE &&
                                   DS_STENCIL_INCREMENT_AND_CLAMP == VK_STENCIL_OP_INCREMENT_AND_CLAMP &&
                                   DS_STENCIL_DECREMENT_AND_CLAMP == VK_STENCIL_OP_DECREMENT_AND_CLAMP &&
                                   DS_STENCIL_INVERT == VK_STENCIL_OP_INVERT &&
                                   DS_STENCIL_INCREMENT_AND_WRAP == VK_STENCIL_OP_INCREMENT_AND_WRAP &&
                                   DS_STENCIL_DECREMENT_AND_WRAP == VK_STENCIL_OP_DECREMENT_AND_WRAP
                               ? 1
                               : -1];

/* vertex attribute format from the size code (bits 21-26) and component type (27-29): 1 snorm, 2 unorm, 3 sint,
   4 uint, 5 uscaled, 6 sscaled, 7 float */
static VkFormat attr_format(u32 size, u32 ty, int bgra) {
#define BY(sn, un, si, ui, us, ss, fl)                                                                                 \
    (ty == 1 ? sn : ty == 2 ? un : ty == 3 ? si : ty == 4 ? ui : ty == 5 ? us : ty == 6 ? ss : fl)
    /* Vulkan has no 32-bit scaled formats, and an integer format would hand the float input the integers' bits */
    if ((ty == 5 || ty == 6) && (size == 0x01 || size == 0x02 || size == 0x04 || size == 0x12)) {
        LOG_LIMIT(1, "vulkan: 32-bit scaled vertex attributes aren't done yet (read as 0,0,0,1)\n");
        return VK_FORMAT_UNDEFINED;
    }
    switch (size) {
        case 0x01:
            return BY(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_SINT,
                      VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                      VK_FORMAT_R32G32B32A32_SFLOAT);
        case 0x02:
            return BY(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_UINT,
                      VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32_SFLOAT);
        case 0x04:
            return BY(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_UINT,
                      VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32_SFLOAT);
        case 0x12:
            return BY(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SINT, VK_FORMAT_R32_UINT,
                      VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SFLOAT);
        case 0x03:
            return BY(VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SINT,
                      VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED,
                      VK_FORMAT_R16G16B16A16_SFLOAT);
        case 0x05:
            return BY(VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SINT,
                      VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16_SSCALED,
                      VK_FORMAT_R16G16B16_SFLOAT);
        case 0x0F:
            return BY(VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_UINT,
                      VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_SFLOAT);
        case 0x1B:
            return BY(VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SINT, VK_FORMAT_R16_UINT,
                      VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED, VK_FORMAT_R16_SFLOAT);
        case 0x0A:
            if (bgra && (ty == 2 || ty == 7)) return VK_FORMAT_B8G8R8A8_UNORM;
            return BY(VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SINT,
                      VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED,
                      VK_FORMAT_R8G8B8A8_UNORM);
        case 0x13:
            return BY(VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8_UINT,
                      VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8_UNORM);
        case 0x18:
            return BY(VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_UINT,
                      VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8_UNORM);
        case 0x1D:
            return BY(VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SINT, VK_FORMAT_R8_UINT,
                      VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED, VK_FORMAT_R8_UNORM);
        case 0x30: /* the type decides the shader's input type too (integer or float): each gets its own format */
            return BY(VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                      VK_FORMAT_A2B10G10R10_SINT_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32,
                      VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                      VK_FORMAT_A2B10G10R10_UNORM_PACK32);
        case 0x31: return ty == 3 || ty == 4 ? VK_FORMAT_UNDEFINED : VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    }
#undef BY
    return VK_FORMAT_UNDEFINED;
}
static int vertex_format_ok(VkFormat f) {
    static VkFormat ok[64], bad[64];
    static int nok, nbad;
    if (f == VK_FORMAT_UNDEFINED) return 0;
    for (int i = 0; i < nok; i++)
        if (ok[i] == f) return 1;
    for (int i = 0; i < nbad; i++)
        if (bad[i] == f) return 0;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    int r = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    if (r && nok < 64) ok[nok++] = f;
    if (!r && nbad < 64) {
        bad[nbad++] = f;
        WARN("vulkan: the GPU can't read vertex format %d (those attributes read as 0,0,0,1)\n", (int)f);
    }
    return r;
}

/* bytes an attribute of a size code (bits 21-26) reads */
static u32 attr_bytes(u32 size) {
    switch (size) {
        case 0x01: return 16;
        case 0x02: return 12;
        case 0x03:
        case 0x04: return 8;
        case 0x05: return 6;
        case 0x13: return 3;
        case 0x18:
        case 0x1B: return 2;
        case 0x1D: return 1;
    }
    return 4; /* 0x0A, 0x0F, 0x12, 0x30, 0x31 */
}
/* the bytes of a component of a vertex attribute of that size, which Vulkan fetches it only from multiples of: a packed
   format's word as one */
static u32 attr_component_bytes(u32 size) {
    switch (size) {
        case 0x03:
        case 0x05:
        case 0x0F:
        case 0x1B: return 2;
        case 0x0A:
        case 0x13:
        case 0x18:
        case 0x1D: return 1;
    }
    return 4;
}

/* ------------------------------------------------------------ resources of a draw */
/* the word at byte off of stage's constant buffer bank, as ds binds it: an inline block's, or guest memory's (0 for
   an unbound buffer) */
static u32 cb_word(const DsState* ds, int stage, u32 bank, u64 off) {
    DsCbBlock in = ds_cb_inline(ds, stage, bank);
    u64 a = bank < DS_CBUFS ? ds->bind.cb_addr[stage][bank] : 0;
    return in.bytes ? ds_inline_word(in, off) : a ? gmem_r32(a + off) : 0;
}
static u32 tex_handle_cb(const DsState* ds, int stage, u32 tcb, const ShTex* st) {
    u32 ci = (st->bindless ? st->bl_cbuf : tcb) & 31, c2i = st->bl2_cbuf & 31;
    u32 h = cb_word(ds, stage, ci, st->cbuf_off * 4);
    if (st->bindless && st->bl2)
        h |= st->bl2_cbuf == SH_HANDLE_IMM ? st->bl2_off : cb_word(ds, stage, c2i, st->bl2_off * 4);
    return h;
}
/* Handles an Ounce program loads from memory (SNX_TEX_MEMORY): the instructions that make them (ShaderInfo.memh)
   run on the draw's constant buffers and the guest memory they load from, a pending write there landed first (as
   gbuf_pointer reads a pointer). A handle in a uniform register pair is the texture's word with the sampler's index
   above it, as a constant buffer's handle holds them; a TEX.B's is its register. Run once a draw for each program
   (vk_memory_handles) */
static u32 memh_cbuf(void* user, u32 bank, u32 off) {
    const DsState* ds = ((const void**)user)[0];
    int stage = (int)(intptr_t)((const void**)user)[1];
    return cb_word(ds, stage, bank, off);
}
int vk_memh_mem(void* user, u64 address, u32* words, u32 count) {
    (void)user;
    u64 cpu = 0;
    if (!address || gmmu_run(address, 4ull * count, &cpu) < 4ull * count) return 0;
    vk_buf_mem_need(address, 4ull * count);
    vk_gmem_words(address, words, count);
    return 1;
}
u32 vk_memory_handles(const ShaderInfo* si, const struct Sm86EvalIo* io, u32* out) {
    u32 probes[2 * SH_MAXTEX], values[2 * SH_MAXTEX], np = 0, found = 0;
    int nt = si->ntex > SH_MAXTEX ? SH_MAXTEX : si->ntex;
    for (int i = 0; i < nt; i++) {
        const ShTex* st = &si->tex[i];
        out[i] = 0;
        if (!(st->flags & SNX_TEX_MEMORY) || st->cbuf_off == SH_PROBE_NONE) continue;
        probes[np++] = st->cbuf_off;
        probes[np++] = st->bl2 ? st->bl2_off : st->cbuf_off;
    }
    if (np && !sm86_eval((const u64(*)[2])si->memh, si->nmemh, io, probes, np, values))
        LOG_LIMIT(4, "vulkan: a program's texture handles from memory could not be read (some are 0)\n");
    for (int i = 0, k = 0; i < nt; i++) {
        const ShTex* st = &si->tex[i];
        if (!(st->flags & SNX_TEX_MEMORY)) continue;
        found++;
        if (st->cbuf_off == SH_PROBE_NONE) { /* past any pool's limit: no texture */
            out[i] = 0xFFFFFFFFu;
            continue;
        }
        out[i] = st->bl2 ? (values[k] & 0xFFFFFu) | values[k + 1] << 20 : values[k];
        k += 2;
    }
    return found;
}
/* the draw's handles of a program's memory slots, kept for the draw (g_memh_draw counts the draws) */
static u64 g_memh_draw;
static struct {
    const ShaderInfo* si;
    u64 draw;
    int stage;
    u32 h[SH_MAXTEX];
} g_memh[4];
static u32 memh_handle(const DsState* ds, int stage, const ShaderInfo* si, int i) {
    u32 at = 0;
    while (at < 4 && !(g_memh[at].si == si && g_memh[at].draw == g_memh_draw && g_memh[at].stage == stage)) at++;
    if (at == 4) {
        at = (u32)(g_memh_draw + (u64)stage) & 3;
        const void* user[2] = {ds, (const void*)(intptr_t)stage};
        Sm86EvalIo io = {(void*)user, memh_cbuf, vk_memh_mem};
        vk_memory_handles(si, &io, g_memh[at].h);
        g_memh[at].si = si, g_memh[at].draw = g_memh_draw, g_memh[at].stage = stage;
    }
    return g_memh[at].h[i];
}
/* the handle of si's texture slot i: its constant buffer's words (tex_handle_cb), or what the program loads from
   memory (memh_handle) */
static u32 tex_handle(const DsState* ds, int stage, u32 tcb, const ShaderInfo* si, int i) {
    if (si->tex[i].flags & SNX_TEX_MEMORY) return memh_handle(ds, stage, si, i);
    return tex_handle_cb(ds, stage, tcb, &si->tex[i]);
}
/* a slot whose handle is one word of the texture bank tcb (tex_handle_cb's first word, and no second one) */
static int tex_handle_plain(const ShTex* st, u32 tcb) {
    return !(st->flags & SNX_TEX_MEMORY) && !(st->bindless && st->bl2) && (!st->bindless || (st->bl_cbuf & 31) == tcb);
}
#define TEX_HANDLE_SPAN 256 /* words of the texture bank read at once */
/* the handles of si's first n texture slots, as tex_handle gives them: those in the texture bank in guest memory read
   in one read of the words they span. A read of a word there took the general path of guest memory (its pending
   results' lock, the queued releases, the owners) once for each texture of each draw: in one game half of the
   fragment textures' keying, about 0.1 us a draw */
static void tex_handles(const DsState* ds, int stage, u32 tcb, const ShaderInfo* si, int n, u32* out) {
    u32 lo = ~0u, hi = 0, words[TEX_HANDLE_SPAN];
    for (int i = 0; i < n; i++)
        if (tex_handle_plain(&si->tex[i], tcb)) {
            if (si->tex[i].cbuf_off < lo) lo = si->tex[i].cbuf_off;
            if (si->tex[i].cbuf_off > hi) hi = si->tex[i].cbuf_off;
        }
    u64 a = tcb < DS_CBUFS ? ds->bind.cb_addr[stage][tcb] : 0;
    int span = lo <= hi && hi - lo < TEX_HANDLE_SPAN && a && !ds_cb_inline(ds, stage, tcb).bytes;
    if (span) gmem_read(a + (u64)lo * 4, words, ((u64)hi - lo + 1) * 4);
    for (int i = 0; i < n; i++)
        out[i] = span && tex_handle_plain(&si->tex[i], tcb) ? snx_ld_le32((const u8*)&words[si->tex[i].cbuf_off - lo])
                                                             : tex_handle(ds, stage, tcb, si, i);
}

/* a texture binding of a draw: the headers its handle names, and the texture. memo: the way of g_tex_memo the headers
   were kept in, plus one (0 for none), which keeps the slot's view and sampler (tex_bind_view) */
typedef struct {
    u32 handle, have;
    u32 tic[8], tsc[8];
    Surf* t;
    u32 memo;
} TexRes;
/* A capture slice shares the original shader state but writes host-only scratch attachments. */
static struct {
    VkCapture* images;
    Surf* snapshot;
    u32 primitive;
    VkDescriptorBufferInfo shadow;
} g_capture_draw;
static Surf* capture_sample(Surf* texture) {
    if (!g_capture_draw.images || texture != g_capture_draw.images->target) return texture;
    if (!g_capture_draw.snapshot) g_capture_draw.snapshot = vk_rt_snapshot(texture, 1);
    return g_capture_draw.snapshot;
}
/* A feedback draw drawn two times (predraw_run). probe: the draw is a candidate, and stage_set_ records each
   fragment texture it binds in place (in tex, n of them, with the textures in place in in_place). phase 1: the save
   draw, whose fragment program fetches texture k of tex. phase 2: the guest's draw, whose textures in place read
   save[k] in place of target[k] */
static struct {
    int probe, phase;
    u32 n, in_place;
    TexRes tex[DS_TARGETS_MAX];
    Surf* target[DS_TARGETS_MAX];
    /* the save attachments, sets of n, in color slots the guest binds no target to (slot): two sets where there is
       room, used in turn by the feedback draws (parity), so the save draw of one draw writes the set that the draw
       before it didn't read. wmask: the slots of the set that the save draw writes */
    Surf* save[2][DS_TARGETS_MAX];
    u8 slot[2][DS_TARGETS_MAX];
    u32 sets, parity, wmask;
    VkSh* fs; /* the save draw's fragment program */
} g_predraw;
u64 g_vk_predraws, g_vk_predraw_refused;
/* why feedback draws stayed in place, counted by reason (each a literal) for vk_predraw_report, and the save draws
   and guest draws whose target or save attachment was not sampled in place (read from a copy, and not ordered) */
static struct {
    const char* why[12];
    u64 count[12];
    u64 target_copied, save_copied;
} g_predraw_stats;
static void predraw_refuse(const char* why) {
    g_vk_predraw_refused++;
    u32 i = 0;
    while (i < 12 && g_predraw_stats.why[i] && g_predraw_stats.why[i] != why) i++;
    if (i == 12) return;
    g_predraw_stats.why[i] = why;
    if (!g_predraw_stats.count[i]++)
        LOG("vulkan: a draw that samples a target it writes at its own pixel stays in place: it %s\n", why);
}
void vk_predraw_report(void) {
    for (u32 i = 0; i < 12 && g_predraw_stats.why[i]; i++)
        LOG("vulkan: %llu feedback draws stayed in place, each because it %s\n", (unsigned long long)g_predraw_stats.count[i],
            g_predraw_stats.why[i]);
    if (g_predraw_stats.target_copied || g_predraw_stats.save_copied)
        LOG("vulkan: %llu save draws read their target from a copy, %llu guest draws read their save attachment from a "
            "copy (not in place)\n",
            (unsigned long long)g_predraw_stats.target_copied, (unsigned long long)g_predraw_stats.save_copied);
}
static void predraw_probe(u32 i, const TexRes* x, Surf* t) {
    if (!g_predraw.probe || i >= 64) return;
    g_predraw.in_place |= i < 32 ? 1u << i : 0;
    if (i >= 32) g_predraw.probe = 0; /* the in-place mask takes 32 slots */
    for (u32 k = 0; k < g_predraw.n; k++)
        if (g_predraw.target[k] == t) return;
    if (g_predraw.n == DS_TARGETS_MAX) {
        g_predraw.probe = 0;
        return;
    }
    g_predraw.tex[g_predraw.n] = *x;
    g_predraw.target[g_predraw.n++] = t;
}
static Surf* predraw_sample(int i, Surf* t) {
    if (g_predraw.phase != 2 || i >= 32 || !(g_predraw.in_place >> i & 1)) return t;
    for (u32 k = 0; k < g_predraw.n; k++)
        if (g_predraw.target[k] == t) return g_predraw.save[g_predraw.parity][k];
    return t;
}

/* The counts of what the draws' lookups found (VkDrawCounts), and the counts of the renderer at the last report
   (vk_render_stats_log), which tell how much of a draw's setup a later draw can keep: the kept draw states, the
   constant buffer and texture lookups, the descriptor sets, the submissions and the write tracking epochs */
static struct {
    VkDrawCounts n, was;
    u32 epoch; /* the last draw's write tracking epoch */
    u64 present, drawn, kept, dset_new, dset_reused, targets_kept, submits[VK_SUB_N];
    u32 ww_epoch;
    u64 heap_buffer_calls, heap_buffer_hits, heap_image_calls, heap_image_hits, heap_cb_addresses;
} g_stats;
/* the bytes of the constant buffers that cb_get copied from the CPU (VK_CB_COPIED) since the last statistics line */
static u64 g_cb_copied_bytes;
static u64 g_cb_padded; /* constant buffers by address padded with zeros past a short binding (cb_pad) */
void vk_draw_counts(VkDrawCounts* c) {
    *c = g_stats.n;
    VkDescriptorHeapStats heaps; /* the kept descriptors of the heaps' lasting part that draw_state_check=1 found wrong */
    vk_descriptor_heaps_get_stats(&heaps);
    c->differs += heaps.lasting_differs;
}
/* Why tex_resolve resolved a handle again (the memo missed), for vk_render_stats_log: no way holds the handle, a way
   holds it for another draw state or other pools, the surface table moved at the texture's address, its headers were
   written, a storage image is pending over it, a result is pending over what it read, a GPU-side write on the
   headers' pages, or the target it kept is undefined. MEMO_UNKEPT counts the textures resolved that are not kept: over
   a render target that is not the texture itself, their next draw misses again */
enum {
    MEMO_WAY,
    MEMO_STATE,
    MEMO_SURFACE,
    MEMO_HEADERS,
    MEMO_IMAGE,
    MEMO_PENDING,
    MEMO_GPU,
    MEMO_UNDEFINED,
    MEMO_UNKEPT,
    MEMO_DRAWN,
    MEMO_OVER_DIFFERS,
    MEMO_CAUSES
};
static u64 g_memo_miss[MEMO_CAUSES];
static void memo_miss_log(u64 presents) {
    static u64 was[MEMO_CAUSES];
    u64 d[MEMO_CAUSES];
    for (int i = 0; i < MEMO_CAUSES; i++) d[i] = g_memo_miss[i] - was[i], was[i] = g_memo_miss[i];
    const double p = (double)presents;
    LOG("vulkan: texture memo misses a present: %.1f no way, %.1f other state, %.1f surface epoch, %.1f headers, %.1f "
        "image, %.1f pending, %.1f GPU write, %.1f undefined target, %.1f target drawn or memory written under a "
        "texture over it; %.1f not kept (over a target)\n",
        (double)d[MEMO_WAY] / p, (double)d[MEMO_STATE] / p, (double)d[MEMO_SURFACE] / p, (double)d[MEMO_HEADERS] / p,
        (double)d[MEMO_IMAGE] / p, (double)d[MEMO_PENDING] / p, (double)d[MEMO_GPU] / p, (double)d[MEMO_UNDEFINED] / p,
        (double)d[MEMO_DRAWN] / p, (double)d[MEMO_UNKEPT] / p);
    if (d[MEMO_OVER_DIFFERS])
        LOG("vulkan: draw_state_check=1: %llu textures kept over a target differed from those resolved again\n",
            (unsigned long long)d[MEMO_OVER_DIFFERS]);
}
/* fs_key's answers a present (VkDrawCounts.fs) */
static void fs_count_log(const VkDrawCounts* n, const VkDrawCounts* w, u64 presents) {
    double d[VK_FS_N];
    for (int i = 0; i < VK_FS_N; i++) d[i] = (double)(n->fs[i] - w->fs[i]) / (double)presents;
    LOG("vulkan: fragment keys a present: %.1f the draw before's, made again for %.1f none kept, %.1f another program "
        "or state, %.1f other pools, %.1f a GPU write, %.1f a "
        "result added, %.1f a storage image added, %.1f a target taken, %.1f the surface table, %.1f an epoch, %.1f a "
        "present, %.1f other handles, %.1f a texture not quiet; not kept for %.1f programs with images and %.1f "
        "textures the memo doesn't keep\n",
        d[VK_FS_KEPT], d[VK_FS_NONE], d[VK_FS_PROGRAM], d[VK_FS_POOLS], d[VK_FS_GPU], d[VK_FS_RESULTS], d[VK_FS_IMAGES],
        d[VK_FS_TAKE], d[VK_FS_SURFACES], d[VK_FS_EPOCH], d[VK_FS_PRESENT], d[VK_FS_HANDLES], d[VK_FS_QUIET],
        d[VK_FS_UNKEPT_IMAGES], d[VK_FS_UNKEPT_TEXTURES]);
}
void vk_render_stats_log(void) {
    const VkDrawCounts* n = &g_stats.n;
    const VkDrawCounts* w = &g_stats.was;
    u64 presents = g_vk_present - g_stats.present, submits = 0, periodic = vk_submits(VK_SUB_PERIODIC);
    u64 was_submits = 0;
    for (int i = 0; i < VK_SUB_N; i++) {
        submits += vk_submits(i);
        was_submits += g_stats.submits[i];
    }
    u32 epoch = vk_ww_epoch();
    if (!presents) presents = 1;
    memo_miss_log(presents);
    fs_count_log(n, w, presents);
    LOG("vulkan: draw states in the last %llu presents: %llu draws, %llu kept an earlier draw's (%llu the last "
        "draw's, %llu one from the table), %llu found one that a later check made again; %llu kept in the table "
        "(%llu putting another out), %llu groups hashed. A present: %.1f submissions (%.1f periodic), %.1f write "
        "tracking epochs (%.1f with draws)\n",
        (unsigned long long)presents, (unsigned long long)(g_vk_drawn - g_stats.drawn),
        (unsigned long long)(g_vk_draw_kept - g_stats.kept), (unsigned long long)(n->last - w->last),
        (unsigned long long)(n->table - w->table), (unsigned long long)(n->late - w->late),
        (unsigned long long)(n->made - w->made), (unsigned long long)(n->evicted - w->evicted),
        (unsigned long long)(n->hashed - w->hashed), (double)(submits - was_submits) / (double)presents,
        (double)(periodic - g_stats.submits[VK_SUB_PERIODIC]) / (double)presents,
        (double)(u32)(epoch - g_stats.ww_epoch) / (double)presents, (double)(n->epochs - w->epochs) / (double)presents);
    LOG("vulkan: lookups in the last %llu presents: constant buffers %llu inline kept, %llu inline staged, %llu kept, "
        "%llu kept after the pending check, %llu copied from the CPU, %llu from the buffer cache, %llu pending, %llu of "
        "the kept ones past their submission; textures %llu kept, %llu resolved again, %llu past the pools, %llu views "
        "and %llu samplers kept, %llu lookups through the memo; descriptor sets %llu written, %llu reused, %llu of "
        "those a stage's last; %llu draws kept the last one's targets\n",
        (unsigned long long)presents, (unsigned long long)(n->cb[VK_CB_INLINE_KEPT] - w->cb[VK_CB_INLINE_KEPT]),
        (unsigned long long)(n->cb[VK_CB_INLINE_STAGED] - w->cb[VK_CB_INLINE_STAGED]),
        (unsigned long long)(n->cb[VK_CB_KEPT] - w->cb[VK_CB_KEPT]),
        (unsigned long long)(n->cb[VK_CB_KEPT_CHECKED] - w->cb[VK_CB_KEPT_CHECKED]),
        (unsigned long long)(n->cb[VK_CB_COPIED] - w->cb[VK_CB_COPIED]),
        (unsigned long long)(n->cb[VK_CB_CACHE] - w->cb[VK_CB_CACHE]),
        (unsigned long long)(n->cb[VK_CB_PENDING] - w->cb[VK_CB_PENDING]),
        (unsigned long long)(n->cb[VK_CB_PAST_SUBMIT] - w->cb[VK_CB_PAST_SUBMIT]),
        (unsigned long long)(n->tex[VK_TEX_KEPT] - w->tex[VK_TEX_KEPT]),
        (unsigned long long)(n->tex[VK_TEX_RESOLVED] - w->tex[VK_TEX_RESOLVED]),
        (unsigned long long)(n->tex[VK_TEX_PAST] - w->tex[VK_TEX_PAST]),
        (unsigned long long)(n->tex[VK_TEX_VIEW_KEPT] - w->tex[VK_TEX_VIEW_KEPT]),
        (unsigned long long)(n->tex[VK_TEX_SAMPLER_KEPT] - w->tex[VK_TEX_SAMPLER_KEPT]),
        (unsigned long long)(n->tex[VK_TEX_MEMO] - w->tex[VK_TEX_MEMO]),
        (unsigned long long)(g_vk_dset_new - g_stats.dset_new),
        (unsigned long long)(g_vk_dset_reused - g_stats.dset_reused), (unsigned long long)(n->sets_last - w->sets_last),
        (unsigned long long)(g_vk_targets_kept - g_stats.targets_kept));
    if (g_vk.has_descriptor_heaps) {
        VkDescriptorHeapStats h;
        vk_descriptor_heaps_get_stats(&h);
        LOG("vulkan: descriptor heaps in the last %llu presents: %llu buffer descriptors written by the driver, %llu "
            "found; %llu image descriptors written, %llu found; %llu constant buffers by address, %llu of them padded\n",
            (unsigned long long)presents, (unsigned long long)(h.buffer_driver_calls - g_stats.heap_buffer_calls),
            (unsigned long long)(h.buffer_hits - g_stats.heap_buffer_hits),
            (unsigned long long)(h.image_driver_calls - g_stats.heap_image_calls),
            (unsigned long long)(h.image_hits - g_stats.heap_image_hits),
            (unsigned long long)(h.cb_addresses - g_stats.heap_cb_addresses), (unsigned long long)g_cb_padded);
        g_stats.heap_cb_addresses = h.cb_addresses;
        g_cb_padded = 0;
        g_stats.heap_buffer_calls = h.buffer_driver_calls;
        g_stats.heap_buffer_hits = h.buffer_hits;
        g_stats.heap_image_calls = h.image_driver_calls;
        g_stats.heap_image_hits = h.image_hits;
    }
    {
        double k[4][2], d = (double)(g_vk_drawn - g_stats.drawn);
        for (int b = 0; b < 4; b++)
            for (int j = 0; j < 2; j++) k[b][j] = (double)(n->inl[b][j] - w->inl[b][j]) / (double)presents;
        LOG("vulkan: inline blocks a present (%.1f draws), kept and staged again: bank 0 to its base vertex words %.1f "
            "and %.1f, bank 0 before them %.1f and %.1f, bank 2 %.1f and %.1f, the root tables %.1f and %.1f\n",
            d / (double)presents, k[0][0], k[0][1], k[1][0], k[1][1], k[2][0], k[2][1], k[3][0], k[3][1]);
    }
    LOG("vulkan: constant buffers copied from the CPU in the last %llu presents: %.1f KB a present\n",
        (unsigned long long)presents, (double)g_cb_copied_bytes / 1024.0 / (double)presents);
    g_cb_copied_bytes = 0;
    g_stats.was = g_stats.n;
    g_stats.present = g_vk_present;
    g_stats.drawn = g_vk_drawn;
    g_stats.kept = g_vk_draw_kept;
    g_stats.dset_new = g_vk_dset_new;
    g_stats.dset_reused = g_vk_dset_reused;
    g_stats.targets_kept = g_vk_targets_kept;
    for (int i = 0; i < VK_SUB_N; i++) g_stats.submits[i] = vk_submits(i);
    g_stats.ww_epoch = epoch;
}

/* What a texture handle resolved to, kept while nothing it came from can have changed: the pools' addresses, the
   surface table at the texture's address (vk_surf_epoch_at: tex_from_tic looks nowhere else, and a texture's drawn
   targets are taken in by vk_tex_recheck at every hit), the CPU's writes to the two headers (tex_memo_unwritten), the
   results pending over what it read (tex_memo_unpended) and the GPU's writes to the two headers. With
   the whole table's epoch, a texture made anywhere dropped them all: 8% of one game's lookups. A draw resolved each
   of its textures again, reading both headers and finding the texture by them: a tenth of the GPU thread in one
   game, for the same few hundred handles every draw. A kept texture is still checked against its memory
   (vk_tex_recheck). A render target read whole as the texture is kept too (rt): its Surf stays the target while the
   surface table doesn't change, and a hit does what tex_from_tic does for it each draw, the read check
   (vk_rt_note_read) and whether its contents are defined. Those were a twentieth of the GPU thread in one game,
   which samples its targets in most draws. A crop of a target, or a texture over a target's memory read from memory,
   isn't kept (their contents change with drawing) */
typedef struct {
    const DsState* ds;
    u64 tic_base, tsc_base, gw, adds;
    u64 t_gva; /* t's address when kept: its bucket of the surface table tells whether t is still there */
    u32 handle, surf_epoch, ww_epoch;
    u32 wseen; /* vk_ww_now() before the headers were read */
    u32 tic[8], tsc[8];
    Surf* t;
    int rt;   /* t is the render target itself, read whole */
    /* t lies over the render target `over` without being it (a crop of it, or a texture loaded from memory with its
       drawn targets taken in), kept while that target holds the pixels it had (its write generation over_wgen), nothing
       GPU-side wrote t's memory since over_gw and the epoch is the same. copy: t is a copy of the target's pixels
       (rt_crop), returned as it is; else checked again as a kept texture is */
    Surf* over;
    u32 over_wgen, over_epoch;
    u64 over_gw;
    int copy;
    u32 used; /* g_tex_memo.use when last found or kept: the way used longest ago makes room */
    /* vk_gfx_image_adds() + 1 when no storage image but its texture's own was pending over its texture: 0 unknown */
    u64 img_adds;
    /* The view and the sampler a slot made of the two headers (tex_bind_view, tex_bind_sampler): what vk_tex_bind_view
       and vk_tex_bind_sampler make depends on nothing else but the surface, how the program reads the slot (how) and
       settings read once at start. Kept in the frame they were made in (frame, vk_frame_id() + 1, 0 for none): nothing
       destroys a view or an image before the work of its frame is done (vk_defer_*), so another surface made since at
       the surface's address has another image (img). A view is also made from the image's finest level with memory
       (lod_floor). Emptied when the headers are kept again */
    struct {
        u64 frame;
        const Surf* t;
        VkImage img;
        u32 floor, how;
        VkImageView view;
    } view;
    struct {
        u64 frame;
        const Surf* t; /* the texture whose view is bound, or NULL */
        VkImage img;
        u32 how;
        VkSampler smp;
    } smp;
} TexMemo;
/* by the handle, in sets of TEX_MEMO_WAYS ways. With one way in 4096 slots, a handle that shared its slot with another
   one the draws used too put it out each time: 9% of an Ounce game's lookups in a race and 22% of a Switch game's in
   its street resolved a handle again that way. A miss doesn't take a way unless what it found is kept */
#define TEX_MEMO_SET_BITS 11
#define TEX_MEMO_WAYS     4
static struct {
    TexMemo set[1u << TEX_MEMO_SET_BITS][TEX_MEMO_WAYS];
    u32 use; /* counts the lookups that found or kept a way */
} g_tex_memo;
/* The CPU's writes since m was kept, when the stream's epoch moved: an epoch moves at each submission and each wait
   that ends, and the CPU writes somewhere in nearly every one. What a handle resolves to comes from its two headers
   alone, with the rest checked at each hit as tex_from_tic checks it (vk_tex_recheck for a texture's texels, the read
   check and the defined contents for a render target), so a memo whose headers weren't written stays (1), with the
   epoch and the baseline brought up to now. In one game's street the epoch dropped 39% of the lookups */
static int tex_memo_unwritten(TexMemo* m, u64 tic_at, u64 tsc_at, u32 epoch, u32 now) {
    if (m->ww_epoch == epoch) return 1;
    /* A written page holds 128 headers: the game writes a few of its pool's every frame, and in one battle that dropped
       about 340 lookups a frame whose own headers were as kept. Where the tracking saw a write, the headers' 64 bytes
       are compared with m's */
    if (vk_written_since(gmem_host(tic_at, 32), 32, m->wseen) != 0 ||
        vk_written_since(gmem_host(tsc_at, 32), 32, m->wseen) != 0) {
        u32 tic[8], tsc[8];
        vk_gmem_words(tic_at, tic, 8);
        vk_gmem_words(tsc_at, tsc, 8);
        if (memcmp(tic, m->tic, sizeof tic) || memcmp(tsc, m->tsc, sizeof tsc)) return 0;
    }
    m->ww_epoch = epoch;
    m->wseen = now;
    return 1;
}
/* The results added since m was kept, when any was added anywhere: what m read is its two headers and its texture's
   memory, and while no pending result covers those, it reads them the same (1), with the count brought up to now. The
   pages pending results cover are counted by GPU page (pend_hit), so this is a few lookups. Dispatches add results all
   through a frame, and the count dropped 37% of one game's lookups once the epoch no longer did */
static int tex_memo_unpended(TexMemo* m, u64 tic_at, u64 tsc_at, u64 adds) {
    if (m->adds == adds) return 1;
    const u64 size = vk_surf_guest_size(m->t);
    if (pend_hit(&g_pend_any, tic_at, 32) || pend_hit(&g_pend_any, tsc_at, 32) ||
        pend_hit(&g_pend_any, m->t->gva, size))
        return 0;
    m->adds = adds;
    return 1;
}
/* a storage image other than m's texture itself pending over its memory. Asked of each texture of each draw while one
   is pending anywhere: a released one is pending across the frame (vk_gfx_image_put_aside), and the lookups of every
   way cost one game 0.45 ms a frame. Unchanged entries keep the answer, as the pending results do (tex_memo_unpended) */
static int tex_memo_image_pending(TexMemo* m) {
    u64 adds = vk_gfx_image_adds() + 1;
    if (m->img_adds == adds) return 0;
    if (vk_gfx_image_pending_besides(m->t->gva, vk_surf_guest_size(m->t), m->t)) return 1;
    m->img_adds = adds;
    return 0;
}
/* tex_resolve through the memo: what x is of it, 2 a way found, 1 a way kept for the next lookup, 4 and 3 the same of
   a texture over a target (TexMemo.over), -1 none but its headers as memory has them, 0 none */
static int tex_resolve_memo(const DsState* ds, u32 handle, TexRes* x) {
    const DsBindings* b = &ds->bind;
    if (b->smp_mode) {
        /* samplers are taken by the handle's sampler index. Ryujinx reads this register as the sampler index mode,
           where one mode takes the texture's index instead: whether a game uses it isn't known yet */
        LOG_LIMIT(1, "vulkan: sampler index mode %u\n", b->smp_mode);
    }
    u64 tic_base = b->tex_pool, tsc_base = b->smp_pool;
    u64 tic_at = tic_base + (handle & 0xFFFFF) * 32ull, tsc_at = tsc_base + ((handle >> 20) & 0xFFF) * 32ull;
    u32 set_at = (u32)(((u64)handle * VK_FIB_MULT) >> (64 - TEX_MEMO_SET_BITS));
    TexMemo* set = g_tex_memo.set[set_at];
    TexMemo* m = NULL;
    int same_handle = 0; /* a way holds the handle for another draw state or other pools */
    for (u32 w = 0; w < TEX_MEMO_WAYS && !m; w++) {
        if (set[w].t && set[w].handle == handle && set[w].ds == ds && set[w].tic_base == tic_base &&
            set[w].tsc_base == tsc_base)
            m = &set[w];
        same_handle |= set[w].t && set[w].handle == handle;
    }
    u64 gw = gpu_mem_generation();
    u32 epoch = vk_ww_epoch(), now = vk_ww_now(); /* before the headers are read */
    /* a write in place is no result added: its own count moves (vk_uma_writes) */
    u64 adds = atomic_load_explicit(&g_pend_any.adds, memory_order_relaxed) + vk_uma_writes();
    g_stats.n.tex[VK_TEX_MEMO]++;
    x->handle = handle;
    x->have = 1;
    x->memo = 0;
    /* an index past its pool's limit names no header: no texture, or the
       default sampler, as Eden's VisitImageView and the sampler table have it. One game's movie overlay hands
       texture indices 0x2000 past a 2048-entry pool, where shader code lies */
    if ((handle & 0xFFFFF) > b->tex_max || ((handle >> 20) & 0xFFF) > b->smp_max) {
        if ((handle & 0xFFFFF) > b->tex_max) {
            memset(x->tic, 0, sizeof x->tic);
            x->t = NULL;
        } else {
            vk_gmem_words(tic_at, x->tic, 8);
            x->t = vk_tex_from_tic(x->tic);
        }
        if (((handle >> 20) & 0xFFF) > b->smp_max)
            memset(x->tsc, 0, sizeof x->tsc);
        else
            vk_gmem_words(tsc_at, x->tsc, 8);
        LOG_LIMIT(1, "vulkan: a texture handle %08x names headers past the pools' limits (%x, %x): no texture there\n",
                  handle, b->tex_max, b->smp_max);
        g_stats.n.tex[VK_TEX_PAST]++;
        return 0;
    }
    /* a pending storage image over this texture's own memory invalidates it; one elsewhere does not. The global flag
       alone dropped every memo once any fragment program's storage image was pending, so a race that draws its
       targets and writes storage images in the same frame re-resolved every texture of every draw. Nor does the
       texture's own surface written as a storage image: it samples those writes where they are. A released one stays
       pending across the frame (vk_gfx_image_put_aside), and one game's draws resolved it again 57 times a frame */
    /* the first condition that fails, in the order they are asked (the log's counts of misses by cause) */
    int miss = !m                                                   ? (same_handle ? MEMO_STATE : MEMO_WAY)
               : m->surf_epoch != vk_surf_epoch_at(m->t_gva)        ? MEMO_SURFACE
               : !tex_memo_unwritten(m, tic_at, tsc_at, epoch, now) ? MEMO_HEADERS
               : atomic_load_explicit(&g_gfx_img_any, memory_order_relaxed) && tex_memo_image_pending(m) ? MEMO_IMAGE
               : !tex_memo_unpended(m, tic_at, tsc_at, adds)                                             ? MEMO_PENDING
               : m->gw != gw && (gpu_mem_dirty_since(m->gw, tic_at, 32) || gpu_mem_dirty_since(m->gw, tsc_at, 32))
                   ? MEMO_GPU
                   : -1;
    /* a texture kept over a target: the target holds the same pixels, at the same address, and nothing wrote the
       texture's memory since (the choice tex_from_tic made, a copy of the target or its memory, stands) */
    if (miss < 0 && m->over &&
        (vk_rt_find(m->t_gva) != m->over || m->over->wgen != m->over_wgen ||
         m->over->layout == VK_IMAGE_LAYOUT_UNDEFINED || m->over_epoch != epoch ||
         gpu_mem_dirty_since(m->over_gw, m->t_gva, vk_surf_guest_size(m->t))))
        miss = MEMO_DRAWN;
    if (miss < 0 && m->over) {
        m->used = ++g_tex_memo.use;
        memcpy(x->tic, m->tic, sizeof x->tic);
        memcpy(x->tsc, m->tsc, sizeof x->tsc);
        x->memo = set_at * TEX_MEMO_WAYS + (u32)(m - set) + 1;
        if (m->copy) vk_rt_note_read(m->over); /* as tex_from_tic notes a read of the target */
        x->t = m->copy ? m->t : vk_tex_recheck(m->t);
        if (g_settings.draw_state_check) { /* resolved again in full, and that one taken */
            u32 tic[8];
            const u32 crop_gen = m->copy ? m->t->crop_gen : 0;
            vk_gmem_words(tic_at, tic, 8);
            Surf* full = vk_tex_from_tic(tic);
            /* a kept copy the full lookup copied again (rt_copy: the target's pixels changed) was stale */
            if (full != x->t || memcmp(tic, m->tic, sizeof tic) || (m->copy && m->t->crop_gen != crop_gen)) {
                g_memo_miss[MEMO_OVER_DIFFERS]++;
                WARN_LIMIT(16,
                           "vulkan: draw_state_check=1: a texture kept over the target at %llx is not the one "
                           "resolved again\n",
                           (unsigned long long)m->t_gva);
            }
            x->t = full;
        }
        if (!x->t) {
            m->t = NULL;
            x->memo = 0;
        }
        g_stats.n.tex[VK_TEX_KEPT]++;
        return x->t ? 4 : 0;
    }
    if (miss < 0) {
        m->gw = gw;
        m->used = ++g_tex_memo.use;
        memcpy(x->tic, m->tic, sizeof x->tic);
        memcpy(x->tsc, m->tsc, sizeof x->tsc);
        x->memo = set_at * TEX_MEMO_WAYS + (u32)(m - set) + 1;
        if (m->rt) {
            if (m->t->layout != VK_IMAGE_LAYOUT_UNDEFINED) vk_rt_note_read(m->t);
            if (m->t->layout != VK_IMAGE_LAYOUT_UNDEFINED) {
                x->t = m->t;
                g_stats.n.tex[VK_TEX_KEPT]++;
                return 2;
            }
            m->t = NULL; /* undefined: memory has its texels, resolved below */
            x->memo = 0;
            miss = MEMO_UNDEFINED;
        } else {
            x->t = vk_tex_recheck(m->t);
            if (!x->t) {
                m->t = NULL;
                x->memo = 0;
            }
            g_stats.n.tex[VK_TEX_KEPT]++;
            return x->t ? 2 : 0;
        }
    }
    g_stats.n.tex[VK_TEX_RESOLVED]++;
    g_memo_miss[miss >= 0 ? miss : MEMO_WAY]++;
    vk_gmem_words(tic_at, x->tic, 8);
    vk_gmem_words(tsc_at, x->tsc, 8);
    x->t = vk_tex_from_tic(x->tic);
    if (m) m->t = NULL; /* what it held is gone: kept again below if it can be */
    Surf* at = x->t ? vk_rt_find(x->t->gva) : NULL;
    /* one over a defined target, not the target itself (a copy of it is a target too), kept too (TexMemo.over) */
    Surf* over = x->t && at && x->t != at && at->layout != VK_IMAGE_LAYOUT_UNDEFINED ? at : NULL;
    if (x->t && !(x->t->is_rt ? x->t == at : !at) && !over) g_memo_miss[MEMO_UNKEPT]++; /* over a target, not it */
    if (x->t && ((x->t->is_rt ? x->t == at : !at) || over)) {
        if (!m) { /* an empty way, else the one used longest ago */
            m = &set[0];
            for (u32 w = 1; w < TEX_MEMO_WAYS && m->t; w++)
                if (!set[w].t || (i32)(set[w].used - m->used) < 0) m = &set[w];
        }
        m->used = ++g_tex_memo.use;
        m->rt = x->t->is_rt && !over; /* a copy of a target is kept as one over it */
        m->ds = ds;
        m->handle = handle;
        m->tic_base = tic_base;
        m->tsc_base = tsc_base;
        m->gw = gpu_mem_generation();
        m->adds = adds;
        m->t_gva = x->t->gva;
        m->surf_epoch = vk_surf_epoch_at(x->t->gva);
        m->ww_epoch = epoch;
        m->wseen = now;
        memcpy(m->tic, x->tic, sizeof m->tic);
        memcpy(m->tsc, x->tsc, sizeof m->tsc);
        m->t = x->t;
        m->img_adds = 0;
        m->view.frame = m->smp.frame = 0;
        m->over = over;
        m->over_wgen = over ? over->wgen : 0;
        m->over_epoch = epoch;
        m->over_gw = gw; /* read before the headers and the texture */
        m->copy = over && vk_tex_is_copy_of(x->t, over);
        x->memo = set_at * TEX_MEMO_WAYS + (u32)(m - set) + 1;
        return over ? 3 : 1;
    }
    return -1;
}
/* A texture the memo keeps over a target (TexMemo.over), for the tables in front of the memo: the target and its
   write generation when the memo's checks last passed, and whether the texture is a copy of it (TexMemo.copy). The
   target's draws move its write generation and none of those tables' stamps, so each asks tex_over_holds; the memo's
   other checks of such a way (the target still at the address, the epoch, the GPU's writes under the texture) read
   stamps the tables hold */
typedef struct {
    Surf* rt;
    u32 wgen;
    int copy;
} TexOver;
/* x's, rt NULL for a texture that is over no target or that the memo doesn't keep */
static TexOver tex_over_of(const TexRes* x) {
    if (!x->t || !x->memo) return (TexOver){0};
    const TexMemo* m = &g_tex_memo.set[(x->memo - 1) / TEX_MEMO_WAYS][(x->memo - 1) % TEX_MEMO_WAYS];
    if (m->t != x->t || !m->over || m->over->wgen != m->over_wgen) return (TexOver){0};
    return (TexOver){.rt = m->over, .wgen = m->over_wgen, .copy = m->copy};
}
/* the target still holds the pixels the texture was kept over */
static int tex_over_holds(const TexOver* o) {
    return o->rt->wgen == o->wgen && o->rt->layout != VK_IMAGE_LAYOUT_UNDEFINED;
}
/* The memo's last answers by handle, with what every check of a memo hit reads, taken before the lookup: while none
   of those moved, the same way is found again and each of its checks says the same (the surface table's epoch moves
   with any bucket's, the stream's with any write epoch, and the counts of pending results, storage images and GPU
   writes with any of theirs), so the lookup comes to what a hit does last. Most draws move none of them since the one
   before (the GPU front end writes no memory between them), and resolve the same handles again: one game's draws
   resolved about 7.6 textures each, 6.1 of them memo hits of about 65 ns, much of it spent in the ways of a memo too
   big for the cache. A draw-level form of the fast draws of Cemu's command processor, which skip the checks of each
   draw while only uniforms and vertex buffers change (cemu-project/Cemu #706, the idea only). A resolve the memo doesn't
   keep keeps its headers here, which the same checks prove unwritten: only the texture is found again from them, as
   the memo's lookup would. A texture the memo keeps over a target (TexMemo.over) is kept with the target's write
   generation (TexOver): one racing game's draws sample such a texture in half of its draws, each a lookup through the
   memo. With draw_state_check=1 each lookup goes to the memo. A frame's draws use some hundreds of handles: 1024 entries of about 150 bytes, which the cache holds */
#define TEX_RECENT_BITS 10
enum { TEX_RECENT_NONE, TEX_RECENT_TARGET, TEX_RECENT_TEXTURE, TEX_RECENT_HEADERS, TEX_RECENT_OVER };
typedef struct {
    TexRes x;
    const DsState* ds;
    u64 tic_base, tsc_base, gw, adds, img_adds;
    u32 surf_epoch, ww_epoch;
    u32 tex_max, smp_max;
    /* TEX_RECENT_*: x.t the render target itself read whole, a texture the memo keeps, headers only, or a texture the
       memo keeps over the target `over` */
    int kind;
    TexOver over;
    /* a texture's: the present and the targets' take count of its last check (vk_tex_recheck), which gives the same
       answer while they and the stamps above are the same */
    u32 present;
    u64 take;
} TexRecent;
static TexRecent g_tex_recent[1u << TEX_RECENT_BITS];
/* whether t's last check left it quiet in this present and write tracking epoch (texture.c's tex_check, Surf.quiet):
   its next check is one compare and gives t again. A texture that isn't (memory no granule watch covers, a failed
   upload, Auto's turn to BC7 pending) is checked in full at each lookup, whose answer a cache can't keep */
static int tex_quiet_now(const Surf* t, u32 ww_epoch) {
    return t->quiet && t->upload_valid && !t->bc7_reread && t->q_present == g_vk_present && t->q_epoch == ww_epoch;
}
static void tex_resolve(const DsState* ds, u32 handle, TexRes* x) {
    const DsBindings* b = &ds->bind;
    TexRecent* r = &g_tex_recent[((u64)handle * VK_FIB_MULT) >> (64 - TEX_RECENT_BITS)];
    /* the results added count the writes in place too, as the memo's do */
    const u64 gw = gpu_mem_generation(),
              adds = atomic_load_explicit(&g_pend_any.adds, memory_order_relaxed) + vk_uma_writes(),
              img_adds = vk_gfx_image_adds();
    const u32 surf_epoch = atomic_load_explicit(&g_vk_surf_epoch, memory_order_relaxed), ww_epoch = vk_ww_epoch();
    const u64 take = vk_rt_take_gen();
    if (r->kind && r->x.handle == handle && r->ds == ds && r->tic_base == b->tex_pool && r->tsc_base == b->smp_pool &&
        r->tex_max == b->tex_max && r->smp_max == b->smp_max && r->gw == gw && r->adds == adds &&
        r->img_adds == img_adds && r->surf_epoch == surf_epoch && r->ww_epoch == ww_epoch && !b->smp_mode &&
        !g_settings.draw_state_check) {
        Surf* t = r->x.t;
        if (r->kind == TEX_RECENT_HEADERS) {
            *x = r->x;
            x->t = vk_tex_from_tic(x->tic);
            g_stats.n.tex[VK_TEX_RESOLVED]++;
            return;
        }
        if (r->kind == TEX_RECENT_TARGET) {
            if (t->layout != VK_IMAGE_LAYOUT_UNDEFINED) vk_rt_note_read(t);
            if (t->layout != VK_IMAGE_LAYOUT_UNDEFINED) {
                *x = r->x;
                g_stats.n.tex[VK_TEX_KEPT]++;
                return;
            }
        } else if (r->kind == TEX_RECENT_OVER && !tex_over_holds(&r->over)) {
            /* the target was drawn to: the memo's lookup, which takes the texture from it again */
        } else if (r->kind == TEX_RECENT_OVER && r->over.copy) {
            vk_rt_note_read(r->over.rt); /* as the memo's hit notes a read of the target */
            *x = r->x;
            g_stats.n.tex[VK_TEX_KEPT]++;
            return;
        } else if (r->present == g_vk_present && r->take == take && tex_quiet_now(t, ww_epoch)) {
            /* checked in this present and left quiet, and since then no GPU-side write (gw), no CPU write epoch, no
               surface, result or target taken: the check (tex_check) would read the texture's own state to give the
               same answer.
               About six of a draw's textures, at about 200 ns each, spent mostly in cache misses on the Surf */
            *x = r->x;
            g_stats.n.tex[VK_TEX_KEPT]++;
            return;
        } else if ((t = vk_tex_recheck(t))) {
            *x = r->x;
            x->t = t;
            r->x.t = t;
            r->present = g_vk_present;
            r->take = take;
            g_stats.n.tex[VK_TEX_KEPT]++;
            return;
        }
        /* undefined now, or its check failed: the memo's lookup, as before */
    }
    r->kind = TEX_RECENT_NONE;
    /* a way just kept is asked about the storage images pending over its texture at its next lookup only */
    const int memo = tex_resolve_memo(ds, handle, x);
    if (!memo || ((memo == 1 || memo == 3) && atomic_load_explicit(&g_gfx_img_any, memory_order_relaxed))) return;
    const TexOver over = memo >= 3 ? tex_over_of(x) : (TexOver){0};
    if (memo >= 3 && !over.rt) return;
    *r = (TexRecent){.x = *x,
                     .over = over,
                     .ds = ds,
                     .tic_base = b->tex_pool,
                     .tsc_base = b->smp_pool,
                     .gw = gw,
                     .adds = adds,
                     .img_adds = img_adds,
                     .surf_epoch = surf_epoch,
                     .ww_epoch = ww_epoch,
                     .tex_max = b->tex_max,
                     .smp_max = b->smp_max,
                     .present = g_vk_present,
                     .take = take,
                     .kind = memo < 0      ? TEX_RECENT_HEADERS
                             : memo >= 3   ? TEX_RECENT_OVER
                             : x->t->is_rt ? TEX_RECENT_TARGET
                                           : TEX_RECENT_TEXTURE};
}

int vk_tic_unnormalized(const u32* tic) {
    u32 type = (tic[4] >> 23) & 0xF;
    if (!tic[0] || (tic[5] >> 31) || type == 6) return 0;
    return type == 1 || type == 7 ? 1 : 2;
}

int vk_tex_num(const u32* tic, const Surf* t) {
    VkFmt f;
    if (vk_tic_reads_stencil(tic)) return 1; /* a depth target's stencil is unsigned, whatever the surface holds */
    f = t ? t->f : vk_tic_format(tic[0], 0);
    return f.is_int ? (f.sint ? 2 : 1) : 0;
}

int vk_image_keyable(const ShaderInfo* si, int i) {
    return (si->img[i].type == 1 || si->img[i].type == 3 || si->img[i].type == 4 || si->img[i].type == 5) &&
           si->img[i].kind != 1;
}

int vk_image_key(const ShaderInfo* si, int i, const u32* tic) {
    return !!vk_tic_format(tic[0], 0).is_int != !!si->img[i].bindless;
}

VkImageView vk_storage_image_view(Surf* t, const ShaderInfo* si, int i, u64* bytes) {
    int volume = si->img[i].type == 5;
    int kind = si->img[i].kind;
    *bytes = vk_surf_guest_size(t);
    /* a 3D surface is a block linear 3D texture at its own size, written back as one volume (vk_surf_flush) */
    if (!t || !t->storage_capable || !vk_surf_can_writeback(t) || !t->levels ||
        (volume ? t->type != VK_IMAGE_VIEW_TYPE_3D || !t->bl || t->sw != t->w || t->sh != t->h
                : t->depth3d != 1 || (si->img[i].type != 3 && si->img[i].type != 4)) ||
        !*bytes || *bytes > UINT32_MAX)
        return VK_NULL_HANDLE;
    /* an sRGB surface is written through its linear twin (vk_tex_storage_view's view, surf_create_'s EXTENDED_USAGE):
       NVN's image stores convert no sRGB, so the program's bytes land as it wrote them. One game's upscaler stores
       its already encoded frame into an RGBA8 sRGB image, and refused, its whole frame stayed black */
    if (kind == 1 ? t->f.vk != VK_FORMAT_R32_UINT
                  : !vk_format_storage_write(vk_srgb_as(t->f.vk, 0)) || kind != (t->f.is_int ? t->f.sint ? 3 : 2 : 0))
        return VK_NULL_HANDLE;
    /* a 2D array surface sees every layer of the image, a single layer one too */
    return vk_tex_storage_view(t, vk_storage_image_type(si, i));
}
VkImageViewType vk_storage_image_type(const ShaderInfo* si, int i) {
    /* the program's dimension (SUST's: 0 1D, 2 1D array, 3 2D, 4 2D array, 5 3D), a 1D one declared as 2D */
    u32 type = si->img[i].type;
    return type == 5                ? VK_IMAGE_VIEW_TYPE_3D
           : type == 2 || type == 4 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                    : VK_IMAGE_VIEW_TYPE_2D;
}

/* Whether a is an address of the GPU's address space. gmmu_run looks an address up by its low
   GMMU_ADDRESS_SPACE_BITS, so a value above them aliases onto whatever is mapped at those bits */
static int gbuf_address(u64 a) { return a && !(a >> GMMU_ADDRESS_SPACE_BITS); }
/* An indirect buffer's address (SH_GBUF_INDIRECT): the 64-bit pointer at its cell, read from guest memory as the
   program loads it, a pending write there landed first. The cell is at the address its parent's constant buffer word
   holds (v). A cell or a pointer with no memory mapped binds nothing (0), and so does a cell whose 8 bytes are no
   address at all. One game's plant culling programs reach a buffer through a pointer at bytes 0x40 to 0x60 of a
   block that, in their dispatches, holds the view's matrices. Two floats there whose bits 32 to 39 fell on mapped
   memory passed for a pointer in a frame now and then: the program got 16 MiB at a nonsense, unaligned address, the
   GPU mirror refused the dispatch for it (compute.c mirror_plan), and the plants' draws did not get that frame's
   lists. Trees and bushes lost their leaves for one frame, a few times a minute */
static u64 gbuf_pointer(u32 off, const u32* v) {
    u64 cell = (((u64)v[1] << 32) | v[0]) + SH_GBUF_CELL(off), cpu = 0;
    u32 w[2];
    if (cell == SH_GBUF_CELL(off) || !gbuf_address(cell) || gmmu_run(cell, 8, &cpu) < 8) return 0;
    vk_buf_mem_need(cell, 8);
    vk_gmem_words(cell, w, 2);
    u64 pointer = ((u64)w[1] << 32) | w[0];
    return gbuf_address(pointer) && gmmu_run(pointer, 4, &cpu) ? pointer : 0;
}
u32 vk_gbuf_desc(int stage, u32 off, const u32* v, u64* addr) {
    u32 size = v[2];
    u64 cpu = 0;
    /* An address with nothing mapped there is no buffer, as address 0 is: it reads zeros and its writes land nowhere,
       as the copies of guest memory gave it before. The program cannot access it, or it would fault on the console.
       An Ounce game's dispatches leave such an address in a slot, and the GPU mirror took no dispatch with one: each
       copied its written buffers on the CPU instead, about 230 MB a second of one race */
    *addr = SH_GBUF_INDIRECT(off) ? gbuf_pointer(off, v) : ((u64)v[1] << 32) | v[0];
    if (!gbuf_address(*addr)) *addr = 0;
    if (!shader_gbuf_sized(stage, off)) {
        /* a bare address in the program's own constant buffer has no size beside it: the memory mapped from it on,
           whole words, to the cap below (Eden binds such a buffer to the end of its mapping, at most 8 MiB) */
        u64 run = *addr ? gmmu_run(*addr, 16u << 20, &cpu) : 0;
        if (!run) *addr = 0;
        size = (u32)run & ~3u;
    } else if (*addr && !gmmu_run(*addr, 1, &cpu))
        *addr = 0;
    if (!size) size = 16;
    if (size > (16u << 20)) size = 16u << 20;
    return (size + 3) & ~3u;
}

/* The images whose bound format changes the program's store type. These bits have their own shader key words. */
static u32 image_variant(const DsState* ds, int stage, const VkSh* s, u32* signed_images) {
    u64 tic_base = ds->bind.tex_pool;
    u32 bits = 0;
    *signed_images = 0;
    for (int i = 0; i < s->si.nimg; i++) {
        if (!vk_image_keyable(&s->si, i)) continue;
        ShTex handle_info = {
            .cbuf_off = s->si.img[i].cbuf_off, .bindless = s->si.img[i].bindless, .bl_cbuf = s->si.img[i].bl_cbuf};
        u32 handle = tex_handle_cb(ds, stage, ds->bind.tex_bank, &handle_info), tic[8];
        vk_gmem_words(tic_base + (handle & 0xFFFFF) * 32ull, tic, 8);
        if (vk_image_key(&s->si, i, tic)) bits |= 1u << i;
        if (vk_tex_num(tic, NULL) == 2) *signed_images |= 1u << i;
    }
    return bits;
}

/* the textures of s, a vertex program, whose headers are integer, by the header alone as image_variant reads its
   images: its variant's unsigned and signed masks, which the key holds for its first 32 textures. Depth compares,
   indexed tables and raw loads take their number type elsewhere. *rect: the 2D textures whose headers take
   unnormalized coordinates */
static u32 vs_tex_variant(const DsState* ds, const VkSh* s, u32* signed_textures, u64* rect, u8* query, int* queried) {
    u64 tic_base = ds->bind.tex_pool;
    int nt = s->si.ntex > SH_MAXTEX ? SH_MAXTEX : s->si.ntex;
    u32 bits = 0;
    *signed_textures = 0;
    *rect = 0;
    *queried = 0;
    memset(query, 0, SH_MAXTEX);
    for (int i = 0; i < nt; i++) {
        const ShTex* st = &s->si.tex[i];
        if (st->flags & (SNX_TEX_INDEXED | 1)) continue;
        if ((st->flags & SNX_TEX_MEMORY) && st->cbuf_off == SH_PROBE_NONE) continue; /* no texture, no header */
        u32 handle = tex_handle(ds, 0, ds->bind.tex_bank, &s->si, i), tic[8];
        vk_gmem_words(tic_base + (handle & 0xFFFFF) * 32ull, tic, 8);
        if (vk_tic_unnormalized(tic) == 1) *rect |= 1ull << i;
        if (s->si.tex_query_mask >> i & 1) { /* only a TXQ names it: the header's dimension (fs_key) */
            query[i] = vk_tic_query_hint(tic);
            *queried |= query[i] != 0;
        }
        if (st->shadow) continue;
        int num = vk_tex_num(tic, NULL);
        if (!num) continue;
        if (i >= 32) {
            LOG_LIMIT(1,
                      "vulkan: a vertex program's texture %d has an integer header (format %02x), past the 32 its key "
                      "holds: it reads zeros\n",
                      i, tic[0] & 0x7F);
            continue;
        }
        bits |= 1u << i;
        if (num == 2) *signed_textures |= 1u << i;
    }
    return bits;
}

/* What picks the fragment shader's variant: which of its textures and outputs are integer (signed), its color textures
   compared as depth and their functions, and its inputs the stage before doesn't write. 0s when the program itself
   is drawn */
typedef struct {
    u64 tm, ts, dm, rect;
    u64 advanced_blend;
    u64 predraw; /* the slots that read their target as it was before the draw (ShaderInfo.predraw_slots) */
    u32 early;   /* vk_shader_get_ex's early */
    u32 advanced_mask, config;
    u32 rm, rs, df, unfed;
    u32 minmax;          /* the textures the program takes a min/max reduction of itself (vk_shader_get_ex's minmax) */
    u32 prim;            /* the generics a mesh program writes per primitive (ShaderInfo.prim_mask), as fs0's key */
    u8 query[SH_MAXTEX]; /* the dimensions of the textures only a TXQ names (ShaderInfo.tex_query_hint) */
    int queried;         /* any of them */
} FsKey;
/* the integer color targets (rm, signed rs) tg names, which key the fragment shader's outputs */
static void rt_int_targets(const DsTargets* tg, u32* rm, u32* rs) {
    *rm = *rs = 0;
    for (u32 i = 0; i < tg->count; i++) {
        if (!tg->color[i].format) continue;
        VkFmt f = vk_rt_format(tg->color[i].format);
        if (f.is_int) {
            *rm |= 1u << i;
            if (f.sint) *rs |= 1u << i;
        }
    }
}
/* the last draw's fs_key: its inputs, and what it resolved and keyed. The same inputs resolve the same textures and
   make the same key. The inputs are the program, the texture bindings (not the generation of all bindings, which
   moves with the constant buffers that most draws bind again), the handle words, the targets' integer outputs, and
   what each slot's resolve checks. Those checks are the stamps of the texture front table (tex_resolve), the
   targets' take generation (tex_take_drawn), and the present (vk_tex_recheck's checks once a frame). About a third
   of one game's draws read other handle words than the draw before, and a sixth run another program. The key
   holds for most of the rest. Only where each slot's texture is one the memo keeps, one over a target with that
   target's write generation (TexOver: the target's draws move none of these stamps), and each texture is quiet in
   this present and epoch, as vk_tex_recheck's quiet check asks (a texture that isn't takes a full check at each
   lookup). A compute result pending over a slot's texture
   or headers is one added since the key was made, which moves the results added (the stamp tex_resolve keeps a
   texture by, pending_pages.c's count_add): those made the key land the results over them first. With
   draw_state_check=1 the key is made again in full and compared (fs_key_check) */
typedef struct {
    const DsState* ds;
    const VkSh* s0;
    u64 tex_pool, smp_pool, gw, adds, img_adds, take;
    u32 tex_max, smp_max, smp_mode, tex_bank, surf_epoch, ww_epoch, present, last_out, rt_int, rt_sint;
    int nt, valid;
    u32 handles[SH_MAXTEX];
    TexRes res[SH_MAXTEX];
    TexOver over[SH_MAXTEX]; /* by slot: the target a slot's texture is kept over, rt NULL for none */
    FsKey fk;
} FsLast;
static FsLast g_fs_last;
/* whether the slots' textures res, which the last draw resolved, hold as they are (FsLast): a target still defined, a
   texture still quiet, one over a target (over, by slot) with that target as it was */
static int fs_last_holds(const TexRes* res, const TexOver* over, int nt, u32 ww_epoch) {
    for (int i = 0; i < nt; i++) {
        const Surf* t = res[i].t;
        if (!t) continue;
        if (t->is_rt ? t->layout == VK_IMAGE_LAYOUT_UNDEFINED : !tex_quiet_now(t, ww_epoch)) return 0;
        if (over[i].rt && !tex_over_holds(&over[i])) return 0;
    }
    return 1;
}
/* whether each slot's texture of res is one the memo keeps, with the target each is kept over into over (by slot) */
static int fs_last_keepable(const TexRes* res, int nt, TexOver* over) {
    for (int i = 0; i < nt; i++) {
        over[i] = (TexOver){0};
        if (!res[i].t) continue;
        if (!res[i].memo) return 0;
        const TexMemo* m = &g_tex_memo.set[(res[i].memo - 1) / TEX_MEMO_WAYS][(res[i].memo - 1) % TEX_MEMO_WAYS];
        if (m->t != res[i].t) return 0;
        if (!m->over) continue;
        over[i] = tex_over_of(&res[i]);
        if (!over[i].rt) return 0;
    }
    return 1;
}
static void draw_differs(const char* what);
/* draw_state_check=1: the last draw's key, which this draw would have kept, against the one made again in full: the
   same variant and, slot by slot, the same headers and texture, or the difference is counted and warned of */
static void fs_key_check(const FsLast* kept, const TexRes* res, int nt, const FsKey* fk) {
    const FsKey* k = &kept->fk;
    int same = k->tm == fk->tm && k->ts == fk->ts && k->dm == fk->dm && k->rect == fk->rect && k->rm == fk->rm &&
               k->rs == fk->rs && k->df == fk->df && k->unfed == fk->unfed && k->minmax == fk->minmax &&
               k->queried == fk->queried && !memcmp(k->query, fk->query, sizeof k->query);
    for (int i = 0; i < nt && same; i++)
        same = kept->res[i].t == res[i].t && !memcmp(kept->res[i].tic, res[i].tic, sizeof res[i].tic) &&
               !memcmp(kept->res[i].tsc, res[i].tsc, sizeof res[i].tsc);
    if (!same) draw_differs("fragment key or texture kept from the draw before");
}
/* resolves the textures of s0, the fragment program, into res (SH_MAXTEX entries, for stage_set), and fills fk with
   the variant they and the targets (rt_int, rt_sint) key. last_out: the generic outputs the stage before rasterization
   writes */
static void fs_key(const DsState* ds, const VkSh* s0, TexRes* res, u32 last_out, u32 rt_int, u32 rt_sint, FsKey* fk) {
    u32 nd = 0;
    *fk = (FsKey){.rm = rt_int, .rs = rt_sint, .unfed = s0->si.in_mask & ~last_out};
    int nt = s0->si.ntex > SH_MAXTEX ? SH_MAXTEX : s0->si.ntex;
    u32 handles[SH_MAXTEX];
    tex_handles(ds, 4, ds->bind.tex_bank, &s0->si, nt, handles);
    const DsBindings* b = &ds->bind;
    /* the results added count the writes in place too, as the memo's do */
    const u64 gw = gpu_mem_generation(),
              adds = atomic_load_explicit(&g_pend_any.adds, memory_order_relaxed) + vk_uma_writes(),
              img_adds = vk_gfx_image_adds(), take = vk_rt_take_gen();
    const u32 surf_epoch = atomic_load_explicit(&g_vk_surf_epoch, memory_order_relaxed), ww_epoch = vk_ww_epoch();
    const int why = !g_fs_last.valid ? VK_FS_NONE
                    : g_fs_last.ds != ds || g_fs_last.s0 != s0 || g_fs_last.nt != nt ||
                            g_fs_last.last_out != last_out || g_fs_last.rt_int != rt_int || g_fs_last.rt_sint != rt_sint
                        ? VK_FS_PROGRAM
                    : g_fs_last.tex_pool != b->tex_pool || g_fs_last.smp_pool != b->smp_pool ||
                            g_fs_last.tex_max != b->tex_max || g_fs_last.smp_max != b->smp_max ||
                            g_fs_last.smp_mode != b->smp_mode || g_fs_last.tex_bank != b->tex_bank
                        ? VK_FS_POOLS
                    : g_fs_last.gw != gw                                               ? VK_FS_GPU
                    : g_fs_last.adds != adds                                           ? VK_FS_RESULTS
                    : g_fs_last.img_adds != img_adds                                   ? VK_FS_IMAGES
                    : g_fs_last.take != take                                           ? VK_FS_TAKE
                    : g_fs_last.surf_epoch != surf_epoch                               ? VK_FS_SURFACES
                    : g_fs_last.ww_epoch != ww_epoch                                   ? VK_FS_EPOCH
                    : g_fs_last.present != g_vk_present                                ? VK_FS_PRESENT
                    : memcmp(g_fs_last.handles, handles, (size_t)nt * sizeof *handles) ? VK_FS_HANDLES
                    : !fs_last_holds(g_fs_last.res, g_fs_last.over, nt, ww_epoch)      ? VK_FS_QUIET
                                                                                       : VK_FS_KEPT;
    g_stats.n.fs[why]++;
    const int check = why == VK_FS_KEPT && g_settings.draw_state_check;
    FsLast kept;
    if (check) kept = g_fs_last;
    if (why == VK_FS_KEPT && !check) {
        for (int i = 0; i < nt; i++) {
            res[i] = g_fs_last.res[i];
            /* a target read whole is noted read on each draw, as tex_resolve's hit does. Not a crop of one (a copy
               in no row of the surface table, rt_crop): its generation never moves, so it would reload the target
               at its address on each draw */
            const Surf* t = res[i].t;
            if (t && t->is_rt && t->row && !(t->gva >> 63) && t->layout != VK_IMAGE_LAYOUT_UNDEFINED)
                vk_rt_note_read(res[i].t);
            /* a copy of a target: the read is the target's, as the memo's hit notes it */
            if (t && g_fs_last.over[i].rt && g_fs_last.over[i].copy) vk_rt_note_read(g_fs_last.over[i].rt);
        }
        g_stats.n.tex[VK_TEX_KEPT] += (u64)nt;
        *fk = g_fs_last.fk;
        return;
    }
    for (int i = 0; i < nt; i++) {
        const ShTex* st = &s0->si.tex[i];
        tex_resolve(ds, handles[i], &res[i]);
        if (!(st->flags & (SNX_TEX_INDEXED | 1)) && vk_tic_unnormalized(res[i].tic) == 1) fk->rect |= 1ull << i;
        /* a texture only a TXQ names: a 2D query until the header says what it is (a 3D query answers its depth,
           which a 2D slot can't bind) */
        if (!(st->flags & (SNX_TEX_INDEXED | 1)) && (s0->si.tex_query_mask >> i & 1)) {
            fk->query[i] = vk_tic_query_hint(res[i].tic);
            fk->queried |= fk->query[i] != 0;
        }
        if (st->shadow) {
            if (st->flags & SNX_TEX_INDEXED) continue; /* the selected entry's function comes from the table */
            /* a depth compare of a color texture compares its first component, which Vulkan's compare samplers
               can't do on a color image: the shader samples it and compares (the TSC's function, LEQUAL when the
               TSC has none, as vk_tex_bind_sampler sets) */
            Surf* t = res[i].t;
            if (t && !t->f.depth && !t->f.is_int) {
                u32 func = (res[i].tsc[0] >> 9) & 1 ? (res[i].tsc[0] >> 10) & 7 : 3;
                fk->dm |= 1ull << i;
                if (nd < SH_DC_COLOR_MAX) fk->df |= func << (3 * nd);
                nd++;
            }
            continue;
        }
        int num = vk_tex_num(res[i].tic, res[i].t);
        if (num) fk->tm |= 1ull << i;
        if (num == 2) fk->ts |= 1ull << i;
        /* a min/max reduction the device's sampler can't take, which the program takes of the slot's texels: not of
           an indexed slot's (whose texture changes within the draw), nor where a texture only a TXQ names may number
           the variant's slots otherwise (fs_slot_res), nor past the key's room */
        if (st->flags & SNX_TEX_INDEXED) continue;
        int reduce = vk_tex_minmax_in_program(res[i].tsc, res[i].tic, res[i].t, vk_tex_view_type(st), num);
        const char* which = reduce == 1 ? "least" : "greatest";
        if (reduce && (i >= SH_MINMAX_SLOTS || s0->si.tex_query_mask)) {
            if (i >= SH_MINMAX_SLOTS)
                LOG_LIMIT(1,
                          "vulkan: a fragment program's texture %d averages the texels its sampler asks the %s of: "
                          "the program takes them itself for its first %d textures\n",
                          i, which, SH_MINMAX_SLOTS);
            else
                LOG_LIMIT(1,
                          "vulkan: a fragment program's texture %d averages the texels its sampler asks the %s of: "
                          "a program with a texture only a TXQ names doesn't take them itself\n",
                          i, which);
            continue;
        }
        if (reduce) fk->minmax |= (1u | (reduce == 2 ? 1u << SH_MINMAX_SLOTS : 0)) << i;
    }
    fk->rm &= s0->si.sph[18] ? 0xFF : 0;
    fk->rs &= fk->rm;
    u32 signed_images;
    fk->rm |= image_variant(ds, 4, s0, &signed_images) << 8;
    fk->rs |= signed_images << 8;
    if (check) fs_key_check(&kept, res, nt, fk);
    g_fs_last.valid = 0; /* all a reader asks before the rest, which the next key kept writes whole */
    /* image_variant reads the image bindings, which the key doesn't hold: a program with images is keyed each draw */
    if (s0->si.nimg || !fs_last_keepable(res, nt, g_fs_last.over)) {
        g_stats.n.fs[s0->si.nimg ? VK_FS_UNKEPT_IMAGES : VK_FS_UNKEPT_TEXTURES]++;
        return;
    }
    g_fs_last.ds = ds;
    g_fs_last.s0 = s0;
    g_fs_last.nt = nt;
    g_fs_last.last_out = last_out;
    g_fs_last.rt_int = rt_int;
    g_fs_last.rt_sint = rt_sint;
    g_fs_last.tex_pool = b->tex_pool;
    g_fs_last.smp_pool = b->smp_pool;
    g_fs_last.tex_max = b->tex_max;
    g_fs_last.smp_max = b->smp_max;
    g_fs_last.smp_mode = b->smp_mode;
    g_fs_last.tex_bank = b->tex_bank;
    g_fs_last.gw = gw;
    g_fs_last.adds = adds;
    g_fs_last.img_adds = img_adds;
    g_fs_last.take = take;
    g_fs_last.surf_epoch = surf_epoch;
    g_fs_last.ww_epoch = ww_epoch;
    g_fs_last.present = g_vk_present;
    for (int i = 0; i < nt; i++) {
        g_fs_last.handles[i] = handles[i];
        g_fs_last.res[i] = res[i];
    }
    g_fs_last.fk = *fk;
    g_fs_last.valid = 1;
}
/* the fragment shader at addr in the variant fk names, s0 when it names none */
static VkSh* fs_variant(const DsState* ds, u64 addr, VkSh* s0, const FsKey* fk) {
    if (!fk->tm && !fk->rm && !fk->rs && !fk->dm && !fk->unfed && !fk->rect && !fk->queried && !fk->advanced_blend &&
        !fk->advanced_mask && !fk->config && !fk->minmax && !fk->predraw && !fk->early)
        return s0;
    return vk_shader_get_ex(ds, addr, 0, ST_FS, fk->tm, fk->ts, fk->rm, fk->rs, fk->prim, fk->dm, fk->df, fk->unfed,
                            fk->rect, NULL, fk->queried ? fk->query : NULL, fk->advanced_blend, fk->advanced_mask,
                            fk->rm >> 8, fk->rs >> 8, fk->config, fk->minmax, fk->predraw, (int)fk->early);
}
/* whether slots a and b read their handles from the same words (tex_handle) */
static int tex_same_handle(const ShTex* a, const ShTex* b) {
    return a->cbuf_off == b->cbuf_off && a->bindless == b->bindless && a->bl_cbuf == b->bl_cbuf && a->bl2 == b->bl2 &&
           a->bl2_cbuf == b->bl2_cbuf && a->bl2_off == b->bl2_off && a->stride == b->stride &&
           (a->flags & (SNX_TEX_INDEXED | SNX_TEX_MEMORY)) == (b->flags & (SNX_TEX_INDEXED | SNX_TEX_MEMORY));
}
/* the resolutions of s's texture slots, from res0, which fs_key filled for s0's: res0 itself when s numbers its slots
   as s0 does, else out, each slot taking the resolution of the slot of s0 that reads its handle, or its own. A variant
   may number them otherwise: a texture only a TXQ names takes the dimension of its header there, and its slot is then
   the one that samples the texture as that dimension, which moves the slots after it down by one */
static TexRes* fs_slot_res(const DsState* ds, const VkSh* s0, TexRes* res0, const VkSh* s, TexRes* out) {
    int n0 = s0->si.ntex > SH_MAXTEX ? SH_MAXTEX : s0->si.ntex, n = s->si.ntex > SH_MAXTEX ? SH_MAXTEX : s->si.ntex;
    int same = n == n0;
    for (int i = 0; same && i < n; i++) same = tex_same_handle(&s->si.tex[i], &s0->si.tex[i]);
    if (same) return res0;
    for (int i = 0; i < n; i++) {
        const ShTex* st = &s->si.tex[i];
        int j = 0;
        while (j < n0 && !tex_same_handle(st, &s0->si.tex[j])) j++;
        if (j < n0)
            out[i] = res0[j];
        else
            tex_resolve(ds, tex_handle(ds, 4, ds->bind.tex_bank, &s->si, i), &out[i]);
    }
    return out;
}

VkImageViewType vk_tex_view_type(const ShTex* st) {
    return st->type == 2   ? VK_IMAGE_VIEW_TYPE_3D
           : st->type == 3 ? (st->array ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE)
           : st->array     ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                           : VK_IMAGE_VIEW_TYPE_2D;
}

/* The TIC selects an sRGB-capable format; the TSC controls whether sampling unpacks it to linear. */
static int tex_srgb(const u32* tic, const u32* tsc) { return ((tic[4] >> 22) & 1) && ((tsc[0] >> 13) & 1); }

/* the component mapping of t's view for the header tic */
static VkComponentMapping tex_map(const Surf* t, const u32* tic) {
    static const VkComponentSwizzle sw[8] = {
        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R,   VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_B,    VK_COMPONENT_SWIZZLE_A,    VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE};
    VkComponentSwizzle m[4] = {sw[(tic[0] >> 19) & 7], sw[(tic[0] >> 22) & 7], sw[(tic[0] >> 25) & 7],
                               sw[(tic[0] >> 28) & 7]};
    /* the header's swizzle is in memory component order: B, G, R, A targets swap R and B */
    if (t->f.bgra)
        for (int k = 0; k < 4; k++)
            m[k] = m[k] == VK_COMPONENT_SWIZZLE_R   ? VK_COMPONENT_SWIZZLE_B
                   : m[k] == VK_COMPONENT_SWIZZLE_B ? VK_COMPONENT_SWIZZLE_R
                                                    : m[k];
    if (t->f.depth && t->f.stencil && vk_tic_reads_stencil(tic)) { /* the stencil view's R where the header names
                                                                        the stencil, its constants, nought for depth */
        u32 sc = vk_tic_stencil_component(tic[0] & 0x7F);
        for (int k = 0; k < 4; k++) {
            u32 c = (tic[0] >> (19 + 3 * k)) & 7;
            m[k] = c == sc ? VK_COMPONENT_SWIZZLE_R : c >= 6 ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_ZERO;
        }
    } else if (t->f.depth) { /* depth in r, g and b, alpha 1 */
        m[0] = m[1] = m[2] = VK_COMPONENT_SWIZZLE_R;
        m[3] = VK_COMPONENT_SWIZZLE_ONE;
    } else if (t->f.conv == VKC_ASTC && (vk_astc_bcn(t->f.vk) == 4 || vk_astc_bcn(t->f.vk) == 5)) {
        /* ASTC held as BC4 (luminance) or BC5 (luminance and alpha, texture.c): the texels R, G and B the game's
           blocks decode to are the image's R, and alpha its G, or 1 */
        for (int k = 0; k < 4; k++)
            m[k] = m[k] == VK_COMPONENT_SWIZZLE_A
                       ? (vk_astc_bcn(t->f.vk) == 5 ? VK_COMPONENT_SWIZZLE_G : VK_COMPONENT_SWIZZLE_ONE)
                   : m[k] == VK_COMPONENT_SWIZZLE_G || m[k] == VK_COMPONENT_SWIZZLE_B ? VK_COMPONENT_SWIZZLE_R
                                                                                      : m[k];
    }
    return t->fkey & (1u << 30) ? (VkComponentMapping){VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO,
                                                       VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE}
                                : (VkComponentMapping){m[0], m[1], m[2], m[3]};
}

VkImageView vk_tex_bind_view(Surf* t, VkImageViewType want, const u32* tic, const u32* tsc, int num) {
    int stencil = t && t->f.stencil && vk_tic_reads_stencil(tic);
    if (!t || (stencil ? 1 : t->f.is_int ? (t->f.sint ? 2 : 1) : 0) != num) return VK_NULL_HANDLE;
    VkComponentMapping map = tex_map(t, tic);
    return vk_tex_view(t, want, tex_srgb(tic, tsc), &map, stencil);
}

/* each component of map is its own: what a min/max sampler reads where the device takes no other swizzle */
static int tex_map_identity(VkComponentMapping map) {
    return (map.r == VK_COMPONENT_SWIZZLE_IDENTITY || map.r == VK_COMPONENT_SWIZZLE_R) &&
           (map.g == VK_COMPONENT_SWIZZLE_IDENTITY || map.g == VK_COMPONENT_SWIZZLE_G) &&
           (map.b == VK_COMPONENT_SWIZZLE_IDENTITY || map.b == VK_COMPONENT_SWIZZLE_B) &&
           (map.a == VK_COMPONENT_SWIZZLE_IDENTITY || map.a == VK_COMPONENT_SWIZZLE_A);
}

/* Why a min/max reduction can't go to the device's sampler for t's view of the header tic, with the sampler tsc: 0
   when it can, 1 when the device has no min/max filter, 2 when the view's format (*f) has none on it, 3 when it takes
   no swizzle with it (filterMinmaxImageComponentMapping) and the view has one: Vulkan needs r to be the identity, and
   the other components are poison, which a shader that reads them (a depth view's g, b and a) would get */
static int minmax_unsampled(const Surf* t, const u32* tic, const u32* tsc, VkFormat* f) {
    *f = vk_tex_view_format(t, tex_srgb(tic, tsc));
    return !g_vk.feat12.samplerFilterMinmax                                 ? 1
           : !vk_format_minmax_filter(*f)                                   ? 2
           : !g_vk.has_minmax_swizzle && !tex_map_identity(tex_map(t, tic)) ? 3
                                                                            : 0;
}
/* The line for a sampler asking for which (the least or the greatest) of its texels that the device's sampler can't
   take (why: minmax_unsampled's, of format f) and that averages them: also why the program doesn't take them itself
   (not: what it can't take them of; stage: its stage), when that is so */
static void minmax_averaged(int why, VkFormat f, const char* which, const char* not, const char* stage) {
    char in_program[160] = "";
    if (not)
        snprintf(in_program, sizeof in_program,
                 " (a %s program takes them itself from a 2D texture's base level filtered linearly, not of %s)", stage,
                 not);
    if (why == 1)
        LOG_LIMIT(1,
                  "vulkan: the device has no min/max filter: a sampler asking for the %s of its texels averages "
                  "them%s\n",
                  which, in_program);
    else if (why == 2)
        LOG_LIMIT(1,
                  "vulkan: VkFormat %d has no min/max filter on this device: a sampler asking for the %s of its "
                  "texels averages them%s\n",
                  (int)f, which, in_program);
    else
        LOG_LIMIT(1,
                  "vulkan: the device's min/max filter takes no swizzle: a sampler asking for the %s of the "
                  "texels of a swizzled view averages them%s\n",
                  which, in_program);
}
/* The min/max reduction a sample through the sampler tsc (decoded: sf) of a texture of levels levels viewed as want
   takes in a program of stage (ST_FS or ST_CS) instead of the device's sampler, which can't (why: minmax_unsampled's,
   format f): 1 the least, 2 the greatest (shader_minmax_sample). The program takes the 2x2 texels of the base level's
   bilinear footprint: for a 2D view or an array of them, of one level or sampled without a mip filter under a
   minimum LOD of a half (vk_sampler's base level), filtered linearly both ways and without anisotropy. Otherwise 0:
   the sampler averages, and the log says why */
static int minmax_program(const TscFields* sf, u32 levels, VkImageViewType want, int why, VkFormat f, int stage) {
    const char* not= want != VK_IMAGE_VIEW_TYPE_2D && want != VK_IMAGE_VIEW_TYPE_2D_ARRAY ? "a cube or a 3D texture"
                     : sf->mag != 2 || sf->min != 2                       ? "a filter linear one way only"
                     : sf->anisotropy                                     ? "anisotropic filtering"
                     : levels > 1 && (sf->mip != 1 || sf->min_lod >= 128) ? "a texture of several levels"
                                                                          : NULL;
    if (!not) return (int)sf->reduction;
    minmax_averaged(why, f, sf->reduction == 1 ? "least" : "greatest", not, stage == ST_CS ? "compute" : "fragment");
    return 0;
}
/* whether a sample through the sampler tsc (decoded into sf) asks for a min/max reduction that changes what it reads:
   filtered linearly some way, no compare, of a float texture (num 0) */
static int minmax_asked(const u32* tsc, int num, TscFields* sf) {
    tsc_decode(tsc, sf);
    return (sf->reduction == 1 || sf->reduction == 2) && !num && !sf->compare &&
           (sf->mag == 2 || sf->min == 2 || sf->mip == 3);
}
/* A fragment program's: the reduction the sampler tsc asks of the texture t (header tic, viewed as want, of number
   type num) that the program takes itself (minmax_program), 0 when it asks for none, the device's sampler takes it,
   or no texture is bound */
int vk_tex_minmax_in_program(const u32* tsc, const u32* tic, const Surf* t, VkImageViewType want, int num) {
    TscFields sf;
    VkFormat f;
    int why;
    if (!t || !minmax_asked(tsc, num, &sf) || !(why = minmax_unsampled(t, tic, tsc, &f))) return 0;
    return minmax_program(&sf, t->levels, want, why, f, ST_FS);
}
/* A compute program's, by the header tic alone (compute keys a program before its textures are resolved): the
   levels and format of the view the header makes, and the device's filter. A view's swizzle is the bind's to find
   (vk_tex_bind_sampler, which then averages, as without this) */
int vk_tex_minmax_in_program_tic(const u32* tsc, const u32* tic, VkImageViewType want, int num) {
    TscFields sf;
    TicFields tf;
    if (!minmax_asked(tsc, num, &sf)) return 0;
    tic_decode(tic, &tf);
    VkFormat f = vk_tic_format(tic[0], tex_srgb(tic, tsc)).vk;
    int why = !g_vk.feat12.samplerFilterMinmax ? 1 : !vk_format_minmax_filter(f) ? 2 : 0;
    if (!why) return 0;
    /* the levels texture.c's view of the header shows: base level to max level, one for pitch-linear memory */
    u32 levels = tf.levels;
    if (tf.header == 2)
        levels = 1;
    else if (tf.base_level < tf.levels && (tf.base_level || tf.max_level + 1 != tf.levels)) {
        levels = tf.max_level >= tf.base_level ? tf.max_level - tf.base_level + 1 : 1;
        if (levels > tf.levels - tf.base_level) levels = tf.levels - tf.base_level;
    }
    return minmax_program(&sf, levels, want, why, f, ST_CS);
}

/* the texture (NULL for none) and the view type say whether anisotropy= may raise its filtering: 2D textures with
   mipmaps only, as the yuzu family and Ryujinx do (a cube, a 3D LUT or a copied target keeps the game's). in_program:
   the program takes the min/max reduction the sampler asks for itself (vk_tex_minmax_in_program), which the sampler
   then leaves to it */
VkSampler vk_tex_bind_sampler(const u32* tsc, const u32* tic, const ShTex* st, int num, const Surf* tex,
                              VkImageViewType want, int in_program) {
    u32 tc[8];
    memcpy(tc, tsc, sizeof tc);
    /* the compare mode is what the shader declares */
    if (st->shadow) {
        if (!((tc[0] >> 9) & 1)) tc[0] = (tc[0] & ~(7u << 10)) | 1u << 9 | 3u << 10; /* LEQUAL */
    } else
        tc[0] &= ~(1u << 9);
    /* integer textures can't be filtered: nearest, and a linear mip filter becomes nearest */
    if (num) {
        u32 mip = (tc[1] >> 6) & 3;
        tc[1] = (tc[1] & ~0xF3u) | 0x11u | (mip == 3 ? 2u : mip) << 6;
    }
    /* a min/max reduction (tc[1] bits 10-11) stays where it can change what the filter reads and Vulkan takes it: a
       linear filter (magnification, minification or mip) of a view whose format has the min/max filter on this device
       (minmax_unsampled), without a compare, which Vulkan's samplers only average. Elsewhere the sampler averages: the
       null texture's texels are all alike, nearest filters read one, and a program that takes the reduction itself
       gathers the texels */
    u32 reduction = (tc[1] >> 10) & 3;
    if (reduction == 1 || reduction == 2) {
        int linear = (tc[1] & 3) == 2 || ((tc[1] >> 4) & 3) == 2 || ((tc[1] >> 6) & 3) == 3;
        const char* which = reduction == 1 ? "least" : "greatest";
        VkFormat f;
        int why;
        if (!tex || num || !linear)
            tc[1] &= ~(3u << 10);
        else if ((tc[0] >> 9) & 1) {
            LOG_LIMIT(1,
                      "vulkan: a depth compare sampler asks for the %s of its texels, which Vulkan's compare samplers "
                      "don't take: they average them\n",
                      which);
            tc[1] &= ~(3u << 10);
        } else if ((why = minmax_unsampled(tex, tic, tsc, &f)) != 0) {
            if (!in_program) minmax_averaged(why, f, which, NULL, NULL);
            tc[1] &= ~(3u << 10);
        }
    }
    /* raised only with anisotropy= set: without it the cache keeps one sampler per TSC */
    int raise = g_vk_anisotropy > 1 && tex && tex->levels > 1 &&
                (want == VK_IMAGE_VIEW_TYPE_2D || want == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    return vk_sampler(tc, raise, num != 0);
}

/* the way of g_tex_memo that x's headers were kept in, while it still keeps them: NULL for none */
static TexMemo* tex_memo_of(const TexRes* x) {
    if (!x->memo || x->memo > TEX_MEMO_WAYS << TEX_MEMO_SET_BITS) return NULL;
    TexMemo* m = &g_tex_memo.set[(x->memo - 1) / TEX_MEMO_WAYS][(x->memo - 1) % TEX_MEMO_WAYS];
    if (!m->t || m->handle != x->handle || memcmp(m->tic, x->tic, sizeof m->tic) || memcmp(m->tsc, x->tsc, sizeof m->tsc))
        return NULL;
    return m;
}
/* vk_tex_bind_view of t with x's headers, kept with them (TexMemo.view) in the frame. draw_state_check=1 makes a kept
   one again and compares */
static VkImageView tex_bind_view(const TexRes* x, Surf* t, VkImageViewType want, int num) {
    TexMemo* m = t ? tex_memo_of(x) : NULL;
    u64 frame = vk_frame_id() + 1;
    u32 how = (u32)want | (u32)num << 8;
    if (m && m->view.frame == frame && m->view.t == t && m->view.img == t->img && m->view.floor == t->lod_floor &&
        m->view.how == how) {
        g_stats.n.tex[VK_TEX_VIEW_KEPT]++;
        if (!g_settings.draw_state_check) return m->view.view;
        VkImageView made = vk_tex_bind_view(t, want, x->tic, x->tsc, num);
        if (made != m->view.view) {
            draw_differs("texture view");
            m->view.frame = 0;
        }
        return made;
    }
    VkImageView v = vk_tex_bind_view(t, want, x->tic, x->tsc, num);
    if (m && v) {
        m->view.frame = frame;
        m->view.t = t;
        m->view.img = t->img;
        m->view.floor = t->lod_floor;
        m->view.how = how;
        m->view.view = v;
    }
    return v;
}
/* vk_tex_bind_sampler with x's headers, kept with them (TexMemo.smp) in the frame. draw_state_check=1 makes a kept one
   again and compares */
static VkSampler tex_bind_sampler(const TexRes* x, const ShTex* st, int num, const Surf* sampled, VkImageViewType want,
                                  int in_program) {
    TexMemo* m = tex_memo_of(x);
    u64 frame = vk_frame_id() + 1;
    u32 how = (u32)want | (u32)num << 8 | (u32)(st->shadow != 0) << 16 | (u32)(in_program != 0) << 17;
    VkImage img = sampled ? sampled->img : VK_NULL_HANDLE;
    if (m && m->smp.frame == frame && m->smp.t == sampled && m->smp.img == img && m->smp.how == how) {
        g_stats.n.tex[VK_TEX_SAMPLER_KEPT]++;
        if (!g_settings.draw_state_check) return m->smp.smp;
        VkSampler made = vk_tex_bind_sampler(x->tsc, x->tic, st, num, sampled, want, in_program);
        if (made != m->smp.smp) {
            draw_differs("sampler");
            m->smp.frame = 0;
        }
        return made;
    }
    VkSampler smp = vk_tex_bind_sampler(x->tsc, x->tic, st, num, sampled, want, in_program);
    if (m && smp) {
        m->smp.frame = frame;
        m->smp.t = sampled;
        m->smp.img = img;
        m->smp.how = how;
        m->smp.smp = smp;
    }
    return smp;
}
/* a constant buffer's bytes for this draw, reused while the guest can't have changed them. An inline block (in) is
   staged once while its generation holds. A buffer in guest memory is kept by its address as well as by its stage and
   slot: what vk_buf_get_strict gives for it depends on nothing else. One game binds another buffer to the same slot in
   most draws, and those missed the slot's entry in 37% of the lookups. An entry is kept as long as what it binds lives
   (life): bytes in the frame's staging (vk_stage, vk_stage_upload) until the frame ends (vk_frame_id: the slot of its
   rings is used again only after vk_frame_next), the buffer cache's own buffer while its entry keeps it
   (vk_buf_own_holds), and anything else (a copy in the stream, the page cache, guest memory in place, the mirror) for
   its submission, which its reads and their effects are for. Submissions go for each millisecond of the GPU thread's
   work (cmd.c's frame_check): one game's race made 55 a present, and an entry kept only for its submission had every
   constant buffer looked up again. A submission's landing changes nothing a kept entry binds without telling it: a
   result lands or is published with a GPU-side write's stamp (gpu_mem_written_*), which gpu_mem_dirty_since sees, and
   one added since the entry moves g_pend_any.adds */
enum { CB_SUBMIT, CB_FRAME, CB_OWN };
typedef struct {
    u64 a, gw, adds;
    u64 made; /* the submission it was made in: its life with CB_SUBMIT */
    u64 at;   /* CB_FRAME: vk_frame_id() then; CB_OWN: the own buffer's number (vk_buf_own_got) */
    u32 sz, epoch;
    u32 life, own; /* CB_*, and with CB_OWN the buffer cache's entry */
    VkBuffer buf;
    u64 off;
    const u8* in; /* or the inline block staged, of generation gen */
    u64 gen;
    /* draw_state_check=1: the hash of the bytes it was made of (chk_have), and the write tracking clock before they
       were read (cb_check) */
    u64 chk;
    u32 wseen;
    int chk_have;
} CbEntry;
/* the entries: by stage and slot, by address, and the root tables' (root_get) */
static struct {
    CbEntry slot[DS_GFX_STAGES][18];
    CbEntry by_addr[1024];
    CbEntry root;
} g_cb;
/* the results added that a kept entry was checked against: results of every kind (the mirror's writes, storage
   images and queued releases too: an entry lives past the submission that lands them). Both counts only grow, so
   their sum stays while both do */
static u64 cb_adds(void) {
    return atomic_load_explicit(&g_pend_any.adds, memory_order_relaxed) +
           vk_uma_writes(); /* the writes in place, which no other count has */
}
/* e made now, binding what lives as life says. CB_OWN: what the buffer cache's last lookup bound, its own buffer or
   else something of the submission's */
static void cb_life(CbEntry* e, u32 life) {
    e->life = life;
    e->made = vk_submit_number();
    e->at = 0;
    e->own = 0;
    if (e->life == CB_FRAME) e->at = vk_frame_id();
    if (e->life == CB_OWN && !(e->own = vk_buf_own_got(&e->at))) e->life = CB_SUBMIT;
}
/* whether what e binds still lives (cb_life). The entries kept past their submission are counted */
static int cb_lives(const CbEntry* e) {
    u64 sub = vk_submit_number();
    int lives = e->life == CB_SUBMIT  ? e->made == sub
                : e->life == CB_FRAME ? e->at == vk_frame_id()
                                      : vk_buf_own_holds(e->own, e->at);
    if (lives && e->made != sub) g_stats.n.cb[VK_CB_PAST_SUBMIT]++;
    return lives;
}
/* draw_state_check=1: the hash of the sz guest bytes at a, when the CPU can read them where they are: one run of guest
   memory on no page the GPU owns, whose read would wait for the renderer's lock this thread holds (1) */
static int cb_source_hash(u64 a, u32 sz, u64* h) {
    u64 cpu = 0;
    if (!a || gmmu_run(a, sz, &cpu) != sz || (own_any() && own_trapped(cpu, sz))) return 0;
    *h = vk_hash(GP(cpu), sz);
    return 1;
}
/* draw_state_check=1, for the entry e made now of the guest bytes at a, or of n bytes of the inline block in, with the
   write tracking clock ws from before they were read: the hash of those bytes, which a draw that keeps e compares */
static void cb_check_note(CbEntry* e, u64 a, const u8* in, u32 n, u32 ws) {
    e->chk_have = 0;
    if (!g_settings.draw_state_check) return;
    e->wseen = ws;
    if (in) {
        e->chk = vk_hash(in, n);
        e->chk_have = 1;
    } else
        e->chk_have = cb_source_hash(a, e->sz, &e->chk);
}
/* draw_state_check=1, for a draw that keeps e: its bytes made again and compared, those of n bytes of the inline block
   in, or those of the guest memory at a where the CPU wrote nothing since (a write in the same epoch is for the next
   epoch to show). No result of any kind may be pending over a: a lookup made again binds that */
static void cb_check(const CbEntry* e, u64 a, const u8* in, u32 n) {
    u64 h, cpu = 0;
    if (a && pend_hit(&g_pend_any, a, e->sz)) draw_differs("constant buffer, with a result pending over it");
    if (!e->chk_have) return;
    if (in) {
        if (vk_hash(in, n) != e->chk) draw_differs("constant buffer's inline bytes");
        return;
    }
    if (gmmu_run(a, e->sz, &cpu) == e->sz && vk_written_since(GP(cpu), e->sz, e->wseen) == 0 &&
        cb_source_hash(a, e->sz, &h) && h != e->chk)
        draw_differs("constant buffer's bytes");
}
/* e holds a's bytes, sz of them at least, for this draw: checked as it is cached, and with adds, with no result added
   since (none can cover it, and only a write of its memory leaves the copy stale) */
static int cb_kept(const CbEntry* e, u64 a, u32 sz, int adds, u32 epoch) {
    return a && e->a == a && e->epoch == epoch && e->sz >= sz && (!adds || e->adds == cb_adds()) &&
           !gpu_mem_dirty_since(e->gw, a, sz) && cb_lives(e);
}
/* VkDrawCounts.inl's row of a lookup of sz bytes of the inline block in */
static int inline_kind(DsCbBlock in, u32 sz) {
    return in.size != DS_INLINE_BANK0 ? 2 : sz > SH_CB0_BASE_VERTEX ? 0 : 1;
}
/* the first sz bytes of the root tables an Ounce program reads (DsInline.root), staged once while their generation
   holds (DsState.root_gen): every graphics stage reads the same tables */
static int root_get(const DsState* ds, u32 sz, VkBuffer* buf, u64* off) {
    CbEntry* e = &g_cb.root;
    if (e->in == ds->inl.root && e->gen == ds->root_gen && e->sz >= sz && cb_lives(e)) {
        *buf = e->buf;
        *off = e->off;
        g_stats.n.inl[3][0]++;
        if (g_settings.draw_state_check) cb_check(e, 0, ds->inl.root, e->sz);
        return 1;
    }
    u8* p = vk_stage(sz, vk_stage_align(), buf, off);
    if (!p) return 0;
    memcpy(p, ds->inl.root, sz);
    *e = (CbEntry){.sz = sz, .buf = *buf, .off = *off, .in = ds->inl.root, .gen = ds->root_gen};
    cb_life(e, CB_FRAME);
    cb_check_note(e, 0, ds->inl.root, sz, 0);
    g_stats.n.inl[3][1]++;
    return 1;
}
/* constant buffer i of a stage, at an offset that is a multiple of align: 16 for a bank bound through a window
   (cb_window_place), vk_stage_align() for one bound alone. A kept entry at another offset is not taken by a bank that
   binds alone */
static int cb_get(const DsState* ds, int stage, int i, u32 sz, u64 align, VkBuffer* buf, u64* off) {
    int check = g_settings.draw_state_check != 0;
    DsCbBlock in = ds_cb_inline(ds, stage, (u32)i);
    if (in.bytes) {
        /* staged again only when this block changed (DsState.inl_gen), not when another inline block did */
        const u64 gen = ds_cb_inline_gen(ds, stage, (u32)i);
        CbEntry* e = &g_cb.slot[stage][i];
        if (e->in == in.bytes && e->gen == gen && e->sz >= sz && cb_lives(e)) {
            *buf = e->buf;
            *off = e->off;
            g_stats.n.cb[VK_CB_INLINE_KEPT]++;
            g_stats.n.inl[inline_kind(in, sz)][0]++;
            if (check) cb_check(e, 0, in.bytes, e->sz < in.size ? e->sz : in.size);
            return 1;
        }
        u8* p = vk_stage(sz, vk_stage_align(), buf, off);
        if (!p) return 0;
        u32 n = sz < in.size ? sz : in.size;
        memcpy(p, in.bytes, n);
        memset(p + n, 0, sz - n); /* past the block: zeros, as an unbound buffer reads */
        *e = (CbEntry){.in = in.bytes, .gen = gen, .sz = sz, .buf = *buf, .off = *off};
        cb_life(e, CB_FRAME);
        cb_check_note(e, 0, in.bytes, n, 0);
        g_stats.n.cb[VK_CB_INLINE_STAGED]++;
        g_stats.n.inl[inline_kind(in, sz)][1]++;
        return 1;
    }
    u64 a = ds->bind.cb_addr[stage][i];
    u32 epoch = vk_ww_epoch(); /* before the buffer is checked */
    u32 ws = check ? vk_ww_now() : 0;
    CbEntry* e = &g_cb.slot[stage][i];
    CbEntry* x = &g_cb.by_addr[(u32)((a >> 8) * 0x9E3779B97F4A7C15ull >> 54)];
    /* The pending checks were the most of cb_get's time in one game: an entry is taken first while no result was
       added since it was made */
    if ((cb_kept(e, a, sz, 1, epoch) && !(e->off % align)) ||
        (cb_kept(x, a, sz, 1, epoch) && !(x->off % align) && (*e = *x, 1))) {
        *buf = e->buf;
        *off = e->off;
        g_stats.n.cb[VK_CB_KEPT]++;
        if (check) cb_check(e, a, NULL, 0);
        return 1;
    }
    /* Pending compute results there land first. A pending graphics result is newer than the guest memory the checks
       below see: the buffer then comes from vk_buf_get_strict, which reads it on the GPU, and isn't cached. Neither is
       one under a result of another kind (the mirror's writes, a storage image, a queued release), which the buffer
       cache looks for: the entry would live past the submission that lands it */
    if (atomic_load_explicit(&g_cs_any, memory_order_relaxed) && a) vk_compute_mem_need(a, sz);
    int pending = a && ((atomic_load_explicit(&g_gfx_any, memory_order_relaxed) && vk_buf_pending(a, sz)) ||
                        vk_uma_pending(a, sz) || /* written in place too */
                        pend_hit(&g_pend_any, a, sz));
    if (!pending && ((cb_kept(e, a, sz, 0, epoch) && !(e->off % align)) ||
                     (cb_kept(x, a, sz, 0, epoch) && !(x->off % align) && (*e = *x, 1)))) {
        /* no result of any kind is over the bytes it holds: the next draw takes it at the first check, while no
           result is added */
        if (e->sz == sz) {
            e->adds = cb_adds();
            if (x->a == a && x->sz == sz && x->buf == e->buf && x->off == e->off) x->adds = e->adds;
        }
        *buf = e->buf;
        *off = e->off;
        g_stats.n.cb[VK_CB_KEPT_CHECKED]++;
        if (check) cb_check(e, a, NULL, 0);
        return 1;
    }
    /* a constant buffer's bytes are usually the CPU's, in one run: copy them straight into staging. The general buffer
       path (the mirror, the cache, the hash) runs only where a result holds them, the mirror has them, the run isn't
       whole, unified memory reads them in place, or the GPU owns some of them (memory/own.c): that path copies those
       from their owner on the GPU. The CPU's bytes on a page the GPU owns others of are trapped, and this thread holds
       the renderer's lock, which their landing needs: own_stage reads them through the pool's mirror. What the entry
       is checked against is read before the bytes are. Unified memory whose buffer cache copies them
       (vk_uma_imports_dear) copies them here too: guest memory has their bytes once no write in place is
       pending over them (pending above), and the cache's checks and hash cost each constant buffer more than its copy */
    u64 cpu = 0, adds = cb_adds(), gw = gpu_mem_generation();
    u32 life = CB_OWN; /* the buffer cache's own buffer, or else what lives for the submission */
    if (a && !pending && (!vk_uma() || vk_uma_imports_dear()) && !vk_mirror_pending(a, sz) && !vk_mirror_holds(a, sz) &&
        gmmu_run(a, sz, &cpu) == sz) {
        u8* p = vk_stage_upload(sz, vk_stage_align(), buf, off);
        if (!p) return 0;
        life = CB_FRAME;
        if (!own_any())
            memcpy(p, GP(cpu), sz);
        else if (own_stage(cpu, sz, p, NULL, 0)) {
            if (!vk_buf_get_uniform_at(a, sz, align, buf, off)) return 0;
            life = CB_OWN;
        }
        g_stats.n.cb[VK_CB_COPIED]++;
        g_cb_copied_bytes += sz;
    } else if (!(a ? vk_buf_get_uniform_at(a, sz, align, buf, off) : vk_buf_copy(a, sz, buf, off)))
        return 0;
    else
        g_stats.n.cb[pending ? VK_CB_PENDING : VK_CB_CACHE]++;
    if (pending) {
        e->a = 0;
        return 1;
    }
    *e = (CbEntry){.a = a, .adds = adds, .gw = gw, .sz = sz, .epoch = epoch, .buf = *buf, .off = *off};
    cb_life(e, a ? life : CB_SUBMIT);
    if (a) {
        cb_check_note(e, a, NULL, 0, ws);
        *x = *e;
    }
    return 1;
}

/* A constant buffer mapped by address (vk_cb_addr_mask) has no range, so its binding must hold every byte the shader
   reads. Where the game bound fewer bytes than that (cb_range's sz under cbuf_max), the descriptor's robust range read
   zeros past them, as the console's GPU does past a bank's size: here the sz bytes at *b + *o go into a block of need
   bytes whose rest is zeros, on the GPU (they can be a result the GPU has yet to write), and *b, *o name the block */
static int cb_pad(VkBuffer* b, u64* o, u32 sz, u32 need) {
    VkBuffer pb;
    u64 po;
    u8* p = vk_stage(need, vk_stage_align(), &pb, &po);
    if (!p) return 0;
    memset(p, 0, need);
    vk_render_end(); /* the copy happens outside rendering passes */
    VkCommandBuffer cb = vk_cmd();
    vk_all_barrier(cb); /* what wrote the bytes, before the copy reads them */
    VkBufferCopy c = {*o, po, sz};
    rec_CmdCopyBuffer(cb, *b, pb, 1, &c);
    vk_all_barrier(cb); /* the copy, before the draw reads the block */
    *b = pb;
    *o = po;
    g_cb_padded++;
    return 1;
}
/* the bytes of constant buffer i that stage s reads, as its descriptor binds them */
static u32 cb_range(const DsState* ds, int stage, const VkSh* s, int i) {
    u32 sz = ds->bind.cb_size[stage][i];
    if (sz > 65536 || !sz) sz = 65536;
    if (s->si.cbuf_max[i] && sz > s->si.cbuf_max[i]) sz = s->si.cbuf_max[i]; /* only the bytes the shader reads */
    return vk_cb_range(sz);
}
/* A third of a draw's constant buffers miss cb_get's entries, and each of those looks its range up in the buffer
   cache: two cache misses in a row, the index's slot and then the entry it names, which were most of buf_get's time
   in one game. A draw fetches every slot early (step 0) and the entries they name once the slots are in (step 1), as
   it does for its vertex streams (vk_buf_prefetch) */
static void cb_prefetch(const DsState* ds, int stage, const VkSh* s, int step) {
    for (u32 m = s->si.cbuf_mask & 0x3FFFF; m; m &= m - 1) {
        int i = snx_ctz32(m);
        u64 a = ds->bind.cb_addr[stage][i];
        if (a && !ds_cb_inline(ds, stage, (u32)i).bytes) vk_buf_prefetch(a, cb_range(ds, stage, s, i), step);
    }
}

/* small constants in this frame's staging: 1/resolution scale (SNX_RS) at 0, the 0,0,0,1 of attributes without
   data at 16 (float) and 32 (integer). Staged once a frame and scale (vk_stage lives for the frame) */
static int frame_consts(VkBuffer* buf, u64* off) {
    static u64 frame = ~0ull, o;
    static VkBuffer b;
    static float scale;
    u64 at = vk_frame_id();
    if (frame != at || scale != g_vk_scale) {
        u8* p = vk_stage(64, vk_stage_align(), &b, &o);
        if (!p) return 0;
        float rs[4] = {1 / g_vk_scale, 1 / g_vk_scale, g_vk_scale, 0}, fa[4] = {0, 0, 0, 1};
        u32 ia[4] = {0, 0, 0, 1};
        memcpy(p, rs, 16);
        memcpy(p + 16, fa, 16);
        memcpy(p + 32, ia, 16);
        memset(p + 48, 0, 16); /* empty vertex bindings retain their original format */
        snx_le32_array(p, 12); /* the device reads them little-endian */
        frame = at;
        scale = g_vk_scale;
    }
    *buf = b;
    *off = o;
    return 1;
}

typedef struct {
    VkWriteDescriptorSet w[SNX_VKB_END];
    VkDescriptorBufferInfo bi[SNX_VKB_END];
    VkDescriptorImageInfo ii[SNX_VKB_END];
    VkDescriptorImageInfo depth_table[SNX_TEX_ARRAY];
    VkDescriptorImageInfo table[SNX_TEX_ARRAY];
    VkBufferView bv[8];
    VkBufferViewCreateInfo bvci[8]; /* what descriptor buffers write for bv (vk_descriptors_write) */
    u32 n;
    u32 cb_addr; /* the constant buffer slots heaps take by address (vk_cb_addr_mask), as the pipeline maps them */
} DescWrites;

typedef struct { /* up to eight images for each of the five graphics stages */
    Surf* surf[40];
    u8* original[40];
    u32 size[40];
    u64 after[40]; /* vk_gfx_image_after's entry, with original NULL */
    VkImageKeep keep[40]; /* kept on the GPU where keep.held is, with original NULL and after 0 */
    int n, sampled_general;
} DrawImages;
static void draw_images_discard(DrawImages* images) {
    for (int i = 0; i < images->n; i++) {
        free(images->original[i]);
        if (images->keep[i].held) vk_own_res_unref(images->keep[i].held);
    }
    images->n = 0;
}
static void draw_images_commit(DrawImages* images) {
    for (int i = 0; i < images->n; i++) {
        vk_surf_wrote(images->surf[i]);
        if (images->original[i] || images->after[i] || images->keep[i].held)
            vk_gfx_image_record(images->surf[i], images->original[i], images->size[i], images->after[i],
                                &images->keep[i]);
    }
    images->n = 0;
}
static void dw_buffer(DescWrites* d, VkDescriptorSet set, u32 binding, VkDescriptorType type, VkBuffer b, u64 off,
                      u64 range) {
    d->bi[binding] = (VkDescriptorBufferInfo){b, off, range};
    d->w[d->n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet = set,
                                          .dstBinding = binding,
                                          .descriptorCount = 1,
                                          .descriptorType = type,
                                          .pBufferInfo = &d->bi[binding]};
}

/* The buffer images' views of the frame, for its later draws, whose sets then match (desc_set_cached) */
#define BUFIMG_VIEWS 16
static struct {
    struct {
        VkBuffer buf;
        u64 off, frame;
        u32 bytes;
        VkFormat fmt;
        VkBufferView view;
    } kept[BUFIMG_VIEWS];
    u32 next;
} g_bufimg;
/* A draw's buffer image in slot i: a writable copy of its texels (vk_buf_copy_writable), which lands in guest memory
   as a storage buffer's result does, bound as a storage texel buffer. A header at tic_at that names no texture gets a
   texel of zeros from staging (vk_null_buffer_image), which lands nowhere */
static int dw_buffer_image(DescWrites* d, VkDescriptorSet set, int stage, int i, const u32* tic, u64 tic_at, int kind,
                           u32 handle) {
    VkBufImg bi;
    /* the slots of the set the program takes: a mesh program the vertex stage's, a task program the tessellation
       control stage's (vk_set_stages) */
    u32 slots = g_vk.buffer_image_slots[stage == DS_MESH ? ST_VS : stage == DS_TASK ? ST_TCS : stage];
    const int none = vk_tic_names_none(tic);
    if (none) vk_null_buffer_image(kind, &bi);
    if ((u32)i >= slots || (!none && !vk_buffer_image(tic, kind, &bi))) {
        LOG_LIMIT(4,
                  "vulkan: unsupported buffer image (draw skipped): handle %08x, header word 0 %08x, slot %d of the "
                  "%u this GPU binds\n",
                  handle, tic[0], i, slots);
        return 0;
    }
    VkBuffer b;
    u64 o, frame = vk_frame_id() + 1;
    if (none) {
        u8* zeros = vk_stage(bi.bytes, vk_stage_align(), &b, &o);
        if (!zeros) return 0;
        memset(zeros, 0, bi.bytes);
        if (vk_null_header_first(tic_at))
            LOG("vulkan: buffer image %d of stage %d (handle %08x) names %s at %llx (word 0 %08x): it reads zeros "
                "and its stores land nowhere, and the draw goes on\n",
                i, stage, handle, vk_tic_empty(tic) ? "an empty header" : "a header of no Vulkan format",
                (unsigned long long)tic_at, tic[0]);
    } else if (!vk_buf_copy_writable(bi.gva, bi.bytes, &b, &o, 1))
        return 0;
    d->bvci[i] = (VkBufferViewCreateInfo){.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                                          .buffer = b,
                                          .format = bi.fmt,
                                          .offset = o,
                                          .range = bi.bytes};
    int address = g_vk.has_descriptor_buffers || g_vk.has_descriptor_heaps;
    VkBufferView view = VK_NULL_HANDLE;
    if (!address) {
        for (u32 k = 0; k < BUFIMG_VIEWS && !view; k++)
            if (g_bufimg.kept[k].frame == frame && g_bufimg.kept[k].buf == b && g_bufimg.kept[k].off == o &&
                g_bufimg.kept[k].bytes == bi.bytes && g_bufimg.kept[k].fmt == bi.fmt)
                view = g_bufimg.kept[k].view;
        if (!view) {
            if (!VK_OK(vkCreateBufferView(g_vk.dev, &d->bvci[i], NULL, &view))) return 0;
            vk_defer_buffer_view(view); /* destroyed with the frame's sets */
            u32 k = g_bufimg.next++ % BUFIMG_VIEWS;
            g_bufimg.kept[k].buf = b;
            g_bufimg.kept[k].off = o;
            g_bufimg.kept[k].frame = frame;
            g_bufimg.kept[k].bytes = bi.bytes;
            g_bufimg.kept[k].fmt = bi.fmt;
            g_bufimg.kept[k].view = view;
        }
    }
    d->bv[i] = view; /* Address backends key/encode bvci directly; descriptor sets retain a real native view. */
    d->w[d->n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .pNext = address ? &d->bvci[i] : NULL,
                                          .dstSet = set,
                                          .dstBinding = SNX_VKB_IMGBUF + (u32)i,
                                          .descriptorCount = 1,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
                                          .pTexelBufferView = address ? NULL : &d->bv[i]};
    return 1;
}

/* shadows=0: a depth target a draw samples with a compare is a shadow map, and render_draw skips the passes into it.
   t is the target itself, or what a draw reads of it at its address: a copy, or a texture over it */
enum { SHADOW_MAP_FRAMES = 60 }; /* presents a target stays a shadow map after its last compare sample */
static void shadow_map_sampled(const Surf* t) {
    Surf* target = vk_rt_find(t->gva);
    if (!target || !target->f.depth) return;
    target->compared = 1;
    target->compared_at = g_vk_present;
}
static int shadow_map(const Surf* zt) { return zt->compared && g_vk_present - zt->compared_at <= SHADOW_MAP_FRAMES; }
/* Whether texture t's target is one a draw to rt and zt may fetch its own pixel of: of the size of the draw's first
   target, where a guest pixel of the one is a guest texel of the other and so are their images' pixels. A header on a
   part of a target (dynamic resolution) counts by the target's size. Any other (a table a few texels long, a level of
   another size) has no pixel of its own for the fragment */
static int own_size(const Surf* t, Surf** rt, const Surf* zt, u32 n) {
    const Surf* drawn = zt;
    for (u32 i = n; i-- > 0;)
        if (rt[i]) drawn = rt[i];
    const Surf* whole = t->is_rt ? vk_rt_find(t->gva) : NULL;
    if (!whole) whole = t;
    return drawn && whole->w == drawn->w && whole->h == drawn->h;
}
static int is_target(Surf* s, Surf** rt, Surf* zt, u32 n) {
    if (s == zt) return 1;
    for (u32 i = 0; i < n; i++)
        if (rt[i] == s) return 1;
    return 0;
}

/* An indexed texture kind (shader_same_indexed_kind): how its elements are viewed, and its null element, the view
   and sampler an element of the kind reads where it has no texture, with a shadow kind's depth view and compare
   sampler. Its id is the element, TEX_NO_ENTRY until a table of the kind needs it. */
enum { TEX_NO_ENTRY = 0xFFFFFFFFu };
typedef struct {
    VkImageViewType want;
    int num;
    VkImageView none;
    VkDescriptorImageInfo info, depth;
    u32 id;
} TexKind;

static int tex_kind_init(TexKind* tk, const ShaderInfo* si, int slot) {
    const ShTex* kind = &si->tex[slot];
    u32 null_tsc[8] = {0};
    ShTex plain = *kind;
    plain.shadow = 0;
    tk->want = vk_tex_view_type(kind);
    tk->num = kind->shadow || !((si->tex_int_mask >> slot) & 1) ? 0 : ((si->tex_sint_mask >> slot) & 1) ? 2 : 1;
    tk->none = vk_null_view(tk->want, tk->num, !!(kind->flags & 2));
    tk->id = TEX_NO_ENTRY;
    if (!tk->none) return 0;
    VkSampler null_sampler = vk_tex_bind_sampler(null_tsc, NULL, &plain, tk->num, NULL, tk->want, 0);
    if (!null_sampler) return 0;
    tk->info = (VkDescriptorImageInfo){null_sampler, tk->none, VK_LAYOUT(SHADER_READ_ONLY_OPTIMAL)};
    if (!kind->shadow) return 1;
    VkImageView farthest = vk_null_view(tk->want, 3, 0); /* not far: windef.h defines it away */
    VkSampler null_compare =
        farthest ? vk_tex_bind_sampler(null_tsc, NULL, kind, 0, NULL, tk->want, 0) : VK_NULL_HANDLE;
    if (!null_compare) return 0;
    tk->depth = (VkDescriptorImageInfo){null_compare, farthest, VK_LAYOUT(SHADER_READ_ONLY_OPTIMAL)};
    return 1;
}

/* A repeated indexed table keeps the immutable staging address it already uploaded in this frame. Its descriptor
   set can then match by content too. Frame staging survives submissions and ring growth until the slot is reused. */
static struct {
    u64 frame, offset;
    VkDevice device;
    VkBuffer buffer;
    u32 count, words[SH_INDEXED_TABLES * SNX_TEX_ARRAY];
} g_indexed_uniform[VK_DRAW_STAGES][2];
u64 g_vk_itable_uploads, g_vk_itable_reused, g_vk_itable_bytes;
void vk_render_upload_cache_reset(void) {
    for (u32 stage = 0; stage < VK_DRAW_STAGES; stage++)
        for (u32 kind = 0; kind < 2; kind++) g_indexed_uniform[stage][kind].frame = 0;
}
static int indexed_uniform(DescWrites* writes, VkDescriptorSet set, int stage, u32 binding,
                            const u32* words, u32 count) {
    u32 kind = binding == SNX_VKB_TEXMAP;
    if (stage < 0 || stage >= VK_DRAW_STAGES || !count || count > SH_INDEXED_TABLES * SNX_TEX_ARRAY) return 0;
    u64 frame = vk_frame_id() + 1;
    size_t bytes = (size_t)count * sizeof *words;
    if (g_indexed_uniform[stage][kind].frame != frame || g_indexed_uniform[stage][kind].device != g_vk.dev ||
        g_indexed_uniform[stage][kind].count != count || memcmp(g_indexed_uniform[stage][kind].words, words, bytes)) {
        VkBuffer buffer;
        u64 offset;
        u8* data = vk_stage(bytes, vk_stage_align(), &buffer, &offset);
        if (!data) return 0;
        memcpy(data, words, bytes);
        snx_le32_array(data, count);
        memcpy(g_indexed_uniform[stage][kind].words, words, bytes);
        g_indexed_uniform[stage][kind].buffer = buffer;
        g_indexed_uniform[stage][kind].offset = offset;
        g_indexed_uniform[stage][kind].count = count;
        g_indexed_uniform[stage][kind].device = g_vk.dev;
        g_indexed_uniform[stage][kind].frame = frame;
        g_vk_itable_uploads++;
        g_vk_itable_bytes += bytes;
    } else
        g_vk_itable_reused++;
    dw_buffer(writes, set, binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
              g_indexed_uniform[stage][kind].buffer, g_indexed_uniform[stage][kind].offset, bytes);
    return 1;
}

/* The elements of SNX_VKB_TEXARRAY (g_spirv_translate.tex_array of them), from an indexed texture slot: SNX_TEX_ARRAY
   handles from the slot's offset on, (1 << shift) bytes apart, or as many as the slot says, its stride apart. A handle
   past the constant buffer's size, or with a header or sampler index past its pool's limit (no texture on hardware
   either), reads as its kind's null texture. So does a texture of another kind than the slot's. Targets use snapshots
   refreshed after a draw changes them. Tables of different kinds share the binding, each element viewed as the kind of
   the table that names it: a handle that two kinds name has an element for each. */
static int tex_tables_set(const DsState* ds, int stage, const VkSh* s, const int* first, int count, Surf** rt, Surf* zt,
                          u32 n, VkImageLayout layout, DescWrites* d, VkDescriptorSet set) {
    /* each table's kind, named by the first table of that kind; a non-shadow kind's elements fill the depth binding
       with a shadow kind's null element */
    int kind_of[SH_INDEXED_TABLES] = {0};
    TexKind kinds[SH_INDEXED_TABLES];
    const TexKind* depth_fill = NULL;
    for (int table = 0; table < count; table++) {
        int k = 0;
        while (k < table &&
               !shader_same_indexed_kind(s->si.tex, s->si.tex_int_mask, s->si.tex_sint_mask, first[k], first[table]))
            k++;
        kind_of[table] = k;
        if (k < table) continue;
        if (!tex_kind_init(&kinds[table], &s->si, first[table])) return 0;
        if (!depth_fill && s->si.tex[first[table]].shadow) depth_fill = &kinds[table];
    }
    u32 handles[SNX_TEX_ARRAY] = {0};
    u8 element_kind[SNX_TEX_ARRAY] = {0}; /* the table that names the element's kind, 255 for a null element */
    u16 handle_slots[2 * SNX_TEX_ARRAY] = {0};
    u32 funcs[SNX_TEX_ARRAY] = {0};
    /* each element's entry: the null texture is given an entry only when an element needs it, so a table of
       SNX_TEX_ARRAY real handles fits (edenprobe's bindless suite samples 256) */
    u32 map[SH_INDEXED_TABLES * SNX_TEX_ARRAY]; /* every active table entry is assigned below */
    u32 used = 0;
    for (int table = 0; table < count; table++) {
        const ShTex* st = &s->si.tex[first[table]];
        TexKind* tk = &kinds[kind_of[table]];
        const ShTex* kind = &s->si.tex[first[kind_of[table]]];
        ShTex plain = *kind;
        plain.shadow = 0;
        u32 stride = st->stride ? st->stride : 1u << ((u32)st->flags >> 4), bank = st->bl_cbuf & 31;
        u32 elements = st->count ? st->count : SNX_TEX_ARRAY;
        DsCbBlock in = ds_cb_inline(ds, stage, bank);
        u64 cba = bank < 18 ? ds->bind.cb_addr[stage][bank] : 0;
        u32 size = in.bytes ? in.size : cba ? ds->bind.cb_size[stage][bank] : 0;
        if (!in.bytes && (size > 65536 || !size)) size = 65536;
        for (u32 k = 0; k < elements; k++) {
            u32 at = st->cbuf_off * 4 + k * stride;
            /* past the constant buffer: the null texture. A handle of 0 is header 0 with sampler 0, a texture like any
               other (edenprobe's bindless case samples it) */
            if ((!cba && !in.bytes) || at + 4 > size) {
                if (tk->id == TEX_NO_ENTRY) {
                    if (used == g_spirv_translate.tex_array) {
                        LOG_LIMIT(4, "vulkan: indexed texture tables exceed %u unique handles\n",
                                  g_spirv_translate.tex_array);
                        return 0;
                    }
                    tk->id = used++;
                    element_kind[tk->id] = 255;
                    d->table[tk->id] = tk->info;
                    if (depth_fill) {
                        d->depth_table[tk->id] = kind->shadow ? tk->depth : depth_fill->depth;
                        funcs[tk->id] = 11; /* LEQUAL and null depth */
                    }
                }
                map[table * SNX_TEX_ARRAY + k] = tk->id;
                continue;
            }
            u32 handle = in.bytes ? ds_inline_word(in, at) : gmem_r32(cba + at);
            /* Small tables keep their cheap linear hits. Beyond eight entries a slot index avoids the scan of all
               earlier handles; measured against 1-, 16- and 256-key synthetic tables. */
            u32 id = vk_indexed_handle_find(handle_slots, SNX_TEX_ARRAY, handles, element_kind, used, handle,
                                            (u8)kind_of[table], 8);
            if (id == used) {
                if (used == g_spirv_translate.tex_array) {
                    LOG_LIMIT(4, "vulkan: indexed texture tables exceed %u unique handles\n",
                              g_spirv_translate.tex_array);
                    return 0;
                }
                handles[id] = handle;
                element_kind[id] = (u8)kind_of[table];
                used++;
                TexRes x = {0};
                VkImageView view = VK_NULL_HANDLE;
                if ((handle & 0xFFFFF) <= ds->bind.tex_max && ((handle >> 20) & 0xFFF) <= ds->bind.smp_max) {
                    tex_resolve(ds, handle, &x);
                    x.t = capture_sample(x.t);
                    if (x.t && !!(kind->flags & 2) == !!x.t->msaa && (!kind->shadow || !x.t->f.is_int)) {
                        if (is_target(x.t, rt, zt, n)) {
                            /* An indexed handle needs the same pre-draw contents as a directly bound texture.
                               Snapshot each resolved target; rt_crop reuses a copy until its draw generation changes. */
                            Surf* snapshot = vk_rt_snapshot(x.t, 1);
                            if (snapshot != x.t) {
                                x.t = snapshot;
                                g_vk_rt_copies++;
                                view = vk_tex_bind_view(x.t, tk->want, x.tic, x.tsc, tk->num);
                            }
                        } else
                            view = vk_tex_bind_view(x.t, tk->want, x.tic, x.tsc, tk->num);
                    }
                }
                funcs[id] = x.have && (x.tsc[0] >> 9 & 1) ? (x.tsc[0] >> 10) & 7 : 3;
                int depth = kind->shadow && (!view || x.t->f.depth);
                if (depth) funcs[id] |= 8;
                if (view)
                    vk_surf_use(vk_cmd(), x.t, layout, VK_SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                else {
                    view = tk->none;
                    const u64 tic_at = ds->bind.tex_pool + (handle & 0xFFFFF) * 32ull;
                    if (x.have && !x.t && vk_tic_names_none(x.tic)) {
                        if (vk_null_header_first(tic_at))
                            LOG("vulkan: an indexed texture of stage %d (bank %u handle %08x) names %s at %llx (word 0 "
                                "%08x): it reads the null texture, %s, and the draw goes on\n",
                                stage, bank, handle,
                                vk_tic_empty(x.tic) ? "an empty header" : "a header of no Vulkan format",
                                (unsigned long long)tic_at, x.tic[0],
                                kind->shadow ? "a depth of 1.0 to compare" : "zeros");
                    } else if (handle) /* an unused entry holds 0: a handle that names nothing else is the black-texture
                                          signal */
                        LOG_LIMIT(4, "vulkan: indexed texture reads the null texture (bank %u handle %08x)\n", bank,
                                  handle);
                }
                VkImageView color_view = depth ? tk->none : view;
                VkSampler smp = vk_tex_bind_sampler(x.tsc, x.tic, &plain, tk->num, color_view != tk->none ? x.t : NULL,
                                                    tk->want, 0);
                if (!smp) return 0;
                d->table[id] = (VkDescriptorImageInfo){
                    smp, color_view, color_view == tk->none ? VK_LAYOUT(SHADER_READ_ONLY_OPTIMAL) : layout};
                if (kind->shadow) {
                    VkImageView depth_view = depth && view != tk->none ? view : tk->depth.imageView;
                    if (depth_view != tk->depth.imageView && !g_settings.shadows) shadow_map_sampled(x.t);
                    VkSampler compare = vk_tex_bind_sampler(
                        x.tsc, x.tic, kind, 0, depth_view != tk->depth.imageView ? x.t : NULL, tk->want, 0);
                    if (!compare) return 0;
                    d->depth_table[id] = (VkDescriptorImageInfo){
                        compare, depth_view,
                        depth_view == tk->depth.imageView ? VK_LAYOUT(SHADER_READ_ONLY_OPTIMAL) : layout};
                } else if (depth_fill)
                    d->depth_table[id] = depth_fill->depth;
            }
            map[table * SNX_TEX_ARRAY + k] = id;
        }
    }
    /* the first kind's null texture in every entry left: the binding is read whole */
    for (u32 k = used; k < g_spirv_translate.tex_array; k++) {
        d->table[k] = kinds[0].info;
        if (depth_fill) {
            d->depth_table[k] = depth_fill->depth;
            funcs[k] = 11; /* LEQUAL and null depth */
        }
    }
    VkWriteDescriptorSet* w = &d->w[d->n++];
    *w = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = set,
                                .dstBinding = SNX_VKB_TEXARRAY,
                                .descriptorCount = g_spirv_translate.tex_array,
                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .pImageInfo = d->table};
    if (depth_fill) {
        VkWriteDescriptorSet* depth_write = &d->w[d->n++];
        *depth_write = *w;
        depth_write->dstBinding = SNX_VKB_TEXDEPTH;
        depth_write->pImageInfo = d->depth_table;
        if (!indexed_uniform(d, set, stage, SNX_VKB_TEXFUNCS, funcs, SNX_TEX_ARRAY)) return 0;
    }
    if (count) {
        if (!indexed_uniform(d, set, stage, SNX_VKB_TEXMAP, map, (u32)count * SNX_TEX_ARRAY)) return 0;
    }
    return 1;
}

/* the words of global buffer desc's descriptor (SH_GBUF_*) into v: NVN's descriptors in cbuf 0 (the address and the
   size), or one of the program's own constant buffers (a bare address) */
static void gbuf_words(const DsState* ds, int stage, u32 desc, u32 v[3]) {
    DsCbBlock in = ds_cb_inline(ds, stage, SH_GBUF_BANK(desc));
    u64 cb = in.bytes ? 0 : ds->bind.cb_addr[stage][SH_GBUF_BANK(desc)];
    if (cb && SH_GBUF_BANK(desc)) vk_buf_mem_need(cb + SH_GBUF_OFFSET(desc), 8);
    if (cb) { /* words of a constant buffer, not a header: a pending write there is waited for */
        gmem_read(cb + SH_GBUF_OFFSET(desc), v, SH_GBUF_BANK(desc) ? 8 : 12);
        snx_le32_array(v, SH_GBUF_BANK(desc) ? 2 : 3);
    }
    for (u32 w = 0; in.bytes && w < (SH_GBUF_BANK(desc) ? 2u : 3u); w++)
        v[w] = ds_inline_word(in, SH_GBUF_OFFSET(desc) + 4 * w);
}
/* The windows of the pointer table in the storage buffer whose descriptor is parent (SH_GBUF_TABLE_SSBO), as many as
   the program has for it: the buffer's words, read from guest memory (its first 1 MiB, where a pending write is
   waited for), are the pointers, and each window binds ShaderInfo.table_reach bytes past those in it. Two runs are
   joined only where the memory between them is all mapped: a draw's window is copied as one range. One game's reset
   of its terrain's records reads 1485 of the 15000 pointers in its table, which only the first 4 KiB held 512 of */
#define TABLE_SSBO_MAX (1u << 20)
static void table_ssbo_windows(const DsState* ds, const VkSh* sh, int stage, u32 parent, u64* base, u32* len) {
    u32 windows = 0, v[3] = {0, 0, 0};
    for (int i = 0; i < sh->si.ngbuf && i < 16; i++)
        windows += SH_GBUF_TABLE_SSBO(sh->si.gbuf_cbuf_off[i]) && SH_GBUF_PARENT(sh->si.gbuf_cbuf_off[i]) == parent;
    if (windows > SH_GBUF_TABLE_MAX) windows = SH_GBUF_TABLE_MAX;
    u64 addr = 0;
    gbuf_words(ds, stage, parent, v);
    u32 bytes = vk_gbuf_desc(sh->stage, parent, v, &addr) & ~7u, n = 0;
    if (bytes > TABLE_SSBO_MAX) bytes = TABLE_SSBO_MAX;
    /* the words read into a buffer kept from draw to draw (draws hold vk_lock), through gmem_read, which takes the
       table's runs of guest memory wherever they are and reads what isn't mapped as zero */
    static u64* words;
    static u32 words_size;
    if (addr && bytes > words_size) {
        u64* grown = realloc(words, bytes);
        if (grown) words = grown, words_size = bytes;
    }
    if (addr && bytes && bytes <= words_size) {
        vk_buf_mem_need(addr, bytes);
        gmem_read(addr, words, bytes);
        for (n = 0; n < bytes / 8; n++) words[n] = snx_ld_le64(words + n);
    }
    u32 runs = vk_pointer_windows(words, n, windows, sh->si.table_reach, 1, base, len);
    if (runs)
        LOG_LIMIT(1,
                  "vulkan: a program's pointer table in memory has %u runs of mapped memory that its %u windows can't "
                  "hold: the pointers in the others reach nothing\n",
                  runs, windows);
}
/* the global memory each stage of the draw reads or writes, sh[] by stage (vertex, the tessellation stages, geometry,
   fragment) */
static void draw_storage_read(const DsState* ds, VkDrawStorage* draw, VkSh* const sh[VK_DRAW_STAGES]) {
    /* the counts, the maps and the fetch records here, each buffer as its stage counts it below: the buffers past a
       stage's count are never read. With draw_state_check=1 they hold a pattern, so a reader past a count shows */
    if (g_settings.draw_state_check) memset(draw, 0xA5, sizeof *draw);
    memset(draw, 0, offsetof(VkDrawStorage, b));
    for (int s = 0; s < VK_DRAW_STAGES; s++) {
        if (!sh[s]) continue;
        int stage = s;
        u64 cb0 = ds_cb_inline(ds, stage, 0).bytes ? 0 : ds->bind.cb_addr[stage][0];
        draw->n[s] = sh[s]->si.ngbuf < 16 ? sh[s]->si.ngbuf : 16;
        if (cb0 && draw->n[s]) vk_buf_mem_need(cb0, 0x610);
        u32 table = 0; /* the parent + 1 whose windows tw_ holds */
        u64 tw_base[SH_GBUF_TABLE_MAX];
        u32 tw_len[SH_GBUF_TABLE_MAX];
        for (int i = 0; i < draw->n[s]; i++) {
            VkDrawGbuf* b = &draw->b[s][i];
            *b = (VkDrawGbuf){0};
            u32 v[3] = {0, 0, 0}, desc = sh[s]->si.gbuf_cbuf_off[i];
            b->writable = (sh[s]->si.gbuf_wmask >> i) & 1;
            if (SH_GBUF_TABLE_SSBO(desc)) { /* a window of a pointer table in memory, its base in SNX_VKB_MBASE */
                if (table != SH_GBUF_PARENT(desc) + 1) {
                    table_ssbo_windows(ds, sh[s], stage, SH_GBUF_PARENT(desc), tw_base, tw_len);
                    table = SH_GBUF_PARENT(desc) + 1;
                }
                b->addr = tw_base[SH_GBUF_TABLE_WINDOW(desc)];
                b->size = (tw_len[SH_GBUF_TABLE_WINDOW(desc)] + 3) & ~3u;
                b->bare = 1;
                continue;
            }
            /* the constant buffer holding the address (SH_GBUF_*): NVN's descriptors in cbuf 0, or one of the
               program's own. An indirect buffer's is its parent's, which vk_gbuf_desc follows to the pointer, and it
               takes the memory mapped from there, as a bare address does */
            gbuf_words(ds, stage, desc, v);
            b->size = vk_gbuf_desc(sh[s]->stage, desc, v, &b->addr);
            b->bare = SH_GBUF_BANK(desc) != 0 || SH_GBUF_INDIRECT(desc);
        }
    }
}

/* a stage's descriptor set: constant buffers, storage buffers, textures. *zt_ro is set when it samples the depth
   target, which the pass then binds read-only. *fb gathers the color slots it samples in place, which the pass then
   binds in the feedback-loop layout (feedback_order): the fragment stage's, NULL for the others, which sample a copy.
   res: textures fs_key resolved for this stage, or NULL */
/* A stage's descriptor set by its contents: the same writes (buffers, offsets, ranges, views, samplers, layouts) in
   the same layout reuse the set made for them earlier in the frame, which still holds them (its pool is reset when the
   frame slot comes round again, and what it names is destroyed no earlier: vk_defer_*). One game allocated and wrote
   13,400 sets a frame, two a draw, much of it the same textures again. A 128-bit content hash stands for the
   contents */
u64 g_vk_dset_new, g_vk_dset_reused;
typedef struct {
    u64 h1, h2, frame;
    VkDescriptorSet set;
} DescSetEntry;
/* Each stage's last set and the key of its writes (vk_descriptor_key), in the frame and (with heaps) the descriptor
   heaps' epoch it was made in: a draw whose writes are the same, word for word, takes it with no hash and no lookup in
   the cache, as the draws of one material in a row do. A key longer than key holds isn't kept */
static struct {
    struct {
        u64 frame, heaps;
        size_t n;
        VkDescriptorSet set;
        u64 key[512];
    } last[2 * DS_GFX_STAGES]; /* with split sets, each stage's texture set past them (stage_set_) */
} g_dset;
static VkDescriptorSet desc_set_cached(VkDescriptorSetLayout layout, DescWrites* d, int stage) {
    /* Two ways share one cache line on 64-bit hosts. A collision need not discard a live descriptor set.
       Both ways remain scoped to the frame whose pools and resources keep the sets alive, and to the forgets of
       objects (vk_descriptor_heaps_forgets): a guest memory import destroyed in the middle of a frame (uma.c) let a
       new import of the same handle take the old one's set, whose descriptor named the unmapped memory, and lavapipe
       read it from the CPU and faulted */
    static DescSetEntry cache[1u << 14][2];
    /* Two indexed image arrays and otherwise at most one element per write. Serialize only initialized fields,
       then hash them with the SIMD dispatcher instead of two serial multiply chains for every descriptor. */
    u64 key[VK_DESCRIPTOR_KEY_WORDS(SNX_VKB_END, SNX_VKB_END + 2 * SNX_TEX_ARRAY) + 1];
    size_t words = vk_descriptor_key(key, layout, d->w, d->n);
    key[words++] = d->cb_addr; /* the same writes give a table of other words where other slots go by address */
    const u64 mode = g_vk.has_descriptor_heaps ? 2 : g_vk.has_descriptor_buffers ? 1 : 0;
    u64 frame = (((vk_frame_id() + 1) << 2) | mode) ^ vk_descriptor_heaps_forgets() * VK_FIB_MULT;
    u64 heaps = g_vk.has_descriptor_heaps ? vk_descriptor_heaps_epoch() : 0; /* it moves at each draw without heaps */
    int keep = (u32)stage < 2 * DS_GFX_STAGES && words <= sizeof g_dset.last[0].key / sizeof *key;
    if (keep && g_dset.last[stage].frame == frame && g_dset.last[stage].heaps == heaps &&
        g_dset.last[stage].n == words && !memcmp(g_dset.last[stage].key, key, words * sizeof *key)) {
        g_vk_dset_reused++;
        g_stats.n.sets_last++;
        return g_dset.last[stage].set;
    }
    u64 hash[2];
    vk_hash128(key, words * sizeof *key, hash);
    VkDescriptorSet set = VK_NULL_HANDLE;
    DescSetEntry* bucket = cache[hash[0] >> 50];
    for (u32 i = 0; i < 2 && !set; i++)
        if (bucket[i].frame == frame && bucket[i].h1 == hash[0] && bucket[i].h2 == hash[1]) {
            g_vk_dset_reused++;
            set = bucket[i].set;
        }
    if (!set) {
        DescSetEntry* e = bucket[0].frame != frame   ? &bucket[0]
                          : bucket[1].frame != frame ? &bucket[1]
                                                     : &bucket[hash[1] >> 63];
        set = (g_vk.has_descriptor_buffers || g_vk.has_descriptor_heaps) ? vk_descriptors_write(layout, d->n, d->w, d->cb_addr)
                                                                         : vk_rec_new_set(layout, d->n, d->w);
        if (!set) return VK_NULL_HANDLE;
        g_vk_dset_new++;
        *e = (DescSetEntry){hash[0], hash[1], frame, set};
    }
    if (keep) {
        g_dset.last[stage].frame = frame;
        g_dset.last[stage].heaps = heaps;
        g_dset.last[stage].n = words;
        g_dset.last[stage].set = set;
        memcpy(g_dset.last[stage].key, key, words * sizeof *key);
    }
    return set;
}
/* The fragment program's texture slots a draw samples at the own pixel's centre by where its vertex stage puts the
   generic they are sampled at (ShaderInfo.tex_screen_mask, tex_uv_mask), and the draw's viewport, which has to cover a
   slot's target for it (screen_covers) */
typedef struct {
    VkViewport vp;
    u64 slots;
} ScreenSlots;
static VkDescriptorSet stage_set_(const DsState* ds, int stage, VkSh* s, Surf** rt, Surf* zt, u32 n, int* zt_ro,
                                  u32* fb, u64* scaled, TexRes* res, const VkDrawStorage* draw, DrawImages* images,
                                  const ScreenSlots* screen, const XfbBind* xfb);
/* Whether the viewport covers the target t whole, so a fragment's position mapped onto [0, 1] is its pixel's centre in
   t: x from 0 over t's width, y from 0 down (a positive height) or up from its bottom (negative), as render_draw
   matched the sign to the vertex program's */
static int screen_covers(const VkViewport* vp, const Surf* t) {
    float w = (float)t->sw, h = (float)t->sh;
    return vp && vp->x == 0.0f && vp->width == w &&
           (vp->height > 0 ? vp->y == 0.0f && vp->height == h : vp->y == h && vp->height == -h);
}
/* The fragment program's samples at a generic's (x / w, y / w) read the own pixel's centre when the vertex program,
   the last stage (plain: no tessellation, geometry or swizzle), writes that generic as the position mapped onto
   [0, 1] with the y of the viewport's sign: stage_set_ checks the viewport covers the target. Its samples at a
   generic's (x, y) do when the vertex program writes its position from that generic, [0, 1] mapped onto the viewport
   with a w of 1, the y by the same sign. Samples at the x / w and y / w the fragment program maps onto [0, 1] itself
   (screen_ndc) do when the vertex program writes the generic as the position itself, its y mapped the same way:
   (w - y) / 2 and -y / w / 2 + 1/2 are one value. Not under a swizzle the program applies, which moves the position
   and not the generic */
static ScreenSlots screen_slots(const DsState* ds, const VkSh* fs, const VkSh* vs, int plain) {
    ScreenSlots screen = {0};
    if (!fs || !vs || !plain || vs->si.writes_viewport || !ds->vp.transform_on) return screen;
    u32 ndc = fs->si.screen_ndc;
    int by_w = fs->si.tex_screen_mask && ((ndc ? vs->si.pos_out_mask : vs->si.screen_out_mask) >> fs->si.screen_in & 1);
    int by_uv = fs->si.tex_uv_mask && (vs->si.uv_out_mask >> fs->si.uv_in & 1);
    int yneg = ndc ? ndc == 2 : (int)(vs->si.screen_out_yneg_mask >> fs->si.screen_in & 1);
    if (by_w || by_uv) screen.vp = vk_viewport_from(&ds->dyn, &ds->vp, 0);
    if (by_w && (yneg ? screen.vp.height < 0 : screen.vp.height > 0)) screen.slots |= fs->si.tex_screen_mask;
    if (by_uv && ((vs->si.uv_out_yneg_mask >> fs->si.uv_in & 1) ? screen.vp.height < 0 : screen.vp.height > 0))
        screen.slots |= fs->si.tex_uv_mask;
    return screen;
}
/* The fragment program's slots that read one of the draw's color targets at the own pixel, each fragment taking what
   the pixel held before the draw (ShaderInfo.predraw_slots, vk/predraw.c). Only a target the draw also writes: one it
   only reads holds what was there before the draw at every fragment, and needs no capture. 0 where the device has no
   fragment interlock or the program no room for the images: those slots read a copy of the target (stage_set_).
   fres: the slots' textures (fs_key), screen: the slots sampled at the own pixel's centre, which the viewport has to
   cover */
static u64 predraw_choose(const DsState* ds, const VkSh* fs0, const TexRes* fres, Surf* const* rt, u32 n,
                          const ScreenSlots* screen) {
    if (!fs0 || g_vk.feedback_mode != VK_FEEDBACK_INTERLOCK) return 0; /* NVIDIA's interlock (device.c) */
    u64 slots = 0;
    int nt = fs0->si.ntex > SH_MAXTEX ? SH_MAXTEX : fs0->si.ntex;
    for (int i = 0; i < nt; i++) {
        const Surf* t = fres[i].have ? fres[i].t : NULL;
        if (!t || !(fs0->si.tex_predraw_mask >> i & 1) || t->msaa || t->layers != 1 ||
            t->layout == VK_IMAGE_LAYOUT_UNDEFINED)
            continue;
        if (!(fs0->si.tex_own_pixel_mask >> i & 1) && !((screen->slots >> i & 1) && screen_covers(&screen->vp, t)))
            continue;
        for (u32 j = 0; j < n; j++)
            if (rt[j] == t && (ds->blend.mask[j] & ((fs0->si.sph[18] >> (j * 4)) & 15))) slots |= 1ull << i;
    }
    u32 count = snx_popcount64(slots);
    if (count > SH_PREDRAW_MAX || (u32)fs0->si.nimg + 1 + count > g_vk.image_slots[ST_FS]) return 0;
    return slots;
}
/* scaled: a bit for each texture slot bound to a scaled render target (shader_scaled_fetch in shader/emit_texture.c),
   and in scaled[1] the ones of those whose target has the size of the draw's (own_size) */
/* screen: the slots sampled at the own pixel's centre where the draw's viewport covers their target (render_draw), NULL
   for none.
   xfb: the vertex stage's transform feedback stores (xfb_bind), NULL for none */
static VkDescriptorSet stage_set(const DsState* ds, int stage, VkSh* s, Surf** rt, Surf* zt, u32 n, int* zt_ro, u32* fb,
                                 u64* scaled, TexRes* res, const VkDrawStorage* draw, DrawImages* images,
                                 const ScreenSlots* screen, const XfbBind* xfb) {
    SNX_ZONE(z, "stage set");
    VkDescriptorSet set = stage_set_(ds, stage, s, rt, zt, n, zt_ro, fb, scaled, res, draw, images, screen, xfb);
    SNX_ZONE_END(z);
    return set;
}
/* Constant buffers through windows of the staging ring (ShCaps.cb_windows): a vertex or fragment program's uniform
   bank binds up to 64 KiB of the ring chunk its copy is in, from an aligned base at or before it, and the program adds
   the copy's offset in that window (16-byte units) from push constants (SH_PUSH_CB_WINDOW). Each bank keeps its window
   while its copies land in it: a draw that writes new constants, about a copy a draw in one game, binds the same
   descriptor and keeps its set, where each new copy's offset made a new set. RPCS3's constant ring windows, the idea
   only. A copy that is in no chunk, or past where a window from it can reach, binds alone with offset 0. A window
   stays within its frame (frame, vk_frame_id() + 1): a chunk the frame copies into lives through it, and one destroyed
   later may give its handle to a smaller one */
typedef struct {
    VkBuffer b;
    u64 base, range, frame;
} CbWindow;
/* bank 18: the texture words of a program that reads the texture table (ShCaps.tex_table, table_words) */
#define CB_WINDOW_WORDS 18
static CbWindow g_cb_windows[2][19];
static u16 g_cb_offsets[2][19]; /* the draw's offsets, by the stage's set (0 vertex, 1 fragment) and bank */
static int cb_window_place(int vs_fs, int bank, VkBuffer b, u64 o, u32 sz, u64* base, u64* range) {
    CbWindow* w = &g_cb_windows[vs_fs][bank];
    const u64 frame = vk_frame_id() + 1;
    if (w->b != b || w->frame != frame || o < w->base || o + sz > w->base + w->range) {
        /* the ring's chunks, and the GPU mirror, which is as big as guest memory (cb_get binds banks in it) */
        const u64 size = b == vk_mirror_buffer() ? (u64)AS_SIZE : vk_stage_buffer_size(b),
                  align = g_vk.props.limits.minUniformBufferOffsetAlignment;
        const u64 at = align ? o / align * align : o;
        if (!size || at >= size || (o - at) & 15) return 0;
        const u64 reach = size - at < 65536 ? size - at : 65536;
        if (o + sz > at + reach) return 0;
        *w = (CbWindow){b, at, reach, frame};
    }
    *base = w->base;
    *range = w->range;
    return 1;
}
/* A program of the texture table (ShaderInfo.tex_table): its words, one for each plain texture slot, in a window of
   the staging ring at SNX_VKB_TEXWORDS (bank CB_WINDOW_WORDS of cb_window_place), the window's offset pushed at
   SH_PUSH_TEX_WORDS. The same words as the stage's last draw in this frame take the same bytes: the set and the push
   stay */
static struct {
    u32 words[SH_RECORD_TEX_FETCH + SH_MAXTEX], n;
    u64 frame;
    VkBuffer b;
    u64 o;
} g_table_words[2];
static int table_words(DescWrites* d, VkDescriptorSet set, int vs_fs, const u32* words, u32 n) {
    const u32 bytes = (n * 4 + 15) & ~15u;
    const u64 frame = vk_frame_id() + 1;
    VkBuffer b;
    u64 o, base, range;
    if (g_table_words[vs_fs].frame == frame && g_table_words[vs_fs].n == n &&
        !memcmp(g_table_words[vs_fs].words, words, n * 4)) {
        b = g_table_words[vs_fs].b;
        o = g_table_words[vs_fs].o;
    } else {
        u8* data = vk_stage(bytes, 16, &b, &o);
        if (!data) return 0;
        memset(data, 0, bytes);
        memcpy(data, words, n * 4);
        snx_le32_array(data, n);
        g_table_words[vs_fs].frame = frame;
        g_table_words[vs_fs].n = n;
        memcpy(g_table_words[vs_fs].words, words, n * 4);
        g_table_words[vs_fs].b = b;
        g_table_words[vs_fs].o = o;
    }
    if (cb_window_place(vs_fs, CB_WINDOW_WORDS, b, o, bytes, &base, &range)) {
        g_cb_offsets[vs_fs][CB_WINDOW_WORDS] = (u16)((o - base) >> 4);
        dw_buffer(d, set, SNX_VKB_TEXWORDS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b, base, (u32)range);
        return 1;
    }
    /* no window from it: bound alone, at the descriptor's alignment */
    if (o % vk_stage_align()) {
        u8* data = vk_stage(bytes, vk_stage_align(), &b, &o);
        if (!data) return 0;
        memset(data, 0, bytes);
        memcpy(data, words, n * 4);
        snx_le32_array(data, n);
        g_table_words[vs_fs].frame = 0;
    }
    g_cb_offsets[vs_fs][CB_WINDOW_WORDS] = 0;
    dw_buffer(d, set, SNX_VKB_TEXWORDS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b, o, bytes);
    return 1;
}
/* A draw's read-only storage buffer as the last lookup of its slot found it (vk_buf_get_storage), while nothing that
   lookup's checks read moved: in the same submission, no GPU-side write anywhere (gw), no write tracking epoch, no
   result added, no storage image, no surface and no write of the GPU mirror's programs since. The lookup would bind
   the same buffer and offset then. It found the guest run, checked the mirror's and the page cache's state and walked
   the granules of a range of about 6.5 MB, about 120 to 240 ns, once or twice a draw in one game. Kept only where the
   lookup bound the mirror, the page cache or a copy of the cache's own (vk_buf_kind): a pending graphics or compute
   result orders each read after its writer (a barrier, and the read noted for a later writer), and guest memory in
   place notes each read, which a hit would skip. A write in place through the page cache (vk_pages_written) moves
   nothing else */
typedef struct {
    u64 addr, align, sub, gw, adds, cs_adds, gfx_adds, img_adds, mirror_gen, pages_written;
    u32 size, epoch, surf_epoch;
    VkBuffer b;
    u64 o;
} StorageRecent;
static StorageRecent g_storage_recent[DS_GFX_STAGES][16];
static int storage_recent_same(const StorageRecent* a, const StorageRecent* b) {
    return a->addr == b->addr && a->size == b->size && a->align == b->align && a->sub == b->sub && a->gw == b->gw &&
           a->adds == b->adds && a->cs_adds == b->cs_adds && a->gfx_adds == b->gfx_adds && a->img_adds == b->img_adds &&
           a->mirror_gen == b->mirror_gen && a->pages_written == b->pages_written && a->epoch == b->epoch &&
           a->surf_epoch == b->surf_epoch;
}
/* with split sets (ShCaps.split_sets): the texture set stage_set_ made last for each stage's buffer set number (0 to 4) */
static VkDescriptorSet g_stage_tex_set[5];
/* an indirect mesh draw's first group, which mesh_indirect copied for the draw: the uniform buffer stage_set binds */
static VkBuffer g_mesh_first;
static u64 g_mesh_first_off;
static void index_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                          VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);
/* The compute-task path's buffer for the draws of a frame (sm86.h): the record, the counts and the task programs'
   payloads, one buffer for each frame in flight and one for all of its draws, as the buffer's slots are cut to what a
   draw needs. g_mesh_payload is the frame's */
static VkBuffer g_mesh_payload_slot[VK_FRAMES];
static VkBuffer g_mesh_payload;
static u64 g_mesh_payload_bytes, g_mesh_groups, g_mesh_cap;
/* make the frame's buffer, once (it holds the most slots any task program's payload takes) */
static VkBuffer mesh_payload(void) {
    const u32 slot = vk_frame_slot();
    if (!g_mesh_payload_slot[slot]) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = SNX_MESH_PAYLOAD_BYTES,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        DevMemWant ai = {.at = DM_DEVICE};
        DevMem mem;
        if (!VK_OK(vk_buffer_create(&bi, &ai, &g_mesh_payload_slot[slot], &mem, NULL))) return VK_NULL_HANDLE;
        vk_mem_track(mem, VK_OWN_ENCODER);
    }
    return g_mesh_payload_slot[slot];
}
/* the compute pipelines of the path's task programs: a program's compute module, in the layout a draw's sets bind
   (vk_playout of its arrays), which a pipeline keeps the sample of while a program is drawn */
typedef struct {
    const VkSh* sh;
    u32 arrays;
    VkPipeline pipe;
} MeshTaskPipe;
static MeshTaskPipe g_mesh_task_pipes[64];
static u32 g_mesh_task_pipe_n;
static VkPipeline mesh_task_pipe(const VkSh* sh, u32 arrays) {
    for (u32 i = 0; i < g_mesh_task_pipe_n; i++)
        if (g_mesh_task_pipes[i].sh == sh && g_mesh_task_pipes[i].arrays == arrays) return g_mesh_task_pipes[i].pipe;
    if (!sh || !sh->spv || g_mesh_task_pipe_n == sizeof g_mesh_task_pipes / sizeof g_mesh_task_pipes[0]) {
        LOG_LIMIT(4, "vulkan: a task program's compute module: %s\n",
                  !sh ? "no program" : !sh->spv ? "no SPIR-V kept" : "too many task programs");
        return VK_NULL_HANDLE;
    }
    VkPipeline p = VK_NULL_HANDLE;
    /* with descriptor buffers the pipeline reads them, and says so (VUID-vkCmdDispatch-None-08114). The task program's
       lanes are a warp's (sm86.h): a subgroup of 32, whole where the workgroup is */
    const VkPipelineCreateFlags flags =
        g_vk.has_descriptor_buffers ? VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0;
    VkResult r = snx_vk_compute_pipeline_flags(g_vk.dev, vk_pipeline_cache(), vk_playout_compute(arrays), sh->spv,
                                               (size_t)sh->nspv * 4, 32, (sh->si.mesh.threads % 32) == 0, flags, &p);
    if (r != VK_SUCCESS) {
        LOG_LIMIT(4, "vulkan: a task program's compute pipeline failed: VkResult %d\n", (int)r);
        return VK_NULL_HANDLE;
    }
    g_mesh_task_pipes[g_mesh_task_pipe_n++] = (MeshTaskPipe){sh, arrays, p};
    return p;
}
/* The compute-task path's task program, dispatched before its mesh draw: the record at the buffer's first four words
   zeroed, the dispatch (groups task workgroups, the draw's first among them), and the barrier that makes the payloads,
   the counts and the record the mesh draw's. Recorded outside a render pass: a dispatch cannot be in one */
static int mesh_task_dispatch(const VkSh* task, u32 arrays, VkDescriptorSet set, int indirect, VkBuffer abuf, u64 aoff,
                              u32 groups) {
    VkPipeline pipe = mesh_task_pipe(task, arrays);
    if (!pipe || !set) {
        LOG_LIMIT(4, "vulkan: a mesh task dispatch: pipeline %d, set %d, %u task programs\n", pipe != VK_NULL_HANDLE,
                  set != VK_NULL_HANDLE, g_mesh_task_pipe_n);
        return 0;
    }
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    /* The buffer is the frame's: the draw before this one read it, and a dispatch of a draw that ended between them
       (its pipeline could not be made) still writes it. Both go before the record's fill */
    index_barrier(cb,
                  VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                      VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    rec_CmdFillBuffer(cb, g_mesh_payload, 0, 16, 0);
    /* the fill, and an indirect draw's record that mesh_indirect copied (its own barrier is the draw's), reach the
       dispatch: its indirect count from the copy, the record and the buffer from the fill */
    index_barrier(cb, VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                      VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vk_descriptors_bind_compute(cb, vk_playout_compute(arrays), (u32)shader_vk_set(ST_TS), set);
    /* the vec4 at 0: the flag that makes the first workgroup the uniform buffer's (sm86.h's workgroup_index), and the
       slots the buffer holds above it */
    u32 pc[4] = {0, 0, SH_PUSH_GPU_MESH_FIRST | (u32)g_mesh_cap << 8, 0};
    rec_CmdPushConstants(cb, vk_playout_compute(arrays), VK_PUSH_STAGES, 0, sizeof pc, pc);
    if (indirect)
        rec_CmdDispatchIndirect(cb, abuf, aoff);
    else
        rec_CmdDispatch(cb, groups, 1, 1);
    index_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT |
                      VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    return 1;
}
/* whether a stage of the draw that binds its global memory by descriptor (tessellation, geometry, task, mesh) writes
   a buffer: the vertex and fragment stages' writable buffers then take the descriptor's alignment too, as repeated
   bindings of one writable range in a draw name one storage (vk_buf_copy_writable): read by address at any word, an
   earlier result could be taken in place where the descriptor's binding composes a copy, and the two land over each
   other */
static int draw_writes_by_descriptor(const VkDrawStorage* draw) {
    for (int st = 0; st < VK_DRAW_STAGES; st++) {
        if (st == ST_VS || st == ST_FS) continue;
        for (int i = 0; i < draw->n[st]; i++)
            if (draw->b[st][i].writable) return 1;
    }
    return 0;
}
static VkDescriptorSet stage_set_(const DsState* ds, int stage, VkSh* s, Surf** rt, Surf* zt, u32 n, int* zt_ro,
                                  u32* fb, u64* scaled, TexRes* res, const VkDrawStorage* draw, DrawImages* images,
                                  const ScreenSlots* screen, const XfbBind* xfb) {
    VkDescriptorSetLayout layout = g_vk_dslayout[s->indexed][shader_vk_set(s->stage)];
    VkDescriptorSet set = VK_NULL_HANDLE; /* the writes take it once their contents are known (desc_set_cached) */
    DescWrites d;
    d.n = 0;
    d.cb_addr = vk_cb_addr_mask(&s->si);
    /* the profiler's zones of a stage's set: its constant buffers, storage buffers, textures and the set itself */
    SNX_ZONE(zcb, "stage set constants");
    for (int i = 0; i < 18; i++) {
        if (!(s->si.cbuf_mask & (1u << i))) continue;
        u32 sz = cb_range(ds, stage, s, i);
        VkBuffer b;
        u64 o;
        u64 base, range;
        const int vs_fs = shader_vk_set(s->stage);
        /* Only where the guest's bound size covers every byte the program can read of the bank (si.cbuf_max, 0 for
           any): a window reads on past the bank, into the ring's next bytes, where a descriptor of the bank's own
           size reads 0 (robustness), as the console does past a bank's size. A window's offset and size then fit its
           64 KiB, so the program's index stays in its 4096-element array (cb_window_place) */
        const u32 guest_size = ds->bind.cb_size[stage][i] && ds->bind.cb_size[stage][i] <= 65536
                                   ? ds->bind.cb_size[stage][i]
                                   : 65536,
                  reads = s->si.cbuf_max[i] ? s->si.cbuf_max[i] : 65536;
        const int windowed =
            s->si.cb_window && vs_fs < 2 && (u32)i < g_spirv_translate.uniform_cbufs && guest_size >= reads;
        /* a windowed bank takes its bytes where the GPU mirror has them at any 16 bytes, which a descriptor of its
           own can't: the mirror copied them out instead, and ended the rendering pass for it (most of one game's
           pass ends). One whose window can't be placed takes them again at the descriptor's alignment */
        if (!cb_get(ds, stage, i, sz, windowed ? 16 : vk_stage_align(), &b, &o)) return VK_NULL_HANDLE;
        const int placed = windowed && cb_window_place(vs_fs, i, b, o, sz, &base, &range);
        if (!placed && o % vk_stage_align() && !cb_get(ds, stage, i, sz, vk_stage_align(), &b, &o))
            return VK_NULL_HANDLE;
        if (placed) {
            g_cb_offsets[vs_fs][i] = (u16)((o - base) >> 4);
            dw_buffer(&d, set, SNX_VKB_CB + i, vk_cbuf_type((u32)i), b, base, (u32)range);
        } else {
            if (vs_fs < 2) g_cb_offsets[vs_fs][i] = 0;
            /* by address (with heaps, which take no windows), every byte the shader reads is in the binding: past a
               short one, zeros */
            if ((d.cb_addr >> i & 1) && sz < vk_cb_range(s->si.cbuf_max[i]) &&
                !cb_pad(&b, &o, sz, vk_cb_range(s->si.cbuf_max[i])))
                return VK_NULL_HANDLE;
            dw_buffer(&d, set, SNX_VKB_CB + i, vk_cbuf_type((u32)i), b, o, sz);
        }
    }
    if (s->si.root_max) {
        u32 sz = (s->si.root_max + 15u) & ~15u;
        VkBuffer b;
        u64 o;
        if (!root_get(ds, sz, &b, &o)) return VK_NULL_HANDLE;
        dw_buffer(&d, set, SNX_VKB_ROOT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b, o, sz);
    }
    /* Two draws' stores to one buffer are not ordered on the console: a game orders them with a barrier
       (WAIT_FOR_IDLE, FRAGMENT_BARRIER and the texture data cache invalidation of Maxwell, NVN's barriers), and
       method_barrier gives each of those. A read of an earlier draw's writes still gets its barrier here. One game
       draws about 600 times a frame into one result buffer, and each of those draws ended the pass for a barrier
       before it. nvn_storage_order 0 keeps that order for comparisons */
    SNX_ZONE_END(zcb);
    SNX_ZONE(zsb, "stage set storage");
    int ordered = !g_settings.nvn_storage_ordering;
    /* The stage's record in its words' window (table_words): with gbuf_address, each global buffer slot's device
       address and length and the scratch words' address (SH_RECORD_GBUF), then the texture words. A program of
       gbuf_address takes its slots' addresses there, at any offset, where a descriptor was bound. A vertex program
       that fetches its own attributes has their words after the global buffers' (SH_RECORD_ATTR, fetch_records), and
       its texture words after those */
    const int recorded = g_spirv_translate.gbuf_address && shader_vk_set(s->stage) < 2;
    const int addressed = recorded && s->si.gbuf_address;
    const int fetch = recorded && s->si.vertex_fetch && s->stage == ST_VS;
    const u32 tex_at = fetch ? SH_RECORD_TEX_FETCH : recorded ? SH_RECORD_GBUF : 0;
    u32 rec[SH_RECORD_TEX_FETCH + SH_MAXTEX], rec_n = 0;
    if (addressed || fetch) {
        memset(rec, 0, SH_RECORD_GBUF * sizeof *rec);
        const u64 scratch = vk_table_scratch();
        rec[SH_RECORD_SCRATCH] = (u32)scratch;
        rec[SH_RECORD_SCRATCH + 1] = (u32)(scratch >> 32);
        if (fetch) {
            memcpy(rec + SH_RECORD_ATTR, draw->attr, sizeof draw->attr);
            memcpy(rec + SH_RECORD_DIVISOR, draw->divisor, sizeof draw->divisor);
        }
        rec_n = tex_at;
    }
    for (int i = 0; i < s->si.ngbuf && i < 16; i++) {
        const VkDrawGbuf* gb = &draw->b[stage][i];
        u64 addr = gb->addr;
        u32 size = gb->size;
        /* an earlier draw's result is read in place. By address a slot takes any word: a range in the GPU mirror at an
           offset no descriptor binds is bound in place, where it was copied out and ended the rendering pass */
        u64 sa = addressed ? 4 : g_vk.props.limits.minStorageBufferOffsetAlignment;
        VkBuffer b;
        u64 o;
        /* no address: a null buffer where the device has them, which reads zero, else a staging copy of zeros */
        /* the in-place storage path, as its last lookup found it while nothing it asks about moved */
        StorageRecent* sr = !gb->aliased && addr && !gb->writable && !gb->bare && (u32)stage < DS_GFX_STAGES &&
                                    !g_settings.draw_state_check
                                ? &g_storage_recent[stage][i]
                                : NULL;
        StorageRecent now = {0};
        int hit = 0;
        if (sr) {
            now = (StorageRecent){.addr = addr,
                                  .align = sa,
                                  .sub = vk_submit_number(),
                                  .gw = gpu_mem_generation(),
                                  .adds = cb_adds(),
                                  .cs_adds = vk_compute_adds(),
                                  .gfx_adds = vk_buf_adds(),
                                  .img_adds = vk_gfx_image_adds(),
                                  .mirror_gen = vk_mirror_write_gen(),
                                  .pages_written = vk_pages_written(),
                                  .size = size,
                                  .epoch = vk_ww_epoch(),
                                  .surf_epoch = atomic_load_explicit(&g_vk_surf_epoch, memory_order_relaxed)};
            hit = sr->b && storage_recent_same(sr, &now);
        }
        if (hit) {
            b = sr->b;
            o = sr->o;
            vk_pages_read(addr, size); /* the page cache's order of this read before a later write */
        }
        int ok = hit           ? 1
                 : gb->aliased ? (b = gb->buf, o = gb->off, 1)
                 : addr && gb->writable && addressed && !draw_writes_by_descriptor(draw)
                     ? vk_buf_copy_writable_word(addr, size, &b, &o, ordered)
                 : addr && gb->writable  ? vk_buf_copy_writable(addr, size, &b, &o, ordered)
                 : addr && gb->bare      ? vk_buf_get_window(addr, size, addressed ? 4 : 0, &b, &o)
                 : addr                  ? vk_buf_get_storage(addr, size, sa, &b, &o)
                 : vk_null_descriptors() ? (b = VK_NULL_HANDLE, o = 0, 1)
                                         : vk_buf_copy(0, size, &b, &o);
        if (!ok) return VK_NULL_HANDLE;
        if (sr && !hit) {
            /* the stamps read before the lookup: a write during it moves them, and the next looks again. A binding
               that orders or notes its reads isn't kept (above) */
            const u32 kind = vk_buf_kind();
            *sr = now;
            sr->b = kind == 1 || kind == 2 || kind == 32 || kind == 64 ? b : VK_NULL_HANDLE;
            sr->o = o;
        }
        u64 range = !b ? VK_WHOLE_SIZE : gb->aliased ? gb->range : size;
        if (addressed) { /* no buffer: length 0, each access takes the scratch words */
            const u64 at = b ? vk_buffer_address(b) + o : 0;
            rec[4 * i] = (u32)at;
            rec[4 * i + 1] = (u32)(at >> 32);
            rec[4 * i + 2] = b ? (u32)range : 0;
            continue;
        }
        dw_buffer(&d, set, SNX_VKB_SSBO + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, b, o, range);
    }
    SNX_ZONE_END(zsb);
    /* each pointer table window's base and length, at its buffer's slot: 16 vec4 of words (SNX_VKB_MBASE, which is
       the fragment stage's SNX_VKB_RS: the translator makes no table for a fragment program) */
    u32 mb[64] = {0}, tables = 0;
    for (int i = 0; i < s->si.ngbuf && i < 16; i++)
        if (SH_GBUF_TABLE(s->si.gbuf_cbuf_off[i])) {
            mb[4 * i] = (u32)draw->b[stage][i].addr;
            mb[4 * i + 1] = (u32)(draw->b[stage][i].addr >> 32);
            mb[4 * i + 2] = draw->b[stage][i].size;
            tables = 1;
        }
    if (tables) {
        VkBuffer mbase;
        u64 mo;
        u8* p = vk_stage(sizeof mb, vk_stage_align(), &mbase, &mo);
        if (!p) return VK_NULL_HANDLE;
        memcpy(p, mb, sizeof mb);
        snx_le32_array(p, sizeof mb / 4); /* the shader reads the words little-endian */
        dw_buffer(&d, set, SNX_VKB_MBASE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, mbase, mo, sizeof mb);
    }
    if (s->si.ngbuf)
        dw_buffer(&d, set, SNX_VKB_SMAP, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, draw->map_buf[stage], draw->map_off[stage],
                  draw->map_size[stage]);
    for (u32 b = 0; xfb && xfb->range.declared && b < 5; b++)
        if (b == 4 || xfb->range.declared >> b & 1)
            dw_buffer(&d, set, b == 4 ? SNX_VKB_XFB_PARAMS : SNX_VKB_XFB + b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                      xfb->buf[b], xfb->off[b], xfb->size[b]);
    VkBuffer cb;
    u64 co;
    if (stage == 4 && frame_consts(&cb, &co))
        dw_buffer(&d, set, SNX_VKB_RS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cb, co, 16);
    /* a task or mesh program's first group of an indirect draw (mesh_indirect), any 16 bytes for a direct one */
    if ((stage == DS_TASK || stage == DS_MESH) && (g_mesh_first || frame_consts(&cb, &co)))
        dw_buffer(&d, set, SNX_VKB_RS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, g_mesh_first ? g_mesh_first : cb,
                  g_mesh_first ? g_mesh_first_off : co, 16);
    /* the compute-task path's buffer, which the task program writes and the mesh program reads (sm86.h) */
    if (g_mesh_payload && s->si.mesh.payload_buf)
        dw_buffer(&d, set, SNX_VKB_MESHBUF, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, g_mesh_payload, 0, g_mesh_payload_bytes);
    if (stage == ST_FS && (s->si.advanced_blend_mask & SH_CAPTURE_TILED)) {
        if (!g_capture_draw.images || !g_capture_draw.images->tile_buffer) return VK_NULL_HANDLE;
        dw_buffer(&d, set, SNX_VKB_CAPTURE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    g_capture_draw.images->tile_buffer, 0, g_capture_draw.images->tile_bytes);
    }
    if (stage == ST_FS && s->si.late_depth) {
        const VkDescriptorBufferInfo* shadow = &g_capture_draw.shadow;
        if (!shadow->buffer || !shadow->range) return VK_NULL_HANDLE;
        dw_buffer(&d, set, SNX_VKB_CAPTURE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    shadow->buffer, shadow->offset, shadow->range);
    }
    if (stage == ST_FS && s->si.advanced_blend) {
        if (!n || !rt[0]) return VK_NULL_HANDLE;
        VkImageView view = vk_tex_storage_view(
            rt[0], s->si.advanced_blend_mask & SH_BLEND_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        if (!view) return VK_NULL_HANDLE;
        u32 binding = SNX_VKB_BLEND + (u32)s->si.nimg;
        d.ii[binding] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        d.w[d.n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = set,
                                            .dstBinding = binding,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                            .pImageInfo = &d.ii[binding]};
    }
    /* the pre-draw capture's stamp and side images, after the program's own images (vk/predraw.c) */
    for (u32 k = 0; stage == ST_FS && s->si.predraw_slots && k <= snx_popcount64(s->si.predraw_slots); k++) {
        VkImageView view = vk_predraw_view(k);
        if (!view) return VK_NULL_HANDLE;
        u32 binding = SNX_VKB_IMG + (u32)s->si.nimg + k;
        d.ii[binding] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        d.w[d.n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = set,
                                            .dstBinding = binding,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                            .pImageInfo = &d.ii[binding]};
    }
    SNX_ZONE(ztex, "stage set textures");
    u32 tcb = ds->bind.tex_bank;
    int nt = s->si.ntex > SH_MAXTEX ? SH_MAXTEX : s->si.ntex;
    int table_first[SH_INDEXED_TABLES] = {0}, table_count = 0;
    for (int i = 0; i < nt; i++) {
        ShTex* st = &s->si.tex[i];
        if (st->flags & SNX_TEX_INDEXED) {
            int table = 0;
            while (table < table_count && !shader_same_indexed_table(st, &s->si.tex[table_first[table]])) table++;
            if (table == table_count) {
                if (table_count == SH_INDEXED_TABLES) return VK_NULL_HANDLE;
                table_first[table_count++] = i;
            }
        }
    }
    if (table_count &&
        !tex_tables_set(ds, stage, s, table_first, table_count, rt, zt, n,
                        images->sampled_general ? VK_IMAGE_LAYOUT_GENERAL : VK_LAYOUT(SHADER_READ_ONLY_OPTIMAL), &d, set))
        return VK_NULL_HANDLE;
    /* a stage fs_key didn't key: its handles in one read (tex_handles) */
    u32 handles[SH_MAXTEX];
    /* a program of the texture table: each plain slot's word, in place of its descriptor */
    const int table = s->si.tex_table && g_spirv_translate.tex_table && shader_vk_set(s->stage) < 2;
    u32* words = rec + tex_at; /* after the global buffers' part, and a fetching vertex program's attributes */
    VkImageView tviews[SH_MAXTEX];
    VkImageLayout tlayouts[SH_MAXTEX];
    VkSampler tsamplers[SH_MAXTEX];
    if (!res && nt) tex_handles(ds, stage, tcb, &s->si, nt, handles);
    for (int i = 0; i < nt; i++) {
        ShTex* st = &s->si.tex[i];
        if (st->flags & SNX_TEX_INDEXED) continue;
        ShTex plain;
        if (st->shadow && (s->si.tex_dc_color_mask >> i & 1)) { /* compared in the shader: a plain sampler and view */
            plain = *st;
            plain.shadow = 0;
            st = &plain;
        }
        /* the fragment stage's handle as fs_key read it from the same constant buffer in this draw, which the key it
           made was built from */
        /* the save draw's fetch k takes the texture the guest's draw bound in place (predraw_run) */
        u32 predraw_k = st->cbuf_off - SNX_PREDRAW_HANDLE;
        int predrawn = g_predraw.phase == 1 && stage == ST_FS && predraw_k < g_predraw.n;
        u32 handle = predrawn                ? g_predraw.tex[predraw_k].handle
                     : res && res[i].have ? res[i].handle
                     : !res               ? handles[i]
                                          : tex_handle(ds, stage, tcb, &s->si, i);
        /* what fs_key resolved, unless it is a render target or a copy of one: a crop of the same target at
           another size, for another stage, replaces it. Other textures stay until the next present */
        TexRes own, *x = predrawn ? &g_predraw.tex[predraw_k] : res ? &res[i] : NULL;
        if (!predrawn && (!x || !x->have || x->handle != handle || (x->t && x->t->is_rt))) {
            x = &own;
            tex_resolve(ds, handle, x);
        }
        /* a header with unnormalized coordinates that the program's key doesn't divide (vk_tic_unnormalized: another
           type than 2D, or a stage without the key) is sampled as normalized: said, not hidden */
        int unnormalized = vk_tic_unnormalized(x->tic);
        if (unnormalized && !(st->flags & 3) && !(s->si.tex_rect_mask >> i & 1))
            LOG_LIMIT(4,
                      "vulkan: texture %d of a stage %d program (handle %08x, type %u) takes unnormalized coordinates, "
                      "which its samples don't divide by the size: sampled as normalized\n",
                      i, stage, handle, (x->tic[4] >> 23) & 0xF);
        Surf* t = capture_sample(x->t);
        if (stage == ST_FS) t = predraw_sample(i, t);
        /* a multisampled texture's program fetches its samples (flags bit 1), and an image of other samples than
           the program's reads zeros, as a missing texture does */
        int ms = (st->flags & 2) != 0;
        if (t && ms != (t->msaa != 0)) {
            LOG_LIMIT(4, "vulkan: a texture of %u samples bound where the program fetches %s (read as empty)\n",
                      ds_msaa_samples(t->msaa), ms ? "samples" : "pixels");
            t = NULL;
        }
        /* a stage before the fragment's has no fragment to place a fetch by: its fetches, its size queries and its
           raw loads count the guest's texels, as a compute program's do, so a scaled target comes to it as a copy
           of the guest's size (vk_tex_from_tic_guest), and as its scaled image only where none can be made */
        int guest = stage != ST_FS && t && t->is_rt && (t->sw != t->w || t->sh != t->h);
        if (guest) t = vk_tex_from_tic_guest(x->tic, 0);
        if (st->flags & 1) t = stage != ST_FS ? vk_tex_from_tic_guest(x->tic, 1) : vk_tex_from_tic_raw(x->tic);
        /* a raw load's header without 4-, 8- or 16-byte texels, or no header at all, reads zeros through the null
           view, as a missing texture does: the draw still runs (it was dropped, from one game's movie and gameplay
           passes) */
        if ((st->flags & 1) && !t)
            LOG_LIMIT(1,
                      "vulkan: a raw surface load's header %08x %08x %08x names no texture of 4-, 8- or 16-byte color "
                      "texels stored as they are: it reads zeros\n",
                      x->tic[0], x->tic[2], x->tic[4]);
        VkImageViewType want = vk_tex_view_type(st);
        int num = st->flags & 1                                    ? 1
                  : st->shadow || !((s->si.tex_int_mask >> i) & 1) ? 0
                  : ((s->si.tex_sint_mask >> i) & 1)               ? 2
                                                                   : 1;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = images->sampled_general ? VK_IMAGE_LAYOUT_GENERAL : VK_LAYOUT(SHADER_READ_ONLY_OPTIMAL);
        /* sampling the depth target needs it read-only: no depth writes, and no stencil writes on either face */
        int sten_w = zt && zt->f.stencil && ds->dyn.stencil_test &&
                     ((ds->dyn.face[0].wmask & 0xFF) || (ds->dyn.face[1].wmask & 0xFF));
        int depth_ro = t && t == zt && !ds->dyn.depth_write && !sten_w;
        /* the color slots t is bound to, when the fragment shader may sample it in place: only where each fragment
           reads its own pixel. Other pixels may be the draw's own writes already, in any order the GPU's tiles take,
           and so may the own pixel of a target the draw writes, where two primitives of the draw overlap: the guest's
           texture reads see what was there before the draw. So a slot whose target the draw writes is read in place
           only through the pre-draw capture (ShaderInfo.predraw_slots), and the others read a copy */
        u32 slots = 0;
        if (t && fb && g_vk.has_feedback && t->layout != VK_IMAGE_LAYOUT_UNDEFINED &&
            ((s->si.tex_own_pixel_mask >> i & 1) ||
             (screen && (screen->slots >> i & 1) && screen_covers(&screen->vp, t)))) {
            u32 written = 0;
            for (u32 j = 0; j < n; j++)
                if (rt[j] == t) {
                    slots |= 1u << j;
                    if (ds->blend.mask[j] & ((s->si.sph[18] >> (j * 4)) & 15)) written |= 1u << j;
                }
            /* with the interlock (g_vk.feedback_mode), a written slot without the capture reads a copy. The two draws
               sample in place: the guest's draw reads the save attachment, which it doesn't write */
            if (written && !(s->si.predraw_slots >> i & 1) && g_vk.feedback_mode == VK_FEEDBACK_INTERLOCK) slots = 0;
        }
        if (g_predraw.phase && stage == ST_FS && t && !slots) {
            if (t->predraw) g_predraw_stats.save_copied++;
            else if (predrawn) g_predraw_stats.target_copied++;
        }
        if (slots && predrawn) {
            /* the save draw copies the target's texels as they are, through a view of the format its save attachment
               is written in and none of the header's swizzle or sRGB reading: the guest's draw reads the save
               attachment through the header's view, which applies them once */
            const VkComponentMapping identity = {0};
            view = vk_tex_view(t, want, ds->tgt.srgb_write && vk_srgb_as(t->f.vk, 0) != t->f.vk, &identity, 0);
            if (view) *fb |= slots;
        } else if (slots) {
            /* the guest reads the target's previous contents during a draw that writes it: in place, in a feedback
               loop */
            view = tex_bind_view(x, t, want, num);
            if (view) *fb |= slots;
            if (view && stage == ST_FS) predraw_probe((u32)i, x, t);
        } else if (t && is_target(t, rt, zt, n) && !depth_ro) {
            /* The guest reads the target's previous contents during a draw that writes it. Without a feedback loop
               Vulkan can't bind one image for both uses here, so copy it before the draw starts and sample that
               copy. A slot the program reads no texel of asks only the size or the levels, which the copy answers
               as it is: nothing is copied into it */
            int texels = !(s->si.tex_unread_mask >> i & 1);
            Surf* snap = vk_rt_snapshot(t, texels);
            if (snap != t) {
                t = snap;
                view = tex_bind_view(x, t, want, num);
                if (texels) g_vk_rt_copies++;
            } else
                LOG_LIMIT(1, "vulkan: a draw samples a target it renders to (read as empty)\n");
        } else
            view = tex_bind_view(x, t, want, num);
        const Surf* sampled = view ? t : NULL; /* the texture whose own view is bound, for the sampler */
        if (sampled && st->shadow && !g_settings.shadows) shadow_map_sampled(sampled);
        if (scaled && sampled && (sampled->sw != sampled->w || sampled->sh != sampled->h)) {
            scaled[0] |= 1ull << i;
            if (own_size(sampled, rt, zt, n)) scaled[1] |= 1ull << i;
        }
        if (view && depth_ro) {
            *zt_ro = 1; /* render_begin moves it to the read-only layout */
            layout = VK_LAYOUT(DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        } else if (view && slots)
            layout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT; /* render_begin moves it there */
        else if (view)
            vk_surf_use(vk_cmd(), t, layout, VK_SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        else {
            const char* why = t ? vk_tex_view_refusal(t, want) : NULL;
            if (why)
                LOG_LIMIT(4,
                          "vulkan: texture %d of stage %d shader %016llx (handle %08x, header format %02x, type %u) "
                          "is read as %s, of a %ux%u %s %s of %u layers, and %s: it reads zeros\n",
                          i, stage, (unsigned long long)s->hash, handle, x->tic[0] & 0x7F, (x->tic[4] >> 23) & 0xF,
                          vk_view_type_name(want), t->w, t->h, vk_view_type_name(t->type),
                          t->is_rt ? "render target" : "texture", t->layers, why);
            else if (t)
                LOG_LIMIT(4,
                          "vulkan: texture %d of stage %d (header format %02x) has no %s view of its %s surface: it "
                          "reads zeros\n",
                          i, stage, x->tic[0] & 0x7F,
                          num == 2 ? "signed"
                          : num    ? "unsigned"
                                   : "float",
                          t->f.is_int ? (t->f.sint ? "signed" : "unsigned") : "float");
            view = vk_null_view(want, st->shadow ? 3 : num, ms);
            layout = VK_LAYOUT(SHADER_READ_ONLY_OPTIMAL);
            const u32 index = handle & 0xFFFFF;
            if (view && !x->t && index <= ds->bind.tex_max && vk_tic_names_none(x->tic) &&
                vk_null_header_first(ds->bind.tex_pool + index * 32ull))
                LOG("vulkan: texture %d of stage %d (handle %08x) names %s at %llx (word 0 %08x): it reads the null "
                    "texture, %s, and the draw goes on\n",
                    i, stage, handle, vk_tic_empty(x->tic) ? "an empty header" : "a header of no Vulkan format",
                    (unsigned long long)(ds->bind.tex_pool + index * 32ull), x->tic[0],
                    st->shadow ? "a depth of 1.0 to compare" : "zeros");
        }
        if (!view) return VK_NULL_HANDLE;
        VkSampler smp = tex_bind_sampler(x, st, num, sampled, want, (int)(s->si.tex_minmax_mask >> i & 1));
        if (!smp) return VK_NULL_HANDLE;
        if (table) {
            tviews[i] = view, tlayouts[i] = layout, tsamplers[i] = smp; /* their words after the loop */
            continue;
        }
        u32 b = SNX_VKB_TEX + i;
        d.ii[b] = (VkDescriptorImageInfo){smp, view, layout};
        d.w[d.n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = set,
                                            .dstBinding = b,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                            .pImageInfo = &d.ii[b]};
    }
    if (table && nt) {
        for (int i = 0; i < nt; i++)
            if (s->si.tex[i].flags & SNX_TEX_INDEXED) tviews[i] = VK_NULL_HANDLE; /* in the stage's set: word 0 */
        if (!vk_table_words(tviews, tlayouts, tsamplers, (u32)nt, words)) return VK_NULL_HANDLE;
        if (recorded && !addressed && !fetch) memset(rec, 0, SH_RECORD_GBUF * sizeof *rec);
        rec_n = tex_at + (u32)nt;
    }
    if (rec_n && !table_words(&d, set, shader_vk_set(s->stage), rec, rec_n)) return VK_NULL_HANDLE;
    SNX_ZONE_END(ztex);
    /* by the bindings' stage (DsBindings): an Ounce's task and mesh programs' last */
    static const VkPipelineStageFlags2 image_stages[DS_GFX_STAGES] = {
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT,
        VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT, VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT,
        VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT};
    for (int i = 0; i < s->si.nimg; i++) {
        const u32 bank = s->si.img[i].bindless ? s->si.img[i].bl_cbuf : tcb;
        u64 cba = bank < 18 ? ds->bind.cb_addr[stage][bank] : 0;
        if ((!cba && !ds_cb_inline(ds, stage, bank).bytes) || !images) return VK_NULL_HANDLE;
        u32 handle = cb_word(ds, stage, bank, (u64)s->si.img[i].cbuf_off * 4);
        u64 tic_base = ds->bind.tex_pool;
        u32 tic[8];
        vk_gmem_words(tic_base + (handle & 0xFFFFF) * 32ull, tic, 8);
        int kind = s->si.img[i].kind;
        if (s->si.img[i].type == 1) {
            if (!dw_buffer_image(&d, set, stage, i, tic, tic_base + (handle & 0xFFFFF) * 32ull, kind, handle))
                return VK_NULL_HANDLE;
            continue;
        }
        /* a slot with nothing bound (an empty header): a null descriptor (vk_null_descriptors), or else an empty
           image, which nothing writes back, as a dispatch takes one (compute.c's vk_dispatch), where the draw was
           skipped */
        VkImageView null_view = VK_NULL_HANDLE;
        int null_slot = vk_tic_empty(tic) && vk_null_descriptors();
        Surf* null_image = vk_tic_empty(tic) && !null_slot
                               ? vk_null_storage(vk_storage_image_type(&s->si, i), kind, &null_view)
                               : NULL;
        if (null_image || null_slot) {
            if (null_image)
                vk_surf_use(vk_cmd(), null_image, VK_IMAGE_LAYOUT_GENERAL, image_stages[stage],
                            VK_STORAGE_IMAGE_ACCESS);
            u32 binding = SNX_VKB_IMG + (u32)i;
            d.ii[binding] = (VkDescriptorImageInfo){VK_NULL_HANDLE, null_view, VK_IMAGE_LAYOUT_GENERAL};
            d.w[d.n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                .dstSet = set,
                                                .dstBinding = binding,
                                                .descriptorCount = 1,
                                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                .pImageInfo = &d.ii[binding]};
            continue;
        }
        Surf* surf = vk_tex_from_tic_storage(tic);
        u64 size = vk_surf_guest_size(surf);
        /* a draw can't also write one of its own attachments through a storage image */
        VkImageView view =
            surf && is_target(surf, rt, zt, n) ? VK_NULL_HANDLE : vk_storage_image_view(surf, &s->si, i, &size);
        if (!view) {
            LOG_LIMIT(4,
                      "vulkan: unsupported storage image (draw skipped): %s handle %08x image %p format %u "
                      "raw %d integer %d storage %d levels %u depth %u size %llu target %d view type %d block "
                      "linear %d %ux%u at %ux%u, the program's surface type %d\n",
                      kind == 1 ? "atomic" : "store", handle, (void*)surf, surf ? surf->f.vk : 0,
                      surf ? surf->f.raw : 0, surf ? surf->f.is_int : 0, surf ? surf->storage_capable : 0,
                      surf ? surf->levels : 0, surf ? surf->depth3d : 0, (unsigned long long)size,
                      surf ? is_target(surf, rt, zt, n) : 0, surf ? (int)surf->type : -1, surf ? (int)surf->bl : -1,
                      surf ? surf->w : 0, surf ? surf->h : 0, surf ? surf->sw : 0, surf ? surf->sh : 0,
                      s->si.img[i].type);
            return VK_NULL_HANDLE;
        }
        int seen = 0;
        for (int j = 0; j < images->n; j++)
            if (images->surf[j] == surf) seen = 1;
        /* an earlier draw's writes to it wait for their copy (vk_gfx_image_unrecorded): that copy, recorded later,
           takes this draw's writes too, over the same bytes from before them. Reading guest memory for this draw's own
           would land those writes first, waiting for the GPU (once a frame in one game). One whose copy is recorded
           gives this draw those bytes as it lands (vk_gfx_image_after). A landing during the setup submits, and the
           setup starts over. One no other write-back is over stays on the GPU where it can (vk_surf_keep_begin), with
           no copy of its bytes from before the draw */
        images->after[images->n] = 0;
        images->keep[images->n] = (VkImageKeep){0};
        images->original[images->n] = NULL;
        if (!seen && !vk_gfx_image_unrecorded(surf) &&
            !(images->after[images->n] = vk_gfx_image_after(surf, (u32)size))) {
            if (vk_gfx_image_pending(surf->gva, size) || !vk_surf_keep_begin(surf, &images->keep[images->n])) {
                u8* original = malloc((size_t)size);
                if (!original) return VK_NULL_HANDLE;
                gmem_read(surf->gva, original, size);
                images->original[images->n] = original;
            }
        }
        images->surf[images->n] = surf;
        images->size[images->n] = (u32)size;
        images->n++;
        vk_surf_use(vk_cmd(), surf, VK_IMAGE_LAYOUT_GENERAL, image_stages[stage], VK_STORAGE_IMAGE_ACCESS);
        u32 binding = SNX_VKB_IMG + (u32)i;
        d.ii[binding] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        d.w[d.n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = set,
                                            .dstBinding = binding,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                            .pImageInfo = &d.ii[binding]};
    }
    SNX_ZONE(zset, "stage set descriptors");
    if (!g_spirv_translate.split_sets) return desc_set_cached(layout, &d, stage);
    /* the textures' writes into a set of their own (shader_vk_tex_set), which a draw that changes only its constant
       buffers finds again in the set cache: one set for both was new at each of those draws */
    static DescWrites tex; /* the writes alone: their infos stay in d, which outlives both lookups */
    u32 kept = 0;
    tex.n = 0;
    for (u32 k = 0; k < d.n; k++)
        if (vk_binding_is_texture(d.w[k].dstBinding))
            tex.w[tex.n++] = d.w[k];
        else
            d.w[kept++] = d.w[k];
    d.n = kept;
    const int stage_set_no = shader_vk_set(s->stage);
    /* each set kept as its stage's last apart (desc_set_cached): the textures' past the buffers'. A program of the
       texture table with no indexed textures reads nothing of the set: none is bound */
    VkDescriptorSet textures =
        table && !tex.n ? VK_NULL_HANDLE
                        : desc_set_cached(g_vk_dslayout[s->indexed][stage_set_no + 5], &tex, stage + DS_GFX_STAGES);
    if (!textures && !(table && !tex.n)) return VK_NULL_HANDLE;
    g_stage_tex_set[stage_set_no] = textures;
    return desc_set_cached(layout, &d, stage);
}

/* ------------------------------------------------------------ draws */
/* The dynamic state, pipeline and descriptor sets the previous draw recorded in the command buffer g_dyn.cb names
   (vk_cb_begun): a draw records only what differs, where every draw set about 30 states again (one game: 6,700 draws a
   frame). Every draw pipeline has these states dynamic, so binding another one keeps them. The clear path and the
   on-screen controls set or bind otherwise and forget them (vk_render_forget_state) */
/* Maxwell's viewports, each with its own transform, clip and scissor (NVIDIA open-gpu-doc classes/3d/clb197.h):
   SET_VIEWPORT_SCALE_X(j) at method 0x0A00 + 32 j (register 0x280 + 8 j) for j < 16, then
   SET_VIEWPORT_CLIP_HORIZONTAL(j) at 0x0C00 + 16 j (0x300 + 4 j) and SET_SCISSOR_ENABLE(j) at 0x0E00 + 16 j
   (0x380 + 4 j). A program that writes the viewport index selects among them */
#define GUEST_VIEWPORTS 16
static struct {
    u32 cb;
    u64 heap_epoch;
    int heaps;
    VkPipeline pipe;
    VkPipelineLayout layout; /* the one sets were bound through: another may disturb them */
    VkDescriptorSet sets[11]; /* 5 to 9: the stages' texture sets with split sets; the texture table at table_set */
    u64 set_binds; /* vk_rec_set_binds when sets were: a set bound the old way since unbinds descriptor buffers */
    u32 fb, cull, front, topo, restart, discard, bias_on, dt, dw, dcmp, dbounds, st;
    u32 polygon, clamp, clip, provoking, conservative, patch; /* g_vk.dyn_pr's, as PipeKey holds them */
    VkDepthClampRangeEXT clamp_range;                         /* with has_depth_clamp_range */
    u32 minus_one;                                      /* and its depth range, which no key holds: from -1 */
    VkViewport vp[GUEST_VIEWPORTS];
    VkRect2D sc[GUEST_VIEWPORTS];
    u32 viewports;
    float lw, bias[3], bc[4], depth_bounds[2];
    u32 sfront[7], sback[7];
    /* the vertex layout set as dynamic state, with its bindings' strides (vertex_input_set): vi_set once one is */
    int vi_set;
    VertexLayout vi;
    VkDeviceSize vi_stride[33];
    /* the vertex buffers bound last (render_draw), with the strides as dynamic state where they are */
    u32 vb_n;
    int vb_strided;
    VkBuffer vb_buf[33];
    VkDeviceSize vb_off[33], vb_size[33], vb_stride[33];
} g_dyn;
#define DYN(field, value, call)                                                                                        \
    do {                                                                                                               \
        if (fresh || g_dyn.field != (value)) {                                                                         \
            g_dyn.field = (value);                                                                                     \
            call;                                                                                                      \
        }                                                                                                              \
    } while (0)
#define DYNV(field, value, call)                                                                                       \
    do {                                                                                                               \
        if (fresh || memcmp(&g_dyn.field, &(value), sizeof g_dyn.field)) {                                             \
            memcpy(&g_dyn.field, &(value), sizeof g_dyn.field);                                                        \
            call;                                                                                                      \
        }                                                                                                              \
    } while (0)
void vk_render_forget_state(void) { g_dyn.cb = 0; }
/* the pre-rasterization state a pipeline of libraries takes as dynamic (g_vk.dyn_pr), from the key it was made for:
   what a whole pipeline holds of k, the draw sets (fresh: all of it). And the clip volume's depth, from -w
   (minus_one) or from 0, which the device then makes in place of the last stage's (z + w) / 2 */
static void pr_dynamic(VkCommandBuffer cb, const PipeKey* k, int minus_one, int fresh) {
    u32 d = g_vk.dyn_pr;
    if (d & VK_DYN_PR_DEPTH_RANGE)
        DYN(minus_one, (u32)minus_one, rec_CmdSetDepthClipNegativeOneToOneEXT(cb, minus_one ? VK_TRUE : VK_FALSE));
    if (d & VK_DYN_PR_POLYGON) DYN(polygon, k->polygon, rec_CmdSetPolygonModeEXT(cb, (VkPolygonMode)k->polygon));
    if (d & VK_DYN_PR_DEPTH_CLAMP)
        DYN(clamp, k->depth_clamp, rec_CmdSetDepthClampEnableEXT(cb, k->depth_clamp ? VK_TRUE : VK_FALSE));
    if (d & VK_DYN_PR_DEPTH_CLIP)
        DYN(clip, k->depth_clip, rec_CmdSetDepthClipEnableEXT(cb, k->depth_clip == 2 ? VK_TRUE : VK_FALSE));
    if (d & VK_DYN_PR_PROVOKING)
        DYN(provoking, k->provoking_last,
            rec_CmdSetProvokingVertexModeEXT(cb, k->provoking_last ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT
                                                                   : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT));
    if (d & VK_DYN_PR_CONSERVATIVE)
        DYN(conservative, k->conservative, {
            rec_CmdSetConservativeRasterizationModeEXT(cb, k->conservative
                                                               ? VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT
                                                               : VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT);
            rec_CmdSetExtraPrimitiveOverestimationSizeEXT(cb, vk_conservative_extra(k->conservative));
        });
    if ((d & VK_DYN_PR_PATCH) && k->tes) DYN(patch, k->patch, rec_CmdSetPatchControlPointsEXT(cb, k->patch));
}
u64 g_vk_drawn, g_vk_skipped, g_vk_mesh_draws, g_vk_mesh_drawn, g_vk_mesh_task_compute;
/* draws whose depth the console both clips and clamps (its clamp off), and of them those that write their own depth or
   take a depth bias, whose depth may leave the viewport's range: undefined there without VK_EXT_depth_clip_enable */
u64 g_vk_depth_apart_draws, g_vk_depth_apart_moved;
u64 g_vk_capture_draws, g_vk_capture_primitives, g_vk_capture_capped;

/* The viewport's size with the transform off: the surface clip's, at most the device's viewport dimensions at the
   resolution scale (a game's driver leaves the clip at 16384 x 16384, past every target). The last stage divides
   the position by the same size (the push constant's w), so every pixel stays where it is */
static void window_size(const DsDynamic* d, u32* width, u32* height) {
    const u32* most = g_vk.props.limits.maxViewportDimensions;
    u32 w = (u32)((float)most[0] / g_vk_scale), h = (u32)((float)most[1] / g_vk_scale);
    *width = d->clip_width < w ? d->clip_width : w;
    *height = d->clip_height < h ? d->clip_height : h;
}
/* NVB197_SET_VIEWPORT_COORDINATE_SWIZZLE (0x0A18 + 32 j) gives each of the position's x, y, z and w one of its four
   components, negated or not, before clipping, the scale and the offset. NegativeY alone is a negative Vulkan height,
   the same mapping without changing texture coordinates or guest render-target contents. Any other swizzle the last
   stage before rasterization applies to the position it writes (shader/emit_io.c shader_vertex_position_fix, keyed
   by SNX_SH_SWIZZLE): clipped after it as the console clips, and faced as the picture it makes, as Vulkan faces the
   negative height. One for every viewport, viewport 0's, which a program that writes no viewport index draws with:
   a viewport the program selects that asks for another draws with it (vk_viewport_swizzle_drawn), and the draw logs
   both. VK_NV_viewport_swizzle, which would take each viewport's, is not used. Reference: NVIDIA
   open-gpu-doc/classes/3d/clb197.h. With the transform off (0x64B) the program's x and y are pixels, which its last
   stage takes to clip space by the surface clip's size (SNX_SH_WINDOW_XY): every viewport is the surface clip then,
   depth 0 to 1, the scale, offset and swizzle left to the transform, as Eden's UpdateViewportsState has it. */
u32 vk_viewport_swizzle_key(const DsViewports* v) {
    u32 s = v->swizzle[0] & DS_SWIZZLE_MASK;
    if (!v->transform_on || s == DS_SWIZZLE_IDENTITY || s == DS_SWIZZLE_NEGATIVE_Y) return 0;
    return SNX_SH_SWIZZLE | s << SNX_SH_SWIZZLE_SHIFT;
}
u32 vk_viewport_swizzle_drawn(const DsViewports* v, u32 index) {
    if (index >= DS_VIEWPORTS_MAX) fatal("vulkan: viewport %u of the %u the GPU has", index, DS_VIEWPORTS_MAX);
    u32 key = vk_viewport_swizzle_key(v), own = v->swizzle[index] & DS_SWIZZLE_MASK;
    if (key) return key >> SNX_SH_SWIZZLE_SHIFT & DS_SWIZZLE_MASK;
    return own == DS_SWIZZLE_NEGATIVE_Y ? own : DS_SWIZZLE_IDENTITY;
}
VkViewport vk_viewport_from(const DsDynamic* d, const DsViewports* v, u32 index) {
    if (index >= DS_VIEWPORTS_MAX) fatal("vulkan: viewport %u of the %u the GPU has", index, DS_VIEWPORTS_MAX);
    const float* vs = v->scale[index];
    const float* vt = v->translate[index];
    float sx = vs[0], sy = vs[1], tx = vt[0], ty = vt[1], ax = sx < 0 ? -sx : sx;
    VkViewport vp;
    if (!v->transform_on) {
        u32 w, h;
        window_size(d, &w, &h);
        LOG_LIMIT(1,
                  "vulkan: viewport transform off: positions are pixels of the %ux%u surface clip at (%u, %u), "
                  "the viewport %ux%u of it\n",
                  d->clip_width, d->clip_height, d->clip_x, d->clip_y, w, h);
        float x = vk_scf((float)d->clip_x), y = vk_scf((float)d->clip_y);
        vp = (VkViewport){x, y, vk_scf((float)w), vk_scf((float)h), 0, 1};
    } else
        vp = (VkViewport){vk_scf(tx - ax), vk_scf(ty - sy), vk_scf(2 * ax), vk_scf(2 * sy), 0, 1};
    /* A lower-left window measures y from the surface clip's bottom. Match the 3D blit path's conversion. */
    if (d->window_origin & DS_ORIGIN_LOWER_LEFT) {
        vp.y += vk_scf((float)d->clip_height);
        vp.height = -vp.height;
    }
    if (!v->transform_on) return vp;
    if (vk_viewport_swizzle_drawn(v, index) == DS_SWIZZLE_NEGATIVE_Y) {
        vp.y += vp.height;
        vp.height = -vp.height;
    }
    float zn = v->z_near[index], zf = v->z_far[index];
    if (vs[2] != 0.0f || vt[2] != 0.0f) {
        zn = vt[2] - (d->depth_mode ? 0.0f : vs[2]);
        zf = vt[2] + vs[2];
    }
    vp.minDepth = zn < 0 ? 0 : zn > 1 ? 1 : zn;
    vp.maxDepth = zf < 0 ? 0 : zf > 1 ? 1 : zf;
    return vp;
}
/* the vertices of the geometry program's input primitive: the tessellator's output primitive (0xC8: the domain in
   bits 0-1, isolines 0, and the output in bits 8-9, points 0), or the draw's topology */
static u32 gs_input_vertices(u32 topology, int tes_on, u32 params) {
    if (tes_on) return ((params >> 8) & 3) == 0 ? 1 : (params & 3) == 0 ? 2 : 3;
    return topology == 0                      ? 1
           : topology <= 3                    ? 2
           : topology == 10 || topology == 11 ? 4
           : topology == 12 || topology == 13 ? 6
                                              : 3;
}

/* WINDOW_ORIGIN_FLIP_Y reverses the facing over a negative viewport too: the guest's negative scale, NegativeY and
   the lower-left origin mirror the winding on the console as their negative Vulkan viewports do here (Eden's
   UpdateFrontFace) */
VkFrontFace vk_render_front_face(const DsDynamic* d) {
    int cw = d->front_cw;
    if (d->window_origin & DS_ORIGIN_FLIP_Y) cw = !cw;
    return cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

/* the Vulkan topology of a draw's (the 3D engine's numbers, 14 a patch list) */
static VkPrimitiveTopology topology_vk(u32 topology) {
    static const VkPrimitiveTopology adjacent[5] = {
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    if (topology >= 10 && topology <= 14) return adjacent[topology - 10];
    switch (topology) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case 2: /* a line loop: a list of its sides where the CPU makes it (draw_edges), else open */
        case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 5:
        case 8: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; /* quad strips are triangle strips */
        case 6:
        case 9: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;   /* polygons are fans */
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; /* triangles, and quads made into them */
    }
}
/* ------------------------------------------------------------ a draw's shaders and pipeline, kept between draws */
/* the draw state's groups (their generations, and their bytes' hashes in DrawKey) a draw's shaders and pipeline key are
   made from, but the vertex format, whose change is looked at closer (vertex_format_holds) */
static const u8 g_draw_groups[] = {DS_PROGRAMS, DS_BLEND, DS_RASTER_KEY, DS_XFB, DS_TARGET_FORMATS};
/* the programs a draw's registers select */
typedef struct {
    VkSh *vs, *tcs, *tes, *gs; /* the stages between NULL when off */
    VkSh* fs0;                 /* the fragment program as it is, NULL when off: its textures pick the variant drawn */
    u32 attr_int, attr_sint;   /* the integer vertex attributes, of all 32, which key the vertex program */
    u32 rt_int, rt_sint;       /* the integer color targets the registers name (rt_int_targets) */
    u32 topology;              /* the draw's, every primitive a patch with tessellation */
    int xfb_on;                /* transform feedback captures */
    int window_xy;             /* the viewport transform is off: the last stage keyed SNX_SH_WINDOW_XY */
    u32 swizzle;               /* the last stage's viewport swizzle key (vk_viewport_swizzle_key) */
    /* an Ounce's mesh draw (DrawCall.mesh): vs is the mesh program and tcs its task program (NULL for none), and prim
       the generics the mesh program writes per primitive, which key fs0 */
    int mesh;
    u32 prim;
    /* the compute-task path (sm86.h): the task program is a compute dispatch that writes its payload buffer, whose
       slots are payload_stride words for each task workgroup */
    int task_compute;
    u32 payload_stride;
} DrawShaders;
/* a draw's vertex layout (vi, which the pipeline key holds too without dynamic vertex input) and its bindings, for
   finding their buffers each draw: binding b reads guest stream key[b] & 31 from its base offset key[b] >> 5 on, with
   pad bytes past its last stride when still inside the stream's limit, or the constants of the attributes without
   data when key[b] is ~0u. When the guest's stride isn't a multiple of its attributes' components (guest_stride[b],
   else 0), the binding reads a copy with the vertices stride[b] apart, rounded up to a multiple of 4 (restride) */
typedef struct {
    VertexLayout vi;
    u32 key[33], pad[33], guest_stride[33];
    VkDeviceSize stride[33];
    u32 used;  /* the streams whose limits the bindings were planned with */
    u32 empty; /* attributes whose relative offsets are outside their guest bindings */
    /* a vertex program that fetches its own attributes (fetch_plan): each attribute's format word (SH_ATTR_), and vi
       its bindings, one a stream, for the lookups. No vertex input state is made from vi then */
    u32 fetch[32];
    u32 divisor[32]; /* each attribute's instance divisor, 0 for one a vertex (SH_RECORD_DIVISOR) */
} VertexPlan;
/* pl's vertex layout as dynamic state, when the one the command buffer has (g_dyn) is another or none. Every draw
   pipeline takes it dynamic then, so binding another keeps it; a draw before it is set reads a layout of the driver's */
static void vertex_input_set(VkCommandBuffer cb, const VertexPlan* pl, int fresh) {
    const VertexLayout* vi = &pl->vi;
    if (!fresh && g_dyn.vi_set && !memcmp(&g_dyn.vi, vi, sizeof *vi) &&
        !memcmp(g_dyn.vi_stride, pl->stride, vi->nbind * sizeof *pl->stride))
        return;
    VkVertexInputBindingDescription2EXT b[33];
    VkVertexInputAttributeDescription2EXT a[32];
    for (u32 i = 0; i < vi->nbind; i++)
        b[i] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .binding = i,
            .stride = (u32)pl->stride[i],
            .inputRate = vi->bind[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            .divisor = vi->bind[i].divisor ? vi->bind[i].divisor : 1}; /* 1 per vertex, as Vulkan requires */
    for (u32 i = 0; i < vi->nattr; i++)
        a[i] =
            (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                                                    .location = vi->attr[i].loc,
                                                    .binding = vi->attr[i].binding,
                                                    .format = (VkFormat)vi->attr[i].format,
                                                    .offset = vi->attr[i].offset};
    g_dyn.vi_set = 1;
    g_dyn.vi = *vi;
    memcpy(g_dyn.vi_stride, pl->stride, vi->nbind * sizeof *pl->stride);
    rec_CmdSetVertexInputEXT(cb, vi->nbind, b, vi->nattr, a);
}
/* what a pipeline key takes from the surfaces drawn to: the color slots up to the last bound one, those drawn to,
   which of them are integer, their formats, and the depth target's */
typedef struct {
    u32 n, present, is_int, stencil;
    u32 color[8], depth;
    u32 samples, depth_samples; /* color count (depth count for depth-only); 0 when color counts differ */
} TargetFormats;
/* A key of what a draw's shaders and pipeline are made from, by content: a hash of each group of g_draw_groups (of
   their bytes, not their generations: a draw between two of the same draw gives the groups new generations and leaves
   their bytes), the owner of the state, the draw's topology before tessellation makes it a patch list, whether it is a
   mesh draw, and whether the descriptor heaps are on. All its bytes are set, for memcmp */
enum { DRAW_KEY_GROUPS = 5 };
typedef char draw_key_groups_check[sizeof g_draw_groups == DRAW_KEY_GROUPS ? 1 : -1];
typedef struct {
    u64 hash[DRAW_KEY_GROUPS][2]; /* vk_hash128 (draw_group_hash) */
    u32 owner, topology;
    u32 mesh, heaps;
} DrawKey;
/* What a draw made from its state, kept for later draws while nothing it was made from changed: the state's owner and
   its groups (g_draw_groups and the vertex format), the draw's topology, the shaders' code (trusted within an epoch and
   shader generation, as vk_shader_get trusts it) and the constant buffer words their translation read (checked each
   draw, as vk_shader_get checks them on a hit). What else goes into it is found each draw and compared: the fragment
   variant's key (its textures' formats), the targets' formats, which vertex streams are empty, and whether the draw
   samples a target it renders to. Draws made all of it again, a 1 KiB key built and compared included, for each of
   the ~3,400 draws a frame in one game, most of them with the state of the draw before. draw_state_check=1 makes it
   again for each draw that keeps it, and warns of any difference: a register missing from a group, or an input that
   isn't compared */
typedef struct {
    DrawKey key;           /* owner, topology and the groups' hashes */
    u64 gen[DS_GENS];      /* the state's generations at the last draw that made or took it */
    u32 epoch;             /* vk_ww_epoch */
    u32 used;              /* g_draw.use when last made or taken: of a set's ways, the one used longest ago makes room */
    u64 shader_generation; /* instruction invalidations also expire kept draws */
    /* A program can have more than one translation, each with other constant buffer words folded (cb_fold.h), and
       vk_shader_get takes the one that folds the most words of those that hold. A kept shader still holds when
       another translation of its program starts to hold, because it doesn't read the words that the other folds. So
       fold_gen is vk_shader_fold_gen when the state was kept (no translation came since), and not_top has a bit for
       each shader of used_shaders that another translation of its program folded more than then. draw_holds
       asks vk_shader_chosen for those. Without these checks, draw_state_check=1 found 0.22% of the kept draws in one
       game's town with a fragment program that folded no word, where a lookup gave one that folded a word: the game
       had set that word to the value the other translation folded */
    u32 fold_gen, not_top;
    DrawShaders sh; /* the rest holds when sh.vs is set */
    FsKey fkey;
    VkSh* fs;
    VertexPlan plan;
    TargetFormats tf;
    PipeKey k; /* k and pipe hold together whenever pipe is set */
    VkPipeline pipe;
    /* the vertex format the state was made from, for a change that leaves what it read as it was: attributes and
       streams the vertex program doesn't read. The attributes changed in a third of one game's draws, and comparing
       what was read kept 66% of its draws where the register groups alone kept 60% */
    DsVertexFormat vfmt;
    u32 vstreams; /* the streams the attributes the vertex program reads name */
} DrawKept;
/* a group's hash at a generation (g_draw.group) */
typedef struct {
    u64 gen, hash[2];
} DrawGroupHash;
/* The kept draws, in a table by their key (DrawKey), in sets of DRAW_WAYS ways, where a draw finds what an earlier
   draw of the same state made in this epoch, with draws of other state between them (A, B, A) or not. A set can hold
   two entries of one key, made from other vertex formats (vertex_format_holds) */
#define DRAW_SET_BITS  8
#define DRAW_WAYS      4
#define DRAW_TABLE     1 /* the table's first entry: set s's way w is DRAW_TABLE + s * DRAW_WAYS + w */
#define DRAW_HASH_BITS 6
static struct {
    DrawKept e[DRAW_TABLE + (DRAW_WAYS << DRAW_SET_BITS)]; /* 0 is none */
    u64 tag[1u << DRAW_SET_BITS][DRAW_WAYS]; /* each way's key mixed (draw_key_mix), odd; 0 when the way is empty */
    /* each group's last hashes by generation: a generation is one state of one owner's group (ds_generation_next), so
       a group hashed at a generation is the same bytes while it lasts. 0, a state never synced, is hashed each time */
    DrawGroupHash group[DRAW_KEY_GROUPS][1u << DRAW_HASH_BITS];
    u32 last; /* the entry the last draw made or took, 0 for none */
    u32 use;  /* counts the entries made or taken */
    VkSh* fs; /* the fragment shader the last draw drew with, which a capture's resolve reads */
    int told; /* draw_state_check=1's line is in the log */
} g_draw;
u64 g_vk_draw_kept, g_vk_draw_checked;

static void draw_differs(const char* what) {
    g_stats.n.differs++;
    WARN_LIMIT(32,
               "vulkan: draw_state_check=1: a draw that kept an earlier draw's state has another %s when it is made "
               "again (%llu draws checked)\n",
               what, (unsigned long long)g_vk_draw_checked);
}
/* the next draw keeps nothing of the last one's. The table's entries stay: only draws of the guest's own state make
   them, and each is checked as a draw takes it */
static void draw_forget(void) { g_draw.last = 0; }
/* entry i was found wrong (draw_state_check=1): emptied, and out of its set */
static void draw_drop(u32 i) {
    if (!i) return;
    g_draw.e[i].sh.vs = NULL;
    if (i >= DRAW_TABLE) g_draw.tag[(i - DRAW_TABLE) / DRAW_WAYS][(i - DRAW_TABLE) % DRAW_WAYS] = 0;
    if (g_draw.last == i) g_draw.last = 0;
}
void vk_render_draws_forget(void) {
    memset(g_draw.tag, 0, sizeof g_draw.tag);
    draw_forget();
    /* and the constant buffers, views and samplers a later draw takes past its submission or in its frame: a loaded
       state's surfaces and buffers can be other objects at the same addresses */
    memset(&g_cb, 0, sizeof g_cb);
    memset(&g_dset, 0, sizeof g_dset);
    for (u32 i = 0; i < 1u << TEX_MEMO_SET_BITS; i++)
        for (u32 w = 0; w < TEX_MEMO_WAYS; w++) g_tex_memo.set[i][w].view.frame = g_tex_memo.set[i][w].smp.frame = 0;
    /* and the caches in front of them, which keep Surfs, buffers and words by those addresses too */
    memset(g_tex_recent, 0, sizeof g_tex_recent); /* TEX_RECENT_NONE */
    g_fs_last.valid = 0;
    memset(g_storage_recent, 0, sizeof g_storage_recent);
    memset(g_cb_windows, 0, sizeof g_cb_windows);
    memset(g_table_words, 0, sizeof g_table_words);
}
/* group i of g_draw_groups of ds hashed: the hash its generation had, or (check: also, to compare) its bytes'. The
   targets' formats have no struct: what target_formats_differ (gpu/maxwell/draw_state.c) compares is hashed */
static void draw_group_hash(const DsState* ds, u32 i, int check, u64 out[2]) {
    u64 gen = ds->gen[g_draw_groups[i]], h[2];
    DrawGroupHash* had = &g_draw.group[i][gen & ((1u << DRAW_HASH_BITS) - 1)];
    int known = gen && had->gen == gen;
    if (known && !check) {
        out[0] = had->hash[0];
        out[1] = had->hash[1];
        return;
    }
    switch (g_draw_groups[i]) {
        case DS_PROGRAMS: vk_hash128(&ds->prog, sizeof ds->prog, h); break;
        case DS_BLEND: vk_hash128(&ds->blend, sizeof ds->blend, h); break;
        case DS_RASTER_KEY: vk_hash128(&ds->rkey, sizeof ds->rkey, h); break;
        case DS_XFB: vk_hash128(&ds->xfb, sizeof ds->xfb, h); break;
        default: {
            u32 w[3 + DS_TARGETS_MAX] = {ds->tgt.count, ds->tgt.srgb_write, ds->tgt.msaa};
            for (u32 t = 0; t < DS_TARGETS_MAX; t++) w[3 + t] = ds->tgt.color[t].format;
            vk_hash128(w, sizeof w, h);
        }
    }
    g_stats.n.hashed++;
    if (known && (h[0] != had->hash[0] || h[1] != had->hash[1])) draw_differs("draw state hash");
    had->gen = gen;
    had->hash[0] = out[0] = h[0];
    had->hash[1] = out[1] = h[1];
}
static void draw_memo_key(const DsState* ds, u32 topology, int mesh, int check, DrawKey* key) {
    for (u32 i = 0; i < DRAW_KEY_GROUPS; i++) draw_group_hash(ds, i, check, key->hash[i]);
    key->owner = ds->owner;
    key->topology = topology;
    key->mesh = (u32)mesh;
    key->heaps = (u32)g_vk.has_descriptor_heaps;
}
/* a key's words mixed, odd: its set is the top DRAW_SET_BITS */
static u64 draw_key_mix(const DrawKey* key) {
    u64 m = (u64)key->owner << 32 ^ (u64)key->topology << 2 ^ (u64)key->mesh << 1 ^ key->heaps;
    for (u32 i = 0; i < DRAW_KEY_GROUPS; i++) m = (m ^ key->hash[i][0]) * VK_FIB_MULT;
    return m | 1;
}
static u32 draw_key_set(u64 mix) { return (u32)(mix >> (64 - DRAW_SET_BITS)); }
/* the kept shaders that hold their constant buffer words: the stages, the fragment program and its variant */
static void used_shaders(const DrawKept* e, const VkSh* used[6]) {
    const DrawShaders* s = &e->sh;
    used[0] = s->vs;
    used[1] = s->tcs;
    used[2] = s->tes;
    used[3] = s->gs;
    used[4] = s->fs0;
    used[5] = e->fs;
}
/* whether the vertex format f is, in what e read of it, the one e was made from: the attributes the vertex program
   reads, the streams they name, and which of all 32 attributes are integers (they key the program) */
static int vertex_format_holds(const DsVertexFormat* f, const DrawKept* e) {
    const DrawShaders* s = &e->sh;
    const DsVertexFormat* k = &e->vfmt;
    u32 in = s->vs->si.in_mask, st = e->vstreams;
    if (f->is_int != s->attr_int || f->is_sint != s->attr_sint || ((f->constant ^ k->constant) & in) ||
        ((f->bgra ^ k->bgra) & in) || ((f->stream_on ^ k->stream_on) & st) || ((f->instanced ^ k->instanced) & st))
        return 0;
    for (u32 m = in; m; m &= m - 1) {
        u32 i = snx_ctz32(m);
        if (f->stream[i] != k->stream[i] || f->size[i] != k->size[i] || f->type[i] != k->type[i] ||
            f->offset[i] != k->offset[i])
            return 0;
    }
    for (u32 m = st; m; m &= m - 1) {
        u32 i = snx_ctz32(m);
        if (f->stride[i] != k->stride[i] || f->divisor[i] != k->divisor[i]) return 0;
    }
    return 1;
}
/* whether entry e, whose groups' bytes are this draw's, holds for this draw of ds, as far as what is checked before
   drawing tells */
static int draw_holds(DrawKept* e, const DsState* ds, u32 topology, int mesh, u32 epoch) {
    const DrawShaders* s = &e->sh;
    /* a draw's path, as draw_shaders' mesh argument: 1 mesh, 2 the compute-task path (sm86.h) */
    if (!s->vs || (int)(s->mesh | (s->task_compute ? 2 : 0)) != mesh) return 0;
    if (e->shader_generation != g_vk.shader_generation) return 0;
    if (e->fold_gen != vk_shader_fold_gen()) return 0; /* a translation came that a lookup may take instead */
    if (e->k.heaps != (u32)g_vk.has_descriptor_heaps) return 0;
    if (e->key.owner != ds->owner || e->key.topology != topology || e->epoch != epoch) return 0;
    if (e->gen[DS_VFORMAT] != ds->gen[DS_VFORMAT]) { /* the vertex format changed: what the state read may not have */
        if (!vertex_format_holds(&ds->vfmt, e)) return 0;
        e->gen[DS_VFORMAT] = ds->gen[DS_VFORMAT];
    }
    if (s->xfb_on != xfb_wanted(ds->prog.xfb_on)) return 0; /* capture began or ended, or its counters were made */
    if (s->window_xy != !ds->vp.transform_on) return 0; /* the viewports' group keys no shader otherwise */
    if (s->swizzle != vk_viewport_swizzle_key(&ds->vp)) return 0;
    const VkSh* used[6];
    used_shaders(e, used);
    for (int i = 0; i < 6; i++) {
        if (!used[i]) continue;
        if (e->not_top >> i & 1) {
            if (!vk_shader_chosen(ds, used[i])) return 0;
        } else if (used[i]->cbr.n && !vk_shader_cb_hold(ds, used[i]))
            return 0;
    }
    return 1;
}
/* The kept entry that holds for this draw of ds, 0 for none: the last draw's when the groups' generations are its, as
   they are for most draws in a row; else the table's latest entry of the draw's key that holds. *key is made when the
   table is looked in (*have set). check (draw_state_check=1): the groups are hashed again, and the last draw's entry's
   key is compared with them */
static u32 draw_find(const DsState* ds, u32 topology, int mesh, u32 epoch, int check, DrawKey* key, int* have) {
    u32 last = g_draw.last;
    *have = 0;
    g_stats.n.lookups++;
    if (last) {
        DrawKept* e = &g_draw.e[last];
        int same = e->sh.vs != NULL;
        for (u32 i = 0; same && i < DRAW_KEY_GROUPS; i++)
            same = e->gen[g_draw_groups[i]] == ds->gen[g_draw_groups[i]];
        if (same && draw_holds(e, ds, topology, mesh, epoch)) {
            if (check) {
                draw_memo_key(ds, topology, mesh, 1, key);
                *have = 1;
                if (memcmp(key, &e->key, sizeof *key)) draw_differs("draw state key");
            }
            g_stats.n.last++;
            return last;
        }
    }
    draw_memo_key(ds, topology, mesh, check, key);
    *have = 1;
    u64 mix = draw_key_mix(key);
    u32 set = draw_key_set(mix), found = 0;
    for (u32 w = 0; w < DRAW_WAYS; w++) {
        u32 i = DRAW_TABLE + set * DRAW_WAYS + w;
        DrawKept* e = &g_draw.e[i];
        if (g_draw.tag[set][w] != mix || memcmp(&e->key, key, sizeof *key)) continue;
        if (found && (i32)(e->used - g_draw.e[found].used) < 0) continue; /* the latest of those that hold */
        if (draw_holds(e, ds, topology, mesh, epoch)) found = i;
    }
    if (!found) return 0;
    DrawKept* e = &g_draw.e[found];
    for (u32 i = 0; i < DRAW_KEY_GROUPS; i++) e->gen[g_draw_groups[i]] = ds->gen[g_draw_groups[i]];
    g_stats.n.table++;
    return found;
}
/* the entry a draw's state is kept in: found, which the draw found and couldn't take (of the same key), else an empty
   way of its key's set, else the way used longest ago */
static u32 draw_slot(const DrawKey* key, u32 found) {
    if (found >= DRAW_TABLE) return found;
    u64 mix = draw_key_mix(key);
    u32 set = draw_key_set(mix), w = 0;
    const DrawKept* ways = &g_draw.e[DRAW_TABLE + set * DRAW_WAYS];
    for (u32 v = 1; v < DRAW_WAYS && g_draw.tag[set][w]; v++)
        if (!g_draw.tag[set][v] || (i32)(ways[v].used - ways[w].used) < 0) w = v;
    if (g_draw.tag[set][w]) g_stats.n.evicted++;
    g_draw.tag[set][w] = mix;
    return DRAW_TABLE + set * DRAW_WAYS + w;
}
static int draw_shaders_same(const DrawShaders* a, const DrawShaders* b) {
    return a->vs == b->vs && a->tcs == b->tcs && a->tes == b->tes && a->gs == b->gs && a->fs0 == b->fs0 &&
           a->attr_int == b->attr_int && a->attr_sint == b->attr_sint && a->rt_int == b->rt_int &&
           a->rt_sint == b->rt_sint && a->topology == b->topology && a->xfb_on == b->xfb_on &&
           a->window_xy == b->window_xy && a->swizzle == b->swizzle && a->mesh == b->mesh && a->prim == b->prim &&
           a->task_compute == b->task_compute && a->payload_stride == b->payload_stride;
}
/* An Ounce's mesh draw's shaders (DrawCall.mesh): the mesh program in vs, its task program in tcs, and the fragment
   program keyed on the generics the mesh program writes per primitive. 0 when the draw is skipped, the reason logged
   once for each */
static int mesh_shaders(const DsState* ds, DrawShaders* o, int task_compute) {
    const DsPrograms* p = &ds->prog;
    const DsMeshKey* k = &p->mesh_key;
    u32 stride = 0;
    *o = (DrawShaders){.mesh = 1, .topology = 4, .attr_int = ds->vfmt.is_int, .attr_sint = ds->vfmt.is_sint};
    if (!g_vk.has_mesh) return 0; /* device.c's mesh_choose said why */
    if (!p->mesh || k->has_task != (p->task != 0)) {
        LOG_LIMIT(4, "vulkan: a mesh draw with %s mesh program and %s task program (skipped)\n", p->mesh ? "a" : "no",
                  p->task ? "a" : "no");
        return 0;
    }
    if (p->task && (!g_vk.has_task || g_vk_stage_sets <= (u32)shader_vk_set(ST_TS))) {
        LOG_LIMIT(1, "vulkan: this GPU has no task shaders, or too few descriptor sets for them: mesh draws with a "
                     "task program are skipped\n");
        return 0;
    }
    /* the compute-task path's slot: the larger of the two programs' payloads, as sm86.h's buffer has them */
    if (task_compute) {
        u32 task_words = ((u32)k->task_payload + 3u) / 4u, mesh_words = ((u32)k->mesh_payload + 3u) / 4u;
        stride = task_words > mesh_words ? task_words : mesh_words;
        if (!stride) {
            LOG_LIMIT(4, "vulkan: a mesh draw with a task program of no payload (drawn natively)\n");
            task_compute = 0;
        }
    }
    o->task_compute = task_compute;
    o->payload_stride = stride;
    o->vs = vk_shader_mesh(ds, p->mesh, ST_MS, k, stride, task_compute);
    o->tcs = p->task ? vk_shader_mesh(ds, p->task, ST_TS, k, stride, task_compute) : NULL;
    int bad = !o->vs || o->vs->failed ? 1 : p->task && (!o->tcs || o->tcs->failed) ? 2 : 0;
    if (bad) {
        const VkSh* s = bad == 1 ? o->vs : o->tcs;
        LOG_LIMIT(
            16,
            "vulkan: a mesh draw skipped: its %s program at %llx (%u and %u invocations, payload %#x, %u vertices "
            "and %u primitives, per-primitive SPH at %#x) %s%s\n",
            bad == 1 ? "mesh" : "task", (unsigned long long)(bad == 1 ? p->mesh : p->task), k->task_threads,
            k->mesh_threads, k->mesh_payload, k->max_vertices, k->max_primitives, k->prim_sph,
            s ? "isn't translated: " : "has no shader",
            !s                           ? ""
            : s->si.first_unsupported[0] ? s->si.first_unsupported
                                         : "unsupported feature");
        return 0;
    }
    o->prim = o->vs->si.prim_mask;
    o->fs0 = (p->on >> 5 & 1)
                 ? vk_shader_get(ds, p->code[5], 0, ST_FS, 0, 0, 0, 0, o->prim, 0, 0, 0, 0, NULL, NULL, 0, 0, 0, 0)
                 : NULL;
    if (o->fs0 && !o->fs0->failed && o->fs0->si.reads_primitive_id) {
        /* the mesh translation writes no PrimitiveId, which the fragment program would read undefined */
        LOG_LIMIT(4, "vulkan: a mesh draw whose fragment program at %llx reads the primitive's number, which the mesh "
                     "program doesn't write (skipped)\n",
                  (unsigned long long)p->code[5]);
        return 0;
    }
    rt_int_targets(&ds->tgt, &o->rt_int, &o->rt_sint);
    return 1;
}
/* the shaders of a draw of host programs (DS_HOST_PROGRAM): the vertex and fragment modules as they were given,
   nothing between them and no variant of either, which their GLSL decided. 0 when the draw is skipped (said once) */
static int host_shaders(const DsState* ds, u32 topology, DrawShaders* o) {
    const DsPrograms* p = &ds->prog;
    int fs_on = p->on >> 5 & 1;
    *o = (DrawShaders){.topology = topology,
                       .window_xy = !ds->vp.transform_on,
                       .attr_int = ds->vfmt.is_int,
                       .attr_sint = ds->vfmt.is_sint};
    if ((p->on & 0x1d) || p->xfb_on || vk_viewport_swizzle_key(&ds->vp)) {
        LOG_LIMIT(4,
                  "vulkan: a draw of host programs with program slots %#x on, capture %u and a viewport swizzle "
                  "(%#x) skipped: host programs take a vertex and a fragment stage, nothing more\n",
                  (unsigned)p->on, (unsigned)p->xfb_on, (unsigned)vk_viewport_swizzle_key(&ds->vp));
        return 0;
    }
    o->vs = vk_host_shader(p->code[1], ST_VS);
    o->fs0 = fs_on ? vk_host_shader(p->code[5], ST_FS) : NULL;
    if (!o->vs || (fs_on && !o->fs0)) {
        LOG_LIMIT(4, "vulkan: a draw skipped: its programs %llx and %llx aren't host programs the renderer was given\n",
                  (unsigned long long)p->code[1], (unsigned long long)p->code[5]);
        return 0;
    }
    rt_int_targets(&ds->tgt, &o->rt_int, &o->rt_sint);
    return 1;
}
/* the shaders of the programs, the vertex format and the targets of ds for a draw of topology: 0 when the draw is
   skipped */
static int draw_shaders(const DsState* ds, u32 topology, int mesh, DrawShaders* o) {
    const DsPrograms* p = &ds->prog;
    if (mesh) return mesh_shaders(ds, o, (mesh & 2) != 0);
    if (vk_host_programs(p)) return host_shaders(ds, topology, o);
    o->mesh = 0;
    o->prim = 0;
    const DsVertexFormat* vf = &ds->vfmt;
    /* shaders: VertexB always runs (after VertexA when that is on), the fragment stage when enabled */
    u32 attr_int = vf->is_int, attr_sint = vf->is_sint, clip;
    int tcs_on, tes_on, gs_on;
    /* the stages between: tessellation control and evaluation (program slots 2 and 3), geometry (slot 4) */
    tcs_on = p->on >> 2 & 1;
    tes_on = p->on >> 3 & 1;
    /* an SM86 tessellation control program that runs no invocation is the driver's stand-in for none: the
       passthrough below takes its place (sm86_tcs_empty) */
    if (tcs_on && g_shader_sm86 && sm86_tcs_empty(gmem_r32(p->code[2] + 8))) tcs_on = 0;
    gs_on = p->on >> 4 & 1;
    if ((tcs_on || tes_on || gs_on) && g_vk_stage_sets < 5) {
        LOG_LIMIT(1, "vulkan: this GPU binds too few descriptor sets for tessellation and geometry (draw skipped)\n");
        return 0;
    }
    if (tcs_on && !tes_on) {
        LOG_LIMIT(1, "vulkan: tessellation control without an evaluation program (draw skipped)\n");
        return 0;
    }
    if ((topology == 14 && !tes_on) || (topology >= 10 && topology <= 13 && !gs_on))
        return 0;              /* patches and adjacency need the stages above */
    if (tes_on) topology = 14; /* every primitive is a patch with tessellation, as Vulkan requires */
    /* a clip distance the program writes clips only when its bit is set, in the last stage before rasterization,
       which takes its position from pixels with the viewport transform off, and applies a swizzle the viewport can't */
    o->swizzle = vk_viewport_swizzle_key(&ds->vp);
    clip = p->clip_enable | (ds->vp.transform_on ? 0 : SNX_SH_WINDOW_XY) | o->swizzle;
    /* transform feedback: what the last stage before rasterization captures */
    const ShXfb* xfb = &ds->xfb;
    int xfb_on = xfb_wanted(p->xfb_on), xfb_key = xfb_on;
    if (xfb_on && !g_vk.has_xfb && (tes_on || gs_on)) { /* xfb_write stores from the vertex stage */
        LOG_LIMIT(1,
                  "vulkan: transform feedback after tessellation or geometry isn't captured without the extension\n");
        xfb_key = 0;
    }
    o->topology = topology;
    o->xfb_on = xfb_on;
    o->window_xy = !ds->vp.transform_on;
    o->attr_int = attr_int;
    o->attr_sint = attr_sint;
    o->vs = vk_shader_get(ds, p->code[1], (p->on & 1) ? p->code[0] : 0, ST_VS, attr_int, attr_sint, 0, 0,
                          tes_on || gs_on ? SNX_SH_NOT_LAST : clip, 0, 0, 0, 0,
                          xfb_key && !tes_on && !gs_on ? xfb : NULL, NULL, 0, 0, 0, 0);
    /* a vertex program's images and integer textures pick its variant as a fragment program's do (fs_key) */
    u32 vs_signed_images = 0, vs_signed_tex = 0;
    u32 vs_images = o->vs && !o->vs->failed && o->vs->si.nimg ? image_variant(ds, 0, o->vs, &vs_signed_images) : 0;
    u64 vs_rect = 0;
    u8 vs_query[SH_MAXTEX] = {0};
    int vs_queried = 0;
    u32 vs_tex = o->vs && !o->vs->failed && o->vs->si.ntex
                     ? vs_tex_variant(ds, o->vs, &vs_signed_tex, &vs_rect, vs_query, &vs_queried)
                     : 0;
    if (vs_images || vs_signed_images || vs_tex || vs_rect || vs_queried)
        o->vs = vk_shader_get(ds, p->code[1], (p->on & 1) ? p->code[0] : 0, ST_VS, attr_int, attr_sint, vs_images << 8,
                              vs_signed_images << 8, tes_on || gs_on ? SNX_SH_NOT_LAST : clip, 0, vs_tex, vs_signed_tex,
                              vs_rect, xfb_key && !tes_on && !gs_on ? xfb : NULL, vs_queried ? vs_query : NULL, 0, 0, vs_images, vs_signed_images);
    o->tcs = tcs_on
                 ? vk_shader_get(ds, p->code[2], 0, ST_TCS, 0, 0, 0, 0, SNX_SH_NOT_LAST, 0, 0, 0, 0, NULL, NULL, 0, 0, 0, 0)
                 : NULL;
    o->tes = tes_on
                 ? vk_shader_get(ds, p->code[3], 0, ST_TES, 0, 0, p->tess_params & 0x333, 0,
                                 gs_on ? SNX_SH_NOT_LAST : clip, 0, 0, 0, 0, xfb_key && !gs_on ? xfb : NULL, NULL, 0, 0, 0, 0)
                 : NULL;
    if (tes_on && !tcs_on)
        o->tcs = vk_tess_passthrough(ds->rkey.patch_vertices ? ds->rkey.patch_vertices : 1, o->vs, o->tes);
    /* the generics the stage before writes, which an SM86 passthrough program forwards (and no others). A Maxwell
       program doesn't read them: 0 keeps its key one for each program */
    const VkSh* before = tes_on ? o->tes : o->vs;
    u32 before_out = g_shader_sm86 && before && !before->failed ? before->si.out_mask : 0;
    o->gs = gs_on ? vk_shader_get(ds, p->code[4], 0, ST_GS, 0, 0, gs_input_vertices(topology, tes_on, p->tess_params),
                                  before_out, clip, 0, 0, 0, 0, xfb_key ? xfb : NULL, NULL, 0, 0, 0, 0)
                  : NULL;
    VkSh** middle[3] = {&o->tcs, &o->tes, &o->gs};
    for (int i = 0; i < 3; i++) {
        VkSh* base = *middle[i];
        if (!base || base->failed || !base->si.nimg) continue;
        u32 signed_images;
        u32 images = image_variant(ds, i + ST_TCS, base, &signed_images);
        if (!images && !signed_images) continue;
        *middle[i] = vk_shader_get(ds, p->code[i + 2], 0, i + ST_TCS, 0, 0, base->mask[4], base->mask[5],
                                   base->mask[6], 0, base->mask[9], base->mask[10], 0,
                                   (base->mask[7] || base->mask[8]) ? &base->si.xfb : NULL, NULL, 0, 0, images,
                                   signed_images);
    }
    {
        /* the first stage that has no shader to run: the draw is left out, and the log says which program and why */
        int bad = !o->vs || o->vs->failed                 ? 1
                  : tes_on && (!o->tcs || o->tcs->failed) ? 2
                  : tes_on && (!o->tes || o->tes->failed) ? 3
                  : gs_on && (!o->gs || o->gs->failed)    ? 4
                                                          : 0;
        static const char* const names[5] = {"", "vertex", "tessellation control", "tessellation evaluation",
                                             "geometry"};
        const VkSh* s = bad == 1 ? o->vs : bad == 2 ? o->tcs : bad == 3 ? o->tes : o->gs;
        /* each stage and address once, for the first 32: a run's skips can come from one program or from many */
        static u64 said[32];
        static u32 nsaid;
        u64 key = p->code[bad] << 3 | (u64)bad;
        u32 k;
        for (k = 0; bad && k < nsaid && said[k] != key; k++) {}
        if (bad && k == nsaid && nsaid < 32) {
            said[nsaid++] = key;
            LOG("vulkan: a draw skipped: its %s program at %llx %s%s\n", names[bad], (unsigned long long)p->code[bad],
                s ? "isn't translated: " : "has no shader: out of memory",
                !s                           ? ""
                : s->si.first_unsupported[0] ? s->si.first_unsupported
                                             : "unsupported feature or float controls");
        }
        if (bad) return 0;
    }
    o->fs0 =
        (p->on >> 5 & 1) ? vk_shader_get(ds, p->code[5], 0, ST_FS, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0, 0, 0) : NULL;
    rt_int_targets(&ds->tgt, &o->rt_int, &o->rt_sint);
    return 1;
}
/* The varying pass of a draw with shaders sh (ShaderInfo.varying, the fragment program's key): none where the host
   interpolates as the console does (NVIDIA's driver, which Eden leaves alone too). A pass needs the fragment program
   straight after the vertex program, whose position is the rasterizer's as written (no swizzle the program applies,
   the viewport transform on) in viewport 0, and triangles drawn filled. The per-vertex pass (per_vertex: the draw may
   take it) is for a draw of quads, the game's own topology, that culls no face, and captures nothing: the huge
   clipped primitives one game draws its sea of clouds and its sky with. Eden's rule (PR 4575, vk_pipeline_cache.cpp):
   on every draw the pass cost two other games 4 and 7 points of GPU time for nothing visible. The position goes in
   the generic after the last one the two programs use, and the position inputs stay the position pass's. Every other
   draw takes the position pass, and so does a quad draw the device can do no better for */
static u32 draw_varying(const DsState* ds, const DrawShaders* sh, int per_vertex) {
    const VkSh *vs = sh->vs, *fs = sh->fs0;
    if (g_vk.driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY || sh->mesh || sh->tcs || sh->tes || sh->gs || !fs ||
        vk_host_programs(&ds->prog)) /* a host program interpolates as its GLSL asks, as the device does */
        return 0;
    if (vs->si.writes_viewport || sh->window_xy || sh->swizzle || sh->topology < 4 || sh->topology > 9 ||
        ds->rkey.polygon != DS_POLYGON_FILL)
        return 0;
    u32 used = vs->si.out_mask | fs->si.in_mask, smooth = vs->si.out_mask & fs->si.smooth_in_mask;
    u32 positions = vs->si.pos_out_mask & fs->si.smooth_in_mask, slot = 32 - snx_clz32(used);
    if (per_vertex && (sh->topology == 7 || sh->topology == 8) && ds->dyn.cull == DS_CULL_NONE && !sh->xfb_on &&
        (smooth & ~positions) && !(used >> 31)) {
        if (g_vk.has_barycentric) return shader_varying_per_vertex(SH_VARYING_BARYCENTRIC, slot, positions);
        /* without them a geometry program hands the values over (vk_varying_geometry), which passes on a position
           and generics alone, and needs free generics for its copies and a descriptor set of its stage */
        u32 key = shader_varying_per_vertex(SH_VARYING_GEOMETRY, slot, positions);
        u32 taken = smooth & ~shader_varying_position_mask(key);
        if (g_vk.feat.geometryShader && g_vk_stage_sets >= 5 && !vs->si.writes_layer && !vs->si.writes_point_size &&
            !vs->si.uses_clip_mask && shader_varying_copy(taken, slot, 32, 0) <= 32)
            return key;
    }
    return shader_varying_positions(positions);
}
/* the vertex program vs of ds's draw in the variant that also writes its position to the generic slot, for a
   per-vertex pass: vs's own key, as draw_shaders gave it (no capture: draw_varying), and the slot */
static VkSh* vs_position_copy(const DsState* ds, const VkSh* vs, u32 slot) {
    const DsPrograms* p = &ds->prog;
    u8 query[SH_MAXTEX];
    int queried = 0;
    shader_query_hint_unpack(vs->mask + 14, query);
    for (int i = 0; i < SH_MAXTEX; i++) queried |= query[i] != 0;
    return vk_shader_get_ex(ds, p->code[1], (p->on & 1) ? p->code[0] : 0, ST_VS, vs->mask[0] | (u64)vs->mask[1] << 32,
                            vs->mask[2] | (u64)vs->mask[3] << 32, vs->mask[4], vs->mask[5], vs->mask[6], 0, vs->mask[9],
                            vs->mask[10], vs->mask[12] | (u64)vs->mask[13] << 32, NULL, queried ? query : NULL, 0, 0,
                            vs->mask[24] & 255, vs->mask[25], 1 + slot, 0, 0, 0);
}
/* stream s's address and bytes (to its limit, 0 when the limit is below) */
static u64 stream_start(const DsVertexBuffers* b, u32 s) { return b->addr[s]; }
static u64 stream_len(const DsVertexBuffers* b, u32 s) { return b->size[s]; }
/* the attributes whose offsets start outside their streams */
static u32 attributes_empty(const DsVertexBuffers* b, const DsVertexFormat* vf, u32 inputs) {
    u32 e = 0;
    while (inputs) {
        u32 i = snx_ctz32(inputs);
        inputs &= inputs - 1;
        if (stream_len(b, vf->stream[i]) <= vf->offset[i]) e |= 1u << i;
    }
    return e;
}
static void target_formats(Surf* const* rt, const Surf* zt, u32 n, TargetFormats* tf) {
    *tf = (TargetFormats){.n = n};
    u32 samples = ~0u;
    for (u32 i = 0; i < n; i++)
        if (rt[i]) {
            tf->present |= 1u << i;
            tf->is_int |= (rt[i]->f.is_int ? 1u : 0u) << i;
            tf->color[i] = rt[i]->f.vk;
            samples = samples == ~0u || samples == vk_surf_samples(rt[i]) ? vk_surf_samples(rt[i]) : 0;
        }
    if (zt) {
        tf->depth = zt->f.vk;
        tf->stencil = zt->f.stencil ? 1 : 0;
        tf->depth_samples = vk_surf_samples(zt);
        if (samples == ~0u) samples = tf->depth_samples;
    }
    tf->samples = samples == ~0u ? VK_SAMPLE_COUNT_1_BIT : samples;
}
/* the topology class of a pipeline drawing topo: adjacency and patches are their own, as the geometry program's input
   primitive is one of them */
static u32 topology_class(VkPrimitiveTopology topo) {
    return topo >= VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ? topo
           : topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST             ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
           : topo <= VK_PRIMITIVE_TOPOLOGY_LINE_STRIP             ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                  : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}
/* the polygon mode a pipeline takes for rk's: fill where the device has no other (draw_edges draws the others) */
static u32 polygon_mode(const DsRasterKey* rk) {
    return g_vk.feat.fillModeNonSolid ? rk->polygon : VK_POLYGON_MODE_FILL;
}
/* the size codes a vertex program fetches (sm86_translate.c's g_fetch_sizes) */
static int fetch_size_known(u32 size) {
    switch (size) {
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x0A:
        case 0x0F:
        case 0x12:
        case 0x13:
        case 0x18:
        case 0x1B:
        case 0x1D:
        case 0x30:
        case 0x31: return 1;
    }
    return 0;
}
/* The vertex plan of a vertex program that fetches its own attributes (ShaderInfo.vertex_fetch): a binding for each
   stream with data, whose lookup gives the addresses of the attributes in it (fetch_records), and each attribute's
   format word (SH_ATTR_). It needs no vertex input state, no binding of its own for an offset or a stride the device
   can't fetch, and no restride: the program reads any byte, and also the 32-bit scaled formats that Vulkan doesn't
   have. 0 when the draw is skipped */
static int fetch_plan(const DsVertexFormat* vf, u32 in_mask, VertexPlan* pl) {
    VertexLayout* vi = &pl->vi;
    for (u32 i = 0; i < 32; i++) {
        if (!(in_mask >> i & 1)) continue;
        const u32 sbuf = vf->stream[i], off = vf->offset[i], size = vf->size[i];
        u32 ty = vf->type[i];
        const int known = fetch_size_known(size) && ty >= 1 && ty <= 7 && !(size == 0x31 && (ty == 3 || ty == 4));
        if ((vf->constant >> i & 1) || !(vf->stream_on >> sbuf & 1) || !known) {
            pl->fetch[i] = SH_ATTR_CONSTANT; /* 0, 0, 0, 1 */
            continue;
        }
        const u32 stride = vf->stride[sbuf], asz = attr_bytes(size);
        if (stride > SH_ATTR_STRIDE_MASK) {
            LOG_LIMIT(1, "vulkan: a vertex stream with stride %u can't be fetched (draw skipped)\n", stride);
            return 0;
        }
        u32 b = 0;
        while (b < vi->nbind && pl->key[b] != sbuf) b++;
        if (b == vi->nbind) {
            /* the divisor whole, in the record's word of its own (SH_RECORD_DIVISOR) */
            const u32 div = (vf->instanced >> sbuf) & 1 ? (vf->divisor[sbuf] ? vf->divisor[sbuf] : 1) : 0;
            pl->key[b] = sbuf; /* from the stream's start: an attribute's offset is its own */
            pl->stride[b] = stride;
            vi->bind[b].divisor = div;
            vi->nbind++;
        }
        /* the stream's limit covers its last vertex's stride: an attribute reaching past the stride reads past it */
        const u32 pad = stride && off + asz > stride ? off + asz - stride : 0;
        if (pad > pl->pad[b]) pl->pad[b] = pad;
        pl->used |= 1u << sbuf;
        vi->attr[vi->nattr].loc = i;
        vi->attr[vi->nattr].binding = b;
        vi->attr[vi->nattr].format = 0;
        vi->attr[vi->nattr++].offset = off;
        /* float 8-bit and packed formats are unorm, as attr_format gives them, and B10G11R11 is a float of any type */
        if (ty == 7 && (attr_component_bytes(size) == 1 || size == 0x30)) ty = 2;
        if (size == 0x31) ty = 7;
        const u32 div = vi->bind[b].divisor;
        pl->fetch[i] = stride | size << SH_ATTR_SIZE_SHIFT | ty << SH_ATTR_TYPE_SHIFT |
                       (size == 0x0A && ty == 2 && (vf->bgra >> i & 1) ? SH_ATTR_BGRA : 0) |
                       (div ? SH_ATTR_INSTANCED : 0);
        pl->divisor[i] = div;
    }
    return 1;
}
/* the pipeline key of the shaders (fs the fragment variant drawn), the state ds and the targets' surfaces rt and zt, all but the feedback loop, and the vertex bindings it
   describes: 0 when the draw is skipped */
static int draw_key(const DsState* ds, const DrawShaders* sh, const VkSh* fs, Surf* const* rt, const Surf* zt, u32 n,
                    PipeKey* k, VertexPlan* pl) {
    const DsVertexBuffers* vb = &ds->vbuf;
    const DsVertexFormat* vf = &ds->vfmt;
    const DsBlend* bl = &ds->blend;
    const DsRasterKey* rk = &ds->rkey;
    const DsTargets* tg = &ds->tgt;
    const VkSh* vs = sh->vs;
    u32 topology = sh->topology;
    memset(k, 0, sizeof *k);
    k->heaps = (u32)g_vk.has_descriptor_heaps;
    memset(pl, 0, sizeof *pl);
    k->vs = vs->mod;
    k->fs = fs ? fs->mod : VK_NULL_HANDLE;
    /* with VK_EXT_depth_clip_enable, the clamp and the clip as the console has them apart; without it one switch,
       where clamping turns clipping off: a fragment's depth past the viewport's range is undefined then with the clamp
       off (the specification's Depth Clamping and Range Adjustment), which the console clamps to 0 to 1 */
    int clamp = g_vk.has_depth_clip_enable ? rk->depth_pixel_clamp : rk->depth_clamp;
    k->depth_clamp = g_vk.feat.depthClamp && clamp;
    if (g_vk.has_depth_clip_enable) k->depth_clip = rk->depth_no_clip ? 1 : 2;
    if (!g_vk.feat.depthClamp && clamp) LOG_LIMIT(1, "vulkan: depth clamp requested but unavailable on this device\n");
    k->provoking_last = rk->provoking_last && g_vk.has_provoking_last;
    if (rk->provoking_last && !g_vk.has_provoking_last)
        LOG_LIMIT(1, "vulkan: flat inputs take the first vertex of a primitive, not the last the game asks for\n");
    if (bl->logic_op) {
        if (g_vk.feat.logicOp)
            k->logic_op = bl->logic_op;
        else
            LOG_LIMIT(1, "vulkan: this GPU has no logic operations: targets are blended instead\n");
    }
    k->topo_class = topology_class(topology_vk(topology));
    k->tcs = sh->tcs ? sh->tcs->mod : VK_NULL_HANDLE;
    k->tes = sh->tes ? sh->tes->mod : VK_NULL_HANDLE;
    if (sh->mesh) { /* no input assembly: the mesh program's triangles, and no vertex input (it reads none) */
        k->topo_class = 0;
        /* the compute-task path: the task program is a dispatch before the draw, so the pipeline has no task stage */
        if (sh->task_compute) k->tcs = VK_NULL_HANDLE;
        k->mesh = 1u | (vs->si.mesh.threads % 32 ? 0 : PIPE_MESH_WHOLE(ST_MS)) |
                  (k->tcs && !(sh->tcs->si.mesh.threads % 32) ? PIPE_MESH_WHOLE(ST_TS) : 0) |
                  (sh->task_compute ? PIPE_MESH_TASK_COMPUTE : 0);
    }
    k->gs = sh->gs ? sh->gs->mod : VK_NULL_HANDLE;
    {
        const VkSh* stages[5] = {vs, fs, sh->tcs, sh->tes, sh->gs};
        for (int i = 0; i < 5; i++) {
            if (stages[i] && stages[i]->indexed) k->arrays |= 1u << shader_vk_set(stages[i]->stage);
            if (stages[i] && k->heaps) k->cb_addr[shader_vk_set(stages[i]->stage)] = vk_cb_addr_mask(&stages[i]->si);
        }
    }
    if (sh->tes) { /* the control points of a patch */
        k->patch = rk->patch_vertices ? rk->patch_vertices : 1;
        if (k->patch > g_vk.props.limits.maxTessellationPatchSize)
            k->patch = g_vk.props.limits.maxTessellationPatchSize;
    }
    k->polygon = polygon_mode(rk);

    /* vertex streams: each used guest stream is a binding, and so is an attribute at an offset past the device's
       limit or reaching past its stream's stride (it is read from its own offset there); one more holds the 0,0,0,1
       of attributes without data (an attribute marked constant, a stream disabled, or a format the device
       can't read) */
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    u32 maxoff = lim->maxVertexInputAttributeOffset;
    u32 nloc = lim->maxVertexInputAttributes < 32 ? lim->maxVertexInputAttributes : 32;
    u32 maxbind = lim->maxVertexInputBindings < 33 ? lim->maxVertexInputBindings : 33;
    /* a vertex program that fetches its own attributes: the plan's bindings only find the streams (fetch_plan) */
    const int fetch = vs->si.vertex_fetch && !sh->mesh;
    k->fetch = (u32)fetch;
    if (!fetch && nloc < 32 && (vs->si.in_mask >> nloc)) {
        LOG_LIMIT(1, "vulkan: a vertex shader reads attributes past the %u this GPU has (draw skipped)\n", nloc);
        return 0;
    }
    int cbind = -1, zbind = -1, over = 0;
    u32 align[33] = {0}; /* each binding's largest attribute component */
    VertexLayout* vi = &pl->vi;
    pl->empty = attributes_empty(vb, vf, vs->si.in_mask);
    if (fetch && !fetch_plan(vf, vs->si.in_mask, pl)) return 0;
    for (u32 i = 0; i < nloc && !over && !fetch; i++) {
        if (!(vs->si.in_mask & (1u << i))) continue;
        u32 sbuf = vf->stream[i], off = vf->offset[i], size = vf->size[i], ty = vf->type[i];
        VkFormat fmt = attr_format(size, ty, (vf->bgra >> i) & 1);
        int is_int = ty == 3 || ty == 4,
            data = !((vf->constant >> i) & 1) && ((vf->stream_on >> sbuf) & 1) && vertex_format_ok(fmt);
        if (data) { pl->used |= 1u << sbuf; }
        if (data && (pl->empty & (1u << i))) {
            if (zbind < 0) {
                if (vi->nbind == maxbind) {
                    over = 1;
                    continue;
                }
                pl->key[vi->nbind] = ~1u;
                zbind = (int)vi->nbind++;
            }
            vi->attr[vi->nattr].loc = i;
            vi->attr[vi->nattr].binding = (u32)zbind;
            vi->attr[vi->nattr].format = fmt;
            vi->attr[vi->nattr++].offset = 0;
            continue;
        }
        if (!data) {
            if (cbind < 0) {
                if (vi->nbind == maxbind) {
                    over = 1;
                    continue;
                }
                pl->key[vi->nbind] = ~0u;
                cbind = (int)vi->nbind;
                vi->nbind++;
            }
            vi->attr[vi->nattr].loc = i;
            vi->attr[vi->nattr].binding = (u32)cbind;
            vi->attr[vi->nattr].format = is_int ? (ty == 3 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                                                : VK_FORMAT_R32G32B32A32_SFLOAT;
            vi->attr[vi->nattr].offset = is_int ? 16 : 0;
            vi->nattr++;
            continue;
        }
        u32 stride = vf->stride[sbuf], asz = attr_bytes(size);
        /* an attribute may be wider than its stream's stride (a game may read 8-byte attributes from
           6-byte strides, its world geometry): Maxwell reads the rest from the next vertex, and so does Vulkan,
           which allows it outside the portability subset (vertexAttributeAccessBeyondStride) */
        if (stride > lim->maxVertexInputBindingStride) {
            LOG_LIMIT(1, "vulkan: a vertex stream with stride %u can't be read (draw skipped)\n", stride);
            return 0;
        }
        /* an attribute past the stride, or past the offsets the GPU allows, gets a binding of its own starting at it */
        u32 base = off > maxoff || (stride && off + asz > stride) ? off : 0;
        u32 key = sbuf | base << 5, b = 0;
        while (b < vi->nbind && pl->key[b] != key) b++;
        if (b == vi->nbind) {
            if (b == maxbind) {
                over = 1;
                continue;
            }
            /* the stream's limit covers its last vertex's stride: an attribute reaching past the stride reads past it */
            pl->pad[b] = stride && base + asz > stride ? base + asz - stride : 0;
            pl->stride[b] = stride;
            pl->key[b] = key;
            u32 div = (vf->instanced >> sbuf) & 1 ? (vf->divisor[sbuf] ? vf->divisor[sbuf] : 1) : 0;
            if (div > 1 && !g_vk.has_divisor) div = 1;
            vi->bind[b].divisor = div;
            vi->bind[b].stride = g_vk.has_dynamic_stride ? 0 : stride;
            vi->nbind++;
        }
        vi->attr[vi->nattr].loc = i;
        vi->attr[vi->nattr].binding = b;
        vi->attr[vi->nattr].format = fmt;
        vi->attr[vi->nattr].offset = off - base;
        vi->nattr++;
        if (attr_component_bytes(size) > align[b]) align[b] = attr_component_bytes(size);
    }
    if (over) {
        LOG_LIMIT(1, "vulkan: a draw needs more vertex bindings than the %u this GPU has (draw skipped)\n", maxbind);
        return 0;
    }
    /* Maxwell reads an attribute at any byte, Vulkan only at a multiple of its components' size, which a stride like
       one game's 69 for 4-byte floats breaks every other vertex: RADV's fetches hung the GPU on them */
    for (u32 b = 0; b < vi->nbind; b++) {
        u32 st = (u32)pl->stride[b], a = align[b];
        if (!st || a < 2 || !(st % a)) continue;
        pl->guest_stride[b] = st;
        pl->stride[b] = (st + 3) & ~3u; /* a multiple of 2 or 4, and of the words restride writes */
        if (!g_vk.has_dynamic_stride) vi->bind[b].stride = (u32)pl->stride[b];
    }
    if (!g_vk.vertex_input_dynamic && !fetch) k->vi = *vi;

    /* attachments and blending */
    k->ncolor = n;
    for (u32 i = 0; i < n; i++) {
        if (!rt[i]) continue; /* format UNDEFINED, nothing written */
        k->color[i] = tg->srgb_write ? rt[i]->f.vk : vk_srgb_as(rt[i]->f.vk, 0);
        k->blend[i].mask = bl->mask[i];
        if (bl->en[i] && !rt[i]->f.is_int) {
            k->blend[i].en = 1;
            k->blend[i].cop = bl->cop[i];
            k->blend[i].sc = bl->sc[i];
            k->blend[i].dc = bl->dc[i];
            k->blend[i].aop = bl->aop[i];
            k->blend[i].sa = bl->sa[i];
            k->blend[i].da = bl->da[i];
        }
    }
    if (zt) {
        k->depth = zt->f.vk;
        k->stencil = zt->f.stencil ? zt->f.vk : VK_FORMAT_UNDEFINED;
    }
    /* Color attachments share a count. The coverage key already validated a separate depth/raster count. */
    TargetFormats tf;
    target_formats(rt, zt, n, &tf);
    if (!tf.samples) {
        LOG_LIMIT(1, "vulkan: a draw's targets have different sample counts (skipped)\n");
        return 0;
    }
    k->samples = tf.samples;
    k->sample_off = ~rk->sample_mask & ((1u << k->samples) - 1);
    if (k->samples > 1) {
        k->alpha_to_coverage = rk->alpha_to_coverage && n && rt[0];
        k->alpha_to_one = rk->alpha_to_one && n && rt[0] && g_vk.feat.alphaToOne;
        k->sample_shading = rk->sample_shading && g_vk.feat.sampleRateShading;
        if (rk->sample_shading && !g_vk.feat.sampleRateShading)
            LOG_LIMIT(1,
                      "vulkan: this GPU has no sample shading: a program asked for per sample is shaded per pixel\n");
    }
    if (rk->conservative && g_vk.has_conservative) {
        k->conservative = rk->conservative;
        u8 bit = (u8)(1u << ((rk->conservative - 1) & 3));
        if (rk->conservative > 1 && !(g_vk.conservative_said & bit)) {
            char line[256];
            g_vk.conservative_said |= bit;
            vk_conservative_dilation(rk->conservative, g_vk.conservative_extra, line, sizeof line);
            if (line[0]) LOG("%s\n", line);
        }
    } else if (rk->conservative)
        LOG_LIMIT(1,
                  "vulkan: this GPU has no conservative rasterization: such draws cover only the pixels they center\n");
    return 1;
}

/* A range's highest index is kept until its memory is written (by the GPU or, through the write tracking, the CPU):
   scanning the indices of every draw was a tenth of one game's GPU thread, most of them meshes' that never change.
   The baselines are taken before the bytes are read, as buf_get takes them. One game reads about 1,900 index ranges a
   frame: 256 entries answered 55% of them, and scanning the rest again was a twentieth of a draw's setup. A range
   goes in a set of four by its address, the latest used first: with an entry for each address, two ranges of one
   slot put each other out, and a town scene of another game with 1,500 ranges a frame scanned 133,000 indices a
   frame for the fifth it missed. Four ways missed 3.5% and scanned 16,500. An entry's key is the range's address
   with its index size in the top byte (GPU addresses have 40 bits), its length and its restart index */
typedef struct {
    u64 key, gw;
    u32 n, ws, ri, top;
} TopMemo;
static TopMemo g_top_memo[1024][4];
static u32 top_memo_set(u64 a) { return (u32)((a * 0x9E3779B97F4A7C15ull) >> 54); }
/* A draw's entry above and its indices' range in the buffer cache, fetched when the draw starts: each was a cache miss
   in the middle of its setup, the entry above the most frequent last-level miss in vk_render_draw's own code in one
   game */
static void draw_indices_prefetch(const DsIndex* ix, int indexed, u32 first, u32 count, int step) {
    u32 ifmt = ix->format, isz = ifmt == 0 ? 1 : ifmt == 1 ? 2 : 4;
    u64 a = ix->addr + (u64)first * isz;
    if (indexed != 1 && indexed != 2) return;
    if (!step) {
        __builtin_prefetch(&g_top_memo[top_memo_set(a)][0]);
        __builtin_prefetch(&g_top_memo[top_memo_set(a)][3]);
    }
    vk_buf_prefetch(a, (u64)count * isz, step);
}
/* The last vertex a draw reads, for its streams' ranges, or ~0 when that isn't known: indices outside contiguous guest
   memory, or under results the GPU hasn't landed. A stream's limit is often the end of a ring the game streams
   vertices through, so its range from the draw's start to the limit was megabytes for a few hundred vertices: one
   game copied 100 to 360 MB a frame that way */
/* the bytes up to which a vertex stream binds whole, with no last vertex (render_draw) */
#define VB_WHOLE (1u << 20)
static u64 draw_last_vertex(const u32* inline_indices, const DsIndex* ix, int indexed, u32 first, u32 count,
                            u32 base_vertex) {
    u64 top = 0;
    if (!indexed) return (u64)first + count - 1;
    if (indexed == 3) { /* inline indices, 32-bit */
        for (u32 j = 0; j < count; j++)
            if (inline_indices[first + j] > top) top = inline_indices[first + j];
    } else {
        u32 ifmt = ix->format, isz = ifmt == 0 ? 1 : ifmt == 1 ? 2 : 4;
        u64 ia = ix->addr, iend = ix->end;
        if (iend < ia || first >= (iend - ia + 1) / isz) return ~0ull;
        u64 n = (iend - ia + 1) / isz - first, a = ia + (u64)first * isz;
        VkBuffer tb;
        u64 to;
        if (n > count) n = count;
        /* indices the GPU wrote and holds aren't read here either: landing them takes a submit and a wait for the GPU,
           and the draw reads them where their owner holds them (buffer.c buf_get_owned) */
        if (vk_buf_pending(a, n * isz) || vk_uma_pending(a, n * isz) || vk_compute_gpu_src(a, n * isz, &tb, &to) ||
            vk_buf_owned(a, n * isz))
            return ~0ull;
        vk_compute_mem_need(a, n * isz);
        /* the restart index, and all ones where Vulkan restarts instead, mark no vertex: with the restart off its
           index is all ones too */
        u32 ones = isz == 1   ? 0xFFu
                   : isz == 2 ? 0xFFFFu
                              : 0xFFFFFFFFu,
            ri = ix->restart ? ix->restart_index & ones : ones, skip = ones;
        TopMemo* set = g_top_memo[top_memo_set(a)];
        u64 key = a | (u64)isz << 56, gw = gpu_mem_generation();
        u32 way = 0, ws = vk_ww_now();
        const u8* p = gmem_ptr(a, n * isz);
        if (!p) return ~0ull;
        while (way < 3 && !(set[way].key == key && set[way].n == n && set[way].ri == ri)) way++;
        int hit = set[way].key == key && set[way].n == n && set[way].ri == ri &&
                  !gpu_mem_dirty_since(set[way].gw, a, n * isz) && vk_written_since(p, n * isz, set[way].ws) == 0;
        TopMemo e = set[way];
        for (; way > 0; way--) set[way] = set[way - 1]; /* the latest used first, the last way or the stale one out */
        if (hit)
            set[0] = e;
        else {
            /* a loop for each size, in signed arithmetic where the indices fit it, which SSE2 makes vector maxima of */
            if (isz == 4) {
                u32 m = 0;
                for (u64 j = 0; j < n; j++) {
                    u32 v = snx_ld_le32(p + j * 4);
                    v = v == ri || v == skip ? 0 : v;
                    m = v > m ? v : m;
                }
                top = m;
            } else {
                i32 m = 0, sri = (i32)ri, sskip = (i32)skip;
                if (isz == 1)
                    for (u64 j = 0; j < n; j++) {
                        i32 v = p[j];
                        v = v == sri || v == sskip ? 0 : v;
                        m = v > m ? v : m;
                    }
                else
                    for (u64 j = 0; j < n; j++) {
                        i32 v = snx_ld_le16(p + j * 2);
                        v = v == sri || v == sskip ? 0 : v;
                        m = v > m ? v : m;
                    }
                top = (u32)m;
            }
            set[0] = (TopMemo){.key = key, .gw = gw, .n = (u32)n, .ws = ws, .ri = ri, .top = (u32)top};
        }
        top = set[0].top;
    }
    i64 last = (i64)top + (i32)base_vertex;
    return last < 0 ? 0 : (u64)last;
}

/* Index buffers Vulkan can't read as they are, made on the GPU (texconv's TC_INDEX_* modes) in this frame's output
   ring: quads as triangles, 8-bit indices without VK_KHR_index_type_uint8, and a restart index other than all ones.
   The CPU made them for each draw before, reading guest memory an index at a time, and the restart rewrite read the
   output ring back, which can be device memory behind the BAR. Quads and quad strips of vertex numbers take one
   buffer for every draw of each kind and provoking vertex (QUADS_ flags, quad_indices.h's order), made once for each
   size it grows to, drawn from the draw's first vertex */
enum { QUADS_LAST = 1, QUADS_STRIP = 2 }; /* TC_INDEX_QUADS's const_a */
static struct {
    VkBuffer buf;
    DevMem mem;
    u32 quads; /* how many quads it has the indices of */
} g_quads[4];
/* what the GPU wrote (src: the stage and access) before what reads it next (dst) */
static void index_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                          VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = src_stage,
                          .srcAccessMask = src_access,
                          .dstStageMask = dst_stage,
                          .dstAccessMask = dst_access};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}
/* the len bytes of a vertex stream at gva, its vertices gstride apart, made on the GPU into this frame's output ring
   with them stride apart (a multiple of 4) instead: each vertex's stride bytes, so an attribute reaching past the
   guest's stride still reads the next vertex's first bytes, and 0 past the stream. The stream comes as the guest
   range's buffer (vk_buf_get), a GPU result that hasn't landed read where it is. Its words are bound whole: the range
   asked for starts and ends on them, which never takes in another page. The CPU made the copy before, a vertex at a
   time from a whole read of the stream, each draw */
/* The records of a vertex program that fetches its own attributes (ShaderInfo.vertex_fetch, SH_RECORD_ATTR): each
   attribute's address of its first element in the buffer its stream's lookup found, the bytes from there to the end
   of what the lookup bound (0 for none: the program reads zeros there), and its format word from the plan */
static void fetch_records(const VertexPlan* pl, const VkBuffer* vbuf, const VkDeviceSize* voff,
                          const VkDeviceSize* vsize, u32 rec[32][4], u32 divisor[32]) {
    u64 base[33];
    for (u32 b = 0; b < pl->vi.nbind; b++) base[b] = vbuf[b] && vsize[b] ? vk_buffer_address(vbuf[b]) + voff[b] : 0;
    for (u32 i = 0; i < 32; i++) {
        rec[i][0] = rec[i][1] = rec[i][2] = 0;
        rec[i][3] = pl->fetch[i];
        divisor[i] = pl->divisor[i];
    }
    for (u32 a = 0; a < pl->vi.nattr; a++) {
        const u32 loc = pl->vi.attr[a].loc, b = pl->vi.attr[a].binding, off = pl->vi.attr[a].offset;
        if (!base[b] || vsize[b] <= off) continue;
        const u64 at = base[b] + off, bytes = vsize[b] - off;
        rec[loc][0] = (u32)at;
        rec[loc][1] = (u32)(at >> 32);
        rec[loc][2] = bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)bytes;
    }
}
static int restride(u64 gva, u64 len, u32 gstride, u32 stride, VkBuffer* buf, VkDeviceSize* off, VkDeviceSize* size) {
    u64 nv = (len + gstride - 1) / gstride, lo = gva & ~3ull, hi = (gva + len + 3) & ~3ull;
    VkBuffer src;
    u64 at;
    if (nv > 0xFFFFFFFFu || !vk_buf_get_input(lo, hi - lo, 4, &src, &at)) return 0;
    if (!vk_stage_output(nv * stride, vk_stage_align(), buf, off, NULL)) return 0;
    vk_render_end(); /* dispatches happen outside rendering passes */
    VkCommandBuffer cb = vk_cmd();
    if (!vk_tc_restride(cb, gstride, stride, (u32)nv, src, at + (gva - lo), len, *buf, *off)) return 0;
    index_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
    *size = nv * stride;
    return 1;
}
static int quad_indices(u32 nq, u32 flags, VkBuffer* buf, u64* off) {
    if (nq > g_quads[flags].quads) {
        u32 cap = g_quads[flags].quads ? g_quads[flags].quads : 16384;
        while (cap < nq) cap *= 2;
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = (u64)cap * 24,
                                  .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        DevMemWant aci = {.at = DM_DEVICE};
        VkBuffer b;
        DevMem m;
        if (!VK_OK(vk_buffer_create(&bci, &aci, &b, &m, NULL))) return 0;
        vk_mem_track(m, VK_OWN_XFB);
        vk_render_end(); /* dispatches happen outside rendering passes */
        VkCommandBuffer cb = vk_cmd();
        /* the input is bound but not read: vertex numbers are their own indices */
        if (!vk_tc_index(cb, TC_INDEX_QUADS, 0, flags, cap, b, 0, 0, b, 0, (u64)cap * 24)) {
            vk_defer_buffer(b, m);
            return 0;
        }
        /* every later draw reads it, in this submission and the ones after (a barrier's second scope) */
        index_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT);
        if (g_quads[flags].buf) /* earlier draws of this frame still read it */
            vk_defer_buffer(g_quads[flags].buf, g_quads[flags].mem);
        g_quads[flags].buf = b;
        g_quads[flags].mem = m;
        g_quads[flags].quads = cap;
    }
    *buf = g_quads[flags].buf;
    *off = 0;
    return 1;
}
/* An indexed draw's count indices drawn as quads (flags QUADS_) made into triangles here, into the staging ring, where
   the CPU has the indices: the draw's inline ones, or the guest's in one run of guest memory with no pending result
   over it and no part the GPU holds (reading those would wait for the GPU). The GPU's conversion is a dispatch, outside
   the rendering pass, so each such draw ended its pass: 235 a frame in one game's scene. Here alone a restart index
   (restart on, ri) ends a quad as the 3D engine does: the GPU's quads stay where they were. The quads made, or -1
   when the CPU hasn't the indices, or they're past QUAD_CPU_BYTES: the GPU makes them then */
#define QUAD_CPU_BYTES 16384
static int quad_cpu_reads(int inline_indices, u64 ia, u32 isz, u32 first, u32 count) {
    u64 len = (u64)count * (inline_indices ? 4 : isz), gva = ia + (u64)first * isz, cpu = 0; /* inline: host words */
    OwnRange held;
    if (!count || len > QUAD_CPU_BYTES) return 0;
    return inline_indices || !(vk_buf_pending(gva, len) || vk_uma_pending(gva, len) || vk_compute_pending(gva, len) ||
                               gmmu_run(gva, len, &cpu) < len || own_lookup(cpu, len, &held, 1));
}
static i64 quad_indices_cpu(const DrawCall* dc, int inline_indices, u64 ia, u32 isz, u32 first, u32 count, u32 flags,
                            int restart, u32 ri, VkBuffer* buf, u64* off) {
    u32 esz = inline_indices ? 4 : isz, nq = snx_quads_of(count, (flags & QUADS_STRIP) != 0);
    u8 in[QUAD_CPU_BYTES];
    if (!nq) return 0;
    if (!quad_cpu_reads(inline_indices, ia, isz, first, count)) return -1;
    if (inline_indices)
        for (u32 j = 0; j < count; j++) snx_st_le32(in + 4 * (size_t)j, dc->inline_indices[first + j]);
    else
        gmem_read_raw(ia + (u64)first * isz, in, (u64)count * esz);
    u8* out = vk_stage((u64)nq * 24, vk_stage_align(), buf, off);
    if (!out) return -1;
    return snx_quad_indices(in, esz, count, (flags & QUADS_STRIP) != 0, flags & QUADS_LAST, restart, ri, out);
}
/* Whether a draw goes as a list of lines or points made of its primitives (polygon_edges.h, edge_indices) instead of
   as Vulkan draws its topology: a line loop, which Vulkan has none of; polygons in the polygon mode Line or Point
   where the device has no mode but fill (fillModeNonSolid); and on every device quads, quad strips and polygons drawn
   as lines, whose outline the device's mode would cross with the diagonals of the triangles they become. Lines and
   points take no face culling and no depth bias: where the device has the mode, a draw that asks for either keeps
   it, diagonals and all, and elsewhere the log says the list takes neither. Only where the vertex program's
   primitives are the ones drawn (no stage after it, no mesh program; a geometry program takes a loop's sides as the
   lines they are), none are captured (transform feedback takes the primitives as they were) and the draw is direct
   (the CPU makes the list: an indirect draw's count is the GPU's); the others are drawn as before, and logged */
static int draw_edges(const DsState* ds, const DrawCall* dc, const DrawShaders* sh) {
    u32 t = sh->topology, mode = ds->rkey.polygon;
    int after = sh->tcs || sh->tes || sh->gs || sh->mesh, native = g_vk.feat.fillModeNonSolid != 0;
    int outline = !after && t >= 7 && t <= 9 && mode == DS_POLYGON_LINE;
    if (t != 2 && (mode == DS_POLYGON_FILL || (native && !outline) || (!after && (t < 4 || t > 9))))
        return 0; /* filled, or drawn by the device's mode, or points and lines, which take no polygon mode */
    const char *name = mode == DS_POLYGON_LINE ? "Line" : "Point", *as = mode == DS_POLYGON_LINE ? "lines" : "points";
    const char* what = t == 7 ? "quads" : t == 8 ? "a quad strip" : "a polygon";
    if (t == 2) { /* a geometry program takes its sides as the lines they are */
        if (!sh->xfb_on && !dc->indirect) return 1;
        LOG_LIMIT(1, "vulkan: a line loop %s is drawn open, without the side from its last vertex to its first\n",
                  sh->xfb_on ? "captured by transform feedback" : "of an indirect draw");
        return 0;
    }
    if (after) {
        LOG_LIMIT(1,
                  "vulkan: the device has no polygon mode %s (fillModeNonSolid): the polygons of a draw with "
                  "tessellation, geometry or a mesh program are drawn filled\n",
                  name);
        return 0;
    }
    if (sh->xfb_on || dc->indirect) {
        const char* why = sh->xfb_on ? "and captured by transform feedback" : "by an indirect draw";
        if (native)
            LOG_LIMIT(1, "vulkan: %s drawn as lines %s show the diagonals of the triangles they are drawn as\n", what,
                      why);
        else
            LOG_LIMIT(1,
                      "vulkan: the device has no polygon mode %s (fillModeNonSolid): polygons drawn as %s %s are drawn "
                      "filled\n",
                      name, as, why);
        return 0;
    }
    int culled = ds->dyn.cull != DS_CULL_NONE, biased = ds->dyn.bias_on >> (mode == DS_POLYGON_POINT ? 0 : 1) & 1;
    const char* taken = culled && biased ? "face culling and no depth bias" : culled ? "face culling" : "depth bias";
    if ((culled || biased) && native) {
        LOG_LIMIT(1,
                  "vulkan: %s drawn as lines with %s show the diagonals of the triangles they are drawn as: a list of "
                  "their sides would take no %s\n",
                  what,
                  culled && biased ? "face culling and a depth bias"
                  : culled         ? "face culling"
                                   : "a depth bias",
                  taken);
        return 0;
    }
    if (culled || biased)
        LOG_LIMIT(1,
                  "vulkan: the device has no polygon mode %s (fillModeNonSolid): polygons drawn as %s go as a list of "
                  "%s, which takes no %s\n",
                  name, as, as, taken);
    return 1;
}
/* what a draw of topology that draw_edges would draw as a list is drawn as when edge_indices can't make the list */
static const char* edges_instead(u32 topology) {
    return topology == 2 ? "open" : g_vk.feat.fillModeNonSolid ? "with the diagonals of its triangles" : "filled";
}
/* The list draw_edges draws a direct draw's count indices from first as (polygon_edges.h), made here into the staging
   ring and drawn indexed: from the guest's indices (isz bytes each), the draw's inline ones, or its vertex numbers
   counted from its first (the draw's vertex offset then). 1 with *n indices at *buf and *off; 0 when the CPU hasn't
   the guest's indices without waiting for the GPU (a result of its own over them), or they're more than EDGE_VERTICES:
   the draw is drawn as before then (edges_instead), logged; -1 when staging fails */
#define EDGE_VERTICES (1u << 20)
static int edge_indices(const DsState* ds, const DrawCall* dc, int indexed, u32 isz, u32 first, u32 count, u32 topology,
                        int points, VkBuffer* buf, u64* off, u32* n) {
    SnxEdgeIndices in = {0};
    const char* as = points ? "points" : "lines";
    if (count > EDGE_VERTICES) {
        LOG_LIMIT(1, "vulkan: a draw of %u vertices is past the %u the CPU makes into %s: drawn %s\n", count,
                  EDGE_VERTICES, as, edges_instead(topology));
        return 0;
    }
    if (indexed == 3)
        in.words = dc->inline_indices + first;
    else if (indexed) {
        u64 a = ds->index.addr + (u64)first * isz, len = (u64)count * isz, at;
        VkBuffer held;
        /* as draw_last_vertex reads them: landing what the GPU holds there takes a submit and a wait */
        if (vk_buf_pending(a, len) || vk_uma_pending(a, len) || vk_compute_gpu_src(a, len, &held, &at) ||
            vk_buf_owned(a, len)) {
            LOG_LIMIT(1, "vulkan: a draw whose indices the GPU wrote isn't made into %s on the CPU: drawn %s\n", as,
                      edges_instead(topology));
            return 0;
        }
        vk_compute_mem_need(a, len);
        in.bytes = gmem_ptr(a, len);
        in.size = isz;
        if (!in.bytes) {
            LOG_LIMIT(1,
                      "vulkan: a draw's %u indices at %llx aren't in one run of memory, which the CPU makes into %s: "
                      "drawn %s\n",
                      count, (unsigned long long)a, as, edges_instead(topology));
            return 0;
        }
    }
    u32 ones = isz == 1 ? 0xFFu : isz == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    int restart = indexed && ds->index.restart;
    u32 ri = ds->index.restart_index & ones;
    *n = snx_polygon_edges(topology, &in, count, restart, ri, points, NULL);
    u8* out = vk_stage((u64)(*n ? *n : 1) * 4, vk_stage_align(), buf, off);
    if (!out) return -1;
    snx_polygon_edges(topology, &in, count, restart, ri, points, out);
    return 1;
}
/* n elements of mode (with remap and the restart index restart, as texconv_modes.glsl says) made from count indices of
   isz bytes from index first, the guest's at ia or the draw's inline ones, into out_bytes of the output ring: *buf
   and *off, which the draw binds */
static int index_convert(const DrawCall* dc, int inline_indices, u64 ia, u32 isz, u32 first, u32 count, u32 mode,
                         u32 remap, u32 restart, u32 n, u64 out_bytes, VkBuffer* buf, u64* off) {
    u64 in_bytes = (u64)count * isz, align = vk_stage_align();
    VkBuffer src;
    u64 at;
    if (inline_indices) { /* host words the pushbuffer gave, in the CPU's hands already: copied whole */
        u8* p = vk_stage(in_bytes, align, &src, &at);
        if (!p) return 0;
        memcpy(p, dc->inline_indices + first, (size_t)in_bytes);
        snx_le32_array(p, count); /* read by the device little-endian */
    } else if (!vk_buf_get_input(ia + (u64)first * isz, in_bytes, isz, &src, &at))
        return 0;
    vk_render_end(); /* copies and dispatches happen outside rendering passes */
    VkCommandBuffer cb = vk_cmd();
    if ((at | in_bytes) & 3) { /* the shader reads whole words: a copy of them that starts at one, rounded up */
        VkBuffer word_buf;
        u64 word_at;
        if (!vk_stage_output((in_bytes + 3) & ~3ull, align, &word_buf, &word_at, NULL)) return 0;
        VkBufferCopy c = {at, word_at, in_bytes};
        rec_CmdCopyBuffer(cb, src, word_buf, 1, &c);
        index_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        src = word_buf;
        at = word_at;
    }
    if (!vk_stage_output(out_bytes, align, buf, off, NULL)) return 0;
    if (!vk_tc_index(cb, mode, remap, restart, n, src, at, in_bytes, *buf, *off, out_bytes)) return 0;
    index_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT);
    return 1;
}

/* An indirect mesh draw's record (DrawMeshTasksIndirect), its two words {count, first} as NV_mesh_shader's
   DrawMeshTasksIndirectCommandNV has them, which the driver hands the GPU as they are: guest memory the GPU reads
   (vk_buf_get), often a compute program's result. Vulkan's record is {x, y, z} and has no first group, so the words are
   copied on the GPU into one of this frame's staging, {count, 1, 1} at *off of *buf and first a uniform buffer's
   alignment after it, which the task program or the mesh program without one adds to its workgroup ID
   (SH_PUSH_GPU_MESH_FIRST, g_mesh_first that stage_set binds at SNX_VKB_RS). The copy ends the rendering pass, as an
   index conversion does. 0 when the record can't be had */
static int mesh_indirect(const DrawCall* dc, VkBuffer* buf, u64* off) {
    VkBuffer src;
    u64 at, align = g_vk.props.limits.minUniformBufferOffsetAlignment;
    if (align < 16) align = 16;
    if ((dc->indirect & 3) || !vk_buf_get_at(dc->indirect, 8, 4, &src, &at)) return 0;
    u8* p = vk_stage(2 * align, align, buf, off);
    if (!p) return 0;
    static const u32 record[4] = {0, 1, 1, 0};
    memset(p, 0, (size_t)(2 * align));
    memcpy(p, record, sizeof record);
    snx_le32_array(p, 4); /* the device reads them little-endian */
    vk_render_end(); /* copies happen outside rendering passes */
    VkCommandBuffer cb = vk_cmd();
    /* the program or copy that wrote the record before the copy reads it, and the draw after it */
    index_barrier(cb,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                      VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy c[2] = {{at, *off, 4}, {at + 4, *off + align, 4}};
    rec_CmdCopyBuffer(cb, src, *buf, 2, c);
    index_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
                      (g_vk.has_task ? VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT : 0),
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT);
    g_mesh_first = *buf;
    g_mesh_first_off = *off + align;
    return 1;
}
/* An indirect draw's arguments, and its count: guest memory as buffers the GPU reads (vk_buf_get), which hold a
   compute program's or an earlier draw's results where they are. 0 when they can't be had or drawn here */
static int indirect_args(const DrawCall* dc, int indexed, VkBuffer* abuf, u64* aoff, VkBuffer* cbuf, u64* coff) {
    if (dc->xfb_control) return !indexed && vk_xfb_indirect(dc->xfb_control, abuf, aoff);
    u32 rec = indexed ? 20 : 16, n = dc->draw_count ? dc->draw_count : 1, stride = dc->stride ? dc->stride : rec;
    if ((dc->indirect & 3) || (dc->indirect_count & 3) || (n > 1 && (stride < rec || (stride & 3)))) {
        LOG_LIMIT(1, "vulkan: an indirect draw at %llx with stride %u isn't drawn\n", (unsigned long long)dc->indirect,
                  stride);
        return 0;
    }
    if (dc->indirect_count && !g_vk.has_draw_indirect_count) {
        LOG_LIMIT(1, "vulkan: an indirect draw with a count needs drawIndirectCount, which this GPU lacks\n");
        return 0;
    }
    if (!vk_buf_get_at(dc->indirect, (u64)(n - 1) * stride + rec, 4, abuf, aoff)) return 0;
    return !dc->indirect_count || vk_buf_get_at(dc->indirect_count, 4, 4, cbuf, coff);
}
/* an indirect draw's index buffer: from its address on, as far as its guest memory runs (DsIndex.end), in the
   type Vulkan reads. 0 when the GPU would need its indices made into others first */
static int indirect_indices(const DsIndex* ix, int restart, VkBuffer* buf, u64* off, VkIndexType* type) {
    u32 isz = ix->format == 0 ? 1 : ix->format == 1 ? 2 : 4, ones = isz == 1 ? 0xFFu : isz == 2 ? 0xFFFFu : ~0u;
    u64 len = ix->end >= ix->addr ? ix->end - ix->addr + 1 : 0;
    if ((isz == 1 && !g_vk.has_index8) || (ix->restart && restart && (ix->restart_index & ones) != ones) ||
        (isz == 1 && ix->restart && !restart) || len < isz || (ix->addr & (isz - 1)))
        return 0;
    *type = isz == 1 ? VK_INDEX_TYPE_UINT8_KHR : isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    if (len > (4u << 20)) len = 4u << 20; /* a window holds it, and the binding reads on to the window's end */
    return vk_buf_get_window_input(ix->addr, len, isz, buf, off);
}
/* An indirect draw's indices that Vulkan can't read as they are (8-bit ones without VK_KHR_index_type_uint8, or with
   a restart index that isn't all ones), made on the GPU as a direct draw's are (index_convert). Every index of the
   buffer is made, from its address to its end: the records' first index and count are the GPU's, and they stay as
   they are, counting from the same first index. 0 when they can't be made */
static int indirect_indices_made(const DrawCall* dc, const DsIndex* ix, int restart, VkBuffer* buf, u64* off,
                                 VkIndexType* type) {
    u32 isz = ix->format == 0 ? 1 : ix->format == 1 ? 2 : 4, ones = isz == 1 ? 0xFFu : isz == 2 ? 0xFFFFu : ~0u;
    u32 ri = ix->restart_index & ones;
    u64 len = ix->end >= ix->addr ? ix->end - ix->addr + 1 : 0, n = len / isz;
    if (!n || (ix->addr & (isz - 1))) return 0;
    /* what texconv's 32-bit counts and offsets reach: an output of 4 GiB */
    if (n > (isz == 4 ? 0x3FFFFFFFu : 0x7FFFFFFFu)) {
        LOG_LIMIT(1,
                  "vulkan: an indirect draw's index buffer of %llu indices of %u bytes is past the %u made into "
                  "others\n",
                  (unsigned long long)n, isz, isz == 4 ? 0x3FFFFFFFu : 0x7FFFFFFFu);
        return 0;
    }
    int made;
    u32 words = isz == 4 ? (u32)n : (u32)((n + 1) / 2);
    if (isz == 1) { /* widened to 16 bits, the restart index (when on, whatever the topology) becoming 0xFFFF */
        *type = VK_INDEX_TYPE_UINT16;
        made = index_convert(dc, 0, ix->addr, 1, 0, (u32)n, TC_INDEX_WIDEN8, ix->restart != 0, ri, words,
                             (u64)words * 4, buf, off);
    } else {
        if (!(ix->restart && restart && ri != ones)) return 0;
        *type = isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        made = index_convert(dc, 0, ix->addr, isz, 0, (u32)n, isz == 2 ? TC_INDEX_RESTART16 : TC_INDEX_RESTART32, 0, ri,
                             words, (u64)words * 4, buf, off);
    }
    if (!made)
        LOG_LIMIT(1,
                  "vulkan: an indirect draw's index buffer of %llu indices of %u bytes couldn't be made into "
                  "others (%llu bytes)\n",
                  (unsigned long long)n, isz, (unsigned long long)words * 4);
    return made;
}
/* An indirect draw of quads: its records made the indexed records of their triangles on the GPU (vk_indirect_quads),
   since the counts that say how many are there; as many records as the draw may take (a count buffer cuts them at
   the draw, as before), 20 bytes apart then. Quads of vertices draw through the quad indices of vertex numbers
   (quad_indices), as many as the draw's streams hold vertices for (vertices; 0 when no stream tells), and quads of
   indices have theirs made from the whole index buffer, for a record that starts at any index. Past
   INDIRECT_QUAD_VERTICES vertices or indices, where a direct draw is cut too, the log says what is drawn. A restart
   index ends no quad here, as in a direct draw whose indices the CPU can't read. 0 when they can't be made */
#define INDIRECT_QUAD_VERTICES (1u << 24)
#define INDIRECT_QUAD_UNTOLD   (1u << 20) /* the vertices drawn of a record whose draw has no stream to tell */
static int indirect_quads(const DrawCall* dc, const DsState* ds, int indexed, u64 vertices, VkBuffer* abuf, u64* aoff,
                          VkBuffer* ibuf, u64* ioff) {
    const DsIndex* ix = &ds->index;
    const u32 flags = ds->rkey.provoking_last ? QUADS_LAST : 0, isz = ix->format == 0 ? 1 : ix->format == 1 ? 2 : 4;
    const u32 record = indexed ? 20 : 16, records = dc->xfb_control || !dc->draw_count ? 1 : dc->draw_count;
    const u32 stride = dc->xfb_control || !dc->stride ? record : dc->stride;
    VkBuffer source = VK_NULL_HANDLE;
    u64 source_at = 0, made_at, limit;
    if (indexed) {
        u64 len = ix->end >= ix->addr ? ix->end - ix->addr + 1 : 0;
        limit = len / isz;
        if (limit > INDIRECT_QUAD_VERTICES) {
            LOG_LIMIT(1, "vulkan: an indirect draw of quads reads the first %u of its index buffer's %llu indices\n",
                      INDIRECT_QUAD_VERTICES, (unsigned long long)limit);
            limit = INDIRECT_QUAD_VERTICES;
        }
        if (!limit || (ix->addr & (isz - 1)) || !vk_buf_get_input(ix->addr, limit * isz, isz, &source, &source_at))
            return 0;
        if ((source_at | (limit * isz)) &
            3) { /* the program reads whole words: a copy that starts at one, rounded up */
            VkBuffer words;
            u64 words_at;
            if (!vk_stage_output((limit * isz + 3) & ~3ull, vk_stage_align(), &words, &words_at, NULL)) return 0;
            vk_render_end(); /* copies happen outside rendering passes */
            VkCommandBuffer cb = vk_cmd();
            VkBufferCopy c = {source_at, words_at, limit * isz};
            rec_CmdCopyBuffer(cb, source, words, 1, &c);
            index_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            source = words;
            source_at = words_at;
        }
    } else {
        if (!vertices) {
            LOG_LIMIT(1,
                      "vulkan: an indirect draw of quads with no vertex stream draws %u vertices of a record at "
                      "most\n",
                      INDIRECT_QUAD_UNTOLD);
            vertices = INDIRECT_QUAD_UNTOLD;
        } else if (vertices > INDIRECT_QUAD_VERTICES) {
            LOG_LIMIT(1, "vulkan: an indirect draw of quads draws %u of the %llu vertices its streams hold at most\n",
                      INDIRECT_QUAD_VERTICES, (unsigned long long)vertices);
            vertices = INDIRECT_QUAD_VERTICES;
        }
        limit = vertices / 4 ? vertices / 4 : 1;
        if (!quad_indices((u32)limit, flags, ibuf, ioff)) return 0;
    }
    if (!vk_indirect_quads(*abuf, *aoff, stride, records, indexed ? isz : 0, source, source_at, (u32)limit,
                           flags & QUADS_LAST, abuf, aoff, &made_at)) {
        LOG_LIMIT(1, "vulkan: an indirect draw of quads of %u records and %llu %s couldn't be made into triangles\n",
                  records, (unsigned long long)limit, indexed ? "indices" : "quads");
        return 0;
    }
    if (indexed) {
        *ibuf = *abuf;
        *ioff = made_at;
    }
    return 1;
}
/* the draws: Vulkan's draw count past one needs multiDrawIndirect, else one call each */
static void draw_indirect(VkCommandBuffer cb, int indexed, VkBuffer abuf, u64 aoff, VkBuffer cbuf, u64 coff, u32 n,
                          u32 stride) {
    n = n ? n : 1;
    if (cbuf || n == 1 || g_vk.feat.multiDrawIndirect)
        rec_CmdDrawIndirect(cb, indexed, abuf, aoff, cbuf, coff, n, stride);
    else
        for (u32 i = 0; i < n; i++)
            rec_CmdDrawIndirect(cb, indexed, abuf, aoff + (u64)i * stride, VK_NULL_HANDLE, 0, 1, 0);
}
static void render_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o, const DrawTexture* texture);
static int capture_draw(const DsState* ds, const DrawCall* dc, DrawOwner* owner, Surf* target, u64 blend,
                         int coverage, const VkCoverage* mixed, VkCapture* prepared);
static int capture_supported(const DrawShaders* shaders, const DsState* ds, const DrawCall* dc, u32 colors) {
    if (!colors || colors > 7) return 0;
    u32 outputs = (1u << (colors * 4)) - 1;
    if (dc->indirect || dc->xfb_control || !dc->instances ||
        (u64)dc->base_instance + dc->instances - 1 > UINT32_MAX ||
        (u64)dc->first + dc->count > (u64)UINT32_MAX + 1 ||
        (dc->topology != 0 && dc->topology != 1 && dc->topology != 4) ||
        (dc->indexed && ds->index.restart) || shaders->tcs || shaders->tes || shaders->gs ||
        !shaders->fs0 || !shaders->vs->si.primitive_split_safe ||
        (shaders->fs0->si.sph[18] & ~outputs)) return 0;
    u32 vertices = dc->topology == 0 ? 1 : dc->topology == 1 ? 2 : 3;
    /* A complete non-indexed list presents each vertex once per instance. Shared indices can repeat vertex
       shader stores when split into primitives, and incomplete primitives need not invoke their vertices. */
    int unique_vertices = !dc->indexed && !(dc->count % vertices);
    if (!unique_vertices && (shaders->xfb_on || shaders->vs->si.gbuf_wmask || shaders->vs->si.nimg)) return 0;
    u64 primitives = (u64)dc->instances * (dc->count / vertices);
    if (primitives > VK_CAPTURE_PRIMITIVES_MAX) {
        g_vk_capture_capped++;
        LOG_LIMIT(4,
                  "vulkan: a draw of %llu primitives (%u per instance, %u instance%s) needs the ordered capture path, "
                  "which takes %u at most: draw skipped\n",
                  (unsigned long long)primitives, dc->count / vertices, dc->instances, dc->instances == 1 ? "" : "s",
                  VK_CAPTURE_PRIMITIVES_MAX);
        return 0;
    }
    return 1;
}
static int late_depth_supported(const DsState* ds, const DrawCall* dc, const DrawOwner* owner,
                                 const DrawShaders* shaders, const VkSh* last, const Surf* color, const Surf* depth) {
    if (!depth || !color || !shaders->fs0 || ds->prog.early_z || owner->pred ||
        vk_query_enabled(ds->owner) || !g_vk.has_depth_range_unrestricted || !g_vk_preserve_infnan ||
        !g_vk.has_fragment_interlock || !vk_capture_tiles_supported() ||
        color->layers != 1 || depth->layers != 1 || color->depth3d > 1 || depth->depth3d > 1 ||
        color->volume || depth->volume || color->msaa || depth->msaa || dc->raster_samples ||
        ds->rkey.depth_clamp || ds->rkey.conservative || ds->dyn.bias_on ||
        last->si.writes_layer || last->si.writes_viewport || (shaders->fs0->si.sph[19] & 2u) ||
        (ds->tgt.depth.format != 0x0a && ds->tgt.depth.format != 0x19) ||
        (depth->f.vk != VK_FORMAT_D32_SFLOAT && depth->f.vk != VK_FORMAT_D32_SFLOAT_S8_UINT)) return 0;
    /* Other stages and indexed tables do not retain all resolved sampled views for the alias proof below. */
    const VkSh* earlier[] = {shaders->vs, shaders->tcs, shaders->tes, shaders->gs};
    for (u32 i = 0; i < 4; i++) if (earlier[i] && earlier[i]->si.ntex) return 0;
    const VkSh* stages[] = {shaders->vs, shaders->tcs, shaders->tes, shaders->gs, shaders->fs0};
    for (u32 stage = 0; stage < 5; stage++)
        for (int i = 0; stages[stage] && i < stages[stage]->si.nimg; i++)
            if (stages[stage]->si.img[i].type == 1) return 0; /* texel-buffer ranges are not retained here */
    for (int i = 0; i < shaders->fs0->si.ntex; i++)
        if (shaders->fs0->si.tex[i].flags & SNX_TEX_INDEXED) return 0;
    VkViewport viewport = vk_viewport_from(&ds->dyn, &ds->vp, 0);
    return viewport.minDepth == 0 && viewport.maxDepth == 1;
}

/* a draw as a profiler zone, around its many ways out */
/* Feedback draws without fragment shader interlock. A draw that samples a color target T at its own pixel and writes T
   gets on the console the value of T from before the draw: its texture cache keeps it until a texture barrier. In
   place (feedback_order), a later primitive of the draw can read what an earlier one of the same draw wrote, and no
   barrier orders primitives inside one draw: one game's cobblestones had moving black and green specks from this.

   So the draw is drawn two times in the same pass. The save draw has the same vertex stages and geometry, with the
   depth, stencil and depth bounds tests off, so it covers each fragment that the guest's draw can have. Its fragment
   program (vk_predraw_fragment) fetches T at the own pixel in place and writes it to S (vk_rt_save), an extra color
   attachment of T's format. It writes nothing else, so all its fragments read T from before the draw. A by-region
   barrier follows (feedback_order, strict for S), then the guest's draw, whose texture of T is S (predraw_sample).
   The guest's draw does not read T, so the order of its primitives does not matter. Another barrier orders the reads
   of S before the next save draw writes S. This needs dynamicRenderingLocalRead for the barriers inside the pass, and
   neither interlock nor rasterization order attachment access. In a samples-passed query, the save draw ends the
   query's open segment (vk_query_pass_end), so its samples are not counted: each such draw takes one more segment of
   query.c's ring */
static int predraw_enabled(void) { return g_vk.feedback_mode == VK_FEEDBACK_PREDRAW; }
/* the slots of the pass that hold a target of the draw in two or its save: in the feedback-loop layout from the
   save draw on, so the guest's draw keeps the pass */
static u32 predraw_pass_slots(Surf* const* rt, u32 n) {
    u32 slots = 0;
    for (u32 i = 0; g_predraw.phase && i < n; i++)
        for (u32 k = 0; k < g_predraw.n; k++)
            if (rt[i] && (rt[i] == g_predraw.target[k] || rt[i]->predraw)) slots |= 1u << i;
    return slots;
}
/* 1 when the draw writes a slot that holds a target it samples in place */
static int predraw_writes_sampled(const DsState* ds, const VkSh* fs, Surf* const* rt, u32 n) {
    for (u32 i = 0; i < n; i++)
        for (u32 k = 0; k < g_predraw.n; k++)
            if (rt[i] == g_predraw.target[k] && (ds->blend.mask[i] & (fs->si.sph[18] >> (i * 4) & 15))) return 1;
    return 0;
}
/* 1 when the stages before the fragment's store nothing, which a draw done two times would store two times */
static int predraw_pre_raster_pure(const VkSh* const stages[4]) {
    for (u32 i = 0; i < 4; i++)
        if (stages[i] && (stages[i]->si.nimg || stages[i]->si.gbuf_wmask)) return 0;
    return 1;
}
/* 1 when the save draw can keep the guest draw's depth test, read-only: each fragment that passes in the guest's draw
   then also passes against the depth from before the draw. That holds when the draw leaves depth as it was, and when
   its writes only make a less or greater test stricter. Not when the fragment program writes its own depth */
static int predraw_keeps_depth(const DsState* ds, const VkSh* fs) {
    if (!ds->dyn.depth_test || (fs->si.sph[19] & 2u)) return 0;
    VkCompareOp op = (VkCompareOp)ds->dyn.depth_cmp;
    return !ds->dyn.depth_write || op == VK_COMPARE_OP_LESS || op == VK_COMPARE_OP_LESS_OR_EQUAL ||
           op == VK_COMPARE_OP_GREATER || op == VK_COMPARE_OP_GREATER_OR_EQUAL || op == VK_COMPARE_OP_ALWAYS;
}
/* the save draw, then the guest's draw, with fs the guest's fragment program. free: the color slots the guest binds no
   target to, where the save attachments go (a guest may bind 8 slots and leave some without a format). NULL when
   done, else why the save draw wasn't drawn: the caller draws in place */
static const char* predraw_run(const DsState* ds, const DrawCall* dc, DrawOwner* o, u32 free, const VkSh* fs) {
    u32 m = g_predraw.n, ints = 0, sints = 0, room = g_vk.props.limits.maxColorAttachments;
    if (room > DS_TARGETS_MAX) room = DS_TARGETS_MAX;
    u8 slots[DS_TARGETS_MAX];
    u32 c = 0;
    for (u32 j = 0; j < room; j++)
        if (free >> j & 1) slots[c++] = (u8)j;
    if (c < m) {
        static u8 said[DS_TARGETS_MAX + 1][DS_TARGETS_MAX + 1];
        if (!said[c][m]++)
            LOG("vulkan: a feedback draw samples %u of its targets in place, and its pass has %u color slots without "
                "a target (of %u): it is drawn in place\n", m, c, room);
        return "has no color slot left";
    }
    /* two sets where they fit: one by-region barrier for each feedback draw, not two. The save attachments of a set
       take its slots from the lowest up, in the order of the save program's outputs */
    g_predraw.sets = c >= 2 * m ? 2 : 1;
    for (u32 s = 0; s < g_predraw.sets; s++)
        for (u32 k = 0; k < m; k++) {
            g_predraw.save[s][k] = vk_rt_save(g_predraw.target[k], s);
            if (!g_predraw.save[s][k]) return "samples a target that has no save attachment (layered, multisampled)";
            g_predraw.slot[s][k] = slots[s * m + k];
        }
    for (u32 k = 0; k < m; k++) {
        if (g_predraw.target[k]->f.is_int) ints |= 1u << k;
        if (g_predraw.target[k]->f.sint) sints |= 1u << k;
    }
    u32 parity = g_predraw.sets == 2 ? g_predraw.parity ^ 1 : 0;
    g_predraw.wmask = 0;
    for (u32 k = 0; k < m; k++) g_predraw.wmask |= 1u << g_predraw.slot[parity][k];
    g_predraw.fs = vk_predraw_fragment(m, g_predraw.wmask, ints, sints);
    if (!g_predraw.fs || g_predraw.fs->failed) return "has no save draw program (not translated)";
    g_predraw.parity = parity;
    static DsState pre; /* under the renderer's lock, as each draw is */
    pre = *ds;
    /* the tests stay where they pass a superset of the guest draw's fragments: fewer fragments to save */
    int stencil_kept = !(ds->dyn.face[0].wmask & 0xFF) && !(ds->dyn.face[1].wmask & 0xFF) && !(fs->si.sph[19] & 2u);
    if (!predraw_keeps_depth(ds, fs)) pre.dyn.depth_test = 0;
    if (!stencil_kept) pre.dyn.stencil_test = 0;
    pre.dyn.depth_write = pre.dyn.alpha_test = 0;
    pre.rkey.alpha_to_coverage = pre.rkey.alpha_to_one = 0;
    pre.blend.logic_op = 0;
    memset(pre.blend.en, 0, sizeof pre.blend.en);
    DrawCall call = *dc;
    call.state_flags &= ~DS_DRAW_DEPTH_BOUNDS;
    u64 drawn = g_vk_drawn;
    g_predraw.phase = 1;
    render_draw(&pre, &call, o, NULL);
    int saved = g_vk_drawn != drawn;
    if (saved) {
        g_vk_drawn = drawn; /* the guest's draw is counted, not the save draw */
        g_predraw.phase = 2;
        render_draw(ds, dc, o, NULL);
        if (g_vk_drawn != drawn) g_vk_predraws++;
    }
    g_predraw.phase = 0;
    g_predraw.n = g_predraw.in_place = 0;
    draw_forget(); /* the next draw keeps nothing of these two */
    return saved ? NULL : "had its save draw skipped (the log says why above)";
}
void vk_render_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    SNX_ZONE(z, "draw");
    int heaps = g_vk.has_descriptor_heaps;
    render_draw(ds, dc, o, NULL);
    g_vk.has_descriptor_heaps = heaps;
    SNX_ZONE_END(z);
}
/* in a draw's setup, from its start (retry) to its check of the submission number (setup_frame), where it starts again
   when that changed: an upload's wait for the GPU there may let go of vk_lock (vk_submit_upload_wait). 0 past it, and
   once a draw inside a draw (a save draw, a captured primitive) ended, for the rest of the outer one's setup */
static int g_setup_restarts;
int vk_render_setup_restarts(void) { return g_setup_restarts; }
static void render_draw_main(const DsState* ds, const DrawCall* dc, DrawOwner* o, const DrawTexture* texture);
static void render_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o, const DrawTexture* texture) {
    render_draw_main(ds, dc, o, texture);
    g_setup_restarts = 0;
}
static void render_draw_main(const DsState* ds, const DrawCall* dc, DrawOwner* o, const DrawTexture* texture) {
    DrawCall normalized;
    if (dc->xfb_control) {
        normalized = *dc;
        normalized.indirect = dc->xfb_control;
        dc = &normalized;
    }
    int indexed = (int)dc->indexed;
    u32 topology = dc->topology, first = dc->first, count = dc->count, base_vertex = dc->base_vertex,
        base_instance = dc->base_instance, instances = dc->instances;
    /* the stages between the vertex and fragment programs and their sets */
    int bset_ok, b;
    VkSh *vs, *tcs, *tes, *gs, *between[3];
    VkDescriptorSet bset[3];
    if (!count || !instances) return;
    vk_frame_check();
    g_mesh_first = VK_NULL_HANDLE; /* an indirect mesh draw's first group is this draw's (mesh_indirect) */
    g_mesh_payload = VK_NULL_HANDLE; /* and so is the compute-task path's buffer (sm86.h) */
    g_memh_draw++;                 /* and so are the handles from memory (memh_handle) */
    /* an Ounce's mesh draw: the mesh program reads the vertex stage's set and push constants and has bindings of its
       own, the task program the tessellation control stage's set (shader_vk_set) */
    int mesh = dc->mesh != 0, vs_bind = mesh ? DS_MESH : 0;
    if (mesh) {
        g_vk_mesh_draws++;
        /* an indirect one of one record of two words (mesh_indirect), not the 3D form's x y z or a multi-draw's */
        if ((dc->indirect && (dc->stride != 8 || dc->indirect_count || dc->draw_count > 1)) ||
            count > g_vk.mesh_groups_max) {
            if (g_vk.has_mesh)
                LOG_LIMIT(1, "vulkan: %s (skipped)\n",
                          dc->indirect ? "an indirect mesh draw of the 3D form, or of several records"
                                       : "a mesh draw of more workgroups than this GPU launches");
            g_vk_skipped++;
            return;
        }
    }
    /* read each draw: a test turns them on and off */
    int check = g_settings.draw_state_check != 0;
    if (check && !g_draw.told) {
        g_draw.told = 1;
        LOG("vulkan: draw_state_check=1: draws that keep an earlier draw's shaders and pipeline make them again to "
            "compare\n");
    }
    /* the draw's path, as draw_shaders' mesh argument: a mesh draw is 1, and 2 more where its task program is a
       compute dispatch (g_vk.mesh_task_compute, sm86.h), which a direct draw and an indirect one both take */
    int mesh_path = mesh;
    if (mesh && g_vk.mesh_task_compute && ds->prog.task && ds->prog.mesh_key.has_task &&
        (ds->prog.mesh_key.task_payload || ds->prog.mesh_key.mesh_payload))
        mesh_path |= 2;
    /* a direct draw's task workgroups, cut to the buffer's slots; an indirect one's are the GPU's record's */
    u32 task_groups = mesh_path & 2 && !dc->indirect ? count : 0;
    u32 initial_first = first, initial_count = count, draw_topo = topology;
    int retries = 0, initial_indexed = indexed, predraw_refused = 0;
retry:
    vk_buf_writable_abort();
    indexed = initial_indexed;
    first = initial_first;
    count = initial_count;
    u64 setup_frame = vk_submit_number();
    g_setup_restarts = 1;
    u32 epoch = vk_ww_epoch();
    if (epoch != g_stats.epoch) {
        g_stats.epoch = epoch;
        g_stats.n.epochs++;
    }
    /* kept: the state an earlier draw made (entry found, ke) holds for this one, as far as each check below tells. Each
       part is made here when it doesn't (and when draw_state_check compares). Only draws of the guest's own state look,
       and only they keep what they made: a draw of a texture, a capture's slices and a save draw draw other state,
       which a later draw of the same bytes must not take */
    int capture = g_capture_draw.images != NULL, own = !texture && !capture && !g_predraw.phase;
    DrawKey key;
    int have_key = 0;
    u32 found = own ? draw_find(ds, draw_topo, mesh_path, epoch, check, &key, &have_key) : 0;
    DrawKept* ke = found ? &g_draw.e[found] : NULL;
    int kept = found != 0;
    if (kept && check) g_vk_draw_checked++;
    DrawShaders sh;
    if (kept) sh = ke->sh;
    if (!kept || check) {
        DrawShaders made = {0};
        int shaders_ok;
        if (texture) {
            made.vs = vk_draw_texture_vertex();
            made.fs0 = vk_draw_texture_fragment(0, 0, 0, 0, 0, 0);
            made.topology = 5;
            rt_int_targets(&ds->tgt, &made.rt_int, &made.rt_sint);
            shaders_ok = made.vs && made.fs0;
        } else
            shaders_ok = draw_shaders(ds, draw_topo, mesh_path, &made);
        if (!shaders_ok) {
            if (kept) {
                draw_differs("shaders");
                draw_drop(found);
            }
            g_vk_skipped++;
            return;
        }
        if (kept && !draw_shaders_same(&made, &sh)) {
            draw_differs("shaders");
            kept = 0;
        }
        sh = made;
    }
    topology = sh.topology;
    vs = sh.vs;
    tcs = sh.tcs;
    tes = sh.tes;
    gs = sh.gs;
    int xfb_on = sh.xfb_on;
    /* the draw as a list of lines or points made of its primitives (draw_edges), points for the polygon mode Point:
       until edge_indices finds the CPU can't make it, which draws its topology */
    int edges = !texture && !capture && draw_edges(ds, dc, &sh),
        points = ds->rkey.polygon == DS_POLYGON_POINT && topology != 2;
    /* the fragment shader's variant, keyed on its textures each draw: they are resolved for stage_set anyway. fres is
       filled for the fragment program's textures (each resolved, have set), and read only for them: a clear of all
       SH_MAXTEX (10 KiB) and a check of all of them against each target were per draw */
    TexRes fres[SH_MAXTEX], vres[SH_MAXTEX];
    TexRes* res = fres; /* the variant's slots, as stage_set reads them */
    VkSh* last = gs ? gs : tes ? tes : vs;
    Surf* rt[8];
    Surf* zt;
    u32 n;
    read_targets(ds, rt, &zt, &n);
    const DsSurf* requested_depth = &ds->tgt.depth;
    if (!zt && requested_depth->gva && requested_depth->width && requested_depth->height &&
        (ds->dyn.depth_test || ds->dyn.depth_write || ds->dyn.stencil_test ||
         (dc->state_flags & DS_DRAW_DEPTH_BOUNDS))) {
        LOG_LIMIT(4, "vulkan: a required depth/stencil target is unavailable, draw skipped\n");
        g_vk_skipped++;
        return;
    }
    int alpha_integer = n && rt[0] && rt[0]->f.is_int; /* retain the guest primary type before capture's float scratch */
    int late_depth = 0;
    u32 ratio_one_flags = 0;
    if (dc->raster_samples) {
        u32 color_samples = 0;
        int same = 1;
        for (u32 i = 0; i < n; i++)
            if (rt[i]) {
                u32 samples = vk_surf_samples(rt[i]);
                if (color_samples && samples != color_samples) same = 0;
                color_samples = samples;
            }
        VkCoverage equal;
        if (same && color_samples == dc->raster_samples &&
            vk_coverage_key(&equal, dc, color_samples, n, zt ? (u32)vk_surf_samples(zt) : 0,
                             g_vk.has_mixed_samples, g_vk.props.limits.framebufferNoAttachmentsSampleCounts) &&
            equal.table_enable)
            ratio_one_flags = shader_coverage_r1_flags(equal.mode, equal.table[0]);
        if (ratio_one_flags && !g_vk_preserve_infnan) {
            LOG_LIMIT(4, "vulkan: ratio-one modulation needs signed-zero/infinity/NaN preservation\n");
            g_vk_skipped++;
            return;
        }
    }
    if (capture) {
        u32 colors = g_capture_draw.images->mixed_count ? g_capture_draw.images->mixed_count : 1;
        for (u32 i = 0; i < colors; i++) rt[i] = vk_capture_mixed_source(g_capture_draw.images, i);
        rt[colors] = &g_capture_draw.images->valid;
        n = colors + 1;
    }
    /* the save attachments of a feedback draw drawn two times, after the guest's slots (predraw_run). An ordinary draw
       of the same pass keeps them, with nothing written to them, so the pass doesn't end for it */
    if (g_predraw.phase) {
        for (u32 s = 0; s < g_predraw.sets; s++)
            for (u32 k = 0; k < g_predraw.n; k++) {
                u32 j = g_predraw.slot[s][k];
                rt[j] = g_predraw.save[s][k];
                if (j + 1 > n) n = j + 1;
            }
    } else if (!capture && !texture && g_rp.open) {
        /* the open pass differs from these targets only by save attachments in slots they leave empty */
        int keep = 0;
        for (u32 j = 0; j < DS_TARGETS_MAX; j++)
            if (g_rp.rt[j] != rt[j]) {
                if (rt[j] || !g_rp.rt[j] || !g_rp.rt[j]->predraw) {
                    keep = 0;
                    break;
                }
                keep = 1;
            }
        if (keep) {
            for (u32 j = 0; j < DS_TARGETS_MAX; j++)
                if (!rt[j]) rt[j] = g_rp.rt[j];
            if (g_rp.n > n) n = g_rp.n;
        }
    }
    int saves = 0;
    for (u32 i = 0; i < n; i++)
        if (rt[i] && rt[i]->predraw) saves = 1;
    if (saves) kept = 0;
    u32 to_color = 0, shader_to_color = 0;
    if (dc->multisample_flags & DS_MS_COVERAGE_TO_COLOR) {
        u32 location = dc->coverage_to_color_output;
        int valid = location < n && rt[location] && rt[location]->f.is_int &&
                    (rt[location]->f.vk == VK_FORMAT_R8_UINT || rt[location]->f.vk == VK_FORMAT_R8_SINT ||
                     rt[location]->f.vk == VK_FORMAT_R16_UINT || rt[location]->f.vk == VK_FORMAT_R16_SINT ||
                     rt[location]->f.vk == VK_FORMAT_R32_UINT || rt[location]->f.vk == VK_FORMAT_R32_SINT);
        /* Other formats leave coverage-to-color disabled. */
        if (valid && (ds->blend.mask[location] & 1)) {
            /* SampleMask is per invocation. With one raster sample its nonzero value is the whole pixel mask,
               even if the host splits invocations. Multiple samples still need the ordered capture union. */
            int single_sample = dc->raster_samples <= 1 && (!zt || !zt->msaa);
            for (u32 i = 0; i < n; i++)
                if (rt[i] && rt[i]->msaa) single_sample = 0;
            int post_depth_safe = single_sample && g_vk.has_post_depth_coverage && sh.fs0 &&
                                  !ds->rkey.alpha_to_coverage && !(sh.fs0->si.sph[19] & 3) &&
                                  ((!texture && ds->prog.early_z) ||
                                   (!sh.fs0->si.has_discard && !ds->dyn.alpha_test &&
                                    !sh.fs0->si.gbuf_wmask && !sh.fs0->si.nimg));
            if (g_vk.has_coverage_to_color)
                to_color = location + 1;
            else if (post_depth_safe)
                shader_to_color = SH_COVERAGE_TO_COLOR | location << SH_COVERAGE_OUTPUT_SHIFT | SH_BLEND_EARLY_TESTS;
            else if (!capture && !texture && n == 1 && location == 0 &&
                     late_depth_supported(ds, dc, o, &sh, last, rt[0], zt)) {
                late_depth = 1;
                shader_to_color = SH_COVERAGE_TO_COLOR;
            }
            else if (!capture) {
                if (!texture && location == 0 && n == 1 && !ds->blend.logic_op &&
                    capture_supported(&sh, ds, dc, 1) && capture_draw(ds, dc, o, rt[0], 0, 1, NULL, NULL)) return;
                LOG_LIMIT(4, "vulkan: coverage-to-color requires a native or ordered capture path for this draw\n");
                g_vk_skipped++;
                return;
            } else {
                LOG_LIMIT(4, "vulkan: coverage-to-color needs native support or safe post-depth shader coverage\n");
                g_vk_skipped++;
                return;
            }
        }
    }
    u64 programmable_blend = 0;
    u32 programmable_flags = 0;
    VkAdvancedBlend native_blend = {0};
    DsBlend reduced_blend = ds->blend;
    int fixed_reduction = dc->advanced_blend[0] && nvn_blend_advanced(&reduced_blend, 0, dc->advanced_blend[0]);
    int any_advanced =
        dc->advanced_blend[0] && !capture && ds->blend.en[0] && !ds->blend.logic_op && n && rt[0] && !rt[0]->f.is_int;
    int only_first = 1;
    for (u32 i = 1; i < n; i++)
        if (rt[i] && ds->blend.mask[i]) only_first = 0;
    if (any_advanced && only_first)
        vk_advanced_blend_key(&native_blend, dc->advanced_blend[0], &g_vk.advanced_blend_caps, n, 0);
    int blend_early_safe = sh.fs0 &&
                           (ds->prog.early_z || (!sh.fs0->si.has_discard && !ds->dyn.alpha_test &&
                                                !sh.fs0->si.gbuf_wmask && !sh.fs0->si.nimg));
    int blend_shadow = any_advanced && only_first && !native_blend.operation && !fixed_reduction &&
                       !capture && !texture && n == 1 && !blend_early_safe &&
                       late_depth_supported(ds, dc, o, &sh, last, rt[0], zt);
    u32 blend_samples = rt[0] ? (u32)vk_surf_samples(rt[0]) : 1;
    if (any_advanced && only_first && !native_blend.operation && !fixed_reduction && g_vk.has_fragment_interlock &&
        g_vk.feat.fragmentStoresAndAtomics && g_vk.feat.shaderStorageImageReadWithoutFormat &&
        g_vk.feat.shaderStorageImageWriteWithoutFormat && (!zt || blend_early_safe || blend_shadow) &&
        (!ds->tgt.msaa || (g_vk.feat.shaderStorageImageMultisample && (!zt || g_vk.has_post_depth_coverage) &&
                           (g_vk.props.limits.framebufferNoAttachmentsSampleCounts & blend_samples))) &&
        !ds->rkey.alpha_to_coverage &&
        !ds->rkey.alpha_to_one && !dc->raster_samples && rt[0]->storage_capable && rt[0]->depth3d <= 1 &&
        sh.fs0 && !(sh.fs0->si.sph[19] & 3u) && !(sh.fs0->si.sph[18] & ~15u) && (u32)sh.fs0->si.nimg < g_vk.image_slots[ST_FS]) {
        programmable_blend = dc->advanced_blend[0];
        if (rt[0]->layers > 1 || last->si.writes_layer) programmable_flags |= SH_BLEND_ARRAY;
        if (ds->tgt.srgb_write && rt[0]->f.vk != vk_srgb_as(rt[0]->f.vk, 0)) programmable_flags |= SH_BLEND_SRGB;
        if (blend_samples > 1) programmable_flags |= SH_BLEND_MULTISAMPLE | blend_samples << 8;
        if (zt && blend_shadow) late_depth = 1;
        else if (zt) programmable_flags |= SH_BLEND_EARLY_TESTS;
    }
    if (any_advanced && only_first && !native_blend.operation && !fixed_reduction && !programmable_blend) {
        if (!texture && n == 1 && capture_supported(&sh, ds, dc, 1) &&
            capture_draw(ds, dc, o, rt[0], dc->advanced_blend[0], 0, NULL, NULL)) return;
        LOG_LIMIT(4, "vulkan: advanced blending needs a native, interlock or ordered capture path for this draw\n");
        g_vk_skipped++;
        return;
    }
    /* and the vertex program, or the geometry program, of a per-vertex pass (draw_varying) */
    VkSh *fs = NULL, *varying_vs = NULL, *varying_gs = NULL;
    FsKey fk = {0};
    if (sh.fs0) {
        if (texture) {
            tex_resolve(ds, texture->handle, &fres[0]);
            int num = vk_tex_num(fres[0].tic, fres[0].t);
            fk = (FsKey){.tm = num != 0, .ts = num == 2, .rm = sh.rt_int, .rs = sh.rt_sint};
        } else {
            fs_key(ds, sh.fs0, fres, last->si.out_mask, sh.rt_int, sh.rt_sint, &fk);
            u32 varying = capture ? 0 : draw_varying(ds, &sh, 1);
            if ((varying & 3) == SH_VARYING_BARYCENTRIC) {
                varying_vs = vs_position_copy(ds, sh.vs, SH_VARYING_SLOT(varying));
                if (!varying_vs || varying_vs->failed) { /* its position's copy can't be: the position pass */
                    varying_vs = NULL;
                    varying = draw_varying(ds, &sh, 0);
                }
            } else if ((varying & 3) == SH_VARYING_GEOMETRY) {
                u32 taken = sh.vs->si.out_mask & sh.fs0->si.smooth_in_mask & ~shader_varying_position_mask(varying);
                varying_gs = vk_varying_geometry(sh.vs, taken, SH_VARYING_SLOT(varying));
                if (!varying_gs) varying = draw_varying(ds, &sh, 0);
            }
            fk.config |= varying << SH_FS_VARYING_SHIFT;
        }
        fk.prim = sh.prim;
        if (shader_to_color) {
            fk.rm |= 1u << dc->coverage_to_color_output;
            if (rt[dc->coverage_to_color_output]->f.sint) fk.rs |= 1u << dc->coverage_to_color_output;
            else fk.rs &= ~(1u << dc->coverage_to_color_output);
        }
        for (int i = 0; i < sh.fs0->si.ntex && programmable_blend; i++)
            if (!(sh.fs0->si.tex[i].flags & SNX_TEX_INDEXED) && fres[i].have && fres[i].t == rt[0])
                programmable_blend = 0;
        fk.advanced_blend = programmable_blend;
        fk.advanced_mask = programmable_blend ? (ds->blend.mask[0] & SH_BLEND_MASK) | programmable_flags : 0;
        fk.advanced_mask |= shader_to_color;
        if (late_depth) fk.config |= SH_FS_LATE_DEPTH;
        fk.advanced_mask |= ratio_one_flags;
        if (!alpha_integer && n && rt[0] && ds->rkey.alpha_to_one)
            fk.advanced_mask |= SH_ALPHA_TO_ONE;
        if (capture) {
            u32 colors = g_capture_draw.images->mixed_count ? g_capture_draw.images->mixed_count : 1;
            u32 floating = (1u << (colors + 1)) - 1;
            fk.rm &= ~floating;
            fk.rs &= ~floating;
            if (colors > 1) fk.config |= colors << SH_FS_CAPTURE_SHIFT;
            fk.advanced_mask |= SH_CAPTURE_SOURCE | g_capture_draw.images->flags;
        }
        u32 alpha_samples = rt[0] ? (u32)vk_surf_samples(rt[0]) : 1;
        if (dc->raster_samples > alpha_samples) alpha_samples = dc->raster_samples;
        /* Current pipelines use ordinary rasterization, with no RECTANGULAR_SMOOTH alpha factor after the FS.
           Coverage uses the original alpha before software alpha-to-one and equal-count modulation. */
        if (ds->rkey.alpha_to_coverage && !alpha_integer && n && rt[0] &&
            alpha_samples >= 1 && alpha_samples <= 16) {
            fk.advanced_mask |= SH_ALPHA_COVERAGE | alpha_samples << 8;
            if (dc->multisample_flags & DS_MS_A2C_DITHER) fk.advanced_mask |= SH_ALPHA_COVERAGE_DITHER;
        }
        /* overlapping primitives of the draw read what its targets held before it, not each other's writes */
        if (!texture && !fk.advanced_blend && !late_depth && !capture && !varying_vs && !varying_gs) {
            ScreenSlots early = screen_slots(ds, sh.fs0, sh.vs, !sh.tcs && !sh.tes && !sh.gs && !sh.swizzle);
            fk.predraw = predraw_choose(ds, sh.fs0, fres, rt, n, &early);
            /* the capture's stores would keep the GPU from culling hidden fragments early: where the guest's program
               can't discard or write depth, early tests give the same result, and the hidden fragments capture
               nothing a passing one wants */
            fk.early = fk.predraw && blend_early_safe && !(sh.fs0->si.sph[19] & 3u);
        }
        if (kept && memcmp(&fk, &ke->fkey, sizeof fk)) kept = 0;
        if (kept) fs = ke->fs;
        if (!kept || check) {
            VkSh* made = texture ? vk_draw_texture_fragment((u32)fk.tm, (u32)fk.ts, fk.rm, fk.rs, fk.advanced_blend,
                                                            fk.advanced_mask)
                         : vk_host_programs(&ds->prog) ? sh.fs0 /* its GLSL decided what it does */
                                                       : fs_variant(ds, ds->prog.code[5], sh.fs0, &fk);
            if (kept && made != fs) {
                draw_differs("fragment shader");
                kept = 0;
            }
            fs = made;
        }
        if (texture && !fs) { g_vk_skipped++; return; }
        if (fs && fs->failed) {
            LOG_LIMIT(4, "vulkan: the fragment program at %llx isn't translated (%s): draw skipped\n",
                      (unsigned long long)ds->prog.code[5], fs->si.first_unsupported);
            g_vk_skipped++;
            return;
        }
        if (!texture && fs && fs != sh.fs0) res = fs_slot_res(ds, sh.fs0, fres, fs, vres);
    }
    if (g_predraw.phase == 1) {
        fs = g_predraw.fs; /* the save draw's program, whose textures stage_set_ takes from g_predraw */
        res = NULL;
    }
    DrawShaders drawn = sh; /* the stages drawn: sh's, with a per-vertex pass's vertex or geometry program */
    if (varying_vs) drawn.vs = vs = last = varying_vs;
    if (varying_gs) drawn.gs = gs = last = varying_gs;
    cb_prefetch(ds, vs_bind, vs, 0);
    if (fs) cb_prefetch(ds, 4, fs, 0);
    if (!dc->indirect && !mesh) draw_indices_prefetch(&ds->index, indexed, first, count, 0);
    for (u32 i = 0; i < n; i++)
        if (trace_target(rt[i]))
            LOG_LIMIT(64,
                      "gpu-trace-target: draw color slot %u target=%llx %ux%u count=%u indexed=%d vs=%llx fs=%llx\n", i,
                      (unsigned long long)rt[i]->gva, rt[i]->w, rt[i]->h, count, indexed,
                      (unsigned long long)ds->prog.code[1], (unsigned long long)ds->prog.code[5]);
    if (trace_target(zt))
        LOG_LIMIT(64, "gpu-trace-target: draw depth target=%llx %ux%u count=%u indexed=%d vs=%llx fs=%llx\n",
                  (unsigned long long)zt->gva, zt->w, zt->h, count, indexed, (unsigned long long)ds->prog.code[1],
                  (unsigned long long)ds->prog.code[5]);
    /* a color target the fragment shader samples but doesn't write (no output for it, or a write mask of 0) is drawn
       without: a game's lighting passes may keep the whole G-buffer bound while they read some of it, which
       Maxwell allows, and Vulkan can't sample an attachment of the pass */
    VkExtent2D bound = target_extent(rt, zt, n); /* the render area should none of them stay */
    /* not for the save draw (predraw_run, res NULL): it samples the targets it doesn't write in place, and its pass is
       the guest draw's */
    if (fs && res)
        for (u32 j = 0; j < n; j++) {
            if (!rt[j] || (((fs->si.sph[18] >> (j * 4)) & 15) && ds->blend.mask[j])) continue;
            for (int i = 0; i < (fs->si.ntex > SH_MAXTEX ? SH_MAXTEX : fs->si.ntex); i++)
                if (res[i].have && res[i].t == rt[j]) {
                    rt[j] = NULL;
                    break;
                }
        }
    /* nothing drawn to, captured or stored. A program's stores to memory or images still run in a pass without
       attachments: a per-pixel pass may set every target's format to 0 and keep only what it stores */
    int stores = vs->si.gbuf_wmask || vs->si.nimg || (fs && (fs->si.gbuf_wmask || fs->si.nimg)) ||
                 (tcs && (tcs->si.gbuf_wmask || tcs->si.nimg)) ||
                 (tes && (tes->si.gbuf_wmask || tes->si.nimg)) || (gs && (gs->si.gbuf_wmask || gs->si.nimg));
    if (!n && !zt && !xfb_on && !stores) {
        g_vk_skipped++;
        return;
    }
    if (!g_settings.shadows && !n && zt && shadow_map(zt)) {
        /* shadows=0: depth-only passes into a shadow map are skipped, and the map is cleared to far once a frame so it
           reads as fully lit. Other depth-only passes (a depth prepass, at any size) draw */
        static u32 cleared_frame = ~0u;
        static Surf* cleared;
        if (cleared_frame != g_vk_present || cleared != zt) {
            cleared_frame = g_vk_present;
            cleared = zt;
            if (render_begin(rt, zt, 0, 0, ds->tgt.srgb_write, 0, target_extent(rt, zt, 0))) {
                VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                                                     (zt->f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)};
                a.clearValue.depthStencil = (VkClearDepthStencilValue){1.0f, 0};
                VkClearRect cr = {{{0, 0}, g_rp.ext}, 0, 1};
                rp_record();
                rec_CmdClearAttachments(vk_cmd(), 1, &a, 1, &cr);
                g_vk_clear_cmds++;
                target_written(zt);
            }
        }
        g_vk_skipped++;
        return;
    }

    /* the pipeline key and its vertex bindings: the kept ones when the targets' formats and the empty attributes are the
       same too */
    TargetFormats tf;
    target_formats(rt, zt, n, &tf);
    if (kept && (memcmp(&tf, &ke->tf, sizeof tf) ||
                 attributes_empty(&ds->vbuf, &ds->vfmt, sh.vs->si.in_mask) != ke->plan.empty))
        kept = 0;
    VkCoverage coverage;
    if (!vk_coverage_key(&coverage, dc, tf.samples ? tf.samples : 1, n, tf.depth_samples, g_vk.has_mixed_samples,
                         g_vk.props.limits.framebufferNoAttachmentsSampleCounts) ||
        (coverage.raster_samples > tf.samples && (ds->rkey.sample_shading || (fs && fs->si.uses_sample_id)))) {
        /* Capture at N samples, then reduce to one native color sample. A strict translator proof excludes
           invocation-dependent sources; keep the resource/input checks here as an additional interface gate. */
        const ShaderInfo* info = sh.fs0 ? &sh.fs0->si : NULL;
        int advanced = 0;
        int dual_source = 0;
        for (u32 i = 0; i < n; i++)
            if (rt[i] && ds->blend.mask[i] && ds->blend.en[i] && !ds->blend.logic_op) {
                if (dc->advanced_blend[i]) advanced = 1;
                if (ds->blend.sc[i] >= DS_FACTOR_SRC1_COLOR || ds->blend.dc[i] >= DS_FACTOR_SRC1_COLOR ||
                    ds->blend.sa[i] >= DS_FACTOR_SRC1_COLOR || ds->blend.da[i] >= DS_FACTOR_SRC1_COLOR) dual_source = 1;
            }
        VkCoverage portable;
        if (!capture && !texture && dc->topology == 4 && ds->rkey.polygon == DS_POLYGON_FILL &&
            n && n <= 7 && rt[0] && !rt[0]->msaa && !rt[0]->f.is_int &&
            rt[0]->layers == 1 && rt[0]->depth3d <= 1 && (!zt || tf.depth_samples == dc->raster_samples) &&
            !advanced && !dual_source &&
            (!ds->blend.logic_op || g_vk.feat.logicOp) && !last->si.writes_layer && info &&
            info->mixed_sample_invariant && !info->in_mask && !info->ntex && !info->nimg &&
            !info->ngbuf && !info->cbuf_mask && !info->uses_sample_id && !(info->sph[19] & 3u) &&
            vk_coverage_portable_key(&portable, dc, 1, n, tf.depth_samples, 1,
                                      g_vk.props.limits.framebufferNoAttachmentsSampleCounts) &&
            portable.raster_samples && vk_capture_mixed_outputs_supported(info->sph[18], n, &ds->blend) &&
            capture_supported(&sh, ds, dc, n) &&
            capture_draw(ds, dc, o, rt[0], 0, 0, &portable,
                         vk_capture_get_mixed_targets(rt, n, portable.raster_samples))) return;
        LOG_LIMIT(4, "vulkan: requested mixed-sample coverage is unsupported for these attachments or shaders\n");
        g_vk_skipped++;
        return;
    }
    /* Do not ask the native coverage unit to repeat equal-count modulation. Some devices ignore that state;
       others may apply it, so the shader multiplier is the sole implementation on every host. */
    if (coverage.raster_samples == tf.samples) coverage = (VkCoverage){0};
    VkSampleLocations sample_locations;
    u32 raster_samples = coverage.raster_samples ? coverage.raster_samples : tf.samples ? tf.samples : 1;
    if (ds->rkey.conservative && (dc->subpixel_bias_x || dc->subpixel_bias_y))
        LOG_LIMIT(1, "vulkan: conservative raster precision bias uses the device precision\n");
    if (!vk_sample_locations_key(&sample_locations, dc, raster_samples, &g_vk.sample_locations,
                                  (ds->dyn.window_origin & DS_ORIGIN_LOWER_LEFT) != 0, ds->dyn.clip_height) ||
        (sample_locations.samples && zt && !zt->sample_locations_compatible)) {
        LOG_LIMIT(4, "vulkan: custom sample locations exceed this device's exact grid or depth-image support\n");
        g_vk_skipped++;
        return;
    }
    if (kept && memcmp(&coverage, &ke->k.coverage, sizeof coverage)) kept = 0;
    if (kept && (memcmp(&sample_locations, &ke->k.sample_locations, sizeof sample_locations) ||
                 to_color != ke->k.coverage_to_color)) kept = 0;
    if (kept && memcmp(&native_blend, &ke->k.advanced, sizeof native_blend)) kept = 0;
    u32 fs_spec = fs ? vk_pipe_fs_spec(fs->si.spec, ds->dyn.alpha_test, ds->dyn.alpha_func,
                                       (ds->dyn.window_origin & DS_ORIGIN_LOWER_LEFT) != 0, ds->dyn.sprite_origin)
                     : 0;
    if (kept && fs_spec != ke->k.fs_spec) kept = 0;
    PipeKey made_k;
    VertexPlan made_pl;
    const PipeKey* k = kept ? &ke->k : &made_k;
    const VertexPlan* pl = kept ? &ke->plan : &made_pl;
    if (!kept || check) {
        if (!draw_key(ds, &drawn, fs, rt, zt, n, &made_k, &made_pl)) {
            if (kept) {
                draw_differs("pipeline key");
                draw_drop(found);
            }
            g_vk_skipped++;
            return;
        }
        made_k.coverage = coverage;
        made_k.fs_spec = fs_spec;
        made_k.advanced = native_blend;
        made_k.sample_locations = sample_locations;
        made_k.coverage_to_color = to_color;
        if (capture) {
            made_k.logic_op = 0;
            for (u32 i = 0; i < n; i++) {
                memset(&made_k.blend[i], 0, sizeof made_k.blend[i]);
                made_k.blend[i].mask = i + 1 == n ? 15 : fs->si.sph[18] >> (i * 4) & 15;
            }
        }
        /* a save attachment: written only by the save draw, with no blend; and the save draw writes nothing else */
        if (saves || g_predraw.phase == 1) {
            if (g_predraw.phase == 1) made_k.logic_op = 0;
            for (u32 i = 0; i < n; i++) {
                int save = rt[i] && rt[i]->predraw;
                if (!save && g_predraw.phase != 1) continue;
                memset(&made_k.blend[i], 0, sizeof made_k.blend[i]);
                made_k.blend[i].mask =
                    save && g_predraw.phase == 1 && (g_predraw.wmask >> i & 1) ? 15 : 0;
            }
        }
        if (programmable_blend) {
            made_k.ncolor = 0;
            memset(made_k.color, 0, sizeof made_k.color);
            memset(made_k.blend, 0, sizeof made_k.blend);
        }
        if (coverage.raster_samples) {
            made_k.sample_off = ~ds->rkey.sample_mask & ((1u << coverage.raster_samples) - 1);
            made_k.alpha_to_coverage = ds->rkey.alpha_to_coverage && n && rt[0];
            made_k.alpha_to_one = ds->rkey.alpha_to_one && n && rt[0] && g_vk.feat.alphaToOne;
        }
        if (alpha_integer || (fk.advanced_mask & SH_ALPHA_COVERAGE)) {
            made_k.alpha_to_coverage = 0;
            made_k.alpha_to_one = 0;
        }
        if (fk.advanced_mask & SH_ALPHA_TO_ONE) made_k.alpha_to_one = 0;
        if (ratio_one_flags && (dc->coverage_mode & 2)) made_k.alpha_to_one = 0;
        if (kept && memcmp(&made_pl, pl, sizeof made_pl)) {
            draw_differs("vertex bindings");
            kept = 0;
        }
        k = &made_k;
        pl = &made_pl;
    }
    /* each binding's buffer: all of them prefetched first (vk_buf_prefetch), then looked up */
    VkBuffer vbuf[33];
    VkDeviceSize voff[33], vsize[33];
    struct {
        u64 gva, len;
        u32 b, base;
    } vq[33];
    u32 nvq = 0;
    /* each stream up to the last vertex or instance the draw reads, rather than to its limit, where the limit is far:
       a ring the game streams vertices through. A stream of VB_WHOLE bytes or less binds whole: a miss copies no more
       than the rounding below can, and an indexed draw then needs no scan of its indices for the last vertex, which
       was a quarter of one game's vertex buffer setup. The scan runs once, for the first stream that is larger */
    u64 last_vertex = ~0ull;
    int last_known = !pl->vi.nbind || dc->indirect;
    u64 last_instance = dc->indirect ? ~0ull : (u64)base_instance + instances - 1;
    for (u32 v = 0; v < pl->vi.nbind; v++) {
        if (pl->key[v] == ~0u || pl->key[v] == ~1u) {
            /* the constants: staging that fails skips the draw (it once drew without the attributes) */
            if (!frame_consts(&vbuf[v], &voff[v])) {
                g_vk_skipped++;
                return;
            }
            int zero = pl->key[v] == ~1u;
            voff[v] += zero ? 48 : 16;
            vsize[v] = zero ? 16 : 32; /* typed zero, or float/integer defaults */
            continue;
        }
        u32 sbuf = pl->key[v] & 31;
        /* up to a power of two of the bytes it reads: the cache keeps a range by its length, and draws of a mesh's parts
           that end at different vertices then share a few ranges, not one each */
        u64 limit = stream_len(&ds->vbuf, sbuf), len = limit;
        u64 gstride = pl->guest_stride[v] ? pl->guest_stride[v] : pl->stride[v];
        if (!last_known && !pl->vi.bind[v].divisor && limit > VB_WHOLE) {
            last_vertex = draw_last_vertex(dc->inline_indices, &ds->index, indexed, first, count, base_vertex);
            last_known = 1;
        }
        u64 last = pl->vi.bind[v].divisor ? last_instance : limit > VB_WHOLE ? last_vertex : ~0ull;
        if (gstride && last < len / gstride) {
            u64 need = (last + 1) * gstride, up = 4096;
            while (up < need) up <<= 1;
            if (up < len) len = up;
        }
        if (len > (64u << 20)) len = 64u << 20;
        /* Overlapping records can need the attribute's tail beyond a stride,
           but never beyond the guest binding's inclusive limit. */
        len += pl->pad[v] < limit - len ? pl->pad[v] : limit - len;
        u32 base = pl->key[v] >> 5;
        if (base >= len) {
            if (!frame_consts(&vbuf[v], &voff[v])) {
                g_vk_skipped++;
                return;
            }
            voff[v] += 48;
            vsize[v] = 0;
            continue;
        }
        vq[nvq].gva = stream_start(&ds->vbuf, sbuf);
        vq[nvq].len = len;
        vq[nvq].b = v;
        vq[nvq].base = base;
        nvq++;
    }
    for (u32 q = 0; q < nvq; q++) vk_buf_prefetch(vq[q].gva, vq[q].len, 0);
    for (u32 q = 0; q < nvq; q++) vk_buf_prefetch(vq[q].gva, vq[q].len, 1);
    for (u32 q = 0; q < nvq; q++) {
        u32 b = vq[q].b;
        if (pl->guest_stride[b]) {
            if (!restride(vq[q].gva + vq[q].base, vq[q].len - vq[q].base, pl->guest_stride[b], (u32)pl->stride[b],
                          &vbuf[b], &voff[b], &vsize[b])) {
                g_vk_skipped++;
                return;
            }
            continue;
        }
        /* vertex formats have components of up to 4 bytes, which each attribute's address must be a multiple of. A
           program that fetches its own attributes loads the aligned words that hold them: its streams take the
           guest's 4-byte words around them, in the same pages, so that the word with a stream's last bytes is bound
           whole (the record's length stays the stream's own) */
        const u64 lead = k->fetch ? vq[q].gva & 3 : 0;
        const u64 gva = vq[q].gva - lead, glen = k->fetch ? ((vq[q].gva + vq[q].len + 3) & ~3ull) - gva : vq[q].len;
        if (!vk_buf_get_input(gva, glen, 4, &vbuf[vq[q].b], &voff[vq[q].b])) {
            g_vk_skipped++;
            return;
        }
        voff[vq[q].b] += lead + vq[q].base;
        vsize[vq[q].b] = vq[q].len - vq[q].base;
    }
    /* an indirect draw's arguments and count, as the GPU holds them (a compute program's results that haven't landed
       read where they are) */
    VkBuffer abuf = VK_NULL_HANDLE, cntbuf = VK_NULL_HANDLE;
    u32 astride = dc->stride; /* an indirect draw's bytes from a record to the next */
    u64 aoff = 0, cntoff = 0;
    if (dc->indirect &&
        (mesh ? !mesh_indirect(dc, &abuf, &aoff) : !indirect_args(dc, indexed, &abuf, &aoff, &cntbuf, &cntoff))) {
        g_vk_skipped++;
        return;
    }
    VkPrimitiveTopology topo = topology_vk(topology);

    /* descriptor sets (may record uploads and barriers, so before the pass opens) */
    cb_prefetch(ds, vs_bind, vs, 1);
    if (fs) cb_prefetch(ds, 4, fs, 1);
    if (!dc->indirect && !mesh) draw_indices_prefetch(&ds->index, indexed, first, count, 1);
    VkDrawStorage storage;
    VkSh* staged[VK_DRAW_STAGES] = {mesh ? NULL : vs, mesh ? NULL : tcs, tes, gs, fs};
    if (mesh) {
        staged[DS_MESH] = vs;
        staged[DS_TASK] = tcs;
    }
    draw_storage_read(ds, &storage, staged);
    if (k->fetch) fetch_records(pl, vbuf, voff, vsize, storage.attr, storage.divisor);
    if (late_depth) {
        int overlaps = 0;
        for (u32 stage = 0; stage < VK_DRAW_STAGES; stage++)
            for (int i = 0; i < storage.n[stage]; i++)
                if (vk_depth_shadow_range_aliases(zt, storage.b[stage][i].addr, storage.b[stage][i].size)) overlaps = 1;
        for (u32 i = 0; i < 4 && xfb_on; i++)
            if (ds->bind.xfb[i].on && (i32)ds->bind.xfb[i].size > 0 &&
                vk_depth_shadow_range_aliases(zt, ds->bind.xfb[i].addr, ds->bind.xfb[i].size)) overlaps = 1;
        if (overlaps) {
            g_vk_skipped++;
            return;
        }
    }
    int alias = vk_draw_storage_any(&storage) ? vk_buf_draw_has_alias(&storage) : 0;
    if (alias < 0) {
        g_vk_skipped++;
        return;
    }
    if (vk_draw_storage_any(&storage) && !vk_buf_draw_prepare(&storage)) {
        g_vk_skipped++;
        return;
    }
    if (late_depth && !vk_depth_shadow_reserve(zt, &g_capture_draw.shadow)) {
        vk_depth_shadow_cancel();
        g_vk_skipped++;
        return;
    }
    u32 predraw = 0; /* the draw's number for its pre-draw capture (vk/predraw.c), 0 for none */
    if (fs && fs->si.predraw_slots) {
        u32 w = 0, h = 0;
        for (u32 j = 0; j < n; j++) {
            if (rt[j] && rt[j]->sw > w) w = rt[j]->sw;
            if (rt[j] && rt[j]->sh > h) h = rt[j]->sh;
        }
        if (!vk_predraw_reserve(w, h, snx_popcount64(fs->si.predraw_slots))) {
            LOG_LIMIT(1, "vulkan: no images for the pre-draw capture of %ux%u: draws that read their targets are "
                         "skipped\n", w, h);
            g_vk_skipped++;
            return;
        }
        /* the captured slots' targets, in the order of their side images (shader/spirv_translate.c
           predraw_prologue), and what else decides the texel a slot fetches from its target: the header's swizzle
           (tex_map) and the sRGB reading (tex_srgb) */
        const Surf* targets[SH_PREDRAW_MAX];
        u32 views[SH_PREDRAW_MAX], ntargets = 0;
        for (int i = 0; i < SH_MAXTEX && ntargets < SH_PREDRAW_MAX; i++)
            if (fs->si.predraw_slots >> i & 1) {
                const TexRes* x = res && res[i].have ? &res[i] : NULL;
                views[ntargets] = x ? (x->tic[0] >> 19 & 0xFFFu) | (u32)tex_srgb(x->tic, x->tsc) << 12 : 0;
                targets[ntargets++] = x ? x->t : NULL;
            }
        predraw = vk_predraw_next(targets, views, ntargets);
    }
    int zt_ro = 0;
    u32 fb = 0;
    u64 scaled[2] = {0};
    DrawImages images; /* read below its count alone (a pattern past it with draw_state_check=1, as the storage) */
    if (g_settings.draw_state_check) memset(&images, 0xA5, sizeof images);
    images.n = images.sampled_general = 0;
    for (u32 i = 0; i < VK_DRAW_STAGES; i++)
        if (staged[i] && staged[i]->si.nimg) images.sampled_general = 1;
    /* transform feedback without the extension: the buffers the vertex program stores into */
    XfbBind xfb_stores = {0};
    if (xfb_on && !g_vk.has_xfb &&
        !xfb_bind(ds, o, vs, indexed, dc->indirect != 0, topology, first, count, base_instance, instances,
                  &xfb_stores)) {
        g_vk_skipped++;
        return;
    }
    ScreenSlots screen = screen_slots(ds, fs, vs, !tcs && !tes && !sh.gs && !sh.swizzle);
    g_predraw.probe = !predraw_refused && !g_predraw.phase && !capture && !texture && !mesh && !programmable_blend && !late_depth &&
                      !xfb_on && predraw_enabled();
    if (!g_predraw.phase) g_predraw.n = g_predraw.in_place = 0; /* the two draws keep what the probe found */
    /* The compute-task path's buffers for this draw: the payloads of its task workgroups, their counts and the draw's
       record (sm86.h), and the vec4 of the first workgroup and the groups the buffer holds, which both stages take
       from SNX_VKB_RS. Its task program's dispatch then writes them, before the pass opens */
    if (mesh && sh.task_compute) {
        VkBuffer ub = VK_NULL_HANDLE;
        u64 uo = 0;
        /* an indirect draw's first workgroup is its record's, which mesh_indirect copied and bound at SNX_VKB_RS */
        u8* host = dc->indirect ? NULL : vk_stage(16, vk_stage_align(), &ub, &uo);
        g_mesh_cap = sm86_mesh_task_cap(sh.payload_stride);
        g_mesh_payload = mesh_payload();
        if ((!dc->indirect && !host) || !g_mesh_payload || !g_mesh_cap) {
            draw_images_discard(&images);
            if (late_depth) vk_depth_shadow_cancel();
            g_vk_skipped++;
            return;
        }
        g_mesh_payload_bytes = SNX_MESH_PAYLOAD_BYTES;
        g_mesh_groups = task_groups > g_mesh_cap ? g_mesh_cap : task_groups;
        if (task_groups > g_mesh_cap)
            LOG_LIMIT(4,
                      "vulkan: a mesh draw of %u task workgroups keeps the first %u, the most the compute-task path's "
                      "buffer holds for its payload\n",
                      task_groups, g_mesh_cap);
        if (host) {
            u32 words[4] = {first, 0, 0, 0};
            memcpy(host, words, sizeof words);
            snx_le32_array(host, 4); /* read by the device little-endian */
            g_mesh_first = ub, g_mesh_first_off = uo;
        }
    }
    VkDescriptorSet vset =
        stage_set(ds, vs_bind, vs, rt, zt, n, &zt_ro, NULL, NULL, NULL, &storage, &images, NULL, &xfb_stores);
    VkDescriptorSet fset = fs ? stage_set(ds, 4, fs, rt, zt, n, &zt_ro, &fb, scaled, res, &storage, &images,
                                          screen.slots ? &screen : NULL, NULL)
                              : VK_NULL_HANDLE;
    /* the sets of the stages between, at their stage's index (shader_vk_set) */
    between[0] = tcs;
    between[1] = tes;
    between[2] = gs;
    bset_ok = 1;
    for (b = 0; b < 3; b++) {
        bset[b] = VK_NULL_HANDLE;
        if (!between[b]) continue;
        bset[b] = stage_set(ds, mesh ? DS_TASK : b + 1, between[b], rt, zt, n, &zt_ro, NULL, NULL, NULL, &storage,
                            &images, NULL, NULL);
        if (!bset[b]) bset_ok = 0;
    }
    if (!vset || (fs && !fset) || !bset_ok) {
        draw_images_discard(&images);
        if (late_depth) vk_depth_shadow_cancel();
        if (g_vk.has_descriptor_heaps) {
            g_vk.has_descriptor_heaps = 0;
            vk_descriptor_heaps_invalidate();
            draw_forget();
            goto retry;
        }
        g_vk_skipped++;
        return;
    }
    /* a draw that samples, at its own pixel, a target that it writes: drawn two times, the first saving what the second
       reads (predraw_run) */
    if (mesh && sh.task_compute &&
        !mesh_task_dispatch(tcs, k->arrays, bset[0], dc->indirect != 0, abuf, aoff, g_mesh_groups)) {
        draw_images_discard(&images);
        if (late_depth) vk_depth_shadow_cancel();
        LOG_LIMIT(1, "vulkan: a mesh draw's task program has no compute pipeline (skipped)\n");
        g_vk_skipped++;
        return;
    }
    if (g_predraw.probe) {
        g_predraw.probe = 0;
        const VkSh* staged_pre[] = {vs, tcs, tes, gs};
        /* the slots the guest binds no target to (read_targets keeps the last answer: no lookup again) */
        Surf* guest[8];
        Surf* guest_zt;
        u32 guest_n, free = 0;
        read_targets(ds, guest, &guest_zt, &guest_n);
        for (u32 j = 0; j < DS_TARGETS_MAX; j++)
            if (!guest[j]) free |= 1u << j;
        const char* why = !g_predraw.n || !fs || !predraw_writes_sampled(ds, fs, rt, n) ? NULL
                          : !predraw_pre_raster_pure(staged_pre)                         ? "stores from its vertex stages"
                          : dc->raster_samples                                           ? "has more raster samples"
                          : dc->multisample_flags & DS_MS_COVERAGE_TO_COLOR              ? "writes coverage to color"
                          : dc->advanced_blend[0]                                        ? "blends in advanced modes"
                          : ds->rkey.alpha_to_coverage                                   ? "takes alpha to coverage"
                                                                                         : "";
        if (why && !*why) {
            draw_images_discard(&images);
            why = predraw_run(ds, dc, o, free, fs);
            if (!why) return;
            g_predraw.n = g_predraw.in_place = 0; /* drawn once, in place, as before */
            predraw_refused = 1;
        }
        if (why) {
            predraw_refuse(why);
            if (predraw_refused) goto retry;
        }
        g_predraw.n = g_predraw.in_place = 0;
    }
    /* indices, up to the end of the index buffer (inclusive). What is made into other indices on the CPU (quads,
       8-bit indices, another restart index) is capped */
    VkBuffer ibuf = VK_NULL_HANDLE;
    u64 ioff = 0;
    VkIndexType itype = VK_INDEX_TYPE_UINT32;
    u32 ifmt = indexed == 3 ? 2 : ds->index.format, isz = ifmt == 0 ? 1 : ifmt == 1 ? 2 : 4;
    u64 ia = ds->index.addr, iend = ds->index.end;
    if (indexed && indexed != 3 && iend >= ia && (u64)first + count > (iend - ia + 1) / isz) {
        u64 avail = (iend - ia + 1) / isz;
        LOG_LIMIT(1, "vulkan: a draw reads indices %u..%u of an index buffer of %llu (cut there)\n", first,
                  first + count - 1, (unsigned long long)avail);
        if (first >= avail) {
            draw_images_discard(&images);
            if (late_depth) vk_depth_shadow_cancel();
            return;
        }
        count = (u32)(avail - first);
    }
    if ((topology == 7 || topology == 8 || (indexed && ifmt == 0)) && count > (1u << 24)) {
        LOG_LIMIT(1, "vulkan: a draw of %u vertices made into other indices (cut to %u)\n", count, 1u << 24);
        count = 1u << 24;
    }
    /* Vulkan restarts at all ones of the index type, in strips and fans, and in lists (patch lists apart) as the 3D
       engine does where the device has VK_EXT_primitive_topology_list_restart; the guest's restart index is 0x592 */
    int list = topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
               topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ||
               topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    int list_restart = topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST ? g_vk.has_patch_list_restart : g_vk.has_list_restart;
    int restart = ds->index.restart && ((!list && topo != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) || list_restart);
    u32 icount = count;
    i32 voffset = (i32)base_vertex; /* the draw's vertexOffset */
    if (edges) {
        edges = edge_indices(ds, dc, indexed, isz, first, count, topology, points, &ibuf, &ioff, &icount);
        if (edges < 0) {
            draw_images_discard(&images);
            if (late_depth) vk_depth_shadow_cancel();
            return;
        }
        if (!edges) icount = count;
    }
    if (edges) {
        if (!indexed) voffset = (i32)first; /* the vertex numbers count from the draw's first */
        indexed = 2;                        /* drawn indexed, as a list without restart */
        itype = VK_INDEX_TYPE_UINT32;
        topo = points ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    } else if (dc->indirect) {
        int made = 1;
        if (topology == 7) { /* two triangles a quad, as a direct draw's (below) */
            /* the vertices the longest stream that steps by vertex holds: no record of vertices draws more */
            u64 held = 0;
            for (u32 v = 0; v < pl->vi.nbind; v++) {
                const u64 step = pl->guest_stride[v] ? pl->guest_stride[v] : pl->stride[v];
                if (pl->key[v] == ~0u || pl->key[v] == ~1u || pl->vi.bind[v].divisor || !step) continue;
                const u64 holds = stream_len(&ds->vbuf, pl->key[v] & 31) / step;
                held = holds > held ? holds : held;
            }
            made = indirect_quads(dc, ds, indexed, held, &abuf, &aoff, &ibuf, &ioff);
            astride = 20; /* the records made, one after another */
            indexed = 2;
            itype = VK_INDEX_TYPE_UINT32;
            topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        } else if (indexed)
            made = indirect_indices(&ds->index, restart, &ibuf, &ioff, &itype) ||
                   indirect_indices_made(dc, &ds->index, restart, &ibuf, &ioff, &itype);
        if (!made) {
            LOG_LIMIT(1, "vulkan: an indirect draw of %s isn't drawn\n",
                      topology == 7 ? "quads that can't be made into triangles"
                                    : "indices that can't be made into ones Vulkan reads");
            draw_images_discard(&images);
            if (late_depth) vk_depth_shadow_cancel();
            g_vk_skipped++;
            return;
        }
    } else if (topology == 7 || (topology == 8 && !(indexed && ds->index.restart &&
                                                    !quad_cpu_reads(indexed == 3, ia, isz, first, count)))) {
        /* quads, and quad strips, -> two triangles each that carry the quad's provoking vertex (quad_indices.h). A
           strip whose restart index only the GPU has stays the triangle strip it covers the same pixels as, which
           restarts there, its quads' second halves taking the colour of the vertex before their provoking one */
        u32 flags = (ds->rkey.provoking_last ? QUADS_LAST : 0) | (topology == 8 ? QUADS_STRIP : 0);
        u32 nq = snx_quads_of(count, topology == 8);
        u32 ones = isz == 1 ? 0xFFu : isz == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        i64 made = -1;
        if (indexed) {
            made = quad_indices_cpu(dc, indexed == 3, ia, isz, first, count, flags, ds->index.restart != 0,
                                    ds->index.restart_index & ones, &ibuf, &ioff);
            if (made < 0 && ds->index.restart)
                LOG_LIMIT(1, "vulkan: quads of %u indices the CPU can't read take restart index %#x for a vertex\n",
                          count, ds->index.restart_index & ones);
            if (made < 0 && index_convert(dc, indexed == 3, ia, isz, first, count, TC_INDEX_QUADS, isz, flags, nq,
                                          (u64)nq * 24, &ibuf, &ioff))
                made = nq;
        } else if (quad_indices(nq, flags, &ibuf, &ioff))
            made = nq;
        if (made < 0) {
            draw_images_discard(&images);
            if (late_depth) vk_depth_shadow_cancel();
            return;
        }
        if (!indexed) voffset = (i32)first; /* the vertex numbers count from the draw's first */
        icount = (u32)made * 6;
        indexed = 2; /* drawn indexed, as triangles */
        topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        /* a strip's made indices hold no restart index, and a list restarts on few devices */
        if (topology == 8) restart = 0;
    } else if (indexed) {
        u32 ones = isz == 1 ? 0xFFu : isz == 2 ? 0xFFFFu : 0xFFFFFFFFu, ri = ds->index.restart_index & ones;
        u64 bytes = (u64)count * isz;
        itype = isz == 1 ? VK_INDEX_TYPE_UINT8_KHR : isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        if (indexed == 3) { /* inline indices, host words: the guest's restart index made Vulkan's as they're copied */
            u32* o = (u32*)vk_stage(bytes, vk_stage_align(), &ibuf, &ioff);
            if (!o) {
                draw_images_discard(&images);
                if (late_depth) vk_depth_shadow_cancel();
                return;
            }
            if (restart && ri != ones)
                for (u32 j = 0; j < count; j++) {
                    u32 v = dc->inline_indices[first + j];
                    o[j] = snx_le32(v == ri ? ones : v);
                }
            else {
                memcpy(o, dc->inline_indices + first, (size_t)bytes);
                snx_le32_array(o, count); /* host words, read by the device little-endian */
            }
        } else if (isz == 1 && !(g_vk.has_index8 && (!ds->index.restart || (restart && ri == ones)))) {
            /* 8-bit indices Vulkan can't read, or whose restart index the guest has elsewhere: widened to 16 bits,
               the restart index (when on, whatever the topology) becoming 0xFFFF */
            if (!index_convert(dc, 0, ia, 1, first, count, TC_INDEX_WIDEN8, ds->index.restart != 0, ri, (count + 1) / 2,
                               ((u64)count + 1) / 2 * 4, &ibuf, &ioff)) {
                draw_images_discard(&images);
                if (late_depth) vk_depth_shadow_cancel();
                return;
            }
            itype = VK_INDEX_TYPE_UINT16;
        } else if (isz != 1 && restart && ri != ones && count <= (1u << 24)) { /* the restart index becomes Vulkan's */
            u32 n = isz == 2 ? (count + 1) / 2 : count;
            if (!index_convert(dc, 0, ia, isz, first, count, isz == 2 ? TC_INDEX_RESTART16 : TC_INDEX_RESTART32, 0, ri,
                               n, (u64)n * 4, &ibuf, &ioff)) {
                draw_images_discard(&images);
                if (late_depth) vk_depth_shadow_cancel();
                return;
            }
        } else if (!vk_buf_get_input(ia + (u64)first * isz, bytes, isz, &ibuf, &ioff)) {
            draw_images_discard(&images);
            if (late_depth) vk_depth_shadow_cancel();
            return;
        }
    }

    /* the transform feedback buffers (their enable, address, size and start offset), from the start offset on as
       Eden's UpdateTransformFeedbackBuffer binds them: a writable copy of guest memory each, which lands there and
       which later draws read on the GPU until then (vk_buf_copy_writable) */
    VkBuffer xbuf[4];
    VkDeviceSize xoff[4], xsize[4];
    u32 xbound = 0;
    for (int i = 0; i < 4 && xfb_on && g_vk.has_xfb; i++) {
        i32 size = (i32)ds->bind.xfb[i].size;
        u64 a = ds->bind.xfb[i].addr;
        if (!ds->bind.xfb[i].on || size <= 0) continue;
        if (!vk_buf_copy_writable(a, (u32)size, &xbuf[i], &xoff[i], 1)) {
            draw_images_discard(&images);
            if (late_depth) vk_depth_shadow_cancel();
            g_vk_skipped++;
            return;
        }
        xsize[i] = (u32)size;
        xbound |= 1u << i;
    }
    /* the feedback loop goes into the key last: stage_set found it. So do the topology class and polygon mode, which
       a draw made into lines or points changes (edge_indices) and the kept key may hold from the last draw */
    u32 feedback = fb && !g_vk.has_feedback_dynamic;
    u32 topo_class = edges ? (u32)topo : mesh ? 0 : topology_class(topology_vk(topology));
    u32 polygon = edges ? VK_POLYGON_MODE_FILL : polygon_mode(&ds->rkey);
    if (k == &made_k) {
        made_k.feedback = feedback;
        made_k.topo_class = topo_class;
        made_k.polygon = polygon;
    } else if (k->feedback != feedback || k->topo_class != topo_class || k->polygon != polygon) {
        made_k = *k;
        made_k.feedback = feedback;
        made_k.topo_class = topo_class;
        made_k.polygon = polygon;
        k = &made_k;
        kept = 0;
    }
    /* compared each draw, as above, where draw_state_check made the key again: a kept key of another feedback, topology
       class or polygon mode isn't a difference the check reports. The same state drawn directly and indirectly, or as
       polygons made into lines on the CPU and not, is one key of the table with two of these */
    if (kept && (ke->k.feedback != feedback || ke->k.topo_class != topo_class || ke->k.polygon != polygon)) kept = 0;
    if (kept && check && memcmp(k, &ke->k, sizeof *k)) {
        draw_differs("pipeline key");
        kept = 0;
    }
    /* the kept pipeline, or the found or the last draw's entry's when its key is the same (draws in a row mostly are):
       one comparison of the key, where the table hashed all of it each draw. Pipelines and modules live as long as the
       run, so the handle stays good, a stand-in for one still being made too (vk_pipeline_get_deferred) */
    VkPipeline pipe;
    const DrawKept* pe = ke ? ke : g_draw.last ? &g_draw.e[g_draw.last] : NULL;
    if (kept) {
        pipe = ke->pipe;
        if (check) {
            VkPipeline made = vk_pipeline_get_deferred(k);
            if (vk_pipeline_ready(made) != vk_pipeline_ready(pipe)) {
                draw_differs("pipeline");
                kept = 0;
                pipe = made;
            }
        }
    } else if (pe && pe->pipe && !memcmp(k, &pe->k, sizeof *k))
        pipe = pe->pipe;
    else
        pipe = vk_pipeline_get_deferred(k);
    if (!pipe) {
        draw_images_discard(&images);
        if (late_depth) vk_depth_shadow_cancel();
        g_vk_skipped++;
        return;
    }
    /* A guest read while preparing descriptors can submit and land an older graphics output into memory these
       descriptors and staged inputs were read from. Rebuild them before recording the draw. */
    g_setup_restarts = 0;
    if (setup_frame != vk_submit_number()) {
        draw_images_discard(&images);
        if (late_depth) vk_depth_shadow_cancel();
        if (++retries < 4) goto retry;
        g_vk_skipped++;
        return;
    }
    /* kept for later draws: in the entry found when it didn't hold, else in a way of the table's set of the draw's
       key */
    g_draw.fs = fs;
    if (found && !kept) g_stats.n.late++;
    if (kept) {
        g_vk_draw_kept++;
        ke->used = ++g_draw.use;
        g_draw.last = found;
    } else if (pipe && saves)
        draw_forget(); /* a draw with save attachments keeps nothing for the next draw (it has other targets) */
    else if (pipe && own) {
        if (!have_key) draw_memo_key(ds, draw_topo, mesh_path, 0, &key);
        u32 slot = draw_slot(&key, found);
        DrawKept* e = &g_draw.e[slot];
        e->key = key;
        g_stats.n.made++;
        e->epoch = epoch;
        e->used = ++g_draw.use;
        e->shader_generation = g_vk.shader_generation;
        /* the translations as they are after this draw's lookups, which can have made some */
        e->fold_gen = vk_shader_fold_gen();
        e->sh = sh;
        e->fkey = fk;
        e->fs = fs;
        const VkSh* used[6];
        used_shaders(e, used);
        e->not_top = 0;
        for (int i = 0; i < 6; i++)
            if (used[i] && !vk_shader_fold_top(used[i])) e->not_top |= 1u << i;
        e->tf = tf;
        if (pl != &e->plan) e->plan = *pl;
        if (k != &e->k) e->k = *k;
        e->pipe = pipe;
        memcpy(e->gen, ds->gen, sizeof e->gen);
        e->vfmt = ds->vfmt;
        e->vstreams = 0;
        for (u32 m = vs->si.in_mask; m; m &= m - 1) e->vstreams |= 1u << ds->vfmt.stream[snx_ctz32(m)];
        g_draw.last = slot;
    }
    /* the color slots the draw writes */
    u32 writes = 0;
    if (fs)
        for (u32 i = 0; i < n; i++)
            if (rt[i] && (k->blend[i].mask & ((fs->si.sph[18] >> (i * 4)) & 15))) writes |= 1u << i;
    if (capture) writes |= 1u << (n - 1);
    /* a buffer that captured since its write pointer was loaded resumes from its counter, which the last capture's end
       wrote: the end is ordered before the begin outside a pass */
    u32 resume = xbound & o->xfb_started & ~o->xfb_reload;
    if (resume) {
        vk_render_end();
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT};
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        rec_CmdPipelineBarrier2(vk_cmd(), &di);
    }
    int zt_undefined = zt && zt->layout == VK_IMAGE_LAYOUT_UNDEFINED;
    /* All graphics stages have resolved their views now. A later stage can snapshot RT0 or sample a surface an
       earlier stage stores. Use one layout for sampled/storage aliases and retain the draw's complete write scope. */
    for (int i = 0; i < images.n; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++) if (images.surf[j] == images.surf[i]) { seen = 1; break; }
        if (!seen)
            vk_surf_use(vk_cmd(), images.surf[i], VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                        VK_STORAGE_IMAGE_ACCESS | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    if (programmable_blend)
        vk_surf_use(vk_cmd(), rt[0], VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_STORAGE_IMAGE_ACCESS);
    if (late_depth) {
        /* Import only after all guest resources have resolved. It must follow earlier draws in this submission. */
        int alias = 0;
        for (int i = 0; i < images.n; i++)
            if (vk_depth_shadow_aliases(zt, images.surf[i])) alias = 1;
        for (int i = 0; fs && res && i < fs->si.ntex; i++)
            if (res[i].have && res[i].t &&
                vk_depth_shadow_aliases(zt, res[i].t)) alias = 1;
        VkDescriptorBufferInfo actual;
        if (alias || !vk_depth_shadow_prepare(zt, &ds->dyn, dc, ds->rkey.sample_mask, &actual) ||
            actual.buffer != g_capture_draw.shadow.buffer || actual.offset != g_capture_draw.shadow.offset ||
            actual.range != g_capture_draw.shadow.range) {
            vk_depth_shadow_cancel();
            draw_images_discard(&images);
            g_vk_skipped++;
            return;
        }
        zt_ro = 1;
    }
    int programmable_pass = 1;
    if (programmable_blend) {
        if (zt) {
            Surf* detached[8] = {0};
            /* The storage attachment is absent from the pass. Keep depth/stencil tests and their write ordering. */
            vk_render_end();
            programmable_pass = render_begin(detached, zt, 0, zt_ro, 0, 0, bound);
            if (g_rp.layers > rt[0]->layers) g_rp.layers = rt[0]->layers;
        } else
            programmable_pass = render_begin_empty_area(bound, rt[0]->layers);
    }
    /* Strict for a save draw (predraw_run), and for a feedback draw that the two draws could not take and samples in
       place: each reads the targets after every earlier draw of the pass wrote them. Between two of the game's
       barriers the console's draws all read the targets as they were at the barrier (feedback loops, above), and the
       interlock's capture gives them that (vk/predraw.c). Here a draw read in place what the draws just before it
       wrote or had not written yet: crossing rock props that blend into what lies under them read each other in one
       frame and the cliff in the next, in patches. Ordered, each reads the draws before it, as a copy of the target
       before each draw gives too, the same every frame. Only with dynamicRenderingLocalRead: without it the order
       would begin the pass again at each such draw */
    int strict_feedback = g_predraw.phase == 1 || (!g_predraw.phase && fb && g_vk.feedback_mode == VK_FEEDBACK_PREDRAW &&
                                                   g_vk.has_local_read);
    g_rp.rate_want = dc->shading_rate;
    int begun = programmable_blend ? programmable_pass
                : !n && !zt        ? render_begin_empty()
                                   : render_begin(rt, zt, n, zt_ro, ds->tgt.srgb_write, fb | predraw_pass_slots(rt, n),
                                                  bound) &&
                                  feedback_order(fb, writes, strict_feedback);
    g_rp.rate_want = RATE_KEEP;
    if (!begun) {
        draw_images_discard(&images);
        if (late_depth) vk_depth_shadow_cancel();
        return;
    }
    if (zt && zt->sample_locations_compatible)
        zt->sample_locations = sample_locations.samples ? sample_locations
                                                        : vk_sample_locations_standard(raster_samples);
    rp_record();
    VkCommandBuffer cb = vk_cmd();
    /* only what differs from the previous draw's in this command buffer (g_dyn) */
    u64 heap_epoch = vk_descriptor_heaps_epoch();
    int fresh = g_dyn.cb != vk_cb_begun() || (g_vk.has_descriptor_heaps && g_dyn.heap_epoch != heap_epoch) ||
                g_dyn.heaps != g_vk.has_descriptor_heaps;
    if (fresh) {
        memset(&g_dyn, 0, sizeof g_dyn);
        g_dyn.cb = vk_cb_begun();
        g_dyn.heap_epoch = heap_epoch;
        g_dyn.heaps = g_vk.has_descriptor_heaps;
    }
    if (g_vk.has_descriptor_buffers && g_dyn.set_binds != vk_rec_set_binds()) { /* the sets go again */
        memset(g_dyn.sets, 0, sizeof g_dyn.sets);
        g_dyn.set_binds = vk_rec_set_binds();
    }
    if (fresh || g_dyn.pipe != pipe) {
        g_dyn.pipe = pipe;
        rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        /* a mesh pipeline has no topology, restart or vertex input state: the next draw's pipeline that has them
           dynamic takes them again after it (dynamic state set before a pipeline that has it static is lost) */
        if (mesh) {
            g_dyn.topo = g_dyn.restart = ~0u;
            g_dyn.vi_set = 0;
            g_dyn.vb_n = 0; /* nor binding strides: the next draw with buffers binds them again */
        }
        /* nor has a pipeline whose vertex program fetches its own attributes vertex input or binding strides */
        if (k->fetch) {
            g_dyn.vi_set = 0;
            g_dyn.vb_n = 0;
        }
    }
    if (g_vk.dyn_pr) pr_dynamic(cb, k, !ds->dyn.depth_mode, fresh);
    vk_vrs_draw(cb, dc->shading_rate);
    vk_descriptor_heaps_pipeline_state(g_vk.has_descriptor_heaps);
    if (g_vk.has_feedback_dynamic)
        DYN(fb, fb ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_NONE,
            rec_CmdSetAttachmentFeedbackLoopEnableEXT(cb, fb ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_NONE));
    VkDescriptorSet sets[2] = {vset, fset};
    VkPipelineLayout layout = vk_pipeline_layout(pipe, k->arrays);
    if (g_dyn.layout != layout) { /* the sets bound through another layout are bound again */
        memset(g_dyn.sets, 0, sizeof g_dyn.sets);
        g_dyn.layout = layout;
    }
    if (fresh || g_dyn.sets[0] != vset || (fs && g_dyn.sets[1] != fset)) {
        g_dyn.sets[0] = vset;
        g_dyn.sets[1] = fs ? fset : VK_NULL_HANDLE;
        vk_descriptors_bind(cb, layout, 0, fs ? 2 : 1, sets);
    }
    for (b = 0; b < 3; b++)
        if (bset[b] && (fresh || g_dyn.sets[2 + b] != bset[b])) {
            g_dyn.sets[2 + b] = bset[b];
            vk_descriptors_bind(cb, layout, (u32)b + 2, 1, &bset[b]);
        }
    /* split sets: each present stage's texture set, five after its buffers' (shader_vk_tex_set) */
    for (u32 t = 0; t < 5 && g_spirv_translate.split_sets; t++) {
        const int present = t == 0 || (t == 1 ? fs != NULL : bset[t - 2] != VK_NULL_HANDLE);
        if (present && g_stage_tex_set[t] && (fresh || g_dyn.sets[5 + t] != g_stage_tex_set[t])) {
            g_dyn.sets[5 + t] = g_stage_tex_set[t];
            vk_descriptors_bind(cb, layout, 5 + t, 1, &g_stage_tex_set[t]);
        }
    }
    /* the texture table, after the stages' sets (table.c) */
    if (g_spirv_translate.tex_table) {
        VkDescriptorSet table = vk_table_set();
        const u32 at = g_spirv_translate.table_set;
        if (fresh || g_dyn.sets[at] != table) {
            g_dyn.sets[at] = table;
            vk_descriptors_bind(cb, layout, at, 1, &table);
        }
    }
    /* a program that fetches its own attributes takes an empty layout: its pipeline has the layout static and ignores
       it, but where the draw binds shader objects, a vertex shader object needs vkCmdSetVertexInputEXT in each
       command buffer, after each pipeline whose static layout invalidated it (VUID-vkCmdDrawIndexed-None-04914) */
    static const VertexPlan no_vertices;
    if (g_vk.vertex_input_dynamic && !mesh) vertex_input_set(cb, k->fetch ? &no_vertices : pl, fresh);
    /* the strides: the layout's with dynamic vertex input, else dynamic state of their own where the device has it */
    /* bound again only where they differ from the last draw's in this command buffer: every draw bound them, and one
       game's draws bind the same streams as the draw before in most of them */
    const VkDeviceSize* strides = !g_vk.vertex_input_dynamic && g_vk.has_dynamic_stride ? pl->stride : NULL;
    const u32 nvb = k->fetch ? 0 : pl->vi.nbind; /* a program that fetches its attributes binds no vertex buffers */
    if (nvb && (fresh || g_dyn.vb_n != nvb || (strides != NULL) != g_dyn.vb_strided ||
                memcmp(g_dyn.vb_buf, vbuf, nvb * sizeof *vbuf) || memcmp(g_dyn.vb_off, voff, nvb * sizeof *voff) ||
                memcmp(g_dyn.vb_size, vsize, nvb * sizeof *vsize) ||
                (strides && memcmp(g_dyn.vb_stride, strides, nvb * sizeof *strides)))) {
        rec_CmdBindVertexBuffers2(cb, 0, nvb, vbuf, voff, vsize, strides);
        g_dyn.vb_n = nvb;
        g_dyn.vb_strided = strides != NULL;
        memcpy(g_dyn.vb_buf, vbuf, nvb * sizeof *vbuf);
        memcpy(g_dyn.vb_off, voff, nvb * sizeof *voff);
        memcpy(g_dyn.vb_size, vsize, nvb * sizeof *vsize);
        if (strides) memcpy(g_dyn.vb_stride, strides, nvb * sizeof *strides);
    }

    u32 viewports = last->si.writes_viewport ? GUEST_VIEWPORTS : 1;
    /* Empty viewports still run pre-raster stages and transform feedback. The zero scissor below suppresses pixels. */
    VkViewport viewport[GUEST_VIEWPORTS] = {{0}};
    VkRect2D scissor[GUEST_VIEWPORTS] = {{{0}}};
    u32 sub = g_vk.props.limits.viewportSubPixelBits < g_vk.props.limits.subPixelPrecisionBits
                  ? g_vk.props.limits.viewportSubPixelBits
                  : g_vk.props.limits.subPixelPrecisionBits; /* the points' nudge below */
    for (u32 i = 0; i < viewports; i++) {
        viewport[i] = vk_viewport_from(&ds->dyn, &ds->vp, i);
        if (ds->vp.transform_on && vk_viewport_swizzle_drawn(&ds->vp, i) != (ds->vp.swizzle[i] & DS_SWIZZLE_MASK))
            LOG_LIMIT(4,
                      "vulkan: viewport %u asks for the coordinate swizzle %04x and draws with %04x: a swizzle other "
                      "than NegativeY is applied only as viewport 0's (%04x), to every viewport\n",
                      i, ds->vp.swizzle[i] & DS_SWIZZLE_MASK, vk_viewport_swizzle_drawn(&ds->vp, i),
                      ds->vp.swizzle[0] & DS_SWIZZLE_MASK);
        /* A point a pixel wide covers the pixel its window position falls in on the console (OpenGL's aliased point
           rule), so one exactly on a pixel border goes to the pixel after it. Vulkan covers the pixels whose center is
           in the point's square, and leaves a center on its edge to the device: RADV takes the pixel before. One game
           projects a cube map onto spherical harmonics by drawing every sample as a point at the middle of an 8x8
           target, window (4, 4), and reads texel (4, 4) back on the CPU: its ambient light came out zero, and rooms
           lit by it black. Points move up the guest's window axes (down a lower-left window's y) by 1/128 pixel, too
           little to see, or by a step of the coarser of the device's viewport and rasterizer subpixel grids when that
           is larger (Vulkan only requires 4 bits of the rasterizer's and none of the viewport's): a smaller move
           rounds away. Not at all on a viewport without subpixel bits. Only in the modes that game draws them in,
           the OpenGL point center (0x597) and half-integer pixel centers (0x649): the others' rules aren't known */
        if (topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST && !gs && !tes && ds->dyn.point_nudge && sub) {
            float e = vk_scf(1.0f / (float)(1u << (sub < 7 ? sub : 7)));
            viewport[i].x += e;
            viewport[i].y += (ds->dyn.window_origin & DS_ORIGIN_LOWER_LEFT) ? -e : e;
        }
        int area = viewport[i].width > 0 && viewport[i].height != 0;
        if (!area) /* An empty guest viewport clips everything. Vulkan still needs a valid transform. */
            viewport[i] = (VkViewport){0, 0, 1, 1, 0, 1};
        VkRect2D sc = {{0, 0}, area ? g_rp.ext : (VkExtent2D){0, 0}};
        if (area && (ds->vp.scissor_on >> i & 1)) {
            u32 xs = ds->vp.scissor_x[i], ys = ds->vp.scissor_y[i];
            i32 x0 = (i32)vk_scf(xs & 0xFFFF), x1 = (i32)vk_scf(xs >> 16);
            i32 y0 = (i32)vk_scf(ys & 0xFFFF), y1 = (i32)vk_scf(ys >> 16);
            if (ds->dyn.window_origin & DS_ORIGIN_LOWER_LEFT) {
                i32 clip_height = (i32)vk_scf((float)ds->dyn.clip_height);
                i32 old_y0 = y0;
                y0 = clip_height - y1;
                y1 = clip_height - old_y0;
            }
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > (i32)g_rp.ext.width) x1 = (i32)g_rp.ext.width;
            if (y1 > (i32)g_rp.ext.height) y1 = (i32)g_rp.ext.height;
            sc = x1 > x0 && y1 > y0 ? (VkRect2D){{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}}
                                    : (VkRect2D){{0, 0}, {0, 0}};
        }
        scissor[i] = sc;
    }
    if (fresh || g_dyn.viewports != viewports || memcmp(g_dyn.vp, viewport, viewports * sizeof *viewport)) {
        memcpy(g_dyn.vp, viewport, viewports * sizeof *viewport);
        rec_CmdSetViewportWithCount(cb, viewports, viewport);
    }
    if (fresh || g_dyn.viewports != viewports || memcmp(g_dyn.sc, scissor, viewports * sizeof *scissor)) {
        memcpy(g_dyn.sc, scissor, viewports * sizeof *scissor);
        rec_CmdSetScissorWithCount(cb, viewports, scissor);
    }
    g_dyn.viewports = viewports;
    /* The console clamps a fragment's depth to the viewport's clip range (SET_VIEWPORT_CLIP_MIN_Z and MAX_Z), which
       need not be the transform's minDepth to maxDepth: NVK, which drives the same registers, turns the clamp off on
       these GPUs by setting that range to -inf..inf. Where the clip control forces 0 to 1 (its bit 0, deko3d's clamp
       off) the range is that. Vulkan's range is one for every viewport: viewport 0's */
    if (g_vk.has_depth_clamp_range) {
        float zn = ds->rkey.depth_zero_one ? 0.0f : ds->vp.z_near[0],
              zf = ds->rkey.depth_zero_one ? 1.0f : ds->vp.z_far[0];
        VkDepthClampRangeEXT range = {zn < zf ? zn : zf, zn < zf ? zf : zn};
        int numbers = zn == zn && zf == zf;
        if (!g_vk.has_depth_range_unrestricted) {
            range.minDepthClamp = range.minDepthClamp < 0 ? 0 : range.minDepthClamp > 1 ? 1 : range.minDepthClamp;
            range.maxDepthClamp = range.maxDepthClamp < 0 ? 0 : range.maxDepthClamp > 1 ? 1 : range.maxDepthClamp;
        }
        if (!numbers) range = (VkDepthClampRangeEXT){0, 0}; /* the viewport's range: the mode below says so */
        if (!ds->rkey.depth_zero_one && viewports > 1 && (ds->vp.z_near[1] != zn || ds->vp.z_far[1] != zf))
            LOG_LIMIT(1,
                      "vulkan: viewport 1 clamps depth to %g..%g and viewport 0 to %g..%g: every viewport takes 0's\n",
                      (double)ds->vp.z_near[1], (double)ds->vp.z_far[1], (double)zn, (double)zf);
        DYNV(clamp_range, range,
             rec_CmdSetDepthClampRangeEXT(
                 cb, numbers ? VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT : VK_DEPTH_CLAMP_MODE_VIEWPORT_RANGE_EXT,
                 &range));
    }
    /* Triangle facing has its own window-origin control, independent of viewport scale and swizzle. */
    {
        VkCullModeFlags cull = ds->dyn.cull;
        VkFrontFace front = vk_render_front_face(&ds->dyn);
        u32 prim_restart = restart && (indexed == 1 || indexed == 3);
        DYN(cull, cull, rec_CmdSetCullMode(cb, cull));
        DYN(front, front, rec_CmdSetFrontFace(cb, front));
        if (!mesh) {
            DYN(topo, topo, rec_CmdSetPrimitiveTopology(cb, topo));
            DYN(restart, prim_restart, rec_CmdSetPrimitiveRestartEnable(cb, prim_restart));
        }
        /* RASTERIZE_ENABLE (0xDF) off discards the primitives, after a capture or the vertex program's stores. One game
           draws points whose vertex program only stores to memory and writes no position: rasterized, they all lit
           the pixel in the middle of the screen. A channel starts with it on (reset_3d, as Eden's register defaults)
           (1 off, 2 on: 0 is a new command buffer's) */
        u32 discard = ds->dyn.discard ? 2 : 1;
        DYN(discard, discard, rec_CmdSetRasterizerDiscardEnable(cb, discard == 2));
    }
    {
        float lw = ds->dyn.line_width;
        lw = g_vk.feat.wideLines && lw > 0 && lw < 64 ? lw : 1.0f;
        DYNV(lw, lw, rec_CmdSetLineWidth(cb, lw));
    }
    /* depth bias: enabled per polygon mode (the key's, as the device draws it), and off for a list of lines or points
       made of polygons, which Vulkan biases only as polygons (draw_edges logs it); its values are ds_depth_bias's */
    {
        u32 bias_on = !edges && ds->dyn.bias_on >> (k->polygon == VK_POLYGON_MODE_POINT  ? 0
                                                    : k->polygon == VK_POLYGON_MODE_LINE ? 1
                                                                                         : 2) &
                                    1;
        DYN(bias_on, bias_on, rec_CmdSetDepthBiasEnable(cb, bias_on));
        if (ds->rkey.depth_pixel_clamp && !ds->rkey.depth_no_clip) {
            g_vk_depth_apart_draws++;
            if (bias_on || (sh.fs0 && (sh.fs0->si.sph[19] & 2u))) g_vk_depth_apart_moved++;
        }
    }
    {
        float bias[3];
        ds_depth_bias(&ds->dyn, g_vk_scale, bias);
        if (!g_vk.feat.depthBiasClamp) bias[1] = 0.0f;
        DYNV(bias, bias, rec_CmdSetDepthBias(cb, bias[0], bias[1], bias[2]));
    }
    DYNV(bc, ds->dyn.blend_const, rec_CmdSetBlendConstants(cb, ds->dyn.blend_const));
    /* depth and stencil */
    {
        u32 dt = !late_depth && zt && ds->dyn.depth_test, dw = !late_depth && zt && ds->dyn.depth_write;
        VkCompareOp dcmp = ds->dyn.depth_cmp;
        DYN(dt, dt, rec_CmdSetDepthTestEnable(cb, dt));
        DYN(dw, dw, rec_CmdSetDepthWriteEnable(cb, dw));
        DYN(dcmp, dcmp, rec_CmdSetDepthCompareOp(cb, dcmp));
        u32 bounds_on = !late_depth && zt && g_vk.feat.depthBounds && (dc->state_flags & DS_DRAW_DEPTH_BOUNDS);
        if (!late_depth && zt && (dc->state_flags & DS_DRAW_DEPTH_BOUNDS) && !g_vk.feat.depthBounds)
            LOG_LIMIT(1, "vulkan: depth bounds requested but unavailable on this device\n");
        float bounds[2] = {0, 1};
        if (bounds_on) {
            bounds[0] = dc->depth_bounds[0];
            bounds[1] = dc->depth_bounds[1];
            for (u32 i = 0; i < 2; i++) bounds[i] = bounds[i] > 1 ? 1 : bounds[i] >= 0 ? bounds[i] : 0;
        }
        DYN(dbounds, bounds_on, rec_CmdSetDepthBoundsTestEnable(cb, bounds_on));
        DYNV(depth_bounds, bounds, rec_CmdSetDepthBounds(cb, bounds[0], bounds[1]));
    }
    int stencil = !late_depth && zt && zt->f.stencil && ds->dyn.stencil_test;
    DYN(st, (u32)stencil, rec_CmdSetStencilTestEnable(cb, stencil));
    {
        /* per face: fail, pass and depth fail operations (vkCmdSetStencilOp's order), the compare op, reference,
           compare and write masks */
        u32 sf[7] = {ds->dyn.face[0].fail, ds->dyn.face[0].pass,  ds->dyn.face[0].zfail, ds->dyn.face[0].cmp,
                     ds->dyn.face[0].ref,  ds->dyn.face[0].cmask, ds->dyn.face[0].wmask};
        u32 sb[7] = {ds->dyn.face[1].fail, ds->dyn.face[1].pass,  ds->dyn.face[1].zfail, ds->dyn.face[1].cmp,
                     ds->dyn.face[1].ref,  ds->dyn.face[1].cmask, ds->dyn.face[1].wmask};
        DYNV(sfront, sf,
             (rec_CmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, sf[0], sf[1], sf[2], sf[3]),
              rec_CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, sf[4]),
              rec_CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, sf[5]),
              rec_CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, sf[6])));
        DYNV(sback, sb,
             (rec_CmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, sb[0], sb[1], sb[2], sb[3]),
              rec_CmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, sb[4]),
              rec_CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, sb[5]),
              rec_CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, sb[6])));
    }
    /* push constants: the depth mode (snx_vp.y) for the vertex stage, 0 too where the device makes the range of -1 to
       1 (VK_DYN_PR_DEPTH_RANGE), and the alpha test's reference for the fragment stage. Then the fragment stage's
       scaled textures (stage_set) and the surface clip's height, which the fragment's y counts up from in a lower-left
       window (shader/emit_io.c's shader_frag_y_origin). The test's function, the window's origin and the point
       sprite's T flip are the pipeline's (vk_pipe_fs_spec) */
    float pc[SH_PUSH_BYTES_TABLE / 4] = {0, ds->dyn.depth_mode || (g_vk.dyn_pr & VK_DYN_PR_DEPTH_RANGE) ? 0.0f : 1.0f,
                                         0, 0};
    /* with the viewport transform off, the viewport's width and height (window_size), which the last stage before
       rasterization takes the position's pixels to clip space by (SNX_SH_WINDOW_XY) */
    u32 window_w = 0, window_h = 0;
    if (!ds->vp.transform_on) window_size(&ds->dyn, &window_w, &window_h);
    u32 window_words = window_w | window_h << 16;
    memcpy(&pc[3], &window_words, sizeof(u32));
    /* the instance ID: InstanceIndex less the base instance pushed (depth_word, the 3D engine's base instance), or
       with draw parameters less BaseInstance plus what the draw's first instance adds to the base instance: the
       3D engine's instanced draws continue from an earlier draw's instances, and an indirect draw's base instance is
       the GPU's */
    u32 pc0 = mesh                               ? first /* a mesh draw's first workgroup (sm86.h) */
              : !g_vk.has_shader_draw_parameters ? ds->dyn.depth_word
              : dc->indirect                     ? 0
                                                 : base_instance - ds->dyn.depth_word;
    memcpy(&pc[0], &pc0, sizeof(u32));
    /* an indirect draw's draw parameters that the vertex program takes from its record (DrawCall.params), and an
       indirect mesh draw's first group, which the task or mesh program takes from SNX_VKB_RS */
    u32 params = mesh && sh.task_compute            ? SH_PUSH_GPU_MESH_FIRST | g_mesh_cap << 8
                 : mesh && dc->indirect            ? SH_PUSH_GPU_MESH_FIRST
                 : dc->indirect                     ? dc->params
                                                    : 0;
    memcpy(&pc[2], &params, sizeof(u32));
    memcpy(&pc[6], &ds->dyn.alpha_ref, 4); /* the test's function is the pipeline's (vk_pipe_fs_spec) */
    u32 scaled_words[2] = {(u32)scaled[0], (u32)(scaled[0] >> 32)};
    memcpy(&pc[8], scaled_words, sizeof scaled_words);
    /* and of those the ones whose target has the draw's size, in the uvec4 at byte 96: the one at byte 80 holds a
       texture draw's rectangle and a varying pass's viewport */
    u32 own_words[2] = {(u32)scaled[1], (u32)(scaled[1] >> 32)};
    memcpy(&pc[24], own_words, sizeof own_words);
    memcpy(&pc[26], &predraw, sizeof predraw); /* its z: the pre-draw capture's draw number */
    memcpy(&pc[11], &ds->dyn.clip_height, sizeof(u32));
    float point_size = (dc->state_flags & DS_DRAW_POINT_SIZE) ? dc->point_size : 1.0f;
    if (!g_vk.feat.largePoints || !(point_size > 0)) point_size = 1.0f;
    if (point_size < g_vk.props.limits.pointSizeRange[0]) point_size = g_vk.props.limits.pointSizeRange[0];
    if (point_size > g_vk.props.limits.pointSizeRange[1]) point_size = g_vk.props.limits.pointSizeRange[1];
    pc[12] = point_size;
    pc[14] = pc[15] = 1.0f;
    for (u32 i = 16; i < 20; i++) pc[i] = 1.0f;
    if (dc->state_flags & DS_DRAW_TESS_LEVELS) {
        memcpy(pc + 16, dc->tess_levels, 4 * sizeof pc[0]);
        memcpy(pc + 14, dc->tess_levels + 4, 2 * sizeof pc[0]);
    }
    if (capture) memcpy(&pc[28], &g_capture_draw.primitive, sizeof g_capture_draw.primitive);
    /* a varying pass's viewport (shader/emit_io.c's shader_frag_ndc): this draw's, as it was set above */
    int varying = fs && fs->si.varying;
    if (varying) {
        pc[20] = viewport[0].x;
        pc[21] = viewport[0].y;
        pc[22] = viewport[0].width;
        pc[23] = viewport[0].height;
    }
    /* the whole block: a Maxwell fragment program declares every member up to the capture's at byte 112
       (spirv_translate.c), and with descriptor heaps the validation layer wants each declared member pushed */
    /* the vertex and fragment programs' constant buffer window offsets (cb_window_place), two banks a word */
    if (g_spirv_translate.cb_windows)
        for (u32 st = 0; st < 2; st++)
            for (u32 w = 0; w < 9; w++) {
                const u32 packed = (u32)g_cb_offsets[st][2 * w] | (u32)g_cb_offsets[st][2 * w + 1] << 16;
                memcpy(&pc[(st ? SH_PUSH_CB_WINDOW_FS : SH_PUSH_CB_WINDOW) / 4 + w], &packed, sizeof packed);
            }
    /* the vertex and fragment programs' texture words (table_words) */
    if (g_spirv_translate.tex_table)
        for (u32 st = 0; st < 2; st++) {
            const u32 at = g_cb_offsets[st][CB_WINDOW_WORDS];
            memcpy(&pc[SH_PUSH_TEX_WORDS / 4 + st], &at, sizeof at);
        }
    rec_CmdPushConstants(cb, g_vk_playout, VK_PUSH_STAGES, 0, vk_push_bytes(), pc);
    if (texture) {
        Surf* source = fres[0].t;
        if (!source || !rt[0]) {
            draw_images_discard(&images);
            if (late_depth) vk_depth_shadow_cancel();
            return;
        }
        float width = (float)rt[0]->w, height = (float)rt[0]->h;
        float rectangle[8] = {2 * texture->x0 / width - 1, 2 * texture->y0 / height - 1,
            2 * (texture->x0 + texture->w) / width - 1, 2 * (texture->y0 + texture->h) / height - 1,
            texture->sx0 / source->w, texture->sy0 / source->h,
            (float)((texture->sx0 + texture->du * texture->w) / source->w),
            (float)((texture->sy0 + texture->dv * texture->h) / source->h)};
        rec_CmdPushConstants(cb, g_vk_playout, VK_PUSH_STAGES, 80, sizeof rectangle, rectangle);
    }

    if (capture && !g_vk.has_descriptor_heaps)
        rec_CmdPushConstants(cb, layout, VK_PUSH_STAGES, 112, sizeof g_capture_draw.primitive, &g_capture_draw.primitive);

    /* counted by the channel's samples-passed counter when it runs: the targets' samples, which the guest's are. Not
       the save draw (predraw_run): it ends the open query, and the guest's draw after it begins another */
    if (g_predraw.phase == 1)
        vk_query_pass_end(cb);
    else
        vk_query_draw(cb, ds->owner);
    VkBuffer counters[4] = {g_xfb.buf, g_xfb.buf, g_xfb.buf, g_xfb.buf};
    u64 counter_base = xfb_counter_offset(ds->owner);
    VkDeviceSize counter_offs[4] = {counter_base, counter_base + 4, counter_base + 8, counter_base + 12};
    /* the buffers from 0 to the last one bound, as begin and end take the counters of: one between that isn't bound
       gets another's memory for 4 bytes, which nothing captures into (its layout has no words) */
    u32 xcount = xbound & 8 ? 4 : xbound & 4 ? 3 : xbound & 2 ? 2 : xbound & 1 ? 1 : 0;
    if (xcount) {
        VkBuffer from[4];
        int first = xbound & 1 ? 0 : xbound & 2 ? 1 : xbound & 4 ? 2 : 3;
        for (u32 i = 0; i < xcount; i++) {
            if (!(xbound >> i & 1)) {
                xbuf[i] = xbuf[first];
                xoff[i] = xoff[first];
                xsize[i] = 4;
            }
            from[i] = resume >> i & 1 ? g_xfb.buf : VK_NULL_HANDLE; /* none: from the bound offset */
        }
        rec_CmdBindTransformFeedbackBuffersEXT(cb, 0, xcount, xbuf, xoff, xsize);
        rec_CmdBeginTransformFeedbackEXT(cb, 0, xcount, from, counter_offs);
    }
    /* the vertex or mesh program, the fragment program, and where the draw's ranges of guest memory were bound */
    rec_draw_tag(vs ? vs->hash : 0, fs ? fs->hash : 0, vk_buf_sources() | vk_tex_tags());
    /* outside the predication: a predicated region writes no timestamp. Only a Tracy client times a draw
       (gpuprof.c), so a build without Tracy makes no call per draw */
#if defined(SNX_TRACY)
    int zoned = vk_gpuprof_op_begin(cb, VK_OP_DRAW, (vs ? vs->hash : 0) * 31 + (fs ? fs->hash : 0), 0);
#else
    const int zoned = 0;
#endif
    pred_begin(cb, o->pred);
    if (mesh && sh.task_compute)
        rec_CmdDrawMeshTasksIndirect(cb, g_mesh_payload, 0); /* the task dispatch's record */
    else if (mesh && dc->indirect)
        rec_CmdDrawMeshTasksIndirect(cb, abuf, aoff);
    else if (mesh)
        rec_CmdDrawMeshTasks(cb, count);
    else if (dc->indirect) {
        if (indexed) rec_CmdBindIndexBuffer(cb, ibuf, ioff, itype);
        draw_indirect(cb, indexed, abuf, aoff, cntbuf, cntoff, dc->draw_count, astride);
    } else if (indexed) {
        rec_CmdBindIndexBuffer(cb, ibuf, ioff, itype);
        rec_CmdDrawIndexed(cb, icount, instances, 0, voffset, base_instance);
    } else
        rec_CmdDraw(cb, count, instances, first, base_instance);
    pred_end(cb, o->pred);
    if (zoned) vk_gpuprof_op_end(cb);
    rec_draw_tag(0, 0, 0); /* the runtime's own draws after it (clears, blits) name no program */
    if (xcount) {
        rec_CmdEndTransformFeedbackEXT(cb, 0, xcount, counters, counter_offs);
        o->xfb_started = (u8)((o->xfb_started | xbound) & 15);
        o->xfb_reload = (u8)(o->xfb_reload & ~xbound);
        vk_render_end(); /* the next capture's begin waits for this end outside a pass, and readers for its writes */
    }
    if (late_depth) vk_depth_shadow_finish(zt);
    if (xfb_stores.range.declared) xfb_bind_done(o, &xfb_stores);
    vk_buf_writable_commit();
    draw_images_commit(&images);
    feedback_done(fb, writes);
    if (fb && g_predraw.phase != 1) g_vk_fb_draws++; /* the guest's draws: a save draw is the renderer's own */
    if (predraw) vk_predraw_drawn();
    for (u32 i = 0; i < n; i++)
        if ((writes >> i & 1) && !capture && !g_rp.rt[i]->predraw) target_written(g_rp.rt[i]);
    if (programmable_blend && ds->blend.mask[0]) target_written(rt[0]);
    if (zt && !zt_ro &&
        (ds->dyn.depth_write ||
         (ds->dyn.stencil_test && ((ds->dyn.face[0].wmask & 0xFF) || (ds->dyn.face[1].wmask & 0xFF)))) &&
        zt_written(zt, o->pred, zt_undefined))
        target_written(zt);
    g_vk_drawn++;
    if (mesh) g_vk_mesh_drawn++;
    if (sh.task_compute) g_vk_mesh_task_compute++;
}

/* Late tests stay native. Resolve each primitive before the next so noncommutative blend equations and
   coverage masks preserve overlap order. The normal native/interlock paths do not pay this fallback's cost. */
static int capture_draw(const DsState* ds, const DrawCall* dc, DrawOwner* owner, Surf* target, u64 blend,
                         int coverage, const VkCoverage* mixed, VkCapture* prepared) {
    if (!mixed && dc->raster_samples > (u32)vk_surf_samples(target)) return 0;
    VkCapture* images = mixed ? prepared : vk_capture_get(target);
    if (!images) return 0;
    DrawCall slice = *dc;
    slice.multisample_flags &= ~DS_MS_COVERAGE_TO_COLOR;
    if (mixed) {
        /* The scratch attachments store every raster sample. Apply the original N/C table only in the final
           color pass, not as a second ratio-one modulation of the captured source. */
        slice.coverage_mode = 0;
        slice.coverage_table_enable = 0;
    }
    memset(slice.advanced_blend, 0, sizeof slice.advanced_blend);
    u32 vertices = dc->topology == 0 ? 1 : dc->topology == 1 ? 2 : 3;
    slice.count = vertices;
    slice.instances = 1;
    u64 before = g_vk_drawn;
    g_capture_draw.images = images;
    g_capture_draw.snapshot = NULL;
    int complete = 1;
    /* Vulkan primitive order places every primitive of the earlier instance before the next instance. Keep
       firstInstance on each slice so attributes and guest instance IDs retain their original indices. */
    for (u32 instance = 0; instance < dc->instances && complete; instance++) {
        slice.base_instance = dc->base_instance + instance;
        for (u32 primitive = 0; primitive < dc->count / vertices; primitive++) {
            u64 first = (u64)dc->first + (u64)primitive * vertices;
            if (first > UINT32_MAX || !vk_capture_clear(images)) { complete = 0; break; }
            slice.first = (u32)first;
            g_capture_draw.primitive = primitive;
            u64 drawn = g_vk_drawn;
            render_draw(ds, &slice, owner, NULL);
            if (g_vk_drawn != drawn + 1) { complete = 0; break; }
            u32 flags = images->flags | (ds->blend.mask[0] & 15);
            if (ds->tgt.srgb_write && target->f.vk != vk_srgb_as(target->f.vk, 0)) flags |= SH_BLEND_SRGB;
            int resolved = mixed ? vk_capture_mixed_resolve(images, mixed, ds, g_draw.fs->si.sph[18])
                                  : vk_capture_resolve(images, blend, flags, coverage);
            if (!resolved) { complete = 0; break; }
        }
    }
    g_capture_draw.images = NULL;
    g_capture_draw.snapshot = NULL;
    draw_forget();
    if (complete) {
        g_vk_drawn = before + 1;
        g_vk_capture_draws++;
        g_vk_capture_primitives += (u64)dc->instances * (dc->count / vertices);
    }
    return complete;
}

/* DrawTexture uses the raster path so fragment tests, masks, blends and the sampler apply. */
void vk_render_draw_texture(const DsState* ds, const DrawTexture* texture) {
    if (!(texture->w > 0 && texture->h > 0)) return;
    TexRes source;
    tex_resolve(ds, texture->handle, &source);
    Surf *targets[8], *depth;
    u32 count;
    read_targets(ds, targets, &depth, &count);
    if (!source.t || !count || !targets[0]) return;
    if (source.t->msaa || source.t->depth3d > 1 || source.t->layers > 1) {
        LOG_LIMIT(4, "vulkan: DrawTexture needs a single-sample 2D texture\n");
        return;
    }
    /* Built-in programs have no guest attributes, constant buffers or storage bindings. Keep those large blocks
       zeroed instead of copying the caller's inline banks and immediately unused program/vertex state each draw.
       Renderer calls hold vk_lock; this scratch object is consumed before the call returns. */
    static DsState state;
    state.owner = ds->owner;
    state.blend = ds->blend;
    state.rkey = ds->rkey;
    state.tgt = ds->tgt;
    state.dyn = ds->dyn;
    state.vp = ds->vp;
    state.bind.tex_pool = ds->bind.tex_pool;
    state.bind.smp_pool = ds->bind.smp_pool;
    state.bind.tex_max = ds->bind.tex_max;
    state.bind.smp_max = ds->bind.smp_max;
    state.bind.smp_mode = ds->bind.smp_mode;
    state.bind.tex_bank = ds->bind.tex_bank;
    state.tgt.count = 1;
    state.tgt.depth.gva = 0;
    state.tgt.depth.format = 0;
    state.rkey.polygon = 0;
    state.rkey.conservative = 0;
    state.rkey.depth_clamp = 0;
    state.dyn.cull = DS_CULL_NONE;
    state.dyn.discard = 0;
    state.dyn.bias_on = 0;
    state.dyn.depth_test = state.dyn.depth_write = state.dyn.stencil_test = 0;
    state.dyn.depth_mode = 1;
    state.dyn.point_nudge = 0;
    state.vp.transform_on = 1;
    state.vp.scale[0][0] = state.vp.translate[0][0] = (float)targets[0]->w * 0.5f;
    state.vp.scale[0][1] = state.vp.translate[0][1] = (float)targets[0]->h * 0.5f;
    state.vp.scale[0][2] = state.vp.translate[0][2] = 0;
    state.vp.swizzle[0] = DS_SWIZZLE_IDENTITY;
    state.vp.z_near[0] = 0;
    state.vp.z_far[0] = 1;
    u64 generation = ds_generation_next();
    for (u32 i = 0; i < DS_GENS; i++) state.gen[i] = generation;
    DrawCall call = texture->state;
    call.topology = 5;
    call.first = 0;
    call.count = 4;
    call.instances = 1;
    call.indexed = 0;
    call.indirect = 0;
    call.xfb_control = 0;
    DrawOwner owner = texture->owner;
    int heaps = g_vk.has_descriptor_heaps;
    render_draw(&state, &call, &owner, texture);
    g_vk.has_descriptor_heaps = heaps;
    draw_forget();
}
