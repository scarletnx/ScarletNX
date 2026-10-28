/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's registers as a draw's dynamic state (gpu/maxwell/draw_state.c). The expected values are what
   render_draw computed from the same registers before it read them through the struct */
#include "nx/gpu/maxwell/draw_state.h"
#include "nx/gpu/maxwell/kepler_compute_regs.h"
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
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
    /* one-sided: the back face is the front's */
    CHECK(d.stencil_test == 1);
    CHECK(d.face[0].fail == DS_STENCIL_REPLACE && d.face[0].zfail == DS_STENCIL_INVERT &&
          d.face[0].pass == DS_STENCIL_INCREMENT_AND_WRAP && d.face[0].cmp == DS_CMP_LESS_OR_EQUAL);
    CHECK(d.face[0].ref == 0x11 && d.face[0].cmask == 0x22 && d.face[0].wmask == 0x33);
    CHECK(!memcmp(&d.face[1], &d.face[0], sizeof d.face[0]));
    r[0x565] = 1; /* two-sided */
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
    CHECK(d.face[0].fail == DS_STENCIL_REPLACE && d.face[0].ref == 0x11);
    CHECK(d.face[1].fail == DS_STENCIL_KEEP && d.face[1].zfail == DS_STENCIL_DECREMENT_AND_WRAP &&
          d.face[1].pass == DS_STENCIL_ZERO && d.face[1].cmp == DS_CMP_NOT_EQUAL);
    CHECK(d.face[1].ref == 0x44 && d.face[1].cmask == 0x66 && d.face[1].wmask == 0x55);
    r[0x4E0] = 0;
    r[0x4E4] = 9; /* out of both ranges: always */
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
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
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
    CHECK(d.window_origin == (DS_ORIGIN_LOWER_LEFT | DS_ORIGIN_FLIP_Y));
    CHECK(d.clip_height == 100);
    CHECK(v.swizzle[0] == 0x6430 && ((v.swizzle[0] >> 4) & 7) == 3);
    CHECK(d.sprite_origin == 1 && d.front_cw == 1 && d.depth_mode == 1);
    CHECK(d.cull == DS_CULL_NONE);
    r[0x646] = 1;
    r[0x648] = 0x404;
    r[0x647] = 0x901;
    r[0x35F] = 0;
    r[0x4EB] = 0;
    r[0x581] = 0;
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
    CHECK(d.cull == DS_CULL_FRONT && d.front_cw == 0 && d.window_origin == 0 && d.sprite_origin == 0 &&
          d.depth_mode == 0);
    r[0x648] = 0x408;
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
    CHECK(d.cull == DS_CULL_FRONT_AND_BACK);
    r[0x648] = 0x405;
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
    CHECK(d.cull == DS_CULL_BACK);
}

/* 0x55C on: the smooth width 0x4EC, off: the aliased 0x4ED, as given (the device's clamp is the renderer's) */
static void line_width(void) {
    u32 r[REGS] = {0};
    DsDynamic d;
    DsViewports v;
    setf(r, 0x4EC, 2.5f);
    setf(r, 0x4ED, 100.0f);
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
    CHECK(d.line_width == 100.0f);
    r[0x55C] = 1;
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
    CHECK(d.line_width == 2.5f);
}

/* the bias enables per polygon mode: 0x370 points (bit 0), 0x371 lines (bit 1), 0x372 filled (bit 2); units 0x56F,
   slope 0x55B, clamp 0x61F */
static void bias(void) {
    u32 r[REGS] = {0};
    DsDynamic d;
    r[0x370] = 1;
    r[0x371] = 0;
    r[0x372] = 2;
    setf(r, 0x56F, 3.0f);
    setf(r, 0x55B, -1.5f);
    setf(r, 0x61F, 0.25f);
    mx_draw_dynamic(r, &d);
    CHECK(d.bias_on == 5);
    CHECK(d.bias_units == 3.0f && d.bias_slope == -1.5f && d.bias_clamp == 0.25f);
    r[0x370] = 0;
    r[0x371] = 5;
    r[0x372] = 0;
    mx_draw_dynamic(r, &d);
    CHECK(d.bias_on == 2);
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
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
    CHECK(d.depth_test == 1 && d.depth_write == 0 && d.depth_cmp == DS_CMP_LESS);
    CHECK(d.alpha_test == 1 && d.alpha_func == DS_CMP_GREATER_OR_EQUAL && d.alpha_ref == 0.5f);
    CHECK(d.blend_const[0] == 0.1f && d.blend_const[1] == 0.2f && d.blend_const[2] == 0.3f && d.blend_const[3] == 0.4f);
    CHECK(d.discard == 1 && d.msaa_mode == 3 && d.depth_word == 0x3F800000u);
    r[0xDF] = 1;
    r[0x4BB] = 2;
    r[0x4C5] = 0x300; /* unknown: always */
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
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
    mx_draw_dynamic(r, &d);
    mx_draw_viewports(r, &v);
    CHECK(v.transform_on == 1);
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
    r[0x64B] = 0;
    mx_draw_viewports(r, &v);
    CHECK(v.transform_on == 0 && v.scissor_on == scissors);
}

/* program slot j at 0x800 + 0x10 j: enable bit 0, offset from the base 0x582/0x583 at + 1; 0x544 clip enable,
   0xC8 tessellation, 0x490-0x497 the geometry passthrough mask, 0x084 bit 0 early Z, 0x1D1 transform feedback */
static void programs(void) {
    u32 r[REGS] = {0};
    DsPrograms p;
    r[0x582] = 0x12;
    r[0x583] = 0x34560000;
    for (u32 j = 0; j < 6; j++) r[0x801 + 0x10 * j] = 0x100 * (j + 1);
    r[0x800] = 1;
    r[0x810] = 0; /* VertexB's bit: it runs anyway, as the renderer decides */
    r[0x840] = 3;
    r[0x850] = 2; /* bit 0 only */
    r[0x544] = 0x1F3;
    r[0xC8] = 0x321;
    for (u32 j = 0; j < 8; j++) r[0x490 + j] = 0xA0 + j;
    r[0x084] = 3;
    r[0x1D1] = 5;
    mx_draw_programs(r, &p);
    for (u32 j = 0; j < 6; j++) CHECK(p.code[j] == 0x1234560000ull + 0x100 * (j + 1));
    CHECK(p.on == ((1u << 0) | (1u << 4)));
    CHECK(p.clip_enable == 0xF3 && p.tess_params == 0x321 && p.early_z == 1 && p.xfb_on == 1);
    for (u32 j = 0; j < 8; j++) CHECK(p.gs_skip[j] == 0xA0 + j);
    r[0x084] = 2;
    r[0x1D1] = 0;
    mx_draw_programs(r, &p);
    CHECK(p.early_z == 0 && p.xfb_on == 0);
}

/* attribute 0x458 + i: stream bits 0-4, constant 6, offset 7-20, size 21-26, type 27-29, BGRA 31; stream 0x700 + 4 s:
   stride bits 0-11, enable 12, divisor at + 3; 0x620 + s per instance */
static void vertex_format(void) {
    u32 r[REGS] = {0};
    DsVertexFormat f;
    r[0x458 + 0] = 3u | (0x123u << 7) | (0x0Fu << 21) | (7u << 27); /* float, stream 3 */
    r[0x458 + 1] =
        31u | (1u << 6) | (0x3FFFu << 7) | (0x3Fu << 21) | (3u << 27) | (1u << 31); /* sint, constant, BGRA */
    r[0x458 + 2] = (4u << 27);                                                      /* uint */
    r[0x458 + 31] = (2u << 27);                                                     /* unorm */
    r[0x700 + 3 * 4] = 0x1000 | 0xFFF;
    r[0x703 + 3 * 4] = 7;
    r[0x620 + 3] = 2;
    r[0x700 + 31 * 4] = 0x2010; /* enable is bit 12 only: off */
    r[0x620 + 31] = 1;
    mx_draw_vertex_format(r, &f);
    CHECK(f.stream[0] == 3 && f.offset[0] == 0x123 && f.size[0] == 0x0F && f.type[0] == 7);
    CHECK(f.stream[1] == 31 && f.offset[1] == 0x3FFF && f.size[1] == 0x3F && f.type[1] == 3);
    CHECK(f.constant == 2u && f.bgra == 2u);
    CHECK(f.is_int == (2u | 4u) && f.is_sint == 2u);
    CHECK(f.type[31] == 2 && !(f.is_int >> 31));
    CHECK(f.stride[3] == 0xFFF && f.divisor[3] == 7 && f.stride[31] == 0x10);
    CHECK(f.stream_on == (1u << 3) && f.instanced == ((1u << 3) | (1u << 31)));
}

/* blending: 0x680 + i masks (0x680 for all with 0x3E4), 0x4D8 + i enables, 0x4B9 per-target equations at 0x780 + 8 i
   (+1 op, +2 source, +3 destination, +4..+6 alpha's, 0 the color's), else 0x4D0-0x4D6 for all; 0x671/0x672 the logic
   operation, 0x56E the sRGB write */
static void blend(void) {
    u32 r[REGS] = {0};
    DsBlend b;
    r[0x680 + 0] = 0x1010; /* R and B nibbles' neighbours: G and A */
    r[0x680 + 1] = 0x0F0F;
    r[0x4D8 + 0] = 1;
    r[0x4D8 + 2] = 4;
    r[0x4D0] = 0x800A;          /* GL_FUNC_SUBTRACT */
    r[0x4D1] = 0x4000 | 0x302;  /* GL_SRC_ALPHA */
    r[0x4D2] = 0x4000 | 0x303;  /* GL_ONE_MINUS_SRC_ALPHA */
    r[0x4D3] = 0;               /* alpha takes the color's */
    r[0x4D4] = 2;               /* D3D one */
    r[0x4D6] = 0x4000 | 0x8902; /* the hardware's SRC1_ALPHA */
    mx_draw_blend(r, &b);
    CHECK(b.mask[0] == (DS_COMPONENT_G | DS_COMPONENT_A) && b.mask[1] == (DS_COMPONENT_R | DS_COMPONENT_B));
    CHECK(b.en[0] == 1 && b.en[1] == 0 && b.en[2] == 1);
    for (u32 i = 0; i < 8; i++) /* one set of equations for every target */
        CHECK(b.cop[i] == DS_BLEND_SUBTRACT && b.sc[i] == DS_FACTOR_SRC_ALPHA &&
              b.dc[i] == DS_FACTOR_ONE_MINUS_SRC_ALPHA && b.aop[i] == DS_BLEND_SUBTRACT && b.sa[i] == DS_FACTOR_ONE &&
              b.da[i] == DS_FACTOR_SRC1_ALPHA);
    CHECK(b.logic_op == 0);
    r[0x3E4] = 1; /* target 0's mask for all */
    r[0x4B9] = 1;
    r[0x780 + 8 * 2 + 1] = 5;               /* max */
    r[0x780 + 8 * 2 + 2] = 14;              /* D3D blend factor */
    r[0x780 + 8 * 2 + 3] = 0x4000 | 0x8004; /* GL_ONE_MINUS_CONSTANT_ALPHA */
    r[0x780 + 8 * 2 + 4] = 0x8007;          /* GL_MIN */
    r[0x780 + 8 * 2 + 5] = 0;               /* the color's */
    r[0x780 + 8 * 2 + 6] = 99;              /* unknown: one */
    r[0x671] = 3;
    r[0x672] = 0x1506; /* GL_XOR */
    mx_draw_blend(r, &b);
    CHECK(b.mask[1] == (DS_COMPONENT_G | DS_COMPONENT_A));
    CHECK(b.cop[2] == DS_BLEND_MAX && b.sc[2] == DS_FACTOR_CONSTANT_COLOR &&
          b.dc[2] == DS_FACTOR_ONE_MINUS_CONSTANT_ALPHA && b.aop[2] == DS_BLEND_MIN &&
          b.sa[2] == DS_FACTOR_CONSTANT_COLOR && b.da[2] == DS_FACTOR_ONE);
    CHECK(b.cop[0] == DS_BLEND_ADD && b.sc[0] == DS_FACTOR_ONE && b.dc[0] == DS_FACTOR_ONE); /* 0: no D3D factor, one */
    CHECK(b.logic_op == 7);                                                                  /* VK_LOGIC_OP_XOR + 1 */
}

/* 0x36B polygon mode (0x1B00 points, 0x1B01 lines), 0x5A1 bit 0 the last vertex provokes, 0x64F bits 11-13 the
   geometry clip (1, 3 and 5 clip depth, the rest clamp it), 0x373 patch vertices */
static void raster_key(void) {
    u32 r[REGS] = {0};
    DsRasterKey k;
    for (u32 mode = 0; mode <= 7; mode++) {
        r[0x64F] = mode << 11 | 0x7FF;
        mx_draw_raster_key(r, &k);
        CHECK(k.depth_clamp == (mode == 0 || mode == 2 || mode == 4 || mode == 6 || mode == 7));
    }
    r[0x36B] = 0x1B00;
    r[0x5A1] = 3;
    r[0x373] = 16;
    mx_draw_raster_key(r, &k);
    CHECK(k.polygon == DS_POLYGON_POINT && k.provoking_last == 1 && k.patch_vertices == 16);
    r[0x36B] = 0x1B01;
    r[0x5A1] = 2;
    mx_draw_raster_key(r, &k);
    CHECK(k.polygon == DS_POLYGON_LINE && k.provoking_last == 0);
    r[0x36B] = 0x1B02;
    mx_draw_raster_key(r, &k);
    CHECK(k.polygon == DS_POLYGON_FILL);
}

/* buffer i's words at 0x1C1 + 4 i (at most 128), stride 0x1C2 + 4 i, layout bytes from 0xA00 + 0x20 i */
static void xfb(void) {
    u32 r[REGS] = {0};
    ShXfb x;
    r[0x1C1] = 200;
    r[0x1C2] = 0x10040;
    r[0x1C1 + 12] = 3;
    r[0xA00] = 0x23222120;
    r[0xA00 + 0x60 + 31] = 0x7F000000;
    mx_draw_xfb(r, &x);
    CHECK(x.count[0] == 128 && x.stride[0] == 0x40 && x.count[3] == 3 && x.count[1] == 0);
    CHECK(x.layout[0][0] == 0x20 && x.layout[0][3] == 0x23 && x.layout[3][127] == 0x7F);
}

/* RT_CONTROL 0x487: the count (at most 8) and 3 bits per slot from bit 4 naming target registers 0x200 + 0x10 j
   (address high and low, width, height, format, memory layout, third dimension bits 0-27, array pitch in words);
   depth at 0x3F8-0x3FC and 0x48A-0x48C, bound by 0x54E, its layers forced to one by 0x48C bit 16; 0x56E sRGB */
static void targets(void) {
    u32 r[REGS] = {0};
    DsTargets t;
    for (u32 j = 0; j < 8; j++) {
        u32* c = &r[0x200 + 0x10 * j];
        c[0] = j, c[1] = 0x1000000u * (j + 1), c[2] = 100 + j, c[3] = 50 + j, c[4] = 0xC0 + j, c[5] = 0x1010 + j;
        c[6] = 0xF0000000u | (j + 2), c[7] = 0x400 + j;
    }
    r[0x487] = 3 | (5u << 4) | (0u << 7) | (7u << 10) | (1u << 13); /* slots 0-2 take targets 5, 0 and 7 */
    r[0x56E] = 3;
    r[0x3F8] = 1, r[0x3F9] = 0x2000, r[0x3FA] = 0x16, r[0x3FB] = 0x20, r[0x3FC] = 0x80;
    r[0x48A] = 640, r[0x48B] = 360, r[0x48C] = 0x10006; /* forced to one layer */
    mx_draw_targets(r, &t);
    CHECK(t.count == 3 && t.srgb_write == 1);
    CHECK(t.color[0].gva == ((5ull << 32) | 0x6000000u) && t.color[0].width == 105 && t.color[0].height == 55);
    CHECK(t.color[0].format == 0xC5 && t.color[0].mem == 0x1015 && t.color[0].layers == 7 &&
          t.color[0].layer_stride == 0x405 * 4ull);
    CHECK(t.color[1].format == 0xC0 && t.color[2].format == 0xC7 && t.color[2].layers == 9);
    CHECK(t.color[3].gva == 0 && t.color[3].format == 0); /* past the count */
    CHECK(t.depth.gva == 0);                              /* 0x54E off */
    r[0x54E] = 1;
    r[0x487] = 15; /* a count above 8: 8, every slot taking target 0 */
    mx_draw_targets(r, &t);
    CHECK(t.count == 8 && t.color[7].format == 0xC0);
    CHECK(t.depth.gva == 0x100002000ull && t.depth.width == 640 && t.depth.height == 360 && t.depth.format == 0x16 &&
          t.depth.mem == 0x20 && t.depth.layers == 1 && t.depth.layer_stride == 0x200);
    r[0x48C] = 0x0006;
    mx_draw_targets(r, &t);
    CHECK(t.depth.layers == 6);
}

/* stream s: start 0x701 + 4 s (high, low), limit 0x7C0 + 2 s (high, low, inclusive) */
static void vertex_buffers(void) {
    u32 r[REGS] = {0};
    DsVertexBuffers b;
    r[0x701 + 4 * 2] = 1, r[0x702 + 4 * 2] = 0x1000, r[0x7C0 + 2 * 2] = 1, r[0x7C1 + 2 * 2] = 0x1FFF; /* 4 KiB */
    r[0x701 + 4 * 5] = 0, r[0x702 + 4 * 5] = 0x2000, r[0x7C1 + 2 * 5] = 0x1FFF;                       /* below */
    r[0x702 + 4 * 31] = 0x3000, r[0x7C1 + 2 * 31] = 0x3000;                                           /* a byte */
    r[0x702 + 4 * 7] = 0x4000, r[0x7C1 + 2 * 7] = 0x5000;
    mx_draw_vertex_buffers(r, &b);
    CHECK(b.addr[2] == 0x100001000ull && b.size[2] == 0x1000);
    CHECK(b.addr[5] == 0x2000 && b.size[5] == 0);
    CHECK(b.addr[31] == 0x3000 && b.size[31] == 1);
    CHECK(b.addr[7] == 0x4000 && b.size[7] == 0x1001);
    CHECK(b.addr[0] == 0 && b.size[0] == 1); /* start and limit 0: one byte */
}

/* the index buffer 0x5F2/0x5F3, its last byte 0x5F4/0x5F5, format 0x5F6; restart 0x591 and its index 0x592 */
static void index_state(void) {
    u32 r[REGS] = {0};
    DsIndex x;
    r[0x5F2] = 2, r[0x5F3] = 0x100, r[0x5F4] = 2, r[0x5F5] = 0x1FF, r[0x5F6] = 1;
    r[0x591] = 4, r[0x592] = 0xFFFE;
    mx_draw_index(r, &x);
    CHECK(x.addr == 0x200000100ull && x.end == 0x2000001FFull && x.format == 1);
    CHECK(x.restart == 1 && x.restart_index == 0xFFFE);
    r[0x591] = 0;
    mx_draw_index(r, &x);
    CHECK(x.restart == 0 && x.restart_index == 0xFFFE);
}

/* the texture bank 0x982, the TIC pool 0x55D/0x55E and its highest index 0x55F, the TSC pool 0x557/0x558 and 0x559,
   the sampler index mode 0x48D; capture buffer i at 0xE0 + 8 i (enable, address bits 32-39 and 0-31, size, start
   offset), on with a layout (0x1C1 + 4 i words or 0x1C2 + 4 i stride); the constant buffer slots left alone */
static void bindings(void) {
    u32 r[REGS] = {0};
    DsBindings b;
    memset(&b, 0, sizeof b);
    b.cb_addr[4][3] = 0x1234;
    r[0x982] = 2;
    r[0x55D] = 0x1, r[0x55E] = 0x200000, r[0x55F] = 0x7FF;
    r[0x557] = 0x2, r[0x558] = 0x300000, r[0x559] = 0xFF;
    r[0x48D] = 1;
    r[0xE0] = 1, r[0xE1] = 0x1FF, r[0xE2] = 0x400000, r[0xE3] = 0x1000, r[0xE4] = 0x40;
    r[0x1C1] = 4;
    r[0xE0 + 8] = 1, r[0xE3 + 8] = 0x800;                             /* no layout: off */
    r[0xE0 + 16] = 0, r[0x1C2 + 8] = 16;                              /* a layout, not enabled: off */
    r[0xE0 + 24] = 1, r[0xE3 + 24] = 0xFFFFFFF0u, r[0x1C2 + 12] = 32; /* a negative size, for the renderer */
    mx_draw_bindings(r, &b);
    CHECK(b.tex_bank == 2 && b.tex_pool == 0x100200000ull && b.tex_max == 0x7FF);
    CHECK(b.smp_pool == 0x200300000ull && b.smp_max == 0xFF && b.smp_mode == 1);
    CHECK(b.xfb[0].on && b.xfb[0].addr == 0xFF00400040ull && b.xfb[0].size == 0x1000);
    CHECK(!b.xfb[1].on && !b.xfb[2].on && b.xfb[3].on && b.xfb[3].size == 0xFFFFFFF0u);
    CHECK(b.cb_addr[4][3] == 0x1234);
}

/* a front end's inline constant buffer blocks: the slot names a stage's block 0 or 2, read as little-endian words,
   zeros past its end */
static void inline_blocks(void) {
    static DsState ds;
    ds.bind.cb_inline[4][0] = DS_CB_BANK0;
    ds.bind.cb_inline[4][2] = DS_CB_BANK2;
    ds.bind.cb_addr[4][3] = 0x5000;
    ds.inl.bank0[4][0x510] = 0x78, ds.inl.bank0[4][0x513] = 0x12;
    ds.inl.bank2[4][0x20] = 0x05, ds.inl.bank2[4][0x22] = 0x30;
    DsCbBlock b0 = ds_cb_inline(&ds, 4, 0), b2 = ds_cb_inline(&ds, 4, 2);
    CHECK(b0.bytes == ds.inl.bank0[4] && b0.size == DS_INLINE_BANK0 && b2.bytes == ds.inl.bank2[4] &&
          b2.size == DS_INLINE_BANK2);
    CHECK(!ds_cb_inline(&ds, 4, 3).bytes && !ds_cb_inline(&ds, 0, 0).bytes && !ds_cb_inline(&ds, 4, 40).bytes);
    CHECK(ds_inline_word(b0, 0x510) == 0x12000078u);
    CHECK(ds_inline_word(b2, 0x20) == 0x300005u);
    /* NVN's last words: the base instance at 0x644 of bank 0, separate sampler 127 at 0x960 of bank 2 */
    ds.inl.bank0[4][0x644] = 7, ds.inl.bank2[4][0x960] = 9;
    CHECK(ds_inline_word(b0, 0x644) == 7 && ds_inline_word(b2, 0x960) == 9);
    CHECK(ds_inline_word(b0, DS_INLINE_BANK0) == 0 && ds_inline_word(b2, DS_INLINE_BANK2 - 2) == 0);
}

/* a compute launch: its QMD and the compute engine's registers as the dispatch state. A fixture with each field set
   by hand, then random QMDs and registers checked against the accessors vk_dispatch read them through before the
   state (same program address, grid, block, shared memory, constant buffers and pools, so the same pipeline and
   shader keys and bindings) */
static u32 rng = 0x9E3779B9u;
static u32 rand32(void) { return rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5; }

static void dispatch_state(void) {
    u32 q[KEPLER_COMPUTE_QMD_WORDS] = {0}, r[REGS] = {0};
    DispatchState d;
    q[QMD_PROGRAM_START] = 0x1200;
    q[QMD_LINKED_TSC] = 1u << 30;
    q[QMD_GRID_DIM_X] = 0x80000040u; /* bit 31 isn't the grid's */
    q[QMD_GRID_DIM_YZ] = 3u << 16 | 5;
    q[QMD_SHARED_ALLOC] = 0xC0000u | 0x2000;
    q[QMD_BLOCK_DIM_X] = 64u << 16 | 0xFFFF;
    q[QMD_BLOCK_DIM_YZ] = 1u << 16 | 2;
    q[QMD_CONST_BUFFER_ENABLE] = 0x60000000u | 0x85; /* 0, 2 and 7; the cache layout isn't a buffer */
    q[QMD_CONST_BUFFER_CONFIG + 0] = 0x40001000u, q[QMD_CONST_BUFFER_CONFIG + 1] = 0x10000u << 15 | 0x1FF;
    q[QMD_CONST_BUFFER_CONFIG + 4] = 0x2000, q[QMD_CONST_BUFFER_CONFIG + 5] = 0x100u << 15 | 0x01;
    q[QMD_CONST_BUFFER_CONFIG + 2] = 0x7000, q[QMD_CONST_BUFFER_CONFIG + 3] = 0x40u << 15; /* not enabled */
    q[QMD_CONST_BUFFER_CONFIG + 14] = 0x3000, q[QMD_CONST_BUFFER_CONFIG + 15] = 0;
    r[KEPLER_COMPUTE_TIC] = 0x1, r[KEPLER_COMPUTE_TIC + 1] = 0x200000, r[KEPLER_COMPUTE_TIC + 2] = 0x7FF;
    r[KEPLER_COMPUTE_TSC] = 0x2, r[KEPLER_COMPUTE_TSC + 1] = 0x300000, r[KEPLER_COMPUTE_TSC + 2] = 0xFF;
    r[KEPLER_COMPUTE_TEX_CB_INDEX] = 0xA; /* index 2 */
    mx_dispatch_state(q, r, 0x500000000ull, &d);
    CHECK(d.code == 0x500001200ull && d.grid[0] == 0x40 && d.grid[1] == 5 && d.grid[2] == 3);
    CHECK(d.block[0] == 64 && d.block[1] == 2 && d.block[2] == 1 && d.shared == 0x2000);
    CHECK(d.cb_on == 0x85 && d.cb_addr[0] == 0xFF40001000ull && d.cb_size[0] == 0x10000);
    CHECK(d.cb_addr[2] == 0x100002000ull && d.cb_size[2] == 0x100 && d.cb_addr[7] == 0x3000 && !d.cb_size[7]);
    CHECK(!d.cb_addr[1] && !d.cb_size[1]);
    CHECK(d.tex_pool == 0x100200000ull && d.tex_max == 0x7FF && d.smp_pool == 0x200300000ull && d.smp_max == 0xFF);
    CHECK(d.smp_mode == 1 && d.tex_bank == 2);
    for (u32 i = 0; i < DX_CBUFS; i++) CHECK(!dx_cb_inline(&d, i).bytes);

    for (int n = 0; n < 100000; n++) {
        int bad = 0;
        u64 code = (u64)(rand32() & 0xFF) << 32 | rand32();
        for (u32 i = 0; i < KEPLER_COMPUTE_QMD_WORDS; i++) q[i] = rand32();
        r[KEPLER_COMPUTE_TIC] = rand32(), r[KEPLER_COMPUTE_TIC + 1] = rand32(), r[KEPLER_COMPUTE_TIC + 2] = rand32();
        r[KEPLER_COMPUTE_TSC] = rand32(), r[KEPLER_COMPUTE_TSC + 1] = rand32(), r[KEPLER_COMPUTE_TSC + 2] = rand32();
        r[KEPLER_COMPUTE_TEX_CB_INDEX] = rand32();
        mx_dispatch_state(q, r, code, &d);
        bad |= d.code != code + qmd_program_start(q);
        bad |= d.grid[0] != qmd_grid_dim_x(q) || d.grid[1] != qmd_grid_dim_y(q) || d.grid[2] != qmd_grid_dim_z(q);
        bad |= d.block[0] != qmd_block_dim_x(q) || d.block[1] != qmd_block_dim_y(q) ||
               d.block[2] != qmd_block_dim_z(q) || d.shared != qmd_shared_alloc(q);
        for (u32 i = 0; i < QMD_NUM_CONST_BUFFERS; i++) {
            int on = qmd_const_buffer_enabled(q, i);
            bad |= (int)(d.cb_on >> i & 1) != on || dx_cb_inline(&d, i).bytes != NULL;
            if (on) bad |= d.cb_addr[i] != qmd_const_buffer_address(q, i) || d.cb_size[i] != qmd_const_buffer_size(q, i);
        }
        bad |= d.cb_on >> QMD_NUM_CONST_BUFFERS != 0;
        bad |= d.tex_pool != kepler_compute_address(r, KEPLER_COMPUTE_TIC) ||
               d.smp_pool != kepler_compute_address(r, KEPLER_COMPUTE_TSC) ||
               d.tex_bank != kepler_compute_tex_cb_index(r);
        if (bad) {
            CHECK(!"a random launch's dispatch state differs from its QMD's fields");
            break;
        }
    }

    /* a front end's inline blocks: a slot names the dispatch's bank 0 or 2 (NVN's dispatch size at 0x200) */
    static DispatchState x;
    x.cb_on = 5, x.cb_inline[0] = DS_CB_BANK0, x.cb_inline[2] = DS_CB_BANK2;
    x.bank0[0x200] = 8, x.bank0[0x204] = 4, x.bank2[0x30] = 0x11;
    DsCbBlock b0 = dx_cb_inline(&x, 0), b2 = dx_cb_inline(&x, 2);
    CHECK(b0.bytes == x.bank0 && b0.size == DS_INLINE_BANK0 && b2.bytes == x.bank2 && b2.size == DS_INLINE_BANK2);
    CHECK(ds_inline_word(b0, 0x200) == 8 && ds_inline_word(b0, 0x204) == 4 && ds_inline_word(b2, 0x30) == 0x11);
    CHECK(!dx_cb_inline(&x, 1).bytes && !dx_cb_inline(&x, DX_CBUFS).bytes);
}

int main(void) {
    stencil();
    origin();
    line_width();
    bias();
    others();
    viewports();
    programs();
    vertex_format();
    blend();
    raster_key();
    xfb();
    targets();
    vertex_buffers();
    index_state();
    bindings();
    inline_blocks();
    dispatch_state();
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    printf("draw_state_mx: all checks passed\n");
    return 0;
}
