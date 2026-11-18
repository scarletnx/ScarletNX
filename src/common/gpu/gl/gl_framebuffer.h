/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's framebuffers and renderbuffers (gl_state.h's layer), shared by the Android bridge and the console.

   A renderbuffer is a flat record: its format (gl_format.h's index), size and samples. A framebuffer is its
   attachments (a texture's level and layer, or a renderbuffer, by name), its draw buffers and its read buffer. Each
   context binds a draw and a read framebuffer (0: the window's, the platform's) and a renderbuffer. A platform's image
   of a renderbuffer is its own, made when the layer says its storage is new; what a platform made of a framebuffer's
   attachments (its render targets) it drops when the layer says they changed. The records are one block a save state
   holds as it is; the caller holds its platform's GL lock, the textures' too (gl_texture.h), which completeness reads */
#ifndef SNX_GL_FRAMEBUFFER_H
#define SNX_GL_FRAMEBUFFER_H
#include <stddef.h>
#include <stdint.h>

#define SNX_GL_RENDERBUFFERS 1024
#define SNX_GL_FRAMEBUFFERS  1024
#define SNX_GL_COLORS        4 /* GL_MAX_COLOR_ATTACHMENTS and GL_MAX_DRAW_BUFFERS */
enum { SNX_GL_DEPTH_ATTACHMENT = SNX_GL_COLORS, SNX_GL_STENCIL_ATTACHMENT, SNX_GL_ATTACHMENTS };
enum { SNX_GL_ATTACH_NONE = 0, SNX_GL_ATTACH_TEXTURE = 1, SNX_GL_ATTACH_RENDERBUFFER = 2 };

typedef struct {
    uint32_t used;
    int32_t format;        /* gl_format.h's index, -1 for none yet */
    int32_t width, height; /* 0 x 0 until its storage */
    int32_t samples;       /* 1, 2 or 4 */
    uint32_t generation;   /* counts the storages made: a platform's image of an older one is stale */
    uint64_t storage, storage_size; /* the platform memory its storage takes, where the platform lays it out */
} SnxGlRenderbuffer;

/* an attachment of a framebuffer: a texture's level and layer (a cube map's face), or a renderbuffer */
typedef struct {
    uint8_t kind; /* SNX_GL_ATTACH_* */
    uint8_t zero[3];
    uint32_t name;
    int32_t level, layer;
} SnxGlAttachment;

typedef struct {
    uint32_t used;
    SnxGlAttachment at[SNX_GL_ATTACHMENTS];
    uint32_t draw[SNX_GL_COLORS]; /* glDrawBuffers: GL_COLOR_ATTACHMENTi or GL_NONE */
    uint32_t read;                /* glReadBuffer */
    uint32_t generation;          /* counts the changes of its attachments and draw buffers */
} SnxGlFramebuffer;

/* what a context binds of framebuffers and renderbuffers */
typedef struct {
    uint32_t draw, read, renderbuffer;
} SnxGlFramebufferContext;

/* What a platform hears of them; each may be NULL. Not part of a save state: given again after a load */
typedef struct {
    /* the renderbuffer's storage made anew (generation moved), of a size other than 0 x 0: 1 when the platform made
       its image, 0 when it has none (the platform records its own error, if any); it then has no storage */
    int (*renderbuffer_storage)(uint32_t ctx, uint32_t renderbuffer);
    /* the renderbuffer deleted, or its storage dropped: the platform's image goes */
    void (*renderbuffer_gone)(uint32_t renderbuffer);
    /* a framebuffer's attachments or draw buffers changed, or one went: the platform's render targets are stale */
    void (*changed)(void);
    /* the window's depth and stencil bits, which glGetFramebufferAttachmentParameteriv answers for framebuffer 0 */
    void (*window_bits)(uint32_t ctx, int32_t* depth, int32_t* stencil);
    uint32_t (*max_size)(void); /* the largest width and height the platform makes now; without it 16384 */
    /* the bytes a renderbuffer's storage takes in the platform's own layout, at *align, its record just set: the
       layer takes them from the platform memory (gl_object.h's) for the platform to draw into. 0, or without it: the
       renderbuffer keeps no storage there */
    uint64_t (*storage_bytes)(uint32_t renderbuffer, uint64_t* align);
} SnxGlFramebufferHost;
void snx_gl_framebuffer_host(const SnxGlFramebufferHost* host);

/* a context made anew: the window's framebuffer bound, no renderbuffer */
void snx_gl_framebuffer_context_reset(uint32_t ctx);
SnxGlFramebufferContext* snx_gl_framebuffers(uint32_t ctx);
SnxGlRenderbuffer* snx_gl_renderbuffer(uint32_t name);
SnxGlFramebuffer* snx_gl_framebuffer(uint32_t name);
/* glCheckFramebufferStatus's answer for a framebuffer: GL_FRAMEBUFFER_COMPLETE, or why it is not */
uint32_t snx_gl_framebuffer_status(uint32_t name);
/* the size, format (gl_format.h's index) and samples of an attachment's image: 0 when it has none */
int snx_gl_attachment_image(const SnxGlAttachment* a, int32_t* w, int32_t* h, int32_t* format, int32_t* samples);
/* glGet*'s framebuffer, read framebuffer and renderbuffer bindings, read buffer and draw buffers of pname into v[0]:
   1, or 0 for a pname not of them */
int snx_gl_framebuffer_get(uint32_t ctx, uint32_t pname, double v[4]);

/* GL's calls */
void snx_gl_gen_renderbuffers(uint32_t ctx, int32_t n, uint32_t* out);
void snx_gl_delete_renderbuffers(uint32_t ctx, int32_t n, const uint32_t* names);
int snx_gl_is_renderbuffer(uint32_t name);
void snx_gl_bind_renderbuffer(uint32_t ctx, uint32_t target, uint32_t name);
/* glRenderbufferStorage (samples 0) and glRenderbufferStorageMultisample */
void snx_gl_renderbuffer_storage(uint32_t ctx, uint32_t target, int32_t samples, uint32_t internal, int32_t w,
                                 int32_t h);
/* glGetRenderbufferParameteriv: 1 with the value in *v, 0 with the error recorded */
int snx_gl_get_renderbuffer_parameter(uint32_t ctx, uint32_t target, uint32_t pname, int32_t* v);
void snx_gl_gen_framebuffers(uint32_t ctx, int32_t n, uint32_t* out);
void snx_gl_delete_framebuffers(uint32_t ctx, int32_t n, const uint32_t* names);
int snx_gl_is_framebuffer(uint32_t name);
void snx_gl_bind_framebuffer(uint32_t ctx, uint32_t target, uint32_t name);
/* glFramebufferTexture2D (textarget a cube map's face or the texture's target), glFramebufferTextureLayer and
   glFramebufferTexture (layer 0) */
void snx_gl_framebuffer_texture(uint32_t ctx, uint32_t target, uint32_t attachment, uint32_t textarget,
                                uint32_t texture, int32_t level, int32_t layer);
void snx_gl_framebuffer_renderbuffer(uint32_t ctx, uint32_t target, uint32_t attachment, uint32_t rbtarget,
                                     uint32_t renderbuffer);
/* the framebuffer bound to target's status; 0 with GL_INVALID_ENUM recorded for a target GL has not */
uint32_t snx_gl_check_framebuffer_status(uint32_t ctx, uint32_t target);
/* glGetFramebufferAttachmentParameteriv: 1 with the value in *v, 0 with the error recorded */
int snx_gl_get_framebuffer_attachment_parameter(uint32_t ctx, uint32_t target, uint32_t attachment, uint32_t pname,
                                                int32_t* v);
void snx_gl_draw_buffers(uint32_t ctx, int32_t n, const uint32_t* buffers);
void snx_gl_read_buffer(uint32_t ctx, uint32_t mode);

/* the save state's part (after the textures'): its bytes, a copy, the check (why names the field and its value) and
   its load */
size_t snx_gl_framebuffer_state_bytes(void);
void snx_gl_framebuffer_state_save(void* out);
int snx_gl_framebuffer_state_check(const void* in, size_t size, char* why, size_t whysize);
void snx_gl_framebuffer_state_load(const void* in);

#endif
