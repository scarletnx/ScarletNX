/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's thunks on the console (gl_layer.h): guest memory and the display. A surface is two RGBA8 images in
   guest memory, pitch-linear, rows from the top; glClear fills the one the surface draws next on the CPU, and
   eglSwapBuffers hands it to the renderer on the GPU thread as vi hands a game's buffer (present_buffer), then draws
   into the other. The surfaces and the strings are guest addresses and sizes: a save state's guest memory holds their
   pixels and bytes */
#include "common/gpu/gl/gl_state.h"
#include "common/platform/thread.h"
#include "nx/gpu/gl/gl_layer.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/memory/mem.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include <string.h>

#define GL_HOST_STRINGS      32
#define GL_HOST_STRING_BYTES 0x1000

typedef struct {
    u32 used, w, h, next;
    u64 image[2];
} GlSurface;
/* an image of a surface on its way to the display, read on the GPU thread */
typedef struct {
    u64 image;
    u32 w, h;
} GlPresent;

static struct {
    PMutex lock;
    GlSurface surfaces[GL_HOST_SURFACES];
    GlPresent present[GL_HOST_SURFACES][2];
    u64 strings, string_at[GL_HOST_STRINGS]; /* the strings' page, and each string's address in it */
    u32 string_count, string_bytes;
} g_gl_host = {.lock = PMUTEX_INIT};

u32 gl_host_surface_new(u32 w, u32 h) {
    u64 bytes = (u64)w * h * 4, size = (bytes + 0xfff) & ~0xfffull;
    u32 s;
    pm_lock(&g_gl_host.lock);
    for (s = 0; s < GL_HOST_SURFACES && g_gl_host.surfaces[s].used; s++) {}
    if (s == GL_HOST_SURFACES) {
        pm_unlock(&g_gl_host.lock);
        WARN("gl layer: a surface of %ux%u isn't made: the layer holds %d, all used\n", w, h, GL_HOST_SURFACES);
        return 0;
    }
    GlSurface* f = &g_gl_host.surfaces[s];
    for (int i = 0; i < 2; i++)
        if (!f->image[i] || f->w != w || f->h != h) f->image[i] = kmem_alloc(size, MS_KERNEL, PERM_RW);
    if (!f->image[0] || !f->image[1]) {
        pm_unlock(&g_gl_host.lock);
        WARN("gl layer: a surface of %ux%u isn't made: guest memory has no 2 x %llu bytes for its images\n", w, h,
             (unsigned long long)size);
        return 0;
    }
    f->used = 1, f->w = w, f->h = h, f->next = 0;
    pm_unlock(&g_gl_host.lock);
    return s + 1;
}
void gl_host_surface_free(u32 s) {
    pm_lock(&g_gl_host.lock);
    if (s && s <= GL_HOST_SURFACES) g_gl_host.surfaces[s - 1].used = 0; /* its images stay for the next surface */
    pm_unlock(&g_gl_host.lock);
}
void gl_host_surface_size(u32 s, u32* w, u32* h) {
    const GlSurface* f = s && s <= GL_HOST_SURFACES ? &g_gl_host.surfaces[s - 1] : NULL;
    *w = f && f->used ? f->w : 0;
    *h = f && f->used ? f->h : 0;
}

/* a color channel in [0, 1] as a byte */
static u8 unorm8(float v) { return (u8)(v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0f + 0.5f)); }
void gl_host_clear(u32 s, u32 ctx, u32 mask) {
    const SnxGlFixed* f = snx_gl_fixed(ctx);
    GlSurface* g = &g_gl_host.surfaces[s - 1];
    u8 rgba[4];
    i32 x0 = 0, y0 = 0, x1 = (i32)g->w, y1 = (i32)g->h;
    (void)mask; /* the color alone: depth and stencil have no buffer here yet */
    for (int i = 0; i < 4; i++) rgba[i] = unorm8(f->clear_color[i]);
    if (f->scissor_test) {
        x0 = f->scissor[0] > 0 ? f->scissor[0] : 0;
        y0 = f->scissor[1] > 0 ? f->scissor[1] : 0;
        x1 = f->scissor[0] + f->scissor[2] < x1 ? f->scissor[0] + f->scissor[2] : x1;
        y1 = f->scissor[1] + f->scissor[3] < y1 ? f->scissor[1] + f->scissor[3] : y1;
    }
    u8* image = GP(g->image[g->next]);
    for (i32 y = y0; y < y1; y++) { /* GL's rows count from the bottom, the image's from the top */
        u8* row = image + (u64)(g->h - 1 - (u32)y) * g->w * 4;
        for (i32 x = x0; x < x1; x++)
            for (int k = 0; k < 4; k++)
                if (f->color_mask[k]) row[4 * x + k] = rgba[k];
    }
}

/* on the GPU thread, in order with the GPU's other work: an image of a surface on the display */
static void present_on_gpu(void* arg) {
    const GlPresent* p = arg;
    i32 crop[4] = {0, 0, (i32)p->w, (i32)p->h};
    if (g_renderer_up)
        g_renderer->present_buffer(p->image, p->w, p->h, p->w * 4, 0 /* pitch linear */, 0, 1 /* RGBA8888 */, crop, 0,
                                   0, 0, 0);
    __atomic_add_fetch(&g_frames, 1, __ATOMIC_RELAXED);
}
void gl_host_present(u32 s, u32 interval) {
    GlSurface* g = &g_gl_host.surfaces[s - 1];
    GlPresent* p = &g_gl_host.present[s - 1][g->next];
    present_pace(interval, -1);
    *p = (GlPresent){g->image[g->next], g->w, g->h};
    gpu_call(present_on_gpu, p, NULL, 0);
    g->next ^= 1;
}

u64 gl_host_string(const char* s) {
    size_t n = strlen(s) + 1;
    u64 at = 0;
    pm_lock(&g_gl_host.lock);
    for (u32 i = 0; i < g_gl_host.string_count && !at; i++)
        if (!strcmp((const char*)GP(g_gl_host.string_at[i]), s)) at = g_gl_host.string_at[i];
    if (!at && !g_gl_host.strings) g_gl_host.strings = kmem_alloc(GL_HOST_STRING_BYTES, MS_KERNEL, PERM_R);
    if (!at && g_gl_host.strings && g_gl_host.string_count < GL_HOST_STRINGS &&
        g_gl_host.string_bytes + n <= GL_HOST_STRING_BYTES) {
        at = g_gl_host.strings + g_gl_host.string_bytes;
        memcpy(GP(at), s, n);
        g_gl_host.string_bytes += (u32)n;
        g_gl_host.string_at[g_gl_host.string_count++] = at;
    }
    pm_unlock(&g_gl_host.lock);
    if (!at)
        WARN_LIMIT(4, "gl layer: \"%s\" has no room in the %d-byte page of the layer's strings: NULL\n", s,
                   GL_HOST_STRING_BYTES);
    return at;
}
