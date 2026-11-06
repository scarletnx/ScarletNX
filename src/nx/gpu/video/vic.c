/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/host1x/vic.cpp, vic.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project */
/* VIC: composes the enabled slots' NV12 (or NV21) surfaces, each read from guest memory where NVDEC wrote it, onto
   the output surface, over the background colour the target rectangle is filled with. host1x.c's channel worker
   calls vic_execute when the stream writes Execute; this file resolves the ConfigStruct (vic_regs.h) into a VicJob
   and hands it to the renderer, which composes it as a compute shader (vic.comp, through the renderer's
   vic_compose). tests/support/vic_ref.c is the same composition on the CPU, for the tests.

   The pixel pipeline is Eden's (Vic::Blend): a sample becomes a 10 bit pixel, Y, U and V in R, G and B (the pair's
   chroma for both of its pixels) and the slot's planar alpha in A. With the slot's colour matrix enabled each
   channel is ((R*c0 + G*c1 + B*c2) >> shift + c3) >> 8, clamped to the slot's soft clamp (the alpha stays out of
   both); without it the samples are copied as they are, VIC doing no YUV to RGB of its own. A later slot overwrites
   an earlier one, with no blending, and the output is 8 bit.

   Not modelled: scaling (a slot whose source and destination sizes differ is cropped), deinterlacing, the slots'
   other surfaces (Previous, Next...), blending, luma keys, the gamut and output matrices and the clear rectangles */
#include "nx/gpu/video/host1x_int.h"
#include "nx/gpu/video/vic.h"
#include "nx/gpu/renderer.h"

/* A GOB, the unit of the block linear layout: 64 bytes wide and 8 rows high, stacked 1 << block_height high in a
   block (docs/BLOCKLINEAR.md) */
#define GOB_WIDTH       64u
#define GOB_HEIGHT_LOG2 3u
#define GOB_BYTES_LOG2  9u /* 64 x 8 */
/* a pitch linear plane's rows, when the config's padded width is short of the surface: 256-byte aligned, as the
   surfaces NVIDIA's libraries allocate */
#define PITCH_ALIGN 256u

static u32 min_u32(u32 a, u32 b) { return a < b ? a : b; }
static int is_yuv(u32 fmt) { return fmt == VIC_PIXEL_Y8__V8U8_N420 || fmt == VIC_PIXEL_Y8__U8V8_N420; }

/* a plane's row in bytes: the padded width the config names, as it is (rounding it turned a width that isn't a
   multiple of 16 green), unless that is short of the surface's row */
static u32 row_bytes(u32 padded, u32 need, u32 block_linear) {
    if (padded >= need) return padded;
    return block_linear ? need : (need + PITCH_ALIGN - 1) & ~(PITCH_ALIGN - 1);
}
/* the plane at iova: rows pitch bytes apart, or a block linear surface pitch bytes wide. It is mapped for its whole
   extent (whole GOBs wide, whole blocks high), or for its rows alone when the buffer ends inside the last block
   row. 0 when unmapped */
static int plane_map(VicPlane* pl, u64 iova, u32 pitch, u32 rows, u32 block_linear, u32 block_height) {
    u64 gob_row = (u64)(pitch + GOB_WIDTH - 1) / GOB_WIDTH * GOB_WIDTH;
    u64 block_rows = 1ull << (GOB_HEIGHT_LOG2 + block_height), ext;
    memset(pl, 0, sizeof *pl);
    if (!iova || !pitch || !rows) return 0;
    ext = block_linear ? gob_row * ((rows + block_rows - 1) / block_rows * block_rows) : (u64)pitch * rows;
    pl->p = iova_ptr(iova, ext);
    if (!pl->p && block_linear) pl->p = iova_ptr(iova, ext = gob_row * rows);
    if (!pl->p) return 0;
    pl->lim = ext;
    pl->pitch = pitch;
    pl->h = rows;
    pl->blk = block_linear != 0;
    pl->bh = block_height;
    return 1;
}
/* OutputConfig and OutputSurfaceConfig: the output surface, its planes and the target rectangle. 0 when the job
   can't be composed */
static int job_output(VicJob* j, const u8* cfg, const u32* r) {
    const u8* osc = cfg + VIC_CONFIG_OUTPUT_SURFACE_CONFIG;
    u64 sc = rd64(osc, VIC_SURFACE_CONFIG_FORMAT), sizes = rd64(osc, VIC_SURFACE_CONFIG_PLANE_SIZES);
    u64 out = rd64(cfg, VIC_CONFIG_OUTPUT_CONFIG);
    u32 lr = rd32(cfg, VIC_CONFIG_OUTPUT_TARGET_RECT_LR), tb = rd32(cfg, VIC_CONFIG_OUTPUT_TARGET_RECT_TB);
    u32 block_linear = vic_surface_block_kind(sc) != VIC_BLK_KIND_PITCH, block_height = vic_surface_block_height(sc);
    u32 bpp, yuv, left, right, top, bottom;
    j->fmt = vic_surface_pixel_format(sc);
    j->sw = vic_surface_width(sc);
    j->sh = vic_surface_height(sc);
    switch (j->fmt) {
        case VIC_PIXEL_A8B8G8R8:
        case VIC_PIXEL_A8R8G8B8:
        case VIC_PIXEL_X8B8G8R8: bpp = 4, yuv = 0; break;
        case VIC_PIXEL_Y8__U8V8_N420:
        case VIC_PIXEL_Y8__V8U8_N420: bpp = 1, yuv = 1; break;
        default: WARN_LIMIT(4, "video: VIC output format %#x isn't composed\n", j->fmt); return 0;
    }
    if (!plane_map(&j->oy, vic_offset_address(r[VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET]),
                   row_bytes(vic_luma_width(sizes) * bpp, j->sw * bpp, block_linear), j->sh, block_linear,
                   block_height)) {
        WARN_LIMIT(8, "video: VIC output surface isn't mapped, job skipped\n");
        return 0;
    }
    /* 4:2:0 chroma: a U,V pair for every two pixels of every two rows */
    if (yuv && !plane_map(&j->oc, vic_offset_address(r[VIC_SET_OUTPUT_SURFACE_CHROMA_OFFSET]),
                          row_bytes(vic_chroma_width(sizes) * 2, (j->sw + 1) / 2 * 2, block_linear), (j->sh + 1) / 2,
                          block_linear, block_height))
        WARN_LIMIT(4, "video: VIC output chroma isn't mapped, only the luma is written\n");
    left = vic_rect_lo(lr);
    right = vic_rect_hi(lr);
    top = vic_rect_lo(tb);
    bottom = vic_rect_hi(tb);
    if (yuv) { /* a chroma pair starts on an even pixel and row; RGBA keeps an odd start */
        left &= ~1u;
        top &= ~1u;
    }
    if (right >= left && bottom >= top && left < j->sw && top < j->sh) {
        j->tx0 = left;
        j->ty0 = top;
        j->tw = min_u32(right, j->sw - 1) - left + 1;
        j->th = min_u32(bottom, j->sh - 1) - top + 1;
    }
    j->bg[0] = vic_background_r(out);
    j->bg[1] = vic_background_g(out);
    j->bg[2] = vic_background_b(out);
    j->bg[3] = vic_background_a(out);
    return 1;
}
/* SlotStruct s resolved: its rectangles cropped to each other (VIC scales, this doesn't), clipped to the target and
   the input, its Current surface's planes mapped (planes: that surface's luma and chroma_u registers). Off when
   nothing of it composes */
static void slot_resolve(VicJob* j, const u8* sl, const u32* planes, u32 s) {
    VicSlot* o = &j->slots[s];
    const u8* ssc = sl + VIC_SLOT_SURFACE_CONFIG;
    u64 sc = rd64(ssc, VIC_SURFACE_CONFIG_FORMAT), sizes = rd64(ssc, VIC_SURFACE_CONFIG_PLANE_SIZES);
    u64 src_lr = rd64(sl, VIC_SLOT_SOURCE_RECT_LR), src_tb = rd64(sl, VIC_SLOT_SOURCE_RECT_TB);
    u64 dst = rd64(sl, VIC_SLOT_DEST_RECT), clamp_alpha = rd64(sl, VIC_SLOT_CLAMP_ALPHA);
    u32 fmt = vic_surface_pixel_format(sc), block_linear = vic_surface_block_kind(sc) != VIC_BLK_KIND_PITCH;
    u32 block_height = vic_surface_block_height(sc), iw = vic_surface_width(sc), ih = vic_surface_height(sc);
    u32 sx0 = vic_source_rect_lo(src_lr), sx1 = vic_source_rect_hi(src_lr);
    u32 sy0 = vic_source_rect_lo(src_tb), sy1 = vic_source_rect_hi(src_tb);
    u32 dx0 = vic_dest_rect_left(dst), dx1 = vic_dest_rect_right(dst);
    u32 dy0 = vic_dest_rect_top(dst), dy1 = vic_dest_rect_bottom(dst);
    u32 k, w, h, cut;
    u64 m[VIC_MATRIX_COLUMNS];
    memset(o, 0, sizeof *o);
    if (!vic_slot_enable(rd64(sl, VIC_SLOT_CONFIG))) return;
    if (!is_yuv(fmt)) {
        WARN_LIMIT(4, "video: VIC slot format %#x isn't composed\n", fmt);
        return;
    }
    if (is_yuv(j->fmt)) { /* YUV output: chroma pairs start on even pixels and rows */
        sx0 &= ~1u;
        sy0 &= ~1u;
        dx0 &= ~1u;
        dy0 &= ~1u;
    }
    if (sx1 < sx0 || sy1 < sy0 || dx1 < dx0 || dy1 < dy0 || !j->tw || !j->th) return;
    w = sx1 - sx0 + 1;
    h = sy1 - sy0 + 1;
    if (w != dx1 - dx0 + 1 || h != dy1 - dy0 + 1) {
        WARN_LIMIT(4, "video: VIC slot %u scales %ux%u to %ux%u: cropped instead\n", s, w, h, dx1 - dx0 + 1,
                   dy1 - dy0 + 1);
        w = min_u32(w, dx1 - dx0 + 1);
        h = min_u32(h, dy1 - dy0 + 1);
    }
    /* the target rectangle (inside the surface) cuts the source by as much as the destination */
    if (dx0 < j->tx0) {
        if ((cut = j->tx0 - dx0) >= w) return;
        sx0 += cut, dx0 += cut, w -= cut;
    }
    if (dy0 < j->ty0) {
        if ((cut = j->ty0 - dy0) >= h) return;
        sy0 += cut, dy0 += cut, h -= cut;
    }
    if (dx0 >= j->tx0 + j->tw || dy0 >= j->ty0 + j->th || sx0 >= iw || sy0 >= ih) return;
    w = min_u32(min_u32(w, j->tx0 + j->tw - dx0), iw - sx0);
    h = min_u32(min_u32(h, j->ty0 + j->th - dy0), ih - sy0);
    if (!plane_map(&o->iy, vic_offset_address(planes[0]), row_bytes(vic_luma_width(sizes), iw, block_linear), ih,
                   block_linear, block_height) ||
        !plane_map(&o->ic, vic_offset_address(planes[1]),
                   row_bytes(vic_chroma_width(sizes) * 2, (iw + 1) / 2 * 2, block_linear), (ih + 1) / 2,
                   block_linear, block_height)) {
        WARN_LIMIT(8, "video: VIC slot %u's surface isn't mapped, slot skipped\n", s);
        memset(o, 0, sizeof *o);
        return;
    }
    for (k = 0; k < VIC_MATRIX_COLUMNS; k++) m[k] = rd64(sl, VIC_SLOT_COLOR_MATRIX + 8 * k);
    for (k = 0; k < VIC_MATRIX_ROWS * VIC_MATRIX_COLUMNS; k++)
        o->mat[k / VIC_MATRIX_COLUMNS][k % VIC_MATRIX_COLUMNS] =
            vic_matrix_coeff(m[k % VIC_MATRIX_COLUMNS], k / VIC_MATRIX_COLUMNS);
    o->mat_shift = (int)vic_matrix_r_shift(m[0]);
    o->mat_enable = (int)vic_matrix_enable(m[1]);
    o->alpha = vic_planar_alpha(clamp_alpha);
    o->clamp_lo = vic_soft_clamp_low(clamp_alpha);
    o->clamp_hi = vic_soft_clamp_high(clamp_alpha);
    o->vu = fmt == VIC_PIXEL_Y8__U8V8_N420;
    o->sx0 = sx0;
    o->sy0 = sy0;
    o->dx0 = dx0;
    o->dy0 = dy0;
    o->w = w;
    o->h = h;
    o->on = 1;
}

/* ------------------------------------------------ the shader's parameters (vic_params.glsl says what each is) */
/* a plane's block linear layout as shifts and masks: a block is 1 << bh GOBs high, so a block column of GOBs takes
   GOB_BYTES_LOG2 + bh bits and a block row GOB_HEIGHT_LOG2 + bh bits of the row */
static VicGpuPlane gpu_plane(const VicPlane* p, u32 base) {
    VicGpuPlane g = {0};
    g.base = base;
    if (!p->p) return g; /* lim 0: nothing is read or written */
    g.lim = (u32)p->lim;
    g.blk = p->blk;
    g.pitch = p->pitch;
    g.bsh = GOB_BYTES_LOG2 + p->bh;
    g.ysh = GOB_HEIGHT_LOG2 + p->bh;
    g.gm = ((1u << p->bh) - 1) << GOB_BYTES_LOG2;
    g.bstride = ((p->pitch + GOB_WIDTH - 1) / GOB_WIDTH) << g.bsh;
    return g;
}
void vic_gpu_params(const VicJob* j, VicGpuParams* p, const u32 in_base[16], u32 oc_base) {
    u32 yuv = is_yuv(j->fmt), ovu = j->fmt == VIC_PIXEL_Y8__U8V8_N420, rb = j->fmt == VIC_PIXEL_A8R8G8B8 ? 16 : 0;
    u32 bg[4], s, n = 0;
    VicGpuJob* g = &p->job;
    memset(p, 0, sizeof *p);
    for (s = 0; s < 4; s++) bg[s] = j->bg[s] >> 2; /* 10 bit to the output's 8 */
    g->nv12 = yuv;
    g->rpos = rb;
    g->gpos = yuv ? 8 * ovu : 8;
    g->bpos = yuv ? 8 - 8 * ovu : 16 - rb;
    g->tx0 = j->tx0;
    g->tx1 = j->tx0 + j->tw;
    g->ty0 = j->ty0;
    g->th = j->th;
    g->gx0 = j->tx0 & ~3u; /* an invocation's 4x2 pixels */
    g->gy0 = j->ty0 & ~1u;
    g->bgw = (bg[0] << g->rpos) | (bg[1] << 8) | (bg[2] << g->bpos) | (bg[3] << 24);
    g->bgy = bg[0] * 0x01010101u;                                     /* the byte in each of four */
    g->bgc = ((bg[1] << g->gpos) | (bg[2] << g->bpos)) * 0x00010001u; /* the pair in each half */
    g->ol = gpu_plane(&j->oy, 0);
    g->oc = gpu_plane(&j->oc, oc_base);
    for (s = 0; s < VIC_SLOTS; s++) {
        const VicSlot* sl = &j->slots[s];
        VicGpuSlot* o = &p->slots[n];
        if (!sl->on) continue;
        /* the shader writes the target rectangle alone, which holds every slot slot_resolve leaves on */
        if (sl->dx0 < j->tx0 || sl->dx0 + sl->w > j->tx0 + j->tw || sl->dy0 < j->ty0 ||
            sl->dy0 + sl->h > j->ty0 + j->th)
            fatal("VIC slot %u outside the target rectangle, which slot_resolve clips it to", s);
        n++;
        o->dx0 = sl->dx0;
        o->dx1 = sl->dx0 + sl->w;
        o->dy0 = sl->dy0;
        o->h = sl->h;
        o->ox = sl->sx0 - sl->dx0;
        o->oy = sl->sy0 - sl->dy0;
        o->mat = sl->mat_enable != 0;
        o->msh = (u32)sl->mat_shift;
        o->ry = sl->mat[0][0] * 4, o->ru = sl->mat[0][1] * 4, o->rv = sl->mat[0][2] * 4, o->r3 = sl->mat[0][3];
        o->gy = sl->mat[1][0] * 4, o->gu = sl->mat[1][1] * 4, o->gv = sl->mat[1][2] * 4, o->g3 = sl->mat[1][3];
        o->by = sl->mat[2][0] * 4, o->bu = sl->mat[2][1] * 4, o->bv = sl->mat[2][2] * 4, o->b3 = sl->mat[2][3];
        o->lo = (int)sl->clamp_lo;
        o->hi = (int)sl->clamp_hi;
        o->aw = (sl->alpha >> 2) << 24;
        o->ush = 8 * sl->vu;
        o->vsh = 8 - 8 * sl->vu;
        o->cswap = sl->vu ^ ovu;
        o->iy = gpu_plane(&sl->iy, in_base[2 * s]);
        o->ic = gpu_plane(&sl->ic, in_base[2 * s + 1]);
    }
    g->nslots = n;
}

/* the shader's build for word aligned rows needs every plane's rows to keep a word aligned column word aligned: block
   linear rows do (bl_offset keeps the column's alignment), pitch linear ones when the pitch is a multiple of 4 */
static int plane_aligned(const VicPlane* p) { return !p->p || p->blk || !(p->pitch & 3); }
int vic_gpu_aligned(const VicJob* j) {
    u32 s;
    for (s = 0; s < VIC_SLOTS; s++)
        if (j->slots[s].on && !(plane_aligned(&j->slots[s].iy) && plane_aligned(&j->slots[s].ic))) return 0;
    return plane_aligned(&j->oy) && plane_aligned(&j->oc);
}

/* Execute: the ConfigStruct at config_struct_offset, the output surface and each slot's Current surface, composed */
void vic_execute(const u32* r) {
    VicJob j = {0};
    const u8* cfg = iova_ptr(vic_offset_address(r[VIC_SET_CONFIG_STRUCT_OFFSET]), VIC_CONFIG_SIZE);
    u32 s;
    int any = 0;
    if (!cfg) {
        WARN_LIMIT(8, "video: VIC config isn't mapped, job skipped\n");
        return;
    }
    if (!job_output(&j, cfg, r)) return;
    for (s = 0; s < VIC_SLOTS; s++) {
        slot_resolve(&j, cfg + VIC_CONFIG_SLOT_STRUCTS + VIC_SLOT_STRUCT_SIZE * s, r + vic_slot_surface(s), s);
        any |= j.slots[s].on;
    }
    if (!any && !j.tw) return;
    /* on the GPU, whose result stays there (docs/GPU_OWNERSHIP.md): a job before the renderer is up has nowhere to go */
    if (!g_renderer_up || !g_renderer || !g_renderer->vic_compose) {
        WARN_LIMIT(4, "video: a VIC job before the renderer started, skipped\n");
        return;
    }
    if (!g_renderer->vic_compose(&j))
        WARN_LIMIT(4, "video: the GPU could not compose a VIC job of %ux%u pixels: rows of it are not composed\n", j.tw,
                   j.th);
}
