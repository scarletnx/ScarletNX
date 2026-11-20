/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's GL calls on the console (gl_layer.h, gpu/gl/gl.hook's state and draw families): each the shared
   layer's (common/gpu/gl/gl_state.h) on the calling thread's current context, with the guest's pointers read and
   written here. Without a current context a call does nothing and returns 0, as GL's do */
#include "common/gpu/gl/gl_framebuffer.h"
#include "common/gpu/gl/gl_state.h"
#include "gpu/gl/gl.hook.h"
#include "nx/gpu/gl/gl_layer.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include <math.h>
#include <string.h>

enum { GL_FRONT_AND_BACK_ = 0x0408, GL_COLOR_BUFFER_BIT_ = 0x4000 };

u32 gl_get_error(CPU* c) {
    u32 ctx = gl_egl_context();
    (void)c;
    return ctx ? snx_gl_get_error(ctx) : 0;
}
#define CTX                                                                                                            \
    u32 ctx = gl_egl_context();                                                                                        \
    (void)c;                                                                                                           \
    if (!ctx) return
void gl_enable(CPU* c, u32 cap) {
    CTX;
    snx_gl_enable(ctx, cap, 1);
}
void gl_disable(CPU* c, u32 cap) {
    CTX;
    snx_gl_enable(ctx, cap, 0);
}
u8 gl_is_enabled(CPU* c, u32 cap) {
    CTX 0;
    return (u8)snx_gl_is_enabled(ctx, cap);
}
void gl_blend_func(CPU* c, u32 src, u32 dst) {
    CTX;
    snx_gl_blend_func_separate(ctx, src, dst, src, dst);
}
void gl_blend_func_separate(CPU* c, u32 src_rgb, u32 dst_rgb, u32 src_alpha, u32 dst_alpha) {
    CTX;
    snx_gl_blend_func_separate(ctx, src_rgb, dst_rgb, src_alpha, dst_alpha);
}
void gl_blend_equation(CPU* c, u32 mode) {
    CTX;
    snx_gl_blend_equation_separate(ctx, mode, mode);
}
void gl_blend_equation_separate(CPU* c, u32 rgb, u32 alpha) {
    CTX;
    snx_gl_blend_equation_separate(ctx, rgb, alpha);
}
void gl_blend_color(CPU* c, f32 r, f32 g, f32 b, f32 a) {
    CTX;
    snx_gl_blend_color(ctx, r, g, b, a);
}
void gl_color_mask(CPU* c, u8 r, u8 g, u8 b, u8 a) {
    CTX;
    snx_gl_color_mask(ctx, r, g, b, a);
}
void gl_depth_mask(CPU* c, u8 flag) {
    CTX;
    snx_gl_depth_mask(ctx, flag);
}
void gl_depth_func(CPU* c, u32 func) {
    CTX;
    snx_gl_depth_func(ctx, func);
}
void gl_depth_rangef(CPU* c, f32 n, f32 f) {
    CTX;
    snx_gl_depth_range(ctx, n, f);
}
void gl_depth_range(CPU* c, f64 n, f64 f) {
    CTX;
    snx_gl_depth_range(ctx, (float)n, (float)f);
}
void gl_cull_face(CPU* c, u32 mode) {
    CTX;
    snx_gl_cull_face(ctx, mode);
}
void gl_front_face(CPU* c, u32 mode) {
    CTX;
    snx_gl_front_face(ctx, mode);
}
void gl_stencil_func(CPU* c, u32 func, i32 ref, u32 mask) {
    CTX;
    snx_gl_stencil_func_separate(ctx, GL_FRONT_AND_BACK_, func, ref, mask);
}
void gl_stencil_func_separate(CPU* c, u32 face, u32 func, i32 ref, u32 mask) {
    CTX;
    snx_gl_stencil_func_separate(ctx, face, func, ref, mask);
}
void gl_stencil_op(CPU* c, u32 fail, u32 zfail, u32 zpass) {
    CTX;
    snx_gl_stencil_op_separate(ctx, GL_FRONT_AND_BACK_, fail, zfail, zpass);
}
void gl_stencil_op_separate(CPU* c, u32 face, u32 fail, u32 zfail, u32 zpass) {
    CTX;
    snx_gl_stencil_op_separate(ctx, face, fail, zfail, zpass);
}
void gl_stencil_mask(CPU* c, u32 mask) {
    CTX;
    snx_gl_stencil_mask_separate(ctx, GL_FRONT_AND_BACK_, mask);
}
void gl_stencil_mask_separate(CPU* c, u32 face, u32 mask) {
    CTX;
    snx_gl_stencil_mask_separate(ctx, face, mask);
}
void gl_clear_color(CPU* c, f32 r, f32 g, f32 b, f32 a) {
    CTX;
    snx_gl_clear_color(ctx, r, g, b, a);
}
void gl_clear_depthf(CPU* c, f32 depth) {
    CTX;
    snx_gl_clear_depth(ctx, depth);
}
void gl_clear_depth(CPU* c, f64 depth) {
    CTX;
    snx_gl_clear_depth(ctx, (float)depth);
}
void gl_clear_stencil(CPU* c, i32 s) {
    CTX;
    snx_gl_clear_stencil(ctx, s);
}
void gl_viewport(CPU* c, i32 x, i32 y, i32 width, i32 height) {
    CTX;
    snx_gl_viewport(ctx, x, y, width, height);
}
void gl_scissor(CPU* c, i32 x, i32 y, i32 width, i32 height) {
    CTX;
    snx_gl_scissor(ctx, x, y, width, height);
}
void gl_line_width(CPU* c, f32 width) {
    CTX;
    snx_gl_line_width(ctx, width);
}
void gl_point_size(CPU* c, f32 size) {
    CTX;
    snx_gl_point_size(ctx, size);
}
void gl_polygon_mode(CPU* c, u32 face, u32 mode) {
    CTX;
    snx_gl_polygon_mode(ctx, face, mode);
}
void gl_polygon_offset(CPU* c, f32 factor, f32 units) {
    CTX;
    snx_gl_polygon_offset(ctx, factor, units);
}
void gl_sample_coverage(CPU* c, f32 value, u8 invert) {
    CTX;
    snx_gl_sample_coverage(ctx, value, invert);
}
void gl_hint(CPU* c, u32 target, u32 mode) {
    CTX;
    snx_gl_hint(ctx, target, mode);
}
void gl_pixel_storei(CPU* c, u32 pname, i32 value) {
    CTX;
    snx_gl_pixel_store(ctx, pname, value);
}

/* glGet*'s values of pname into the guest's data, each of size bytes as kind writes it ('i' 32-bit, 'l' 64-bit, 'f'
   float, 'b' boolean): a pname the layer doesn't answer records GL_INVALID_ENUM and is said once */
static void get(u32 pname, u64 data, char kind) {
    double v[4];
    u32 ctx = gl_egl_context();
    int n;
    if (!ctx) return;
    n = snx_gl_get(ctx, pname, v);
    if (!n) {
        LOG_LIMIT(32, "gl layer: glGet of 0x%04x isn't answered yet: GL_INVALID_ENUM\n", (unsigned)pname);
        snx_gl_error(ctx, SNX_GL_INVALID_ENUM);
        return;
    }
    for (int i = 0; data && i < n; i++) switch (kind) {
            case 'i': gw32(data + 4 * (u64)i, (u32)(i32)lround(v[i])); break;
            case 'l': gw64(data + 8 * (u64)i, (u64)llround(v[i])); break;
            case 'f': {
                float f = (float)v[i];
                u32 bits;
                memcpy(&bits, &f, sizeof bits);
                gw32(data + 4 * (u64)i, bits);
                break;
            }
            default: *GP(data + (u64)i) = v[i] != 0; break;
        }
}
void gl_get_integerv(CPU* c, u32 pname, u64 data) { (void)c, get(pname, data, 'i'); }
void gl_get_integer64v(CPU* c, u32 pname, u64 data) { (void)c, get(pname, data, 'l'); }
void gl_get_floatv(CPU* c, u32 pname, u64 data) { (void)c, get(pname, data, 'f'); }
void gl_get_booleanv(CPU* c, u32 pname, u64 data) { (void)c, get(pname, data, 'b'); }
u64 gl_get_string(CPU* c, u32 name) {
    const char* s;
    CTX 0;
    s = snx_gl_string(ctx, name, 0, 0);
    return s ? gl_host_string(s) : 0;
}
u64 gl_get_stringi(CPU* c, u32 name, u32 index) {
    const char* s;
    CTX 0;
    s = snx_gl_string(ctx, name, 1, index);
    return s ? gl_host_string(s) : 0;
}
void gl_flush(CPU* c) { (void)c; }
void gl_finish(CPU* c) { (void)c; }

/* glClear: the renderer's clear of the bound draw framebuffer's targets, or the window surface's */
void gl_clear(CPU* c, u32 mask) {
    u32 buffers;
    CTX;
    buffers = snx_gl_clear(ctx, mask);
    if (buffers) gl_object_calls_lock(), gl_draw_clear(ctx, buffers), gl_object_calls_unlock();
}

/* ---- the draws (gl_draw.c), under the object lock their buffers and textures are under ---- */

/* GL's primitive modes are the 3D engine's numbers (points 0 to triangle fan 6, the adjacency ones 10 to 13,
   patches 14) */
static void draw(u32 ctx, DrawCall* call, u32 type, u64 indices) {
    gl_object_calls_lock();
    gl_draw(ctx, call, type, indices);
    gl_object_calls_unlock();
}
void gl_draw_arrays(CPU* c, u32 mode, i32 first, i32 count) {
    CTX;
    if (count > 0)
        draw(ctx, &(DrawCall){.topology = mode, .first = (u32)first, .count = (u32)count, .instances = 1}, 0, 0);
}
void gl_draw_arrays_instanced(CPU* c, u32 mode, i32 first, i32 count, i32 instances) {
    CTX;
    if (count > 0 && instances > 0)
        draw(ctx, &(DrawCall){.topology = mode, .first = (u32)first, .count = (u32)count, .instances = (u32)instances},
             0, 0);
}
void gl_draw_elements_base_vertex(CPU* c, u32 mode, i32 count, u32 type, u64 indices, i32 base) {
    CTX;
    if (count > 0)
        draw(ctx,
             &(DrawCall){.topology = mode, .indexed = 1, .count = (u32)count, .base_vertex = (u32)base, .instances = 1},
             type, indices);
}
void gl_draw_elements(CPU* c, u32 mode, i32 count, u32 type, u64 indices) {
    gl_draw_elements_base_vertex(c, mode, count, type, indices, 0);
}
void gl_draw_range_elements(CPU* c, u32 mode, u32 start, u32 end, i32 count, u32 type, u64 indices) {
    (void)start, (void)end;
    gl_draw_elements_base_vertex(c, mode, count, type, indices, 0);
}
void gl_draw_elements_instanced_base_vertex(CPU* c, u32 mode, i32 count, u32 type, u64 indices, i32 instances,
                                            i32 base) {
    CTX;
    if (count > 0 && instances > 0)
        draw(ctx,
             &(DrawCall){.topology = mode,
                         .indexed = 1,
                         .count = (u32)count,
                         .base_vertex = (u32)base,
                         .instances = (u32)instances},
             type, indices);
}
void gl_draw_elements_instanced(CPU* c, u32 mode, i32 count, u32 type, u64 indices, i32 instances) {
    gl_draw_elements_instanced_base_vertex(c, mode, count, type, indices, instances, 0);
}
