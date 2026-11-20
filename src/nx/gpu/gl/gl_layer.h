/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The console's side of the GL layer (gl_layer=1, #950): a game that draws through its own GL driver gets the
   runtime's EGL and GL instead, the driver's code never running, as nvn_loader.c takes a game's NVN. The GL state is
   the shared layer's (common/gpu/gl/gl_state.h); what is the console's alone is here: taking the driver's functions
   (gl_loader.c), EGL's displays, configs, contexts and surfaces (gl_egl.c), the GL calls' adapters (gl_calls.c,
   gl_program_calls.c for shaders and programs, gl_object_calls.c for buffers and vertex arrays), and the thunks to
   guest memory and the renderer (gl_host_nx.c).

   A function of the driver the layer doesn't implement yet stops the game when it is called, naming it, as
   nvn_loader.c does for NVN */
#ifndef SNX_NX_GL_LAYER_H
#define SNX_NX_GL_LAYER_H
#include "nx/gpu/draw_state.h"
#include "nx/gpu/nvn/nvn_obj.h"
#include "common/cpu/aarch64/cpu.h"

/* ---- gl_loader.c */
/* With gl_layer=1, once the game's symbols and dispatch table are loaded and before any guest thread runs: every egl
   and _gl function of the module that exports eglGetProcAddress takes the layer's handler in its slot */
void gl_loader_init(void);
/* the address of the driver's export the game asks eglGetProcAddress for by name (glClearColor is the export
   _glClearColor, eglSwapBuffers itself), 0 when the driver exports none */
u64 gl_loader_lookup(const char* name);

/* ---- gl_egl.c */
/* the calling thread's current GL context (gl_state.h's name) and draw surface (gl_host_nx.c's), 0 for none */
u32 gl_egl_context(void);
u32 gl_egl_draw(void);

/* ---- gl_program_calls.c */
/* the shared layer's programs say what does not compile or link in the log */
void gl_program_calls_init(void);

/* ---- gl_object_calls.c */
/* the shared layer's buffers take their bytes from guest memory */
void gl_object_calls_init(void);
/* the GPU address of guest address guest in the layer's memory (its chunks are mapped in the GPU's address space), 0
   for one outside it */
u64 gl_gpu_address(u64 guest);
/* the lock of the layer's buffers and vertex arrays, and of its textures, which keep their texels in that memory */
void gl_object_calls_lock(void);
void gl_object_calls_unlock(void);

/* ---- gl_texture_calls.c */
/* texture name as NVN describes the same storage (the GL layer lays its textures out as NVN does, at gl_gpu_address of
   their memory): 1, or 0 (out zeroed) for a texture of none or of a format NVN has no form of */
int gl_nvn_texture(u32 name, NvnTexture* out);
/* ---- gl_object_calls.c */
/* size bytes of guest pages for the layer (64 KiB steps), mapped read and write past the program's regions; it stops
   the game, saying the sizes, where there is no room. And the bytes taken so far */
u64 gl_guest_pages(u64 size);
u64 gl_guest_pages_taken(void);
/* ---- gl_program_calls.c */
/* the program layout the renderer's rules give now, installed for the links that follow when it changed */
void gl_program_layout_refresh(void);
/* the NVN format of a shared format index (gl_format.h's), 0 for one NVN has no form of */
u16 gl_nvn_format(int32_t index);
/* renderbuffer name as NVN describes the same storage: 1, or 0 (out zeroed) for none or a format NVN has no form of */
int gl_nvn_renderbuffer(u32 name, NvnTexture* out);

/* ---- gl_draw.c */
/* glDraw*: the renderer's draw of the context's state, call's topology, counts and bases as GL gave them (indexed:
   indices the offset into the element array buffer, of elements_type), on the GPU thread in order. 1, or 0 for a draw
   that is skipped (said) */
int gl_draw(u32 ctx, const DrawCall* call, u32 elements_type, u64 indices);
/* glClear of buffers (GL's bits) in the bound draw framebuffer, by the renderer: 1, or 0 when there is no target */
int gl_draw_clear(u32 ctx, u32 buffers);
/* the fences' serials (gl_sync.h's host): the draws, clears and uploads handed to the GPU thread, those it finished,
   and a wait until it finished serial */
u64 gl_draw_recorded(void);
u64 gl_draw_completed(void);
void gl_draw_wait(u32 ctx, u64 serial);
/* other work handed to the GPU thread in the same order (a texture's upload, its levels): its serial, under the object
   lock, and its end there, so a range given back waits for it too */
u64 gl_draw_issue(void);
void gl_draw_done(u64 serial);
/* the shared layer's textures keep their texels in guest memory, and say in the log what they leave out */
void gl_texture_calls_init(void);

/* ---- gl_framebuffer_calls.c */
/* the shared layer's framebuffers and renderbuffers, with no platform image of a renderbuffer yet */
void gl_framebuffer_calls_init(void);

/* ---- gl_sync_calls.c */
/* the shared layer's fences, every one signaled while the layer records no GPU work */
void gl_sync_calls_init(void);

/* ---- gl_host_nx.c */
#define GL_HOST_SURFACES 8
/* a window or pbuffer surface of w x h RGBA8 pixels, two images in guest memory: its name, 0 when every one is used
   or guest memory runs out (said in the log) */
u32 gl_host_surface_new(u32 w, u32 h);
void gl_host_surface_free(u32 s);
void gl_host_surface_size(u32 s, u32* w, u32* h);
/* the targets a draw into surface s's next image has: the image (pitch-linear RGBA8) at its GPU address and the
   surface's depth and stencil. 0 for no surface */
int gl_host_surface_targets(u32 s, DsTargets* t);
/* the surface's image on the display (eglSwapBuffers), after the pace interval asks; the next one drawn next */
void gl_host_present(u32 s, u32 interval);
/* the guest's copy of a host string, made once a string: its address, 0 when guest memory runs out */
u64 gl_host_string(const char* s);

#endif
