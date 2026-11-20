/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's state machine, shared by every platform that translates a guest's GL to Vulkan: the Android bridge's
   GLES 2 and 3 (src/android/bridge/gles2*.c) and the console's GL games (#950). Its data is one block of flat records
   (gl_state.c's g_gl): numbers, GL enums and indices, no pointer, so a save state holds it as it is and a load puts it
   back whole. What only lives in a process (Vulkan objects, host pointers, the guest's memory, the device and the
   present) is each platform's, beside it, made again from these records after a load.

   This is the fixed state of the pipeline (enables, blending, depth, stencil, viewport, scissor, clear values, pixel
   store) and the GL error of each context. A context is named by its index + 1; 0 is none. The caller holds its
   platform's GL lock around every call */
#ifndef SNX_GL_STATE_H
#define SNX_GL_STATE_H
#include <stddef.h>
#include <stdint.h>

#define SNX_GL_CONTEXTS 32 /* contexts alive at once: an engine makes a throwaway one or two beside its own */

/* GL's errors */
#define SNX_GL_NO_ERROR          0u
#define SNX_GL_INVALID_ENUM      0x0500u
#define SNX_GL_INVALID_VALUE     0x0501u
#define SNX_GL_INVALID_OPERATION 0x0502u

/* a context's fixed state, as GL's calls set it; the values GL's own enums are kept as the guest gave them */
typedef struct {
    uint32_t error; /* the first error since glGetError read it */
    uint8_t blend, cull, depth_test, stencil_test, scissor_test, polygon_offset, dither, alpha_to_coverage,
        sample_coverage, rasterizer_discard, primitive_restart, sample_mask_on;
    uint8_t color_mask[4];
    uint8_t depth_mask, coverage_invert, pad[2];
    uint32_t blend_src[2], blend_dst[2], blend_eq[2]; /* [0] RGB, [1] alpha */
    float blend_color[4];
    uint32_t depth_func;
    float depth_range[2];
    uint32_t cull_face, front_face;
    struct {
        uint32_t func, fail, zfail, zpass, mask, write;
        int32_t ref;
    } stencil[2]; /* front, back */
    float clear_color[4], clear_depth;
    int32_t clear_stencil;
    int32_t viewport[4], scissor[4];
    float line_width, offset_factor, offset_units, coverage_value;
    uint32_t hints[2]; /* GL_GENERATE_MIPMAP_HINT, GL_FRAGMENT_SHADER_DERIVATIVE_HINT */
    struct {
        int32_t alignment, row_length, skip_rows, skip_pixels, image_height, skip_images;
    } pixel[2]; /* glPixelStorei's pack [0] and unpack [1] */
    uint32_t sample_mask;
    float point_size;      /* glPointSize's */
    uint32_t polygon_mode; /* glPolygonMode's of both faces, a desktop context's */
} SnxGlFixed;

/* a context's API */
#define SNX_GL_API_ES      0u /* OpenGL ES */
#define SNX_GL_API_DESKTOP 1u /* OpenGL's core profile */
/* a context: used, its API and version (major * 10 + minor: 31, 45), debug (EGL_CONTEXT_OPENGL_DEBUG), its fixed
   state, and the objects bound by name (gl_program.h's program and program pipeline) */
typedef struct {
    uint32_t used, api, version, debug;
    SnxGlFixed fixed;
    uint32_t program, pipeline;
} SnxGlContext;

/* a context of api and version, its fixed state GL's initial one: its name, 0 when every one of SNX_GL_CONTEXTS is
   used */
uint32_t snx_gl_context_new(uint32_t api, uint32_t version, uint32_t debug);
/* whether the layer makes contexts of api and version (desktop 3.0 to 4.5, ES 2.0, 3.0 to 3.2) */
int snx_gl_version_made(uint32_t api, uint32_t version);
/* a context's api and version (0 for a name not made) */
uint32_t snx_gl_context_api(uint32_t ctx);
uint32_t snx_gl_context_version(uint32_t ctx);
void snx_gl_context_free(uint32_t ctx);
/* the fixed state of a context made, for a platform's draws to read */
SnxGlFixed* snx_gl_fixed(uint32_t ctx);
/* a context made */
SnxGlContext* snx_gl_context(uint32_t ctx);
/* records error e unless one is waiting (GL keeps the first) */
void snx_gl_error(uint32_t ctx, uint32_t e);

/* GL's calls of the fixed state: each records GL's error for an argument GL refuses */
uint32_t snx_gl_get_error(uint32_t ctx);
void snx_gl_enable(uint32_t ctx, uint32_t cap, int on);
int snx_gl_is_enabled(uint32_t ctx, uint32_t cap);
/* the enable of cap, 0 or 1, as glGet* reads it: -1 when cap is not a capability (no error recorded) */
int snx_gl_capability(uint32_t ctx, uint32_t cap);
void snx_gl_blend_func_separate(uint32_t ctx, uint32_t src_rgb, uint32_t dst_rgb, uint32_t src_alpha,
                                uint32_t dst_alpha);
void snx_gl_blend_equation_separate(uint32_t ctx, uint32_t rgb, uint32_t alpha);
void snx_gl_blend_color(uint32_t ctx, float r, float g, float b, float a);
void snx_gl_color_mask(uint32_t ctx, int r, int g, int b, int a);
void snx_gl_depth_mask(uint32_t ctx, int on);
void snx_gl_depth_func(uint32_t ctx, uint32_t func);
void snx_gl_depth_range(uint32_t ctx, float near_value, float far_value);
void snx_gl_cull_face(uint32_t ctx, uint32_t face);
void snx_gl_front_face(uint32_t ctx, uint32_t mode);
/* face: GL_FRONT, GL_BACK or GL_FRONT_AND_BACK (the calls without Separate pass GL_FRONT_AND_BACK) */
void snx_gl_stencil_func_separate(uint32_t ctx, uint32_t face, uint32_t func, int32_t ref, uint32_t mask);
void snx_gl_stencil_op_separate(uint32_t ctx, uint32_t face, uint32_t fail, uint32_t zfail, uint32_t zpass);
void snx_gl_stencil_mask_separate(uint32_t ctx, uint32_t face, uint32_t mask);
void snx_gl_clear_color(uint32_t ctx, float r, float g, float b, float a);
void snx_gl_clear_depth(uint32_t ctx, float depth);
void snx_gl_clear_stencil(uint32_t ctx, int32_t s);
void snx_gl_viewport(uint32_t ctx, int32_t x, int32_t y, int32_t width, int32_t height);
void snx_gl_scissor(uint32_t ctx, int32_t x, int32_t y, int32_t width, int32_t height);
void snx_gl_line_width(uint32_t ctx, float width);
void snx_gl_point_size(uint32_t ctx, float size);
/* glPolygonMode: GL_FRONT_AND_BACK alone, a core profile's only face */
void snx_gl_polygon_mode(uint32_t ctx, uint32_t face, uint32_t mode);
void snx_gl_polygon_offset(uint32_t ctx, float factor, float units);
void snx_gl_sample_coverage(uint32_t ctx, float value, int invert);
void snx_gl_hint(uint32_t ctx, uint32_t target, uint32_t mode);
void snx_gl_pixel_store(uint32_t ctx, uint32_t pname, int32_t value);

/* glGet*'s values of pname, as numbers: how many it has (1 to 4), 0 for a pname the layer doesn't answer (the caller
   records GL_INVALID_ENUM and says it) */
int snx_gl_get(uint32_t ctx, uint32_t pname, double v[4]);
/* glGetString(name): the string, NULL for a name GL has not (GL_INVALID_ENUM recorded). glGetStringi(GL_EXTENSIONS,
   index) with stringi: NULL past the last (GL_INVALID_VALUE). The renderer names the GPU the game sees */
const char* snx_gl_string(uint32_t ctx, uint32_t name, int stringi, uint32_t index);
/* glClear(mask): the buffers to clear (GL_COLOR_BUFFER_BIT, GL_DEPTH_BUFFER_BIT, GL_STENCIL_BUFFER_BIT), 0 with
   GL_INVALID_VALUE recorded for a bit GL has not */
uint32_t snx_gl_clear(uint32_t ctx, uint32_t mask);

/* A save state's copy of the layer: snx_gl_state_bytes() bytes, a header (magic, layout version, the contexts'
   size), the contexts, then the programs' part (gl_program.h), the buffers' and vertex arrays' (gl_object.h), the
   textures' and samplers' (gl_texture.h), the framebuffers' and renderbuffers' (gl_framebuffer.h) and the syncs'
   (gl_sync.h). Check refuses a copy this build cannot load, with why
   naming the field and its value; load puts a checked copy in place of the running state: 0, or -1 when there is no
   memory for it (the running state left as it was) */
#define SNX_GL_STATE_MAGIC   0x4c47584eu /* "NXGL" */
#define SNX_GL_STATE_VERSION 9u
size_t snx_gl_state_bytes(void);
void snx_gl_state_save(void* out);
int snx_gl_state_check(const void* in, size_t size, char* why, size_t whysize);
int snx_gl_state_load(const void* in, size_t size);

#endif
