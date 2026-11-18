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

/* ---- gl_host_nx.c */
#define GL_HOST_SURFACES 8
/* a window or pbuffer surface of w x h RGBA8 pixels, two images in guest memory: its name, 0 when every one is used
   or guest memory runs out (said in the log) */
u32 gl_host_surface_new(u32 w, u32 h);
void gl_host_surface_free(u32 s);
void gl_host_surface_size(u32 s, u32* w, u32* h);
/* glClear's color into the image the surface draws next, in the context's scissor where its test is on, as its color
   mask lets */
void gl_host_clear(u32 s, u32 ctx, u32 mask);
/* the surface's image on the display (eglSwapBuffers), after the pace interval asks; the next one drawn next */
void gl_host_present(u32 s, u32 interval);
/* the guest's copy of a host string, made once a string: its address, 0 when guest memory runs out */
u64 gl_host_string(const char* s);

#endif
