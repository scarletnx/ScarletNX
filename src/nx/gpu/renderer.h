/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The renderer interface: what the GPU front end (gpu.c, engines.c, maxwell3d.c) and the window backends ask of the
   Vulkan renderer (gpu/vk/renderer.c). It works with a window or offscreen without a swapchain. */
#ifndef SNX_RENDERER_H
#define SNX_RENDERER_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/gpu/gpu.h"

typedef struct Renderer {
    const char* name;
    int (*start)(void); /* 0 when it can't run here */
    /* guest memory may have changed since the renderer's caches last checked it: a submission came (on the
       submitting thread), or a stream goes on after a wait (gpu_park, on the GPU thread) that the CPU may have ended
       after writing data. Caches read guest memory again. Any thread */
    void (*mem_may_have_changed)(void);
    /* the 3D engine (maxwell3d.c) */
    void (*clear)(GpuChannel* ch, u32 v);
    void (*draw)(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
                 u32 instances);
    /* Maxwell ordering methods: 0 texture barrier, 1 tiled barrier, 2 data-cache invalidate,
       3 descriptor-cache invalidate, 4 wait for idle. */
    void (*method_barrier)(int kind);
    /* DRAW_TEXTURE: a texture's rectangle onto color target 0. NULL: not drawn */
    void (*draw_texture)(GpuChannel* ch);
    /* the 2D engine: 1 when the renderer did the blit on the GPU, 0 to do it in guest memory */
    int (*blit)(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                double sdy);
    /* the VIC composition (gpu/video/vic.c): 1 when the renderer composed the picture as a compute shader,
       0 to compose in guest memory */
    int (*vic_compose)(const VicJob* j);
    void (*rt_flush)(u64 gva);  /* a render target at gva is about to be read from guest memory: write it back */
    void (*rt_reload)(u64 gva); /* guest memory under a render target changed: load it again */
    void (*dispatch)(GpuChannel* ch, const u32* q, u64 code); /* a compute launch (q: its QMD) */
    /* gmem_read and gmem_ptr land pending compute or graphics storage writes before reading memory */
    void (*cs_mem_need)(u64 a, u64 n);
    void (*gfx_mem_need)(u64 a, u64 n);
    void (*before_syncwait)(void); /* the stream is about to wait on a syncpoint: flush everything in flight */
    void (*stream_incr)(u32 id);   /* a syncpoint increment in the stream, after everything before it is done */
    /* a semaphore or report the game can poll: len bytes written at gva once everything before it is done (it may
       happen later, in order with the other releases). NULL: written now, after landing everything */
    void (*release_write)(u64 gva, const void* data, u32 len);
    /* the stream reads or writes [a, a + n): a release still waiting to write there happens first */
    void (*release_need)(u64 a, u64 n);
    u32 (*release_value)(u32 id);   /* completed and queued increments, read consistently */
    void (*release_retire)(u32 id); /* finish this syncpoint's queued releases before its id is reused */
    /* presenting a guest framebuffer */
    /* crop: the part shown (left, top, right, bottom), all 0 for the whole buffer */
    /* transform: Android buffer flags, bit 0 horizontal flip and bit 1 vertical flip */
    void (*present_buffer)(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                           u32 transform);
    u64 (*screenshot)(const char* path); /* the hash of the frame it saved */
    /* the window again, its last frame (or black) under the on-screen controls and the game's menu, for when the game
       has shown nothing for a while: the frontend's loop calls it while either shows. NULL when there is no window */
    void (*refresh)(void);
    /* writes what should outlive the process (the pipeline cache): every 5 s from the frontend loop, and before an exit
       that skips the normal shutdown. Any thread, while the others still run. NULL when the renderer keeps nothing */
    void (*save)(void);
    /* the window given up for a fatal error's screen (plat.c's fatal, backend/sdl3.c): the swapchain and the surface go,
       as nothing else can draw in a window something presents to. Called on the failing thread, which never returns to
       its callers, so it keeps no lock. 0 when another thread held the renderer too long. NULL without a window */
    int (*release_window)(void);
} Renderer;

extern const Renderer vk_renderer;
/* the Vulkan renderer, windowed or headless */
extern const Renderer* g_renderer;

/* shared state (gpu/renderer.c) */
extern int g_renderer_up; /* the renderer started */
extern u64 g_draws;
extern _Atomic int g_cs_any;      /* compute results are pending somewhere */
extern _Atomic int g_gfx_any;     /* graphics storage-buffer results are pending somewhere */
extern _Atomic int g_gfx_img_any; /* graphics storage-image results are pending somewhere */

#endif
