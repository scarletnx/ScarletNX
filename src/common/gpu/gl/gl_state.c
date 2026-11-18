/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's state machine (gl_state.h): the contexts and their fixed state, in one block of flat records */
#include "common/gpu/gl/gl_state.h"
#include "common/gpu/gl/gl_object.h"
#include "common/gpu/gl/gl_program.h"
#include "common/gpu/gl/gl_framebuffer.h"
#include "common/gpu/gl/gl_sync.h"
#include "common/gpu/gl/gl_texture.h"
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
    GL_POINT_ = 0x1b00,
    GL_FILL_ = 0x1b02,
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

uint32_t snx_gl_context_new(uint32_t api, uint32_t version, uint32_t debug) {
    for (uint32_t i = 0; i < SNX_GL_CONTEXTS; i++) {
        SnxGlContext* c = &g_gl.contexts[i];
        SnxGlFixed* f = &c->fixed;
        if (c->used) continue;
        *c = (SnxGlContext){.used = 1, .api = api, .version = version, .debug = debug != 0};
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
        f->point_size = 1;
        f->polygon_mode = GL_FILL_;
        f->coverage_value = 1;
        f->dither = 1;
        f->hints[0] = f->hints[1] = GL_DONT_CARE_;
        f->pixel[0].alignment = f->pixel[1].alignment = 4;
        snx_gl_object_context_reset(i + 1);
        snx_gl_texture_context_reset(i + 1);
        snx_gl_framebuffer_context_reset(i + 1);
        return i + 1;
    }
    return 0;
}

void snx_gl_context_free(uint32_t ctx) {
    if (ctx && ctx <= SNX_GL_CONTEXTS) g_gl.contexts[ctx - 1] = (SnxGlContext){0};
}

SnxGlFixed* snx_gl_fixed(uint32_t ctx) { return fixed(ctx); }
SnxGlContext* snx_gl_context(uint32_t ctx) { return &g_gl.contexts[ctx - 1]; }
uint32_t snx_gl_context_api(uint32_t ctx) {
    return ctx && ctx <= SNX_GL_CONTEXTS && g_gl.contexts[ctx - 1].used ? g_gl.contexts[ctx - 1].api : 0;
}
uint32_t snx_gl_context_version(uint32_t ctx) {
    return ctx && ctx <= SNX_GL_CONTEXTS && g_gl.contexts[ctx - 1].used ? g_gl.contexts[ctx - 1].version : 0;
}

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
void snx_gl_point_size(uint32_t ctx, float size) {
    if (!(size > 0))
        snx_gl_error(ctx, SNX_GL_INVALID_VALUE);
    else
        fixed(ctx)->point_size = size;
}
void snx_gl_polygon_mode(uint32_t ctx, uint32_t face, uint32_t mode) {
    if (face != GL_FRONT_AND_BACK_ || mode < GL_POINT_ || mode > GL_FILL_)
        snx_gl_error(ctx, SNX_GL_INVALID_ENUM);
    else
        fixed(ctx)->polygon_mode = mode;
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

/* the limits a context answers: a desktop GPU's, as the games' own driver answers on the console's */
enum {
    MAX_TEXTURE_SIZE = 16384,
    MAX_3D_TEXTURE_SIZE = 2048,
    MAX_ARRAY_TEXTURE_LAYERS = 2048, /* desktop GL 4.5's least */
    MAX_SAMPLES = 4,                 /* the counts the layer makes: 1, 2 and 4 */
    MAX_POINT_SIZE = 64,
    MAX_TEXTURE_LOD_BIAS = 15,
    MAX_VERTEX_ATTRIBS = 16,
    MAX_TEXTURE_UNITS = 32,
    MAX_COMBINED_TEXTURE_UNITS = 160,
    MAX_DRAW_BUFFERS = 8,
    MAX_UNIFORM_BLOCK_SIZE = 65536,
};

int snx_gl_get(uint32_t ctx, uint32_t pname, double v[4]) {
    const SnxGlFixed* f = fixed(ctx);
    const SnxGlContext* c = &g_gl.contexts[ctx - 1];
    int on = snx_gl_capability(ctx, pname), i;
    if (on >= 0) {
        v[0] = on;
        return 1;
    }
    if (snx_gl_object_get(ctx, pname, v) || snx_gl_texture_get(ctx, pname, v) || snx_gl_framebuffer_get(ctx, pname, v))
        return 1;
    switch (pname) {
        case 0x0ba2: /* GL_VIEWPORT */
            for (i = 0; i < 4; i++) v[i] = f->viewport[i];
            return 4;
        case 0x0c10: /* GL_SCISSOR_BOX */
            for (i = 0; i < 4; i++) v[i] = f->scissor[i];
            return 4;
        case 0x8b8d: v[0] = c->program; return 1;  /* GL_CURRENT_PROGRAM */
        case 0x825a: v[0] = c->pipeline; return 1; /* GL_PROGRAM_PIPELINE_BINDING */
        case 0x0c22: /* GL_COLOR_CLEAR_VALUE */
            for (i = 0; i < 4; i++) v[i] = f->clear_color[i];
            return 4;
        case 0x0c23: /* GL_COLOR_WRITEMASK */
            for (i = 0; i < 4; i++) v[i] = f->color_mask[i];
            return 4;
        case 0x0b70: /* GL_DEPTH_RANGE */ v[0] = f->depth_range[0], v[1] = f->depth_range[1]; return 2;
        case 0x0b73: v[0] = f->clear_depth; return 1;        /* GL_DEPTH_CLEAR_VALUE */
        case 0x0b91: v[0] = f->clear_stencil; return 1;      /* GL_STENCIL_CLEAR_VALUE */
        case 0x0b74: v[0] = f->depth_func; return 1;         /* GL_DEPTH_FUNC */
        case 0x0b72: v[0] = f->depth_mask; return 1;         /* GL_DEPTH_WRITEMASK */
        case 0x0b45: v[0] = f->cull_face; return 1;          /* GL_CULL_FACE_MODE */
        case 0x0b46: v[0] = f->front_face; return 1;         /* GL_FRONT_FACE */
        case 0x0b21: v[0] = f->line_width; return 1;         /* GL_LINE_WIDTH */
        case 0x0b11: v[0] = f->point_size; return 1;         /* GL_POINT_SIZE */
        case 0x0b40: v[0] = v[1] = f->polygon_mode; return 2; /* GL_POLYGON_MODE: front and back */
        case 0x80c9: v[0] = f->blend_src[0]; return 1;       /* GL_BLEND_SRC_RGB */
        case 0x80c8: v[0] = f->blend_dst[0]; return 1;       /* GL_BLEND_DST_RGB */
        case 0x80cb: v[0] = f->blend_src[1]; return 1;       /* GL_BLEND_SRC_ALPHA */
        case 0x80ca: v[0] = f->blend_dst[1]; return 1;       /* GL_BLEND_DST_ALPHA */
        case 0x8009: v[0] = f->blend_eq[0]; return 1;        /* GL_BLEND_EQUATION_RGB */
        case 0x883d: v[0] = f->blend_eq[1]; return 1;        /* GL_BLEND_EQUATION_ALPHA */
        case 0x0cf5: v[0] = f->pixel[1].alignment; return 1; /* GL_UNPACK_ALIGNMENT */
        case 0x0d05: v[0] = f->pixel[0].alignment; return 1; /* GL_PACK_ALIGNMENT */
        case 0x821b: v[0] = c->version / 10; return 1;       /* GL_MAJOR_VERSION */
        case 0x821c: v[0] = c->version % 10; return 1;       /* GL_MINOR_VERSION */
        case 0x821d: v[0] = 0; return 1;                     /* GL_NUM_EXTENSIONS: none yet */
        case 0x821e: v[0] = c->debug ? 2 : 0; return 1;      /* GL_CONTEXT_FLAGS: GL_CONTEXT_FLAG_DEBUG_BIT */
        case 0x9126:                                         /* GL_CONTEXT_PROFILE_MASK: the core profile */
            if (c->api != SNX_GL_API_DESKTOP) return 0;
            v[0] = 1;
            return 1;
        case 0x0d33: /* GL_MAX_TEXTURE_SIZE */
        case 0x851c: /* GL_MAX_CUBE_MAP_TEXTURE_SIZE */
        case 0x84e8: /* GL_MAX_RENDERBUFFER_SIZE */ v[0] = MAX_TEXTURE_SIZE; return 1;
        case 0x8073: v[0] = MAX_3D_TEXTURE_SIZE; return 1;      /* GL_MAX_3D_TEXTURE_SIZE */
        case 0x88ff: v[0] = MAX_ARRAY_TEXTURE_LAYERS; return 1; /* GL_MAX_ARRAY_TEXTURE_LAYERS */
        case 0x8d57: v[0] = MAX_SAMPLES; return 1;              /* GL_MAX_SAMPLES */
        case 0x80a8:                                            /* GL_SAMPLE_BUFFERS */
        case 0x80a9: v[0] = 0; return 1;                        /* GL_SAMPLES: the window's surface has one */
        case 0x0b12:                                            /* GL_POINT_SIZE_RANGE */
        case 0x846d: /* GL_ALIASED_POINT_SIZE_RANGE */ v[0] = 1, v[1] = MAX_POINT_SIZE; return 2;
        case 0x84fd: v[0] = MAX_TEXTURE_LOD_BIAS; return 1; /* GL_MAX_TEXTURE_LOD_BIAS */
        case 0x91b0: v[0] = 0xffffffffu; return 1;          /* GL_MAX_SHADER_COMPILER_THREADS: as many as it likes */
        case 0x0d3a: /* GL_MAX_VIEWPORT_DIMS */ v[0] = v[1] = MAX_TEXTURE_SIZE; return 2;
        case 0x8869: v[0] = MAX_VERTEX_ATTRIBS; return 1;         /* GL_MAX_VERTEX_ATTRIBS */
        case 0x8872: v[0] = MAX_TEXTURE_UNITS; return 1;          /* GL_MAX_TEXTURE_IMAGE_UNITS */
        case 0x8b4c: v[0] = MAX_TEXTURE_UNITS; return 1;          /* GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS */
        case 0x8b4d: v[0] = MAX_COMBINED_TEXTURE_UNITS; return 1; /* GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */
        case 0x8824:                                              /* GL_MAX_DRAW_BUFFERS */
        case 0x8cdf: /* GL_MAX_COLOR_ATTACHMENTS */ v[0] = MAX_DRAW_BUFFERS; return 1;
        case 0x8a30: v[0] = MAX_UNIFORM_BLOCK_SIZE; return 1; /* GL_MAX_UNIFORM_BLOCK_SIZE */
        default: return 0;
    }
}

/* GL_VERSION and GL_SHADING_LANGUAGE_VERSION of a context, by its API and version; NULL for one the layer makes no
   context of */
static const char* version_string(uint32_t api, uint32_t version, int glsl) {
    static const struct {
        uint32_t api, version;
        const char *gl, *glsl;
    } versions[] = {
        {SNX_GL_API_DESKTOP, 30, "3.0.0 ScarletNX", "1.30 ScarletNX"},
        {SNX_GL_API_DESKTOP, 31, "3.1.0 ScarletNX", "1.40 ScarletNX"},
        {SNX_GL_API_DESKTOP, 32, "3.2.0 ScarletNX", "1.50 ScarletNX"},
        {SNX_GL_API_DESKTOP, 33, "3.3.0 ScarletNX", "3.30 ScarletNX"},
        {SNX_GL_API_DESKTOP, 40, "4.0.0 ScarletNX", "4.00 ScarletNX"},
        {SNX_GL_API_DESKTOP, 41, "4.1.0 ScarletNX", "4.10 ScarletNX"},
        {SNX_GL_API_DESKTOP, 42, "4.2.0 ScarletNX", "4.20 ScarletNX"},
        {SNX_GL_API_DESKTOP, 43, "4.3.0 ScarletNX", "4.30 ScarletNX"},
        {SNX_GL_API_DESKTOP, 44, "4.4.0 ScarletNX", "4.40 ScarletNX"},
        {SNX_GL_API_DESKTOP, 45, "4.5.0 ScarletNX", "4.50 ScarletNX"},
        {SNX_GL_API_ES, 20, "OpenGL ES 2.0 ScarletNX", "OpenGL ES GLSL ES 1.00 ScarletNX"},
        {SNX_GL_API_ES, 30, "OpenGL ES 3.0 ScarletNX", "OpenGL ES GLSL ES 3.00 ScarletNX"},
        {SNX_GL_API_ES, 31, "OpenGL ES 3.1 ScarletNX", "OpenGL ES GLSL ES 3.10 ScarletNX"},
        {SNX_GL_API_ES, 32, "OpenGL ES 3.2 ScarletNX", "OpenGL ES GLSL ES 3.20 ScarletNX"},
    };
    for (size_t i = 0; i < sizeof versions / sizeof *versions; i++)
        if (versions[i].api == api && versions[i].version == version) return glsl ? versions[i].glsl : versions[i].gl;
    return NULL;
}
int snx_gl_version_made(uint32_t api, uint32_t version) { return version_string(api, version, 0) != NULL; }

const char* snx_gl_string(uint32_t ctx, uint32_t name, int stringi, uint32_t index) {
    const SnxGlContext* c = &g_gl.contexts[ctx - 1];
    int desktop = c->api == SNX_GL_API_DESKTOP;
    if (stringi) {
        /* no extension yet: GL_NUM_EXTENSIONS is 0, so every index is past the last */
        snx_gl_error(ctx, name == 0x1f03 ? SNX_GL_INVALID_VALUE : SNX_GL_INVALID_ENUM);
        (void)index;
        return NULL;
    }
    switch (name) {
        case 0x1f00: return "NVIDIA Corporation"; /* GL_VENDOR: the GPU's maker, as the games' own driver says */
        case 0x1f01: return "NVIDIA Tegra X1";    /* GL_RENDERER */
        case 0x1f02: return version_string(c->api, c->version, 0); /* GL_VERSION */
        case 0x8b8c: return version_string(c->api, c->version, 1); /* GL_SHADING_LANGUAGE_VERSION */
        case 0x1f03:                                               /* GL_EXTENSIONS */
            if (!desktop) return "";
            break; /* the core profile has it only through glGetStringi */
        default: break;
    }
    snx_gl_error(ctx, SNX_GL_INVALID_ENUM);
    return NULL;
}

uint32_t snx_gl_clear(uint32_t ctx, uint32_t mask) {
    const uint32_t buffers = 0x4000u | 0x0100u | 0x0400u; /* color, depth, stencil */
    if (mask & ~buffers) {
        snx_gl_error(ctx, SNX_GL_INVALID_VALUE);
        return 0;
    }
    return mask;
}

size_t snx_gl_state_bytes(void) {
    return sizeof(SnxGlStateHeader) + sizeof g_gl + snx_gl_program_state_bytes() + snx_gl_object_state_bytes() +
           snx_gl_texture_state_bytes() + snx_gl_framebuffer_state_bytes() + snx_gl_sync_state_bytes();
}

void snx_gl_state_save(void* out) {
    const SnxGlStateHeader h = {SNX_GL_STATE_MAGIC, SNX_GL_STATE_VERSION, (uint32_t)sizeof g_gl, 0};
    memcpy(out, &h, sizeof h);
    memcpy((unsigned char*)out + sizeof h, &g_gl, sizeof g_gl);
    snx_gl_program_state_save((unsigned char*)out + sizeof h + sizeof g_gl);
    snx_gl_object_state_save((unsigned char*)out + sizeof h + sizeof g_gl + snx_gl_program_state_bytes());
    snx_gl_texture_state_save((unsigned char*)out + sizeof h + sizeof g_gl + snx_gl_program_state_bytes() +
                              snx_gl_object_state_bytes());
    snx_gl_framebuffer_state_save((unsigned char*)out + sizeof h + sizeof g_gl + snx_gl_program_state_bytes() +
                                  snx_gl_object_state_bytes() + snx_gl_texture_state_bytes());
    snx_gl_sync_state_save((unsigned char*)out + sizeof h + sizeof g_gl + snx_gl_program_state_bytes() +
                           snx_gl_object_state_bytes() + snx_gl_texture_state_bytes() +
                           snx_gl_framebuffer_state_bytes());
}
/* the bytes of the programs' part at programs, of the left bytes there: 0 when its header says more than that */
static size_t program_part(const unsigned char* programs, size_t left) {
    uint32_t h[2];
    if (left < sizeof h) return 0;
    memcpy(h, programs, sizeof h);
    return (uint64_t)sizeof h + h[0] + h[1] <= left ? sizeof h + (size_t)h[0] + h[1] : 0;
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
    if (h.version != SNX_GL_STATE_VERSION || h.bytes != sizeof g_gl || size < sizeof h + sizeof g_gl) {
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
    {
        const unsigned char* programs = (const unsigned char*)in + sizeof h + sizeof g_gl;
        size_t left = size - sizeof h - sizeof g_gl, part = program_part(programs, left);
        if (!part) {
            snprintf(why, whysize,
                     "the GL layer's programs say more bytes than the %zu its state has after the contexts", left);
            return -1;
        }
        size_t objects = snx_gl_object_state_bytes(), textures = snx_gl_texture_state_bytes(),
               framebuffers = snx_gl_framebuffer_state_bytes();
        if (snx_gl_program_state_check(programs, part, why, whysize)) return -1;
        if (left - part < objects + textures + framebuffers) {
            snprintf(why, whysize,
                     "the GL layer's state has %zu bytes after its programs, less than its buffers', textures' and "
                     "framebuffers' %zu",
                     left - part, objects + textures + framebuffers);
            return -1;
        }
        if (snx_gl_object_state_check(programs + part, objects, why, whysize) ||
            snx_gl_texture_state_check(programs + part + objects, textures, why, whysize) ||
            snx_gl_framebuffer_state_check(programs + part + objects + textures, framebuffers, why, whysize) ||
            snx_gl_sync_state_check(programs + part + objects + textures + framebuffers,
                                    left - part - objects - textures - framebuffers, why, whysize))
            return -1;
    }
    for (uint32_t i = 0; i < SNX_GL_CONTEXTS; i++) {
        SnxGlContext c;
        memcpy(&c, (const unsigned char*)in + sizeof h + i * sizeof c, sizeof c);
        if (c.used &&
            !snx_gl_program_state_bound((const unsigned char*)in + sizeof h + sizeof g_gl, c.program, c.pipeline)) {
            snprintf(why, whysize,
                     "the GL layer's context %u has program %u and pipeline %u bound, and one of them is not made",
                     (unsigned)(i + 1), (unsigned)c.program, (unsigned)c.pipeline);
            return -1;
        }
    }
    return 0;
}

int snx_gl_state_load(const void* in, size_t size) {
    const unsigned char* i = in;
    size_t at = sizeof(SnxGlStateHeader) + sizeof g_gl, part = program_part(i + at, size - at);
    if (snx_gl_program_state_load(i + at, part)) return -1;
    snx_gl_object_state_load(i + at + part);
    snx_gl_texture_state_load(i + at + part + snx_gl_object_state_bytes());
    snx_gl_framebuffer_state_load(i + at + part + snx_gl_object_state_bytes() + snx_gl_texture_state_bytes());
    snx_gl_sync_state_load(i + at + part + snx_gl_object_state_bytes() + snx_gl_texture_state_bytes() +
                           snx_gl_framebuffer_state_bytes());
    memcpy(&g_gl, i + sizeof(SnxGlStateHeader), sizeof g_gl);
    return 0;
}
