/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's textures and samplers (gl_texture.h).

   A level's storage comes with the base level: glTexImage* of the base level (or of a level whose size the base
   level's gives) makes the whole chain of levels at once, as the Android bridge's images are made, and a level of
   another size is left out, said. glTexStorage* makes the chain it names, immutable. An upload reads the client's
   texels where GL's pixel unpack state says they are: in the pixel unpack buffer bound, or at the client's address */
#include "common/gpu/gl/gl_texture.h"
#include "common/gpu/gl/gl_format.h"
#include "common/gpu/gl/gl_object.h"
#include "common/gpu/gl/gl_state.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    GL_OUT_OF_MEMORY_ = 0x0505,
    GL_TEXTURE_2D_ = 0x0de1,
    GL_TEXTURE_3D_ = 0x806f,
    GL_TEXTURE_CUBE_MAP_ = 0x8513,
    GL_TEXTURE_CUBE_MAP_POSITIVE_X_ = 0x8515,
    GL_TEXTURE_2D_ARRAY_ = 0x8c1a,
    GL_TEXTURE_2D_MULTISAMPLE_ = 0x9100,
    GL_TEXTURE0_ = 0x84c0,
    GL_TEXTURE_MAG_FILTER_ = 0x2800,
    GL_TEXTURE_MIN_FILTER_ = 0x2801,
    GL_TEXTURE_WRAP_S_ = 0x2802,
    GL_TEXTURE_WRAP_T_ = 0x2803,
    GL_TEXTURE_WRAP_R_ = 0x8072,
    GL_TEXTURE_MIN_LOD_ = 0x813a,
    GL_TEXTURE_MAX_LOD_ = 0x813b,
    GL_TEXTURE_BASE_LEVEL_ = 0x813c,
    GL_TEXTURE_MAX_LEVEL_ = 0x813d,
    GL_TEXTURE_LOD_BIAS_ = 0x8501,
    GL_TEXTURE_MAX_ANISOTROPY_ = 0x84fe,
    GL_TEXTURE_COMPARE_MODE_ = 0x884c,
    GL_TEXTURE_COMPARE_FUNC_ = 0x884d,
    GL_TEXTURE_SWIZZLE_R_ = 0x8e42,
    GL_TEXTURE_IMMUTABLE_FORMAT_ = 0x912f,
    GL_TEXTURE_IMMUTABLE_LEVELS_ = 0x82df,
    GL_DEPTH_STENCIL_TEXTURE_MODE_ = 0x90ea,
    GL_GENERATE_MIPMAP_ = 0x8191,
    GL_STENCIL_INDEX_ = 0x1901,
    GL_DEPTH_COMPONENT_ = 0x1902,
    GL_RED_ = 0x1903,
    GL_GREEN_ = 0x1904,
    GL_BLUE_ = 0x1905,
    GL_ALPHA_ = 0x1906,
    GL_RGB_ = 0x1907,
    GL_RGBA_ = 0x1908,
    GL_BGRA_ = 0x80e1,
    GL_BGR_ = 0x80e0,
    GL_RG_ = 0x8227,
    GL_UNSIGNED_INT_8_8_8_8_REV_ = 0x8367,
    GL_UNSIGNED_BYTE_ = 0x1401,
    GL_NEAREST_MIPMAP_LINEAR_ = 0x2702,
    GL_LINEAR_ = 0x2601,
    GL_REPEAT_ = 0x2901,
    GL_LEQUAL_ = 0x0203,
    GL_PIXEL_UNPACK_BUFFER_ = 0x88ec,
};

/* what a save state holds */
typedef struct {
    SnxGlTexture textures[SNX_GL_TEXTURES];
    SnxGlSampler samplers[SNX_GL_SAMPLERS];
    SnxGlTextureContext contexts[SNX_GL_CONTEXTS];
} SnxGlTextures;

static struct {
    SnxGlTextures s;
    SnxGlTextures checked; /* a save state's copy being checked: a check never touches the running state */
    SnxGlTextureHost host;
    uint32_t said; /* the lines said once, a bit each */
} g_gl_texture;

#define S g_gl_texture.s
#define H g_gl_texture.host

void snx_gl_texture_host(const SnxGlTextureHost* host) { H = host ? *host : (SnxGlTextureHost){0}; }
static uint32_t max_size(void) { return H.max_size ? H.max_size() : 16384; }

static void error(uint32_t ctx, uint32_t e) {
    if (ctx) snx_gl_error(ctx, e);
}
/* a line said once, by its bit */
[[gnu::format(printf, 2, 3)]] static void say_once(unsigned bit, const char* fmt, ...) {
    char line[300];
    va_list ap;
    if ((g_gl_texture.said >> bit) & 1 || !H.say) return;
    g_gl_texture.said |= 1u << bit;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    H.say(line);
}

/* ---- the records ---- */

SnxGlTextureContext* snx_gl_textures(uint32_t ctx) { return &S.contexts[ctx - 1]; }
void snx_gl_texture_context_reset(uint32_t ctx) { S.contexts[ctx - 1] = (SnxGlTextureContext){0}; }
SnxGlTexture* snx_gl_texture(uint32_t name) {
    return name && name <= SNX_GL_TEXTURES && S.textures[name - 1].used ? &S.textures[name - 1] : NULL;
}
SnxGlSampler* snx_gl_sampler(uint32_t name) {
    return name && name <= SNX_GL_SAMPLERS && S.samplers[name - 1].used ? &S.samplers[name - 1] : NULL;
}
static SnxGlSamplerState sampler_default(void) {
    return (SnxGlSamplerState){.min = GL_NEAREST_MIPMAP_LINEAR_,
                               .mag = GL_LINEAR_,
                               .wrap = {GL_REPEAT_, GL_REPEAT_, GL_REPEAT_},
                               .compare_func = GL_LEQUAL_,
                               .min_lod = -1000,
                               .max_lod = 1000,
                               .anisotropy = 1};
}
static void texture_init(SnxGlTexture* t) {
    *t = (SnxGlTexture){.used = 1, .target = 0xff, .format = -1, .max_level = 1000, .sampler = sampler_default()};
    t->swizzles[0] = GL_RED_, t->swizzles[1] = GL_GREEN_, t->swizzles[2] = GL_BLUE_, t->swizzles[3] = GL_ALPHA_;
}
uint32_t snx_gl_unit_texture(uint32_t ctx, uint32_t unit, uint32_t target) {
    return unit < SNX_GL_UNITS && target < SNX_GL_TARGETS ? snx_gl_textures(ctx)->textures[unit][target] : 0;
}
const SnxGlSamplerState* snx_gl_unit_sampler(uint32_t ctx, uint32_t unit, uint32_t texture) {
    const SnxGlSampler* s = unit < SNX_GL_UNITS ? snx_gl_sampler(snx_gl_textures(ctx)->samplers[unit]) : NULL;
    const SnxGlTexture* t = snx_gl_texture(texture);
    return s ? &s->state : t ? &t->sampler : NULL;
}
int snx_gl_texture_complete(uint32_t texture, int32_t* levels) {
    const SnxGlTexture* t = snx_gl_texture(texture);
    int32_t n = 1, top;
    if (!t || t->format < 0 || t->base_level >= t->levels || !(t->defined & (1u << t->base_level))) return 0;
    top = t->max_level < t->levels - 1 ? t->max_level : t->levels - 1;
    while (t->base_level + n <= top && (t->defined & (1u << (t->base_level + n)))) ++n;
    *levels = n;
    return 1;
}
int snx_gl_texture_get(uint32_t ctx, uint32_t pname, double v[4]) {
    const SnxGlTextureContext* c = snx_gl_textures(ctx);
    switch (pname) {
        case 0x84e0: v[0] = GL_TEXTURE0_ + c->active; return 1;              /* GL_ACTIVE_TEXTURE */
        case 0x8069: v[0] = c->textures[c->active][SNX_GL_T2D]; return 1;    /* GL_TEXTURE_BINDING_2D */
        case 0x8514: v[0] = c->textures[c->active][SNX_GL_TCUBE]; return 1;  /* GL_TEXTURE_BINDING_CUBE_MAP */
        case 0x806a: v[0] = c->textures[c->active][SNX_GL_T3D]; return 1;    /* GL_TEXTURE_BINDING_3D */
        case 0x8c1d: v[0] = c->textures[c->active][SNX_GL_TARRAY]; return 1; /* GL_TEXTURE_BINDING_2D_ARRAY */
        case 0x9104: v[0] = c->textures[c->active][SNX_GL_TMS]; return 1;    /* GL_TEXTURE_BINDING_2D_MULTISAMPLE */
        case 0x8919: v[0] = c->samplers[c->active]; return 1;                /* GL_SAMPLER_BINDING */
        default: return 0;
    }
}
void snx_gl_texture_defined(uint32_t texture, int32_t level) {
    SnxGlTexture* t = snx_gl_texture(texture);
    if (t && level >= 0 && level < SNX_GL_LEVELS) t->defined |= 1u << level;
}

static int target_index(uint32_t target) {
    switch (target) {
        case GL_TEXTURE_2D_: return SNX_GL_T2D;
        case GL_TEXTURE_CUBE_MAP_: return SNX_GL_TCUBE;
        case GL_TEXTURE_3D_: return SNX_GL_T3D;
        case GL_TEXTURE_2D_ARRAY_: return SNX_GL_TARRAY;
        case GL_TEXTURE_2D_MULTISAMPLE_: return SNX_GL_TMS;
        default: return -1;
    }
}
/* the texture bound to a GL target of the active unit, its name in *name; a cube map's face in *face */
static SnxGlTexture* bound_texture(uint32_t ctx, uint32_t target, int* face, uint32_t* name) {
    int f = 0, i;
    SnxGlTexture* t;
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X_ && target < GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + 6) {
        f = (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X_);
        target = GL_TEXTURE_CUBE_MAP_;
    }
    if (face) *face = f;
    i = target_index(target);
    if (i < 0) return error(ctx, SNX_GL_INVALID_ENUM), NULL;
    *name = snx_gl_unit_texture(ctx, snx_gl_textures(ctx)->active, (uint32_t)i);
    t = snx_gl_texture(*name);
    if (!t) error(ctx, SNX_GL_INVALID_OPERATION); /* texture 0: GL's default texture, which nothing here has */
    return t;
}

static int32_t full_levels(int32_t width, int32_t height, int32_t depth) {
    int32_t m = width > height ? width : height, n = 1;
    if (depth > m) m = depth;
    while (m > 1) m >>= 1, ++n;
    return n;
}
/* the bytes of a level of the texture, all its layers (a 3D texture's: its slices at the level) */
static uint64_t level_bytes(const SnxGlTexture* t, const SnxGlFormat* f, int32_t level) {
    int32_t images = t->target == SNX_GL_T3D ? snx_gl_level_size(t->depth, level) : t->layers;
    return snx_gl_format_bytes(f, snx_gl_level_size(t->width, level), snx_gl_level_size(t->height, level), images);
}
/* the texture's storage: format f, base level w x h x d, levels of it (samples, a multisampled one's), anew when it
   differs. 0, or -1 (the error recorded) when it has none */
static int storage(uint32_t ctx, uint32_t name, int32_t format, int32_t w, int32_t h, int32_t d, int32_t levels,
                   int32_t samples) {
    SnxGlTexture* t = &S.textures[name - 1];
    const SnxGlFormat* f = snx_gl_format_at(format);
    int32_t layers = t->target == SNX_GL_TCUBE ? 6 : t->target == SNX_GL_TARRAY ? d : 1;
    int32_t depth = t->target == SNX_GL_T3D ? d : 1;
    if (t->format == format && t->width == w && t->height == h && t->depth == depth && t->layers == layers &&
        t->levels >= levels && t->samples == samples)
        return 0;
    if (t->texels) snx_gl_memory_give(t->texels, t->texels_size);
    t->texels = t->texels_size = 0;
    t->format = format, t->width = w, t->height = h, t->depth = depth, t->layers = layers, t->levels = levels;
    t->samples = samples, t->defined = 0;
    t->generation++;
    if (H.keeps_texels && samples <= 1) {
        uint64_t size = 0, align = 256;
        if (H.storage_bytes)
            size = H.storage_bytes(name, &align);
        else
            for (int32_t l = 0; l < levels; l++) size += level_bytes(t, f, l);
        /* a platform layout of 0 bytes: one the platform has none of (it said so), and the texture keeps no texels */
        t->texels = size ? snx_gl_memory_take_zeroed(size, align) : 0; /* GL leaves them undefined: zeros */
        if (size && !t->texels) {
            t->format = -1, t->levels = 0;
            error(ctx, GL_OUT_OF_MEMORY_);
            return -1;
        }
        t->texels_size = t->texels ? size : 0;
    }
    if (H.storage && !H.storage(ctx, name)) { /* none: the next call that gives a storage tries again */
        if (t->texels) snx_gl_memory_give(t->texels, t->texels_size);
        t->texels = t->texels_size = 0;
        t->format = -1, t->levels = 0;
        return -1;
    }
    return 0;
}

/* desktop GL takes GL_BGRA texels into a texture of RGBA bytes, its red and blue swapped on the way in */
static int bgra_into(uint32_t ctx, const SnxGlFormat* f, uint32_t format, uint32_t type) {
    return snx_gl_context_api(ctx) == SNX_GL_API_DESKTOP && format == GL_BGRA_ && f->format == GL_RGBA_ &&
           f->type == GL_UNSIGNED_BYTE_ && f->bytes == 4 &&
           (type == GL_UNSIGNED_BYTE_ || type == GL_UNSIGNED_INT_8_8_8_8_REV_);
}

/* A desktop context's 8-bit texels of another component order or count than its texture's own 8-bit ones, which
   desktop GL converts and GLES refuses (a game's RGBA texels into an RGB8 texture): the client's bytes a texel, and
   for each of the texture's components the client byte it takes, or CONV_ZERO, or CONV_ONE (alpha) */
enum { CONV_ZERO = -1, CONV_ONE = -2 };
typedef struct {
    int8_t bytes;   /* the client's, 0 for no conversion */
    int8_t from[4]; /* the texture's R, G, B, A in their order */
} Conv;
/* the components a format of 8-bit ones holds, in order (0 R, 1 G, 2 B, 3 A): their count, 0 for another format */
static int components_of(uint32_t format, int8_t at[4]) {
    static const struct {
        uint32_t format;
        int8_t n, at[4];
    } table[] = {{GL_RED_, 1, {0}},           {GL_RG_, 2, {0, 1}},     {GL_RGB_, 3, {0, 1, 2}},
                 {GL_RGBA_, 4, {0, 1, 2, 3}}, {GL_BGR_, 3, {2, 1, 0}}, {GL_BGRA_, 4, {2, 1, 0, 3}}};
    for (size_t i = 0; i < sizeof table / sizeof *table; i++)
        if (table[i].format == format) {
            memcpy(at, table[i].at, 4);
            return table[i].n;
        }
    return 0;
}
static int convert_into(uint32_t ctx, const SnxGlFormat* f, uint32_t format, uint32_t type, Conv* c) {
    int8_t client[4], own[4];
    int nc, no;
    *c = (Conv){0};
    if (snx_gl_context_api(ctx) != SNX_GL_API_DESKTOP || type != GL_UNSIGNED_BYTE_ || f->type != GL_UNSIGNED_BYTE_ ||
        f->format == GL_BGRA_ || f->format == GL_BGR_)
        return 0;
    nc = components_of(format, client), no = components_of(f->format, own);
    if (!nc || !no || no != (int)f->bytes) return 0;
    c->bytes = (int8_t)nc;
    for (int i = 0; i < no; i++) {
        int want = own[i]; /* the component (R, G, B or A) the texture keeps i-th */
        c->from[i] = want == 3 ? CONV_ONE : CONV_ZERO;
        for (int k = 0; k < nc; k++)
            if (client[k] == want) c->from[i] = (int8_t)k;
    }
    return 1;
}

/* the client's texels of a box into the texture: read where GL's unpack state says, kept where the platform asks,
   then handed to the platform. With a conversion (conv->bytes), the client's texels are made the texture's first */
static void upload(uint32_t ctx, uint32_t name, int32_t level, int32_t x, int32_t y, int32_t z, int32_t w, int32_t h,
                   int32_t d, uint64_t pixels, int compressed, uint64_t csize, int swap_rb, const Conv* conv) {
    SnxGlTexture* t = &S.textures[name - 1];
    const SnxGlFormat* f = snx_gl_format_at(t->format);
    const SnxGlFixed* fx = snx_gl_fixed(ctx);
    uint32_t unpack = snx_gl_objects(ctx)->pixel_unpack;
    const SnxGlBuffer* b = snx_gl_buffer(unpack);
    const uint8_t* src;
    uint8_t* made = NULL;
    uint64_t skip = 0, need, texel;
    SnxGlUpload u = {level, x, y, z, w, h, d, NULL, 0, 0, 0, 0, compressed, swap_rb};
    if (!f || w <= 0 || h <= 0 || d <= 0) return;
    if (compressed) {
        int32_t bw = f->block >> 4, bh = f->block & 15;
        u.row = (uint64_t)((w + bw - 1) / bw) * f->bytes;
        u.rows = (uint64_t)((h + bh - 1) / bh);
        u.stride = u.row;
        u.image = u.row * u.rows;
        if (u.image * (uint64_t)d > csize) {
            error(ctx, SNX_GL_INVALID_VALUE);
            return;
        }
    } else {
        const int32_t row_length = fx->pixel[1].row_length, height = fx->pixel[1].image_height;
        uint64_t alignment = (uint64_t)fx->pixel[1].alignment, length;
        texel = conv && conv->bytes ? (uint64_t)conv->bytes : f->bytes; /* the client's */
        u.row = (uint64_t)w * texel;
        u.rows = (uint64_t)h;
        length = (uint64_t)(row_length ? row_length : w) * texel;
        u.stride = (length + alignment - 1) / alignment * alignment;
        u.image = u.stride * (uint64_t)(height ? height : h);
        skip = (uint64_t)fx->pixel[1].skip_images * u.image + (uint64_t)fx->pixel[1].skip_rows * u.stride +
               (uint64_t)fx->pixel[1].skip_pixels * texel;
    }
    /* the client's bytes the box reads, from pixels: the last image's last row ends them */
    need = skip + (uint64_t)(d - 1) * u.image + (u.rows - 1) * u.stride + u.row;
    if (b) {
        if (pixels > b->size || need > b->size - pixels) {
            error(ctx, SNX_GL_INVALID_OPERATION);
            return;
        }
        src = snx_gl_buffer_bytes(unpack) + pixels;
    } else {
        src = snx_gl_client_bytes(pixels);
    }
    if (!src) return;
    u.src = src + skip;
    if (conv && conv->bytes) { /* the texture's own texels, tight rows, from the client's */
        uint64_t row = (uint64_t)w * f->bytes;
        made = malloc((size_t)(row * u.rows * (uint64_t)d));
        if (!made) {
            error(ctx, GL_OUT_OF_MEMORY_);
            return;
        }
        for (int32_t k = 0; k < d; k++)
            for (uint64_t r = 0; r < u.rows; r++) {
                const uint8_t* from = u.src + (uint64_t)k * u.image + r * u.stride;
                uint8_t* to = made + ((uint64_t)k * u.rows + r) * row;
                for (int32_t i = 0; i < w; i++)
                    for (uint32_t m = 0; m < f->bytes; m++) {
                        int8_t at = conv->from[m];
                        to[(uint64_t)i * f->bytes + m] = at >= 0
                                                             ? from[(uint64_t)i * (uint64_t)conv->bytes + (uint64_t)at]
                                                         : at == CONV_ONE ? 0xff
                                                                          : 0;
                    }
            }
        u.src = made, u.row = row, u.stride = row, u.image = row * u.rows;
    }
    if (t->texels && !H.storage_bytes) { /* the layer's own: the level's images after the levels before, tight rows */
        uint64_t at = 0, level_row, level_image, x_bytes, y_rows;
        int32_t lw = snx_gl_level_size(t->width, level), lh = snx_gl_level_size(t->height, level);
        uint8_t* to = snx_gl_client_bytes(t->texels);
        for (int32_t l = 0; l < level; l++) at += level_bytes(t, f, l);
        level_row = snx_gl_format_bytes(f, lw, 1, 1);
        level_image = snx_gl_format_bytes(f, lw, lh, 1);
        x_bytes = f->block ? (uint64_t)(x / (f->block >> 4)) * f->bytes : (uint64_t)x * f->bytes;
        y_rows = f->block ? (uint64_t)(y / (f->block & 15)) : (uint64_t)y;
        for (int32_t k = 0; k < d; k++)
            for (uint64_t r = 0; r < u.rows; r++) {
                uint64_t off = at + (uint64_t)(z + k) * level_image + (y_rows + r) * level_row + x_bytes;
                const uint8_t* from = u.src + (uint64_t)k * u.image + r * u.stride;
                if (off + u.row > t->texels_size) continue;
                if (!swap_rb) {
                    memcpy(to + off, from, u.row);
                    continue;
                }
                for (uint64_t i = 0; i < u.row; i += 4) {
                    to[off + i] = from[i + 2], to[off + i + 1] = from[i + 1];
                    to[off + i + 2] = from[i], to[off + i + 3] = from[i + 3];
                }
            }
    }
    if (H.upload) H.upload(ctx, name, &u);
    free(made);
}

void snx_gl_gen_textures(uint32_t ctx, int32_t n, uint32_t* out) {
    uint32_t name = 1;
    if (n < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t v = 0;
        while (name <= SNX_GL_TEXTURES && S.textures[name - 1].used) name++;
        if (name <= SNX_GL_TEXTURES) {
            texture_init(&S.textures[name - 1]);
            v = name++;
        } else {
            error(ctx, GL_OUT_OF_MEMORY_);
        }
        memcpy(out + i, &v, 4);
    }
}
void snx_gl_delete_textures(uint32_t ctx, int32_t n, const uint32_t* names) {
    if (n < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t name;
        SnxGlTexture* t;
        memcpy(&name, names + i, 4);
        t = snx_gl_texture(name);
        if (!t) continue;
        if (t->texels) snx_gl_memory_give(t->texels, t->texels_size);
        *t = (SnxGlTexture){0};
        if (H.gone) H.gone(name);
        if (!ctx) continue;
        {
            SnxGlTextureContext* c = snx_gl_textures(ctx);
            for (int u = 0; u < SNX_GL_UNITS; u++)
                for (int k = 0; k < SNX_GL_TARGETS; k++)
                    if (c->textures[u][k] == name) c->textures[u][k] = 0;
            for (int u = 0; u < SNX_GL_IMAGE_UNITS; u++)
                if (c->images[u].texture == name) c->images[u].texture = 0;
        }
    }
}
int snx_gl_is_texture(uint32_t name) {
    const SnxGlTexture* t = snx_gl_texture(name);
    return t && t->target != 0xff;
}
void snx_gl_bind_texture(uint32_t ctx, uint32_t target, uint32_t name) {
    int i = target_index(target);
    SnxGlTexture* t;
    if (i < 0) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return;
    }
    if (name > SNX_GL_TEXTURES) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    t = snx_gl_texture(name);
    if (name && !t) { /* GLES makes a texture of a name its first bind gives */
        t = &S.textures[name - 1];
        texture_init(t);
    }
    if (t && t->target == 0xff) t->target = (uint8_t)i;
    if (t && t->target != i) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    snx_gl_textures(ctx)->textures[snx_gl_textures(ctx)->active][i] = name;
}
void snx_gl_active_texture(uint32_t ctx, uint32_t unit) {
    if (unit - GL_TEXTURE0_ < SNX_GL_UNITS)
        snx_gl_textures(ctx)->active = unit - GL_TEXTURE0_;
    else
        error(ctx, SNX_GL_INVALID_ENUM);
}

void snx_gl_tex_image(uint32_t ctx, uint32_t target, int32_t level, uint32_t internal, int32_t w, int32_t h, int32_t d,
                      uint32_t format, uint32_t type, uint64_t pixels, int compressed, uint64_t csize) {
    int face = 0;
    uint32_t name;
    SnxGlTexture* t = bound_texture(ctx, target, &face, &name);
    int32_t index, bw, bh, bd;
    int swap_rb = 0;
    Conv conv = {0};
    const SnxGlFormat* f;
    if (!t) return;
    if (level < 0 || level >= SNX_GL_LEVELS || w < 0 || h < 0 || d < 0 || (uint32_t)w > max_size() ||
        (uint32_t)h > max_size()) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    if (t->immutable) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    index = snx_gl_format(internal, format, type);
    f = snx_gl_format_at(index);
    if (!f || (!compressed && f->block) || (compressed && !f->block)) {
        say_once(0, "no texture format for 0x%x (0x%x/0x%x): GL_INVALID_OPERATION", (unsigned)internal,
                 (unsigned)format, (unsigned)type);
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    if (!compressed && (format != f->format || type != f->type) && !(swap_rb = bgra_into(ctx, f, format, type)) &&
        !convert_into(ctx, f, format, type, &conv)) {
        say_once(1, "texels of 0x%x/0x%x into a texture of 0x%x, whose own are 0x%x/0x%x: the texture stays empty",
                 (unsigned)format, (unsigned)type, (unsigned)internal, (unsigned)f->format, (unsigned)f->type);
        pixels = 0;
    }
    if (!w || !h || !d) return;
    /* the base level's size, from this level's */
    bw = w << level, bh = h << level, bd = t->target == SNX_GL_T3D ? d << level : d;
    if (!(t->format == index && snx_gl_level_size(t->width, level) == w && snx_gl_level_size(t->height, level) == h)) {
        if (level && t->format >= 0) {
            say_once(2, "a level of %dx%d is not level %d of the base level's %dx%d: it is left out", (int)w, (int)h,
                     (int)level, (int)t->width, (int)t->height);
            return;
        }
        if (storage(ctx, name, index, bw, bh, bd, full_levels(bw, bh, t->target == SNX_GL_T3D ? bd : 1), 1)) return;
    }
    t->defined |= 1u << level;
    if (pixels || snx_gl_buffer(snx_gl_objects(ctx)->pixel_unpack))
        upload(ctx, name, level, 0, 0, t->target == SNX_GL_TCUBE ? face : 0, w, h, t->target == SNX_GL_TCUBE ? 1 : d,
               pixels, compressed, csize, swap_rb, &conv);
}
void snx_gl_tex_sub_image(uint32_t ctx, uint32_t target, int32_t level, int32_t x, int32_t y, int32_t z, int32_t w,
                          int32_t h, int32_t d, uint32_t format, uint32_t type, uint64_t pixels, int compressed,
                          uint64_t csize) {
    int face = 0;
    uint32_t name;
    SnxGlTexture* t = bound_texture(ctx, target, &face, &name);
    const SnxGlFormat* f;
    Conv conv = {0};
    if (!t) return;
    f = snx_gl_format_at(t->format);
    if (!f || level < 0 || level >= t->levels || x < 0 || y < 0 || z < 0 || w < 0 || h < 0 || d < 0 ||
        x + w > snx_gl_level_size(t->width, level) || y + h > snx_gl_level_size(t->height, level)) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    if (compressed != (f->block != 0)) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    if (!compressed && (format != f->format || type != f->type) && !bgra_into(ctx, f, format, type) &&
        !convert_into(ctx, f, format, type, &conv)) {
        say_once(1, "texels of 0x%x/0x%x into a texture of 0x%x, whose own are 0x%x/0x%x: the texture stays empty",
                 (unsigned)format, (unsigned)type, (unsigned)f->internal, (unsigned)f->format, (unsigned)f->type);
        return;
    }
    if (t->target == SNX_GL_TCUBE) z = face, d = 1;
    upload(ctx, name, level, x, y, z, w, h, d, pixels, compressed, csize,
           !compressed && !conv.bytes && (format != f->format || type != f->type), &conv);
}
/* glTexStorage*'s storage of the texture bound to target, its samples a multisampled one's, else 0 */
static void tex_storage(uint32_t ctx, uint32_t target, int32_t levels, uint32_t internal, int32_t w, int32_t h,
                        int32_t d, int32_t samples) {
    uint32_t name;
    SnxGlTexture* t = bound_texture(ctx, target, NULL, &name);
    int32_t index = snx_gl_format(internal, 0, 0);
    if (!t) return;
    if (samples && (t->target != SNX_GL_TMS || samples < 1)) {
        error(ctx, samples < 1 ? SNX_GL_INVALID_VALUE : SNX_GL_INVALID_ENUM);
        return;
    }
    if (index < 0 || levels < 1 || w < 1 || h < 1 || d < 1 || (uint32_t)w > max_size() || (uint32_t)h > max_size() ||
        levels > full_levels(w, h, t->target == SNX_GL_T3D ? d : 1)) {
        error(ctx, index < 0 ? SNX_GL_INVALID_ENUM : SNX_GL_INVALID_VALUE);
        return;
    }
    if (t->immutable) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    if (storage(ctx, name, index, w, h, d, levels, samples > 2 ? 4 : samples > 1 ? 2 : 1)) return;
    t->immutable = 1;
    t->defined = levels >= 32 ? 0xffffffffu : (1u << levels) - 1;
    if (t->max_level > levels - 1) t->max_level = levels - 1;
}
void snx_gl_tex_storage(uint32_t ctx, uint32_t target, int32_t levels, uint32_t internal, int32_t w, int32_t h,
                        int32_t d) {
    tex_storage(ctx, target, levels, internal, w, h, d, 0);
}
void snx_gl_tex_storage_multisample(uint32_t ctx, uint32_t target, int32_t samples, uint32_t internal, int32_t w,
                                    int32_t h) {
    tex_storage(ctx, target, 1, internal, w, h, 1, samples < 1 ? -1 : samples);
}
uint32_t snx_gl_copy_tex_image(uint32_t ctx, uint32_t target, int32_t level, uint32_t internal, int32_t w, int32_t h) {
    uint32_t name;
    SnxGlTexture* t;
    int32_t index, bw, bh;
    if (target != GL_TEXTURE_2D_ &&
        !(target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X_ && target < GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + 6)) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return 0;
    }
    t = bound_texture(ctx, target, NULL, &name);
    if (!t) return 0;
    if (level < 0 || level >= SNX_GL_LEVELS || w < 0 || h < 0 || (uint32_t)w > max_size() || (uint32_t)h > max_size()) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return 0;
    }
    if (t->immutable) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return 0;
    }
    if (!w || !h) return 0;
    /* an unsized internal format takes the read framebuffer's 4 bytes a texel */
    index = snx_gl_format(internal, internal == GL_RGB_ ? GL_RGB_ : GL_RGBA_, GL_UNSIGNED_BYTE_);
    if (index < 0) index = snx_gl_format(GL_RGBA_, GL_RGBA_, GL_UNSIGNED_BYTE_);
    bw = w << level, bh = h << level;
    if ((t->format != index || snx_gl_level_size(t->width, level) != w || snx_gl_level_size(t->height, level) != h) &&
        storage(ctx, name, index, bw, bh, 1, full_levels(bw, bh, 1), 1))
        return 0;
    t->defined |= 1u << level;
    return name;
}

/* a parameter of a sampler's state, of a desktop context (its LOD bias) or any: 0 for one that is not a sampler's */
static int sampler_set(uint32_t ctx, SnxGlSamplerState* s, uint32_t pname, float v) {
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER_: s->min = (uint32_t)v; return 1;
        case GL_TEXTURE_MAG_FILTER_: s->mag = (uint32_t)v; return 1;
        case GL_TEXTURE_WRAP_S_: s->wrap[0] = (uint32_t)v; return 1;
        case GL_TEXTURE_WRAP_T_: s->wrap[1] = (uint32_t)v; return 1;
        case GL_TEXTURE_WRAP_R_: s->wrap[2] = (uint32_t)v; return 1;
        case GL_TEXTURE_MIN_LOD_: s->min_lod = v; return 1;
        case GL_TEXTURE_MAX_LOD_: s->max_lod = v; return 1;
        case GL_TEXTURE_COMPARE_MODE_: s->compare_mode = (uint32_t)v; return 1;
        case GL_TEXTURE_COMPARE_FUNC_: s->compare_func = (uint32_t)v; return 1;
        case GL_TEXTURE_MAX_ANISOTROPY_: s->anisotropy = v < 1 ? 1 : v; return 1;
        case GL_TEXTURE_LOD_BIAS_:
            if (snx_gl_context_api(ctx) != SNX_GL_API_DESKTOP) return 0;
            s->lod_bias = v;
            return 1;
        default: return 0;
    }
}
static int sampler_get(uint32_t ctx, const SnxGlSamplerState* s, uint32_t pname, float* v) {
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER_: *v = (float)s->min; return 1;
        case GL_TEXTURE_MAG_FILTER_: *v = (float)s->mag; return 1;
        case GL_TEXTURE_WRAP_S_: *v = (float)s->wrap[0]; return 1;
        case GL_TEXTURE_WRAP_T_: *v = (float)s->wrap[1]; return 1;
        case GL_TEXTURE_WRAP_R_: *v = (float)s->wrap[2]; return 1;
        case GL_TEXTURE_MIN_LOD_: *v = s->min_lod; return 1;
        case GL_TEXTURE_MAX_LOD_: *v = s->max_lod; return 1;
        case GL_TEXTURE_COMPARE_MODE_: *v = (float)s->compare_mode; return 1;
        case GL_TEXTURE_COMPARE_FUNC_: *v = (float)s->compare_func; return 1;
        case GL_TEXTURE_MAX_ANISOTROPY_: *v = s->anisotropy; return 1;
        case GL_TEXTURE_LOD_BIAS_:
            if (snx_gl_context_api(ctx) != SNX_GL_API_DESKTOP) return 0;
            *v = s->lod_bias;
            return 1;
        default: return 0;
    }
}
void snx_gl_tex_parameter(uint32_t ctx, uint32_t target, uint32_t pname, float v) {
    uint32_t name;
    SnxGlTexture* t = bound_texture(ctx, target, NULL, &name);
    if (!t || sampler_set(ctx, &t->sampler, pname, v)) return;
    switch (pname) {
        case GL_TEXTURE_BASE_LEVEL_: t->base_level = (int32_t)v; break;
        case GL_TEXTURE_MAX_LEVEL_: t->max_level = (int32_t)v; break;
        case GL_TEXTURE_SWIZZLE_R_:
        case GL_TEXTURE_SWIZZLE_R_ + 1:
        case GL_TEXTURE_SWIZZLE_R_ + 2:
        case GL_TEXTURE_SWIZZLE_R_ + 3: t->swizzles[pname - GL_TEXTURE_SWIZZLE_R_] = (uint32_t)v; break;
        case GL_GENERATE_MIPMAP_: break; /* GLES 1.1's */
        case GL_DEPTH_STENCIL_TEXTURE_MODE_: t->stencil_mode = (uint32_t)v == GL_STENCIL_INDEX_; break;
        default: error(ctx, SNX_GL_INVALID_ENUM);
    }
}
int snx_gl_get_tex_parameter(uint32_t ctx, uint32_t target, uint32_t pname, float* v) {
    uint32_t name;
    const SnxGlTexture* t = bound_texture(ctx, target, NULL, &name);
    if (!t) return 0;
    if (sampler_get(ctx, &t->sampler, pname, v)) return 1;
    switch (pname) {
        case GL_TEXTURE_BASE_LEVEL_: *v = (float)t->base_level; return 1;
        case GL_TEXTURE_MAX_LEVEL_: *v = (float)t->max_level; return 1;
        case GL_TEXTURE_IMMUTABLE_FORMAT_: *v = t->immutable; return 1;
        case GL_TEXTURE_IMMUTABLE_LEVELS_: *v = t->immutable ? (float)t->levels : 0; return 1;
        case GL_DEPTH_STENCIL_TEXTURE_MODE_:
            *v = t->stencil_mode ? (float)GL_STENCIL_INDEX_ : (float)GL_DEPTH_COMPONENT_;
            return 1;
        case GL_TEXTURE_SWIZZLE_R_:
        case GL_TEXTURE_SWIZZLE_R_ + 1:
        case GL_TEXTURE_SWIZZLE_R_ + 2:
        case GL_TEXTURE_SWIZZLE_R_ + 3: *v = (float)t->swizzles[pname - GL_TEXTURE_SWIZZLE_R_]; return 1;
        default: error(ctx, SNX_GL_INVALID_ENUM); return 0;
    }
}
int snx_gl_get_tex_level_parameter(uint32_t ctx, uint32_t target, int32_t level, uint32_t pname, float* v) {
    uint32_t name;
    const SnxGlTexture* t = bound_texture(ctx, target, NULL, &name);
    const SnxGlFormat* f;
    int has;
    if (!t) return 0;
    f = snx_gl_format_at(t->format);
    has = f && level >= 0 && level < t->levels && (t->defined & (1u << level));
    switch (pname) {
        case 0x1000: *v = has ? (float)snx_gl_level_size(t->width, level) : 0; return 1;  /* GL_TEXTURE_WIDTH */
        case 0x1001: *v = has ? (float)snx_gl_level_size(t->height, level) : 0; return 1; /* GL_TEXTURE_HEIGHT */
        case 0x8071:                                                                      /* GL_TEXTURE_DEPTH */
            *v = has ? (float)(t->target == SNX_GL_T3D ? snx_gl_level_size(t->depth, level) : t->layers) : 0;
            return 1;
        case 0x1003: *v = has ? (float)f->internal : (float)GL_RGBA_; return 1;    /* GL_TEXTURE_INTERNAL_FORMAT */
        case 0x9106: *v = has && t->samples > 1 ? (float)t->samples : 0; return 1; /* GL_TEXTURE_SAMPLES */
        case 0x9107: *v = 1; return 1;                                             /* fixed sample locations */
        case 0x86a1: *v = has && f->block; return 1;                               /* GL_TEXTURE_COMPRESSED */
        default:
            if (pname >= 0x805c && pname <= 0x805f) { /* GL_TEXTURE_RED_SIZE to ALPHA */
                *v = has && f->kind < SNX_GL_DEPTH ? 8 : 0;
                return 1;
            }
            error(ctx, SNX_GL_INVALID_ENUM);
            return 0;
    }
}
void snx_gl_generate_mipmap(uint32_t ctx, uint32_t target) {
    uint32_t name;
    SnxGlTexture* t = bound_texture(ctx, target, NULL, &name);
    int32_t base;
    if (!t || t->format < 0) return;
    base = t->base_level;
    if (base >= t->levels - 1 || !(t->defined & (1u << base))) return;
    if (H.mipmaps && !H.mipmaps(ctx, name)) return;
    for (int32_t l = base + 1; l < t->levels; l++) t->defined |= 1u << l;
}
void snx_gl_bind_image_texture(uint32_t ctx, uint32_t unit, uint32_t texture, int32_t level, int layered, int32_t layer,
                               uint32_t access, uint32_t format) {
    if (unit >= SNX_GL_IMAGE_UNITS || texture > SNX_GL_TEXTURES) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    snx_gl_textures(ctx)->images[unit] = (SnxGlImageUnit){texture, level, layer, layered != 0, access, format};
    if (snx_gl_texture(texture) && H.image_bound) H.image_bound(ctx, texture);
}

/* ---- samplers ---- */

void snx_gl_gen_samplers(uint32_t ctx, int32_t n, uint32_t* out) {
    uint32_t name = 1;
    if (n < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t v = 0;
        while (name <= SNX_GL_SAMPLERS && S.samplers[name - 1].used) name++;
        if (name <= SNX_GL_SAMPLERS) {
            S.samplers[name - 1] = (SnxGlSampler){1, sampler_default()};
            v = name++;
        } else {
            error(ctx, GL_OUT_OF_MEMORY_);
        }
        memcpy(out + i, &v, 4);
    }
}
void snx_gl_delete_samplers(uint32_t ctx, int32_t n, const uint32_t* names) {
    if (n < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t name;
        memcpy(&name, names + i, 4);
        if (!snx_gl_sampler(name)) continue;
        S.samplers[name - 1] = (SnxGlSampler){0};
        for (int u = 0; ctx && u < SNX_GL_UNITS; u++)
            if (snx_gl_textures(ctx)->samplers[u] == name) snx_gl_textures(ctx)->samplers[u] = 0;
    }
}
int snx_gl_is_sampler(uint32_t name) { return snx_gl_sampler(name) != NULL; }
void snx_gl_bind_sampler(uint32_t ctx, uint32_t unit, uint32_t sampler) {
    if (unit >= SNX_GL_UNITS || (sampler && !snx_gl_sampler(sampler)))
        error(ctx, unit >= SNX_GL_UNITS ? SNX_GL_INVALID_VALUE : SNX_GL_INVALID_OPERATION);
    else
        snx_gl_textures(ctx)->samplers[unit] = sampler;
}
void snx_gl_sampler_parameter(uint32_t ctx, uint32_t sampler, uint32_t pname, float v) {
    SnxGlSampler* s = snx_gl_sampler(sampler);
    if (!s)
        error(ctx, SNX_GL_INVALID_OPERATION);
    else if (!sampler_set(ctx, &s->state, pname, v))
        error(ctx, SNX_GL_INVALID_ENUM);
}
int snx_gl_get_sampler_parameter(uint32_t ctx, uint32_t sampler, uint32_t pname, float* v) {
    const SnxGlSampler* s = snx_gl_sampler(sampler);
    if (!s || !sampler_get(ctx, &s->state, pname, v)) {
        error(ctx, s ? SNX_GL_INVALID_ENUM : SNX_GL_INVALID_OPERATION);
        return 0;
    }
    return 1;
}

/* ---- the save state ---- */

size_t snx_gl_texture_state_bytes(void) { return 2 * sizeof(uint32_t) + sizeof S; }
void snx_gl_texture_state_save(void* out) {
    const uint32_t h[2] = {(uint32_t)sizeof S, 0};
    memcpy(out, h, sizeof h);
    memcpy((unsigned char*)out + sizeof h, &S, sizeof S);
}
int snx_gl_texture_state_check(const void* in, size_t size, char* why, size_t whysize) {
    SnxGlTextures* const c = &g_gl_texture.checked;
    uint32_t h[2];
    if (size < sizeof h) {
        snprintf(why, whysize, "the GL layer's textures are %zu bytes, less than their %zu-byte header", size,
                 sizeof h);
        return -1;
    }
    memcpy(h, in, sizeof h);
    if (h[0] != sizeof *c || size != sizeof h + sizeof *c) {
        snprintf(why, whysize, "the GL layer's textures are %u bytes of records (%zu in all); this build reads %zu",
                 (unsigned)h[0], size, sizeof *c);
        return -1;
    }
    memcpy(c, (const unsigned char*)in + sizeof h, sizeof *c);
    for (uint32_t n = 1; n <= SNX_GL_TEXTURES; n++) {
        const SnxGlTexture* t = &c->textures[n - 1];
        if (!t->used) continue;
        if ((t->target >= SNX_GL_TARGETS && t->target != 0xff) || t->format < -1 || t->format >= SNX_GL_FORMATS ||
            t->levels < 0 || t->levels > SNX_GL_LEVELS || t->layers < 0 || t->width < 0 || t->height < 0) {
            snprintf(why, whysize,
                     "the GL layer's texture %u has target %u, format %d, %d levels, %d layers of %dx%d, which GL "
                     "has not",
                     (unsigned)n, (unsigned)t->target, (int)t->format, (int)t->levels, (int)t->layers, (int)t->width,
                     (int)t->height);
            return -1;
        }
        if (t->texels && !snx_gl_object_checked_holds(t->texels, t->texels_size)) {
            snprintf(why, whysize, "the GL layer's texture %u keeps its texels at %llx+%llx, outside its memory",
                     (unsigned)n, (unsigned long long)t->texels, (unsigned long long)t->texels_size);
            return -1;
        }
    }
    for (uint32_t i = 0; i < SNX_GL_CONTEXTS; i++) {
        const SnxGlTextureContext* x = &c->contexts[i];
        int bad = x->active >= SNX_GL_UNITS;
        for (int u = 0; !bad && u < SNX_GL_UNITS; u++) {
            bad = x->samplers[u] > SNX_GL_SAMPLERS;
            for (int k = 0; !bad && k < SNX_GL_TARGETS; k++) bad = x->textures[u][k] > SNX_GL_TEXTURES;
        }
        for (int u = 0; !bad && u < SNX_GL_IMAGE_UNITS; u++) bad = x->images[u].texture > SNX_GL_TEXTURES;
        if (bad) {
            snprintf(why, whysize, "the GL layer's context %u binds a texture or sampler past %d and %d (unit %u)",
                     (unsigned)(i + 1), SNX_GL_TEXTURES, SNX_GL_SAMPLERS, (unsigned)x->active);
            return -1;
        }
    }
    return 0;
}
void snx_gl_texture_state_load(const void* in) {
    memcpy(&S, (const unsigned char*)in + 2 * sizeof(uint32_t), sizeof S);
    for (uint32_t n = 1; H.gone && n <= SNX_GL_TEXTURES; n++) H.gone(n); /* what a platform made is of the old */
}
