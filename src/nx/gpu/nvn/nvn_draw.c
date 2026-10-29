/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The driver constant buffers' layout from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/nvn_mtl.m */
/* nvn_direct's draws. A queue's replayed records set NVN's state here, as the game recorded it: state objects,
   viewports, programs, vertex layout and buffers, uniform and storage buffers, texture handles. A draw makes the
   renderer's draw state of it (gpu/draw_state.h), each group whose bytes changed with a new generation, as
   maxwell/draw_state.c makes it of a channel's 3D engine registers, and hands it the draw.

   Where the 3D engine reads what NVN's driver put in its own constant buffers, the queue gives them as inline blocks
   (renderer.h): bank 0 holds each stage's storage buffers (address and size, 16 bytes apart: the vertex stage's at
   0x110, the fragment stage's at 0x510), the largest point size at 0x610 and the draw's base vertex and base instance
   at 0x640 and 0x644; bank 2 the texture handles at 0x20 + 8 r, the images' at 0x120 + 8 r, separate textures' at
   0x168 + 8 r and separate samplers' at 0x568 + 8 r. Those layouts are nxnative's measurements of the games' shaders
   (dm/gfx/nvn/nvn_mtl.m, MIT, Copyright (c) 2026 seedds). Bank 1 is the program's own constants, after its code in
   the code area; the application's uniform buffer r is bank 3 + r */
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/draw_state.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/gpu/nvn/nvn_verify.h"
#include "nx/runtime/options.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* the inline banks' layout (above) */
enum {
    C0_POINT_SIZE = 0x610,
    C0_BASE_VERTEX = 0x640,
    C0_BASE_INSTANCE = 0x644,
    C2_TEX = 0x20,
    C2_IMG = 0x120,
    C2_SEPTEX = 0x168,
    C2_SAMP = 0x568
};
#define SEPTEX_SLOT0 128 /* nvn_cmd.c's NVN_SEPTEX_SLOT0: separate textures' slots in OP_BIND_TEX */

/* the groups a record changed, made again at the next draw */
enum {
    D_PROGRAMS = 1u << DS_PROGRAMS,
    D_VFORMAT = 1u << DS_VFORMAT,
    D_BLEND = 1u << DS_BLEND,
    D_RASTER = 1u << DS_RASTER_KEY,
    D_DYNAMIC = 1u << DS_DYNAMIC,
    D_VIEWPORTS = 1u << DS_VIEWPORTS,
    D_VBUF = 1u << DS_VBUF,
    D_BIND = 1u << DS_BIND,
    D_INLINE = 1u << DS_INLINE
};

/* the NVN state a queue's records set: the state objects' words as the game packed them (nvn_obj.h) */
struct NvnDraw {
    DsState ds; /* what the renderer reads */
    DrawOwner own;
    DsBindings bind; /* the bindings and inline blocks as the records left them */
    DsInline inl;
    u64 blend[DS_TARGETS_MAX], dss;
    u32 color, chmask, poly;
    NvnMultisample ms;
    u32 sample_off; /* the samples SetSampleMask leaves out (NVN's default mask has every one) */
    u32 svalue[2], smask[2], sref[2]; /* the stencil's compare mask, write mask and reference, front and back */
    float blend_color[4], bias[3];    /* the polygon offset's factor, units and clamp */
    u8 discard, depth_clamp, restart, render_off;
    u8 provoking_last, conservative, dilate; /* SetProvokingVertexMode's last vertex, conservative rasterization */
    u8 sample_shading;                       /* the bound fragment program's SetSampleShading mode */
    float line_width;
    u8 samples; /* the samples-passed counter was reset: the renderer counts it */
    u32 restart_index;
    float vp[16][4], depth[2]; /* x, y, w, h; depth range 0 */
    i32 scissor[4];
    u8 swizzle[16][4]; /* XOR the identity, as recorded */
    u32 attr[16], nattr;
    NvnVertexStream stream[16];
    u32 nstream;
    NVNbufferRange vbuf[16];
    u64 code[DS_PROGRAM_SLOTS];
    u32 prog_on;
    ShXfb xfb;            /* the bound program's capture layout (zero: none) */
    NVNbufferRange tf[4]; /* the transform feedback buffers */
    u8 tf_on;             /* between Begin (or Resume) and End (or Pause) */
    u32 dirty;
    DsState scratch;   /* a texture's clear, as a target of its own (OP_CLEAR_TEX) */
    DispatchState dx;  /* the compute stage's program and bindings, a dispatch's state */
};

/* the device's, for every queue (set on the game's threads, read on the GPU thread) */
static struct {
    u32 origin, depth_mode;
} g_nvn_device = {NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT, NVN_DEPTH_MODE_NEAR_IS_MINUS_W};
void nvn_draw_device(u32 origin, u32 depth_mode) {
    __atomic_store_n(&g_nvn_device.origin, origin, __ATOMIC_RELAXED);
    __atomic_store_n(&g_nvn_device.depth_mode, depth_mode, __ATOMIC_RELAXED);
}

/* DsState.gen: a new generation of group *gen, from the sequence every front end shares */
static void new_gen(u64* gen) { *gen = ds_generation_next(); }
/* the group of n bytes at kept as made: a new generation when it changed */
static void keep(void* kept, const void* made, size_t n, u64* gen) {
    if (!memcmp(kept, made, n)) return;
    memcpy(kept, made, n);
    new_gen(gen);
}

NvnDraw* nvn_draw_new(u32 owner) {
    NvnDraw* d = (NvnDraw*)calloc(1, sizeof *d);
    int i, s;
    if (!d) fatal("nvn: out of memory for a queue's draw state");
    d->ds.owner = owner;
    /* NVN's state before anything is bound: the state objects' defaults (nvn_object.c's SetDefaults) */
    for (i = 0; i < DS_TARGETS_MAX; i++) {
        u64 w = 0;
        w = nvn_set_field(w, NVN_BLEND_TARGET, (u64)i);
        w = nvn_set_field(w, NVN_BLEND_SRC_RGB, NVN_BLEND_FUNC_ONE);
        w = nvn_set_field(w, NVN_BLEND_DST_RGB, NVN_BLEND_FUNC_ZERO);
        w = nvn_set_field(w, NVN_BLEND_SRC_A, NVN_BLEND_FUNC_ONE);
        w = nvn_set_field(w, NVN_BLEND_DST_A, NVN_BLEND_FUNC_ZERO);
        w = nvn_set_field(w, NVN_BLEND_EQ_RGB, NVN_BLEND_EQUATION_ADD);
        d->blend[i] = nvn_set_field(w, NVN_BLEND_EQ_A, NVN_BLEND_EQUATION_ADD);
    }
    d->color = (u32)nvn_set_field(nvn_set_field(0, NVN_COLOR_LOGIC_OP, NVN_LOGIC_OP_COPY), NVN_COLOR_ALPHA_FUNC,
                                  NVN_ALPHA_FUNC_ALWAYS);
    d->chmask = 0xffffffffu;
    d->poly = (u32)nvn_set_field(
        nvn_set_field(nvn_set_field(0, NVN_POLY_CULL, NVN_FACE_NONE), NVN_POLY_FRONT, NVN_FRONT_FACE_CCW),
        NVN_POLY_MODE, NVN_POLYGON_MODE_FILL);
    d->dss = nvn_set_field(nvn_set_field(nvn_set_field(0, NVN_DS_WRITE, 1), NVN_DS_FUNC, NVN_DEPTH_FUNC_LESS),
                           NVN_DS_SFUNC_F, NVN_STENCIL_FUNC_ALWAYS);
    d->dss = nvn_set_field(d->dss, NVN_DS_SFUNC_B, NVN_STENCIL_FUNC_ALWAYS);
    d->dss =
        nvn_set_field(nvn_set_field(nvn_set_field(d->dss, NVN_DS_FAIL_F, 1), NVN_DS_ZFAIL_F, 1), NVN_DS_ZPASS_F, 1);
    d->dss =
        nvn_set_field(nvn_set_field(nvn_set_field(d->dss, NVN_DS_FAIL_B, 1), NVN_DS_ZFAIL_B, 1), NVN_DS_ZPASS_B, 1);
    d->svalue[0] = d->svalue[1] = d->smask[0] = d->smask[1] = 0xff;
    d->provoking_last = 1; /* NVN's default, as GL's */
    d->line_width = 1.0f;
    d->depth[1] = 1.0f;
    d->scissor[2] = d->scissor[3] = 0x10000;
    d->bind.tex_bank = 2;
    for (s = 0; s < DS_GFX_STAGES; s++) {
        const float point = 2047.0f; /* Maxwell's largest point size, which GLSLC clamps a written size to */
        d->bind.cb_inline[s][0] = DS_CB_BANK0;
        d->bind.cb_inline[s][2] = DS_CB_BANK2;
        memcpy(d->inl.bank0[s] + C0_POINT_SIZE, &point, 4);
    }
    /* compute: bank 0 and 2 inline (slots 0 and 2), bank 2 starting with the words 0 to 7 as the driver's does */
    d->dx.cb_on = 1u << 0 | 1u << 2;
    d->dx.cb_inline[0] = DS_CB_BANK0;
    d->dx.cb_inline[2] = DS_CB_BANK2;
    d->dx.tex_bank = 2;
    for (s = 0; s < 8; s++) snx_st_le32(d->dx.bank2 + 4 * (u32)s, (u32)s);
    d->dirty = ~0u;
    return d;
}

/* ---- conversions of NVN's enumerants */
/* an NVN shader stage's graphics stage (DS_GFX_STAGES' order: VS, TCS, TES, GS, FS), -1 for compute */
static int gfx_stage(i32 nvn) {
    switch (nvn) {
        case NVN_SHADER_STAGE_VERTEX: return 0;
        case NVN_SHADER_STAGE_TESS_CONTROL: return 1;
        case NVN_SHADER_STAGE_TESS_EVALUATION: return 2;
        case NVN_SHADER_STAGE_GEOMETRY: return 3;
        case NVN_SHADER_STAGE_FRAGMENT: return 4;
    }
    return -1;
}
/* NVN's blend functions, numbered as D3D's from 1 with the constant ones at 0x61 */
static u8 blend_factor(u32 f) {
    switch (f) {
        case NVN_BLEND_FUNC_ZERO: return DS_FACTOR_ZERO;
        case NVN_BLEND_FUNC_ONE: return DS_FACTOR_ONE;
        case NVN_BLEND_FUNC_SRC_COLOR: return DS_FACTOR_SRC_COLOR;
        case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return DS_FACTOR_ONE_MINUS_SRC_COLOR;
        case NVN_BLEND_FUNC_SRC_ALPHA: return DS_FACTOR_SRC_ALPHA;
        case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return DS_FACTOR_ONE_MINUS_SRC_ALPHA;
        case NVN_BLEND_FUNC_DST_ALPHA: return DS_FACTOR_DST_ALPHA;
        case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return DS_FACTOR_ONE_MINUS_DST_ALPHA;
        case NVN_BLEND_FUNC_DST_COLOR: return DS_FACTOR_DST_COLOR;
        case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return DS_FACTOR_ONE_MINUS_DST_COLOR;
        case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return DS_FACTOR_SRC_ALPHA_SATURATE;
        case NVN_BLEND_FUNC_SRC1_COLOR: return DS_FACTOR_SRC1_COLOR;
        case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return DS_FACTOR_ONE_MINUS_SRC1_COLOR;
        case NVN_BLEND_FUNC_SRC1_ALPHA: return DS_FACTOR_SRC1_ALPHA;
        case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return DS_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case NVN_BLEND_FUNC_CONSTANT_COLOR: return DS_FACTOR_CONSTANT_COLOR;
        case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return DS_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case NVN_BLEND_FUNC_CONSTANT_ALPHA: return DS_FACTOR_CONSTANT_ALPHA;
        case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return DS_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    return DS_FACTOR_ONE;
}
/* NVN's blend equations, compare functions and stencil operations count from 1 in Vulkan's order */
static u8 from1(u64 v, u8 max, u8 dflt) { return v >= 1 && v <= (u64)max + 1 ? (u8)(v - 1) : dflt; }

/* An NVN vertex attribute format's 3D engine size code and type (1 snorm, 2 unorm, 3 sint, 4 uint, 5 uscaled,
   6 sscaled, 7 float; vk/render.c's attr_format), 0 for a format no attribute takes */
static int attr_format(u32 f, u8* size, u8* type) {
    static const u8 by_comps[3][4] = {
        {0x1D, 0x18, 0x13, 0x0A}, /* 8 bits: 8, 8_8, 8_8_8, 8_8_8_8 */
        {0x1B, 0x0F, 0x05, 0x03}, /* 16 */
        {0x12, 0x04, 0x02, 0x01}, /* 32 */
    };
    u32 comps = 0, bits = 0, kind = 0;
    if (f >= NVN_FORMAT_R8 && f <= NVN_FORMAT_RGBA32I) { /* R, RG, RGB, RGBA: 12 formats each */
        static const u8 kinds[12] = {2, 1, 4, 3, 7, 2, 1, 4, 3, 7, 4, 3};
        static const u8 width[12] = {0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2};
        u32 i = (f - NVN_FORMAT_R8) % 12;
        comps = (f - NVN_FORMAT_R8) / 12 + 1;
        kind = kinds[i];
        bits = width[i];
    } else if (f >= NVN_FORMAT_R8_UI2F && f <= NVN_FORMAT_RGBA32_I2F) { /* the scaled ones: 6 a component count */
        u32 i = (f - NVN_FORMAT_R8_UI2F) % 6;
        comps = (f - NVN_FORMAT_R8_UI2F) / 6 + 1;
        kind = i & 1 ? 6 : 5;
        bits = i / 2;
    } else if (f >= NVN_FORMAT_RGBX8 && f <= NVN_FORMAT_RGBX32I) { /* four components, as RGBA's */
        static const u8 kinds[11] = {2, 1, 4, 3, 7, 2, 1, 4, 3, 7, 4};
        static const u8 width[11] = {0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2};
        u32 i = f - NVN_FORMAT_RGBX8;
        comps = 4;
        kind = i == 10 ? 3 : kinds[i]; /* RGBX32F, RGBX32UI, RGBX32I */
        bits = width[i];
        if (f == NVN_FORMAT_RGBX32F) kind = 7;
        if (f == NVN_FORMAT_RGBX32UI) kind = 4;
    } else {
        switch (f) {
            case NVN_FORMAT_RGB10A2: *size = 0x30, *type = 2; return 1;
            case NVN_FORMAT_RGB10A2SN: *size = 0x30, *type = 1; return 1;
            case NVN_FORMAT_RGB10A2UI: *size = 0x30, *type = 4; return 1;
            case NVN_FORMAT_RGB10A2I: *size = 0x30, *type = 3; return 1;
            case NVN_FORMAT_RGB10A2_UI2F: *size = 0x30, *type = 5; return 1;
            case NVN_FORMAT_RGB10A2_I2F: *size = 0x30, *type = 6; return 1;
            case NVN_FORMAT_R11G11B10F: *size = 0x31, *type = 7; return 1;
            case NVN_FORMAT_RGBA8_SRGB:
            case NVN_FORMAT_BGRA8: *size = 0x0A, *type = 2; return 1;
        }
        return 0;
    }
    *size = by_comps[bits][comps - 1];
    *type = (u8)kind;
    return 1;
}

/* ---- render targets and clears */
/* whether the targets t and u differ in what DS_TARGET_FORMATS covers */
static int target_formats_differ(const DsTargets* t, const DsTargets* u) {
    u32 i;
    if (t->count != u->count || t->srgb_write != u->srgb_write || t->msaa != u->msaa) return 1;
    for (i = 0; i < DS_TARGETS_MAX; i++)
        if (t->color[i].format != u->color[i].format) return 1;
    return 0;
}
/* a render target from a texture snapshot and its view, as the 3D engine's registers describe one: a multisampled
   texture's size in samples, its sample layout in *msaa (the texture header's, nvn_tic.c) */
static int target(u64 snapshot, const NvnView* v, int has_view, DsSurf* s, int* depth, u32* msaa) {
    NvnTexture t;
    NvnTexLayout l;
    u32 format, level = 0, layer = 0, layers = 1, mode;
    unsigned th, td;
    memset(s, 0, sizeof *s);
    *msaa = DS_MSAA_1X1;
    if (!snapshot) return 0;
    memset(&t, 0, sizeof t);
    memcpy(&t, GP(snapshot), NVN_TEX_SNAP);
    if (t.magic != NVN_MAGIC('T', 'E', 'X', 0) || !t.gpu) return 0;
    format = has_view && (v->set & NVN_VIEW_FORMAT) ? v->format : t.d.format;
    s->format = nvn_rt_format(format, depth);
    if (!s->format) {
        LOG_LIMIT(8, "nvn: a render target of format %#x, which no target takes\n", format);
        return 0;
    }
    nvn_tex_layout(&t.d, &l);
    if (has_view && (v->set & NVN_VIEW_LEVELS)) level = v->base_level;
    if (t.d.target == NVN_TEXTURE_TARGET_2D_ARRAY || t.d.target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY ||
        t.d.target == NVN_TEXTURE_TARGET_CUBEMAP || t.d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) {
        layers = t.d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 : t.d.depth;
        if (has_view && (v->set & NVN_VIEW_LAYERS)) {
            layer = v->min_layer;
            layers = v->layers ? v->layers : layers - layer;
        }
    }
    s->gva = t.gpu + nvn_tex_level_offset(&t.d, level) + (u64)layer * l.layer_size;
    mode = nvn_tex_msaa(t.d.samples);
    *msaa = mode;
    s->width = (t.d.width >> level ? t.d.width >> level : 1) << ds_msaa_log2_x(mode);
    s->height = (t.d.height >> level ? t.d.height >> level : 1) << ds_msaa_log2_y(mode);
    s->layers = layers;
    s->layer_stride = l.layer_size;
    if (t.d.flags & NVN_TEX_LINEAR) {
        int bw, bh, bpb = nvn_fmt_bpb((int)t.d.format, &bw, &bh);
        u32 align = (t.d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET) ? 128 : 32;
        s->mem = 1u << 12;
        s->width = t.d.stride ? t.d.stride : ((u32)bpb * s->width + align - 1) & ~(align - 1);
    } else {
        /* the width a whole number of GOBs (64 bytes) across, as the game's driver gives a block-linear target */
        int bw, bh, bpb = nvn_fmt_bpb((int)format, &bw, &bh);
        if (bpb > 0 && bw == 1) s->width = ((u32)bpb * s->width + 63) / 64 * 64 / (u32)bpb;
        nvn_tex_level_tiles(&t.d, level, &th, &td);
        if (t.d.target == NVN_TEXTURE_TARGET_3D) { /* a volume: its slices, the GOB slices of the 3D surface */
            u32 slices = t.d.depth >> level ? t.d.depth >> level : 1;
            s->layers = slices;
            /* a view of some slices targets those, from the first one's GOBs on, as the game's driver binds each
               slice of a volume to a target of its own */
            if (has_view && (v->set & NVN_VIEW_LAYERS) && v->min_layer < slices) {
                s->gva += nvn_tex_slice_offset(&t.d, level, v->min_layer);
                s->layers = v->layers && v->layers <= slices - v->min_layer ? v->layers : slices - v->min_layer;
            }
            s->mem |= 1u << 16;
        }
        s->mem |= th << 4 | td << 8;
    }
    return 1;
}
int nvn_draw_surface(u64 snapshot, const NvnView* v, DsSurf* s, int* depth, u32* msaa) {
    return target(snapshot, v, v != NULL, s, depth, msaa);
}
static void set_targets(NvnDraw* d, u64 payload) {
    NvnPSetRT r;
    DsTargets t;
    i32 i;
    int depth, have = 0;
    u32 msaa;
    memcpy(&r, GP(payload), sizeof r);
    memset(&t, 0, sizeof t);
    t.count = r.count > 0 && r.count <= DS_TARGETS_MAX ? (u32)r.count : 0;
    t.srgb_write = 1; /* NVN writes sRGB targets as sRGB */
    /* NVN requires every target of a draw to have the same samples: the first one bound gives the layout, and one
       with other samples isn't bound */
    for (i = 0; i <= (i32)t.count; i++) {
        DsSurf* s = i < (i32)t.count ? &t.color[i] : &t.depth;
        int ok = i < (i32)t.count ? target(r.color[i], &r.cview[i], r.has_cview[i], s, &depth, &msaa) && !depth
                                  : target(r.depth, &r.dview, r.has_dview, s, &depth, &msaa) && depth;
        if (ok && have && msaa != t.msaa) {
            LOG_LIMIT(4, "nvn: render targets of sample layouts %u and %u bound together: the second isn't bound\n",
                      (unsigned)t.msaa, (unsigned)msaa);
            ok = 0;
        }
        if (!ok) {
            memset(s, 0, sizeof *s);
            continue;
        }
        t.msaa = (u8)msaa;
        have = 1;
    }
    if (target_formats_differ(&t, &d->ds.tgt)) new_gen(&d->ds.gen[DS_TARGET_FORMATS]);
    keep(&d->ds.tgt, &t, sizeof t, &d->ds.gen[DS_TARGETS]);
    d->dirty |= D_DYNAMIC | D_VIEWPORTS; /* the surface's height, which a lower-left origin measures from */
}

/* ---- the groups, made of the NVN state at a draw */
static void make_programs(NvnDraw* d) {
    DsPrograms p;
    memset(&p, 0, sizeof p);
    memcpy(p.code, d->code, sizeof p.code);
    p.on = d->prog_on;
    p.clip_enable = 0xff; /* NVN has no clip distance enables: those the program writes clip */
    p.xfb_on = d->tf_on;
    keep(&d->ds.prog, &p, sizeof p, &d->ds.gen[DS_PROGRAMS]);
    keep(&d->ds.xfb, &d->xfb, sizeof d->xfb, &d->ds.gen[DS_XFB]);
}
static void make_vertex_format(NvnDraw* d) {
    DsVertexFormat f;
    u32 i;
    memset(&f, 0, sizeof f);
    for (i = 0; i < DS_ATTRS; i++) {
        u32 a = i < d->nattr ? d->attr[i] : 0, s = (u32)nvn_field(a, NVN_VA_STREAM);
        u8 size, type;
        if (!nvn_field(a, NVN_VA_FORMAT) || !attr_format((u32)nvn_field(a, NVN_VA_FORMAT), &size, &type)) {
            if (nvn_field(a, NVN_VA_FORMAT))
                LOG_LIMIT(4, "nvn: a vertex attribute of format %#x\n", (unsigned)nvn_field(a, NVN_VA_FORMAT));
            f.constant |= 1u << i;
            continue;
        }
        f.stream[i] = (u8)s;
        f.offset[i] = (u16)nvn_field(a, NVN_VA_OFFSET);
        f.size[i] = size;
        f.type[i] = type;
        if (type == 3 || type == 4) f.is_int |= 1u << i;
        if (type == 3) f.is_sint |= 1u << i;
        if (nvn_field(a, NVN_VA_FORMAT) == NVN_FORMAT_BGRA8) f.bgra |= 1u << i;
        f.stream_on |= 1u << s;
    }
    for (i = 0; i < 16 && i < d->nstream; i++) {
        f.stride[i] = (u16)d->stream[i].stride;
        f.divisor[i] = d->stream[i].divisor;
        if (d->stream[i].divisor) f.instanced |= 1u << i;
    }
    keep(&d->ds.vfmt, &f, sizeof f, &d->ds.gen[DS_VFORMAT]);
}
static void make_blend(NvnDraw* d) {
    DsBlend b;
    u32 i, logic = (u32)nvn_field(d->color, NVN_COLOR_LOGIC_OP);
    memset(&b, 0, sizeof b);
    for (i = 0; i < DS_TARGETS_MAX; i++) {
        u64 w = d->blend[i];
        b.en[i] = (u8)((nvn_field(d->color, NVN_COLOR_BLEND_EN) >> i) & 1);
        b.cop[i] = from1(nvn_field(w, NVN_BLEND_EQ_RGB), DS_BLEND_MAX, DS_BLEND_ADD);
        b.sc[i] = blend_factor((u32)nvn_field(w, NVN_BLEND_SRC_RGB));
        b.dc[i] = blend_factor((u32)nvn_field(w, NVN_BLEND_DST_RGB));
        b.aop[i] = from1(nvn_field(w, NVN_BLEND_EQ_A), DS_BLEND_MAX, DS_BLEND_ADD);
        b.sa[i] = blend_factor((u32)nvn_field(w, NVN_BLEND_SRC_A));
        b.da[i] = blend_factor((u32)nvn_field(w, NVN_BLEND_DST_A));
        b.mask[i] = (u8)((d->chmask >> (4 * i)) & 15); /* r 1, g 2, b 4, a 8, as Vulkan's */
        if (b.en[i] && nvn_field(w, NVN_BLEND_ADV))
            LOG_LIMIT(1, "nvn: advanced blend mode %u is drawn as plain blending\n",
                      (unsigned)nvn_field(w, NVN_BLEND_ADV));
    }
    /* NVN's logic operations are Vulkan's; COPY is NVN's default, writing what the shader made */
    if (logic != NVN_LOGIC_OP_COPY) b.logic_op = (u8)(logic + 1);
    keep(&d->ds.blend, &b, sizeof b, &d->ds.gen[DS_BLEND]);
}
static void make_raster_key(NvnDraw* d) {
    DsRasterKey k;
    u32 mode = (u32)nvn_field(d->poly, NVN_POLY_MODE);
    memset(&k, 0, sizeof k);
    k.polygon = (u8)(mode == NVN_POLYGON_MODE_POINT  ? DS_POLYGON_POINT
                     : mode == NVN_POLYGON_MODE_LINE ? DS_POLYGON_LINE
                                                     : DS_POLYGON_FILL);
    k.provoking_last = d->provoking_last;
    k.depth_clamp = d->depth_clamp;
    k.conservative = d->conservative ? (u8)(1 + d->dilate) : 0;
    k.sample_shading = d->sample_shading == 1; /* NVN_SAMPLE_SHADING_ENABLE: every sample */
    k.alpha_to_coverage = d->ms.alpha_to_coverage;
    k.sample_mask = ~d->sample_off;
    keep(&d->ds.rkey, &k, sizeof k, &d->ds.gen[DS_RASTER_KEY]);
}
/* the height of the targets' surface, which a lower-left window origin measures from */
static u32 surface_height(const DsTargets* t) {
    u32 i;
    for (i = 0; i < t->count; i++)
        if (t->color[i].gva) return t->color[i].height;
    return t->depth.gva ? t->depth.height : 0;
}
static void make_dynamic(NvnDraw* d, u32 base_instance) {
    DsDynamic y;
    u64 s = d->dss;
    u32 alpha = (u32)nvn_field(d->color, NVN_COLOR_ALPHA_FUNC);
    int f;
    memset(&y, 0, sizeof y);
    y.cull = (u8)nvn_field(d->poly, NVN_POLY_CULL); /* NVN's faces are Vulkan's cull modes */
    y.front_cw = nvn_field(d->poly, NVN_POLY_FRONT) == NVN_FRONT_FACE_CW;
    y.discard = d->discard;
    y.bias_on = (u8)nvn_field(d->poly, NVN_POLY_OFFSET_EN); /* point 1, line 2, fill 4: the same bits */
    y.depth_test = (u8)nvn_field(s, NVN_DS_TEST);
    y.depth_write = (u8)nvn_field(s, NVN_DS_WRITE);
    y.depth_cmp = from1(nvn_field(s, NVN_DS_FUNC), DS_CMP_ALWAYS, DS_CMP_LESS);
    y.stencil_test = (u8)nvn_field(s, NVN_DS_STENCIL);
    y.alpha_test = alpha != NVN_ALPHA_FUNC_ALWAYS;
    y.alpha_func = from1(alpha, DS_CMP_ALWAYS, DS_CMP_ALWAYS);
    /* lower left: y counts up from the surface's bottom; upper left reverses the facing (one game's NVN queue writes
       the window origin register 0x10, the flip, after its first submission) */
    y.window_origin = __atomic_load_n(&g_nvn_device.origin, __ATOMIC_RELAXED) == NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT
                          ? DS_ORIGIN_FLIP_Y
                          : DS_ORIGIN_LOWER_LEFT;
    y.depth_mode = __atomic_load_n(&g_nvn_device.depth_mode, __ATOMIC_RELAXED) == NVN_DEPTH_MODE_NEAR_IS_ZERO;
    y.point_nudge = 1; /* the game's driver leaves the OpenGL point center and half-integer pixel centers on (0x597
                          and 0x649 zero) */
    for (f = 0; f < 2; f++) {
        y.face[f].fail = from1(nvn_field(s, f ? NVN_DS_FAIL_B : NVN_DS_FAIL_F), DS_STENCIL_DECREMENT_AND_WRAP, 0);
        y.face[f].zfail = from1(nvn_field(s, f ? NVN_DS_ZFAIL_B : NVN_DS_ZFAIL_F), DS_STENCIL_DECREMENT_AND_WRAP, 0);
        y.face[f].pass = from1(nvn_field(s, f ? NVN_DS_ZPASS_B : NVN_DS_ZPASS_F), DS_STENCIL_DECREMENT_AND_WRAP, 0);
        y.face[f].cmp = from1(nvn_field(s, f ? NVN_DS_SFUNC_B : NVN_DS_SFUNC_F), DS_CMP_ALWAYS, DS_CMP_ALWAYS);
        y.face[f].ref = d->sref[f];
        y.face[f].cmask = d->svalue[f];
        y.face[f].wmask = d->smask[f];
    }
    y.line_width = d->line_width;
    y.bias_slope = d->bias[0];
    /* the 3D engine's units register holds twice NVN's units: the game's driver writes -11.32 for a game's -5.66 */
    y.bias_units = d->bias[1] * 2.0f;
    y.bias_clamp = d->bias[2];
    memcpy(y.blend_const, d->blend_color, sizeof y.blend_const);
    y.clip_height = surface_height(&d->ds.tgt);
    /* NVN's multisample state names its targets' samples, which the targets themselves give here; with
       multisampling off a multisampled target is still drawn at every sample (Vulkan has no single-sample
       rasterization into one) */
    if (d->ds.tgt.msaa != DS_MSAA_1X1 && !d->ms.enable)
        LOG_LIMIT(1, "nvn: a multisampled target drawn with multisampling off is drawn at every sample\n");
    y.depth_word = base_instance; /* the base instance, as the 3D engine's draws push it */
    keep(&d->ds.dyn, &y, sizeof y, &d->ds.gen[DS_DYNAMIC]);
}
/* clamped to the 3D engine's 16-bit window coordinates */
static u32 clamp16(i64 v) { return v < 0 ? 0 : v > 0xffff ? 0xffff : (u32)v; }
static void make_viewports(NvnDraw* d) {
    DsViewports v;
    int near_zero = __atomic_load_n(&g_nvn_device.depth_mode, __ATOMIC_RELAXED) == NVN_DEPTH_MODE_NEAR_IS_ZERO;
    /* clip space's y points up in both origins: an upper-left window's rows count down, so its scale is negative */
    int upper_left = __atomic_load_n(&g_nvn_device.origin, __ATOMIC_RELAXED) == NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT;
    u32 j, k;
    memset(&v, 0, sizeof v);
    v.transform_on = 1;
    v.scissor_on = 1; /* scissor 0, the only one recorded: NVN's covers everything until it is set */
    for (j = 0; j < DS_VIEWPORTS_MAX; j++) {
        float x = d->vp[j][0], y = d->vp[j][1], w = d->vp[j][2], h = d->vp[j][3];
        float n = j ? 0.0f : d->depth[0], f = j ? 1.0f : d->depth[1];
        v.scale[j][0] = w * 0.5f;
        v.scale[j][1] = upper_left ? -h * 0.5f : h * 0.5f;
        v.translate[j][0] = x + w * 0.5f;
        v.translate[j][1] = y + h * 0.5f;
        v.scale[j][2] = near_zero ? f - n : (f - n) * 0.5f;
        v.translate[j][2] = near_zero ? n : (n + f) * 0.5f;
        for (k = 0; k < 4; k++) v.swizzle[j] |= (u32)((d->swizzle[j][k] ^ (2 * k)) & 7) << (4 * k);
        v.clip_x[j] = clamp16((i64)x) | clamp16((i64)w) << 16;
        v.clip_y[j] = clamp16((i64)y) | clamp16((i64)h) << 16;
        v.z_near[j] = n;
        v.z_far[j] = f;
    }
    v.scissor_x[0] = clamp16(d->scissor[0]) | clamp16((i64)d->scissor[0] + d->scissor[2]) << 16;
    v.scissor_y[0] = clamp16(d->scissor[1]) | clamp16((i64)d->scissor[1] + d->scissor[3]) << 16;
    keep(&d->ds.vp, &v, sizeof v, &d->ds.gen[DS_VIEWPORTS]);
}
static void make_vertex_buffers(NvnDraw* d) {
    DsVertexBuffers b;
    u32 i;
    memset(&b, 0, sizeof b);
    for (i = 0; i < 16; i++) {
        b.addr[i] = d->vbuf[i].address;
        b.size[i] = d->vbuf[i].size;
    }
    keep(&d->ds.vbuf, &b, sizeof b, &d->ds.gen[DS_VBUF]);
}

/* ---- the draws */
/* the groups the records since the last draw changed, made again, and the draw's own words in bank 0 */
static void sync(NvnDraw* d, i32 base_vertex, u32 base_instance) {
    int s;
    for (s = 0; s < DS_GFX_STAGES; s++) {
        u8* c0 = d->inl.bank0[s];
        if (snx_ld_le32(c0 + C0_BASE_VERTEX) != (u32)base_vertex ||
            snx_ld_le32(c0 + C0_BASE_INSTANCE) != base_instance) {
            snx_st_le32(c0 + C0_BASE_VERTEX, (u32)base_vertex);
            snx_st_le32(c0 + C0_BASE_INSTANCE, base_instance);
            d->dirty |= D_INLINE;
        }
    }
    if (d->ds.dyn.depth_word != base_instance) d->dirty |= D_DYNAMIC;
    if (d->dirty & D_PROGRAMS) make_programs(d);
    if (d->dirty & D_VFORMAT) make_vertex_format(d);
    if (d->dirty & D_BLEND) make_blend(d);
    if (d->dirty & D_RASTER) make_raster_key(d);
    if (d->dirty & D_DYNAMIC) make_dynamic(d, base_instance);
    if (d->dirty & D_VIEWPORTS) make_viewports(d);
    if (d->dirty & D_VBUF) make_vertex_buffers(d);
    if (d->dirty & D_BIND) keep(&d->ds.bind, &d->bind, sizeof d->bind, &d->ds.gen[DS_BIND]);
    if (d->dirty & D_INLINE) keep(&d->ds.inl, &d->inl, sizeof d->inl, &d->ds.gen[DS_INLINE]);
    d->dirty = 0;
}
/* An indirect draw's arguments stay in guest memory, which the GPU reads when it draws (DrawCall.indirect): NVN's
   DrawArraysIndirect and DrawElementsIndirect records have Vulkan's indirect command layouts, {count, instances, first,
   base instance} and {count, instances, first index, base vertex, base instance}, and a multi-draw's count is a 32-bit
   word the GPU reads too. Nothing of them is read on the CPU: a compute program may write them just before */
static void indirect(DrawCall* dc, u64 ind, u64 count, i32 max, i32 stride, u32 record) {
    dc->indirect = ind;
    dc->indirect_count = count;
    dc->draw_count = count ? (u32)max : 1;
    dc->stride = count && stride > 0 ? (u32)stride : record;
    dc->count = dc->instances = 1; /* the GPU's: these only say there is a draw */
}
static void draw_arrays(NvnDraw* d, u64 payload, u32 size) {
    NvnPDraw r;
    DrawCall dc;
    memset(&r, 0, sizeof r);
    memcpy(&r, GP(payload), size < sizeof r ? size : sizeof r);
    memset(&dc, 0, sizeof dc);
    dc.topology = (u32)r.prim; /* NVN's primitives are the 3D engine's numbers */
    if (r.ind)
        indirect(&dc, r.ind, r.mdi_count, r.mdi_max, r.mdi_stride, 16);
    else {
        if (r.count <= 0) return;
        dc.first = (u32)r.first;
        dc.count = (u32)r.count;
        dc.base_instance = (u32)r.base_instance;
        dc.instances = r.instances > 0 ? (u32)r.instances : 1;
    }
    if (d->render_off) return;
    sync(d, 0, dc.base_instance);
    if (g_settings.nvn_verify) nvn_verify_live_draw(&d->ds, &dc);
    if (g_renderer->draw) g_renderer->draw(&d->ds, &dc, &d->own);
}
/* how far an indirect draw's indices may reach, which NVN doesn't say (DrawElementsIndirect takes the index buffer's
   address only, and the GPU reads the count): the guest memory mapped contiguously from that address, at most 64 MiB */
#define INDIRECT_INDEX_SPAN (64ull << 20)
static void draw_elements(NvnDraw* d, u64 payload, u32 size) {
    NvnPDrawIdx r;
    DrawCall dc;
    DsIndex x;
    u32 isz;
    u64 cpu;
    memset(&r, 0, sizeof r);
    memcpy(&r, GP(payload), size < sizeof r ? size : sizeof r);
    memset(&dc, 0, sizeof dc);
    if ((u32)r.index_type > NVN_INDEX_TYPE_UNSIGNED_INT || (!r.ind && r.count <= 0)) return;
    isz = 1u << r.index_type;
    memset(&x, 0, sizeof x);
    x.addr = r.index_buffer;
    if (r.ind) {
        u64 run = gmmu_run(r.index_buffer, INDIRECT_INDEX_SPAN, &cpu);
        if (run < isz) {
            LOG_LIMIT(4, "nvn: an indirect draw's index buffer at %llx isn't mapped\n",
                      (unsigned long long)r.index_buffer);
            return;
        }
        x.end = r.index_buffer + run - 1;
    } else
        x.end = r.index_buffer + (u64)r.count * isz - 1;
    x.format = (u32)r.index_type; /* 8, 16 and 32-bit as the 3D engine numbers them */
    x.restart = d->restart;
    x.restart_index = d->restart_index;
    keep(&d->ds.index, &x, sizeof x, &d->ds.gen[DS_INDEX]);
    dc.topology = (u32)r.prim;
    dc.indexed = 1;
    if (r.ind)
        indirect(&dc, r.ind, r.mdi_count, r.mdi_max, r.mdi_stride, 20);
    else {
        dc.count = (u32)r.count;
        dc.base_vertex = (u32)r.base_vertex;
        dc.base_instance = (u32)r.base_instance;
        dc.instances = r.instances > 0 ? (u32)r.instances : 1;
    }
    if (d->render_off) return;
    sync(d, (i32)dc.base_vertex, dc.base_instance);
    if (g_settings.nvn_verify) nvn_verify_live_draw(&d->ds, &dc);
    if (g_renderer->draw) g_renderer->draw(&d->ds, &dc, &d->own);
}

/* ---- bindings */
/* 8 bytes at off of an inline block of size bytes, when they fit */
static void put64(NvnDraw* d, u8* block, u32 size, u32 off, u64 v) {
    if (off + 8 > size) {
        LOG_LIMIT(1, "nvn: a handle at %#x of a driver constant buffer, past its %#x bytes\n", off, size);
        return;
    }
    snx_st_le64(block + off, v);
    d->dirty |= D_INLINE;
}
static void bind_buffer(NvnDraw* d, u32 op, const NvnPBindBuf* r) {
    int s = gfx_stage(r->stage);
    if (r->index < 0 || r->index >= 16) return;
    if (r->stage == NVN_SHADER_STAGE_COMPUTE) {
        /* bank 0's tables of {address, size}: uniform buffers from 0x210, which are also constant buffers 3 + r
           while there are slots, storage buffers from 0x310 */
        u32 at = (op == OP_BIND_UBO ? 0x210 : 0x310) + 16 * (u32)r->index;
        snx_st_le64(d->dx.bank0 + at, r->address);
        snx_st_le32(d->dx.bank0 + at + 8, (u32)r->size);
        if (op == OP_BIND_UBO && 3 + r->index < DX_CBUFS) {
            u32 slot = 3 + (u32)r->index;
            d->dx.cb_addr[slot] = r->address;
            d->dx.cb_size[slot] = r->size >= 0x10000 ? 0 : (u32)r->size;
            d->dx.cb_on = r->size ? d->dx.cb_on | 1u << slot : d->dx.cb_on & ~(1u << slot);
        }
        return;
    }
    if (s < 0) return;
    if (op == OP_BIND_UBO) {
        if (3 + r->index >= DS_CBUFS) return;
        d->bind.cb_addr[s][3 + r->index] = r->address;
        d->bind.cb_size[s][3 + r->index] = r->size >= 0x10000 ? 0 : (u32)r->size;
        d->dirty |= D_BIND;
    } else {
        /* the storage buffer table's base in bank 0, measured for the vertex and fragment stages */
        u32 base = s == 0 ? 0x110 : s == 4 ? 0x510 : 0;
        if (!base) {
            LOG_LIMIT(1, "nvn: a storage buffer of shader stage %d, whose table nobody measured\n", r->stage);
            return;
        }
        put64(d, d->inl.bank0[s], DS_INLINE_BANK0, base + 16 * (u32)r->index, r->address);
        snx_st_le32(d->inl.bank0[s] + base + 16 * (u32)r->index + 8, (u32)r->size);
    }
}
static void bind_handles(NvnDraw* d, u32 op, u64 payload) {
    const NvnPBindHandles* r = NVN_OBJ(const NvnPBindHandles, payload);
    int s = gfx_stage(r->stage);
    u8* bank2 = r->stage == NVN_SHADER_STAGE_COMPUTE ? d->dx.bank2 : s >= 0 ? d->inl.bank2[s] : NULL;
    i32 i;
    if (!bank2) return;
    for (i = 0; i < r->count && i < 32; i++) {
        /* a deferred bind reads the handles where they are as the GPU runs it: eight bytes each, which the game's CPU
           writes, read on the replay's thread as the renderer reads the bank they go into on the CPU */
        u64 h = r->defer ? gr64(r->src + 8 * (u64)i) : r->h[i];
        i32 slot = r->first + i;
        u32 off = op == OP_BIND_SAMP     ? C2_SAMP + 8 * (u32)slot
                  : op == OP_BIND_IMG    ? C2_IMG + 8 * (u32)slot
                  : slot >= SEPTEX_SLOT0 ? C2_SEPTEX + 8 * (u32)(slot - SEPTEX_SLOT0)
                                         : C2_TEX + 8 * (u32)slot;
        if (slot >= 0) put64(d, bank2, DS_INLINE_BANK2, off, h);
    }
}
/* the capture buffers, each capturing when the layout gives it words or a stride (as the 3D engine's registers
   do, maxwell/draw_state.c mx_draw_bindings) */
static void tf_buffers(NvnDraw* d) {
    u32 i;
    for (i = 0; i < 4; i++) {
        d->bind.xfb[i].addr = d->tf[i].address;
        d->bind.xfb[i].size = d->tf[i].size > 0x7fffffffu ? 0x7fffffffu : (u32)d->tf[i].size;
        d->bind.xfb[i].on = d->tf[i].address && (d->xfb.count[i] || d->xfb.stride[i]);
    }
    d->dirty |= D_BIND;
}
static void bind_program(NvnDraw* d, u64 payload) {
    NvnPBindProgram r;
    static const i32 nvn_stage[DS_PROGRAM_SLOTS] = {NVN_SHADER_STAGE_VERTEX,       NVN_SHADER_STAGE_VERTEX,
                                                    NVN_SHADER_STAGE_TESS_CONTROL, NVN_SHADER_STAGE_TESS_EVALUATION,
                                                    NVN_SHADER_STAGE_GEOMETRY,     NVN_SHADER_STAGE_FRAGMENT};
    u32 j;
    memcpy(&r, GP(payload), sizeof r);
    for (j = 0; j < DS_PROGRAM_SLOTS; j++) {
        i32 n = nvn_stage[j];
        const NvnStage* st = &r.st[n];
        int s = gfx_stage(n);
        if (!((u32)r.stages >> n & 1)) continue; /* a later bind replaces only its stages */
        d->code[j] = 0;
        d->prog_on &= ~(1u << j);
        if (st->data && (j || r.off_a)) {
            d->code[j] = st->data + (j ? st->off : r.off_a);
            d->prog_on |= 1u << j;
        }
        if (j == 0) continue;
        /* bank 1: the program's constants, after its code */
        d->bind.cb_addr[s][1] = st->data && st->cb_size ? st->data + 256 * (u64)st->cb : 0;
        d->bind.cb_size[s][1] = 256 * (u32)st->cb_size;
    }
    if ((u32)r.stages & NVN_SHADER_STAGE_FRAGMENT_BIT) {
        d->sample_shading = (u8)(r.sample_shading >> 8); /* as its SetShaders applied it */
        d->dirty |= D_RASTER;
    }
    if ((u32)r.stages & NVN_SHADER_STAGE_COMPUTE_BIT) {
        const NvnStage* st = &r.st[NVN_SHADER_STAGE_COMPUTE];
        d->dx.code = st->data ? st->data + st->off : 0;
        d->dx.block[0] = r.block[0], d->dx.block[1] = r.block[1], d->dx.block[2] = r.block[2];
        d->dx.shared = r.off_a;
        /* bank 1: the program's constants, after its code */
        d->dx.cb_addr[1] = st->data && st->cb_size ? st->data + 256 * (u64)st->cb : 0;
        d->dx.cb_size[1] = 256 * (u32)st->cb_size;
        d->dx.cb_on = d->dx.cb_addr[1] ? d->dx.cb_on | 2u : d->dx.cb_on & ~2u;
    }
    /* the capture layout of the program's last stage before rasterization, when it binds one */
    if ((u32)r.stages & (NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_TESS_CONTROL_BIT |
                         NVN_SHADER_STAGE_TESS_EVALUATION_BIT | NVN_SHADER_STAGE_GEOMETRY_BIT)) {
        if (!r.serial || !nvn_program_xfb(r.serial, &d->xfb)) memset(&d->xfb, 0, sizeof d->xfb);
        tf_buffers(d);
    }
    d->dirty |= D_PROGRAMS | D_BIND;
}
static void bind_state(NvnDraw* d, u64 payload) {
    const NvnPBindState* r = NVN_OBJ(const NvnPBindState, payload);
    u64 w;
    u32 v;
    memcpy(&w, r->bytes, 8);
    memcpy(&v, r->bytes, 4);
    switch (r->kind) {
        case 0: d->blend[nvn_field(w, NVN_BLEND_TARGET)] = w, d->dirty |= D_BLEND; break;
        case 1: d->color = v, d->dirty |= D_BLEND | D_DYNAMIC; break;
        case 2: d->chmask = v, d->dirty |= D_BLEND; break;
        case 3: memcpy(&d->ms, r->bytes, sizeof d->ms), d->dirty |= D_DYNAMIC | D_RASTER; break;
        case 4: d->poly = v, d->dirty |= D_DYNAMIC | D_RASTER; break;
        case 5: d->dss = w, d->dirty |= D_DYNAMIC; break;
    }
}

/* SetRenderEnable (modes 2 and 3: on, off) and SetRenderEnableConditional (0: draw when the counts at address and
   address + 16 are equal, 1: when they differ), as the 3D engine's render enable (maxwell/engines.c): NVN's occlusion
   test, a samples-passed count against zero, compared by the GPU when the count isn't known yet */
static void render_enable(NvnDraw* d, const NvnPRenderCond* r) {
    u32 pred = 0;
    int known;
    if (d->own.pred) {
        g_renderer->render_predicate_drop(d->own.pred);
        d->own.pred = 0;
    }
    if (r->mode >= 2) {
        d->render_off = r->mode == 3;
        return;
    }
    d->render_off = 0;
    known = g_renderer->render_predicate ? g_renderer->render_predicate(r->address, r->mode == 0, &pred) : -1;
    if (known >= 0) {
        d->render_off = !known;
        if (known == 2) {
            d->render_off = 0;
            d->own.pred = pred;
        }
        return;
    }
    { /* counts the GPU can't compare for the renderer: read as the stream sees them, past the releases queued before
         (gmem_read), as the 3D engine's render enable reads them */
        u64 x = 0, y = 0;
        gmem_read(r->address, &x, 8);
        gmem_read(r->address + 16, &y, 8);
        d->render_off = (x == y) != (r->mode == 0);
    }
}
int nvn_draw_samples(NvnDraw* d, u64 address, u64 ticks) {
    return d->samples && g_renderer->samples_report && g_renderer->samples_report(d->ds.owner, address, 16, ticks);
}

/* ClearTexture: the region of the texture, a color target of its own, cleared within a scissor of the region, a
   layer (or a volume's slice) at a time, as the 3D engine's clears of a target do */
static void clear_texture(NvnDraw* d, u64 payload) {
    NvnPClearTex r;
    DsState* x = &d->scratch;
    DrawClear c;
    DsTargets t;
    i32 k, depth;
    int is_depth;
    u32 msaa;
    memcpy(&r, GP(payload), sizeof r);
    if (r.r.width <= 0 || r.r.height <= 0 || !(r.mask & 15) || !g_renderer->clear) return;
    memset(&t, 0, sizeof t);
    t.count = 1;
    t.srgb_write = 1;
    int ok = target(r.texture, &r.view, r.has_view, &t.color[0], &is_depth, &msaa);
    t.msaa = (u8)msaa; /* a multisampled texture's every sample, in a region of its pixels */
    if (!ok || is_depth) {
        if (is_depth) LOG_LIMIT(1, "nvn: a ClearTexture of a depth texture isn't replayed yet\n");
        return;
    }
    if (target_formats_differ(&t, &x->tgt)) new_gen(&x->gen[DS_TARGET_FORMATS]);
    keep(&x->tgt, &t, sizeof t, &x->gen[DS_TARGETS]);
    x->owner = d->ds.owner;
    {
        DsViewports v;
        memcpy(&v, &x->vp, sizeof v);
        v.scissor_on = 1;
        v.scissor_x[0] = clamp16(r.r.xoffset) | clamp16((i64)r.r.xoffset + r.r.width) << 16;
        v.scissor_y[0] = clamp16(r.r.yoffset) | clamp16((i64)r.r.yoffset + r.r.height) << 16;
        keep(&x->vp, &v, sizeof v, &x->gen[DS_VIEWPORTS]);
    }
    memset(&c, 0, sizeof c);
    c.color_mask = (u32)r.mask & 15;
    c.scissored = 1;
    memcpy(c.color, r.c, sizeof c.color); /* floats, or the integers of an integer texture */
    depth = r.r.depth > 0 ? r.r.depth : 1;
    for (k = 0; k < depth; k++) {
        c.layer = (u32)(r.r.zoffset + k);
        g_renderer->clear(x, &c, &d->own);
    }
}

/* a compute dispatch: its grid (or the GPU's words at ind) in bank 0 at 0x200, and the compute stage's state */
static void dispatch(NvnDraw* d, u64 payload) {
    const NvnPDispatch* r = NVN_OBJ(const NvnPDispatch, payload);
    u32 g[3];
    g[0] = (u32)r->x, g[1] = (u32)r->y, g[2] = (u32)r->z;
    if (!d->dx.code || !g_renderer->dispatch || (!r->ind && (!g[0] || !g[1] || !g[2]))) return;
    /* DispatchComputeIndirect's grid stays in guest memory, which the GPU reads (DispatchState.indirect) */
    d->dx.indirect = r->ind;
    if (!r->ind) {
        memcpy(d->dx.grid, g, sizeof g);
        snx_st_le32(d->dx.bank0 + 0x200, g[0]);
        snx_st_le32(d->dx.bank0 + 0x204, g[1]);
        snx_st_le32(d->dx.bank0 + 0x208, g[2]);
    }
    g_renderer->dispatch(&d->dx);
}

/* DrawTexture: the 3D engine's DRAW_TEXTURE as the game's driver writes it, in its fixed point: the destination's
   corner and size in 1/256 pixel steps, the source's corner the same, and the source's texels per destination pixel
   in 32.32. An empty destination draws nothing */
static double draw_texture_fixed(f32 v) { return floor((double)v * 256.0 + 0.5) / 256.0; }
static void draw_texture(NvnDraw* d, u64 payload) {
    NvnPDrawTexture r;
    DrawTexture t;
    double x0, y0, x1, y1;
    memcpy(&r, GP(payload), sizeof r);
    x0 = draw_texture_fixed(r.dst[0]), y0 = draw_texture_fixed(r.dst[1]);
    x1 = draw_texture_fixed(r.dst[2]), y1 = draw_texture_fixed(r.dst[3]);
    if (x0 == x1 || y0 == y1 || d->render_off || !g_renderer->draw_texture) return;
    memset(&t, 0, sizeof t);
    t.handle = (u32)r.handle; /* the texture's index in bits 0-19, its sampler's in 20-31 */
    t.x0 = (float)x0;
    t.y0 = (float)y0;
    t.w = (float)(x1 - x0);
    t.h = (float)(y1 - y0);
    t.du = floor((double)(r.src[2] - r.src[0]) / (x1 - x0) * 4294967296.0 + 0.5) / 4294967296.0;
    t.dv = floor((double)(r.src[3] - r.src[1]) / (y1 - y0) * 4294967296.0 + 0.5) / 4294967296.0;
    t.sx0 = (float)draw_texture_fixed(r.src[0]);
    t.sy0 = (float)draw_texture_fixed(r.src[1]);
    sync(d, 0, 0);
    g_renderer->draw_texture(&d->ds, &t);
}
int nvn_draw_record(NvnDraw* d, u32 op, u64 payload, u32 size) {
    switch (op) {
        case OP_SET_RT: set_targets(d, payload); return 1;
        case OP_CLEAR_COLOR: {
            NvnPClearColor r;
            DrawClear c;
            memcpy(&r, GP(payload), sizeof r);
            memset(&c, 0, sizeof c);
            c.color_mask = (u32)r.mask & 15;
            c.slot = (u32)r.index;
            memcpy(c.color, r.rgba, sizeof c.color);
            if (c.color_mask && !d->render_off && g_renderer->clear) g_renderer->clear(&d->ds, &c, &d->own);
            return 1;
        }
        case OP_CLEAR_DS: {
            NvnPClearDS r;
            DrawClear c;
            memcpy(&r, GP(payload), sizeof r);
            memset(&c, 0, sizeof c);
            c.aspects = (r.depth_mask ? 1u : 0) | (r.stencil_mask ? 2u : 0);
            c.depth = r.depth;
            c.stencil = (u32)r.stencil;
            if (c.aspects && !d->render_off && g_renderer->clear) g_renderer->clear(&d->ds, &c, &d->own);
            return 1;
        }
        case OP_VIEWPORTS: {
            i32 first = NVN_OBJ(const NvnPViewports, payload)->first, n = NVN_OBJ(const NvnPViewports, payload)->count;
            i32 i;
            for (i = 0; i < n && first + i < 16; i++)
                memcpy(d->vp[first + i], GP(payload + offsetof(NvnPViewports, v) + 24 * (u64)i), 16);
            d->dirty |= D_VIEWPORTS;
            return 1;
        }
        case OP_SCISSOR: memcpy(d->scissor, GP(payload), 16), d->dirty |= D_VIEWPORTS; return 1;
        case OP_DEPTH_RANGE: memcpy(d->depth, GP(payload), 8), d->dirty |= D_VIEWPORTS; return 1;
        case OP_VP_SWIZZLE: {
            const NvnPVpSwizzle* r = NVN_OBJ(const NvnPVpSwizzle, payload);
            i32 i;
            for (i = 0; i < r->count && r->first + i < 16; i++) memcpy(d->swizzle[r->first + i], r->sw[i], 4);
            d->dirty |= D_VIEWPORTS;
            return 1;
        }
        case OP_STENCIL: {
            const NvnPStencil* r = NVN_OBJ(const NvnPStencil, payload);
            int f;
            for (f = 0; f < 2; f++) {
                if (!((u32)r->face >> f & 1)) continue; /* NVN_FACE_FRONT 1, BACK 2 */
                if (r->which == 0) d->svalue[f] = (u32)r->value_mask & 0xff;
                if (r->which == 1) d->smask[f] = (u32)r->mask & 0xff;
                if (r->which == 2) d->sref[f] = (u32)r->ref & 0xff;
            }
            d->dirty |= D_DYNAMIC;
            return 1;
        }
        case OP_BLEND_COLOR: memcpy(d->blend_color, GP(payload), 16), d->dirty |= D_DYNAMIC; return 1;
        case OP_POLY_OFFSET: memcpy(d->bias, GP(payload), 12), d->dirty |= D_DYNAMIC; return 1;
        case OP_RAST_DISCARD:
            d->discard = NVN_OBJ(const NvnPRastDiscard, payload)->on != 0, d->dirty |= D_DYNAMIC;
            return 1;
        case OP_DEPTH_CLAMP:
            d->depth_clamp = NVN_OBJ(const NvnPDepthClamp, payload)->on != 0, d->dirty |= D_RASTER;
            return 1;
        case OP_SAMPLE_MASK:
            d->sample_off = ~(u32)NVN_OBJ(const NvnPSampleMask, payload)->mask, d->dirty |= D_RASTER;
            return 1;
        case OP_PRIM_RESTART:
            d->restart = NVN_OBJ(const NvnPPrimRestart, payload)->on != 0;
            d->restart_index = NVN_OBJ(const NvnPPrimRestart, payload)->index;
            return 1;
        case OP_BIND_PROGRAM: bind_program(d, payload); return 1;
        case OP_BIND_STATE: bind_state(d, payload); return 1;
        case OP_BIND_VATTR: {
            const NvnPBindVAttr* r = NVN_OBJ(const NvnPBindVAttr, payload);
            d->nattr = r->count > 0 && r->count <= 16 ? (u32)r->count : 0;
            memcpy(d->attr, r->st, 4 * (size_t)d->nattr);
            d->dirty |= D_VFORMAT;
            return 1;
        }
        case OP_BIND_VSTREAM: {
            const NvnPBindVStream* r = NVN_OBJ(const NvnPBindVStream, payload);
            d->nstream = r->count > 0 && r->count <= 16 ? (u32)r->count : 0;
            memcpy(d->stream, r->st, sizeof d->stream[0] * d->nstream);
            d->dirty |= D_VFORMAT;
            return 1;
        }
        case OP_BIND_VBUF: {
            const NvnPBindVBuf* r = NVN_OBJ(const NvnPBindVBuf, payload);
            i32 i;
            for (i = 0; i < r->count && r->first + i < 16; i++)
                memcpy(&d->vbuf[r->first + i],
                       GP(payload + offsetof(NvnPBindVBuf, r) + sizeof(NVNbufferRange) * (u64)i),
                       sizeof(NVNbufferRange));
            d->dirty |= D_VBUF;
            return 1;
        }
        case OP_BIND_UBO:
        case OP_BIND_SSBO: bind_buffer(d, op, NVN_OBJ(const NvnPBindBuf, payload)); return 1;
        case OP_BIND_BUFS: { /* deferred: the ranges where they are now, as the GPU reads them when it runs this */
            const NvnPBindBufs* r = NVN_OBJ(const NvnPBindBufs, payload);
            i32 i;
            for (i = 0; i < r->count && i < 16; i++) {
                NvnPBindBuf b;
                b.stage = r->stage;
                b.index = r->first + i;
                b.address = gr64(r->src + 16 * (u64)i);
                b.size = gr64(r->src + 16 * (u64)i + 8);
                bind_buffer(d, (u32)r->op, &b);
            }
            return 1;
        }
        case OP_LINE_WIDTH: memcpy(&d->line_width, GP(payload), 4), d->dirty |= D_DYNAMIC; return 1;
        case OP_PROVOKING:
            d->provoking_last = NVN_OBJ(const NvnPProvoking, payload)->mode == 1, d->dirty |= D_RASTER;
            return 1;
        case OP_CONSERVATIVE: {
            const NvnPConservative* r = NVN_OBJ(const NvnPConservative, payload);
            if (r->which)
                d->dilate = (u8)r->dilate;
            else
                d->conservative = r->on != 0;
            d->dirty |= D_RASTER;
            return 1;
        }
        case OP_DRAW_TEXTURE: draw_texture(d, payload); return 1;
        case OP_BIND_TEX:
        case OP_BIND_SAMP:
        case OP_BIND_IMG: bind_handles(d, op, payload); return 1;
        case OP_BIND_TEX1: {
            const NvnPBindTex1* r = NVN_OBJ(const NvnPBindTex1, payload);
            int s = gfx_stage(r->stage);
            u8* bank2 = r->stage == NVN_SHADER_STAGE_COMPUTE ? d->dx.bank2 : s >= 0 ? d->inl.bank2[s] : NULL;
            if (bank2 && r->slot >= 0) put64(d, bank2, DS_INLINE_BANK2, C2_TEX + 8 * (u32)r->slot, r->h);
            return 1;
        }
        case OP_SET_TEX_POOL:
        case OP_SET_SAMP_POOL: {
            const NvnPSetPool* r = NVN_OBJ(const NvnPSetPool, payload);
            u32 max = r->count > 0 ? (u32)r->count - 1 : 0;
            if (op == OP_SET_TEX_POOL)
                d->bind.tex_pool = d->dx.tex_pool = r->descriptors, d->bind.tex_max = d->dx.tex_max = max;
            else
                d->bind.smp_pool = d->dx.smp_pool = r->descriptors, d->bind.smp_max = d->dx.smp_max = max;
            d->dirty |= D_BIND;
            return 1;
        }
        case OP_UPDATE_UB: { /* the bytes land in the buffer in order with the draws around them: the command's own
                                bytes, written into guest memory as the 3D engine's LOAD_CONSTANT_BUFFER writes them */
            const NvnPUpdateUB* r = NVN_OBJ(const NvnPUpdateUB, payload);
            if (r->size && r->size <= (u64)size - sizeof *r) gmem_write(r->dst, GP(payload + sizeof *r), r->size);
            return 1;
        }
        case OP_BARRIER: {
            i32 bits;
            memcpy(&bits, GP(payload), 4);
            if (!g_renderer->method_barrier) return 1;
            if (bits & NVN_BARRIER_INVALIDATE_SHADER_BIT) g_renderer->method_barrier(5);
            if (bits & (NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_BIT))
                g_renderer->method_barrier(0);
            return 1;
        }
        case OP_CLEAR_TEX: clear_texture(d, payload); return 1;
        case OP_RENDER_COND: render_enable(d, NVN_OBJ(const NvnPRenderCond, payload)); return 1;
        case OP_TF_BIND: {
            const NvnPTfBind* r = NVN_OBJ(const NvnPTfBind, payload);
            if (r->index < 0 || r->index >= 4) return 1;
            d->tf[r->index].address = r->address;
            d->tf[r->index].size = r->size;
            tf_buffers(d);
            return 1;
        }
        case OP_TF_STATE: {
            /* Begin: the capture starts over at each buffer's start (the driver's macro resets the counters); End and
               Pause stop it, their counters kept where the renderer keeps them; Resume goes on from them. The driver
               also writes the counters into the control memory at End and Pause (4 bytes each from + 4), which only
               a DrawTransformFeedback reads, and the replay doesn't draw those yet */
            i32 what = NVN_OBJ(const NvnPTfState, payload)->what;
            d->tf_on = what == 0 || what == 3;
            if (what == 0) d->own.xfb_reload = 15;
            d->dirty |= D_PROGRAMS;
            return 1;
        }
        case OP_RESET_COUNTER:
            /* samples passed counts the queue's draws from here; the other counters count nothing yet */
            if (gr64(payload) == NVN_COUNTER_TYPE_SAMPLES_PASSED && g_renderer->samples_reset) {
                if (!d->samples && g_renderer->samples_enable) g_renderer->samples_enable(d->ds.owner, 1);
                g_renderer->samples_reset(d->ds.owner);
                d->samples = 1;
            }
            return 1;
        case OP_DISPATCH: dispatch(d, payload); return 1;
        case OP_DRAW: draw_arrays(d, payload, size); return 1;
        case OP_DRAW_IDX: draw_elements(d, payload, size); return 1;
    }
    return 0;
}
