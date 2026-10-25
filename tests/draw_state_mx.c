/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's registers as a draw's dynamic state (gpu/maxwell/draw_state.c). The expected values are what
   render_draw computed from the same registers before it read them through the struct */
#include "nx/gpu/maxwell/draw_state.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                                               \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define REGS 0xE00
static void setf(u32* r, u32 reg, float f) { memcpy(&r[reg], &f, 4); }

/* front: 0x4E1 fail, 0x4E2 depth fail, 0x4E3 pass, 0x4E4 function, 0x4E5 reference, 0x4E6 function mask, 0x4E7
   write mask; back: 0x566-0x569 the same, 0x3D5 reference, 0x3D6 write mask, 0x3D7 function mask */
static void stencil(void) {
    u32 r[REGS] = {0};
    DsDynamic d;
    DsViewports v;
    r[0x4E0] = 1;
    r[0x4E1] = 0x1E01; /* GL_REPLACE */
    r[0x4E2] = 0x150A; /* GL_INVERT */
    r[0x4E3] = 7;      /* increment and wrap */
    r[0x4E4] = 0x203;  /* GL_LEQUAL */
    r[0x4E5] = 0x11;
    r[0x4E6] = 0x22;
    r[0x4E7] = 0x33;
    r[0x566] = 0x1E00; /* GL_KEEP */
    r[0x567] = 0x8508; /* GL_DECR_WRAP */
    r[0x568] = 0;      /* unknown: zero */
    r[0x569] = 6;      /* not equal */
    r[0x3D5] = 0x44;
    r[0x3D6] = 0x55;
    r[0x3D7] = 0x66;
    mx_draw_dynamic(r, DS_POLYGON_FILL, 1, &d, &v);
    /* one-sided: the back face is the front's */
    CHECK(d.stencil_test == 1);
    CHECK(d.face[0].fail == DS_STENCIL_REPLACE && d.face[0].zfail == DS_STENCIL_INVERT &&
          d.face[0].pass == DS_STENCIL_INCREMENT_AND_WRAP && d.face[0].cmp == DS_CMP_LESS_OR_EQUAL);
    CHECK(d.face[0].ref == 0x11 && d.face[0].cmask == 0x22 && d.face[0].wmask == 0x33);
    CHECK(!memcmp(&d.face[1], &d.face[0], sizeof d.face[0]));
    r[0x565] = 1; /* two-sided */
    mx_draw_dynamic(r, DS_POLYGON_FILL, 1, &d, &v);
    CHECK(d.face[0].fail == DS_STENCIL_REPLACE && d.face[0].ref == 0x11);
    CHECK(d.face[1].fail == DS_STENCIL_KEEP && d.face[1].zfail == DS_STENCIL_DECREMENT_AND_WRAP &&
          d.face[1].pass == DS_STENCIL_ZERO && d.face[1].cmp == DS_CMP_NOT_EQUAL);
    CHECK(d.face[1].ref == 0x44 && d.face[1].cmask == 0x66 && d.face[1].wmask == 0x55);
    r[0x4E0] = 0;
    r[0x4E4] = 9; /* out of both ranges: always */
    mx_draw_dynamic(r, DS_POLYGON_FILL, 1, &d, &v);
    CHECK(d.stencil_test == 0 && d.face[0].cmp == DS_CMP_ALWAYS);
}

/* 0x4EB bit 0 lower left, bit 4 facing flip; 0x3FE's top half the clip height; 0x286 the swizzle; 0x581 bit 2 the
   point sprite's origin; 0x647 0x900 clockwise; 0x646 cull on, 0x648 0x404 front, 0x405 back, 0x408 both */
static void origin(void) {
    u32 r[REGS] = {0};
    DsDynamic d;
    DsViewports v;
    r[0x4EB] = 0x11 | 0x100; /* a bit the state has no use for */
    r[0x3FE] = (100u << 16) | 7;
    r[0x286] = 0x6430; /* NegativeY */
    r[0x581] = 4;
    r[0x647] = 0x900;
    r[0x35F] = 1;
    mx_draw_dynamic(r, DS_POLYGON_FILL, 1, &d, &v);
    CHECK(d.window_origin == (DS_ORIGIN_LOWER_LEFT | DS_ORIGIN_FLIP_Y));
    CHECK(d.clip_height == 100);
    CHECK(v.count == 1 && v.swizzle[0] == 0x6430 && ((v.swizzle[0] >> 4) & 7) == 3);
    CHECK(d.sprite_origin == 1 && d.front_cw == 1 && d.depth_mode == 1);
    CHECK(d.cull == DS_CULL_NONE);
    r[0x646] = 1;
    r[0x648] = 0x404;
    r[0x647] = 0x901;
    r[0x35F] = 0;
    r[0x4EB] = 0;
    r[0x581] = 0;
    mx_draw_dynamic(r, DS_POLYGON_FILL, 1, &d, &v);
    CHECK(d.cull == DS_CULL_FRONT && d.front_cw == 0 && d.window_origin == 0 && d.sprite_origin == 0 &&
          d.depth_mode == 0);
    r[0x648] = 0x408;
    mx_draw_dynamic(r, DS_POLYGON_FILL, 1, &d, &v);
    CHECK(d.cull == DS_CULL_FRONT_AND_BACK);
    r[0x648] = 0x405;
    mx_draw_dynamic(r, DS_POLYGON_FILL, 1, &d, &v);
    CHECK(d.cull == DS_CULL_BACK);
}

/* 0x55C on: the smooth width 0x4EC, off: the aliased 0x4ED, as given (the device's clamp is the renderer's) */
static void line_width(void) {
    u32 r[REGS] = {0};
    DsDynamic d;
    DsViewports v;
    setf(r, 0x4EC, 2.5f);
    setf(r, 0x4ED, 100.0f);
    mx_draw_dynamic(r, DS_POLYGON_LINE, 1, &d, &v);
    CHECK(d.line_width == 100.0f);
    r[0x55C] = 1;
    mx_draw_dynamic(r, DS_POLYGON_LINE, 1, &d, &v);
    CHECK(d.line_width == 2.5f);
}

/* the bias enable for the polygon mode drawn: 0x370 points, 0x371 lines, 0x372 filled; units 0x56F, slope 0x55B,
   clamp 0x61F */
static void bias(void) {
    u32 r[REGS] = {0};
    DsDynamic d;
    DsViewports v;
    r[0x370] = 1;
    r[0x371] = 0;
    r[0x372] = 2;
    setf(r, 0x56F, 3.0f);
    setf(r, 0x55B, -1.5f);
    setf(r, 0x61F, 0.25f);
    mx_draw_dynamic(r, DS_POLYGON_POINT, 1, &d, &v);
    CHECK(d.bias_on == 1);
    mx_draw_dynamic(r, DS_POLYGON_LINE, 1, &d, &v);
    CHECK(d.bias_on == 0);
    mx_draw_dynamic(r, DS_POLYGON_FILL, 1, &d, &v);
    CHECK(d.bias_on == 1);
    CHECK(d.bias_units == 3.0f && d.bias_slope == -1.5f && d.bias_clamp == 0.25f);
    r[0x370] = 0;
    r[0x371] = 5;
    r[0x372] = 0;
    mx_draw_dynamic(r, DS_POLYGON_POINT, 1, &d, &v);
    CHECK(d.bias_on == 0);
    mx_draw_dynamic(r, DS_POLYGON_LINE, 1, &d, &v);
    CHECK(d.bias_on == 1);
    mx_draw_dynamic(r, DS_POLYGON_FILL, 1, &d, &v);
    CHECK(d.bias_on == 0);
}

/* depth 0x4B3 test, 0x4BA write, 0x4C3 function; alpha 0x4BB bit 0, 0x4C5 function (the compare's decoding), 0x4C4
   reference; 0x4C7-0x4CA blend constants; 0xDF rasterize enable; 0x574 the samples; 0x50E the pushed word */
static void others(void) {
    u32 r[REGS] = {0};
    DsDynamic d;
    DsViewports v;
    r[0x4B3] = 3;
    r[0x4BA] = 0;
    r[0x4C3] = 2; /* less */
    r[0x4BB] = 3;
    r[0x4C5] = 0x206; /* GL_GEQUAL */
    setf(r, 0x4C4, 0.5f);
    setf(r, 0x4C7, 0.1f);
    setf(r, 0x4C8, 0.2f);
    setf(r, 0x4C9, 0.3f);
    setf(r, 0x4CA, 0.4f);
    r[0x574] = 3;
    r[0x50E] = 0x3F800000u;
    mx_draw_dynamic(r, DS_POLYGON_FILL, 1, &d, &v);
    CHECK(d.depth_test == 1 && d.depth_write == 0 && d.depth_cmp == DS_CMP_LESS);
    CHECK(d.alpha_test == 1 && d.alpha_func == DS_CMP_GREATER_OR_EQUAL && d.alpha_ref == 0.5f);
    CHECK(d.blend_const[0] == 0.1f && d.blend_const[1] == 0.2f && d.blend_const[2] == 0.3f && d.blend_const[3] == 0.4f);
    CHECK(d.discard == 1 && d.msaa_mode == 3 && d.depth_word == 0x3F800000u);
    r[0xDF] = 1;
    r[0x4BB] = 2;
    r[0x4C5] = 0x300; /* unknown: always */
    mx_draw_dynamic(r, DS_POLYGON_FILL, 1, &d, &v);
    CHECK(d.discard == 0 && d.alpha_test == 0 && d.alpha_func == DS_CMP_ALWAYS);
}

/* viewport j: 0x280 + 8 j scale x, y, z and offset x, y, z, 0x286 + 8 j swizzle; 0x300 + 4 j clip horizontal,
   vertical, min z, max z; 0x380 + 4 j scissor enable (bit 0), horizontal, vertical */
static void viewports(void) {
    u32 r[REGS] = {0};
    DsDynamic d;
    DsViewports v;
    for (u32 j = 0; j < 16; j++) {
        for (u32 c = 0; c < 6; c++) setf(r, 0x280 + j * 8 + c, (float)(j * 10 + c));
        r[0x286 + j * 8] = 0x6420 + j;
        r[0x300 + j * 4] = (j << 16) | (j + 1);
        r[0x301 + j * 4] = (j << 16) | (j + 2);
        setf(r, 0x302 + j * 4, (float)j / 32);
        setf(r, 0x303 + j * 4, 1 - (float)j / 32);
        r[0x380 + j * 4] = j % 3 == 0 ? 1 : j % 3 == 1 ? 2 : 3; /* bit 0 only */
        r[0x381 + j * 4] = 0x1000u * j + 1;
        r[0x382 + j * 4] = 0x2000u * j + 2;
    }
    r[0x64B] = 1;
    mx_draw_dynamic(r, DS_POLYGON_FILL, 16, &d, &v);
    CHECK(v.count == 16 && v.transform_on == 1);
    u32 scissors = 0;
    for (u32 j = 0; j < 16; j++) {
        for (u32 c = 0; c < 3; c++) {
            CHECK(v.scale[j][c] == (float)(j * 10 + c));
            CHECK(v.translate[j][c] == (float)(j * 10 + 3 + c));
        }
        CHECK(v.swizzle[j] == 0x6420 + j);
        CHECK(v.clip_x[j] == ((j << 16) | (j + 1)) && v.clip_y[j] == ((j << 16) | (j + 2)));
        CHECK(v.z_near[j] == (float)j / 32 && v.z_far[j] == 1 - (float)j / 32);
        CHECK(v.scissor_x[j] == 0x1000u * j + 1 && v.scissor_y[j] == 0x2000u * j + 2);
        if (j % 3 != 1) scissors |= 1u << j;
    }
    CHECK(v.scissor_on == scissors);
    /* one viewport: only the first is read, and the others' scissor bits stay clear */
    r[0x64B] = 0;
    mx_draw_dynamic(r, DS_POLYGON_FILL, 1, &d, &v);
    CHECK(v.count == 1 && v.transform_on == 0 && v.scissor_on == 1);
    CHECK(v.scale[0][1] == 1.0f && v.translate[0][1] == 4.0f && v.swizzle[0] == 0x6420);
}

int main(void) {
    stencil();
    origin();
    line_width();
    bias();
    others();
    viewports();
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    printf("draw_state_mx: all checks passed\n");
    return 0;
}
