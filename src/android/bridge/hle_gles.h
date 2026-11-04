/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_HLE_GLES_H
#define SNX_ANDROID_HLE_GLES_H
#include <stdbool.h>
#include <stdint.h>
/* GLES 1.1 over Vulkan (hle_gles.c), for EGL (hle_egl.c): a window surface is a swapchain on the host window, a
   context the state of GLES 1.1's fixed function, and the calls of the current context draw into its surface */

typedef struct GlesSurface GlesSurface;
typedef struct GlesContext GlesContext;

/* a surface of the host window, width by height when its surface leaves the size to the swapchain; NULL when Vulkan
   gives none */
GlesSurface* gles_window_surface(int width, int height);
/* EGL's pbuffer: an image of its own, width by height, that nothing presents (GLES 2 and 3 draw into it) */
GlesSurface* gles_pbuffer_surface(int width, int height);
/* the bits of depth and stencil the surface's config has: GLES 2 and 3 draw with such a buffer */
void gles_surface_depth(GlesSurface* surface, int depth, int stencil);
void gles_surface_free(GlesSurface* surface);
/* the size of the surface's images */
void gles_surface_size(const GlesSurface* surface, int* width, int* height);
/* a context of GLES 1.1, which shares its textures with every other one, as one share group */
GlesContext* gles_context_new(void);
void gles_context_free(GlesContext* context);
/* the calling thread's current context and its draw surface (NULL, NULL: none). The first time a context is made
   current with a surface, its viewport and scissor are the surface's size */
void gles_make_current(GlesContext* context, GlesSurface* draw);
/* the frame drawn into the surface since the last swap, presented. False when it could not be */
bool gles_swap(GlesSurface* surface);
/* the surfaces' format changed: GLES 1.1's pipelines are made again (gles_vk.c calls it) */
void gles1_format_changed(void);
#endif
