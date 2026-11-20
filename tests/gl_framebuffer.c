/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shared GL layer's framebuffers and renderbuffers (src/common/gpu/gl/gl_framebuffer.c), with no GPU: names and
   bindings (the draw and read framebuffers apart, made by their bind as GLES makes them), renderbuffer storage and its
   parameters, a storage the platform could not make; attachments of textures (a cube map's face) and renderbuffers,
   depth and stencil at once, and completeness (a missing image, a color format at the depth point, sizes and samples
   that differ, depth and stencil of two objects); the window's attachment parameters; draw and read buffers; what the
   platform hears; deletes clearing their bindings; and a save state's copy loaded back whole and refused when a
   renderbuffer, an attachment or a binding in it is wrong */
#include "common/gpu/gl/gl_format.h"
#include "common/gpu/gl/gl_framebuffer.h"
#include "common/gpu/gl/gl_object.h"
#include "common/gpu/gl/gl_program.h"
#include "common/gpu/gl/gl_state.h"
#include "common/gpu/gl/gl_sync.h"
#include "common/gpu/gl/gl_texture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    T2D = 0x0de1,
    CUBE = 0x8513,
    CUBE_X = 0x8515,
    RGBA = 0x1908,
    RGBA8 = 0x8058,
    UBYTE = 0x1401,
    DEPTH24_STENCIL8 = 0x88f0,
    DEPTH16 = 0x81a5,
    STENCIL8 = 0x8d48,
    RENDERBUFFER = 0x8d41,
    FRAMEBUFFER = 0x8d40,
    READ_FRAMEBUFFER = 0x8ca8,
    DRAW_FRAMEBUFFER = 0x8ca9,
    COLOR0 = 0x8ce0,
    DEPTH = 0x8d00,
    STENCIL = 0x8d20,
    DEPTH_STENCIL = 0x821a,
    COMPLETE = 0x8cd5,
    INCOMPLETE_ATTACHMENT = 0x8cd6,
    MISSING_ATTACHMENT = 0x8cd7,
    DIMENSIONS = 0x8cd9,
    UNSUPPORTED = 0x8cdd,
    MULTISAMPLE = 0x8d56,
};

/* the platform: what it heard */
static struct {
    unsigned storages, gone, changes;
    int refuse;
    uint32_t laid_out;                   /* the renderbuffer storage_bytes was last asked for */
    uint64_t layout_bytes, layout_align; /* what it answers */
} heard;
/* the platform's memory: chunks of the test's */
static uint64_t chunk(uint64_t size, uint64_t* got) {
    void* p = calloc(1, (size_t)size);
    *got = p ? size : 0;
    return (uint64_t)(uintptr_t)p;
}
static uint8_t* bytes(uint64_t at) { return (uint8_t*)(uintptr_t)at; }
static uint64_t layout_bytes(uint32_t renderbuffer, uint64_t* align) {
    heard.laid_out = renderbuffer;
    *align = heard.layout_align;
    return heard.layout_bytes;
}
static int storage(uint32_t ctx, uint32_t renderbuffer) {
    (void)ctx, (void)renderbuffer;
    heard.storages++;
    return !heard.refuse;
}
static void gone(uint32_t renderbuffer) {
    (void)renderbuffer;
    heard.gone++;
}
static void changed(void) { heard.changes++; }
static void window_bits(uint32_t ctx, int32_t* depth, int32_t* stencil) {
    (void)ctx;
    *depth = 24, *stencil = 8;
}
static uint32_t max_size(void) { return 4096; }
static int32_t rb_parameter(uint32_t ctx, uint32_t pname) {
    int32_t v = -1;
    assert(snx_gl_get_renderbuffer_parameter(ctx, RENDERBUFFER, pname, &v));
    return v;
}
static int32_t attachment_parameter(uint32_t ctx, uint32_t target, uint32_t attachment, uint32_t pname) {
    int32_t v = -1;
    assert(snx_gl_get_framebuffer_attachment_parameter(ctx, target, attachment, pname, &v));
    return v;
}
static void refused(const void* copy, size_t size, const char* text) {
    char why[300];
    assert(snx_gl_state_check(copy, size, why, sizeof why) < 0);
    if (!strstr(why, text)) {
        fprintf(stderr, "gl_framebuffer: refused for \"%s\", not naming \"%s\"\n", why, text);
        abort();
    }
}

int main(void) {
    uint32_t ctx = snx_gl_context_new(SNX_GL_API_ES, 32, 0), rb[3] = {0}, fb[2] = {0}, tex[2] = {0};
    double v[4];
    assert(ctx);
    snx_gl_framebuffer_host(&(SnxGlFramebufferHost){.renderbuffer_storage = storage,
                                                    .renderbuffer_gone = gone,
                                                    .changed = changed,
                                                    .window_bits = window_bits,
                                                    .max_size = max_size});

    /* renderbuffers: names, a bind of a target GL has not, storage of a format and size, its parameters */
    snx_gl_gen_renderbuffers(ctx, 3, rb);
    assert(rb[0] == 1 && rb[2] == 3 && snx_gl_is_renderbuffer(2) && !snx_gl_is_renderbuffer(4));
    snx_gl_bind_renderbuffer(ctx, 0x1234, rb[0]);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM && snx_gl_framebuffers(ctx)->renderbuffer == 0);
    snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 0, RGBA8, 16, 16);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION); /* none bound */
    snx_gl_bind_renderbuffer(ctx, RENDERBUFFER, rb[0]);
    assert(snx_gl_get(ctx, 0x8ca7, v) == 1 && v[0] == rb[0]);
    snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 0, 0x83f1, 16, 16); /* a compressed format */
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
    snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 0, RGBA8, 4097, 16);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE && heard.storages == 0);
    snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 0, RGBA8, 16, 8);
    assert(heard.storages == 1 && heard.gone == 1 && snx_gl_renderbuffer(rb[0])->generation == 1);
    assert(rb_parameter(ctx, 0x8d42) == 16 && rb_parameter(ctx, 0x8d43) == 8 && rb_parameter(ctx, 0x8d44) == RGBA8);
    assert(rb_parameter(ctx, 0x8cab) == 0 && rb_parameter(ctx, 0x8d50) == 8 && rb_parameter(ctx, 0x8d54) == 0);
    {
        int32_t x = 0;
        assert(!snx_gl_get_renderbuffer_parameter(ctx, RENDERBUFFER, 0x1234, &x));
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
    }
    /* multisampled: 1 sample asked is 2, more than 2 is 4 */
    snx_gl_bind_renderbuffer(ctx, RENDERBUFFER, rb[1]);
    snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 1, DEPTH24_STENCIL8, 16, 8);
    assert(rb_parameter(ctx, 0x8cab) == 2 && rb_parameter(ctx, 0x8d54) == 24 && rb_parameter(ctx, 0x8d55) == 8);
    snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 3, DEPTH24_STENCIL8, 16, 8);
    assert(rb_parameter(ctx, 0x8cab) == 4 && heard.storages == 3);
    /* a storage the platform could not make: none, the next tries again */
    snx_gl_bind_renderbuffer(ctx, RENDERBUFFER, rb[2]);
    heard.refuse = 1;
    snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 0, DEPTH16, 16, 8);
    assert(snx_gl_renderbuffer(rb[2])->format == -1 && rb_parameter(ctx, 0x8d44) == 0x8056);
    heard.refuse = 0;
    snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 0, DEPTH16, 16, 8);
    assert(snx_gl_renderbuffer(rb[2])->format == snx_gl_format(DEPTH16, 0, 0));
    /* a 0 x 0 storage asks the platform for nothing */
    {
        uint32_t empty = 0;
        unsigned made = heard.storages;
        snx_gl_gen_renderbuffers(ctx, 1, &empty);
        snx_gl_bind_renderbuffer(ctx, RENDERBUFFER, empty);
        snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 0, RGBA8, 0, 0);
        assert(heard.storages == made && snx_gl_renderbuffer(empty)->format >= 0);
    }

    /* framebuffers: made by their bind; the draw and read bindings apart */
    snx_gl_gen_framebuffers(ctx, 2, fb);
    assert(fb[0] == 1 && fb[1] == 2 && snx_gl_is_framebuffer(1));
    snx_gl_bind_framebuffer(ctx, FRAMEBUFFER, fb[0]);
    assert(snx_gl_get(ctx, 0x8ca6, v) == 1 && v[0] == fb[0] && snx_gl_get(ctx, 0x8caa, v) == 1 && v[0] == fb[0]);
    snx_gl_bind_framebuffer(ctx, READ_FRAMEBUFFER, fb[1]);
    assert(snx_gl_framebuffers(ctx)->draw == fb[0] && snx_gl_framebuffers(ctx)->read == fb[1]);
    snx_gl_bind_framebuffer(ctx, READ_FRAMEBUFFER, 77);
    assert(snx_gl_is_framebuffer(77) && snx_gl_framebuffer(77)->read == COLOR0);
    snx_gl_bind_framebuffer(ctx, 0x1234, fb[0]);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
    snx_gl_bind_framebuffer(ctx, DRAW_FRAMEBUFFER, SNX_GL_FRAMEBUFFERS + 1);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE && snx_gl_framebuffers(ctx)->draw == fb[0]);
    snx_gl_bind_framebuffer(ctx, READ_FRAMEBUFFER, fb[0]);
    /* nothing attached: missing */
    assert(snx_gl_check_framebuffer_status(ctx, FRAMEBUFFER) == MISSING_ATTACHMENT);
    assert(!snx_gl_check_framebuffer_status(ctx, 0x1234) && snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);

    /* a texture's level and a renderbuffer: complete, each attachment's parameters */
    snx_gl_gen_textures(ctx, 2, tex);
    snx_gl_bind_texture(ctx, T2D, tex[0]);
    snx_gl_tex_image(ctx, T2D, 0, RGBA8, 32, 16, 1, RGBA, UBYTE, 0, 0, 0);
    {
        unsigned before = heard.changes;
        snx_gl_framebuffer_texture(ctx, FRAMEBUFFER, COLOR0, T2D, tex[0], 1, 0);
        assert(heard.changes == before + 1 && snx_gl_framebuffer(fb[0])->generation == 1);
    }
    snx_gl_bind_renderbuffer(ctx, RENDERBUFFER, rb[1]);
    snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 0, DEPTH24_STENCIL8, 16, 8);
    snx_gl_framebuffer_renderbuffer(ctx, FRAMEBUFFER, DEPTH_STENCIL, RENDERBUFFER, rb[1]);
    assert(snx_gl_check_framebuffer_status(ctx, DRAW_FRAMEBUFFER) == COMPLETE);
    assert(attachment_parameter(ctx, FRAMEBUFFER, COLOR0, 0x8cd0) == 0x1702); /* GL_TEXTURE */
    assert(attachment_parameter(ctx, FRAMEBUFFER, COLOR0, 0x8cd1) == (int32_t)tex[0]);
    assert(attachment_parameter(ctx, FRAMEBUFFER, COLOR0, 0x8cd2) == 1);
    assert(attachment_parameter(ctx, FRAMEBUFFER, STENCIL, 0x8cd0) == RENDERBUFFER);
    assert(attachment_parameter(ctx, FRAMEBUFFER, DEPTH, 0x8216) == 24);
    {
        int32_t x = 0;
        assert(!snx_gl_get_framebuffer_attachment_parameter(ctx, FRAMEBUFFER, 0x1234, 0x8cd0, &x));
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
    }
    /* sizes that differ, samples that differ, depth and stencil of two objects, a color format at the depth point */
    snx_gl_framebuffer_texture(ctx, FRAMEBUFFER, COLOR0, T2D, tex[0], 0, 0);
    assert(snx_gl_check_framebuffer_status(ctx, FRAMEBUFFER) == DIMENSIONS);
    snx_gl_framebuffer_texture(ctx, FRAMEBUFFER, COLOR0, T2D, tex[0], 1, 0);
    snx_gl_bind_renderbuffer(ctx, RENDERBUFFER, rb[0]);
    snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 2, RGBA8, 16, 8);
    snx_gl_framebuffer_renderbuffer(ctx, FRAMEBUFFER, COLOR0 + 1, RENDERBUFFER, rb[0]);
    assert(snx_gl_check_framebuffer_status(ctx, FRAMEBUFFER) == MULTISAMPLE);
    snx_gl_framebuffer_renderbuffer(ctx, FRAMEBUFFER, COLOR0 + 1, RENDERBUFFER, 0);
    assert(snx_gl_framebuffer(fb[0])->at[1].kind == SNX_GL_ATTACH_NONE);
    snx_gl_framebuffer_renderbuffer(ctx, FRAMEBUFFER, STENCIL, RENDERBUFFER, rb[2]); /* depth 16 at stencil */
    assert(snx_gl_check_framebuffer_status(ctx, FRAMEBUFFER) == UNSUPPORTED);
    snx_gl_framebuffer_renderbuffer(ctx, FRAMEBUFFER, STENCIL, RENDERBUFFER, rb[1]);
    assert(snx_gl_check_framebuffer_status(ctx, FRAMEBUFFER) == COMPLETE);
    snx_gl_framebuffer_texture(ctx, FRAMEBUFFER, DEPTH, T2D, tex[0], 1, 0); /* an RGBA texture as depth */
    assert(snx_gl_check_framebuffer_status(ctx, FRAMEBUFFER) == INCOMPLETE_ATTACHMENT);
    snx_gl_framebuffer_renderbuffer(ctx, FRAMEBUFFER, DEPTH, RENDERBUFFER, rb[1]);
    /* a level the texture has not: incomplete */
    snx_gl_framebuffer_texture(ctx, FRAMEBUFFER, COLOR0, T2D, tex[0], 9, 0);
    assert(snx_gl_check_framebuffer_status(ctx, FRAMEBUFFER) == INCOMPLETE_ATTACHMENT);
    snx_gl_framebuffer_texture(ctx, FRAMEBUFFER, COLOR0, T2D, tex[0], 1, 0);
    /* names that are none, targets GL has not */
    snx_gl_framebuffer_texture(ctx, FRAMEBUFFER, COLOR0, T2D, 99, 0, 0);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION && snx_gl_framebuffer(fb[0])->at[0].name == tex[0]);
    snx_gl_framebuffer_renderbuffer(ctx, FRAMEBUFFER, COLOR0, 0x1234, rb[0]);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
    snx_gl_framebuffer_renderbuffer(ctx, FRAMEBUFFER, 0x1234, RENDERBUFFER, rb[0]);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_ENUM);
    /* a cube map's face: its layer */
    snx_gl_bind_texture(ctx, CUBE, tex[1]);
    snx_gl_tex_image(ctx, CUBE_X + 4, 0, RGBA8, 16, 16, 1, RGBA, UBYTE, 0, 0, 0);
    snx_gl_framebuffer_texture(ctx, FRAMEBUFFER, COLOR0 + 2, CUBE_X + 4, tex[1], 0, 0);
    assert(snx_gl_framebuffer(fb[0])->at[2].layer == 4);
    assert(attachment_parameter(ctx, FRAMEBUFFER, COLOR0 + 2, 0x8cd3) == CUBE_X + 4);
    snx_gl_framebuffer_texture(ctx, FRAMEBUFFER, COLOR0 + 2, 0, 0, 0, 0);

    /* draw and read buffers, and glGet of them */
    {
        const uint32_t two[2] = {0, COLOR0 + 1};
        unsigned before = heard.changes;
        snx_gl_draw_buffers(ctx, 2, two);
        assert(snx_gl_framebuffer(fb[0])->draw[1] == COLOR0 + 1 && snx_gl_framebuffer(fb[0])->draw[0] == 0);
        assert(heard.changes == before + 1 && snx_gl_get(ctx, 0x8826, v) == 1 && v[0] == COLOR0 + 1);
        snx_gl_draw_buffers(ctx, SNX_GL_COLORS + 1, two);
        assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_VALUE);
        snx_gl_read_buffer(ctx, COLOR0 + 1);
        assert(snx_gl_get(ctx, 0x0c02, v) == 1 && v[0] == COLOR0 + 1);
    }
    /* the window's framebuffer: its parameters from the platform, GL_BACK its buffers */
    snx_gl_bind_framebuffer(ctx, FRAMEBUFFER, 0);
    assert(attachment_parameter(ctx, FRAMEBUFFER, 0x0405, 0x8cd0) == 0x8218); /* GL_FRAMEBUFFER_DEFAULT */
    assert(attachment_parameter(ctx, FRAMEBUFFER, DEPTH, 0x8216) == 24);
    assert(attachment_parameter(ctx, FRAMEBUFFER, STENCIL, 0x8217) == 8);
    assert(snx_gl_check_framebuffer_status(ctx, FRAMEBUFFER) == COMPLETE);
    assert(snx_gl_get(ctx, 0x8825, v) == 1 && v[0] == 0x0405 && snx_gl_get(ctx, 0x0c02, v) == 1 && v[0] == 0x0405);
    snx_gl_framebuffer_texture(ctx, FRAMEBUFFER, COLOR0, T2D, tex[0], 0, 0);
    assert(snx_gl_get_error(ctx) == SNX_GL_INVALID_OPERATION); /* nothing attaches to the window's */
    snx_gl_bind_framebuffer(ctx, FRAMEBUFFER, fb[0]);

    /* a save state's copy: loaded back whole, the platform told every renderbuffer's image is of the old */
    size_t total = snx_gl_state_bytes();
    unsigned char *saved = malloc(total), *copy = malloc(total), *after = malloc(total);
    char why[300];
    assert(saved && copy && after);
    snx_gl_state_save(saved);
    assert(!snx_gl_state_check(saved, total, why, sizeof why));
    {
        unsigned went = heard.gone, changes = heard.changes;
        snx_gl_delete_framebuffers(ctx, 1, fb);
        assert(!snx_gl_is_framebuffer(fb[0]) && snx_gl_framebuffers(ctx)->draw == 0 &&
               snx_gl_framebuffers(ctx)->read == 0 && heard.changes == changes + 1);
        snx_gl_delete_renderbuffers(ctx, 1, rb);
        assert(heard.gone == went + 1 && !snx_gl_is_renderbuffer(rb[0]));
        assert(!snx_gl_state_load(saved, total));
        assert(heard.gone >= went + 1 + SNX_GL_RENDERBUFFERS);
        snx_gl_state_save(after);
        assert(!memcmp(saved, after, total));
        assert(snx_gl_is_framebuffer(fb[0]) && snx_gl_framebuffers(ctx)->draw == fb[0] &&
               snx_gl_is_renderbuffer(rb[0]));
        assert(snx_gl_check_framebuffer_status(ctx, FRAMEBUFFER) == COMPLETE);
    }
    /* deleting the bound renderbuffer unbinds it */
    snx_gl_bind_renderbuffer(ctx, RENDERBUFFER, rb[2]);
    snx_gl_delete_renderbuffers(ctx, 1, &rb[2]);
    assert(snx_gl_framebuffers(ctx)->renderbuffer == 0);
    /* refused copies, each naming what it found */
    {
        size_t renderbuffers = total - snx_gl_sync_state_bytes() - snx_gl_framebuffer_state_bytes() + 8;
        size_t framebuffers = renderbuffers + SNX_GL_RENDERBUFFERS * sizeof(SnxGlRenderbuffer);
        size_t contexts = framebuffers + SNX_GL_FRAMEBUFFERS * sizeof(SnxGlFramebuffer);
        SnxGlRenderbuffer r;
        SnxGlFramebuffer f;
        SnxGlFramebufferContext c;
        memcpy(copy, saved, total);
        memcpy(&r, copy + renderbuffers, sizeof r); /* renderbuffer 1 */
        r.format = SNX_GL_FORMATS;
        memcpy(copy + renderbuffers, &r, sizeof r);
        refused(copy, total, "renderbuffer 1 has format 137 of 16x8");
        memcpy(copy, saved, total);
        memcpy(&f, copy + framebuffers, sizeof f); /* framebuffer 1 */
        f.at[SNX_GL_DEPTH_ATTACHMENT].kind = 3;
        memcpy(copy + framebuffers, &f, sizeof f);
        refused(copy, total, "framebuffer 1 has attachment 4 of kind 3");
        memcpy(copy, saved, total);
        memcpy(&c, copy + contexts, sizeof c); /* context 1 */
        c.read = SNX_GL_FRAMEBUFFERS + 1;
        memcpy(copy + contexts, &c, sizeof c);
        refused(copy, total, "context 1 binds framebuffers 1 and 1025");
        refused(saved, total - 8, "this build reads");
    }
    /* a platform that lays renderbuffers out itself: each storage's bytes taken from its memory at its alignment,
       given back when the storage is made anew or the renderbuffer deleted; a save state's copy whose storage is
       outside that memory refused; 0 bytes, no storage and no error */
    {
        uint32_t r2;
        uint64_t first;
        const SnxGlRenderbuffer* x;
        snx_gl_object_host(&(SnxGlObjectHost){.chunk = chunk, .bytes = bytes});
        snx_gl_framebuffer_host(&(SnxGlFramebufferHost){.renderbuffer_storage = storage,
                                                        .renderbuffer_gone = gone,
                                                        .changed = changed,
                                                        .window_bits = window_bits,
                                                        .max_size = max_size,
                                                        .storage_bytes = layout_bytes});
        heard.layout_bytes = 8192, heard.layout_align = 4096;
        snx_gl_gen_renderbuffers(ctx, 1, &r2);
        snx_gl_bind_renderbuffer(ctx, RENDERBUFFER, r2);
        snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 0, RGBA8, 16, 16);
        x = snx_gl_renderbuffer(r2);
        assert(!snx_gl_get_error(ctx) && heard.laid_out == r2 && x->format >= 0);
        assert(x->storage && !(x->storage % 4096) && x->storage_size == 8192);
        first = x->storage;
        heard.layout_bytes = 4096;
        snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 0, RGBA8, 8, 8);
        assert(x->storage == first && x->storage_size == 4096); /* the first storage's bytes, given back, taken */
        size_t total2 = snx_gl_state_bytes();
        unsigned char* kept = malloc(total2);
        assert(kept);
        snx_gl_state_save(kept);
        assert(!snx_gl_state_check(kept, total2, why, sizeof why));
        {
            size_t at = total2 - snx_gl_sync_state_bytes() - snx_gl_framebuffer_state_bytes() + 8 +
                        (r2 - 1) * sizeof(SnxGlRenderbuffer);
            SnxGlRenderbuffer r;
            memcpy(&r, kept + at, sizeof r);
            r.storage = 16; /* no chunk of the memory starts there */
            memcpy(kept + at, &r, sizeof r);
            snprintf(why, sizeof why, "renderbuffer %u keeps its storage at 10+1000", (unsigned)r2);
            refused(kept, total2, why);
        }
        free(kept);
        snx_gl_delete_renderbuffers(ctx, 1, &r2);
        assert(snx_gl_memory_take(4096, 4096) == first); /* given back */
        snx_gl_memory_give(first, 4096);
        heard.layout_bytes = 0;
        snx_gl_gen_renderbuffers(ctx, 1, &r2);
        snx_gl_bind_renderbuffer(ctx, RENDERBUFFER, r2);
        snx_gl_renderbuffer_storage(ctx, RENDERBUFFER, 0, RGBA8, 16, 16);
        assert(!snx_gl_get_error(ctx) && !snx_gl_renderbuffer(r2)->storage && snx_gl_renderbuffer(r2)->format >= 0);
    }
    free(saved);
    free(copy);
    free(after);
    puts("gl_framebuffer: ok");
    return 0;
}
