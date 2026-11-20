/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shared GL layer's state machine (src/common/gpu/gl/gl_state.c), with no GPU: contexts made up to
   SNX_GL_CONTEXTS and freed for reuse, GL's initial fixed state, each call's effect on its own context alone, the
   errors GL gives (the first kept until glGetError reads it) with the state left as it was, the clamps GL makes, and
   a save state's copy: saved, changed, loaded back to the same bytes, and refused when cut, of another layout or
   holding a value GL never leaves, with the reason naming it; glGet*'s values, the strings of each API and version,
   and glClear's buffers */
#include "common/gpu/gl/gl_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    GL_FRONT = 0x0404,
    GL_BACK = 0x0405,
    GL_FRONT_AND_BACK = 0x0408,
    GL_CCW = 0x0901,
    GL_CW = 0x0900,
    GL_LESS = 0x0201,
    GL_GREATER = 0x0204,
    GL_ALWAYS = 0x0207,
    GL_KEEP = 0x1e00,
    GL_REPLACE = 0x1e01,
    GL_FUNC_ADD = 0x8006,
    GL_FUNC_SUBTRACT = 0x800a,
    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_BLEND = 0x0be2,
    GL_DEPTH_TEST = 0x0b71,
    GL_SCISSOR_TEST = 0x0c11,
    GL_SAMPLE_MASK = 0x8e51,
    GL_UNPACK_ALIGNMENT = 0x0cf5,
    GL_PACK_ALIGNMENT = 0x0d05,
    GL_UNPACK_ROW_LENGTH = 0x0cf2,
    GL_UNPACK_IMAGE_HEIGHT = 0x806e,
    GL_DONT_CARE = 0x1100,
    GL_NICEST = 0x1102,
};

static void defaults(const SnxGlFixed* f) {
    assert(!f->error && !f->blend && !f->depth_test && f->dither);
    assert(f->blend_src[0] == 1 && f->blend_src[1] == 1 && !f->blend_dst[0] && !f->blend_dst[1]);
    assert(f->blend_eq[0] == GL_FUNC_ADD && f->blend_eq[1] == GL_FUNC_ADD);
    assert(f->color_mask[0] && f->color_mask[1] && f->color_mask[2] && f->color_mask[3] && f->depth_mask);
    assert(f->depth_func == GL_LESS && f->depth_range[0] == 0 && f->depth_range[1] == 1);
    assert(f->cull_face == GL_BACK && f->front_face == GL_CCW);
    for (int s = 0; s < 2; s++)
        assert(f->stencil[s].func == GL_ALWAYS && f->stencil[s].fail == GL_KEEP && f->stencil[s].zpass == GL_KEEP &&
               f->stencil[s].mask == 0xffffffffu && f->stencil[s].write == 0xffffffffu && !f->stencil[s].ref);
    assert(f->clear_depth == 1 && !f->clear_stencil && f->sample_mask == 0xffffffffu && f->line_width == 1);
    assert(f->coverage_value == 1 && f->hints[0] == GL_DONT_CARE && f->hints[1] == GL_DONT_CARE);
    assert(f->pixel[0].alignment == 4 && f->pixel[1].alignment == 4);
}

/* check refuses the copy, its reason holding text */
static void refused(const void* copy, size_t size, const char* text) {
    char why[256];
    assert(snx_gl_state_check(copy, size, why, sizeof why) < 0);
    if (!strstr(why, text)) {
        fprintf(stderr, "gl_state: refused for \"%s\", not naming \"%s\"\n", why, text);
        abort();
    }
}

int main(void) {
    uint32_t a, b, all[SNX_GL_CONTEXTS];
    size_t bytes = snx_gl_state_bytes();
    unsigned char *saved = malloc(bytes), *copy = malloc(bytes), *after = malloc(bytes);
    char why[256];
    assert(saved && copy && after);

    /* contexts: every one, then none, then a freed one again */
    for (uint32_t i = 0; i < SNX_GL_CONTEXTS; i++) {
        all[i] = snx_gl_context_new(SNX_GL_API_ES, 31, 0);
        assert(all[i] == i + 1);
        defaults(snx_gl_fixed(all[i]));
    }
    assert(!snx_gl_context_new(SNX_GL_API_ES, 31, 0));
    for (uint32_t i = 2; i < SNX_GL_CONTEXTS; i++) snx_gl_context_free(all[i]);
    a = all[0], b = all[1];

    /* each call sets its own context only */
    snx_gl_enable(a, GL_BLEND, 1);
    snx_gl_enable(a, GL_SAMPLE_MASK, 1);
    snx_gl_blend_func_separate(a, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, 1, 0);
    snx_gl_blend_equation_separate(a, GL_FUNC_SUBTRACT, GL_FUNC_ADD);
    snx_gl_color_mask(a, 1, 0, 1, 0);
    snx_gl_depth_mask(a, 0);
    snx_gl_depth_func(a, GL_GREATER);
    snx_gl_cull_face(a, GL_FRONT);
    snx_gl_front_face(a, GL_CW);
    snx_gl_clear_color(a, 0.25f, 0.5f, 0.75f, 1);
    snx_gl_clear_stencil(a, 7);
    snx_gl_viewport(a, 1, 2, 640, 360);
    snx_gl_scissor(a, 3, 4, 320, 180);
    snx_gl_polygon_offset(a, 1.5f, 2);
    snx_gl_hint(a, 0x8192, GL_NICEST);
    snx_gl_pixel_store(a, GL_UNPACK_ALIGNMENT, 1);
    snx_gl_pixel_store(a, GL_UNPACK_ROW_LENGTH, 64);
    snx_gl_pixel_store(a, GL_UNPACK_IMAGE_HEIGHT, 16);
    {
        const SnxGlFixed* f = snx_gl_fixed(a);
        assert(snx_gl_is_enabled(a, GL_BLEND) && snx_gl_is_enabled(a, GL_SAMPLE_MASK));
        assert(!snx_gl_is_enabled(a, GL_DEPTH_TEST));
        assert(snx_gl_capability(a, GL_BLEND) == 1 && snx_gl_capability(a, GL_DEPTH_TEST) == 0);
        assert(snx_gl_capability(a, 0x1234) == -1 && !snx_gl_fixed(a)->error); /* glGet*'s look records nothing */
        assert(f->blend_src[0] == GL_SRC_ALPHA && f->blend_dst[0] == GL_ONE_MINUS_SRC_ALPHA);
        assert(f->blend_src[1] == 1 && f->blend_dst[1] == 0);
        assert(f->blend_eq[0] == GL_FUNC_SUBTRACT && f->blend_eq[1] == GL_FUNC_ADD);
        assert(f->color_mask[0] && !f->color_mask[1] && f->color_mask[2] && !f->color_mask[3] && !f->depth_mask);
        assert(f->depth_func == GL_GREATER && f->cull_face == GL_FRONT && f->front_face == GL_CW);
        assert(f->clear_color[0] == 0.25f && f->clear_color[2] == 0.75f && f->clear_stencil == 7);
        assert(f->viewport[0] == 1 && f->viewport[1] == 2 && f->viewport[2] == 640 && f->viewport[3] == 360);
        assert(f->scissor[0] == 3 && f->scissor[3] == 180);
        assert(f->offset_factor == 1.5f && f->offset_units == 2 && f->hints[0] == GL_NICEST);
        assert(f->pixel[1].alignment == 1 && f->pixel[1].row_length == 64 && f->pixel[1].image_height == 16);
        assert(f->pixel[0].alignment == 4 && !f->error);
    }
    defaults(snx_gl_fixed(b));

    /* stencil by face: the front's, the back's, or both */
    snx_gl_stencil_func_separate(a, GL_FRONT, GL_LESS, 3, 0xf0);
    snx_gl_stencil_op_separate(a, GL_BACK, GL_REPLACE, GL_KEEP, GL_REPLACE);
    snx_gl_stencil_mask_separate(a, GL_FRONT_AND_BACK, 0x0f);
    {
        const SnxGlFixed* f = snx_gl_fixed(a);
        assert(f->stencil[0].func == GL_LESS && f->stencil[0].ref == 3 && f->stencil[0].mask == 0xf0);
        assert(f->stencil[1].func == GL_ALWAYS && f->stencil[1].ref == 0);
        assert(f->stencil[1].fail == GL_REPLACE && f->stencil[1].zpass == GL_REPLACE && f->stencil[0].fail == GL_KEEP);
        assert(f->stencil[0].write == 0x0f && f->stencil[1].write == 0x0f);
    }

    /* GL's clamps */
    snx_gl_depth_range(a, -1, 2);
    snx_gl_clear_depth(a, 3);
    snx_gl_sample_coverage(a, 1.5f, 1);
    assert(snx_gl_fixed(a)->depth_range[0] == 0 && snx_gl_fixed(a)->depth_range[1] == 1);
    assert(snx_gl_fixed(a)->clear_depth == 1 && snx_gl_fixed(a)->coverage_value == 1);
    assert(snx_gl_fixed(a)->coverage_invert);

    /* errors: the state stays, the first error waits for glGetError, which clears it */
    snx_gl_viewport(a, 0, 0, -1, 10);
    snx_gl_enable(a, 0x1234, 1);
    assert(snx_gl_fixed(a)->viewport[2] == 640);
    assert(snx_gl_get_error(a) == SNX_GL_INVALID_VALUE && snx_gl_get_error(a) == SNX_GL_NO_ERROR);
    assert(!snx_gl_is_enabled(a, 0x1234) && snx_gl_get_error(a) == SNX_GL_INVALID_ENUM);
    snx_gl_scissor(a, 0, 0, 5, -2);
    assert(snx_gl_fixed(a)->scissor[2] == 320 && snx_gl_get_error(a) == SNX_GL_INVALID_VALUE);
    snx_gl_line_width(a, 0);
    assert(snx_gl_fixed(a)->line_width == 1 && snx_gl_get_error(a) == SNX_GL_INVALID_VALUE);
    snx_gl_point_size(a, -1);
    assert(snx_gl_fixed(a)->point_size == 1 && snx_gl_get_error(a) == SNX_GL_INVALID_VALUE);
    snx_gl_polygon_mode(a, 0x0404, 0x1b01); /* GL_FRONT: a core profile has both faces alone */
    assert(snx_gl_fixed(a)->polygon_mode == 0x1b02 && snx_gl_get_error(a) == SNX_GL_INVALID_ENUM);
    snx_gl_polygon_mode(a, 0x0408, 0x1b03);
    assert(snx_gl_fixed(a)->polygon_mode == 0x1b02 && snx_gl_get_error(a) == SNX_GL_INVALID_ENUM);
    snx_gl_pixel_store(a, GL_PACK_ALIGNMENT, 3);
    assert(snx_gl_fixed(a)->pixel[0].alignment == 4 && snx_gl_get_error(a) == SNX_GL_INVALID_VALUE);
    snx_gl_pixel_store(a, 0x1234, 1);
    assert(snx_gl_get_error(a) == SNX_GL_INVALID_ENUM);
    snx_gl_stencil_func_separate(a, 0x1234, GL_GREATER, 9, 1);
    assert(snx_gl_fixed(a)->stencil[0].func == GL_LESS && snx_gl_fixed(a)->stencil[1].func == GL_ALWAYS);
    assert(snx_gl_get_error(a) == SNX_GL_INVALID_ENUM);
    assert(!snx_gl_fixed(b)->error);

    /* a save state's copy: saved, changed, loaded back */
    snx_gl_error(a, SNX_GL_INVALID_OPERATION);
    snx_gl_state_save(saved);
    assert(!snx_gl_state_check(saved, bytes, why, sizeof why));
    snx_gl_context_free(b);
    snx_gl_viewport(a, 9, 9, 9, 9);
    assert(!snx_gl_state_load(saved, bytes));
    snx_gl_state_save(after);
    assert(!memcmp(saved, after, bytes));
    assert(snx_gl_fixed(a)->viewport[0] == 1 && snx_gl_fixed(b)->depth_func == GL_LESS);
    assert(snx_gl_get_error(a) == SNX_GL_INVALID_OPERATION);
    assert(snx_gl_context_new(SNX_GL_API_DESKTOP, 45, 1) == 3); /* b is made again by the load */

    /* refused copies, each naming what it found */
    refused(saved, 8, "8 bytes");
    memcpy(copy, saved, bytes);
    copy[0] ^= 1;
    refused(copy, bytes, "not 4c47584e");
    memcpy(copy, saved, bytes);
    copy[4] = 10; /* the layout */
    refused(copy, bytes, "layout 10");
    refused(saved, 20, "this build reads layout 9");
    refused(saved, bytes - 4, "this build reads");
    {
        SnxGlContext c;
        size_t at = 16; /* the first context, after the header */
        memcpy(copy, saved, bytes);
        memcpy(&c, copy + at, sizeof c);
        c.used = 2;
        memcpy(copy + at, &c, sizeof c);
        refused(copy, bytes, "context 1 has used=2");
        c.used = 1;
        c.fixed.error = 0x1234;
        memcpy(copy + at, &c, sizeof c);
        refused(copy, bytes, "context 1 holds error 1234");
        c.fixed.error = 0;
        c.fixed.pixel[1].alignment = 3;
        memcpy(copy + at, &c, sizeof c);
        refused(copy, bytes, "unpack alignment 3");
    }
    /* glGet*, glGetString and glClear, of the desktop 4.5 debug context made after the load and of a */
    {
        const uint32_t d = 3;
        double v[4];
        assert(snx_gl_get(d, 0x821b, v) == 1 && v[0] == 4 && snx_gl_get(d, 0x821c, v) == 1 && v[0] == 5);
        assert(snx_gl_get(d, 0x821e, v) == 1 && v[0] == 2 && snx_gl_get(d, 0x9126, v) == 1 && v[0] == 1);
        snx_gl_viewport(d, 5, 6, 7, 8);
        assert(snx_gl_get(d, 0x0ba2, v) == 4 && v[0] == 5 && v[1] == 6 && v[2] == 7 && v[3] == 8);
        assert(snx_gl_get(d, GL_BLEND, v) == 1 && v[0] == 0 && snx_gl_get(a, GL_BLEND, v) == 1 && v[0] == 1);
        assert(snx_gl_get(d, 0x1234, v) == 0 && !snx_gl_fixed(d)->error); /* its caller records the error */
        /* the limits two GL games ask for, and the point size and polygon mode they set */
        assert(snx_gl_get(d, 0x851c, v) == 1 && v[0] == 16384 && snx_gl_get(d, 0x8073, v) == 1 && v[0] == 2048);
        assert(snx_gl_get(d, 0x88ff, v) == 1 && v[0] == 2048 && snx_gl_get(d, 0x8d57, v) == 1 && v[0] == 4);
        assert(snx_gl_get(d, 0x80a8, v) == 1 && v[0] == 0 && snx_gl_get(d, 0x80a9, v) == 1 && v[0] == 0);
        assert(snx_gl_get(d, 0x0b12, v) == 2 && v[0] == 1 && v[1] == 64 && snx_gl_get(d, 0x84fd, v) == 1 && v[0] == 15);
        assert(snx_gl_get(d, 0x91b0, v) == 1 && v[0] == 0xffffffffu);
        snx_gl_point_size(d, 3.5f);
        snx_gl_polygon_mode(d, 0x0408, 0x1b01); /* GL_FRONT_AND_BACK, GL_LINE */
        assert(snx_gl_get(d, 0x0b11, v) == 1 && v[0] == 3.5 && snx_gl_get(d, 0x0b40, v) == 2 && v[1] == 0x1b01);
        assert(!snx_gl_fixed(d)->error);
        assert(!strcmp(snx_gl_string(d, 0x1f02, 0, 0), "4.5.0 ScarletNX"));
        assert(!strcmp(snx_gl_string(d, 0x8b8c, 0, 0), "4.50 ScarletNX"));
        assert(!strcmp(snx_gl_string(d, 0x1f00, 0, 0), "NVIDIA Corporation"));
        assert(!snx_gl_string(d, 0x1f03, 0, 0) && snx_gl_get_error(d) == SNX_GL_INVALID_ENUM); /* core: no list */
        assert(!snx_gl_string(d, 0x1f03, 1, 0) && snx_gl_get_error(d) == SNX_GL_INVALID_VALUE);
        assert(!strcmp(snx_gl_string(a, 0x1f02, 0, 0), "OpenGL ES 3.1 ScarletNX") && !snx_gl_get(a, 0x9126, v));
        assert(snx_gl_clear(d, 0x4000 | 0x100) == (0x4000 | 0x100) && !snx_gl_fixed(d)->error);
        assert(!snx_gl_clear(d, 0x4000 | 1) && snx_gl_get_error(d) == SNX_GL_INVALID_VALUE);
        assert(snx_gl_version_made(SNX_GL_API_DESKTOP, 33) && !snx_gl_version_made(SNX_GL_API_DESKTOP, 46));
        assert(snx_gl_version_made(SNX_GL_API_ES, 20) && !snx_gl_version_made(SNX_GL_API_ES, 21));
        assert(snx_gl_context_api(d) == SNX_GL_API_DESKTOP && snx_gl_context_version(d) == 45);
        assert(!snx_gl_context_version(SNX_GL_CONTEXTS));
    }
    free(saved);
    free(copy);
    free(after);
    puts("gl_state: ok");
    return 0;
}
