/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shared GL layer's textures and samplers (src/common/gpu/gl/gl_texture.c), with no GPU and the layer keeping each
   texture's texels in the platform's memory (a few chunks of the test's), as the console has it: names, targets and
   units; a level's storage made with the base level and a level of another size left out, said; texels read where
   GL's unpack state says (alignment, row length, skips, the pixel unpack buffer) and kept as tight rows, level after
   level; compressed blocks; immutable and multisampled storage; the texture's and sampler objects' parameters (the
   LOD bias only a desktop context's); levels' parameters, completeness and mipmaps; image units; what the platform
   hears (a storage, each upload, mipmaps, an image bound, a texture gone); deletes clearing their bindings and giving
   the texels' memory back; glCopyTexImage2D's storage; and a save state's copy loaded back whole and refused when a
   texture or a binding in it is wrong */
#include "common/gpu/gl/gl_format.h"
#include "common/gpu/gl/gl_object.h"
#include "common/gpu/gl/gl_program.h"
#include "common/gpu/gl/gl_state.h"
#include "common/gpu/gl/gl_texture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    T2D = 0x0de1,
    T3D = 0x806f,
    CUBE = 0x8513,
    CUBE_X = 0x8515,
    ARRAY2D = 0x8c1a,
    MS2D = 0x9100,
    UNIT0 = 0x84c0,
    RGB = 0x1907,
    RGBA = 0x1908,
    RGB8 = 0x8051,
    RGBA8 = 0x8058,
    UBYTE = 0x1401,
    FLOAT = 0x1406,
    BC1 = 0x83f1,
    MIN_FILTER = 0x2801,
    WRAP_S = 0x2802,
    LOD_BIAS = 0x8501,
    BASE_LEVEL = 0x813c,
    MAX_LEVEL = 0x813d,
    SWIZZLE_R = 0x8e42,
    IMMUTABLE_FORMAT = 0x912f,
    IMMUTABLE_LEVELS = 0x82df,
    STENCIL_MODE = 0x90ea,
    WIDTH = 0x1000,
    INTERNAL = 0x1003,
    SAMPLES = 0x9106,
    COMPRESSED = 0x86a1,
    ROW_LENGTH = 0x0cf2,
    SKIP_ROWS = 0x0cf3,
    SKIP_PIXELS = 0x0cf4,
    ALIGNMENT = 0x0cf5,
    UNPACK_BUFFER = 0x88ec,
    NEAREST = 0x2600,
    CLAMP = 0x812f,
};

/* the platform: its chunks, and what it heard */
static struct {
    unsigned storages, uploads, mipmaps, images, gone;
    int refuse_storage, make_mipmaps;
    SnxGlUpload last;
    char said[300];
    uint64_t layout_bytes, layout_align;
} heard;
static uint64_t layout_bytes(uint32_t texture, uint64_t* align) {
    (void)texture;
    *align = heard.layout_align;
    return heard.layout_bytes;
}
static uint64_t chunk(uint64_t size, uint64_t* got) {
    void* p = calloc(1, (size_t)size);
    *got = p ? size : 0;
    return (uint64_t)(uintptr_t)p;
}
static uint8_t* bytes(uint64_t at) { return (uint8_t*)(uintptr_t)at; }
static int storage(uint32_t ctx, uint32_t texture) {
    (void)ctx, (void)texture;
    heard.storages++;
    return !heard.refuse_storage;
}
static void upload(uint32_t ctx, uint32_t texture, const SnxGlUpload* u) {
    (void)ctx, (void)texture;
    heard.uploads++;
    heard.last = *u;
}
static int mipmaps(uint32_t ctx, uint32_t texture) {
    (void)ctx, (void)texture;
    heard.mipmaps++;
    return heard.make_mipmaps;
}
static void image_bound(uint32_t ctx, uint32_t texture) {
    (void)ctx, (void)texture;
    heard.images++;
}
static void gone(uint32_t texture) {
    (void)texture;
    heard.gone++;
}
static void say(const char* line) { snprintf(heard.said, sizeof heard.said, "%s", line); }
static uint32_t max_size(void) { return 4096; }
static void said(const char* text) {
    if (!strstr(heard.said, text)) {
        fprintf(stderr, "gl_texture: said \"%s\", not \"%s\"\n", heard.said, text);
        abort();
    }
    heard.said[0] = 0;
}
static float parameter(uint32_t ctx, uint32_t target, uint32_t pname) {
    float v = -1;
    assert(snx_gl_get_tex_parameter(ctx, target, pname, &v));
    return v;
}
static float level_parameter(uint32_t ctx, uint32_t target, int32_t level, uint32_t pname) {
    float v = -1;
    assert(snx_gl_get_tex_level_parameter(ctx, target, level, pname, &v));
    return v;
}
static const uint8_t* texels(uint32_t name) { return bytes(snx_gl_texture(name)->texels); }
static void refused(const void* copy, size_t size, const char* text) {
    char why[300];
    assert(snx_gl_state_check(copy, size, why, sizeof why) < 0);
    if (!strstr(why, text)) {
        fprintf(stderr, "gl_texture: refused for \"%s\", not naming \"%s\"\n", why, text);
        abort();
    }
}

int main(void) {
    uint32_t ctx = snx_gl_context_new(SNX_GL_API_ES, 32, 0), desktop = snx_gl_context_new(SNX_GL_API_DESKTOP, 45, 0);
    uint32_t names[4] = {0};
    uint8_t pattern[512];
    double v[4];
    assert(ctx && desktop);
    snx_gl_object_host(&(SnxGlObjectHost){.chunk = chunk, .bytes = bytes});
    snx_gl_texture_host(&(SnxGlTextureHost){.storage = storage,
                                            .upload = upload,
                                            .mipmaps = mipmaps,
                                            .image_bound = image_bound,
                                            .gone = gone,
                                            .say = say,
                                            .max_size = max_size,
                                            .keeps_texels = 1});
    for (int i = 0; i < 512; i++) pattern[i] = (uint8_t)(i * 7 + 1);

    /* names: a texture is one once bound; a target it was not bound to, a target GL has not, a name past the table */
    snx_gl_gen_textures(ctx, 3, names);
    assert(names[0] == 1 && names[1] == 2 && names[2] == 3 && !snx_gl_is_texture(1));
    snx_gl_bind_texture(ctx, T2D, 1);
    assert(snx_gl_is_texture(1) && snx_gl_get(ctx, 0x8069, v) == 1 && v[0] == 1);
    snx_gl_bind_texture(ctx, CUBE, 1);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION && snx_gl_unit_texture(ctx, 0, SNX_GL_TCUBE) == 0);
    snx_gl_bind_texture(ctx, 0x1234, 1);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
    snx_gl_bind_texture(ctx, T2D, SNX_GL_TEXTURES + 1);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE && snx_gl_unit_texture(ctx, 0, SNX_GL_T2D) == 1);
    snx_gl_bind_texture(ctx, T2D, 40); /* GLES makes a texture of a name its first bind gives */
    assert(snx_gl_is_texture(40) && snx_gl_unit_texture(ctx, 0, SNX_GL_T2D) == 40);
    /* units: each its own bindings */
    snx_gl_active_texture(ctx, UNIT0 + 3);
    assert(snx_gl_get(ctx, 0x84e0, v) == 1 && v[0] == UNIT0 + 3 && snx_gl_get(ctx, 0x8069, v) == 1 && v[0] == 0);
    snx_gl_active_texture(ctx, UNIT0 + SNX_GL_UNITS);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM && snx_gl_textures(ctx)->active == 3);
    snx_gl_active_texture(ctx, UNIT0);
    snx_gl_bind_texture(ctx, T2D, 1);
    /* texture 0, GL's default texture, which nothing here has */
    snx_gl_bind_texture(ctx, T3D, 0);
    snx_gl_tex_image(ctx, T3D, 0, RGBA8, 1, 1, 1, RGBA, UBYTE, 0, 0, 0);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);

    /* the base level makes the whole chain of levels, its texels kept as tight rows */
    snx_gl_tex_image(ctx, T2D, 0, RGBA8, 4, 4, 1, RGBA, UBYTE, (uint64_t)(uintptr_t)pattern, 0, 0);
    {
        const SnxGlTexture* t = snx_gl_texture(1);
        assert(heard.storages == 1 && t->levels == 3 && t->width == 4 && t->defined == 1 && t->generation == 1);
        assert(t->format == snx_gl_format(RGBA8, 0, 0) && t->texels && t->texels_size == 64 + 16 + 4);
        assert(heard.uploads == 1 && heard.last.row == 16 && heard.last.rows == 4 && heard.last.stride == 16);
        assert(heard.last.src == pattern && !memcmp(texels(1), pattern, 64));
    }
    /* a level the base level's size gives: the same storage; one of another size: left out, said with both sizes */
    snx_gl_tex_image(ctx, T2D, 1, RGBA8, 2, 2, 1, RGBA, UBYTE, (uint64_t)(uintptr_t)(pattern + 100), 0, 0);
    assert(heard.storages == 1 && snx_gl_texture(1)->defined == 3 && !memcmp(texels(1) + 64, pattern + 100, 16));
    snx_gl_tex_image(ctx, T2D, 2, RGBA8, 3, 3, 1, RGBA, UBYTE, (uint64_t)(uintptr_t)pattern, 0, 0);
    said("a level of 3x3 is not level 2 of the base level's 4x4");
    assert(snx_gl_texture(1)->defined == 3 && heard.uploads == 2);
    /* a level past GL's, a size past the platform's */
    snx_gl_tex_image(ctx, T2D, SNX_GL_LEVELS, RGBA8, 1, 1, 1, RGBA, UBYTE, 0, 0, 0);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE);
    snx_gl_tex_image(ctx, T2D, 0, RGBA8, 4097, 1, 1, RGBA, UBYTE, 0, 0, 0);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE && snx_gl_texture(1)->width == 4);
    /* texels of another type than the format's own: said, and the texture keeps no texels from them */
    snx_gl_tex_sub_image(ctx, T2D, 0, 0, 0, 0, 1, 1, 1, RGBA, FLOAT, (uint64_t)(uintptr_t)pattern, 0, 0);
    said("texels of 0x1908/0x1406 into a texture of 0x8058, whose own are 0x1908/0x1401");
    assert(heard.uploads == 2);
    /* a box: the client's rows by the unpack state, the box's rows into the kept level */
    snx_gl_pixel_store(ctx, ROW_LENGTH, 8);
    snx_gl_pixel_store(ctx, SKIP_ROWS, 1);
    snx_gl_pixel_store(ctx, SKIP_PIXELS, 2);
    snx_gl_tex_sub_image(ctx, T2D, 0, 1, 2, 0, 2, 2, 1, RGBA, UBYTE, (uint64_t)(uintptr_t)pattern, 0, 0);
    assert(heard.uploads == 3 && heard.last.x == 1 && heard.last.y == 2 && heard.last.row == 8);
    assert(heard.last.stride == 32 && heard.last.src == pattern + 32 + 8);
    assert(!memcmp(texels(1) + 2 * 16 + 4, pattern + 40, 8) && !memcmp(texels(1) + 3 * 16 + 4, pattern + 72, 8));
    assert(!memcmp(texels(1) + 2 * 16, pattern + 32, 4)); /* the texel left of the box as it was */
    snx_gl_pixel_store(ctx, ROW_LENGTH, 0);
    snx_gl_pixel_store(ctx, SKIP_ROWS, 0);
    snx_gl_pixel_store(ctx, SKIP_PIXELS, 0);
    snx_gl_tex_sub_image(ctx, T2D, 1, 1, 1, 0, 2, 1, 1, RGBA, UBYTE, (uint64_t)(uintptr_t)pattern, 0, 0);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE); /* past level 1's 2x2 */
    /* 3-byte texels: each client row padded to the unpack alignment, kept tight */
    snx_gl_bind_texture(ctx, T2D, 2);
    snx_gl_tex_image(ctx, T2D, 0, RGB8, 3, 2, 1, RGB, UBYTE, (uint64_t)(uintptr_t)pattern, 0, 0);
    assert(heard.last.row == 9 && heard.last.stride == 12 && !memcmp(texels(2), pattern, 9));
    assert(!memcmp(texels(2) + 9, pattern + 12, 9));
    snx_gl_pixel_store(ctx, ALIGNMENT, 1);
    snx_gl_tex_sub_image(ctx, T2D, 0, 0, 0, 0, 3, 2, 1, RGB, UBYTE, (uint64_t)(uintptr_t)pattern, 0, 0);
    assert(heard.last.stride == 9 && !memcmp(texels(2), pattern, 18));
    snx_gl_pixel_store(ctx, ALIGNMENT, 4);
    /* the pixel unpack buffer: pixels an offset into it, one past its bytes refused */
    {
        uint32_t b = 0;
        snx_gl_gen_buffers(ctx, 1, &b);
        snx_gl_bind_buffer(ctx, UNPACK_BUFFER, b);
        snx_gl_buffer_data(ctx, b, 64, pattern + 200, 0x88e4);
        snx_gl_bind_texture(ctx, T2D, 1);
        snx_gl_tex_sub_image(ctx, T2D, 1, 0, 0, 0, 2, 2, 1, RGBA, UBYTE, 16, 0, 0);
        assert(heard.last.src == snx_gl_buffer_bytes(b) + 16 && !memcmp(texels(1) + 64, pattern + 216, 16));
        snx_gl_tex_sub_image(ctx, T2D, 1, 0, 0, 0, 2, 2, 1, RGBA, UBYTE, 49, 0, 0); /* 16 bytes from 49: past 64 */
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION && heard.last.src == snx_gl_buffer_bytes(b) + 16);
        snx_gl_tex_sub_image(ctx, T2D, 1, 0, 0, 0, 2, 2, 1, RGBA, UBYTE, 48, 0, 0);
        assert(!snx_gl_get_error(ctx) && heard.last.src == snx_gl_buffer_bytes(b) + 48);
        assert(!memcmp(texels(1) + 64, pattern + 248, 16));
        snx_gl_bind_buffer(ctx, UNPACK_BUFFER, 0);
    }

    /* compressed blocks: kept as they come; fewer bytes than the blocks refused; blocks into a texture of texels */
    snx_gl_bind_texture(ctx, T2D, 3);
    snx_gl_tex_image(ctx, T2D, 0, BC1, 8, 8, 1, 0, 0, (uint64_t)(uintptr_t)pattern, 1, 32);
    assert(snx_gl_texture(3)->format == snx_gl_format(BC1, 0, 0) && heard.last.compressed && heard.last.row == 16);
    assert(heard.last.rows == 2 && !memcmp(texels(3), pattern, 32) && level_parameter(ctx, T2D, 0, COMPRESSED) == 1);
    snx_gl_tex_sub_image(ctx, T2D, 0, 4, 4, 0, 4, 4, 1, 0, 0, (uint64_t)(uintptr_t)(pattern + 300), 1, 8);
    assert(!memcmp(texels(3) + 24, pattern + 300, 8) && !memcmp(texels(3), pattern, 24));
    snx_gl_tex_sub_image(ctx, T2D, 0, 0, 0, 0, 8, 8, 1, 0, 0, (uint64_t)(uintptr_t)pattern, 1, 16);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE);
    snx_gl_bind_texture(ctx, T2D, 1);
    snx_gl_tex_sub_image(ctx, T2D, 0, 0, 0, 0, 4, 4, 1, 0, 0, (uint64_t)(uintptr_t)pattern, 1, 64);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);

    /* immutable storage: every level there, glTexImage and a second storage refused */
    {
        uint32_t s = 0;
        snx_gl_gen_textures(ctx, 1, &s);
        snx_gl_bind_texture(ctx, T2D, s);
        snx_gl_tex_storage(ctx, T2D, 5, RGBA8, 8, 8, 1);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE); /* 8x8 has 4 levels: 5 is past them */
        snx_gl_tex_storage(ctx, T2D, 2, 0x1234, 8, 8, 1);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
        snx_gl_tex_storage(ctx, T2D, 2, RGBA8, 8, 8, 1);
        assert(snx_gl_texture(s)->defined == 3 && parameter(ctx, T2D, IMMUTABLE_FORMAT) == 1);
        assert(parameter(ctx, T2D, IMMUTABLE_LEVELS) == 2 && parameter(ctx, T2D, MAX_LEVEL) == 1);
        assert(snx_gl_texture(s)->texels_size == 256 + 64);
        snx_gl_tex_image(ctx, T2D, 0, RGBA8, 8, 8, 1, RGBA, UBYTE, 0, 0, 0);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);
        snx_gl_tex_storage(ctx, T2D, 1, RGBA8, 8, 8, 1);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);
        /* glCopyTexImage2D into it: refused */
        assert(!snx_gl_copy_tex_image(ctx, T2D, 0, RGBA8, 8, 8) && snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);
    }
    /* multisampled storage: its samples, none kept; no samples, and samples of a texture not multisampled, refused */
    {
        uint32_t m = 0;
        snx_gl_gen_textures(ctx, 1, &m);
        snx_gl_bind_texture(ctx, MS2D, m);
        snx_gl_tex_storage_multisample(ctx, MS2D, 0, RGBA8, 16, 16);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE);
        snx_gl_tex_storage_multisample(ctx, MS2D, 3, RGBA8, 16, 16);
        assert(snx_gl_texture(m)->samples == 4 && !snx_gl_texture(m)->texels);
        assert(level_parameter(ctx, MS2D, 0, SAMPLES) == 4);
        snx_gl_tex_storage_multisample(ctx, T2D, 4, RGBA8, 16, 16);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
    }
    /* the base level given again at another size: a storage anew, the old texels' memory taken by the next */
    {
        uint32_t r[3] = {0};
        uint64_t was;
        snx_gl_gen_textures(ctx, 3, r);
        snx_gl_bind_texture(ctx, T2D, r[0]);
        snx_gl_tex_image(ctx, T2D, 0, RGBA8, 4, 4, 1, RGBA, UBYTE, 0, 0, 0);
        was = snx_gl_texture(r[0])->texels;
        snx_gl_bind_texture(ctx, T2D, r[1]); /* after it, so that it is not the free end of the memory */
        snx_gl_tex_image(ctx, T2D, 0, RGBA8, 4, 4, 1, RGBA, UBYTE, 0, 0, 0);
        snx_gl_bind_texture(ctx, T2D, r[0]);
        snx_gl_tex_image(ctx, T2D, 0, RGBA8, 16, 16, 1, RGBA, UBYTE, 0, 0, 0);
        assert(snx_gl_texture(r[0])->width == 16 && snx_gl_texture(r[0])->levels == 5);
        assert(snx_gl_texture(r[0])->generation == 2 && snx_gl_texture(r[0])->texels != was);
        snx_gl_bind_texture(ctx, T2D, r[2]);
        snx_gl_tex_image(ctx, T2D, 0, RGBA8, 4, 4, 1, RGBA, UBYTE, 0, 0, 0);
        assert(snx_gl_texture(r[2])->texels == was);
        snx_gl_bind_texture(ctx, T2D, r[0]); /* smaller: fewer levels than it has, a storage anew all the same */
        snx_gl_tex_image(ctx, T2D, 0, RGBA8, 2, 2, 1, RGBA, UBYTE, 0, 0, 0);
        assert(snx_gl_texture(r[0])->width == 2 && snx_gl_texture(r[0])->levels == 2);
    }
    /* an array's layers: the unpack state's images skipped, each layer's rows kept after the other's */
    {
        uint32_t a = 0;
        snx_gl_gen_textures(ctx, 1, &a);
        snx_gl_bind_texture(ctx, ARRAY2D, a);
        snx_gl_pixel_store(ctx, 0x806d, 1); /* GL_UNPACK_SKIP_IMAGES */
        snx_gl_tex_image(ctx, ARRAY2D, 0, RGBA8, 2, 2, 2, RGBA, UBYTE, (uint64_t)(uintptr_t)pattern, 0, 0);
        snx_gl_pixel_store(ctx, 0x806d, 0);
        assert(snx_gl_texture(a)->layers == 2 && heard.last.d == 2 && heard.last.image == 16);
        assert(heard.last.src == pattern + 16 && !memcmp(texels(a), pattern + 16, 32));
    }
    /* a cube map's face: its layer of the kept level */
    {
        uint32_t c = 0;
        snx_gl_gen_textures(ctx, 1, &c);
        snx_gl_bind_texture(ctx, CUBE, c);
        snx_gl_tex_image(ctx, CUBE_X, 0, RGBA8, 2, 2, 1, RGBA, UBYTE, 0, 0, 0);
        snx_gl_tex_image(ctx, CUBE_X + 3, 0, RGBA8, 2, 2, 1, RGBA, UBYTE, (uint64_t)(uintptr_t)pattern, 0, 0);
        assert(snx_gl_texture(c)->layers == 6 && heard.last.z == 3 && heard.last.d == 1);
        assert(!memcmp(texels(c) + 3 * 16, pattern, 16));
    }

    /* the texture's parameters, and GL's errors */
    snx_gl_bind_texture(ctx, T2D, 1);
    snx_gl_tex_parameter(ctx, T2D, MIN_FILTER, NEAREST);
    snx_gl_tex_parameter(ctx, T2D, SWIZZLE_R + 3, 0x1903);
    snx_gl_tex_parameter(ctx, T2D, STENCIL_MODE, 0x1901);
    assert(parameter(ctx, T2D, MIN_FILTER) == NEAREST && parameter(ctx, T2D, SWIZZLE_R + 3) == 0x1903);
    assert(parameter(ctx, T2D, STENCIL_MODE) == 0x1901 && parameter(ctx, T2D, IMMUTABLE_FORMAT) == 0);
    snx_gl_tex_parameter(ctx, T2D, 0x1234, 1);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
    /* GL_BGRA texels: a desktop context takes them into an RGBA texture, red and blue swapped; GLES leaves it empty */
    {
        uint32_t d = 0;
        snx_gl_gen_textures(desktop, 1, &d);
        snx_gl_bind_texture(desktop, T2D, d);
        snx_gl_tex_image(desktop, T2D, 0, RGBA8, 2, 1, 1, 0x80e1, UBYTE, (uint64_t)(uintptr_t)pattern, 0, 0);
        assert(heard.last.swap_rb && texels(d)[0] == pattern[2] && texels(d)[2] == pattern[0]);
        assert(texels(d)[1] == pattern[1] && texels(d)[3] == pattern[3] && texels(d)[4] == pattern[6]);
        snx_gl_tex_sub_image(desktop, T2D, 0, 1, 0, 0, 1, 1, 1, 0x80e1, 0x8367, (uint64_t)(uintptr_t)(pattern + 40), 0,
                             0); /* GL_UNSIGNED_INT_8_8_8_8_REV: the same bytes */
        assert(texels(d)[4] == pattern[42] && texels(d)[6] == pattern[40] && texels(d)[0] == pattern[2]);
        snx_gl_tex_sub_image(desktop, T2D, 0, 0, 0, 0, 1, 1, 1, RGBA, UBYTE, (uint64_t)(uintptr_t)(pattern + 80), 0, 0);
        assert(!heard.last.swap_rb && texels(d)[0] == pattern[80]);
        snx_gl_bind_texture(ctx, T2D, 1);
        {
            unsigned before = heard.uploads; /* the line was said once already, above */
            snx_gl_tex_sub_image(ctx, T2D, 0, 0, 0, 0, 1, 1, 1, 0x80e1, UBYTE, (uint64_t)(uintptr_t)pattern, 0, 0);
            assert(heard.uploads == before);
        }
    }
    /* 8-bit texels of another component order or count: a desktop context converts them into the texture's own, a
       missing alpha one, a missing color zero; GLES leaves the texture empty */
    {
        uint32_t d[2] = {0};
        snx_gl_gen_textures(desktop, 2, d);
        snx_gl_bind_texture(desktop, T2D, d[0]);
        snx_gl_tex_image(desktop, T2D, 0, 0x8051, 2, 1, 1, RGBA, UBYTE, (uint64_t)(uintptr_t)pattern, 0, 0); /* RGB8 */
        assert(!snx_gl_get_error(desktop) && !heard.last.swap_rb);
        assert(texels(d[0])[0] == pattern[0] && texels(d[0])[2] == pattern[2] && texels(d[0])[3] == pattern[4]);
        assert(texels(d[0])[5] == pattern[6]); /* the second texel's blue: its alpha left out */
        snx_gl_bind_texture(desktop, T2D, d[1]);
        snx_gl_tex_image(desktop, T2D, 0, RGBA8, 2, 1, 1, 0x1907, UBYTE, (uint64_t)(uintptr_t)pattern, 0, 0); /* RGB */
        assert(texels(d[1])[0] == pattern[0] && texels(d[1])[3] == 0xff && texels(d[1])[4] == pattern[3]);
        snx_gl_tex_sub_image(desktop, T2D, 0, 1, 0, 0, 1, 1, 1, 0x1903, UBYTE, (uint64_t)(uintptr_t)(pattern + 9), 0,
                             0); /* GL_RED */
        assert(texels(d[1])[4] == pattern[9] && texels(d[1])[5] == 0 && texels(d[1])[6] == 0 &&
               texels(d[1])[7] == 0xff);
    }
    /* the LOD bias: a desktop context's alone */
    snx_gl_tex_parameter(ctx, T2D, LOD_BIAS, 1.5f);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM && snx_gl_texture(1)->sampler.lod_bias == 0);
    {
        uint32_t d = 0;
        float bias = 0;
        snx_gl_gen_textures(desktop, 1, &d);
        snx_gl_bind_texture(desktop, T2D, d);
        snx_gl_tex_parameter(desktop, T2D, LOD_BIAS, 1.5f);
        assert(snx_gl_get_tex_parameter(desktop, T2D, LOD_BIAS, &bias) && bias == 1.5f);
    }
    /* levels: a level's size and format while it has texels; completeness from the base level up to the max */
    assert(level_parameter(ctx, T2D, 1, WIDTH) == 2 && level_parameter(ctx, T2D, 2, WIDTH) == 0);
    assert(level_parameter(ctx, T2D, 0, INTERNAL) == RGBA8 && level_parameter(ctx, T2D, 0, COMPRESSED) == 0);
    {
        int32_t levels = 0;
        assert(snx_gl_texture_complete(1, &levels) && levels == 2);
        snx_gl_tex_parameter(ctx, T2D, MAX_LEVEL, 0);
        assert(snx_gl_texture_complete(1, &levels) && levels == 1);
        snx_gl_tex_parameter(ctx, T2D, MAX_LEVEL, 1000);
        snx_gl_tex_parameter(ctx, T2D, BASE_LEVEL, 2);
        assert(!snx_gl_texture_complete(1, &levels)); /* level 2 has no texels */
        snx_gl_tex_parameter(ctx, T2D, BASE_LEVEL, 0);
        /* mipmaps: none made by the platform leaves the levels; made, every level has texels */
        heard.make_mipmaps = 0;
        snx_gl_generate_mipmap(ctx, T2D);
        assert(heard.mipmaps == 1 && snx_gl_texture(1)->defined == 3);
        heard.make_mipmaps = 1;
        snx_gl_generate_mipmap(ctx, T2D);
        assert(heard.mipmaps == 2 && snx_gl_texture(1)->defined == 7);
        assert(snx_gl_texture_complete(1, &levels) && levels == 3);
        snx_gl_texture_defined(2, 1);
        assert(snx_gl_texture(2)->defined == 3);
    }

    /* sampler objects: a unit's reads its texture over the texture's own state */
    {
        uint32_t s[2] = {0};
        float got = 0;
        snx_gl_gen_samplers(ctx, 2, s);
        assert(s[0] == 1 && s[1] == 2 && snx_gl_is_sampler(2) && !snx_gl_is_sampler(3));
        assert(snx_gl_unit_sampler(ctx, 0, 1)->min == NEAREST);
        snx_gl_sampler_parameter(ctx, s[0], WRAP_S, CLAMP);
        snx_gl_bind_sampler(ctx, 0, s[0]);
        assert(snx_gl_unit_sampler(ctx, 0, 1)->wrap[0] == CLAMP && snx_gl_unit_sampler(ctx, 0, 1)->min != NEAREST);
        assert(snx_gl_get(ctx, 0x8919, v) == 1 && v[0] == s[0]);
        assert(snx_gl_get_sampler_parameter(ctx, s[0], WRAP_S, &got) && got == CLAMP);
        assert(!snx_gl_get_sampler_parameter(ctx, s[0], BASE_LEVEL, &got));
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
        snx_gl_bind_sampler(ctx, SNX_GL_UNITS, s[0]);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE);
        snx_gl_bind_sampler(ctx, 1, 9);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);
        snx_gl_sampler_parameter(ctx, 9, WRAP_S, CLAMP);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION);
        snx_gl_delete_samplers(ctx, 1, s);
        assert(!snx_gl_is_sampler(1) && snx_gl_textures(ctx)->samplers[0] == 0);
    }
    /* image units: bound, the platform told; a unit past GL's refused */
    snx_gl_bind_image_texture(ctx, 2, 1, 0, 0, 0, 0x88b9, RGBA8);
    assert(heard.images == 1 && snx_gl_textures(ctx)->images[2].texture == 1);
    snx_gl_bind_image_texture(ctx, SNX_GL_IMAGE_UNITS, 1, 0, 0, 0, 0x88b9, RGBA8);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE);

    /* glCopyTexImage2D: an unsized format as the read framebuffer's 4 bytes, the level marked, the name to copy into */
    {
        uint32_t c = 0;
        snx_gl_gen_textures(ctx, 1, &c);
        snx_gl_bind_texture(ctx, T2D, c);
        assert(snx_gl_copy_tex_image(ctx, T2D, 0, RGB, 4, 2) == c);
        assert(snx_gl_texture(c)->format == snx_gl_format(RGB, RGB, UBYTE) && snx_gl_texture(c)->defined == 1);
        assert(snx_gl_copy_tex_image(ctx, T2D, 1, RGB, 2, 1) == c && snx_gl_texture(c)->defined == 3);
        assert(!snx_gl_copy_tex_image(ctx, T3D, 0, RGB, 2, 1) && snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
    }
    /* a storage the platform could not make: none, and the next call tries again */
    {
        uint32_t f = 0;
        unsigned made = heard.storages;
        snx_gl_gen_textures(ctx, 1, &f);
        snx_gl_bind_texture(ctx, T2D, f);
        heard.refuse_storage = 1;
        snx_gl_tex_image(ctx, T2D, 0, RGBA8, 4, 4, 1, RGBA, UBYTE, 0, 0, 0);
        assert(heard.storages == made + 1 && snx_gl_texture(f)->format == -1 && !snx_gl_texture(f)->texels);
        heard.refuse_storage = 0;
        snx_gl_tex_image(ctx, T2D, 0, RGBA8, 4, 4, 1, RGBA, UBYTE, 0, 0, 0);
        assert(heard.storages == made + 2 && snx_gl_texture(f)->format >= 0 && snx_gl_texture(f)->texels);
    }

    /* deletes: the bindings cleared, the platform told, the texels' memory taken by the next */
    {
        uint64_t was = snx_gl_texture(2)->texels;
        uint32_t two = 2, n = 0;
        unsigned went = heard.gone;
        snx_gl_bind_texture(ctx, T2D, 2);
        snx_gl_bind_image_texture(ctx, 4, 2, 0, 0, 0, 0x88b9, RGB8);
        snx_gl_delete_textures(ctx, 1, &two);
        assert(!snx_gl_texture(2) && heard.gone == went + 1 && snx_gl_unit_texture(ctx, 0, SNX_GL_T2D) == 0);
        assert(snx_gl_textures(ctx)->images[4].texture == 0);
        snx_gl_gen_textures(ctx, 1, &n);
        assert(n == 2);
        snx_gl_bind_texture(ctx, T2D, n);
        snx_gl_tex_image(ctx, T2D, 0, RGB8, 3, 2, 1, RGB, UBYTE, 0, 0, 0);
        assert(snx_gl_texture(n)->texels == was);
        snx_gl_bind_texture(ctx, T2D, 1);
    }

    /* a platform that lays the storage out itself: the bytes and alignment it says, its upload writes the texels, the
       layer keeps no tight rows; a layout of 0 bytes (a format the platform has none of) keeps no texels */
    {
        uint32_t p[2] = {0};
        uint64_t at;
        heard.layout_bytes = 4096, heard.layout_align = 2048;
        snx_gl_texture_host(&(SnxGlTextureHost){.storage = storage,
                                                .upload = upload,
                                                .mipmaps = mipmaps,
                                                .image_bound = image_bound,
                                                .gone = gone,
                                                .say = say,
                                                .max_size = max_size,
                                                .keeps_texels = 1,
                                                .storage_bytes = layout_bytes});
        snx_gl_gen_textures(ctx, 2, p);
        snx_gl_bind_texture(ctx, T2D, p[0]);
        snx_gl_tex_image(ctx, T2D, 0, RGBA8, 4, 4, 1, RGBA, UBYTE, (uint64_t)(uintptr_t)pattern, 0, 0);
        at = snx_gl_texture(p[0])->texels;
        assert(at && !(at % 2048) && snx_gl_texture(p[0])->texels_size == 4096 && heard.last.src == pattern);
        assert(texels(p[0])[0] == 0); /* the platform's to write: zeros, not the client's tight rows */
        heard.layout_bytes = 0;
        snx_gl_bind_texture(ctx, T2D, p[1]);
        snx_gl_tex_image(ctx, T2D, 0, RGBA8, 4, 4, 1, RGBA, UBYTE, 0, 0, 0);
        assert(!snx_gl_texture(p[1])->texels && snx_gl_texture(p[1])->format >= 0 && !snx_gl_get_error(ctx));
        snx_gl_delete_textures(ctx, 2, p);
        snx_gl_texture_host(&(SnxGlTextureHost){.storage = storage,
                                                .upload = upload,
                                                .mipmaps = mipmaps,
                                                .image_bound = image_bound,
                                                .gone = gone,
                                                .say = say,
                                                .max_size = max_size,
                                                .keeps_texels = 1});
        snx_gl_bind_texture(ctx, T2D, 1);
    }

    /* a save state's copy: loaded back whole, the platform told every texture's image is of the old */
    size_t total = snx_gl_state_bytes();
    unsigned char *saved = malloc(total), *copy = malloc(total), *after = malloc(total);
    char why[300];
    assert(saved && copy && after);
    snx_gl_state_save(saved);
    assert(!snx_gl_state_check(saved, total, why, sizeof why));
    {
        unsigned went = heard.gone;
        uint32_t one = 1;
        snx_gl_delete_textures(ctx, 1, &one);
        snx_gl_active_texture(ctx, UNIT0 + 5);
        assert(!snx_gl_state_load(saved, total));
        assert(heard.gone >= went + 1 + SNX_GL_TEXTURES);
        snx_gl_state_save(after);
        assert(!memcmp(saved, after, total));
        assert(snx_gl_is_texture(1) && snx_gl_unit_texture(ctx, 0, SNX_GL_T2D) == 1 &&
               snx_gl_textures(ctx)->active == 0);
        assert(snx_gl_texture(1)->levels == 3 && !memcmp(texels(1) + 64, pattern + 248, 16)); /* from offset 48 */
    }
    /* refused copies, each naming what it found */
    {
        size_t textures = 16 + (size_t)SNX_GL_CONTEXTS * sizeof(SnxGlContext) + snx_gl_program_state_bytes() +
                          snx_gl_object_state_bytes() + 8;
        size_t contexts = textures + SNX_GL_TEXTURES * sizeof(SnxGlTexture) + SNX_GL_SAMPLERS * sizeof(SnxGlSampler);
        SnxGlTexture t;
        SnxGlTextureContext c;
        memcpy(copy, saved, total);
        memcpy(&t, copy + textures, sizeof t); /* texture 1 */
        t.format = SNX_GL_FORMATS;
        memcpy(copy + textures, &t, sizeof t);
        refused(copy, total, "texture 1 has target 0, format 137");
        memcpy(copy, saved, total);
        memcpy(&t, copy + textures, sizeof t);
        t.texels_size += 1u << 30;
        memcpy(copy + textures, &t, sizeof t);
        refused(copy, total, "texture 1 keeps its texels at");
        memcpy(copy, saved, total);
        memcpy(&c, copy + contexts, sizeof c); /* context 1 */
        c.textures[0][SNX_GL_T2D] = SNX_GL_TEXTURES + 1;
        memcpy(copy + contexts, &c, sizeof c);
        refused(copy, total, "context 1 binds a texture or sampler past");
        refused(saved, total - 8, "this build reads");
    }
    free(saved);
    free(copy);
    free(after);
    puts("gl_texture: ok");
    return 0;
}
