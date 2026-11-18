/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's framebuffer and renderbuffer calls on the console (gl_layer.h, gpu/gl/gl.hook's framebuffer family):
   each the shared layer's (common/gpu/gl/gl_framebuffer.h) on the calling thread's current context, under the object
   lock, since completeness reads the textures' records (gl_texture_calls.c). Framebuffer 0 is the window's surface
   (gl_host_nx.c), which has color alone. Without a current context a call does nothing and returns 0 */
#include "common/gpu/gl/gl_framebuffer.h"
#include "common/gpu/gl/gl_state.h"
#include "gpu/gl/gl.hook.h"
#include "nx/gpu/gl/gl_layer.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/runtime/platform/log.h"
#include "nx/memory/mem.h"
#include <string.h>

int gl_nvn_renderbuffer(u32 name, NvnTexture* out) {
    const SnxGlRenderbuffer* r = snx_gl_renderbuffer(name);
    u16 format = r ? gl_nvn_format(r->format) : 0;
    *out = (NvnTexture){0};
    if (!format) return 0;
    out->magic = NVN_MAGIC('T', 'E', 'X', 0);
    out->d = (NvnTexDesc){.format = format,
                          .target = (u8)(r->samples > 1 ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE : NVN_TEXTURE_TARGET_2D),
                          .samples = (u8)(r->samples > 1 ? r->samples : 0),
                          .levels = 1,
                          .width = (u32)r->width,
                          .height = (u32)r->height,
                          .depth = 1};
    out->memory = r->storage, out->size = r->storage_size;
    out->gpu = r->storage ? gl_gpu_address(r->storage) : 0;
    return 1;
}
/* a renderbuffer's storage in NVN's layout of a 2D texture of its format and size: its bytes and alignment */
static uint64_t renderbuffer_bytes(uint32_t name, uint64_t* align) {
    NvnTexture n;
    NvnTexStorage st;
    if (!gl_nvn_renderbuffer(name, &n)) {
        LOG_LIMIT(8, "gl layer: renderbuffer %u's format has no NVN form here: it keeps no storage\n", (unsigned)name);
        return 0;
    }
    nvn_tex_storage(&n.d, &st);
    *align = st.align > 256 ? st.align : 256;
    return st.size;
}
void gl_framebuffer_calls_init(void) {
    snx_gl_framebuffer_host(&(SnxGlFramebufferHost){.storage_bytes = renderbuffer_bytes});
}

#define LOCKED(fallback, ...)                                                                                          \
    do {                                                                                                               \
        u32 ctx = gl_egl_context();                                                                                    \
        (void)c;                                                                                                       \
        if (!ctx) return fallback;                                                                                     \
        gl_object_calls_lock();                                                                                        \
        __VA_ARGS__;                                                                                                   \
        gl_object_calls_unlock();                                                                                      \
    } while (0)
#define NONE

static void* host(u64 a) { return a ? GP(a) : NULL; }
static void put_int(u64 a, i32 v) { memcpy(GP(a), &v, 4); }

/* ---- renderbuffers ---- */

void gl_gen_renderbuffers(CPU* c, i32 n, u64 renderbuffers) {
    LOCKED(NONE, snx_gl_gen_renderbuffers(ctx, n, host(renderbuffers)));
}
void gl_delete_renderbuffers(CPU* c, i32 n, u64 renderbuffers) {
    LOCKED(NONE, snx_gl_delete_renderbuffers(ctx, n, host(renderbuffers)));
}
u8 gl_is_renderbuffer(CPU* c, u32 renderbuffer) {
    u8 is = 0;
    LOCKED(0, is = (u8)snx_gl_is_renderbuffer(renderbuffer));
    return is;
}
void gl_bind_renderbuffer(CPU* c, u32 target, u32 renderbuffer) {
    LOCKED(NONE, snx_gl_bind_renderbuffer(ctx, target, renderbuffer));
}
void gl_renderbuffer_storage(CPU* c, u32 target, u32 internal, i32 width, i32 height) {
    LOCKED(NONE, snx_gl_renderbuffer_storage(ctx, target, 0, internal, width, height));
}
void gl_renderbuffer_storage_multisample(CPU* c, u32 target, i32 samples, u32 internal, i32 width, i32 height) {
    LOCKED(NONE, snx_gl_renderbuffer_storage(ctx, target, samples, internal, width, height));
}
void gl_get_renderbuffer_parameteriv(CPU* c, u32 target, u32 pname, u64 params) {
    i32 v = 0;
    LOCKED(NONE, if (snx_gl_get_renderbuffer_parameter(ctx, target, pname, &v)) put_int(params, v));
}

/* ---- framebuffers ---- */

void gl_gen_framebuffers(CPU* c, i32 n, u64 framebuffers) {
    LOCKED(NONE, snx_gl_gen_framebuffers(ctx, n, host(framebuffers)));
}
void gl_delete_framebuffers(CPU* c, i32 n, u64 framebuffers) {
    LOCKED(NONE, snx_gl_delete_framebuffers(ctx, n, host(framebuffers)));
}
u8 gl_is_framebuffer(CPU* c, u32 framebuffer) {
    u8 is = 0;
    LOCKED(0, is = (u8)snx_gl_is_framebuffer(framebuffer));
    return is;
}
void gl_bind_framebuffer(CPU* c, u32 target, u32 framebuffer) {
    LOCKED(NONE, snx_gl_bind_framebuffer(ctx, target, framebuffer));
}
void gl_framebuffer_texture_2d(CPU* c, u32 target, u32 attachment, u32 textarget, u32 texture, i32 level) {
    LOCKED(NONE, snx_gl_framebuffer_texture(ctx, target, attachment, textarget, texture, level, 0));
}
void gl_framebuffer_texture_layer(CPU* c, u32 target, u32 attachment, u32 texture, i32 level, i32 layer) {
    LOCKED(NONE, snx_gl_framebuffer_texture(ctx, target, attachment, 0, texture, level, layer));
}
/* glFramebufferTexture: a layered attachment of an array or a cube map, its first layer drawn */
void gl_framebuffer_texture(CPU* c, u32 target, u32 attachment, u32 texture, i32 level) {
    LOCKED(NONE, snx_gl_framebuffer_texture(ctx, target, attachment, 0, texture, level, 0));
}
void gl_framebuffer_renderbuffer(CPU* c, u32 target, u32 attachment, u32 rbtarget, u32 renderbuffer) {
    LOCKED(NONE, snx_gl_framebuffer_renderbuffer(ctx, target, attachment, rbtarget, renderbuffer));
}
u32 gl_check_framebuffer_status(CPU* c, u32 target) {
    u32 status = 0;
    LOCKED(0, status = snx_gl_check_framebuffer_status(ctx, target));
    return status;
}
void gl_get_framebuffer_attachment_parameteriv(CPU* c, u32 target, u32 attachment, u32 pname, u64 params) {
    i32 v = 0;
    LOCKED(NONE,
           if (snx_gl_get_framebuffer_attachment_parameter(ctx, target, attachment, pname, &v)) put_int(params, v));
}
void gl_draw_buffers(CPU* c, i32 n, u64 buffers) { LOCKED(NONE, snx_gl_draw_buffers(ctx, n, host(buffers))); }
/* desktop GL's glDrawBuffer: one draw buffer */
void gl_draw_buffer(CPU* c, u32 mode) { LOCKED(NONE, snx_gl_draw_buffers(ctx, 1, &mode)); }
void gl_read_buffer(CPU* c, u32 mode) { LOCKED(NONE, snx_gl_read_buffer(ctx, mode)); }
/* a hint: what the attachments hold stays */
void gl_invalidate_framebuffer(CPU* c, u32 target, i32 n, u64 attachments) {
    (void)c, (void)target, (void)n, (void)attachments;
}
