/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* libEGL and libGLESv2, pending on purpose (docs/android/NATIVE.md, "EGL and GLES"). Only the GPU's identity is
   there: engines name the GPU from a throwaway context's glGetString(GL_RENDERER) before they choose an API, and
   choose Vulkan only on the GPUs of their own lists. So the display initializes and makes contexts without a config
   (EGL_KHR_no_config_context) or a surface, which glGetString answers with the device that Vulkan reports. Nothing
   draws: eglChooseConfig finds no config, a window surface fails, and the other GLES functions are absent (each a
   missing export that returns 0). GLES over Vulkan is a later step */
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include <string.h>

enum {
    EGL_FALSE_ = 0,
    EGL_TRUE_ = 1,
    EGL_SUCCESS_ = 0x3000,
    EGL_NOT_INITIALIZED_ = 0x3001,
    EGL_BAD_CONFIG_ = 0x3005,
    EGL_BAD_CONTEXT_ = 0x3006,
    EGL_BAD_DISPLAY_ = 0x3008,
    EGL_BAD_MATCH_ = 0x3009,
    EGL_BAD_PARAMETER_ = 0x300c,
    EGL_BAD_SURFACE_ = 0x300d,
    EGL_VENDOR_ = 0x3053,
    EGL_VERSION_ = 0x3054,
    EGL_EXTENSIONS_ = 0x3055,
    EGL_CLIENT_APIS_ = 0x308d,
    GL_VENDOR_ = 0x1f00,
    GL_RENDERER_ = 0x1f01,
    GL_VERSION_ = 0x1f02,
    GL_EXTENSIONS_ = 0x1f03,
    GL_SHADING_LANGUAGE_VERSION_ = 0x8b8c,
};

/* the one display, and the contexts: opaque handles the guest only passes back. A context is the address of a byte
   of contexts[], so any number of them may exist at once and each is told from the display */
static unsigned char display;
static unsigned char contexts[64];
static _Thread_local uint64_t t_current; /* the calling thread's current context */
static _Thread_local int t_error = EGL_SUCCESS_;

static uint64_t handle(const void* p) {
    return (uint64_t)(uintptr_t)p;
}
static bool is_display(uint64_t d) {
    return d == handle(&display);
}
static bool is_context(uint64_t x) {
    return x >= handle(contexts) && x < handle(contexts) + sizeof contexts;
}
/* an EGL result, with the error that eglGetError returns next */
static void done(CPU* c, uint64_t result, int error) {
    t_error = error;
    X(0) = result;
}

static void g_get_display(CPU* c, void* data) {
    (void)data;
    X(0) = handle(&display); /* the default display, and any native one */
}
static void g_get_platform_display(CPU* c, void* data) {
    (void)data;
    X(0) = handle(&display);
}
/* eglInitialize(display, major, minor): EGL 1.5 */
static void g_initialize(CPU* c, void* data) {
    (void)data;
    if (!is_display(X(0))) {
        done(c, EGL_FALSE_, EGL_BAD_DISPLAY_);
        return;
    }
    int32_t major = 1, minor = 5;
    if (X(1)) memcpy(PTR(1), &major, 4);
    if (X(2)) memcpy(PTR(2), &minor, 4);
    done(c, EGL_TRUE_, EGL_SUCCESS_);
}
static void g_terminate(CPU* c, void* data) {
    (void)data;
    done(c, is_display(X(0)) ? EGL_TRUE_ : EGL_FALSE_, is_display(X(0)) ? EGL_SUCCESS_ : EGL_BAD_DISPLAY_);
}
/* eglChooseConfig(display, attributes, configs, size, count): success, with no config */
static void g_choose_config(CPU* c, void* data) {
    (void)data;
    if (!is_display(X(0)) || !X(4)) {
        done(c, EGL_FALSE_, is_display(X(0)) ? EGL_BAD_PARAMETER_ : EGL_BAD_DISPLAY_);
        return;
    }
    int32_t none = 0;
    memcpy(PTR(4), &none, 4);
    done(c, EGL_TRUE_, EGL_SUCCESS_);
}
/* eglGetConfigs(display, configs, size, count): no config either */
static void g_get_configs(CPU* c, void* data) {
    (void)data;
    if (!is_display(X(0)) || !X(3)) {
        done(c, EGL_FALSE_, is_display(X(0)) ? EGL_BAD_PARAMETER_ : EGL_BAD_DISPLAY_);
        return;
    }
    int32_t none = 0;
    memcpy(PTR(3), &none, 4);
    done(c, EGL_TRUE_, EGL_SUCCESS_);
}
static void g_bad_config(CPU* c, void* data) {
    (void)data;
    done(c, 0, EGL_BAD_CONFIG_);
}
/* eglCreateContext(display, config, share, attributes): only without a config (EGL_NO_CONFIG_KHR) */
static void g_create_context(CPU* c, void* data) {
    (void)data;
    if (!is_display(X(0))) {
        done(c, 0, EGL_BAD_DISPLAY_);
        return;
    }
    if (X(1)) {
        done(c, 0, EGL_BAD_CONFIG_);
        return;
    }
    static unsigned next;
    unsigned i = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) % sizeof contexts;
    done(c, handle(&contexts[i]), EGL_SUCCESS_);
}
static void g_destroy_context(CPU* c, void* data) {
    (void)data;
    bool ok = is_display(X(0)) && is_context(X(1));
    if (ok && t_current == X(1)) t_current = 0;
    done(c, ok ? EGL_TRUE_ : EGL_FALSE_, ok ? EGL_SUCCESS_ : EGL_BAD_CONTEXT_);
}
/* eglMakeCurrent(display, draw, read, context): a context without surfaces (EGL_KHR_surfaceless_context), or none */
static void g_make_current(CPU* c, void* data) {
    (void)data;
    if (X(3) && !is_display(X(0))) {
        done(c, EGL_FALSE_, EGL_BAD_DISPLAY_);
        return;
    }
    if (X(1) || X(2)) {
        done(c, EGL_FALSE_, EGL_BAD_SURFACE_);
        return;
    }
    if (X(3) && !is_context(X(3))) {
        done(c, EGL_FALSE_, EGL_BAD_CONTEXT_);
        return;
    }
    t_current = X(3);
    done(c, EGL_TRUE_, EGL_SUCCESS_);
}
static void g_get_current_context(CPU* c, void* data) {
    (void)data;
    X(0) = t_current;
}
static void g_get_current_display(CPU* c, void* data) {
    (void)data;
    X(0) = t_current ? handle(&display) : 0;
}
static void g_no_surface(CPU* c, void* data) {
    (void)data;
    X(0) = 0; /* EGL_NO_SURFACE */
}
static void g_bad_surface(CPU* c, void* data) {
    (void)data;
    done(c, EGL_FALSE_, EGL_BAD_SURFACE_);
}
static void g_bad_match(CPU* c, void* data) {
    (void)data;
    done(c, EGL_FALSE_, EGL_BAD_MATCH_);
}
static void g_get_error(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)t_error;
    t_error = EGL_SUCCESS_;
}
static void g_query_string(CPU* c, void* data) {
    (void)data;
    const char* s = NULL;
    switch ((int)X(1)) {
    case EGL_VENDOR_: s = "ARM"; break;
    case EGL_VERSION_: s = "1.5 Android META-EGL"; break;
    case EGL_EXTENSIONS_: s = X(0) ? "EGL_KHR_no_config_context EGL_KHR_surfaceless_context" : ""; break;
    case EGL_CLIENT_APIS_: s = "OpenGL_ES"; break;
    }
    if (X(0) && !is_display(X(0))) s = NULL;
    t_error = s ? EGL_SUCCESS_ : EGL_BAD_PARAMETER_;
    X(0) = (uint64_t)(uintptr_t)s;
}

/* glGetString: the device's identity in the current context, NULL without one (GL_INVALID_ENUM's NULL for others) */
static void g_gl_get_string(CPU* c, void* data) {
    (void)data;
    const char* s = NULL;
    if (t_current) switch ((int)X(0)) {
        case GL_VENDOR_: s = "ARM"; break;
        case GL_RENDERER_: s = hle_gpu_name; break;
        case GL_VERSION_: s = "OpenGL ES 3.2"; break;
        case GL_SHADING_LANGUAGE_VERSION_: s = "OpenGL ES GLSL ES 3.20"; break;
        case GL_EXTENSIONS_: s = ""; break;
        }
    X(0) = (uint64_t)(uintptr_t)s;
}

/* eglGetProcAddress: glGetString alone, as the GLES functions that draw are absent */
static void g_get_proc_address(CPU* c, void* data) {
    (void)data;
    X(0) = X(0) && !strcmp(STR(0), "glGetString") ? hle_symbol("glGetString") : 0;
}

const HleSymbol hle_egl[] = {
    {"eglGetDisplay", g_get_display, NULL},
    {"eglGetPlatformDisplay", g_get_platform_display, NULL},
    {"eglInitialize", g_initialize, NULL},
    {"eglTerminate", g_terminate, NULL},
    {"eglQueryString", g_query_string, NULL},
    {"eglGetProcAddress", g_get_proc_address, NULL},
    {"eglChooseConfig", g_choose_config, NULL},
    {"eglGetConfigs", g_get_configs, NULL},
    {"eglGetConfigAttrib", g_bad_config, NULL},
    {"eglCreateContext", g_create_context, NULL},
    {"eglDestroyContext", g_destroy_context, NULL},
    {"eglMakeCurrent", g_make_current, NULL},
    {"eglGetCurrentContext", g_get_current_context, NULL},
    {"eglGetCurrentDisplay", g_get_current_display, NULL},
    {"eglGetCurrentSurface", g_no_surface, NULL},
    {"eglCreateWindowSurface", g_bad_config, NULL},
    {"eglCreatePbufferSurface", g_bad_config, NULL},
    {"eglDestroySurface", g_bad_surface, NULL},
    {"eglQuerySurface", g_bad_surface, NULL},
    {"eglSwapBuffers", g_bad_surface, NULL},
    {"eglSwapInterval", g_bad_match, NULL},
    {"eglGetError", g_get_error, NULL},
    {"glGetString", g_gl_get_string, NULL},
    {"", NULL, NULL},
};
