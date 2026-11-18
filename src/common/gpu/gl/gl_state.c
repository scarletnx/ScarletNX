/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's state machine (gl_state.h): the contexts and their fixed state, in one block of flat records */
#include "common/gpu/gl/gl_state.h"
#include <stdio.h>
#include <string.h>

enum {
    GL_FRONT_ = 0x0404,
    GL_BACK_ = 0x0405,
    GL_FRONT_AND_BACK_ = 0x0408,
    GL_CCW_ = 0x0901,
    GL_LESS_ = 0x0201,
    GL_ALWAYS_ = 0x0207,
    GL_KEEP_ = 0x1e00,
    GL_ONE_ = 1,
    GL_FUNC_ADD_ = 0x8006,
    GL_DONT_CARE_ = 0x1100,
};

/* the layer's state: what a save state holds */
typedef struct {
    SnxGlContext contexts[SNX_GL_CONTEXTS];
} SnxGl;
static SnxGl g_gl;

/* the header of a save state's copy */
typedef struct {
    uint32_t magic, version, bytes, pad;
} SnxGlStateHeader;

static SnxGlFixed* fixed(uint32_t ctx) { return &g_gl.contexts[ctx - 1].fixed; }

uint32_t snx_gl_context_new(uint32_t version) {
    for (uint32_t i = 0; i < SNX_GL_CONTEXTS; i++) {
        SnxGlContext* c = &g_gl.contexts[i];
        SnxGlFixed* f = &c->fixed;
        if (c->used) continue;
        *c = (SnxGlContext){.used = 1, .version = version};
        f->blend_src[0] = f->blend_src[1] = GL_ONE_;
        f->blend_eq[0] = f->blend_eq[1] = GL_FUNC_ADD_;
        for (int k = 0; k < 4; k++) f->color_mask[k] = 1;
        f->depth_mask = 1;
        f->depth_func = GL_LESS_;
        f->depth_range[1] = 1;
        f->cull_face = GL_BACK_;
        f->front_face = GL_CCW_;
        for (int s = 0; s < 2; s++) {
            f->stencil[s].func = GL_ALWAYS_;
            f->stencil[s].fail = f->stencil[s].zfail = f->stencil[s].zpass = GL_KEEP_;
            f->stencil[s].mask = f->stencil[s].write = 0xffffffffu;
        }
        f->clear_depth = 1;
        f->sample_mask = 0xffffffffu;
        f->line_width = 1;
        f->coverage_value = 1;
        f->dither = 1;
        f->hints[0] = f->hints[1] = GL_DONT_CARE_;
        f->pixel[0].alignment = f->pixel[1].alignment = 4;
        return i + 1;
    }
    return 0;
}

void snx_gl_context_free(uint32_t ctx) {
    if (ctx && ctx <= SNX_GL_CONTEXTS) g_gl.contexts[ctx - 1] = (SnxGlContext){0};
}

SnxGlFixed* snx_gl_fixed(uint32_t ctx) { return fixed(ctx); }

void snx_gl_error(uint32_t ctx, uint32_t e) {
    if (!fixed(ctx)->error) fixed(ctx)->error = e;
}

uint32_t snx_gl_get_error(uint32_t ctx) {
    uint32_t e = fixed(ctx)->error;
    fixed(ctx)->error = SNX_GL_NO_ERROR;
    return e;
}

/* the enable of capability cap, NULL for one GL has not */
static uint8_t* capability(SnxGlFixed* f, uint32_t cap) {
    switch (cap) {
        case 0x0be2: return &f->blend;
        case 0x0b44: return &f->cull;
        case 0x0b71: return &f->depth_test;
        case 0x0b90: return &f->stencil_test;
        case 0x0c11: return &f->scissor_test;
        case 0x8037: return &f->polygon_offset;
        case 0x0bd0: return &f->dither;
        case 0x809e: return &f->alpha_to_coverage;
        case 0x80a0: return &f->sample_coverage;
        case 0x8c89: return &f->rasterizer_discard;
        case 0x8d69: return &f->primitive_restart;
        case 0x8e51: return &f->sample_mask_on; /* GL_SAMPLE_MASK */
        default: return NULL;
    }
}
void snx_gl_enable(uint32_t ctx, uint32_t cap, int on) {
    uint8_t* e = capability(fixed(ctx), cap);
    if (e)
        *e = on != 0;
    else
        snx_gl_error(ctx, SNX_GL_INVALID_ENUM);
}
int snx_gl_capability(uint32_t ctx, uint32_t cap) {
    const uint8_t* e = capability(fixed(ctx), cap);
    return e ? *e : -1;
}
int snx_gl_is_enabled(uint32_t ctx, uint32_t cap) {
    const uint8_t* e = capability(fixed(ctx), cap);
    if (!e) snx_gl_error(ctx, SNX_GL_INVALID_ENUM);
    return e && *e;
}

void snx_gl_blend_func_separate(uint32_t ctx, uint32_t src_rgb, uint32_t dst_rgb, uint32_t src_alpha,
                                uint32_t dst_alpha) {
    SnxGlFixed* f = fixed(ctx);
    f->blend_src[0] = src_rgb;
    f->blend_dst[0] = dst_rgb;
    f->blend_src[1] = src_alpha;
    f->blend_dst[1] = dst_alpha;
}
void snx_gl_blend_equation_separate(uint32_t ctx, uint32_t rgb, uint32_t alpha) {
    fixed(ctx)->blend_eq[0] = rgb;
    fixed(ctx)->blend_eq[1] = alpha;
}
void snx_gl_blend_color(uint32_t ctx, float r, float g, float b, float a) {
    const float v[4] = {r, g, b, a};
    memcpy(fixed(ctx)->blend_color, v, sizeof v);
}
void snx_gl_color_mask(uint32_t ctx, int r, int g, int b, int a) {
    const int v[4] = {r, g, b, a};
    for (int i = 0; i < 4; i++) fixed(ctx)->color_mask[i] = v[i] != 0;
}
void snx_gl_depth_mask(uint32_t ctx, int on) { fixed(ctx)->depth_mask = on != 0; }
void snx_gl_depth_func(uint32_t ctx, uint32_t func) { fixed(ctx)->depth_func = func; }
/* GL clamps a depth value to [0, 1] */
static float unit(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
void snx_gl_depth_range(uint32_t ctx, float near_value, float far_value) {
    fixed(ctx)->depth_range[0] = unit(near_value);
    fixed(ctx)->depth_range[1] = unit(far_value);
}
void snx_gl_cull_face(uint32_t ctx, uint32_t face) { fixed(ctx)->cull_face = face; }
void snx_gl_front_face(uint32_t ctx, uint32_t mode) { fixed(ctx)->front_face = mode; }

/* the faces a stencil call sets: bit 0 the front's, bit 1 the back's; 0 for a face GL has not */
static unsigned faces(uint32_t face) {
    return face == GL_FRONT_ ? 1 : face == GL_BACK_ ? 2 : face == GL_FRONT_AND_BACK_ ? 3 : 0;
}
void snx_gl_stencil_func_separate(uint32_t ctx, uint32_t face, uint32_t func, int32_t ref, uint32_t mask) {
    unsigned which = faces(face);
    if (!which) snx_gl_error(ctx, SNX_GL_INVALID_ENUM);
    for (int s = 0; s < 2; s++)
        if (which & (1u << s)) {
            fixed(ctx)->stencil[s].func = func;
            fixed(ctx)->stencil[s].ref = ref;
            fixed(ctx)->stencil[s].mask = mask;
        }
}
void snx_gl_stencil_op_separate(uint32_t ctx, uint32_t face, uint32_t fail, uint32_t zfail, uint32_t zpass) {
    unsigned which = faces(face);
    if (!which) snx_gl_error(ctx, SNX_GL_INVALID_ENUM);
    for (int s = 0; s < 2; s++)
        if (which & (1u << s)) {
            fixed(ctx)->stencil[s].fail = fail;
            fixed(ctx)->stencil[s].zfail = zfail;
            fixed(ctx)->stencil[s].zpass = zpass;
        }
}
void snx_gl_stencil_mask_separate(uint32_t ctx, uint32_t face, uint32_t mask) {
    unsigned which = faces(face);
    if (!which) snx_gl_error(ctx, SNX_GL_INVALID_ENUM);
    for (int s = 0; s < 2; s++)
        if (which & (1u << s)) fixed(ctx)->stencil[s].write = mask;
}

void snx_gl_clear_color(uint32_t ctx, float r, float g, float b, float a) {
    const float v[4] = {r, g, b, a};
    memcpy(fixed(ctx)->clear_color, v, sizeof v);
}
void snx_gl_clear_depth(uint32_t ctx, float depth) { fixed(ctx)->clear_depth = unit(depth); }
void snx_gl_clear_stencil(uint32_t ctx, int32_t s) { fixed(ctx)->clear_stencil = s; }

/* a viewport's or scissor's rectangle: GL refuses a negative size and keeps the one before */
static void rectangle(uint32_t ctx, int32_t out[4], int32_t x, int32_t y, int32_t width, int32_t height) {
    if (width < 0 || height < 0) {
        snx_gl_error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    out[0] = x, out[1] = y, out[2] = width, out[3] = height;
}
void snx_gl_viewport(uint32_t ctx, int32_t x, int32_t y, int32_t width, int32_t height) {
    rectangle(ctx, fixed(ctx)->viewport, x, y, width, height);
}
void snx_gl_scissor(uint32_t ctx, int32_t x, int32_t y, int32_t width, int32_t height) {
    rectangle(ctx, fixed(ctx)->scissor, x, y, width, height);
}
void snx_gl_line_width(uint32_t ctx, float width) {
    if (!(width > 0))
        snx_gl_error(ctx, SNX_GL_INVALID_VALUE);
    else
        fixed(ctx)->line_width = width;
}
void snx_gl_polygon_offset(uint32_t ctx, float factor, float units) {
    fixed(ctx)->offset_factor = factor;
    fixed(ctx)->offset_units = units;
}
void snx_gl_sample_coverage(uint32_t ctx, float value, int invert) {
    fixed(ctx)->coverage_value = unit(value);
    fixed(ctx)->coverage_invert = invert != 0;
}
void snx_gl_hint(uint32_t ctx, uint32_t target, uint32_t mode) {
    if (target == 0x8192) /* GL_GENERATE_MIPMAP_HINT */
        fixed(ctx)->hints[0] = mode;
    else if (target == 0x8b8b) /* GL_FRAGMENT_SHADER_DERIVATIVE_HINT */
        fixed(ctx)->hints[1] = mode;
}
void snx_gl_pixel_store(uint32_t ctx, uint32_t pname, int32_t value) {
    SnxGlFixed* f = fixed(ctx);
    int pack = pname >= 0x0d00 && pname < 0x0d10 ? 0 : 1;
    switch (pname) {
        case 0x0d05: /* GL_PACK_ALIGNMENT */
        case 0x0cf5: /* GL_UNPACK_ALIGNMENT */
            if (value != 1 && value != 2 && value != 4 && value != 8) {
                snx_gl_error(ctx, SNX_GL_INVALID_VALUE);
                return;
            }
            f->pixel[pack].alignment = value;
            return;
        case 0x0d02:
        case 0x0cf2: f->pixel[pack].row_length = value; return;
        case 0x0d03:
        case 0x0cf3: f->pixel[pack].skip_rows = value; return;
        case 0x0d04:
        case 0x0cf4: f->pixel[pack].skip_pixels = value; return;
        case 0x806e: f->pixel[1].image_height = value; return; /* GL_UNPACK_IMAGE_HEIGHT */
        case 0x806d: f->pixel[1].skip_images = value; return;  /* GL_UNPACK_SKIP_IMAGES */
        default: snx_gl_error(ctx, SNX_GL_INVALID_ENUM);
    }
}

size_t snx_gl_state_bytes(void) { return sizeof(SnxGlStateHeader) + sizeof g_gl; }

void snx_gl_state_save(void* out) {
    const SnxGlStateHeader h = {SNX_GL_STATE_MAGIC, SNX_GL_STATE_VERSION, (uint32_t)sizeof g_gl, 0};
    memcpy(out, &h, sizeof h);
    memcpy((unsigned char*)out + sizeof h, &g_gl, sizeof g_gl);
}

/* an error of a context's record, by what GL can leave there */
static int known_error(uint32_t e) {
    return e == SNX_GL_NO_ERROR || (e >= SNX_GL_INVALID_ENUM && e <= 0x0507u); /* through GL_CONTEXT_LOST */
}
int snx_gl_state_check(const void* in, size_t size, char* why, size_t whysize) {
    SnxGlStateHeader h;
    if (size < sizeof h) {
        snprintf(why, whysize, "the GL layer's state is %zu bytes, less than its %zu-byte header", size, sizeof h);
        return -1;
    }
    memcpy(&h, in, sizeof h);
    if (h.magic != SNX_GL_STATE_MAGIC) {
        snprintf(why, whysize, "the GL layer's state starts with %08x, not %08x", (unsigned)h.magic,
                 (unsigned)SNX_GL_STATE_MAGIC);
        return -1;
    }
    if (h.version != SNX_GL_STATE_VERSION || h.bytes != sizeof g_gl || size != sizeof h + sizeof g_gl) {
        snprintf(why, whysize,
                 "the GL layer's state is layout %u of %u bytes (%zu in all); this build reads layout %u of %zu",
                 (unsigned)h.version, (unsigned)h.bytes, size, (unsigned)SNX_GL_STATE_VERSION, sizeof g_gl);
        return -1;
    }
    for (uint32_t i = 0; i < SNX_GL_CONTEXTS; i++) {
        SnxGlContext c;
        memcpy(&c, (const unsigned char*)in + sizeof h + i * sizeof c, sizeof c);
        if (c.used > 1) {
            snprintf(why, whysize, "the GL layer's context %u has used=%u, not 0 or 1", (unsigned)(i + 1),
                     (unsigned)c.used);
            return -1;
        }
        if (!c.used) continue;
        if (!known_error(c.fixed.error)) {
            snprintf(why, whysize, "the GL layer's context %u holds error %04x, which GL has not", (unsigned)(i + 1),
                     (unsigned)c.fixed.error);
            return -1;
        }
        for (int p = 0; p < 2; p++) {
            int32_t a = c.fixed.pixel[p].alignment;
            if (a != 1 && a != 2 && a != 4 && a != 8) {
                snprintf(why, whysize, "the GL layer's context %u has %s alignment %d, not 1, 2, 4 or 8",
                         (unsigned)(i + 1), p ? "unpack" : "pack", (int)a);
                return -1;
            }
        }
    }
    return 0;
}

void snx_gl_state_load(const void* in) {
    memcpy(&g_gl, (const unsigned char*)in + sizeof(SnxGlStateHeader), sizeof g_gl);
}
