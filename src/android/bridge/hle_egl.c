/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* libEGL and libGLESv2, pending on purpose (docs/android/NATIVE.md, "EGL and GLES"): the libraries exist, since an
   engine library needs them, and report no display, so that the engine takes Vulkan. GLES over Vulkan is a later
   step. A GLES call without a context does nothing, as the missing exports already do */
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"

enum { EGL_FALSE_ = 0, EGL_NOT_INITIALIZED_ = 0x3001, EGL_BAD_DISPLAY_ = 0x3008 };

/* eglGetDisplay and eglGetPlatformDisplay: EGL_NO_DISPLAY */
static void no_display(CPU* c, void* data) {
    (void)data;
    X(0) = 0;
}
/* eglInitialize, eglChooseConfig, eglMakeCurrent and the others: EGL_FALSE, as on EGL_NO_DISPLAY */
static void egl_false(CPU* c, void* data) {
    (void)data;
    X(0) = EGL_FALSE_;
}
static void egl_error(CPU* c, void* data) {
    (void)data;
    X(0) = EGL_BAD_DISPLAY_;
}

const HleSymbol hle_egl[] = {
    {"eglGetDisplay", no_display, NULL},
    {"eglGetPlatformDisplay", no_display, NULL},
    {"eglGetCurrentContext", no_display, NULL},
    {"eglGetCurrentSurface", no_display, NULL},
    {"eglGetCurrentDisplay", no_display, NULL},
    {"eglQueryString", no_display, NULL},
    {"eglGetProcAddress", no_display, NULL},
    {"eglInitialize", egl_false, NULL},
    {"eglTerminate", egl_false, NULL},
    {"eglChooseConfig", egl_false, NULL},
    {"eglGetConfigAttrib", egl_false, NULL},
    {"eglCreateContext", no_display, NULL},
    {"eglCreateWindowSurface", no_display, NULL},
    {"eglDestroyContext", egl_false, NULL},
    {"eglDestroySurface", egl_false, NULL},
    {"eglMakeCurrent", egl_false, NULL},
    {"eglQuerySurface", egl_false, NULL},
    {"eglSwapBuffers", egl_false, NULL},
    {"eglSwapInterval", egl_false, NULL},
    {"eglGetError", egl_error, NULL},
    {"", NULL, NULL},
};
