/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* libEGL (docs/android/NATIVE.md, "EGL and GLES"), for the guest's native code and for Java's EGL10
   (snx.android.opengl.Egl10), which share its objects: a context or surface that Java makes is current for the
   native GLES calls of the same thread, as on Android.

   The configs are GLES 1.1's, 2's and 3's (EGL_OPENGL_ES_BIT, EGL_OPENGL_ES2_BIT, EGL_OPENGL_ES3_BIT_KHR), with
   window surfaces of the host window and pbuffers. A context of version 1 draws with GLES 1.1 over Vulkan
   (hle_gles.c), one of 2 or 3 with GLES 3.0 over Vulkan (gles2.h). Engines name the GPU from a throwaway context's glGetString(GL_RENDERER) before they choose an API, and choose Vulkan only
   on the GPUs of their own lists: so a context without a config (EGL_KHR_no_config_context) of any version can be
   made current without a surface, and glGetString answers with the device that Vulkan reports */
#include "android/bridge/hle.h"
#include "android/bridge/gles2.h"
#include "android/bridge/hle_gles.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/jni.h"
#include "android/runtime/jvm.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum {
    EGL_FALSE_ = 0,
    EGL_TRUE_ = 1,
    EGL_DONT_CARE_ = -1,
    EGL_SUCCESS_ = 0x3000,
    EGL_NOT_INITIALIZED_ = 0x3001,
    EGL_BAD_ALLOC_ = 0x3003,
    EGL_BAD_ATTRIBUTE_ = 0x3004,
    EGL_BAD_CONFIG_ = 0x3005,
    EGL_BAD_CONTEXT_ = 0x3006,
    EGL_BAD_DISPLAY_ = 0x3008,
    EGL_BAD_MATCH_ = 0x3009,
    EGL_BAD_NATIVE_WINDOW_ = 0x300b,
    EGL_BAD_PARAMETER_ = 0x300c,
    EGL_BAD_SURFACE_ = 0x300d,
    EGL_BUFFER_SIZE_ = 0x3020,
    EGL_ALPHA_SIZE_ = 0x3021,
    EGL_BLUE_SIZE_ = 0x3022,
    EGL_GREEN_SIZE_ = 0x3023,
    EGL_RED_SIZE_ = 0x3024,
    EGL_DEPTH_SIZE_ = 0x3025,
    EGL_STENCIL_SIZE_ = 0x3026,
    EGL_CONFIG_CAVEAT_ = 0x3027,
    EGL_CONFIG_ID_ = 0x3028,
    EGL_LEVEL_ = 0x3029,
    EGL_MAX_PBUFFER_HEIGHT_ = 0x302a,
    EGL_MAX_PBUFFER_PIXELS_ = 0x302b,
    EGL_MAX_PBUFFER_WIDTH_ = 0x302c,
    EGL_NATIVE_RENDERABLE_ = 0x302d,
    EGL_NATIVE_VISUAL_ID_ = 0x302e,
    EGL_NATIVE_VISUAL_TYPE_ = 0x302f,
    EGL_SAMPLES_ = 0x3031,
    EGL_SAMPLE_BUFFERS_ = 0x3032,
    EGL_SURFACE_TYPE_ = 0x3033,
    EGL_TRANSPARENT_TYPE_ = 0x3034,
    EGL_TRANSPARENT_BLUE_VALUE_ = 0x3035,
    EGL_TRANSPARENT_GREEN_VALUE_ = 0x3036,
    EGL_TRANSPARENT_RED_VALUE_ = 0x3037,
    EGL_NONE_ = 0x3038,
    EGL_BIND_TO_TEXTURE_RGB_ = 0x3039,
    EGL_BIND_TO_TEXTURE_RGBA_ = 0x303a,
    EGL_MIN_SWAP_INTERVAL_ = 0x303b,
    EGL_MAX_SWAP_INTERVAL_ = 0x303c,
    EGL_LUMINANCE_SIZE_ = 0x303d,
    EGL_ALPHA_MASK_SIZE_ = 0x303e,
    EGL_COLOR_BUFFER_TYPE_ = 0x303f,
    EGL_RENDERABLE_TYPE_ = 0x3040,
    EGL_CONFORMANT_ = 0x3042,
    EGL_VENDOR_ = 0x3053,
    EGL_VERSION_ = 0x3054,
    EGL_EXTENSIONS_ = 0x3055,
    EGL_HEIGHT_ = 0x3056,
    EGL_WIDTH_ = 0x3057,
    EGL_DRAW_ = 0x3059,
    EGL_READ_ = 0x305a,
    EGL_BACK_BUFFER_ = 0x3084,
    EGL_RENDER_BUFFER_ = 0x3086,
    EGL_CLIENT_APIS_ = 0x308d,
    EGL_RGB_BUFFER_ = 0x308e,
    EGL_SWAP_BEHAVIOR_ = 0x3093,
    EGL_BUFFER_DESTROYED_ = 0x3095,
    EGL_CONTEXT_CLIENT_TYPE_ = 0x3097,
    EGL_CONTEXT_CLIENT_VERSION_ = 0x3098,
    EGL_OPENGL_ES_API_ = 0x30a0,
    EGL_RECORDABLE_ANDROID_ = 0x3142,
    EGL_PBUFFER_BIT_ = 0x01,
    EGL_WINDOW_BIT_ = 0x04,
    EGL_OPENGL_ES_BIT_ = 0x01,
    EGL_OPENGL_ES2_BIT_ = 0x04,
    EGL_OPENGL_ES3_BIT_ = 0x40,
    EGL_ES_BITS_ = EGL_OPENGL_ES_BIT_ | EGL_OPENGL_ES2_BIT_ | EGL_OPENGL_ES3_BIT_,
    EGL_SURFACE_BITS_ = EGL_WINDOW_BIT_ | EGL_PBUFFER_BIT_,
    EGL_CONTEXT_MINOR_VERSION_ = 0x30fb,
    EGL_LARGEST_PBUFFER_ = 0x3058,
    GL_VENDOR_ = 0x1f00,
    GL_RENDERER_ = 0x1f01,
    GL_VERSION_ = 0x1f02,
    GL_EXTENSIONS_ = 0x1f03,
    GL_SHADING_LANGUAGE_VERSION_ = 0x8b8c,
};

/* the configs: the window's color as GLES 1.1 sees it, with the depth and stencil sizes apps ask for (the depth and
   stencil tests have no effect, hle_gles.c). The native visual is Android's HAL pixel format */
typedef struct {
    int32_t id, red, green, blue, alpha, depth, stencil, visual;
} Config;
static const Config configs[] = {
    {1, 8, 8, 8, 8, 0, 0, 1},  {2, 8, 8, 8, 8, 16, 0, 1}, {3, 8, 8, 8, 8, 24, 8, 1},
    {4, 8, 8, 8, 0, 0, 0, 2},  {5, 8, 8, 8, 0, 16, 0, 2}, {6, 8, 8, 8, 0, 24, 8, 2},
    {7, 5, 6, 5, 0, 0, 0, 4},  {8, 5, 6, 5, 0, 16, 0, 4}, {9, 5, 6, 5, 0, 24, 8, 4},
};
enum { CONFIGS = sizeof configs / sizeof *configs, CONTEXTS = 64, SURFACES = 16 };

typedef struct {
    bool used;
    int version;
    const Config* config; /* NULL: made without one */
    GlesContext* gl;      /* GLES 1.1's state, for version 1 */
    Gles2Context* gl2;    /* GLES 2 and 3's, for versions 2 and 3 */
} Context;
typedef struct {
    bool used;
    const Config* config;
    GlesSurface* gles;
} Surface;

/* the one display, and the objects: a handle is the address of its entry, so it is told from the others */
static struct {
    pthread_mutex_t lock;
    unsigned char display;
    Context contexts[CONTEXTS];
    Surface surfaces[SURFACES];
    bool said[4]; /* the versions whose first context was said */
} e = {.lock = PTHREAD_MUTEX_INITIALIZER};
static _Thread_local Context* t_context; /* the calling thread's current ones */
static _Thread_local Surface *t_draw, *t_read;
static _Thread_local int t_error = EGL_SUCCESS_;

static uint64_t handle(const void* p) {
    return (uint64_t)(uintptr_t)p;
}
static bool is_display(uint64_t d) {
    return d == handle(&e.display);
}
static const Config* config_of(uint64_t h) {
    for (size_t i = 0; i < CONFIGS; ++i)
        if (h == handle(&configs[i])) return &configs[i];
    return NULL;
}
static Context* context_of(uint64_t h) {
    for (size_t i = 0; i < CONTEXTS; ++i)
        if (h == handle(&e.contexts[i]) && e.contexts[i].used) return &e.contexts[i];
    return NULL;
}
static Surface* surface_of(uint64_t h) {
    for (size_t i = 0; i < SURFACES; ++i)
        if (h == handle(&e.surfaces[i]) && e.surfaces[i].used) return &e.surfaces[i];
    return NULL;
}
/* an EGL result, with the error that eglGetError returns next */
static uint64_t done(uint64_t result, int error) {
    t_error = error;
    return result;
}

/* ---- the calls, with their arguments as the host's: the guest's exports and Java's natives both call them ---- */

static bool config_attribute(const Config* c, int32_t attribute, int32_t* value) {
    switch (attribute) {
    case EGL_BUFFER_SIZE_: *value = c->red + c->green + c->blue + c->alpha; break;
    case EGL_RED_SIZE_: *value = c->red; break;
    case EGL_GREEN_SIZE_: *value = c->green; break;
    case EGL_BLUE_SIZE_: *value = c->blue; break;
    case EGL_ALPHA_SIZE_: *value = c->alpha; break;
    case EGL_DEPTH_SIZE_: *value = c->depth; break;
    case EGL_STENCIL_SIZE_: *value = c->stencil; break;
    case EGL_CONFIG_CAVEAT_: *value = EGL_NONE_; break;
    case EGL_CONFIG_ID_: *value = c->id; break;
    case EGL_NATIVE_VISUAL_ID_: *value = c->visual; break;
    case EGL_SURFACE_TYPE_: *value = EGL_SURFACE_BITS_; break;
    case EGL_TRANSPARENT_TYPE_: *value = EGL_NONE_; break;
    case EGL_MIN_SWAP_INTERVAL_:
    case EGL_MAX_SWAP_INTERVAL_: *value = 1; break;
    case EGL_COLOR_BUFFER_TYPE_: *value = EGL_RGB_BUFFER_; break;
    case EGL_RENDERABLE_TYPE_:
    case EGL_CONFORMANT_: *value = EGL_ES_BITS_; break;
    case EGL_RECORDABLE_ANDROID_: *value = EGL_TRUE_; break;
    case EGL_LEVEL_:
    case EGL_MAX_PBUFFER_HEIGHT_:
    case EGL_MAX_PBUFFER_WIDTH_: *value = 4096; break;
    case EGL_MAX_PBUFFER_PIXELS_: *value = 4096 * 4096; break;
    case EGL_NATIVE_RENDERABLE_:
    case EGL_NATIVE_VISUAL_TYPE_:
    case EGL_SAMPLES_:
    case EGL_SAMPLE_BUFFERS_:
    case EGL_TRANSPARENT_RED_VALUE_:
    case EGL_TRANSPARENT_GREEN_VALUE_:
    case EGL_TRANSPARENT_BLUE_VALUE_:
    case EGL_BIND_TO_TEXTURE_RGB_:
    case EGL_BIND_TO_TEXTURE_RGBA_:
    case EGL_LUMINANCE_SIZE_:
    case EGL_ALPHA_MASK_SIZE_: *value = 0; break;
    default: return false;
    }
    return true;
}

/* whether a config has what the list of attributes asks for, as eglChooseConfig matches (sizes at least, masks
   all of their bits, the others exactly), the requested color bits into *bits */
static bool config_matches(const Config* c, const int32_t* attributes, int* bits) {
    int32_t surface = EGL_WINDOW_BIT_, renderable = EGL_OPENGL_ES_BIT_;
    *bits = 0;
    for (const int32_t* a = attributes; a && a[0] != EGL_NONE_; a += 2) {
        int32_t want = a[1], have = 0;
        switch (a[0]) {
        case EGL_SURFACE_TYPE_: surface = want; continue;
        case EGL_RENDERABLE_TYPE_: renderable = want; continue;
        case EGL_CONFORMANT_:
            if (want != EGL_DONT_CARE_ && (want & ~EGL_ES_BITS_)) return false;
            continue;
        case EGL_RED_SIZE_:
        case EGL_GREEN_SIZE_:
        case EGL_BLUE_SIZE_:
        case EGL_ALPHA_SIZE_:
            config_attribute(c, a[0], &have);
            if (want != EGL_DONT_CARE_ && want > 0) *bits += have;
            if (want != EGL_DONT_CARE_ && have < want) return false;
            continue;
        case EGL_BUFFER_SIZE_:
        case EGL_DEPTH_SIZE_:
        case EGL_STENCIL_SIZE_:
        case EGL_SAMPLES_:
        case EGL_SAMPLE_BUFFERS_:
        case EGL_LUMINANCE_SIZE_:
        case EGL_ALPHA_MASK_SIZE_:
            config_attribute(c, a[0], &have);
            if (want != EGL_DONT_CARE_ && have < want) return false;
            continue;
        case EGL_CONFIG_ID_:
        case EGL_CONFIG_CAVEAT_:
        case EGL_COLOR_BUFFER_TYPE_:
        case EGL_NATIVE_VISUAL_TYPE_:
        case EGL_TRANSPARENT_TYPE_:
        case EGL_LEVEL_:
        case EGL_NATIVE_RENDERABLE_:
            config_attribute(c, a[0], &have);
            if (want != EGL_DONT_CARE_ && have != want) return false;
            continue;
        default: continue; /* the others hold for every config here */
        }
    }
    /* EGL_SWAP_BEHAVIOR_PRESERVED_BIT (0x400) and the others are not here */
    return (surface == EGL_DONT_CARE_ || !(surface & ~EGL_SURFACE_BITS_)) &&
           (renderable == EGL_DONT_CARE_ || !(renderable & ~EGL_ES_BITS_));
}

/* eglChooseConfig: the configs that match, in EGL's order (more of the requested color bits first, then a smaller
   buffer, then smaller depth and stencil buffers, then the ID), up to size of them into out; their count */
static int choose_configs(const int32_t* attributes, const Config** out, int size) {
    const Config* found[CONFIGS];
    int bits[CONFIGS], n = 0;
    for (size_t i = 0; i < CONFIGS; ++i) {
        int b;
        if (config_matches(&configs[i], attributes, &b)) {
            found[n] = &configs[i];
            bits[n++] = b;
        }
    }
    for (int i = 1; i < n; ++i) /* insertion sort: nine configs */
        for (int k = i; k > 0; --k) {
            const Config *a = found[k - 1], *b = found[k];
            int buffer_a = a->red + a->green + a->blue + a->alpha, buffer_b = b->red + b->green + b->blue + b->alpha;
            bool later = bits[k - 1] != bits[k]         ? bits[k - 1] < bits[k]
                         : buffer_a != buffer_b         ? buffer_a > buffer_b
                         : a->depth != b->depth         ? a->depth > b->depth
                         : a->stencil != b->stencil     ? a->stencil > b->stencil
                                                        : a->id > b->id;
            if (!later) break;
            found[k - 1] = b;
            found[k] = a;
            int t = bits[k - 1];
            bits[k - 1] = bits[k];
            bits[k] = t;
        }
    for (int i = 0; out && i < n && i < size; ++i) out[i] = found[i];
    return out && n > size ? size : n;
}

/* eglCreateContext: a config's or one without a config, of the version the attributes ask: GLES 1.1, or GLES 2
   and 3.0 (gles2.h). A minor version past 3.0 is not here: an engine then asks for 3.0 */
static uint64_t create_context(uint64_t display, uint64_t config, const int32_t* attributes) {
    if (!is_display(display)) return done(0, EGL_BAD_DISPLAY_);
    const Config* c = config ? config_of(config) : NULL;
    if (config && !c) return done(0, EGL_BAD_CONFIG_);
    int version = 1, minor = 0;
    for (const int32_t* a = attributes; a && a[0] != EGL_NONE_; a += 2) {
        if (a[0] == EGL_CONTEXT_CLIENT_VERSION_) version = a[1];
        if (a[0] == EGL_CONTEXT_MINOR_VERSION_) minor = a[1];
    }
    if (version < 1 || version > 3 || (version == 3 && minor > 0) || (version == 1 && minor > 1))
        return done(0, EGL_BAD_MATCH_);
    if (!e.said[version]) { /* each version's first context, said once */
        e.said[version] = true;
        fprintf(stderr, "scarletnx-android: EGL: a context of GLES %d%s\n", version, c ? "" : ", without a config");
    }
    pthread_mutex_lock(&e.lock);
    Context* x = NULL;
    for (size_t i = 0; i < CONTEXTS && !x; ++i)
        if (!e.contexts[i].used) x = &e.contexts[i];
    if (x)
        *x = (Context){.used = true, .version = version, .config = c, .gl = version == 1 ? gles_context_new() : NULL,
                       .gl2 = version >= 2 ? gles2_context_new(version) : NULL};
    pthread_mutex_unlock(&e.lock);
    return x ? done(handle(x), EGL_SUCCESS_) : done(0, EGL_BAD_ALLOC_);
}
static bool destroy_context(uint64_t display, uint64_t context) {
    pthread_mutex_lock(&e.lock);
    Context* x = is_display(display) ? context_of(context) : NULL;
    if (x) {
        if (t_context == x) {
            t_context = NULL;
            gles_make_current(NULL, NULL);
            gles2_make_current(NULL, NULL, NULL);
        }
        gles_context_free(x->gl);
        gles2_context_free(x->gl2);
        *x = (Context){0};
    }
    pthread_mutex_unlock(&e.lock);
    return done(x != NULL, x ? EGL_SUCCESS_ : EGL_BAD_CONTEXT_);
}

static uint64_t add_surface(const Config* c, GlesSurface* gles);
/* eglCreateWindowSurface: the host window, width by height where its surface leaves the size to the swapchain */
static uint64_t create_window_surface(uint64_t display, uint64_t config, int width, int height) {
    if (!is_display(display)) return done(0, EGL_BAD_DISPLAY_);
    const Config* c = config_of(config);
    if (!c) return done(0, EGL_BAD_CONFIG_);
    GlesSurface* gles = gles_window_surface(width, height);
    if (!gles) return done(0, EGL_BAD_NATIVE_WINDOW_);
    gles_surface_depth(gles, c->depth, c->stencil);
    return add_surface(c, gles);
}
/* eglCreatePbufferSurface: an image of its own, the attributes' width by height */
static uint64_t create_pbuffer_surface(uint64_t display, uint64_t config, const int32_t* attributes) {
    if (!is_display(display)) return done(0, EGL_BAD_DISPLAY_);
    const Config* c = config_of(config);
    if (!c) return done(0, EGL_BAD_CONFIG_);
    int width = 0, height = 0;
    for (const int32_t* a = attributes; a && a[0] != EGL_NONE_; a += 2) {
        if (a[0] == EGL_WIDTH_) width = a[1];
        if (a[0] == EGL_HEIGHT_) height = a[1];
    }
    if (width < 0 || height < 0) return done(0, EGL_BAD_PARAMETER_);
    GlesSurface* gles = gles_pbuffer_surface(width ? width : 1, height ? height : 1);
    if (!gles) return done(0, EGL_BAD_ALLOC_);
    gles_surface_depth(gles, c->depth, c->stencil);
    return add_surface(c, gles);
}
static uint64_t add_surface(const Config* c, GlesSurface* gles) {
    pthread_mutex_lock(&e.lock);
    Surface* s = NULL;
    for (size_t i = 0; i < SURFACES && !s; ++i)
        if (!e.surfaces[i].used) s = &e.surfaces[i];
    if (s) *s = (Surface){.used = true, .config = c, .gles = gles};
    pthread_mutex_unlock(&e.lock);
    if (!s) {
        gles_surface_free(gles);
        return done(0, EGL_BAD_ALLOC_);
    }
    return done(handle(s), EGL_SUCCESS_);
}
static bool destroy_surface(uint64_t display, uint64_t surface) {
    pthread_mutex_lock(&e.lock);
    Surface* s = is_display(display) ? surface_of(surface) : NULL;
    GlesSurface* gles = s ? s->gles : NULL;
    if (s) {
        if (t_draw == s || t_read == s) {
            t_draw = t_read = NULL;
            gles_make_current(t_context ? t_context->gl : NULL, NULL);
            gles2_make_current(t_context ? t_context->gl2 : NULL, NULL, NULL);
        }
        *s = (Surface){0};
    }
    pthread_mutex_unlock(&e.lock);
    gles_surface_free(gles);
    return done(s != NULL, s ? EGL_SUCCESS_ : EGL_BAD_SURFACE_);
}
static bool query_surface(uint64_t display, uint64_t surface, int32_t attribute, int32_t* value) {
    Surface* s = is_display(display) ? surface_of(surface) : NULL;
    if (!s) return done(0, EGL_BAD_SURFACE_);
    int w, h;
    gles_surface_size(s->gles, &w, &h);
    switch (attribute) {
    case EGL_WIDTH_: *value = w; break;
    case EGL_HEIGHT_: *value = h; break;
    case EGL_CONFIG_ID_: *value = s->config->id; break;
    case EGL_RENDER_BUFFER_: *value = EGL_BACK_BUFFER_; break;
    case EGL_SWAP_BEHAVIOR_: *value = EGL_BUFFER_DESTROYED_; break;
    default: return done(0, EGL_BAD_ATTRIBUTE_);
    }
    return done(1, EGL_SUCCESS_);
}
static bool query_context(uint64_t display, uint64_t context, int32_t attribute, int32_t* value) {
    Context* x = is_display(display) ? context_of(context) : NULL;
    if (!x) return done(0, EGL_BAD_CONTEXT_);
    switch (attribute) {
    case EGL_CONTEXT_CLIENT_VERSION_: *value = x->version; break;
    case EGL_CONTEXT_CLIENT_TYPE_: *value = EGL_OPENGL_ES_API_; break;
    case EGL_CONFIG_ID_: *value = x->config ? x->config->id : 0; break;
    case EGL_RENDER_BUFFER_: *value = EGL_BACK_BUFFER_; break;
    default: return done(0, EGL_BAD_ATTRIBUTE_);
    }
    return done(1, EGL_SUCCESS_);
}

/* eglMakeCurrent: a context with its surfaces, one without a surface (EGL_KHR_surfaceless_context), or none */
static bool make_current(uint64_t display, uint64_t draw, uint64_t read, uint64_t context) {
    if (!context) {
        if (draw || read) return done(0, EGL_BAD_MATCH_);
        t_context = NULL;
        t_draw = t_read = NULL;
        gles_make_current(NULL, NULL);
        gles2_make_current(NULL, NULL, NULL);
        return done(1, EGL_SUCCESS_);
    }
    if (!is_display(display)) return done(0, EGL_BAD_DISPLAY_);
    Context* x = context_of(context);
    if (!x) return done(0, EGL_BAD_CONTEXT_);
    Surface* d = draw ? surface_of(draw) : NULL;
    Surface* r = read ? surface_of(read) : NULL;
    if ((draw && !d) || (read && !r)) return done(0, EGL_BAD_SURFACE_);
    t_context = x;
    t_draw = d;
    t_read = r;
    gles_make_current(x->gl, d ? d->gles : NULL);
    gles2_make_current(x->gl2, d ? d->gles : NULL, r ? r->gles : NULL);
    return done(1, EGL_SUCCESS_);
}
static bool swap_buffers(uint64_t display, uint64_t surface) {
    Surface* s = is_display(display) ? surface_of(surface) : NULL;
    if (!s) return done(0, EGL_BAD_SURFACE_);
    return done(gles_swap(s->gles), EGL_SUCCESS_);
}
static const char* query_string(uint64_t display, int32_t name) {
    const char* s = NULL;
    switch (name) {
    case EGL_VENDOR_: s = "ARM"; break;
    case EGL_VERSION_: s = "1.5 Android META-EGL"; break;
    case EGL_EXTENSIONS_:
        s = display ? "EGL_KHR_no_config_context EGL_KHR_surfaceless_context EGL_ANDROID_recordable "
                      "EGL_KHR_create_context"
                    : "";
        break;
    case EGL_CLIENT_APIS_: s = "OpenGL_ES"; break;
    }
    if (display && !is_display(display)) s = NULL;
    t_error = s ? EGL_SUCCESS_ : EGL_BAD_PARAMETER_;
    return s;
}

/* ---- the guest's exports ---- */

static void g_get_display(CPU* c, void* data) {
    (void)data;
    X(0) = handle(&e.display); /* the default display, and any native one */
}
/* eglInitialize(display, major, minor): EGL 1.5 */
static void g_initialize(CPU* c, void* data) {
    (void)data;
    if (!is_display(X(0))) {
        X(0) = done(EGL_FALSE_, EGL_BAD_DISPLAY_);
        return;
    }
    int32_t major = 1, minor = 5;
    if (X(1)) memcpy(PTR(1), &major, 4);
    if (X(2)) memcpy(PTR(2), &minor, 4);
    X(0) = done(EGL_TRUE_, EGL_SUCCESS_);
}
static void g_terminate(CPU* c, void* data) {
    (void)data;
    X(0) = done(is_display(X(0)), is_display(X(0)) ? EGL_SUCCESS_ : EGL_BAD_DISPLAY_);
}
/* eglChooseConfig(display, attributes, configs, size, count) and eglGetConfigs(display, configs, size, count) */
static void configs_out(CPU* c, const int32_t* attributes, uint64_t out, int32_t size, uint64_t count) {
    const Config* found[CONFIGS];
    int n = choose_configs(attributes, out ? found : NULL, size < 0 ? 0 : size);
    for (int i = 0; out && i < n; ++i) {
        uint64_t h = handle(found[i]);
        memcpy((unsigned char*)(uintptr_t)out + 8 * (size_t)i, &h, 8);
    }
    int32_t n32 = n;
    memcpy((void*)(uintptr_t)count, &n32, 4);
    X(0) = done(EGL_TRUE_, EGL_SUCCESS_);
}
static void g_choose_config(CPU* c, void* data) {
    (void)data;
    if (!is_display(X(0)) || !X(4)) {
        X(0) = done(EGL_FALSE_, is_display(X(0)) ? EGL_BAD_PARAMETER_ : EGL_BAD_DISPLAY_);
        return;
    }
    configs_out(c, PTR(1), X(2), (int32_t)X(3), X(4));
}
static void g_get_configs(CPU* c, void* data) {
    (void)data;
    if (!is_display(X(0)) || !X(3)) {
        X(0) = done(EGL_FALSE_, is_display(X(0)) ? EGL_BAD_PARAMETER_ : EGL_BAD_DISPLAY_);
        return;
    }
    static const int32_t none[] = {EGL_NONE_};
    configs_out(c, none, X(1), (int32_t)X(2), X(3));
}
static void g_get_config_attrib(CPU* c, void* data) {
    (void)data;
    const Config* k = is_display(X(0)) ? config_of(X(1)) : NULL;
    int32_t v = 0;
    if (!k) X(0) = done(EGL_FALSE_, is_display(X(0)) ? EGL_BAD_CONFIG_ : EGL_BAD_DISPLAY_);
    else if (!config_attribute(k, (int32_t)X(2), &v)) X(0) = done(EGL_FALSE_, EGL_BAD_ATTRIBUTE_);
    else {
        memcpy(PTR(3), &v, 4);
        X(0) = done(EGL_TRUE_, EGL_SUCCESS_);
    }
}
static void g_create_context(CPU* c, void* data) {
    (void)data;
    X(0) = create_context(X(0), X(1), PTR(3));
}
static void g_destroy_context(CPU* c, void* data) {
    (void)data;
    X(0) = destroy_context(X(0), X(1));
}
static void g_make_current(CPU* c, void* data) {
    (void)data;
    X(0) = make_current(X(0), X(1), X(2), X(3));
}
static void g_get_current_context(CPU* c, void* data) {
    (void)data;
    X(0) = handle(t_context);
}
static void g_get_current_display(CPU* c, void* data) {
    (void)data;
    X(0) = t_context ? handle(&e.display) : 0;
}
static void g_get_current_surface(CPU* c, void* data) {
    (void)data;
    X(0) = handle((int32_t)X(0) == EGL_READ_ ? t_read : t_draw);
}
/* eglCreateWindowSurface(display, config, window, attributes): the ANativeWindow is the host window */
static void g_create_window_surface(CPU* c, void* data) {
    (void)data;
    int w = 0, h = 0;
    hle_android_window(&w, &h);
    X(0) = create_window_surface(X(0), X(1), w, h);
}
static void g_create_pbuffer_surface(CPU* c, void* data) {
    (void)data;
    X(0) = create_pbuffer_surface(X(0), X(1), PTR(2));
}
static void g_destroy_surface(CPU* c, void* data) {
    (void)data;
    X(0) = destroy_surface(X(0), X(1));
}
static void g_query_surface(CPU* c, void* data) {
    (void)data;
    int32_t v = 0;
    bool ok = query_surface(X(0), X(1), (int32_t)X(2), &v);
    if (ok) memcpy(PTR(3), &v, 4);
    X(0) = ok;
}
static void g_query_context(CPU* c, void* data) {
    (void)data;
    int32_t v = 0;
    bool ok = query_context(X(0), X(1), (int32_t)X(2), &v);
    if (ok) memcpy(PTR(3), &v, 4);
    X(0) = ok;
}
static void g_swap_buffers(CPU* c, void* data) {
    (void)data;
    X(0) = swap_buffers(X(0), X(1));
}
static void g_true(CPU* c, void* data) {
    (void)data;
    X(0) = done(EGL_TRUE_, EGL_SUCCESS_);
}
static void g_get_error(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)t_error;
    t_error = EGL_SUCCESS_;
}
static void g_query_string(CPU* c, void* data) {
    (void)data;
    X(0) = handle(query_string(X(0), (int32_t)X(1)));
}

/* glGetString: the device's identity in the current context, NULL without one (GL_INVALID_ENUM's NULL for others).
   A GLES 1.1 context names its version and extensions as GLES 1.1's, one of GLES 2 or 3 as GLES 3.0's (gles2.c) */
static void g_gl_get_string(CPU* c, void* data) {
    (void)data;
    const char* s = NULL;
    if (t_context && t_context->version >= 2) s = gles2_string(t_context->version, (unsigned)X(0));
    else if (t_context) switch ((int)X(0)) {
        case GL_VENDOR_: s = "ARM"; break;
        case GL_RENDERER_: s = hle_gpu_name; break;
        case GL_VERSION_: s = "OpenGL ES-CM 1.1"; break;
        case GL_EXTENSIONS_:
            s = "GL_OES_draw_texture GL_OES_fixed_point GL_OES_single_precision GL_OES_matrix_get "
                "GL_EXT_texture_format_BGRA8888";
            break;
        }
    X(0) = handle(s);
}

/* eglGetProcAddress: the HLE's EGL and GLES functions by name */
static void g_get_proc_address(CPU* c, void* data) {
    (void)data;
    const char* name = STR(0);
    X(0) = name && (!strncmp(name, "gl", 2) || !strncmp(name, "egl", 3)) ? hle_symbol(name) : 0;
}

const HleSymbol hle_egl[] = {
    {"eglGetDisplay", g_get_display, NULL},
    {"eglGetPlatformDisplay", g_get_display, NULL},
    {"eglInitialize", g_initialize, NULL},
    {"eglTerminate", g_terminate, NULL},
    {"eglQueryString", g_query_string, NULL},
    {"eglGetProcAddress", g_get_proc_address, NULL},
    {"eglChooseConfig", g_choose_config, NULL},
    {"eglGetConfigs", g_get_configs, NULL},
    {"eglGetConfigAttrib", g_get_config_attrib, NULL},
    {"eglCreateContext", g_create_context, NULL},
    {"eglDestroyContext", g_destroy_context, NULL},
    {"eglMakeCurrent", g_make_current, NULL},
    {"eglGetCurrentContext", g_get_current_context, NULL},
    {"eglGetCurrentDisplay", g_get_current_display, NULL},
    {"eglGetCurrentSurface", g_get_current_surface, NULL},
    {"eglCreateWindowSurface", g_create_window_surface, NULL},
    {"eglCreatePbufferSurface", g_create_pbuffer_surface, NULL},
    {"eglDestroySurface", g_destroy_surface, NULL},
    {"eglQuerySurface", g_query_surface, NULL},
    {"eglQueryContext", g_query_context, NULL},
    {"eglSwapBuffers", g_swap_buffers, NULL},
    {"eglSwapInterval", g_true, NULL},
    {"eglBindAPI", g_true, NULL},
    {"eglWaitGL", g_true, NULL},
    {"eglWaitClient", g_true, NULL},
    {"eglReleaseThread", g_true, NULL},
    {"eglGetError", g_get_error, NULL},
    {"glGetString", g_gl_get_string, NULL},
    {"", NULL, NULL},
};

/* ---- snx.android.opengl.Egl10's natives ---- */

/* a Java int[] of attributes ending in EGL_NONE, as a C array (NULL for none); free it with free */
static int32_t* attributes_of(JNIEnv* env, jintArray list) {
    if (!list) return NULL;
    jsize n = (*env)->GetArrayLength(env, list);
    int32_t* a = calloc((size_t)n + 1, sizeof *a);
    if (!a) return NULL;
    jint* v = (*env)->GetIntArrayElements(env, list, NULL);
    for (jsize i = 0; v && i < n; ++i) a[i] = v[i];
    if (v) (*env)->ReleaseIntArrayElements(env, list, v, JNI_ABORT);
    a[n] = EGL_NONE_;
    return a;
}
static void set_int(JNIEnv* env, jintArray out, jsize at, int32_t value) {
    jint v = value;
    if (out && (*env)->GetArrayLength(env, out) > at) (*env)->SetIntArrayRegion(env, out, at, 1, &v);
}

static jlong JNICALL j_display(JNIEnv* env, jclass unused) {
    (void)env, (void)unused;
    return (jlong)handle(&e.display);
}
static jboolean JNICALL j_initialize(JNIEnv* env, jclass unused, jlong display, jintArray version) {
    (void)unused;
    if (!is_display((uint64_t)display)) return (jboolean)done(0, EGL_BAD_DISPLAY_);
    set_int(env, version, 0, 1);
    set_int(env, version, 1, 5);
    return (jboolean)done(1, EGL_SUCCESS_);
}
static jboolean JNICALL j_terminate(JNIEnv* env, jclass unused, jlong display) {
    (void)env, (void)unused;
    return (jboolean)done(is_display((uint64_t)display), is_display((uint64_t)display) ? EGL_SUCCESS_ : EGL_BAD_DISPLAY_);
}
static jstring JNICALL j_query_string(JNIEnv* env, jclass unused, jlong display, jint name) {
    (void)unused;
    const char* s = query_string((uint64_t)display, name);
    return s ? (*env)->NewStringUTF(env, s) : NULL;
}
static jint JNICALL j_error(JNIEnv* env, jclass unused) {
    (void)env, (void)unused;
    int error = t_error;
    t_error = EGL_SUCCESS_;
    return error;
}
static jint JNICALL j_choose_config(JNIEnv* env, jclass unused, jlong display, jintArray list, jlongArray out) {
    (void)unused;
    if (!is_display((uint64_t)display)) return (jint)done((uint64_t)-1, EGL_BAD_DISPLAY_);
    int32_t* attributes = attributes_of(env, list);
    const Config* found[CONFIGS];
    jsize size = out ? (*env)->GetArrayLength(env, out) : 0;
    int n = choose_configs(attributes, out ? found : NULL, size);
    free(attributes);
    for (int i = 0; out && i < n; ++i) {
        jlong h = (jlong)handle(found[i]);
        (*env)->SetLongArrayRegion(env, out, i, 1, &h);
    }
    done(0, EGL_SUCCESS_);
    return n;
}
static jboolean JNICALL j_config_attrib(JNIEnv* env, jclass unused, jlong display, jlong config, jint attribute,
                                        jintArray value) {
    (void)unused;
    const Config* k = is_display((uint64_t)display) ? config_of((uint64_t)config) : NULL;
    int32_t v = 0;
    if (!k) return (jboolean)done(0, EGL_BAD_CONFIG_);
    if (!config_attribute(k, attribute, &v)) return (jboolean)done(0, EGL_BAD_ATTRIBUTE_);
    set_int(env, value, 0, v);
    return (jboolean)done(1, EGL_SUCCESS_);
}
static jlong JNICALL j_create_context(JNIEnv* env, jclass unused, jlong display, jlong config, jlong share,
                                      jintArray list) {
    (void)unused, (void)share; /* one share group: every context's textures are every other's */
    int32_t* attributes = attributes_of(env, list);
    uint64_t h = create_context((uint64_t)display, (uint64_t)config, attributes);
    free(attributes);
    return (jlong)h;
}
static jboolean JNICALL j_destroy_context(JNIEnv* env, jclass unused, jlong display, jlong context) {
    (void)env, (void)unused;
    return destroy_context((uint64_t)display, (uint64_t)context);
}
static jboolean JNICALL j_query_context(JNIEnv* env, jclass unused, jlong display, jlong context, jint attribute,
                                        jintArray value) {
    (void)unused;
    int32_t v = 0;
    bool ok = query_context((uint64_t)display, (uint64_t)context, attribute, &v);
    if (ok) set_int(env, value, 0, v);
    return ok;
}
static jlong JNICALL j_create_window_surface(JNIEnv* env, jclass unused, jlong display, jlong config, jint width,
                                             jint height, jintArray list) {
    (void)env, (void)unused, (void)list; /* EGL_RENDER_BUFFER and the color space: a back buffer, as the window's */
    return (jlong)create_window_surface((uint64_t)display, (uint64_t)config, width, height);
}
static jlong JNICALL j_create_pbuffer_surface(JNIEnv* env, jclass unused, jlong display, jlong config,
                                              jintArray list) {
    (void)unused;
    int32_t* attributes = attributes_of(env, list);
    uint64_t h = create_pbuffer_surface((uint64_t)display, (uint64_t)config, attributes);
    free(attributes);
    return (jlong)h;
}
static jboolean JNICALL j_destroy_surface(JNIEnv* env, jclass unused, jlong display, jlong surface) {
    (void)env, (void)unused;
    return destroy_surface((uint64_t)display, (uint64_t)surface);
}
static jboolean JNICALL j_query_surface(JNIEnv* env, jclass unused, jlong display, jlong surface, jint attribute,
                                        jintArray value) {
    (void)unused;
    int32_t v = 0;
    bool ok = query_surface((uint64_t)display, (uint64_t)surface, attribute, &v);
    if (ok) set_int(env, value, 0, v);
    return ok;
}
static jboolean JNICALL j_make_current(JNIEnv* env, jclass unused, jlong display, jlong draw, jlong read,
                                       jlong context) {
    (void)env, (void)unused;
    return make_current((uint64_t)display, (uint64_t)draw, (uint64_t)read, (uint64_t)context);
}
static jlong JNICALL j_current(JNIEnv* env, jclass unused, jint which) {
    (void)env, (void)unused;
    switch (which) {
    case 0: return (jlong)handle(t_context);
    case 1: return t_context ? (jlong)handle(&e.display) : 0;
    case 2: return (jlong)handle(t_draw);
    default: return (jlong)handle(t_read);
    }
}
static jboolean JNICALL j_swap_buffers(JNIEnv* env, jclass unused, jlong display, jlong surface) {
    (void)env, (void)unused;
    return swap_buffers((uint64_t)display, (uint64_t)surface);
}

bool android_egl_register(JNIEnv* env) {
    static const JNINativeMethod methods[] = {
        {(char*)"nativeDisplay", (char*)"()J", (void*)j_display},
        {(char*)"nativeInitialize", (char*)"(J[I)Z", (void*)j_initialize},
        {(char*)"nativeTerminate", (char*)"(J)Z", (void*)j_terminate},
        {(char*)"nativeQueryString", (char*)"(JI)Ljava/lang/String;", (void*)j_query_string},
        {(char*)"nativeError", (char*)"()I", (void*)j_error},
        {(char*)"nativeChooseConfig", (char*)"(J[I[J)I", (void*)j_choose_config},
        {(char*)"nativeConfigAttrib", (char*)"(JJI[I)Z", (void*)j_config_attrib},
        {(char*)"nativeCreateContext", (char*)"(JJJ[I)J", (void*)j_create_context},
        {(char*)"nativeDestroyContext", (char*)"(JJ)Z", (void*)j_destroy_context},
        {(char*)"nativeQueryContext", (char*)"(JJI[I)Z", (void*)j_query_context},
        {(char*)"nativeCreateWindowSurface", (char*)"(JJII[I)J", (void*)j_create_window_surface},
        {(char*)"nativeCreatePbufferSurface", (char*)"(JJ[I)J", (void*)j_create_pbuffer_surface},
        {(char*)"nativeDestroySurface", (char*)"(JJ)Z", (void*)j_destroy_surface},
        {(char*)"nativeQuerySurface", (char*)"(JJI[I)Z", (void*)j_query_surface},
        {(char*)"nativeMakeCurrent", (char*)"(JJJJ)Z", (void*)j_make_current},
        {(char*)"nativeCurrent", (char*)"(I)J", (void*)j_current},
        {(char*)"nativeSwapBuffers", (char*)"(JJ)Z", (void*)j_swap_buffers},
    };
    return android_jvm_register(env, "snx/android/opengl/Egl10", methods, (int)(sizeof methods / sizeof *methods));
}
