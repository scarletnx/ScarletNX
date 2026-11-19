/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's EGL (gl_layer.h, gpu/gl/gl.hook's egl family): one display and one config (RGBA8888 with a 24-bit
   depth and 8-bit stencil buffer, window and pbuffer surfaces, OpenGL and OpenGL ES), contexts that are the shared GL
   layer's (a context's EGL handle is its name in common/gpu/gl/gl_state.h), and surfaces that are gl_host_nx.c's. A
   context is made at the version asked when the layer makes that version: GL 3.0 to 4.5 (the core profile) and ES 2.0,
   3.0 to 3.2, else EGL_BAD_MATCH, so an engine tries its next version as on any driver. Each thread has its bound API,
   current context and surfaces, and its EGL error, as EGL's are per thread */
#include "nx/gpu/gl/gl_layer.h"
#include "common/gpu/gl/gl_state.h"
#include "nx/hle/services/vi.h"
#include "nx/hle/hook/hook.h"
#include "gpu/gl/gl.hook.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include <stdatomic.h>
#include <string.h>

enum {
    EGL_SUCCESS = 0x3000,
    EGL_NOT_INITIALIZED = 0x3001,
    EGL_BAD_ALLOC = 0x3003,
    EGL_BAD_ATTRIBUTE = 0x3004,
    EGL_BAD_CONFIG = 0x3005,
    EGL_BAD_CONTEXT = 0x3006,
    EGL_BAD_DISPLAY = 0x3008,
    EGL_BAD_MATCH = 0x3009,
    EGL_BAD_PARAMETER = 0x300c,
    EGL_BAD_SURFACE = 0x300d,
    EGL_NONE = 0x3038,
    EGL_DONT_CARE = -1,
    EGL_CONFIG_ID = 0x3028,
    EGL_SURFACE_TYPE = 0x3033,
    EGL_RENDERABLE_TYPE = 0x3040,
    EGL_CONFORMANT = 0x3042,
    EGL_WIDTH = 0x3057,
    EGL_HEIGHT = 0x3056,
    EGL_DRAW = 0x3059,
    EGL_READ = 0x305a,
    EGL_RENDER_BUFFER = 0x3086,
    EGL_BACK_BUFFER = 0x3084,
    EGL_SWAP_BEHAVIOR = 0x3093,
    EGL_BUFFER_DESTROYED = 0x3095,
    EGL_CONTEXT_CLIENT_TYPE = 0x3097,
    EGL_CONTEXT_MAJOR_VERSION = 0x3098, /* EGL_CONTEXT_CLIENT_VERSION too */
    EGL_CONTEXT_MINOR_VERSION = 0x30fb,
    EGL_CONTEXT_FLAGS_KHR = 0x30fc,
    EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x30fd,
    EGL_CONTEXT_OPENGL_DEBUG = 0x31b0,
    EGL_OPENGL_ES_API = 0x30a0,
    EGL_OPENGL_API = 0x30a2,
    EGL_VENDOR = 0x3053,
    EGL_VERSION = 0x3054,
    EGL_EXTENSIONS = 0x3055,
    EGL_CLIENT_APIS = 0x308d,
};
#define DISPLAY 1u /* the display's handle */
#define CONFIG  1u /* the config's */

/* the config's attributes, and how a request compares with each: 0 at least the value, 1 every bit of it, 2 equal */
static const struct {
    i32 name, value;
    u8 rule;
} config[] = {
    {0x3020, 32, 0},     /* EGL_BUFFER_SIZE */
    {0x3021, 8, 0},      /* EGL_ALPHA_SIZE */
    {0x3022, 8, 0},      /* EGL_BLUE_SIZE */
    {0x3023, 8, 0},      /* EGL_GREEN_SIZE */
    {0x3024, 8, 0},      /* EGL_RED_SIZE */
    {0x3025, 24, 0},     /* EGL_DEPTH_SIZE */
    {0x3026, 8, 0},      /* EGL_STENCIL_SIZE */
    {0x3027, 0x3038, 2}, /* EGL_CONFIG_CAVEAT: EGL_NONE */
    {0x3028, 1, 2},      /* EGL_CONFIG_ID */
    {0x3029, 0, 2},      /* EGL_LEVEL */
    {0x302a, 16384, 0},  /* EGL_MAX_PBUFFER_HEIGHT */
    {0x302b, 16384 * 16384, 0},
    {0x302c, 16384, 0}, /* EGL_MAX_PBUFFER_WIDTH */
    {0x302d, 0, 2},     /* EGL_NATIVE_RENDERABLE */
    {0x302e, 0, 2},     /* EGL_NATIVE_VISUAL_ID */
    {0x302f, 0x3038, 2},
    {0x3031, 0, 0},      /* EGL_SAMPLES */
    {0x3032, 0, 0},      /* EGL_SAMPLE_BUFFERS */
    {0x3033, 0x5, 1},    /* EGL_SURFACE_TYPE: window and pbuffer */
    {0x3034, 0x3038, 2}, /* EGL_TRANSPARENT_TYPE */
    {0x3039, 0, 2},      /* EGL_BIND_TO_TEXTURE_RGB */
    {0x303a, 0, 2},      /* EGL_BIND_TO_TEXTURE_RGBA */
    {0x303b, 0, 2},      /* EGL_MIN_SWAP_INTERVAL */
    {0x303c, 4, 2},      /* EGL_MAX_SWAP_INTERVAL */
    {0x303d, 0, 0},      /* EGL_LUMINANCE_SIZE */
    {0x303e, 0, 0},      /* EGL_ALPHA_MASK_SIZE */
    {0x303f, 0x308e, 2}, /* EGL_COLOR_BUFFER_TYPE: EGL_RGB_BUFFER */
    {0x3040, 0x4d, 1},   /* EGL_RENDERABLE_TYPE: ES, ES 2, ES 3 and OpenGL */
    {0x3042, 0x4d, 1},   /* EGL_CONFORMANT */
};

static _Thread_local struct {
    u32 api, ctx, draw, read, error;
} t_egl;
/* the display's state and the swap interval, which the game's threads set, each on a cache line of its own */
static struct {
    _Atomic u32 initialized;
    char pad[60];
    _Atomic u32 interval;
    u64 answer; /* guest bytes a native window's query writes its answer to */
} g_gl_egl = {.interval = 1};

static u32 fail(u32 error) {
    t_egl.error = error;
    return 0;
}
static int display_ok(u64 dpy) {
    if (dpy != DISPLAY) return fail(EGL_BAD_DISPLAY), 0;
    if (!atomic_load_explicit(&g_gl_egl.initialized, memory_order_relaxed)) return fail(EGL_NOT_INITIALIZED), 0;
    return 1;
}
static void write_i32(u64 at, i32 v) {
    if (at) gw32(at, (u32)v);
}
static int config_index(i32 name) {
    for (size_t i = 0; i < sizeof config / sizeof *config; i++)
        if (config[i].name == name) return (int)i;
    return -1;
}

u32 gl_egl_context(void) { return t_egl.ctx; }
u32 gl_egl_draw(void) { return t_egl.draw; }

i32 gl_egl_get_error(CPU* c) {
    u32 e = t_egl.error ? t_egl.error : EGL_SUCCESS;
    (void)c;
    t_egl.error = EGL_SUCCESS;
    return (i32)e;
}
u64 gl_egl_get_display(CPU* c, u64 native) { return (void)c, (void)native, DISPLAY; }
u32 gl_egl_initialize(CPU* c, u64 dpy, u64 major, u64 minor) {
    (void)c;
    if (dpy != DISPLAY) return fail(EGL_BAD_DISPLAY);
    atomic_store_explicit(&g_gl_egl.initialized, 1, memory_order_relaxed);
    write_i32(major, 1);
    write_i32(minor, 5);
    return 1;
}
u32 gl_egl_terminate(CPU* c, u64 dpy) {
    (void)c;
    return dpy == DISPLAY ? 1 : fail(EGL_BAD_DISPLAY);
}
u64 gl_egl_query_string(CPU* c, u64 dpy, i32 name) {
    (void)c;
    if (!display_ok(dpy)) return 0;
    switch (name) {
        case EGL_VENDOR: return gl_host_string("NVIDIA");
        case EGL_VERSION: return gl_host_string("1.5 ScarletNX");
        case EGL_CLIENT_APIS: return gl_host_string("OpenGL OpenGL_ES");
        case EGL_EXTENSIONS: return gl_host_string("EGL_KHR_create_context EGL_KHR_surfaceless_context");
        default: return fail(EGL_BAD_PARAMETER);
    }
}
u32 gl_egl_bind_api(CPU* c, u32 api) {
    (void)c;
    if (api != EGL_OPENGL_API && api != EGL_OPENGL_ES_API) return fail(EGL_BAD_PARAMETER);
    t_egl.api = api;
    return 1;
}
u32 gl_egl_query_api(CPU* c) { return (void)c, t_egl.api ? t_egl.api : EGL_OPENGL_ES_API; }

/* eglGetConfigs and eglChooseConfig's answer: the config when it matches */
static u32 configs_out(u64 configs, i32 size, u64 count, int match) {
    if (!count) return fail(EGL_BAD_PARAMETER);
    gw32(count, (u32)(configs && size <= 0 ? 0 : match)); /* without a list, how many match */
    if (configs && size > 0 && match) gw64(configs, CONFIG);
    return 1;
}
u32 gl_egl_get_configs(CPU* c, u64 dpy, u64 configs, i32 size, u64 count) {
    (void)c;
    if (!display_ok(dpy)) return 0;
    return configs_out(configs, size, count, 1);
}
u32 gl_egl_choose_config(CPU* c, u64 dpy, u64 attribs, u64 configs, i32 size, u64 count) {
    int match = 1;
    (void)c;
    if (!display_ok(dpy)) return 0;
    for (u32 i = 0; attribs && i < 128; i++) {
        i32 name = (i32)gr32(attribs + 8 * i), value = (i32)gr32(attribs + 8 * i + 4);
        int k;
        if (name == EGL_NONE) break;
        k = config_index(name);
        if (k < 0) {
            WARN_LIMIT(8, "gl layer: eglChooseConfig asked for attribute 0x%x, which the layer's config doesn't have\n",
                       (unsigned)name);
            return fail(EGL_BAD_ATTRIBUTE);
        }
        if (value == EGL_DONT_CARE) continue;
        if (config[k].rule == 0 && value > config[k].value) match = 0;
        if (config[k].rule == 1 && (value & ~config[k].value)) match = 0;
        if (config[k].rule == 2 && value != config[k].value) match = 0;
    }
    return configs_out(configs, size, count, match);
}
u32 gl_egl_get_config_attrib(CPU* c, u64 dpy, u64 cfg, i32 attribute, u64 value) {
    int k = config_index(attribute);
    (void)c;
    if (!display_ok(dpy)) return 0;
    if (cfg != CONFIG) return fail(EGL_BAD_CONFIG);
    if (k < 0) return fail(EGL_BAD_ATTRIBUTE);
    write_i32(value, config[k].value);
    return 1;
}

/* the value of an attribute of a list ending at EGL_NONE, or dflt */
static i32 attrib(u64 list, i32 name, i32 dflt) {
    for (u32 i = 0; list && i < 128; i++) {
        i32 n = (i32)gr32(list + 8 * i);
        if (n == EGL_NONE) break;
        if (n == name) return (i32)gr32(list + 8 * i + 4);
    }
    return dflt;
}
u64 gl_egl_create_context(CPU* c, u64 dpy, u64 cfg, u64 share, u64 attribs) {
    u32 api = t_egl.api == EGL_OPENGL_API ? SNX_GL_API_DESKTOP : SNX_GL_API_ES;
    i32 major = attrib(attribs, EGL_CONTEXT_MAJOR_VERSION, api == SNX_GL_API_DESKTOP ? 4 : 1);
    i32 minor = attrib(attribs, EGL_CONTEXT_MINOR_VERSION, api == SNX_GL_API_DESKTOP && major == 4 ? 5 : 0);
    i32 profile = attrib(attribs, EGL_CONTEXT_OPENGL_PROFILE_MASK, 1);
    u32 debug = attrib(attribs, EGL_CONTEXT_OPENGL_DEBUG, 0) || (attrib(attribs, EGL_CONTEXT_FLAGS_KHR, 0) & 1);
    u32 version = major >= 0 && minor >= 0 && major < 10 && minor < 10 ? (u32)(major * 10 + minor) : 0, ctx;
    (void)c, (void)share;
    if (!display_ok(dpy)) return 0;
    if (cfg != CONFIG) return fail(EGL_BAD_CONFIG);
    /* the core profile alone: a context of a version before 3.2 has no profile, and the compatibility profile's
       fixed functions aren't the layer's */
    if (!snx_gl_version_made(api, version) || (api == SNX_GL_API_DESKTOP && version >= 32 && !(profile & 1))) {
        LOG_LIMIT(8,
                  "gl layer: eglCreateContext asked for %s %d.%d%s, which the layer doesn't make: EGL_BAD_MATCH, for "
                  "the game to try another version\n",
                  api == SNX_GL_API_DESKTOP ? "OpenGL" : "OpenGL ES", (int)major, (int)minor,
                  api == SNX_GL_API_DESKTOP && !(profile & 1) ? " (the compatibility profile)" : "");
        return fail(EGL_BAD_MATCH);
    }
    ctx = snx_gl_context_new(api, version, debug);
    if (!ctx) {
        WARN("gl layer: eglCreateContext: the layer holds %d contexts, all used: EGL_BAD_ALLOC\n", SNX_GL_CONTEXTS);
        return fail(EGL_BAD_ALLOC);
    }
    LOG("gl layer: context %u, %s %d.%d%s\n", ctx, api == SNX_GL_API_DESKTOP ? "OpenGL core" : "OpenGL ES", (int)major,
        (int)minor, debug ? ", debug" : "");
    return ctx;
}
u32 gl_egl_destroy_context(CPU* c, u64 dpy, u64 context) {
    (void)c;
    if (!display_ok(dpy)) return 0;
    if (!snx_gl_context_version((u32)context)) return fail(EGL_BAD_CONTEXT);
    if (t_egl.ctx == context) t_egl.ctx = 0;
    snx_gl_context_free((u32)context);
    return 1;
}
u32 gl_egl_query_context(CPU* c, u64 dpy, u64 context, i32 attribute, u64 value) {
    u32 version = snx_gl_context_version((u32)context);
    (void)c;
    if (!display_ok(dpy)) return 0;
    if (!version) return fail(EGL_BAD_CONTEXT);
    switch (attribute) {
        case EGL_CONFIG_ID: write_i32(value, (i32)CONFIG); return 1;
        case EGL_CONTEXT_CLIENT_TYPE:
            write_i32(value,
                      snx_gl_context_api((u32)context) == SNX_GL_API_DESKTOP ? EGL_OPENGL_API : EGL_OPENGL_ES_API);
            return 1;
        case EGL_CONTEXT_MAJOR_VERSION: write_i32(value, (i32)(version / 10)); return 1;
        case EGL_RENDER_BUFFER: write_i32(value, EGL_BACK_BUFFER); return 1;
        default: return fail(EGL_BAD_ATTRIBUTE);
    }
}

/* what the game's native window answers its query for what (ANativeWindow's query, through the game's own code): 0
   when the window is no ANativeWindow ('_wnd') or answers an error */
static u32 window_query(CPU* c, u64 window, i32 what) {
    u64 query;
    if (!window || !mem_user_mapped(window, 152) || gr32(window) != 0x5f776e64u) return 0;
    query = gr64(window + 144);
    if (!query) return 0;
    if (!g_gl_egl.answer) g_gl_egl.answer = gl_guest_pages(0x10000);
    gw32(g_gl_egl.answer, 0);
    hook_guest_call(c, query, window, (u64)(i64)what, g_gl_egl.answer, 0);
    return (i32)c->x[0] ? 0 : gr32(g_gl_egl.answer);
}
u64 gl_egl_create_window_surface(CPU* c, u64 dpy, u64 cfg, u64 window, u64 attribs) {
    u32 s, w, h, dw, dh, cw, ch;
    (void)attribs;
    if (!display_ok(dpy)) return 0;
    if (cfg != CONFIG) return fail(EGL_BAD_CONFIG);
    /* the native window's size as the game's driver reads it: its default size (NATIVE_WINDOW_DEFAULT_WIDTH and
       _HEIGHT), which is what the game set (native_window_set_buffers_user_dimensions), else its layer's from vi,
       1280x720 docked too (docked output scales it). One game sets 1920x1080 docked; another draws its last pass at
       1280x720 into the layer's */
    dw = window_query(c, window, 6), dh = window_query(c, window, 7);
    cw = window_query(c, window, 0), ch = window_query(c, window, 1);
    vi_window_size(&w, &h);
    if (dw && dh) w = dw, h = dh;
    LOG_LIMIT(8,
              "gl layer: a window surface of %ux%u: its native window's default size %ux%u (its buffers' %ux%u), "
              "vi's layer's taken where that is 0\n",
              w, h, dw, dh, cw, ch);
    s = gl_host_surface_new(w, h);
    return s ? s : fail(EGL_BAD_ALLOC);
}
u64 gl_egl_create_pbuffer_surface(CPU* c, u64 dpy, u64 cfg, u64 attribs) {
    i32 w = attrib(attribs, EGL_WIDTH, 0), h = attrib(attribs, EGL_HEIGHT, 0);
    u32 s;
    (void)c;
    if (!display_ok(dpy)) return 0;
    if (cfg != CONFIG) return fail(EGL_BAD_CONFIG);
    if (w < 0 || h < 0 || w > 16384 || h > 16384) return fail(EGL_BAD_PARAMETER);
    s = gl_host_surface_new(w ? (u32)w : 1, h ? (u32)h : 1);
    return s ? s : fail(EGL_BAD_ALLOC);
}
static int surface_ok(u64 surface) {
    u32 w = 0, h = 0;
    if (surface && surface <= GL_HOST_SURFACES) gl_host_surface_size((u32)surface, &w, &h);
    return w != 0;
}
u32 gl_egl_destroy_surface(CPU* c, u64 dpy, u64 surface) {
    (void)c;
    if (!display_ok(dpy)) return 0;
    if (!surface_ok(surface)) return fail(EGL_BAD_SURFACE);
    if (t_egl.draw == surface) t_egl.draw = 0;
    if (t_egl.read == surface) t_egl.read = 0;
    gl_host_surface_free((u32)surface);
    return 1;
}
u32 gl_egl_query_surface(CPU* c, u64 dpy, u64 surface, i32 attribute, u64 value) {
    u32 w = 0, h = 0;
    (void)c;
    if (!display_ok(dpy)) return 0;
    if (!surface_ok(surface)) return fail(EGL_BAD_SURFACE);
    gl_host_surface_size((u32)surface, &w, &h);
    switch (attribute) {
        case EGL_WIDTH: write_i32(value, (i32)w); return 1;
        case EGL_HEIGHT: write_i32(value, (i32)h); return 1;
        case EGL_CONFIG_ID: write_i32(value, (i32)CONFIG); return 1;
        case EGL_RENDER_BUFFER: write_i32(value, EGL_BACK_BUFFER); return 1;
        case EGL_SWAP_BEHAVIOR: write_i32(value, EGL_BUFFER_DESTROYED); return 1;
        default: return fail(EGL_BAD_ATTRIBUTE);
    }
}
u32 gl_egl_surface_attrib(CPU* c, u64 dpy, u64 surface, i32 attribute, i32 value) {
    (void)c, (void)attribute, (void)value;
    if (!display_ok(dpy)) return 0;
    return surface_ok(surface) ? 1 : fail(EGL_BAD_SURFACE);
}

u32 gl_egl_make_current(CPU* c, u64 dpy, u64 draw, u64 read, u64 context) {
    (void)c;
    if (!display_ok(dpy)) return 0;
    if (!context) {
        t_egl.ctx = t_egl.draw = t_egl.read = 0;
        return 1;
    }
    if (!snx_gl_context_version((u32)context)) return fail(EGL_BAD_CONTEXT);
    if ((draw && !surface_ok(draw)) || (read && !surface_ok(read))) return fail(EGL_BAD_SURFACE);
    t_egl.ctx = (u32)context;
    t_egl.draw = (u32)draw;
    t_egl.read = (u32)read;
    if (draw) { /* GL sets a context's viewport and scissor to its first draw surface's size */
        SnxGlFixed* f = snx_gl_fixed(t_egl.ctx);
        if (!f->viewport[2] && !f->viewport[3]) {
            u32 w, h;
            gl_host_surface_size((u32)draw, &w, &h);
            snx_gl_viewport(t_egl.ctx, 0, 0, (i32)w, (i32)h);
            snx_gl_scissor(t_egl.ctx, 0, 0, (i32)w, (i32)h);
        }
    }
    return 1;
}
u64 gl_egl_get_current_context(CPU* c) { return (void)c, t_egl.ctx; }
u64 gl_egl_get_current_display(CPU* c) { return (void)c, t_egl.ctx ? DISPLAY : 0; }
u64 gl_egl_get_current_surface(CPU* c, i32 which) {
    (void)c;
    if (which == EGL_DRAW) return t_egl.draw;
    if (which == EGL_READ) return t_egl.read;
    return fail(EGL_BAD_PARAMETER);
}
u32 gl_egl_swap_buffers(CPU* c, u64 dpy, u64 surface) {
    (void)c;
    if (!display_ok(dpy)) return 0;
    if (!surface_ok(surface)) return fail(EGL_BAD_SURFACE);
    gl_host_present((u32)surface, atomic_load_explicit(&g_gl_egl.interval, memory_order_relaxed));
    return 1;
}
u32 gl_egl_swap_interval(CPU* c, u64 dpy, i32 interval) {
    (void)c;
    if (!display_ok(dpy)) return 0;
    /* EGL holds it to the config's EGL_MIN_SWAP_INTERVAL and EGL_MAX_SWAP_INTERVAL */
    atomic_store_explicit(&g_gl_egl.interval,
                          (u32)(interval < 0   ? 0
                                : interval > 4 ? 4
                                               : interval),
                          memory_order_relaxed);
    return 1;
}
u32 gl_egl_wait_client(CPU* c) { return (void)c, 1; }
u32 gl_egl_wait_gl(CPU* c) { return (void)c, 1; }
u32 gl_egl_wait_native(CPU* c, i32 engine) { return (void)c, (void)engine, 1; }
u32 gl_egl_release_thread(CPU* c) {
    (void)c;
    t_egl.ctx = t_egl.draw = t_egl.read = 0;
    t_egl.error = EGL_SUCCESS;
    return 1;
}
u64 gl_egl_get_proc_address(CPU* c, u64 name) {
    char text[160];
    int len = mem_user_string(text, name, sizeof text);
    u64 at;
    (void)c;
    if (len <= 1) return 0;
    at = gl_loader_lookup(text);
    if (!at) LOG_LIMIT(32, "gl layer: eglGetProcAddress(%s): the game's driver exports no such function: 0\n", text);
    return at;
}
