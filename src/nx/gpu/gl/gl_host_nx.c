/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's thunks on the console (gl_layer.h): guest memory and the display. A surface is two RGBA8 images in
   guest memory, pitch-linear, rows from the top; glClear fills the one the surface draws next on the CPU, and
   eglSwapBuffers hands it to the renderer on the GPU thread as vi hands a game's buffer (present_buffer), then draws
   into the other once the GPU thread showed it last. The surfaces and the strings are guest addresses and sizes: a
   save state's guest memory holds their pixels and bytes */
#include "common/gpu/gl/gl_state.h"
#include "common/platform/thread.h"
#include "nx/gpu/gl/gl_layer.h"
#include "nx/hle/hook/hook.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include <stdatomic.h>
#include <string.h>

#define GL_HOST_STRINGS      32
#define GL_HOST_STRING_BYTES 0x1000

typedef struct {
    u32 used, w, h, next;
    u64 image[2], gpu[2]; /* the images' guest and GPU addresses */
    NvnTexture depth;     /* its depth and stencil, as NVN lays a DEPTH24_STENCIL8 texture out */
} GlSurface;
/* an image of a surface on its way to the display, read on the GPU thread, done there at serial (gl_draw_issue's) */
typedef struct {
    u64 image, serial;
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
        if (!f->image[i] || f->w != w || f->h != h) {
            f->image[i] = gl_guest_pages(size);
            f->gpu[i] = f->image[i] ? gmmu_space_alloc(size, 0) : 0;
            if (f->gpu[i]) gmmu_space_map(f->gpu[i], f->image[i], size);
        }
    if (f->image[0] && (!f->depth.gpu || f->w != w || f->h != h)) {
        NvnTexStorage st;
        f->depth = (NvnTexture){.magic = NVN_MAGIC('T', 'E', 'X', 0),
                                .d = {.format = NVN_FORMAT_DEPTH24_STENCIL8,
                                      .target = NVN_TEXTURE_TARGET_2D,
                                      .levels = 1,
                                      .width = w,
                                      .height = h,
                                      .depth = 1}};
        nvn_tex_storage(&f->depth.d, &st);
        f->depth.size = (st.size + 0xffff) & ~0xffffull;
        f->depth.memory = gl_guest_pages(f->depth.size);
        f->depth.gpu = f->depth.memory ? gmmu_space_alloc(f->depth.size, st.align > 0x10000 ? st.align : 0) : 0;
        if (f->depth.gpu) gmmu_space_map(f->depth.gpu, f->depth.memory, f->depth.size);
    }
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

int gl_host_surface_targets(u32 s, DsTargets* t) {
    const GlSurface* g = s && s <= GL_HOST_SURFACES ? &g_gl_host.surfaces[s - 1] : NULL;
    int depth;
    u32 msaa;
    memset(t, 0, sizeof *t);
    if (!g || !g->used || !g->gpu[g->next]) return 0;
    t->count = 1;
    t->color[0] = (DsSurf){.gva = g->gpu[g->next],
                           .width = g->w * 4, /* a pitch-linear target's row bytes */
                           .height = g->h,
                           .format = nvn_rt_format(NVN_FORMAT_RGBA8, &depth),
                           .mem = 1u << 12,
                           .layers = 1};
    if (g->depth.gpu) nvn_draw_surface_of(&g->depth, NULL, &t->depth, &depth, &msaa);
    return 1;
}

/* on the GPU thread, in order with the GPU's other work: an image of a surface on the display. The layer draws with
   GL's lower left origin, so the image's rows count up, and it is shown flipped (vi's FlipV, 2), as the NVN front end
   queues a lower left window (nvn_queue.c) */
static void present_on_gpu(void* arg) {
    const GlPresent* p = arg;
    i32 crop[4] = {0, 0, (i32)p->w, (i32)p->h};
    if (g_renderer_up)
        g_renderer->present_buffer(p->image, p->w, p->h, p->w * 4, 0 /* pitch linear */, 0, 1 /* RGBA8888 */, crop,
                                   2 /* FlipV */, 0, 0, 0);
    atomic_fetch_add_explicit(&g_frames, 1, memory_order_relaxed);
    gl_draw_done(p->serial);
}
void gl_host_present(u32 s, u32 interval) {
    GlSurface* g = &g_gl_host.surfaces[s - 1];
    GlPresent* p = &g_gl_host.present[s - 1][g->next];
    /* off the guest core while it waits for the display, as NVN's present waits: the threads of the swapping
       thread's core run meanwhile (a game's texture loader, behind its main thread, waited for its core 47 s of a
       load of 50) */
    hook_wait_begin();
    present_pace(interval, -1);
    hook_wait_end();
    gl_object_calls_lock(); /* the serial in order with the draws' */
    *p = (GlPresent){.image = g->image[g->next], .serial = gl_draw_issue(), .w = g->w, .h = g->h};
    gpu_call(present_on_gpu, p, NULL, 0);
    gl_object_calls_unlock();
    g->next ^= 1;
    /* the next image's last present done before the game draws into it again, as NVN's acquire waits for its texture's
       and a GL driver's swap holds a game a window's images ahead of the display: a game whose frames take the GPU
       longer than the CPU ran on ahead of the GPU thread, which then showed each frame later than the one before (a
       game's opening on a phone at twice its length) */
    gl_draw_wait(0, g_gl_host.present[s - 1][g->next].serial);
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
