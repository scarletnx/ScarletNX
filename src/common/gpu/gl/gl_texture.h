/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's textures and samplers (gl_state.h's layer), shared by the Android bridge and the console.

   A texture is a flat record: its target, its format (gl_format.h's index), its size, levels and layers, the levels
   that have texels, its sampler state and its other parameters. Each context binds a texture to each target of each
   unit, a sampler to each unit, and textures to its image units. A platform's image of a texture is its own, made
   when the layer says the texture's storage is new (its generation moved) and filled from what the layer hands it of
   each upload. A platform that asks for it (the console, whose save state must hold them) also has the layer keep
   each texture's texels in platform memory (gl_object.h's), level after level, each a layer's tight rows after the
   other's. The records are one block a save state holds as it is; the caller holds its platform's GL lock */
#ifndef SNX_GL_TEXTURE_H
#define SNX_GL_TEXTURE_H
#include "common/gpu/gl/gl_program.h"
#include <stddef.h>
#include <stdint.h>

#define SNX_GL_TEXTURES    4096 /* texture names: a game made 1267 */
#define SNX_GL_SAMPLERS    256
#define SNX_GL_UNITS       32 /* texture image units, GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */
#define SNX_GL_TARGETS     5  /* a unit's targets: SNX_GL_T2D, TCUBE, T3D, TARRAY, TMS */
#define SNX_GL_IMAGE_UNITS 8  /* GL_MAX_IMAGE_UNITS */
#define SNX_GL_LEVELS      16

typedef struct {
    uint32_t min, mag, wrap[3], compare_mode, compare_func;
    float min_lod, max_lod, anisotropy;
    float lod_bias; /* a desktop context's GL_TEXTURE_LOD_BIAS */
} SnxGlSamplerState;

typedef struct {
    uint32_t used;
    SnxGlSamplerState state;
} SnxGlSampler;

typedef struct {
    uint8_t used;
    uint8_t target; /* SNX_GL_T*, 0xff until its first bind */
    uint8_t immutable, stencil_mode;
    int32_t format;         /* gl_format.h's index, -1 for none yet */
    int32_t width, height;  /* the base level's */
    int32_t depth;          /* a 3D texture's, else 1 */
    int32_t levels, layers; /* layers: a cube map's 6, an array's count, else 1 */
    int32_t samples;        /* a multisampled texture's, else 1 */
    uint32_t defined;       /* the levels that have texels */
    uint32_t generation;    /* counts the storages made: a platform's image of an older one is stale */
    int32_t base_level, max_level;
    uint32_t swizzles[4]; /* GL_TEXTURE_SWIZZLE_R to A */
    SnxGlSamplerState sampler;
    uint32_t zero;                /* keeps the record free of padding, which a save state would hold as it found it */
    uint64_t texels, texels_size; /* the platform memory its texels are kept in, where the platform has them kept */
} SnxGlTexture;

typedef struct {
    uint32_t texture;
    int32_t level, layer;
    uint32_t layered, access, format;
} SnxGlImageUnit;

/* what a context binds of textures and samplers */
typedef struct {
    uint32_t active; /* the active unit, from 0 */
    uint32_t textures[SNX_GL_UNITS][SNX_GL_TARGETS];
    uint32_t samplers[SNX_GL_UNITS];
    SnxGlImageUnit images[SNX_GL_IMAGE_UNITS];
} SnxGlTextureContext;

/* an upload of the client's texels to a box of a level: src at the box's first texel, row bytes of each row, rows
   rows of each image, stride bytes from a row to the next, image bytes from an image to the next, d images (a cube
   map's face, an array's layers from z); a compressed one's rows are rows of blocks */
typedef struct {
    int32_t level, x, y, z, w, h, d;
    const uint8_t* src;
    uint64_t row, rows, stride, image;
    int compressed;
    int swap_rb; /* the client's texels are GL_BGRA for the texture's RGBA bytes: its red and blue the other way */
} SnxGlUpload;

/* What a platform hears of the textures; each may be NULL. Not part of a save state: given again after a load */
typedef struct {
    /* the texture's storage made anew (generation moved): 1 when the platform made its image, 0 when it has none
       (the platform records its own error, if any); the texture then has no storage */
    int (*storage)(uint32_t ctx, uint32_t texture);
    /* the client's texels for the texture */
    void (*upload)(uint32_t ctx, uint32_t texture, const SnxGlUpload* u);
    /* glGenerateMipmap: the levels past the base level made from it, 1 when they were (the layer marks them
       defined); without it the layer marks them, for the platform to make when it reads them */
    int (*mipmaps)(uint32_t ctx, uint32_t texture);
    /* the texture bound to an image unit */
    void (*image_bound)(uint32_t ctx, uint32_t texture);
    /* the texture deleted: the platform's image goes */
    void (*gone)(uint32_t texture);
    /* a level of a size not its base level's (left out), a format the layer has not: said */
    void (*say)(const char* line);
    uint32_t (*max_size)(void); /* the largest width and height the platform makes now; without it 16384 */
    int keeps_texels;           /* the layer keeps each texture's texels in platform memory, for the platform to read */
    /* With keeps_texels: the bytes the texture's storage takes in the platform's own layout (the console's: the GPU's
       block-linear one), and in *align their alignment, the record's sizes just set; the layer takes that memory and
       the platform's upload writes the texels there. Without it, the layer keeps them itself as tight rows, level
       after level */
    uint64_t (*storage_bytes)(uint32_t texture, uint64_t* align);
} SnxGlTextureHost;
void snx_gl_texture_host(const SnxGlTextureHost* host);

/* a context made anew: unit 0 active, nothing bound */
void snx_gl_texture_context_reset(uint32_t ctx);
SnxGlTextureContext* snx_gl_textures(uint32_t ctx);
SnxGlTexture* snx_gl_texture(uint32_t name);
SnxGlSampler* snx_gl_sampler(uint32_t name);
/* the texture bound to a target index (SNX_GL_T*) of a unit, 0 for none */
uint32_t snx_gl_unit_texture(uint32_t ctx, uint32_t unit, uint32_t target);
/* the sampler state a unit reads its texture with: its sampler object's, else the texture's own */
const SnxGlSamplerState* snx_gl_unit_sampler(uint32_t ctx, uint32_t unit, uint32_t texture);
/* whether a sampler reads the texture: its base level has texels; *levels from it that have, up to the max level */
int snx_gl_texture_complete(uint32_t texture, int32_t* levels);
/* glGet*'s active unit and its texture and sampler bindings of pname into v[0]: 1, or 0 for a pname not of them */
int snx_gl_texture_get(uint32_t ctx, uint32_t pname, double v[4]);
/* a level of the texture written by the platform itself (a copy, a blit into it): it has texels now */
void snx_gl_texture_defined(uint32_t texture, int32_t level);
/* a level's size from the base level's */
static inline int32_t snx_gl_level_size(int32_t size, int32_t level) {
    int32_t s = size >> level;
    return s > 0 ? s : 1;
}

/* GL's calls. Pixels are the client's: an offset into the pixel unpack buffer when one is bound, else the client's
   address, which gl_object.h's platform turns into a host pointer */
void snx_gl_gen_textures(uint32_t ctx, int32_t n, uint32_t* out);
void snx_gl_delete_textures(uint32_t ctx, int32_t n, const uint32_t* names);
int snx_gl_is_texture(uint32_t name);
void snx_gl_bind_texture(uint32_t ctx, uint32_t target, uint32_t name);
void snx_gl_active_texture(uint32_t ctx, uint32_t unit);
/* glTexImage2D/3D and the compressed ones (csize: the client's bytes of blocks) */
void snx_gl_tex_image(uint32_t ctx, uint32_t target, int32_t level, uint32_t internal, int32_t w, int32_t h, int32_t d,
                      uint32_t format, uint32_t type, uint64_t pixels, int compressed, uint64_t csize);
void snx_gl_tex_sub_image(uint32_t ctx, uint32_t target, int32_t level, int32_t x, int32_t y, int32_t z, int32_t w,
                          int32_t h, int32_t d, uint32_t format, uint32_t type, uint64_t pixels, int compressed,
                          uint64_t csize);
/* glTexStorage2D (d 1) and glTexStorage3D, and glTexStorage2DMultisample */
void snx_gl_tex_storage(uint32_t ctx, uint32_t target, int32_t levels, uint32_t internal, int32_t w, int32_t h,
                        int32_t d);
void snx_gl_tex_storage_multisample(uint32_t ctx, uint32_t target, int32_t samples, uint32_t internal, int32_t w,
                                    int32_t h);
/* glCopyTexImage2D's storage: the level made as the format (an unsized one as 4 bytes a texel) and size give, and
   marked; the texture's name, for the platform to copy the read framebuffer into, or 0 (an error recorded, or an
   empty rectangle) */
uint32_t snx_gl_copy_tex_image(uint32_t ctx, uint32_t target, int32_t level, uint32_t internal, int32_t w, int32_t h);
void snx_gl_tex_parameter(uint32_t ctx, uint32_t target, uint32_t pname, float v);
/* glGetTexParameter*, glGetTexLevelParameter*: 1 with the value in *v, 0 with the error recorded */
int snx_gl_get_tex_parameter(uint32_t ctx, uint32_t target, uint32_t pname, float* v);
int snx_gl_get_tex_level_parameter(uint32_t ctx, uint32_t target, int32_t level, uint32_t pname, float* v);
void snx_gl_generate_mipmap(uint32_t ctx, uint32_t target);
void snx_gl_bind_image_texture(uint32_t ctx, uint32_t unit, uint32_t texture, int32_t level, int layered, int32_t layer,
                               uint32_t access, uint32_t format);

void snx_gl_gen_samplers(uint32_t ctx, int32_t n, uint32_t* out);
void snx_gl_delete_samplers(uint32_t ctx, int32_t n, const uint32_t* names);
int snx_gl_is_sampler(uint32_t name);
void snx_gl_bind_sampler(uint32_t ctx, uint32_t unit, uint32_t sampler);
void snx_gl_sampler_parameter(uint32_t ctx, uint32_t sampler, uint32_t pname, float v);
int snx_gl_get_sampler_parameter(uint32_t ctx, uint32_t sampler, uint32_t pname, float* v);

/* the save state's part (after the buffers'): its bytes, a copy, the check (why names the field and its value) and
   its load. Kept texels are in platform memory, which the console's save state holds */
size_t snx_gl_texture_state_bytes(void);
void snx_gl_texture_state_save(void* out);
int snx_gl_texture_state_check(const void* in, size_t size, char* why, size_t whysize);
void snx_gl_texture_state_load(const void* in);

#endif
