/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's registers as the neutral draw state. Register numbers are method / 4 (NVIDIA open-gpu-doc
   classes/3d/clb197.h). Compare functions and stencil operations come in two encodings, the hardware's from 1 and
   OpenGL's enumerants, both in Vulkan's order */
#include "nx/gpu/maxwell/draw_state.h"
#include "nx/gpu/gpu.h"

static u32 cmp_op(u32 v) {
    if (v >= 0x200 && v <= 0x207) return v - 0x200;
    if (v >= 1 && v <= 8) return v - 1;
    return DS_CMP_ALWAYS;
}
static u32 stencil_op(u32 v) {
    switch (v) {
        case 1:
        case 0x1E00: return DS_STENCIL_KEEP;
        case 3:
        case 0x1E01: return DS_STENCIL_REPLACE;
        case 4:
        case 0x1E02: return DS_STENCIL_INCREMENT_AND_CLAMP;
        case 5:
        case 0x1E03: return DS_STENCIL_DECREMENT_AND_CLAMP;
        case 6:
        case 0x150A: return DS_STENCIL_INVERT;
        case 7:
        case 0x8507: return DS_STENCIL_INCREMENT_AND_WRAP;
        case 8:
        case 0x8508: return DS_STENCIL_DECREMENT_AND_WRAP;
    }
    return DS_STENCIL_ZERO;
}

/* blend factors in the D3D-style numbering from 1, or OpenGL's enumerants with bit 14 set */
static u32 blend_factor(u32 v) {
    if (v & 0x4000)
        v &= ~0x4000u; /* a GL enum */
    else
        switch (v) { /* the D3D-style numbering */
            case 1: return DS_FACTOR_ZERO;
            case 2: return DS_FACTOR_ONE;
            case 3: return DS_FACTOR_SRC_COLOR;
            case 4: return DS_FACTOR_ONE_MINUS_SRC_COLOR;
            case 5: return DS_FACTOR_SRC_ALPHA;
            case 6: return DS_FACTOR_ONE_MINUS_SRC_ALPHA;
            case 7: return DS_FACTOR_DST_ALPHA;
            case 8: return DS_FACTOR_ONE_MINUS_DST_ALPHA;
            case 9: return DS_FACTOR_DST_COLOR;
            case 10: return DS_FACTOR_ONE_MINUS_DST_COLOR;
            case 11: return DS_FACTOR_SRC_ALPHA_SATURATE;
            case 14: return DS_FACTOR_CONSTANT_COLOR;
            case 15: return DS_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            case 16: return DS_FACTOR_SRC1_COLOR;
            case 17: return DS_FACTOR_ONE_MINUS_SRC1_COLOR;
            case 18: return DS_FACTOR_SRC1_ALPHA;
            case 19: return DS_FACTOR_ONE_MINUS_SRC1_ALPHA;
            default: return DS_FACTOR_ONE;
        }
    switch (v) {
        case 0: return DS_FACTOR_ZERO;
        case 1: return DS_FACTOR_ONE;
        case 0x300: return DS_FACTOR_SRC_COLOR;
        case 0x301: return DS_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return DS_FACTOR_SRC_ALPHA;
        case 0x303: return DS_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return DS_FACTOR_DST_ALPHA;
        case 0x305: return DS_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return DS_FACTOR_DST_COLOR;
        case 0x307: return DS_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return DS_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return DS_FACTOR_CONSTANT_COLOR;
        case 0x8002: return DS_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return DS_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return DS_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        /* the dual-source factors as the hardware numbers them (0xC900-0xC903), not by their GL values */
        case 0x8900: return DS_FACTOR_SRC1_COLOR;
        case 0x8901: return DS_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 0x8902: return DS_FACTOR_SRC1_ALPHA;
        case 0x8903: return DS_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x88F9: return DS_FACTOR_SRC1_COLOR;
        case 0x88FA: return DS_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 0x8589: return DS_FACTOR_SRC1_ALPHA;
        case 0x88FB: return DS_FACTOR_ONE_MINUS_SRC1_ALPHA;
    }
    return DS_FACTOR_ONE;
}
static u32 blend_op(u32 v) {
    switch (v) {
        case 2:
        case 0x800A: return DS_BLEND_SUBTRACT;
        case 3:
        case 0x800B: return DS_BLEND_REVERSE_SUBTRACT;
        case 4:
        case 0x8007: return DS_BLEND_MIN;
        case 5:
        case 0x8008: return DS_BLEND_MAX;
    }
    return DS_BLEND_ADD;
}

void mx_draw_dynamic(const u32* r, DsDynamic* d) {
    memset(d, 0, sizeof *d);
    d->cull = (u8)(!r[0x646]           ? DS_CULL_NONE
                   : r[0x648] == 0x404 ? DS_CULL_FRONT
                   : r[0x648] == 0x408 ? DS_CULL_FRONT_AND_BACK
                                       : DS_CULL_BACK);
    d->front_cw = r[0x647] == 0x900;
    d->discard = !r[0xDF]; /* RASTERIZE_ENABLE */
    /* enabled per polygon mode: 0x370 points, 0x371 lines, 0x372 filled */
    d->bias_on = (u8)((r[0x370] != 0) | (r[0x371] != 0) << 1 | (r[0x372] != 0) << 2);
    d->depth_test = r[0x4B3] != 0;
    d->depth_write = r[0x4BA] != 0;
    d->depth_cmp = (u8)cmp_op(r[0x4C3]);
    d->stencil_test = r[0x4E0] != 0;
    d->alpha_test = r[0x4BB] & 1;
    d->alpha_func = (u8)cmp_op(r[0x4C5]);
    d->window_origin = (u8)(r[0x4EB] & (DS_ORIGIN_LOWER_LEFT | DS_ORIGIN_FLIP_Y));
    d->sprite_origin = (r[0x581] >> 2) & 1;
    d->depth_mode = r[0x35F] & 1;
    d->point_nudge = !r[0x597] && !r[0x649]; /* the OpenGL point center (0x597) and half-integer pixel centers (0x649) */
    /* GL order in the registers: fail, depth fail, pass; then the function, reference, function mask and write mask
       (the back's 0x3D5 reference, 0x3D6 write mask, 0x3D7 function mask) */
    int two = r[0x565] != 0;
    d->face[0].fail = (u8)stencil_op(r[0x4E1]);
    d->face[0].zfail = (u8)stencil_op(r[0x4E2]);
    d->face[0].pass = (u8)stencil_op(r[0x4E3]);
    d->face[0].cmp = (u8)cmp_op(r[0x4E4]);
    d->face[0].ref = r[0x4E5];
    d->face[0].cmask = r[0x4E6];
    d->face[0].wmask = r[0x4E7];
    d->face[1].fail = (u8)stencil_op(r[two ? 0x566 : 0x4E1]);
    d->face[1].zfail = (u8)stencil_op(r[two ? 0x567 : 0x4E2]);
    d->face[1].pass = (u8)stencil_op(r[two ? 0x568 : 0x4E3]);
    d->face[1].cmp = (u8)cmp_op(r[two ? 0x569 : 0x4E4]);
    d->face[1].ref = r[two ? 0x3D5 : 0x4E5];
    d->face[1].cmask = r[two ? 0x3D7 : 0x4E6];
    d->face[1].wmask = r[two ? 0x3D6 : 0x4E7];
    memcpy(&d->line_width, &r[r[0x55C] ? 0x4EC : 0x4ED], 4); /* smooth lines (0x55C) have a width of their own */
    memcpy(&d->bias_units, &r[0x56F], 4);
    memcpy(&d->bias_slope, &r[0x55B], 4);
    memcpy(&d->bias_clamp, &r[0x61F], 4);
    memcpy(d->blend_const, &r[0x4C7], 16);
    memcpy(&d->alpha_ref, &r[0x4C4], 4);
    d->clip_height = r[0x3FE] >> 16;
    d->msaa_mode = r[0x574];
    d->depth_word = r[0x50E];
}

/* SET_VIEWPORT_SCALE_X(j) at register 0x280 + 8 j (scale x, y, z, offset x, y, z, then the coordinate swizzle),
   SET_VIEWPORT_CLIP_HORIZONTAL(j) at 0x300 + 4 j (then vertical, min z, max z) and SET_SCISSOR_ENABLE(j) at
   0x380 + 4 j (then horizontal, vertical); 0x64B the transform enable */
void mx_draw_viewports(const u32* r, DsViewports* v) {
    v->transform_on = r[0x64B] & 1;
    v->scissor_on = 0;
    for (u32 j = 0; j < DS_VIEWPORTS_MAX; j++) {
        memcpy(v->scale[j], &r[0x280 + j * 8], 12);
        memcpy(v->translate[j], &r[0x283 + j * 8], 12);
        v->swizzle[j] = r[0x286 + j * 8];
        v->clip_x[j] = r[0x300 + j * 4];
        v->clip_y[j] = r[0x301 + j * 4];
        memcpy(&v->z_near[j], &r[0x302 + j * 4], 4);
        memcpy(&v->z_far[j], &r[0x303 + j * 4], 4);
        v->scissor_on |= (r[0x380 + j * 4] & 1) << j;
        v->scissor_x[j] = r[0x381 + j * 4];
        v->scissor_y[j] = r[0x382 + j * 4];
    }
}

/* The program slots at 0x800 + 0x10 j (enable bit 0, then the offset from the code base 0x582/0x583) */
void mx_draw_programs(const u32* r, DsPrograms* p) {
    u64 code = ((u64)r[0x582] << 32) | r[0x583];
    memset(p, 0, sizeof *p);
    for (u32 j = 0; j < DS_PROGRAM_SLOTS; j++) {
        p->code[j] = code + r[0x801 + 0x10 * j];
        p->on |= (r[0x800 + 0x10 * j] & 1) << j;
    }
    p->clip_enable = r[0x544] & 0xFF; /* SetUserClipEnable */
    p->tess_params = r[0xC8];
    memcpy(p->gs_skip, &r[0x490], sizeof p->gs_skip);
    p->early_z = r[0x084] & 1; /* MANDATED_EARLY_Z */
    p->xfb_on = r[0x1D1] != 0;
}

/* Attribute i's format at 0x458 + i: the stream in bits 0-4, constant bit 6, the offset in 7-20, the size code in
   21-26, the type in 27-29, BGRA bit 31. Stream s at 0x700 + 4 s (the stride in bits 0-11, enable bit 12, the divisor
   at + 3), stepped per instance when 0x620 + s is set */
void mx_draw_vertex_format(const u32* r, DsVertexFormat* f) {
    memset(f, 0, sizeof *f);
    for (u32 i = 0; i < DS_ATTRS; i++) {
        u32 a = r[0x458 + i], ty = (a >> 27) & 7;
        f->stream[i] = (u8)(a & 31);
        f->offset[i] = (u16)((a >> 7) & 0x3FFF);
        f->size[i] = (u8)((a >> 21) & 0x3F);
        f->type[i] = (u8)ty;
        f->constant |= ((a >> 6) & 1) << i;
        f->bgra |= ((a >> 31) & 1) << i;
        if (ty == 3 || ty == 4) {
            f->is_int |= 1u << i;
            if (ty == 3) f->is_sint |= 1u << i;
        }
    }
    for (u32 s = 0; s < DS_STREAMS; s++) {
        u32 w = r[0x700 + s * 4];
        f->stride[s] = (u16)(w & 0xFFF);
        f->stream_on |= ((w >> 12) & 1) << s;
        f->instanced |= (u32)(r[0x620 + s] != 0) << s;
        f->divisor[s] = r[0x703 + s * 4];
    }
}

/* Target i's color mask at 0x680 + i (0x680 for all when 0x3E4 is set), a nibble per component; its blend enable at
   0x4D8 + i, and its equations at 0x780 + 8 i (+1 to +6) when 0x4B9 is set, else all targets' at 0x4D0-0x4D6.
   The alpha fields are used even without separate alpha, as on hardware */
void mx_draw_blend(const u32* r, DsBlend* b) {
    memset(b, 0, sizeof *b);
    for (u32 i = 0; i < DS_TARGETS_MAX; i++) {
        u32 cm = r[0x3E4] ? r[0x680] : r[0x680 + i];
        b->mask[i] = (u8)(((cm & 0xF) ? DS_COMPONENT_R : 0) | ((cm & 0xF0) ? DS_COMPONENT_G : 0) |
                          ((cm & 0xF00) ? DS_COMPONENT_B : 0) | ((cm & 0xF000) ? DS_COMPONENT_A : 0));
        b->en[i] = r[0x4D8 + i] != 0;
        const u32* t = &r[0x780 + i * 8];
        u32 eq, sc, dc, eqa, sa, da;
        if (r[0x4B9]) {
            eq = t[1], sc = t[2], dc = t[3];
            eqa = t[4] ? t[4] : t[1], sa = t[5] ? t[5] : t[2], da = t[6] ? t[6] : t[3];
        } else {
            eq = r[0x4D0], sc = r[0x4D1], dc = r[0x4D2];
            eqa = r[0x4D3] ? r[0x4D3] : r[0x4D0], sa = r[0x4D4] ? r[0x4D4] : r[0x4D1];
            da = r[0x4D6] ? r[0x4D6] : r[0x4D2];
        }
        b->cop[i] = (u8)blend_op(eq);
        b->sc[i] = (u8)blend_factor(sc);
        b->dc[i] = (u8)blend_factor(dc);
        b->aop[i] = (u8)blend_op(eqa);
        b->sa[i] = (u8)blend_factor(sa);
        b->da[i] = (u8)blend_factor(da);
    }
    /* LOGIC_OP (0x671 on, 0x672 the operation as GL numbers it from CLEAR 0x1500 to SET 0x150F, VkLogicOp's order) */
    if (r[0x671] & 1) b->logic_op = (u8)((r[0x672] & 15) + 1);
}

void mx_draw_raster_key(const u32* r, DsRasterKey* k) {
    u32 clip = (r[0x64F] >> 11) & 7;
    memset(k, 0, sizeof *k);
    k->polygon = (u8)(r[0x36B] == 0x1B01 ? DS_POLYGON_LINE : r[0x36B] == 0x1B00 ? DS_POLYGON_POINT : DS_POLYGON_FILL);
    k->provoking_last = r[0x5A1] & 1; /* PROVOKING_VERTEX: 1 the last vertex, GL's and NVN's default */
    /* GeometryClip: Passthrough, FrustumXYZ and FrustumZ clip depth; the remaining modes clamp it */
    k->depth_clamp = clip != 1 && clip != 3 && clip != 5;
    k->patch_vertices = r[0x373];
}

/* Transform feedback: each buffer's control at 0x1C0 + 4 i (its stream, the words of a vertex, their stride in
   bytes) and the layout of those words at 0xA00 + 0x20 i, a byte each (an attribute word, as Eden's
   MakeTransformFeedbackVaryings reads them) */
void mx_draw_xfb(const u32* r, ShXfb* x) {
    memset(x, 0, sizeof *x);
    for (int i = 0; i < 4; i++) {
        u32 words = r[0x1C1 + 4 * i];
        x->count[i] = (u8)(words > 128 ? 128 : words);
        x->stride[i] = (u16)r[0x1C2 + 4 * i];
        memcpy(x->layout[i], &r[0xA00 + 0x20 * i], sizeof x->layout[i]); /* word n / 4's byte n % 4 */
    }
}

/* RT_CONTROL (0x487): the slot count in bits 0-3 (at most 8), then 3 bits per slot naming the target registers it
   takes, 0x200 + 0x10 j: the address (2 words), width, height, format, memory layout, third dimension (bits 0-27) and
   array pitch in words. The depth target at 0x3F8-0x3FC (address, format, memory layout, array pitch) with its size
   at 0x48A-0x48C, bound when 0x54E is set: ZETA_SIZE's dimension control (bit 16) forces one layer, else its
   depth (bits 0-15) is the array size. 0x56E: the sRGB write */
void mx_draw_targets(const u32* r, DsTargets* t) {
    u32 ctl = r[0x487], count = ctl & 15;
    if (count > DS_TARGETS_MAX) count = DS_TARGETS_MAX;
    memset(t, 0, sizeof *t);
    t->count = count;
    t->srgb_write = r[0x56E] & 1;
    for (u32 i = 0; i < count; i++) {
        const u32* c = &r[0x200 + ((ctl >> (4 + i * 3)) & 7) * 0x10];
        DsSurf* s = &t->color[i];
        s->gva = ((u64)c[0] << 32) | c[1];
        s->width = c[2];
        s->height = c[3];
        s->format = c[4];
        s->mem = c[5];
        s->layers = c[6] & 0x0fffffff;
        s->layer_stride = (u64)c[7] * 4;
    }
    t->depth.gva = r[0x54E] ? ((u64)r[0x3F8] << 32) | r[0x3F9] : 0;
    t->depth.width = r[0x48A];
    t->depth.height = r[0x48B];
    t->depth.format = r[0x3FA];
    t->depth.mem = r[0x3FB];
    t->depth.layers = r[0x48C] & (1u << 16) ? 1 : r[0x48C] & 0xffff;
    t->depth.layer_stride = (u64)r[0x3FC] * 4;
}

/* Stream s starts at 0x701 + 4 s (2 words) and ends at its limit 0x7C0 + 2 s (2 words, inclusive) */
void mx_draw_vertex_buffers(const u32* r, DsVertexBuffers* b) {
    for (u32 s = 0; s < DS_STREAMS; s++) {
        u64 start = ((u64)r[0x701 + s * 4] << 32) | r[0x702 + s * 4],
            end = ((u64)r[0x7C0 + s * 2] << 32) | r[0x7C1 + s * 2];
        b->addr[s] = start;
        b->size[s] = end >= start ? end - start + 1 : 0;
    }
}

/* The index buffer at 0x5F2/0x5F3, its last byte at 0x5F4/0x5F5, its format at 0x5F6; primitive restart 0x591, its
   index 0x592 */
void mx_draw_index(const u32* r, DsIndex* x) {
    memset(x, 0, sizeof *x);
    x->addr = ((u64)r[0x5F2] << 32) | r[0x5F3];
    x->end = ((u64)r[0x5F4] << 32) | r[0x5F5];
    x->format = r[0x5F6];
    x->restart = r[0x591] != 0;
    x->restart_index = r[0x592];
}

/* CLEAR_SURFACE's word v: bit 0 depth, 1 stencil, 2-5 the RGBA mask, 6-9 the color slot, 10-20 the layer. The
   color at 0x360-0x363, depth 0x364, stencil 0x368 */
void mx_draw_clear(const u32* r, u32 v, DrawClear* c) {
    memset(c, 0, sizeof *c);
    c->aspects = v & 3;
    c->color_mask = (v >> 2) & 15;
    c->slot = (v >> 6) & 15;
    c->layer = (v >> 10) & 0x7ff;
    memcpy(c->color, &r[0x360], 16);
    memcpy(&c->depth, &r[0x364], 4);
    c->stencil = r[0x368];
    c->scissored = r3d_clear_scissored(r) != 0;
}

/* DRAW_TEXTURE's registers 0x420-0x42B: the destination's x, y, width and height in 20.12 fixed point, du and dv in
   32.32 (low word first), the texture and sampler indices, and the source's x and y in 20.12 */
void mx_draw_texture(const u32* r, DrawTexture* t) {
    memset(t, 0, sizeof *t);
    t->handle = (r[0x428] & 0xFFF) << 20 | (r[0x429] & 0xFFFFF);
    t->x0 = (float)(i32)r[0x420] / 4096.f, t->y0 = (float)(i32)r[0x421] / 4096.f;
    t->w = (float)(i32)r[0x422] / 4096.f, t->h = (float)(i32)r[0x423] / 4096.f;
    t->du = (double)(i64)((u64)r[0x425] << 32 | r[0x424]) / 4294967296.0;
    t->dv = (double)(i64)((u64)r[0x427] << 32 | r[0x426]) / 4294967296.0;
    t->sx0 = (float)(i32)r[0x42A] / 4096.f, t->sy0 = (float)(i32)r[0x42B] / 4096.f;
}

/* Brings ch's draw state up to its registers: the groups whose registers a method changed since (GpuChannel.r3d_dirty,
   which the renderer still takes for what it keeps between draws), and those no register group covers yet, every
   time */
void mx_draw_state_sync(GpuChannel* ch) {
    const u32* r = ch->r3d;
    u32 dirty = ch->r3d_dirty;
    DrawState* ds = &ch->ds;
    if (dirty & (R3D_SHADER | R3D_PIPEMISC | R3D_XFB)) mx_draw_programs(r, &ds->prog);
    if (dirty & (R3D_VATTR | R3D_VFORMAT)) mx_draw_vertex_format(r, &ds->vfmt);
    if (dirty & R3D_BLEND) mx_draw_blend(r, &ds->blend);
    if (dirty & R3D_PIPEMISC) mx_draw_raster_key(r, &ds->rkey);
    if (dirty & R3D_XFB) mx_draw_xfb(r, &ds->xfb);
    mx_draw_targets(r, &ds->tgt);
    mx_draw_dynamic(r, &ds->dyn);
    mx_draw_viewports(r, &ds->vp);
    mx_draw_vertex_buffers(r, &ds->vbuf);
    mx_draw_index(r, &ds->index);
}

u32 mx_draw_state_stale(const GpuChannel* ch) {
    const u32* r = ch->r3d;
    const DrawState* ds = &ch->ds;
    DrawState f; /* every group filled again */
    u32 stale = 0;
    mx_draw_programs(r, &f.prog);
    mx_draw_vertex_format(r, &f.vfmt);
    mx_draw_blend(r, &f.blend);
    mx_draw_raster_key(r, &f.rkey);
    mx_draw_xfb(r, &f.xfb);
    mx_draw_targets(r, &f.tgt);
    mx_draw_dynamic(r, &f.dyn);
    mx_draw_viewports(r, &f.vp);
    mx_draw_vertex_buffers(r, &f.vbuf);
    mx_draw_index(r, &f.index);
    stale |= (u32)(memcmp(&f.prog, &ds->prog, sizeof f.prog) != 0) << DS_PROGRAMS;
    stale |= (u32)(memcmp(&f.vfmt, &ds->vfmt, sizeof f.vfmt) != 0) << DS_VFORMAT;
    stale |= (u32)(memcmp(&f.blend, &ds->blend, sizeof f.blend) != 0) << DS_BLEND;
    stale |= (u32)(memcmp(&f.rkey, &ds->rkey, sizeof f.rkey) != 0) << DS_RASTER_KEY;
    stale |= (u32)(memcmp(&f.xfb, &ds->xfb, sizeof f.xfb) != 0) << DS_XFB;
    stale |= (u32)(memcmp(&f.tgt, &ds->tgt, sizeof f.tgt) != 0) << DS_TARGETS;
    stale |= (u32)(memcmp(&f.dyn, &ds->dyn, sizeof f.dyn) != 0) << DS_DYNAMIC;
    stale |= (u32)(memcmp(&f.vp, &ds->vp, sizeof f.vp) != 0) << DS_VIEWPORTS;
    stale |= (u32)(memcmp(&f.vbuf, &ds->vbuf, sizeof f.vbuf) != 0) << DS_VBUF;
    stale |= (u32)(memcmp(&f.index, &ds->index, sizeof f.index) != 0) << DS_INDEX;
    return stale;
}
