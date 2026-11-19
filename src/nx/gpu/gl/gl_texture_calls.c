/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's texture and sampler calls on the console (gl_layer.h, gpu/gl/gl.hook's texture family): each the
   shared layer's (common/gpu/gl/gl_texture.h) on the calling thread's current context, under the object lock, since
   the layer keeps each texture's texels in the same guest memory as the buffers' bytes (gl_object_calls.c) and reads
   the pixel unpack buffer. The texels kept there, level after level, are what the renderer samples once the layer
   draws, and a save state holds them with the game's memory. Without a current context a call does nothing and
   returns 0 */
#include "common/gpu/gl/gl_state.h"
#include "common/gpu/gl/gl_format.h"
#include "common/gpu/gl/gl_texture.h"
#include "gpu/gl/gl.hook.h"
#include "nx/gpu/gl/gl_layer.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn/nvn_copy.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>
#include <string.h>

static void say(const char* line) { LOG("gl layer: %s\n", line); }

/* ---- the storage: as NVN lays out a texture of the same description, in the GPU's block-linear layout ---- */

/* each shared format (common/gpu/gl/gl_format.c's table, in its order) as an NVN format, the NVN swizzle its view
   reads (0: the identity), and whether its client's 3-byte texels take the format's 4 bytes (alpha one) */
#define SW(r, g, b, a)                                                                                                 \
    (NVN_TEXTURE_SWIZZLE_##r | NVN_TEXTURE_SWIZZLE_##g << 3 | NVN_TEXTURE_SWIZZLE_##b << 6 |                           \
     NVN_TEXTURE_SWIZZLE_##a << 9 | NVN_TEX_SWIZZLE_SET)
static const struct {
    u16 format, swizzle;
    u8 expand;
} g_nvn_formats[] = {
    /* GLES 2's unsized formats, as their format and type */
    {NVN_FORMAT_RGBA8, 0, 0},                      /* GL_RGBA_ */
    {NVN_FORMAT_RGB8, SW(R, G, B, ONE), 0},        /* GL_RGB_ */
    {NVN_FORMAT_RGBA4, 0, 0},                      /* GL_RGBA_ */
    {NVN_FORMAT_RGB5A1, 0, 0},                     /* GL_RGBA_ */
    {NVN_FORMAT_RGB565, SW(R, G, B, ONE), 0},      /* GL_RGB_ */
    {NVN_FORMAT_R8, SW(R, R, R, ONE), 0},          /* GL_LUMINANCE_ */
    {NVN_FORMAT_R8, SW(ZERO, ZERO, ZERO, R), 0},   /* GL_ALPHA_ */
    {NVN_FORMAT_RG8, SW(R, R, R, G), 0},           /* GL_LUMINANCE_ALPHA_ */
    {NVN_FORMAT_BGRA8, 0, 0},                      /* GL_BGRA_EXT_ */
    {NVN_FORMAT_RGBA16F, 0, 0},                    /* GL_RGBA_ */
    {NVN_FORMAT_RGBA16F, 0, 0},                    /* GL_RGBA_ */
    {NVN_FORMAT_RGBA32F, 0, 0},                    /* GL_RGBA_ */
    {NVN_FORMAT_RGB16F, SW(R, G, B, ONE), 0},      /* GL_RGB_ */
    {NVN_FORMAT_RGB16F, SW(R, G, B, ONE), 0},      /* GL_RGB_ */
    {NVN_FORMAT_RGB32F, SW(R, G, B, ONE), 0},      /* GL_RGB_ */
    {NVN_FORMAT_R16F, SW(R, R, R, ONE), 0},        /* GL_LUMINANCE_ */
    {NVN_FORMAT_R32F, SW(R, R, R, ONE), 0},        /* GL_LUMINANCE_ */
    {NVN_FORMAT_R16F, SW(ZERO, ZERO, ZERO, R), 0}, /* GL_ALPHA_ */
    {NVN_FORMAT_R32F, SW(ZERO, ZERO, ZERO, R), 0}, /* GL_ALPHA_ */
    {NVN_FORMAT_RG16F, SW(R, R, R, G), 0},         /* GL_LUMINANCE_ALPHA_ */
    {NVN_FORMAT_RG32F, SW(R, R, R, G), 0},         /* GL_LUMINANCE_ALPHA_ */
    {NVN_FORMAT_R8, 0, 0},                         /* GL_RED_ */
    {NVN_FORMAT_RG8, 0, 0},                        /* GL_RG_ */
    {NVN_FORMAT_DEPTH16, 0, 0},                    /* GL_DEPTH_COMPONENT_ */
    {NVN_FORMAT_DEPTH24, 0, 0},                    /* GL_DEPTH_COMPONENT_ */
    {NVN_FORMAT_DEPTH24_STENCIL8, 0, 0},           /* GL_DEPTH_STENCIL_ */
    /* GLES 3's sized formats */
    {NVN_FORMAT_R8, 0, 0},                /* 0x8229 */
    {NVN_FORMAT_R8SN, 0, 0},              /* 0x8f94 */
    {NVN_FORMAT_R16F, 0, 0},              /* 0x822d */
    {NVN_FORMAT_R32F, 0, 0},              /* 0x822e */
    {NVN_FORMAT_R8UI, 0, 0},              /* 0x8232 */
    {NVN_FORMAT_R8I, 0, 0},               /* 0x8231 */
    {NVN_FORMAT_R16UI, 0, 0},             /* 0x8234 */
    {NVN_FORMAT_R16I, 0, 0},              /* 0x8233 */
    {NVN_FORMAT_R32UI, 0, 0},             /* 0x8236 */
    {NVN_FORMAT_R32I, 0, 0},              /* 0x8235 */
    {NVN_FORMAT_RG8, 0, 0},               /* 0x822b */
    {NVN_FORMAT_RG8SN, 0, 0},             /* 0x8f95 */
    {NVN_FORMAT_RG16F, 0, 0},             /* 0x822f */
    {NVN_FORMAT_RG32F, 0, 0},             /* 0x8230 */
    {NVN_FORMAT_RG8UI, 0, 0},             /* 0x8238 */
    {NVN_FORMAT_RG8I, 0, 0},              /* 0x8237 */
    {NVN_FORMAT_RG16UI, 0, 0},            /* 0x823a */
    {NVN_FORMAT_RG16I, 0, 0},             /* 0x8239 */
    {NVN_FORMAT_RG32UI, 0, 0},            /* 0x823c */
    {NVN_FORMAT_RG32I, 0, 0},             /* 0x823b */
    {NVN_FORMAT_RGB8, 0, 0},              /* 0x8051 */
    {NVN_FORMAT_RGBX8_SRGB, 0, 1},        /* 0x8c41 */
    {NVN_FORMAT_RGB565, 0, 0},            /* 0x8d62 */
    {NVN_FORMAT_RGB8SN, 0, 0},            /* 0x8f96 */
    {NVN_FORMAT_R11G11B10F, 0, 0},        /* 0x8c3a */
    {NVN_FORMAT_RGB9E5F, 0, 0},           /* 0x8c3d */
    {NVN_FORMAT_RGB16F, 0, 0},            /* 0x881b */
    {NVN_FORMAT_RGB32F, 0, 0},            /* 0x8815 */
    {NVN_FORMAT_RGB8UI, 0, 0},            /* 0x8d7d */
    {NVN_FORMAT_RGB8I, 0, 0},             /* 0x8d8f */
    {NVN_FORMAT_RGB16UI, 0, 0},           /* 0x8d77 */
    {NVN_FORMAT_RGB16I, 0, 0},            /* 0x8d89 */
    {NVN_FORMAT_RGB32UI, 0, 0},           /* 0x8d71 */
    {NVN_FORMAT_RGB32I, 0, 0},            /* 0x8d83 */
    {NVN_FORMAT_RGBA8, 0, 0},             /* 0x8058 */
    {NVN_FORMAT_RGBA8_SRGB, 0, 0},        /* 0x8c43 */
    {NVN_FORMAT_RGBA8SN, 0, 0},           /* 0x8f97 */
    {NVN_FORMAT_RGB5A1, 0, 0},            /* 0x8057 */
    {NVN_FORMAT_RGBA4, 0, 0},             /* 0x8056 */
    {NVN_FORMAT_RGB10A2, 0, 0},           /* 0x8059 */
    {NVN_FORMAT_RGBA16F, 0, 0},           /* 0x881a */
    {NVN_FORMAT_RGBA32F, 0, 0},           /* 0x8814 */
    {NVN_FORMAT_RGBA8UI, 0, 0},           /* 0x8d7c */
    {NVN_FORMAT_RGBA8I, 0, 0},            /* 0x8d8e */
    {NVN_FORMAT_RGB10A2UI, 0, 0},         /* 0x906f */
    {NVN_FORMAT_RGBA16UI, 0, 0},          /* 0x8d76 */
    {NVN_FORMAT_RGBA16I, 0, 0},           /* 0x8d88 */
    {NVN_FORMAT_RGBA32UI, 0, 0},          /* 0x8d70 */
    {NVN_FORMAT_RGBA32I, 0, 0},           /* 0x8d82 */
    {NVN_FORMAT_BGRA8, 0, 0},             /* 0x93a1 */
    {NVN_FORMAT_DEPTH16, 0, 0},           /* 0x81a5 */
    {NVN_FORMAT_DEPTH24, 0, 0},           /* 0x81a6 */
    {NVN_FORMAT_DEPTH32F, 0, 0},          /* 0x8cac */
    {NVN_FORMAT_DEPTH24_STENCIL8, 0, 0},  /* 0x88f0 */
    {NVN_FORMAT_DEPTH32F_STENCIL8, 0, 0}, /* 0x8cad */
    {NVN_FORMAT_STENCIL8, 0, 0},          /* 0x8d48 */
    /* compressed: the device's own formats, the bytes of a block, its width and height */
    {0, 0, 0}, /* 0x8d64: ETC2_R8G8B8_UNORM_BLOCK, which NVN's formats have not */
    {0, 0, 0}, /* 0x9274: ETC2_R8G8B8_UNORM_BLOCK, which NVN's formats have not */
    {0, 0, 0}, /* 0x9275: ETC2_R8G8B8_SRGB_BLOCK, which NVN's formats have not */
    {0, 0, 0}, /* 0x9276: ETC2_R8G8B8A1_UNORM_BLOCK, which NVN's formats have not */
    {0, 0, 0}, /* 0x9277: ETC2_R8G8B8A1_SRGB_BLOCK, which NVN's formats have not */
    {0, 0, 0}, /* 0x9278: ETC2_R8G8B8A8_UNORM_BLOCK, which NVN's formats have not */
    {0, 0, 0}, /* 0x9279: ETC2_R8G8B8A8_SRGB_BLOCK, which NVN's formats have not */
    {0, 0, 0}, /* 0x9270: EAC_R11_UNORM_BLOCK, which NVN's formats have not */
    {0, 0, 0}, /* 0x9271: EAC_R11_SNORM_BLOCK, which NVN's formats have not */
    {0, 0, 0}, /* 0x9272: EAC_R11G11_UNORM_BLOCK, which NVN's formats have not */
    {0, 0, 0}, /* 0x9273: EAC_R11G11_SNORM_BLOCK, which NVN's formats have not */
    /* BCn */
    {NVN_FORMAT_RGB_DXT1, 0, 0},
    {NVN_FORMAT_RGBA_DXT1, 0, 0},
    {NVN_FORMAT_RGBA_DXT3, 0, 0},
    {NVN_FORMAT_RGBA_DXT5, 0, 0},
    {NVN_FORMAT_RGB_DXT1_SRGB, 0, 0},
    {NVN_FORMAT_RGBA_DXT1_SRGB, 0, 0},
    {NVN_FORMAT_RGBA_DXT3_SRGB, 0, 0},
    {NVN_FORMAT_RGBA_DXT5_SRGB, 0, 0},
    {NVN_FORMAT_RGTC1_UNORM, 0, 0},
    {NVN_FORMAT_RGTC1_SNORM, 0, 0},
    {NVN_FORMAT_RGTC2_UNORM, 0, 0},
    {NVN_FORMAT_RGTC2_SNORM, 0, 0},
    {NVN_FORMAT_BPTC_UNORM, 0, 0},
    {NVN_FORMAT_BPTC_UNORM_SRGB, 0, 0},
    {NVN_FORMAT_BPTC_SFLOAT, 0, 0},
    {NVN_FORMAT_BPTC_UFLOAT, 0, 0},

};
#undef SW
typedef char gl_nvn_formats_counted[sizeof g_nvn_formats / sizeof *g_nvn_formats == SNX_GL_TABLE_FORMATS ? 1 : -1];

/* the NVN form of shared format index: its format (ASTC's in GL's order, as NVN's are), 0 for none */
static u16 nvn_format(int32_t index, u16* swizzle, u8* expand) {
    *swizzle = 0, *expand = 0;
    if (index < 0 || index >= SNX_GL_FORMATS) return 0;
    if (index >= SNX_GL_TABLE_FORMATS) { /* 14 ASTC block sizes, then their sRGB forms */
        int k = index - SNX_GL_TABLE_FORMATS;
        return (u16)(k < 14 ? NVN_FORMAT_RGBA_ASTC_4x4 + k : NVN_FORMAT_RGBA_ASTC_4x4_SRGB + k - 14);
    }
    *swizzle = g_nvn_formats[index].swizzle, *expand = g_nvn_formats[index].expand;
    return g_nvn_formats[index].format;
}
u16 gl_nvn_format(int32_t index) {
    u16 swizzle;
    u8 expand;
    return nvn_format(index, &swizzle, &expand);
}
/* GL's texture swizzle (GL_TEXTURE_SWIZZLE_R to A) over the format's own (fs, 0 for the identity): NVN's view swizzle,
   0 where both are the identity. A one-channel format of GL's (a font atlas's RG8 read as luminance and alpha) shows
   its channels where the game asks */
static u16 texture_swizzle(u16 fs, const uint32_t gl[4]) {
    u16 own[4] = {NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A}, out = 0;
    int identity = !(fs & NVN_TEX_SWIZZLE_SET);
    for (int c = 0; c < 4; c++)
        if (fs & NVN_TEX_SWIZZLE_SET) own[c] = (u16)(fs >> (3 * c) & 7);
    for (int c = 0; c < 4; c++) {
        u16 v;
        switch (gl[c]) {
            case 0x1903: v = own[0]; break; /* GL_RED */
            case 0x1904: v = own[1]; break; /* GL_GREEN */
            case 0x1905: v = own[2]; break; /* GL_BLUE */
            case 0x1906: v = own[3]; break; /* GL_ALPHA */
            case 0: v = NVN_TEXTURE_SWIZZLE_ZERO; break;
            case 1: v = NVN_TEXTURE_SWIZZLE_ONE; break;
            default: v = own[c];
        }
        if (v != (u16)(NVN_TEXTURE_SWIZZLE_R + c)) identity = 0;
        out |= (u16)(v << (3 * c));
    }
    return identity ? 0 : (u16)(out | NVN_TEX_SWIZZLE_SET);
}
int gl_nvn_texture(u32 name, NvnTexture* out) {
    static const u8 targets[SNX_GL_TARGETS] = {NVN_TEXTURE_TARGET_2D, NVN_TEXTURE_TARGET_CUBEMAP, NVN_TEXTURE_TARGET_3D,
                                               NVN_TEXTURE_TARGET_2D_ARRAY, NVN_TEXTURE_TARGET_2D_MULTISAMPLE};
    const SnxGlTexture* t = snx_gl_texture(name);
    u16 swizzle;
    u8 expand;
    u16 format = t && t->target < SNX_GL_TARGETS ? nvn_format(t->format, &swizzle, &expand) : 0;
    *out = (NvnTexture){0};
    if (!format) return 0;
    out->magic = NVN_MAGIC('T', 'E', 'X', 0);
    out->d = (NvnTexDesc){.format = format,
                          .swizzle = texture_swizzle(swizzle, t->swizzles),
                          .target = targets[t->target],
                          .samples = (u8)(t->samples > 1 ? t->samples : 0),
                          .levels = (u8)t->levels,
                          .width = (u32)t->width,
                          .height = (u32)t->height,
                          .depth = (u32)(t->target == SNX_GL_T3D ? t->depth : t->layers)};
    out->memory = t->texels, out->size = t->texels_size;
    out->gpu = t->texels ? gl_gpu_address(t->texels) : 0;
    return 1;
}
/* the bytes a texture's storage takes in NVN's layout, at its alignment; 0 for a format NVN has no form of (said) */
static uint64_t storage_bytes(uint32_t name, uint64_t* align) {
    NvnTexture n;
    NvnTexStorage st;
    if (!gl_nvn_texture(name, &n)) {
        const SnxGlFormat* f = snx_gl_format_at(snx_gl_texture(name)->format);
        LOG_LIMIT(8, "gl layer: texture %u's format 0x%x has no NVN form here: it keeps no texels\n", (unsigned)name,
                  f ? (unsigned)f->internal : 0u);
        return 0;
    }
    nvn_tex_storage(&n.d, &st);
    *align = st.align > 256 ? st.align : 256;
    return st.size;
}

/* an upload in flight to the GPU thread: the texture as NVN describes it, the level's region, and the client's bytes
   as they were at the call, rows row bytes apart and slices image bytes apart */
typedef struct {
    NvnTexture t;
    NvnView view;
    NVNcopyRegion region;
    i64 row, image;
    u64 serial;
    u8 bytes[];
} TexelUpload;
static void upload_on_gpu(void* arg) {
    TexelUpload* w = arg;
    if (!nvn_copy_texels_in(&w->t, &w->view, &w->region, w->bytes, w->row, w->image))
        LOG_LIMIT(4, "gl layer: an upload into a texture of NVN format %#x the runtime can't lay out\n", w->t.d.format);
    gl_draw_done(w->serial);
    free(w);
}
/* the client's texels into the texture's storage, by the GPU in order with the draws: as they are, red and blue
   swapped (GL_BGRA into RGBA), or each 3-byte texel made 4 (alpha one) for a format NVN keeps in 4 */
static void upload(uint32_t ctx, uint32_t name, const SnxGlUpload* u) {
    NvnTexture n;
    u16 swizzle;
    u8 expand;
    u64 per = u->row, size;
    TexelUpload* w;
    (void)ctx;
    if (!gl_nvn_texture(name, &n) || !n.gpu) return;
    nvn_format(snx_gl_texture(name)->format, &swizzle, &expand);
    if (expand) per = u->row / 3 * 4;
    size = per * u->rows * (u64)u->d;
    w = malloc(sizeof *w + (size_t)size);
    if (!w) fatal("gl layer: out of memory for an upload of %llu bytes", (unsigned long long)size);
    *w = (TexelUpload){.t = n,
                       .view = {.set = NVN_VIEW_LEVELS, .base_level = (u8)u->level, .levels = 1},
                       .region = {u->x, u->y, u->z, u->w, u->h, u->d},
                       .row = (i64)per,
                       .image = (i64)(per * u->rows)};
    for (int32_t k = 0; k < u->d; k++)
        for (u64 r = 0; r < u->rows; r++) {
            const u8* from = u->src + (u64)k * u->image + r * u->stride;
            u8* to = w->bytes + ((u64)k * u->rows + r) * per;
            if (expand)
                for (u64 i = 0, o = 0; i + 3 <= u->row; i += 3, o += 4)
                    to[o] = from[i], to[o + 1] = from[i + 1], to[o + 2] = from[i + 2], to[o + 3] = 0xff;
            else if (u->swap_rb)
                for (u64 i = 0; i + 4 <= u->row; i += 4)
                    to[i] = from[i + 2], to[i + 1] = from[i + 1], to[i + 2] = from[i], to[i + 3] = from[i + 3];
            else
                memcpy(to, from, (size_t)u->row);
        }
    w->serial = gl_draw_issue();
    gpu_call(upload_on_gpu, w, NULL, 0);
}

/* glGenerateMipmap: the levels past the base level, each filtered from the one above it by the GPU */
typedef struct {
    NvnTexture t;
    u32 base, top;
    u64 serial;
} LevelsMade;
static void mipmaps_on_gpu(void* arg) {
    LevelsMade* m = arg;
    nvn_copy_levels(&m->t, m->base, m->top);
    gl_draw_done(m->serial);
    free(m);
}
static int mipmaps(uint32_t ctx, uint32_t name) {
    const SnxGlTexture* t = snx_gl_texture(name);
    LevelsMade* m;
    (void)ctx;
    if (!t || !t->texels) return 0;
    m = malloc(sizeof *m);
    if (!m) fatal("gl layer: out of memory for glGenerateMipmap");
    if (!gl_nvn_texture(name, &m->t)) {
        free(m);
        return 0;
    }
    m->base = (u32)t->base_level, m->top = (u32)t->levels - 1;
    m->serial = gl_draw_issue();
    gpu_call(mipmaps_on_gpu, m, NULL, 0);
    return 1;
}

void gl_texture_calls_init(void) {
    snx_gl_texture_host(&(SnxGlTextureHost){
        .upload = upload, .mipmaps = mipmaps, .say = say, .keeps_texels = 1, .storage_bytes = storage_bytes});
}

#define LOCKED(fallback, ...)                                                                                          \
    do {                                                                                                               \
        u32 ctx = gl_egl_context();                                                                                    \
        (void)c;                                                                                                       \
        if (!ctx) return fallback;                                                                                     \
        gl_object_calls_lock();                                                                                        \
        __VA_ARGS__;                                                                                                   \
        gl_object_calls_unlock();                                                                                      \
    } while (0)
#define NONE

static void* host(u64 a) { return a ? GP(a) : NULL; }
/* a parameter's value from the game's pointer: an integer one's, or a float's */
static float int_at(u64 a) {
    i32 v = 0;
    memcpy(&v, GP(a), 4);
    return (float)v;
}
static float float_at(u64 a) {
    float v = 0;
    memcpy(&v, GP(a), 4);
    return v;
}
static void put_int(u64 a, float v) {
    i32 x = (i32)v;
    memcpy(GP(a), &x, 4);
}
static void put_float(u64 a, float v) { memcpy(GP(a), &v, 4); }

/* ---- textures ---- */

void gl_gen_textures(CPU* c, i32 n, u64 textures) { LOCKED(NONE, snx_gl_gen_textures(ctx, n, host(textures))); }
void gl_delete_textures(CPU* c, i32 n, u64 textures) { LOCKED(NONE, snx_gl_delete_textures(ctx, n, host(textures))); }
u8 gl_is_texture(CPU* c, u32 texture) {
    u8 is = 0;
    LOCKED(0, is = (u8)snx_gl_is_texture(texture));
    return is;
}
void gl_bind_texture(CPU* c, u32 target, u32 texture) { LOCKED(NONE, snx_gl_bind_texture(ctx, target, texture)); }
void gl_active_texture(CPU* c, u32 texture) { LOCKED(NONE, snx_gl_active_texture(ctx, texture)); }
void gl_tex_image_2d(CPU* c, u32 target, i32 level, u32 internal, i32 width, i32 height, i32 border, u32 format,
                     u32 type, u64 pixels) {
    (void)border;
    LOCKED(NONE, snx_gl_tex_image(ctx, target, level, internal, width, height, 1, format, type, pixels, 0, 0));
}
void gl_tex_image_3d(CPU* c, u32 target, i32 level, u32 internal, i32 width, i32 height, i32 depth, i32 border,
                     u32 format, u32 type, u64 pixels) {
    (void)border;
    LOCKED(NONE, snx_gl_tex_image(ctx, target, level, internal, width, height, depth, format, type, pixels, 0, 0));
}
void gl_tex_sub_image_2d(CPU* c, u32 target, i32 level, i32 x, i32 y, i32 width, i32 height, u32 format, u32 type,
                         u64 pixels) {
    LOCKED(NONE, snx_gl_tex_sub_image(ctx, target, level, x, y, 0, width, height, 1, format, type, pixels, 0, 0));
}
void gl_tex_sub_image_3d(CPU* c, u32 target, i32 level, i32 x, i32 y, i32 z, i32 width, i32 height, i32 depth,
                         u32 format, u32 type, u64 pixels) {
    LOCKED(NONE, snx_gl_tex_sub_image(ctx, target, level, x, y, z, width, height, depth, format, type, pixels, 0, 0));
}
void gl_compressed_tex_image_2d(CPU* c, u32 target, i32 level, u32 internal, i32 width, i32 height, i32 border,
                                i32 size, u64 data) {
    (void)border;
    LOCKED(NONE, snx_gl_tex_image(ctx, target, level, internal, width, height, 1, 0, 0, data, 1, (u32)size));
}
void gl_compressed_tex_image_3d(CPU* c, u32 target, i32 level, u32 internal, i32 width, i32 height, i32 depth,
                                i32 border, i32 size, u64 data) {
    (void)border;
    LOCKED(NONE, snx_gl_tex_image(ctx, target, level, internal, width, height, depth, 0, 0, data, 1, (u32)size));
}
void gl_compressed_tex_sub_image_2d(CPU* c, u32 target, i32 level, i32 x, i32 y, i32 width, i32 height, u32 format,
                                    i32 size, u64 data) {
    (void)format;
    LOCKED(NONE, snx_gl_tex_sub_image(ctx, target, level, x, y, 0, width, height, 1, 0, 0, data, 1, (u32)size));
}
void gl_compressed_tex_sub_image_3d(CPU* c, u32 target, i32 level, i32 x, i32 y, i32 z, i32 width, i32 height,
                                    i32 depth, u32 format, i32 size, u64 data) {
    (void)format;
    LOCKED(NONE, snx_gl_tex_sub_image(ctx, target, level, x, y, z, width, height, depth, 0, 0, data, 1, (u32)size));
}
void gl_tex_storage_2d(CPU* c, u32 target, i32 levels, u32 internal, i32 width, i32 height) {
    LOCKED(NONE, snx_gl_tex_storage(ctx, target, levels, internal, width, height, 1));
}
void gl_tex_storage_3d(CPU* c, u32 target, i32 levels, u32 internal, i32 width, i32 height, i32 depth) {
    LOCKED(NONE, snx_gl_tex_storage(ctx, target, levels, internal, width, height, depth));
}
void gl_tex_storage_2d_multisample(CPU* c, u32 target, i32 samples, u32 internal, i32 width, i32 height, u8 fixed) {
    (void)fixed;
    LOCKED(NONE, snx_gl_tex_storage_multisample(ctx, target, samples, internal, width, height));
}
void gl_tex_parameteri(CPU* c, u32 target, u32 pname, i32 param) {
    LOCKED(NONE, snx_gl_tex_parameter(ctx, target, pname, (float)param));
}
void gl_tex_parameterf(CPU* c, u32 target, u32 pname, f32 param) {
    LOCKED(NONE, snx_gl_tex_parameter(ctx, target, pname, param));
}
void gl_tex_parameteriv(CPU* c, u32 target, u32 pname, u64 params) {
    LOCKED(NONE, snx_gl_tex_parameter(ctx, target, pname, int_at(params)));
}
void gl_tex_parameterfv(CPU* c, u32 target, u32 pname, u64 params) {
    LOCKED(NONE, snx_gl_tex_parameter(ctx, target, pname, float_at(params)));
}
void gl_get_tex_parameteriv(CPU* c, u32 target, u32 pname, u64 params) {
    float v = 0;
    LOCKED(NONE, if (snx_gl_get_tex_parameter(ctx, target, pname, &v)) put_int(params, v));
}
void gl_get_tex_parameterfv(CPU* c, u32 target, u32 pname, u64 params) {
    float v = 0;
    LOCKED(NONE, if (snx_gl_get_tex_parameter(ctx, target, pname, &v)) put_float(params, v));
}
void gl_get_tex_level_parameteriv(CPU* c, u32 target, i32 level, u32 pname, u64 params) {
    float v = 0;
    LOCKED(NONE, if (snx_gl_get_tex_level_parameter(ctx, target, level, pname, &v)) put_int(params, v));
}
void gl_get_tex_level_parameterfv(CPU* c, u32 target, i32 level, u32 pname, u64 params) {
    float v = 0;
    LOCKED(NONE, if (snx_gl_get_tex_level_parameter(ctx, target, level, pname, &v)) put_float(params, v));
}
void gl_generate_mipmap(CPU* c, u32 target) { LOCKED(NONE, snx_gl_generate_mipmap(ctx, target)); }
void gl_bind_image_texture(CPU* c, u32 unit, u32 texture, i32 level, u8 layered, i32 layer, u32 access, u32 format) {
    LOCKED(NONE, snx_gl_bind_image_texture(ctx, unit, texture, level, layered, layer, access, format));
}

/* ---- samplers ---- */

void gl_gen_samplers(CPU* c, i32 n, u64 samplers) { LOCKED(NONE, snx_gl_gen_samplers(ctx, n, host(samplers))); }
void gl_delete_samplers(CPU* c, i32 n, u64 samplers) { LOCKED(NONE, snx_gl_delete_samplers(ctx, n, host(samplers))); }
u8 gl_is_sampler(CPU* c, u32 sampler) {
    u8 is = 0;
    LOCKED(0, is = (u8)snx_gl_is_sampler(sampler));
    return is;
}
void gl_bind_sampler(CPU* c, u32 unit, u32 sampler) { LOCKED(NONE, snx_gl_bind_sampler(ctx, unit, sampler)); }
void gl_sampler_parameteri(CPU* c, u32 sampler, u32 pname, i32 param) {
    LOCKED(NONE, snx_gl_sampler_parameter(ctx, sampler, pname, (float)param));
}
void gl_sampler_parameterf(CPU* c, u32 sampler, u32 pname, f32 param) {
    LOCKED(NONE, snx_gl_sampler_parameter(ctx, sampler, pname, param));
}
void gl_sampler_parameteriv(CPU* c, u32 sampler, u32 pname, u64 params) {
    LOCKED(NONE, snx_gl_sampler_parameter(ctx, sampler, pname, int_at(params)));
}
void gl_sampler_parameterfv(CPU* c, u32 sampler, u32 pname, u64 params) {
    LOCKED(NONE, snx_gl_sampler_parameter(ctx, sampler, pname, float_at(params)));
}
void gl_get_sampler_parameteriv(CPU* c, u32 sampler, u32 pname, u64 params) {
    float v = 0;
    LOCKED(NONE, if (snx_gl_get_sampler_parameter(ctx, sampler, pname, &v)) put_int(params, v));
}
void gl_get_sampler_parameterfv(CPU* c, u32 sampler, u32 pname, u64 params) {
    float v = 0;
    LOCKED(NONE, if (snx_gl_get_sampler_parameter(ctx, sampler, pname, &v)) put_float(params, v));
}
