/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's texture and sampler calls on the console (gl_layer.h, gpu/gl/gl.hook's texture family): each the
   shared layer's (common/gpu/gl/gl_texture.h) on the calling thread's current context, under the object lock, since
   the layer keeps each texture's texels in the same guest memory as the buffers' bytes (gl_object_calls.c) and reads
   the pixel unpack buffer. The texels kept there, level after level, are what the renderer samples once the layer
   draws, and a save state holds them with the game's memory. Without a current context a call does nothing and
   returns 0 */
#include "common/gpu/gl/gl_state.h"
#include "common/gpu/gl/gl_texture.h"
#include "gpu/gl/gl.hook.h"
#include "nx/gpu/gl/gl_layer.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include <string.h>

static void say(const char* line) { LOG("gl layer: %s\n", line); }
void gl_texture_calls_init(void) {
    snx_gl_texture_host(&(SnxGlTextureHost){NULL, NULL, NULL, NULL, NULL, say, NULL, 1});
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
