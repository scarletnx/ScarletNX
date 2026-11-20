/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's shader and program calls on the console (gl_layer.h, gpu/gl/gl.hook's program family): each the
   shared layer's (common/gpu/gl/gl_program.h) on the calling thread's current context, the guest's pointers made host
   pointers into guest memory. Shaders and programs are shared between a game's contexts and threads, so every call
   holds the layer's program lock. Without a current context a call does nothing and returns 0, as GL's do */
#include "common/gpu/gl/gl_program.h"
#include "common/gpu/gl/gl_state.h"
#include "common/platform/thread.h"
#include "gpu/gl/gl.hook.h"
#include "nx/gpu/gl/gl_host_program.h"
#include "nx/gpu/gl/gl_layer.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include <stdint.h>
#include <string.h>

static struct {
    PMutex lock;
    SnxGlLayout installed; /* the renderer's rules the shared layer was given last */
    int given;             /* whether it was given any */
} g_gl_program_calls = {.lock = PMUTEX_INIT};

/* a guest pointer as a host one, NULL for 0 */
static void* host(u64 a) { return a ? GP(a) : NULL; }
static const char* text(u64 a) { return (const char*)host(a); }

static void say(const char* line) { LOG("gl layer: %s\n", line); }
/* the renderer's draws bind each stage's resources in the stage's own set (gl_host_program.h), by its rules for the
   device, which it sets as it starts: after the layer's start, before the game's first link. Each link takes them as
   they are then */
void gl_program_layout_refresh(void) {
    SnxGlLayout now = gl_host_program_layout();
    if (g_gl_program_calls.given && !memcmp(&now, &g_gl_program_calls.installed, sizeof now)) return;
    g_gl_program_calls.installed = now, g_gl_program_calls.given = 1;
    snx_gl_program_host(&(SnxGlProgramHost){.say = say, .stage_sets = 1, .layout = now});
}
void gl_program_calls_init(void) { gl_program_layout_refresh(); }

/* the calling thread's context, the lock held: the body runs with ctx; without a context, none and fallback */
#define LOCKED(fallback, ...)                                                                                          \
    do {                                                                                                               \
        u32 ctx = gl_egl_context();                                                                                    \
        (void)c;                                                                                                       \
        if (!ctx) return fallback;                                                                                     \
        pm_lock(&g_gl_program_calls.lock);                                                                             \
        __VA_ARGS__;                                                                                                   \
        pm_unlock(&g_gl_program_calls.lock);                                                                           \
    } while (0)
#define NONE /* a void call's fallback */

/* ---- shaders ---- */

u32 gl_create_shader(CPU* c, u32 type) {
    u32 name = 0;
    LOCKED(0, name = snx_gl_create_shader(ctx, type));
    return name;
}
void gl_delete_shader(CPU* c, u32 shader) { LOCKED(NONE, snx_gl_delete_shader(ctx, shader)); }
u8 gl_is_shader(CPU* c, u32 shader) {
    u8 is = 0;
    LOCKED(0, is = (u8)snx_gl_is_shader(shader));
    return is;
}
/* glShaderSource's strings (an array of guest pointers, lengths an array of i32 or 0) joined into the shader's text */
static void source(u32 ctx, u32 shader, i32 count, u64 strings, u64 lengths) {
    char* to = NULL;
    if (count < 0) {
        snx_gl_error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    for (int pass = 0; pass < 2; ++pass) {
        size_t n = 0;
        for (i32 i = 0; i < count; ++i) {
            u64 at = gr64(strings + 8 * (u64)i);
            i32 len = lengths ? (i32)gr32(lengths + 4 * (u64)i) : -1;
            const char* t = text(at);
            size_t l = !t ? 0 : len < 0 ? strlen(t) : (size_t)len;
            if (pass) memcpy(to + n, t, l);
            n += l;
        }
        if (!pass) {
            to = snx_gl_shader_source(ctx, shader, n);
            if (!to) return;
        }
    }
}
void gl_shader_source(CPU* c, u32 shader, i32 count, u64 strings, u64 lengths) {
    LOCKED(NONE, source(ctx, shader, count, strings, lengths));
}
void gl_compile_shader(CPU* c, u32 shader) { LOCKED(NONE, snx_gl_compile_shader(ctx, shader)); }
void gl_get_shaderiv(CPU* c, u32 shader, u32 pname, u64 params) {
    i32 v;
    LOCKED(NONE, if (snx_gl_get_shaderiv(ctx, shader, pname, &v)) gw32(params, (u32)v));
}
void gl_get_shader_info_log(CPU* c, u32 shader, i32 size, u64 length, u64 log) {
    LOCKED(NONE, const char* t = snx_gl_shader_info_log(ctx, shader);
           if (t) snx_gl_string_out(t, size, host(length), host(log)));
}
void gl_get_shader_source(CPU* c, u32 shader, i32 size, u64 length, u64 out) {
    LOCKED(NONE, const char* t = snx_gl_shader_text(ctx, shader);
           if (t) snx_gl_string_out(t, size, host(length), host(out)));
}
void gl_get_shader_precision_format(CPU* c, u32 type, u32 precision, u64 range, u64 bits) {
    i32 r[2], b;
    (void)c, (void)type;
    snx_gl_shader_precision_format(precision, r, &b);
    gw32(range, (u32)r[0]);
    gw32(range + 4, (u32)r[1]);
    gw32(bits, (u32)b);
}
void gl_shader_binary(CPU* c, i32 count, u64 shaders, u32 format, u64 binary, i32 length) {
    (void)count, (void)shaders, (void)format, (void)binary, (void)length;
    LOCKED(NONE, snx_gl_error(ctx, SNX_GL_INVALID_ENUM)); /* no binary formats */
}
/* hints: the compiler stays, and runs on the calling thread */
void gl_release_shader_compiler(CPU* c) { (void)c; }
void gl_max_shader_compiler_threads(CPU* c, u32 count) { (void)c, (void)count; }

/* ---- programs ---- */

u32 gl_create_program(CPU* c) {
    u32 name = 0;
    LOCKED(0, name = snx_gl_create_program(ctx));
    return name;
}
void gl_delete_program(CPU* c, u32 program) { LOCKED(NONE, snx_gl_delete_program(ctx, program)); }
u8 gl_is_program(CPU* c, u32 program) {
    u8 is = 0;
    LOCKED(0, is = (u8)snx_gl_is_program(program));
    return is;
}
void gl_attach_shader(CPU* c, u32 program, u32 shader) { LOCKED(NONE, snx_gl_attach_shader(ctx, program, shader)); }
void gl_detach_shader(CPU* c, u32 program, u32 shader) { LOCKED(NONE, snx_gl_detach_shader(ctx, program, shader)); }
void gl_get_attached_shaders(CPU* c, u32 program, i32 max, u64 count, u64 shaders) {
    LOCKED(NONE, i32 n = snx_gl_attached_shaders(ctx, program, max, host(shaders));
           if (n >= 0 && count) gw32(count, (u32)n));
}
void gl_bind_attrib_location(CPU* c, u32 program, u32 index, u64 name) {
    LOCKED(NONE, snx_gl_bind_attrib_location(ctx, program, index, text(name)));
}
void gl_link_program(CPU* c, u32 program) {
    LOCKED(NONE, gl_program_layout_refresh(); snx_gl_link_program(ctx, program));
}
void gl_use_program(CPU* c, u32 program) { LOCKED(NONE, snx_gl_use_program(ctx, program)); }
void gl_validate_program(CPU* c, u32 program) { LOCKED(NONE, snx_gl_validate_program(ctx, program)); }
void gl_program_parameteri(CPU* c, u32 program, u32 pname, i32 value) {
    LOCKED(NONE, snx_gl_program_parameter(ctx, program, pname, value));
}
void gl_get_programiv(CPU* c, u32 program, u32 pname, u64 params) {
    i32 v[3];
    LOCKED(NONE, int n = snx_gl_get_programiv(ctx, program, pname, v);
           for (int i = 0; i < n; ++i) gw32(params + 4 * (u64)i, (u32)v[i]));
}
void gl_get_program_info_log(CPU* c, u32 program, i32 size, u64 length, u64 log) {
    LOCKED(NONE, const char* t = snx_gl_program_info_log(ctx, program);
           if (t) snx_gl_string_out(t, size, host(length), host(log)));
}
/* glGetActiveUniform and glGetActiveAttrib */
static void active(u32 ctx, u32 program, u32 index, i32 size, u64 length, u64 count, u64 type, u64 name, int uniforms) {
    i32 n;
    u32 t;
    const char* s = snx_gl_active(ctx, program, index, uniforms, &n, &t);
    if (!s) return;
    snx_gl_string_out(s, size, host(length), host(name));
    if (count) gw32(count, (u32)n);
    if (type) gw32(type, t);
}
void gl_get_active_uniform(CPU* c, u32 program, u32 index, i32 size, u64 length, u64 count, u64 type, u64 name) {
    LOCKED(NONE, active(ctx, program, index, size, length, count, type, name, 1));
}
void gl_get_active_attrib(CPU* c, u32 program, u32 index, i32 size, u64 length, u64 count, u64 type, u64 name) {
    LOCKED(NONE, active(ctx, program, index, size, length, count, type, name, 0));
}
i32 gl_get_attrib_location(CPU* c, u32 program, u64 name) {
    i32 loc = -1;
    LOCKED(-1, loc = snx_gl_attrib_location(ctx, program, text(name)));
    return loc;
}
i32 gl_get_frag_data_location(CPU* c, u32 program, u64 name) {
    i32 loc = -1;
    LOCKED(-1, loc = snx_gl_frag_data_location(ctx, program, text(name)));
    return loc;
}
i32 gl_get_uniform_location(CPU* c, u32 program, u64 name) {
    i32 loc = -1;
    LOCKED(-1, loc = snx_gl_uniform_location(ctx, program, text(name)));
    return loc;
}
void gl_get_uniformfv(CPU* c, u32 program, i32 location, u64 params) {
    LOCKED(NONE, snx_gl_get_uniform(ctx, program, location, 'f', host(params)));
}
void gl_get_uniformiv(CPU* c, u32 program, i32 location, u64 params) {
    LOCKED(NONE, snx_gl_get_uniform(ctx, program, location, 'i', host(params)));
}
/* glGetnUniform*v: glGetUniform*v's into a buffer of size bytes */
static void get_nuniform(u32 ctx, u32 program, i32 location, i32 size, u64 params, char kind) {
    unsigned char value[64] = {0};
    snx_gl_get_uniform(ctx, program, location, kind, value);
    if (size > 0) memcpy(host(params), value, (size_t)size < sizeof value ? (size_t)size : sizeof value);
}
void gl_get_nuniformfv(CPU* c, u32 program, i32 location, i32 size, u64 params) {
    LOCKED(NONE, get_nuniform(ctx, program, location, size, params, 'f'));
}
void gl_get_nuniformiv(CPU* c, u32 program, i32 location, i32 size, u64 params) {
    LOCKED(NONE, get_nuniform(ctx, program, location, size, params, 'i'));
}
u32 gl_get_uniform_block_index(CPU* c, u32 program, u64 name) {
    u32 index = 0xffffffffu;
    LOCKED(0xffffffffu, index = snx_gl_uniform_block_index(ctx, program, text(name)));
    return index;
}
void gl_uniform_block_binding(CPU* c, u32 program, u32 index, u32 binding) {
    LOCKED(NONE, snx_gl_uniform_block_binding(ctx, program, index, binding));
}
void gl_get_active_uniform_blockiv(CPU* c, u32 program, u32 index, u32 pname, u64 params) {
    LOCKED(NONE, snx_gl_active_uniform_blockiv(ctx, program, index, pname, host(params), INT32_MAX));
}
void gl_get_active_uniform_block_name(CPU* c, u32 program, u32 index, i32 size, u64 length, u64 name) {
    LOCKED(NONE, const char* t = snx_gl_active_uniform_block_name(ctx, program, index);
           if (t) snx_gl_string_out(t, size, host(length), host(name)));
}
/* glGetUniformIndices(program, count, names, indices): names an array of guest pointers */
static void uniform_indices(u32 ctx, u32 program, i32 count, u64 names, u64 indices) {
    if (!snx_gl_is_program(program)) {
        snx_gl_error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    for (i32 k = 0; k < count; ++k)
        gw32(indices + 4 * (u64)k, snx_gl_uniform_index(ctx, program, text(gr64(names + 8 * (u64)k))));
}
void gl_get_uniform_indices(CPU* c, u32 program, i32 count, u64 names, u64 indices) {
    LOCKED(NONE, uniform_indices(ctx, program, count, names, indices));
}
static void active_uniformsiv(u32 ctx, u32 program, i32 count, u64 indices, u32 pname, u64 params) {
    for (i32 k = 0; k < count; ++k) {
        i32 v;
        if (!snx_gl_active_uniformiv(ctx, program, gr32(indices + 4 * (u64)k), pname, &v)) return;
        gw32(params + 4 * (u64)k, (u32)v);
    }
}
void gl_get_active_uniformsiv(CPU* c, u32 program, i32 count, u64 indices, u32 pname, u64 params) {
    LOCKED(NONE, active_uniformsiv(ctx, program, count, indices, pname, params));
}
void gl_get_program_interfaceiv(CPU* c, u32 program, u32 interface, u32 pname, u64 params) {
    i32 v;
    LOCKED(NONE, if (snx_gl_program_interfaceiv(ctx, program, interface, pname, &v)) gw32(params, (u32)v));
}
u32 gl_get_program_resource_index(CPU* c, u32 program, u32 interface, u64 name) {
    u32 index = 0xffffffffu;
    LOCKED(0xffffffffu, index = snx_gl_program_resource_index(ctx, program, interface, text(name)));
    return index;
}
void gl_get_program_resource_name(CPU* c, u32 program, u32 interface, u32 index, i32 size, u64 length, u64 name) {
    LOCKED(NONE, const char* t = snx_gl_program_resource_name(ctx, program, interface, index);
           if (t) snx_gl_string_out(t, size, host(length), host(name)));
}
void gl_get_program_resourceiv(CPU* c, u32 program, u32 interface, u32 index, i32 count, u64 props, i32 size,
                               u64 length, u64 params) {
    LOCKED(NONE,
           i32 n = snx_gl_program_resourceiv(ctx, program, interface, index, count, host(props), size, host(params));
           if (length) gw32(length, (u32)n));
}
i32 gl_get_program_resource_location(CPU* c, u32 program, u32 interface, u64 name) {
    i32 loc = -1;
    LOCKED(-1, loc = snx_gl_program_resource_location(ctx, program, interface, text(name)));
    return loc;
}
void gl_program_binary(CPU* c, u32 program, u32 format, u64 binary, i32 length) {
    (void)format, (void)binary, (void)length;
    LOCKED(NONE, snx_gl_program_binary(ctx, program));
}
void gl_get_program_binary(CPU* c, u32 program, i32 size, u64 length, u64 format, u64 binary) {
    (void)program, (void)size, (void)format, (void)binary;
    LOCKED(NONE, if (length) gw32(length, 0); snx_gl_error(ctx, SNX_GL_INVALID_OPERATION));
}

/* ---- program pipelines ---- */

void gl_gen_program_pipelines(CPU* c, i32 n, u64 pipelines) {
    LOCKED(NONE, snx_gl_gen_pipelines(ctx, n, host(pipelines)));
}
void gl_delete_program_pipelines(CPU* c, i32 n, u64 pipelines) {
    LOCKED(NONE, snx_gl_delete_pipelines(ctx, n, host(pipelines)));
}
u8 gl_is_program_pipeline(CPU* c, u32 pipeline) {
    u8 is = 0;
    LOCKED(0, is = (u8)snx_gl_is_pipeline(pipeline));
    return is;
}
void gl_bind_program_pipeline(CPU* c, u32 pipeline) { LOCKED(NONE, snx_gl_bind_pipeline(ctx, pipeline)); }
void gl_use_program_stages(CPU* c, u32 pipeline, u32 stages, u32 program) {
    LOCKED(NONE, snx_gl_use_program_stages(ctx, pipeline, stages, program));
}
void gl_active_shader_program(CPU* c, u32 pipeline, u32 program) {
    LOCKED(NONE, snx_gl_active_shader_program(ctx, pipeline, program));
}
void gl_validate_program_pipeline(CPU* c, u32 pipeline) { LOCKED(NONE, snx_gl_validate_pipeline(ctx, pipeline)); }
void gl_get_program_pipelineiv(CPU* c, u32 pipeline, u32 pname, u64 params) {
    i32 v;
    LOCKED(NONE, if (snx_gl_get_pipelineiv(ctx, pipeline, pname, &v)) gw32(params, (u32)v));
}
void gl_get_program_pipeline_info_log(CPU* c, u32 pipeline, i32 size, u64 length, u64 log) {
    (void)c, (void)pipeline;
    snx_gl_string_out("", size, host(length), host(log));
}
u32 gl_create_shader_programv(CPU* c, u32 type, i32 count, u64 strings) {
    u32 program = 0;
    LOCKED(
        0, u32 shader = snx_gl_create_shader(ctx, type); if (shader) {
            source(ctx, shader, count, strings, 0);
            gl_program_layout_refresh();
            program = snx_gl_create_shader_program(ctx, shader);
        });
    return program;
}

/* ---- uniforms ---- */

#define UNIFORMS(n, k, t, kind, ...)                                                                                   \
    void gl_uniform##n##k(CPU* c, i32 location, __VA_ARGS__) {                                                         \
        const t v[n] = {UNIFORM_ARGS##n};                                                                              \
        LOCKED(NONE, snx_gl_uniform(ctx, location, 1, v, n, kind, 0));                                                 \
    }                                                                                                                  \
    void gl_uniform##n##k##v(CPU* c, i32 location, i32 count, u64 value) {                                             \
        LOCKED(NONE, snx_gl_uniform(ctx, location, count, host(value), n, kind, 0));                                   \
    }                                                                                                                  \
    void gl_program_uniform##n##k(CPU* c, u32 program, i32 location, __VA_ARGS__) {                                    \
        const t v[n] = {UNIFORM_ARGS##n};                                                                              \
        LOCKED(NONE, snx_gl_program_uniform(ctx, program, location, 1, v, n, kind, 0));                                \
    }                                                                                                                  \
    void gl_program_uniform##n##k##v(CPU* c, u32 program, i32 location, i32 count, u64 value) {                        \
        LOCKED(NONE, snx_gl_program_uniform(ctx, program, location, count, host(value), n, kind, 0));                  \
    }
#define UNIFORM_ARGS1 v0
#define UNIFORM_ARGS2 v0, v1
#define UNIFORM_ARGS3 v0, v1, v2
#define UNIFORM_ARGS4 v0, v1, v2, v3
UNIFORMS(1, f, f32, 'f', f32 v0)
UNIFORMS(2, f, f32, 'f', f32 v0, f32 v1)
UNIFORMS(3, f, f32, 'f', f32 v0, f32 v1, f32 v2)
UNIFORMS(4, f, f32, 'f', f32 v0, f32 v1, f32 v2, f32 v3)
UNIFORMS(1, i, i32, 'i', i32 v0)
UNIFORMS(2, i, i32, 'i', i32 v0, i32 v1)
UNIFORMS(3, i, i32, 'i', i32 v0, i32 v1, i32 v2)
UNIFORMS(4, i, i32, 'i', i32 v0, i32 v1, i32 v2, i32 v3)
UNIFORMS(1, ui, u32, 'u', u32 v0)
UNIFORMS(2, ui, u32, 'u', u32 v0, u32 v1)
UNIFORMS(3, ui, u32, 'u', u32 v0, u32 v1, u32 v2)
UNIFORMS(4, ui, u32, 'u', u32 v0, u32 v1, u32 v2, u32 v3)
#define MATRIX(name, n)                                                                                                \
    void gl_uniform_matrix##name(CPU* c, i32 location, i32 count, u8 transpose, u64 value) {                           \
        LOCKED(NONE, snx_gl_uniform(ctx, location, count, host(value), n, 'f', transpose != 0));                       \
    }                                                                                                                  \
    void gl_program_uniform_matrix##name(CPU* c, u32 program, i32 location, i32 count, u8 transpose, u64 value) {      \
        LOCKED(NONE, snx_gl_program_uniform(ctx, program, location, count, host(value), n, 'f', transpose != 0));      \
    }
MATRIX(2, 4)
MATRIX(3, 9)
MATRIX(4, 16)
MATRIX(2x3, 6)
MATRIX(3x2, 6)
MATRIX(2x4, 8)
MATRIX(4x2, 8)
MATRIX(3x4, 12)
MATRIX(4x3, 12)
