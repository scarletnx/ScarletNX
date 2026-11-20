/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's framebuffers and renderbuffers (gl_framebuffer.h) */
#include "common/gpu/gl/gl_framebuffer.h"
#include "common/gpu/gl/gl_format.h"
#include "common/gpu/gl/gl_object.h"
#include "common/gpu/gl/gl_state.h"
#include "common/gpu/gl/gl_texture.h"
#include <stdio.h>
#include <string.h>

enum {
    GL_OUT_OF_MEMORY_ = 0x0505,
    GL_BACK_ = 0x0405,
    GL_TEXTURE_ = 0x1702,
    GL_TEXTURE_CUBE_MAP_POSITIVE_X_ = 0x8515,
    GL_RENDERBUFFER_ = 0x8d41,
    GL_FRAMEBUFFER_ = 0x8d40,
    GL_READ_FRAMEBUFFER_ = 0x8ca8,
    GL_DRAW_FRAMEBUFFER_ = 0x8ca9,
    GL_COLOR_ATTACHMENT0_ = 0x8ce0,
    GL_DEPTH_ATTACHMENT_ = 0x8d00,
    GL_STENCIL_ATTACHMENT_ = 0x8d20,
    GL_DEPTH_STENCIL_ATTACHMENT_ = 0x821a,
    GL_FRAMEBUFFER_COMPLETE_ = 0x8cd5,
    GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_ = 0x8cd6,
    GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_ = 0x8cd7,
    GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS_ = 0x8cd9,
    GL_FRAMEBUFFER_UNSUPPORTED_ = 0x8cdd,
    GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE_ = 0x8d56,
    GL_FRAMEBUFFER_DEFAULT_ = 0x8218,
    GL_UNSIGNED_NORMALIZED_ = 0x8c17,
    GL_LINEAR_ = 0x2601,
    GL_RGBA4_ = 0x8056,
};

/* what a save state holds */
typedef struct {
    SnxGlRenderbuffer renderbuffers[SNX_GL_RENDERBUFFERS];
    SnxGlFramebuffer framebuffers[SNX_GL_FRAMEBUFFERS];
    SnxGlFramebufferContext contexts[SNX_GL_CONTEXTS];
} SnxGlFramebuffers;

static struct {
    SnxGlFramebuffers s;
    SnxGlFramebuffers checked; /* a save state's copy being checked: a check never touches the running state */
    SnxGlFramebufferHost host;
} g_gl_framebuffer;

#define S g_gl_framebuffer.s
#define H g_gl_framebuffer.host

void snx_gl_framebuffer_host(const SnxGlFramebufferHost* host) { H = host ? *host : (SnxGlFramebufferHost){0}; }
static uint32_t max_size(void) { return H.max_size ? H.max_size() : 16384; }
static void error(uint32_t ctx, uint32_t e) {
    if (ctx) snx_gl_error(ctx, e);
}
static void changed(void) {
    if (H.changed) H.changed();
}

/* ---- the records ---- */

SnxGlFramebufferContext* snx_gl_framebuffers(uint32_t ctx) { return &S.contexts[ctx - 1]; }
void snx_gl_framebuffer_context_reset(uint32_t ctx) { S.contexts[ctx - 1] = (SnxGlFramebufferContext){0}; }
SnxGlRenderbuffer* snx_gl_renderbuffer(uint32_t name) {
    return name && name <= SNX_GL_RENDERBUFFERS && S.renderbuffers[name - 1].used ? &S.renderbuffers[name - 1] : NULL;
}
SnxGlFramebuffer* snx_gl_framebuffer(uint32_t name) {
    return name && name <= SNX_GL_FRAMEBUFFERS && S.framebuffers[name - 1].used ? &S.framebuffers[name - 1] : NULL;
}
static void renderbuffer_init(SnxGlRenderbuffer* r) { *r = (SnxGlRenderbuffer){.used = 1, .format = -1, .samples = 1}; }
static void framebuffer_init(SnxGlFramebuffer* f) {
    *f = (SnxGlFramebuffer){.used = 1, .read = GL_COLOR_ATTACHMENT0_};
    f->draw[0] = GL_COLOR_ATTACHMENT0_;
}
static int framebuffer_target(uint32_t target) {
    return target == GL_FRAMEBUFFER_ || target == GL_READ_FRAMEBUFFER_ || target == GL_DRAW_FRAMEBUFFER_;
}
/* the name bound to target: the read framebuffer's for GL_READ_FRAMEBUFFER, else the draw one's */
static uint32_t bound(uint32_t ctx, uint32_t target) {
    const SnxGlFramebufferContext* c = snx_gl_framebuffers(ctx);
    return target == GL_READ_FRAMEBUFFER_ ? c->read : c->draw;
}

int snx_gl_attachment_image(const SnxGlAttachment* a, int32_t* w, int32_t* h, int32_t* format, int32_t* samples) {
    if (a->kind == SNX_GL_ATTACH_TEXTURE) {
        const SnxGlTexture* t = snx_gl_texture(a->name);
        if (!t || t->format < 0 || a->level < 0 || a->level >= t->levels) return 0;
        *w = snx_gl_level_size(t->width, a->level), *h = snx_gl_level_size(t->height, a->level);
        *format = t->format, *samples = t->samples > 1 ? t->samples : 1;
        return 1;
    }
    if (a->kind == SNX_GL_ATTACH_RENDERBUFFER) {
        const SnxGlRenderbuffer* r = snx_gl_renderbuffer(a->name);
        if (!r || r->format < 0 || !r->width || !r->height) return 0;
        *w = r->width, *h = r->height, *format = r->format, *samples = r->samples;
        return 1;
    }
    return 0;
}
uint32_t snx_gl_framebuffer_status(uint32_t name) {
    const SnxGlFramebuffer* fb = snx_gl_framebuffer(name);
    int count = 0;
    int32_t width = -1, height = -1, samples = 0;
    const SnxGlAttachment *d, *st;
    if (!fb) return GL_FRAMEBUFFER_COMPLETE_; /* the window's */
    for (int i = 0; i < SNX_GL_ATTACHMENTS; i++) {
        const SnxGlAttachment* a = &fb->at[i];
        int32_t w, h, format, s;
        int depth;
        if (!a->kind) continue;
        if (!snx_gl_attachment_image(a, &w, &h, &format, &s)) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_;
        depth = snx_gl_format_at(format)->kind >= SNX_GL_DEPTH;
        if ((i < SNX_GL_COLORS && depth) || (i >= SNX_GL_COLORS && !depth))
            return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_;
        if (count && (w != width || h != height)) return GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS_;
        if (count && s != samples) return GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE_;
        width = w, height = h, samples = s;
        count++;
    }
    if (!count) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_;
    d = &fb->at[SNX_GL_DEPTH_ATTACHMENT], st = &fb->at[SNX_GL_STENCIL_ATTACHMENT];
    if (d->kind && st->kind && (d->kind != st->kind || d->name != st->name)) return GL_FRAMEBUFFER_UNSUPPORTED_;
    return GL_FRAMEBUFFER_COMPLETE_;
}
int snx_gl_framebuffer_get(uint32_t ctx, uint32_t pname, double v[4]) {
    const SnxGlFramebufferContext* c = snx_gl_framebuffers(ctx);
    const SnxGlFramebuffer* fb;
    switch (pname) {
        case 0x8ca6: v[0] = c->draw; return 1;         /* GL_DRAW_FRAMEBUFFER_BINDING */
        case 0x8caa: v[0] = c->read; return 1;         /* GL_READ_FRAMEBUFFER_BINDING */
        case 0x8ca7: v[0] = c->renderbuffer; return 1; /* GL_RENDERBUFFER_BINDING */
        case 0x0c02:                                   /* GL_READ_BUFFER */
            fb = snx_gl_framebuffer(c->read);
            v[0] = fb ? fb->read : GL_BACK_;
            return 1;
        default:
            if (pname >= 0x8825 && pname < 0x8825 + SNX_GL_COLORS) { /* GL_DRAW_BUFFERi */
                fb = snx_gl_framebuffer(c->draw);
                v[0] = fb ? fb->draw[pname - 0x8825] : pname == 0x8825 ? GL_BACK_ : 0;
                return 1;
            }
            return 0;
    }
}

/* ---- renderbuffers ---- */

/* n unused names of a table of count records of size bytes into out, each made by init: GL_OUT_OF_MEMORY past it */
#define GEN(table, count, init)                                                                                        \
    do {                                                                                                               \
        uint32_t name = 1;                                                                                             \
        if (n < 0) {                                                                                                   \
            error(ctx, SNX_GL_INVALID_VALUE);                                                                          \
            return;                                                                                                    \
        }                                                                                                              \
        for (int32_t i = 0; i < n; i++) {                                                                              \
            uint32_t v = 0;                                                                                            \
            while (name <= (count) && S.table[name - 1].used) name++;                                                  \
            if (name <= (count)) {                                                                                     \
                init(&S.table[name - 1]);                                                                              \
                v = name++;                                                                                            \
            } else {                                                                                                   \
                error(ctx, GL_OUT_OF_MEMORY_);                                                                         \
            }                                                                                                          \
            memcpy(out + i, &v, 4);                                                                                    \
        }                                                                                                              \
    } while (0)

void snx_gl_gen_renderbuffers(uint32_t ctx, int32_t n, uint32_t* out) {
    GEN(renderbuffers, SNX_GL_RENDERBUFFERS, renderbuffer_init);
}
void snx_gl_delete_renderbuffers(uint32_t ctx, int32_t n, const uint32_t* names) {
    if (n < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t name;
        memcpy(&name, names + i, 4);
        if (!snx_gl_renderbuffer(name)) continue;
        if (S.renderbuffers[name - 1].storage)
            snx_gl_memory_give(S.renderbuffers[name - 1].storage, S.renderbuffers[name - 1].storage_size);
        S.renderbuffers[name - 1] = (SnxGlRenderbuffer){0};
        if (H.renderbuffer_gone) H.renderbuffer_gone(name);
        if (ctx && snx_gl_framebuffers(ctx)->renderbuffer == name) snx_gl_framebuffers(ctx)->renderbuffer = 0;
        changed();
    }
}
int snx_gl_is_renderbuffer(uint32_t name) { return snx_gl_renderbuffer(name) != NULL; }
void snx_gl_bind_renderbuffer(uint32_t ctx, uint32_t target, uint32_t name) {
    if (target != GL_RENDERBUFFER_) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return;
    }
    if (name > SNX_GL_RENDERBUFFERS) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    if (name && !snx_gl_renderbuffer(name)) renderbuffer_init(&S.renderbuffers[name - 1]); /* made by its bind */
    snx_gl_framebuffers(ctx)->renderbuffer = name;
}
void snx_gl_renderbuffer_storage(uint32_t ctx, uint32_t target, int32_t samples, uint32_t internal, int32_t w,
                                 int32_t h) {
    uint32_t name = snx_gl_framebuffers(ctx)->renderbuffer;
    SnxGlRenderbuffer* r = snx_gl_renderbuffer(name);
    int32_t index = snx_gl_format(internal, 0, 0);
    const SnxGlFormat* f = snx_gl_format_at(index);
    if (target != GL_RENDERBUFFER_) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return;
    }
    if (!r) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    if (!f || f->block) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return;
    }
    if (w < 0 || h < 0 || (uint32_t)w > max_size() || (uint32_t)h > max_size() || samples < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    if (H.renderbuffer_gone) H.renderbuffer_gone(name);
    if (r->storage) snx_gl_memory_give(r->storage, r->storage_size);
    r->storage = r->storage_size = 0;
    r->format = index, r->width = w, r->height = h;
    r->samples = samples > 2 ? 4 : samples >= 1 ? 2 : 1; /* the counts every device has */
    r->generation++;
    changed();
    if (w && h && H.storage_bytes) {
        uint64_t align = 256, size = H.storage_bytes(name, &align);
        r->storage = size ? snx_gl_memory_take(size, align) : 0;
        if (size && !r->storage) {
            r->format = -1;
            error(ctx, GL_OUT_OF_MEMORY_);
            return;
        }
        r->storage_size = r->storage ? size : 0;
    }
    if (w && h && H.renderbuffer_storage && !H.renderbuffer_storage(ctx, name)) r->format = -1;
}
int snx_gl_get_renderbuffer_parameter(uint32_t ctx, uint32_t target, uint32_t pname, int32_t* v) {
    const SnxGlRenderbuffer* r = snx_gl_renderbuffer(snx_gl_framebuffers(ctx)->renderbuffer);
    const SnxGlFormat* f;
    if (target != GL_RENDERBUFFER_) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return 0;
    }
    if (!r) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return 0;
    }
    f = snx_gl_format_at(r->format);
    switch (pname) {
        case 0x8d42: *v = r->width; return 1;                             /* GL_RENDERBUFFER_WIDTH */
        case 0x8d43: *v = r->height; return 1;                            /* GL_RENDERBUFFER_HEIGHT */
        case 0x8d44: *v = f ? (int32_t)f->internal : GL_RGBA4_; return 1; /* _INTERNAL_FORMAT */
        case 0x8cab: *v = r->samples > 1 ? r->samples : 0; return 1;      /* GL_RENDERBUFFER_SAMPLES */
        case 0x8d54: *v = f && (f->kind == SNX_GL_DEPTH || f->kind == SNX_GL_DS) ? 24 : 0; return 1; /* _DEPTH_SIZE */
        case 0x8d55: *v = f && (f->kind == SNX_GL_STENCIL || f->kind == SNX_GL_DS) ? 8 : 0; return 1;
        default:
            if (pname >= 0x8d50 && pname <= 0x8d53) { /* GL_RENDERBUFFER_RED_SIZE to ALPHA */
                *v = f && f->kind < SNX_GL_DEPTH ? 8 : 0;
                return 1;
            }
            error(ctx, SNX_GL_INVALID_ENUM);
            return 0;
    }
}

/* ---- framebuffers ---- */

void snx_gl_gen_framebuffers(uint32_t ctx, int32_t n, uint32_t* out) {
    GEN(framebuffers, SNX_GL_FRAMEBUFFERS, framebuffer_init);
}
#undef GEN
void snx_gl_delete_framebuffers(uint32_t ctx, int32_t n, const uint32_t* names) {
    if (n < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t name;
        memcpy(&name, names + i, 4);
        if (!snx_gl_framebuffer(name)) continue;
        S.framebuffers[name - 1] = (SnxGlFramebuffer){0};
        if (ctx) {
            SnxGlFramebufferContext* c = snx_gl_framebuffers(ctx);
            if (c->draw == name) c->draw = 0;
            if (c->read == name) c->read = 0;
        }
        changed();
    }
}
int snx_gl_is_framebuffer(uint32_t name) { return snx_gl_framebuffer(name) != NULL; }
void snx_gl_bind_framebuffer(uint32_t ctx, uint32_t target, uint32_t name) {
    SnxGlFramebufferContext* c = snx_gl_framebuffers(ctx);
    if (!framebuffer_target(target)) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return;
    }
    if (name > SNX_GL_FRAMEBUFFERS) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    if (name && !snx_gl_framebuffer(name)) framebuffer_init(&S.framebuffers[name - 1]); /* made by its bind */
    if (target != GL_READ_FRAMEBUFFER_) c->draw = name;
    if (target != GL_DRAW_FRAMEBUFFER_) c->read = name;
}
/* the framebuffer bound to target: NULL with GL_INVALID_ENUM recorded for a target GL has not, GL_INVALID_OPERATION
   for the window's */
static SnxGlFramebuffer* target_framebuffer(uint32_t ctx, uint32_t target) {
    SnxGlFramebuffer* f;
    if (!framebuffer_target(target)) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return NULL;
    }
    f = snx_gl_framebuffer(bound(ctx, target));
    if (!f) error(ctx, SNX_GL_INVALID_OPERATION);
    return f;
}
/* the attachment points of a GL attachment, up to two (depth and stencil): their count, 0 for none */
static int attachment_points(uint32_t attachment, int out[2]) {
    if (attachment >= GL_COLOR_ATTACHMENT0_ && attachment < GL_COLOR_ATTACHMENT0_ + SNX_GL_COLORS) {
        out[0] = (int)(attachment - GL_COLOR_ATTACHMENT0_);
        return 1;
    }
    if (attachment == GL_DEPTH_ATTACHMENT_) return out[0] = SNX_GL_DEPTH_ATTACHMENT, 1;
    if (attachment == GL_STENCIL_ATTACHMENT_) return out[0] = SNX_GL_STENCIL_ATTACHMENT, 1;
    if (attachment == GL_DEPTH_STENCIL_ATTACHMENT_) {
        out[0] = SNX_GL_DEPTH_ATTACHMENT, out[1] = SNX_GL_STENCIL_ATTACHMENT;
        return 2;
    }
    return 0;
}
static void attach(uint32_t ctx, uint32_t target, uint32_t attachment, SnxGlAttachment a) {
    SnxGlFramebuffer* f = target_framebuffer(ctx, target);
    int points[2], n = attachment_points(attachment, points);
    if (!f) return;
    if (!n) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return;
    }
    if (!a.name) a = (SnxGlAttachment){0};
    for (int i = 0; i < n; i++) f->at[points[i]] = a;
    f->generation++;
    changed();
}
void snx_gl_framebuffer_texture(uint32_t ctx, uint32_t target, uint32_t attachment, uint32_t textarget,
                                uint32_t texture, int32_t level, int32_t layer) {
    if (texture && !snx_gl_texture(texture)) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    if (textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X_ && textarget < GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + 6)
        layer = (int32_t)(textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X_);
    attach(ctx, target, attachment,
           (SnxGlAttachment){.kind = SNX_GL_ATTACH_TEXTURE, .name = texture, .level = level, .layer = layer});
}
void snx_gl_framebuffer_renderbuffer(uint32_t ctx, uint32_t target, uint32_t attachment, uint32_t rbtarget,
                                     uint32_t renderbuffer) {
    if (rbtarget != GL_RENDERBUFFER_) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return;
    }
    if (renderbuffer && !snx_gl_renderbuffer(renderbuffer)) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    attach(ctx, target, attachment, (SnxGlAttachment){.kind = SNX_GL_ATTACH_RENDERBUFFER, .name = renderbuffer});
}
uint32_t snx_gl_check_framebuffer_status(uint32_t ctx, uint32_t target) {
    if (!framebuffer_target(target)) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return 0;
    }
    return snx_gl_framebuffer_status(bound(ctx, target));
}
int snx_gl_get_framebuffer_attachment_parameter(uint32_t ctx, uint32_t target, uint32_t attachment, uint32_t pname,
                                                int32_t* v) {
    const SnxGlFramebuffer* f;
    const SnxGlAttachment* a;
    int points[2];
    if (!framebuffer_target(target)) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return 0;
    }
    f = snx_gl_framebuffer(bound(ctx, target));
    if (!f) { /* the window's */
        int32_t depth = 0, stencil = 0;
        if (H.window_bits) H.window_bits(ctx, &depth, &stencil);
        *v = pname == 0x8cd0                      ? GL_FRAMEBUFFER_DEFAULT_ /* _OBJECT_TYPE */
             : pname >= 0x8212 && pname <= 0x8215 ? 8                       /* _RED_SIZE to _ALPHA_SIZE */
             : pname == 0x8216                    ? depth
             : pname == 0x8217                    ? stencil
             : pname == 0x8211                    ? GL_UNSIGNED_NORMALIZED_ /* _COMPONENT_TYPE */
                                                  : 0;
        return 1;
    }
    if (!attachment_points(attachment, points)) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return 0;
    }
    a = &f->at[points[0]];
    switch (pname) {
        case 0x8cd0: /* GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE */
            *v = a->kind == SNX_GL_ATTACH_TEXTURE        ? GL_TEXTURE_
                 : a->kind == SNX_GL_ATTACH_RENDERBUFFER ? GL_RENDERBUFFER_
                                                         : 0;
            return 1;
        case 0x8cd1: *v = (int32_t)a->name; return 1; /* _OBJECT_NAME */
        case 0x8cd2: *v = a->level; return 1;         /* _TEXTURE_LEVEL */
        case 0x8cd3:                                  /* _TEXTURE_CUBE_MAP_FACE */
            *v = a->kind == SNX_GL_ATTACH_TEXTURE ? (int32_t)GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + a->layer : 0;
            return 1;
        case 0x8cd4: *v = a->layer; return 1;                /* _TEXTURE_LAYER */
        case 0x8210: *v = GL_LINEAR_; return 1;              /* _COLOR_ENCODING */
        case 0x8211: *v = GL_UNSIGNED_NORMALIZED_; return 1; /* _COMPONENT_TYPE */
        case 0x8212:                                         /* _RED_SIZE to _ALPHA_SIZE */
        case 0x8213:
        case 0x8214:
        case 0x8215: *v = points[0] < SNX_GL_COLORS && a->kind ? 8 : 0; return 1;
        case 0x8216: *v = points[0] == SNX_GL_DEPTH_ATTACHMENT && a->kind ? 24 : 0; return 1;
        case 0x8217: *v = points[0] == SNX_GL_STENCIL_ATTACHMENT && a->kind ? 8 : 0; return 1;
        default: error(ctx, SNX_GL_INVALID_ENUM); return 0;
    }
}
void snx_gl_draw_buffers(uint32_t ctx, int32_t n, const uint32_t* buffers) {
    SnxGlFramebuffer* f;
    if (n < 0 || n > SNX_GL_COLORS) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    f = snx_gl_framebuffer(snx_gl_framebuffers(ctx)->draw);
    if (!f) return; /* the window's: GL_BACK or GL_NONE, both its one buffer here */
    for (int32_t i = 0; i < SNX_GL_COLORS; i++) {
        uint32_t b = 0;
        if (i < n) memcpy(&b, buffers + i, 4);
        f->draw[i] = b;
    }
    f->generation++;
    changed();
}
void snx_gl_read_buffer(uint32_t ctx, uint32_t mode) {
    SnxGlFramebuffer* f = snx_gl_framebuffer(snx_gl_framebuffers(ctx)->read);
    if (f) f->read = mode;
}

/* ---- the save state ---- */

size_t snx_gl_framebuffer_state_bytes(void) { return 2 * sizeof(uint32_t) + sizeof S; }
void snx_gl_framebuffer_state_save(void* out) {
    const uint32_t h[2] = {(uint32_t)sizeof S, 0};
    memcpy(out, h, sizeof h);
    memcpy((unsigned char*)out + sizeof h, &S, sizeof S);
}
int snx_gl_framebuffer_state_check(const void* in, size_t size, char* why, size_t whysize) {
    SnxGlFramebuffers* const c = &g_gl_framebuffer.checked;
    uint32_t h[2];
    if (size < sizeof h) {
        snprintf(why, whysize, "the GL layer's framebuffers are %zu bytes, less than their %zu-byte header", size,
                 sizeof h);
        return -1;
    }
    memcpy(h, in, sizeof h);
    if (h[0] != sizeof *c || size != sizeof h + sizeof *c) {
        snprintf(why, whysize, "the GL layer's framebuffers are %u bytes of records (%zu in all); this build reads %zu",
                 (unsigned)h[0], size, sizeof *c);
        return -1;
    }
    memcpy(c, (const unsigned char*)in + sizeof h, sizeof *c);
    for (uint32_t n = 1; n <= SNX_GL_RENDERBUFFERS; n++) {
        const SnxGlRenderbuffer* r = &c->renderbuffers[n - 1];
        if (r->used && r->storage && !snx_gl_object_checked_holds(r->storage, r->storage_size)) {
            snprintf(why, whysize, "the GL layer's renderbuffer %u keeps its storage at %llx+%llx, outside its memory",
                     (unsigned)n, (unsigned long long)r->storage, (unsigned long long)r->storage_size);
            return -1;
        }
        if (r->used && (r->format < -1 || r->format >= SNX_GL_FORMATS || r->width < 0 || r->height < 0)) {
            snprintf(why, whysize, "the GL layer's renderbuffer %u has format %d of %dx%d, which GL has not",
                     (unsigned)n, (int)r->format, (int)r->width, (int)r->height);
            return -1;
        }
    }
    for (uint32_t n = 1; n <= SNX_GL_FRAMEBUFFERS; n++) {
        const SnxGlFramebuffer* f = &c->framebuffers[n - 1];
        for (int i = 0; f->used && i < SNX_GL_ATTACHMENTS; i++) {
            const SnxGlAttachment* a = &f->at[i];
            uint32_t most = a->kind == SNX_GL_ATTACH_TEXTURE ? SNX_GL_TEXTURES : SNX_GL_RENDERBUFFERS;
            if (a->kind > SNX_GL_ATTACH_RENDERBUFFER || a->name > most) {
                snprintf(why, whysize, "the GL layer's framebuffer %u has attachment %d of kind %u and name %u",
                         (unsigned)n, i, (unsigned)a->kind, (unsigned)a->name);
                return -1;
            }
        }
    }
    for (uint32_t i = 0; i < SNX_GL_CONTEXTS; i++) {
        const SnxGlFramebufferContext* x = &c->contexts[i];
        if (x->draw > SNX_GL_FRAMEBUFFERS || x->read > SNX_GL_FRAMEBUFFERS || x->renderbuffer > SNX_GL_RENDERBUFFERS) {
            snprintf(why, whysize,
                     "the GL layer's context %u binds framebuffers %u and %u and renderbuffer %u, past %d",
                     (unsigned)(i + 1), (unsigned)x->draw, (unsigned)x->read, (unsigned)x->renderbuffer,
                     SNX_GL_FRAMEBUFFERS);
            return -1;
        }
    }
    return 0;
}
void snx_gl_framebuffer_state_load(const void* in) {
    memcpy(&S, (const unsigned char*)in + 2 * sizeof(uint32_t), sizeof S);
    for (uint32_t n = 1; H.renderbuffer_gone && n <= SNX_GL_RENDERBUFFERS; n++) H.renderbuffer_gone(n);
    changed();
}
