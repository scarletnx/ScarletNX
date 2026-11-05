/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn's constant banks and point sprite origin, which the game's driver's must match:
   nvn_draw_init's bank 0 point size and bank 2 words, a program's bank 1, and the window and sprite origins, front face
   and stencil faces of a draw's dynamic state. The records go in through nvn_draw_record */
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "nx/gpu/renderer.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/hle/services/nvdrv.h"
#include "common/platform/thread.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static u8 memory[1 << 20];
u8* g_mem = memory; /* guest address a is memory + a */

/* ---- what the file needs of the runtime: stubs, none of them reached by the cases below */
Settings g_settings;
static Renderer g_none; /* a renderer with no draw: a draw only builds the state */
const Renderer* g_renderer = &g_none;
void pm_init(PMutex* m) { (void)m; }
int g_shader_sm86; /* shader.c's: Maxwell programs */
const char* g_shader_sm86_dump; /* shader.c's: no dumps */
u64 ds_generation_next(void) {
    static u64 g;
    return ++g;
}
/* guest memory is the GPU's */
int gmem_read(u64 gva, void* dst, u64 size) {
    memcpy(dst, GP(gva), size);
    return 1;
}
int gmem_write(u64 gva, const void* src, u64 size) {
    (void)gva;
    (void)src;
    (void)size;
    return 0;
}
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    *cpu = gva;
    return max;
}
int nvn_program_xfb(u64 serial, ShXfb* x) {
    (void)serial;
    (void)x;
    return 0;
}
void nvn_program_c0(u64 serial, u8 c0[NVN_C0_RECORD]) {
    (void)serial;
    memset(c0, 0, NVN_C0_RECORD);
}
int nvn_program_tess(u64 serial, NvnTessStage stages[2]) {
    (void)serial;
    (void)stages;
    return 0;
}
int nvn_subroutine_apply(u32 linkage, i32 first, i32 count, const i32* values) {
    (void)linkage;
    (void)first;
    (void)count;
    (void)values;
    return 1;
}
/* the texture layout and formats: only the render target records use them */
u32 nvn_rt_format(u32 format, int* depth) {
    *depth = format == NVN_FORMAT_DEPTH32F;
    if (*depth) return 0x0a;
    return format == NVN_FORMAT_RGBA8 ? 0xD5 : 0; /* compressed formats: no target takes them */
}
void nvn_tex_layout(const NvnTexDesc* d, NvnTexLayout* o) {
    (void)d;
    memset(o, 0, sizeof *o);
}
u64 nvn_tex_level_offset(const NvnTexDesc* d, unsigned level) {
    (void)d;
    (void)level;
    return 0;
}
void nvn_tex_level_tiles(const NvnTexDesc* d, unsigned level, unsigned* tile_h, unsigned* tile_d) {
    (void)d;
    (void)level;
    *tile_h = *tile_d = 0;
}
u64 nvn_tex_slice_offset(const NvnTexDesc* d, unsigned level, u32 z) {
    (void)d;
    (void)level;
    (void)z;
    return 0;
}
u32 nvn_tex_level_gobs(const NvnTexDesc* d, unsigned level) {
    (void)d;
    (void)level;
    return 0;
}
u32 nvn_tex_msaa(int samples) {
    return samples == 2 ? DS_MSAA_2X1 : samples == 4 ? DS_MSAA_2X2
         : samples == 8 ? DS_MSAA_4X2 : samples == 16 ? DS_MSAA_4X4 : DS_MSAA_1X1;
}
int nvn_fmt_bpb(int format, int* bw, int* bh) {
    int bc7 = format == NVN_FORMAT_BPTC_UNORM_SRGB, bc1 = format == NVN_FORMAT_RGBA_DXT1;
    if (bw) *bw = bc7 || bc1 ? 4 : 1;
    if (bh) *bh = bc7 || bc1 ? 4 : 1;
    return bc7 ? 16 : bc1 ? 8 : format == NVN_FORMAT_RGBA8 ? 4 : 0;
}
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

static int failures;
static void check(int ok, const char* what, ...) {
    if (!ok) {
        va_list ap;
        va_start(ap, what);
        fprintf(stderr, "FAIL: ");
        vfprintf(stderr, what, ap);
        fprintf(stderr, "\n");
        va_end(ap);
        failures++;
    }
}

static NvnDraw d; /* big: not on the stack */

/* nvn_draw_init: bank 0's point size and bank 2's words 0 to 7 in every graphics stage, and in compute's bank 2 */
static void test_init(void) {
    static const float want = 2047.9375f;
    u32 want_bits, s, k;
    memcpy(&want_bits, &want, 4);
    memset(&d, 0, sizeof d); /* nvn_draw_init's contract: a state all zero */
    nvn_draw_init(&d, 0);
    for (s = 0; s < DS_GFX_STAGES; s++) {
        u32 got = snx_ld_le32(d.inl.bank0[s] + 0x610);
        check(got == want_bits, "stage %u bank 0 at 0x610: expected %#x (2047.9375f), found %#x", s, want_bits, got);
        for (k = 0; k < 8; k++) {
            u32 w = snx_ld_le32(d.inl.bank2[s] + 4 * k);
            check(w == k, "stage %u bank 2 word %u: expected %u, found %u", s, k, k, w);
        }
    }
    for (k = 0; k < 8; k++) {
        u32 w = snx_ld_le32(d.dx.bank2 + 4 * k);
        check(w == k, "compute bank 2 word %u: expected %u, found %u", k, k, w);
    }
}

/* OP_BIND_PROGRAM: a stage with code binds its bank 1 as 64 KiB at data + 256 * cb, whatever cb_size says; a stage
   without code binds nothing */
#define PROG 0x1000u
static void bind(const NvnStage* vs, const NvnStage* fs) {
    NvnPBindProgram r;
    memset(&r, 0, sizeof r);
    r.stages = NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT;
    r.off_a = 0; /* a plain vertex program: its code is st.data + st.off */
    r.st[NVN_SHADER_STAGE_VERTEX] = *vs;
    r.st[NVN_SHADER_STAGE_FRAGMENT] = *fs;
    memcpy(g_mem + PROG, &r, sizeof r);
    check(nvn_draw_record(&d, OP_BIND_PROGRAM, PROG, sizeof r) == 1, "OP_BIND_PROGRAM was not taken");
}
static void test_bind_program(void) {
    const int fs = 4; /* the fragment stage's graphics index */
    NvnStage a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    /* vertex: data, cb 5, cb_size 0; fragment: data, cb 7, cb_size 3 */
    a.data = 0x40000, a.cb = 5, a.cb_size = 0;
    b.data = 0x50000, b.cb = 7, b.cb_size = 3;
    bind(&a, &b);
    check(d.bind.cb_addr[0][1] == 0x40000 + 256 * 5, "vertex bank 1 address: expected %#llx, found %#llx",
          0x40000ull + 256 * 5, (unsigned long long)d.bind.cb_addr[0][1]);
    check(d.bind.cb_size[0][1] == 0x10000, "vertex bank 1 size (cb_size 0): expected 0x10000, found %#x",
          d.bind.cb_size[0][1]);
    check(d.bind.cb_addr[fs][1] == 0x50000 + 256 * 7, "fragment bank 1 address: expected %#llx, found %#llx",
          0x50000ull + 256 * 7, (unsigned long long)d.bind.cb_addr[fs][1]);
    check(d.bind.cb_size[fs][1] == 0x10000, "fragment bank 1 size (cb_size 3): expected 0x10000, found %#x",
          d.bind.cb_size[fs][1]);
    /* no code in the fragment stage: nothing bound */
    memset(&b, 0, sizeof b);
    bind(&a, &b);
    check(d.bind.cb_addr[fs][1] == 0, "fragment bank 1 address (no data): expected 0, found %#llx",
          (unsigned long long)d.bind.cb_addr[fs][1]);
    check(d.bind.cb_size[fs][1] == 0, "fragment bank 1 size (no data): expected 0, found %#x", d.bind.cb_size[fs][1]);
}

/* a draw's dynamic state, built by sync on the draw: the window origin the driver writes and its sprite origin bit */
#define DRAW 0x2000u
#define POLY 0x2800u
/* a queue initialized while the device's origin is at_init, the device then set to origin and depth_mode, a polygon
   state of front face front and back culling bound, and a draw */
static void draw_with(u32 at_init, u32 origin, u32 depth_mode, u32 front) {
    NvnPDraw r;
    NvnPBindState st;
    u32 poly = (u32)nvn_set_field(nvn_set_field(nvn_set_field(0, NVN_POLY_CULL, NVN_FACE_BACK), NVN_POLY_FRONT, front),
                                  NVN_POLY_MODE, NVN_POLYGON_MODE_FILL);
    memset(&r, 0, sizeof r);
    r.count = 3;
    r.instances = 1;
    memcpy(g_mem + DRAW, &r, sizeof r);
    memset(&st, 0, sizeof st);
    st.kind = 4;
    memcpy(st.bytes, &poly, sizeof poly);
    memcpy(g_mem + POLY, &st, sizeof st);
    nvn_draw_device(at_init, depth_mode);
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    nvn_draw_device(origin, depth_mode);
    check(nvn_draw_record(&d, OP_BIND_STATE, POLY, sizeof st) == 1, "OP_BIND_STATE was not taken");
    check(nvn_draw_record(&d, OP_DRAW, DRAW, sizeof r) == 1, "OP_DRAW was not taken");
}
/* the driver writes the window origin register as it initializes the queue, the flip for the device's origin lower
   left then (53.13's), and the polygon state's front face as it is: one game initializes its queue and then sets an
   upper left origin, so it keeps the flip; another sets that origin first, so it has none. Neither the depth mode nor
   the driver's version changes either */
static void test_window_origin(void) {
    static const struct {
        u32 api, at_init, origin, depth_mode, front, window;
        const char* what;
    } cases[] = {
        {53u << 16 | 13, NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT,
         NVN_DEPTH_MODE_NEAR_IS_MINUS_W, NVN_FRONT_FACE_CW, DS_ORIGIN_FLIP_Y, "53.13, upper left after the queue"},
        {52u << 16 | 32, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT,
         NVN_DEPTH_MODE_NEAR_IS_MINUS_W, NVN_FRONT_FACE_CCW, 0, "52.32, upper left before the queue"},
        {53u << 16 | 13, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT,
         NVN_DEPTH_MODE_NEAR_IS_MINUS_W, NVN_FRONT_FACE_CW, 0, "53.13, upper left before the queue"},
        {55u << 16 | 15, NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT, NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT,
         NVN_DEPTH_MODE_NEAR_IS_MINUS_W, NVN_FRONT_FACE_CCW, DS_ORIGIN_FLIP_Y, "55.15, lower left"},
        {55u << 16 | 15, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT,
         NVN_DEPTH_MODE_NEAR_IS_ZERO, NVN_FRONT_FACE_CW, 0, "55.15, upper left, near = 0"},
        {53u << 16 | 105, NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT, NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT,
         NVN_DEPTH_MODE_NEAR_IS_ZERO, NVN_FRONT_FACE_CCW, DS_ORIGIN_FLIP_Y, "53.105, lower left, near = 0"},
        {55u << 16 | 15, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT, NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT,
         NVN_DEPTH_MODE_NEAR_IS_MINUS_W, NVN_FRONT_FACE_CCW, 0, "55.15, lower left after the queue"},
    };
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) {
        g_nvn.loader.api = cases[i].api;
        draw_with(cases[i].at_init, cases[i].origin, cases[i].depth_mode, cases[i].front);
        check(d.ds.dyn.window_origin == cases[i].window, "%s window_origin: expected %#x, found %#x", cases[i].what,
              cases[i].window, d.ds.dyn.window_origin);
        check(d.ds.dyn.front_cw == (cases[i].front == NVN_FRONT_FACE_CW), "%s front_cw: expected %d, found %u",
              cases[i].what, cases[i].front == NVN_FRONT_FACE_CW, d.ds.dyn.front_cw);
        check(d.ds.dyn.sprite_origin == 1, "%s sprite_origin: expected 1, found %u", cases[i].what,
              d.ds.dyn.sprite_origin);
    }
}

/* OP_BIND_STATE of a depth-stencil word whose faces differ, then a draw: each face of the dynamic state takes its own
   fields (the back's four were read from bits 4 to 7, under the depth function) */
#define STATE 0x3000u
static void test_stencil_faces(void) {
    NvnPBindState st;
    NvnPDraw r;
    u64 w = 0;
    w = nvn_set_field(w, NVN_DS_TEST, 1);
    w = nvn_set_field(w, NVN_DS_FUNC, NVN_DEPTH_FUNC_GEQUAL);
    w = nvn_set_field(w, NVN_DS_STENCIL, 1);
    w = nvn_set_field(w, NVN_DS_SFUNC_F, NVN_STENCIL_FUNC_ALWAYS);
    w = nvn_set_field(w, NVN_DS_FAIL_F, NVN_STENCIL_OP_KEEP);
    w = nvn_set_field(w, NVN_DS_ZFAIL_F, NVN_STENCIL_OP_KEEP);
    w = nvn_set_field(w, NVN_DS_ZPASS_F, NVN_STENCIL_OP_INCR_WRAP);
    w = nvn_set_field(w, NVN_DS_SFUNC_B, NVN_STENCIL_FUNC_NOTEQUAL);
    w = nvn_set_field(w, NVN_DS_FAIL_B, NVN_STENCIL_OP_ZERO);
    w = nvn_set_field(w, NVN_DS_ZFAIL_B, NVN_STENCIL_OP_REPLACE);
    w = nvn_set_field(w, NVN_DS_ZPASS_B, NVN_STENCIL_OP_DECR_WRAP);
    memset(&st, 0, sizeof st);
    st.kind = 5;
    memcpy(st.bytes, &w, sizeof w);
    memcpy(g_mem + STATE, &st, sizeof st);
    memset(&r, 0, sizeof r);
    r.count = 3;
    r.instances = 1;
    memcpy(g_mem + DRAW, &r, sizeof r);
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    check(nvn_draw_record(&d, OP_BIND_STATE, STATE, sizeof st) == 1, "OP_BIND_STATE was not taken");
    check(nvn_draw_record(&d, OP_DRAW, DRAW, sizeof r) == 1, "OP_DRAW was not taken");
    const DsDynamic* y = &d.ds.dyn;
    check(y->face[0].cmp == DS_CMP_ALWAYS && y->face[0].fail == DS_STENCIL_KEEP &&
              y->face[0].zfail == DS_STENCIL_KEEP && y->face[0].pass == DS_STENCIL_INCREMENT_AND_WRAP,
          "front face: expected compare %u, operations %u %u %u; found %u, %u %u %u", DS_CMP_ALWAYS, DS_STENCIL_KEEP,
          DS_STENCIL_KEEP, DS_STENCIL_INCREMENT_AND_WRAP, y->face[0].cmp, y->face[0].fail, y->face[0].zfail,
          y->face[0].pass);
    check(y->face[1].cmp == DS_CMP_NOT_EQUAL && y->face[1].fail == DS_STENCIL_ZERO &&
              y->face[1].zfail == DS_STENCIL_REPLACE && y->face[1].pass == DS_STENCIL_DECREMENT_AND_WRAP,
          "back face: expected compare %u, operations %u %u %u; found %u, %u %u %u", DS_CMP_NOT_EQUAL, DS_STENCIL_ZERO,
          DS_STENCIL_REPLACE, DS_STENCIL_DECREMENT_AND_WRAP, y->face[1].cmp, y->face[1].fail, y->face[1].zfail,
          y->face[1].pass);
}

/* OP_BIND_VATTR then a draw: each attribute's size code and type (Eden's VertexAttribute::Size and Type), an RGBX
   format as its RGBA twin */
#define VATTR 0x3800u
static void test_vertex_formats(void) {
    static const struct {
        u32 format;
        u8 size, type;
    } cases[] = {
        {NVN_FORMAT_RGBA8, 0x0A, 2},   {NVN_FORMAT_RGBA32I, 0x01, 3}, {NVN_FORMAT_RGBX8, 0x0A, 2},
        {NVN_FORMAT_RGBX16F, 0x03, 7}, {NVN_FORMAT_RGBX32F, 0x01, 7}, {NVN_FORMAT_RGBX32UI, 0x01, 4},
        {NVN_FORMAT_RGBX32I, 0x01, 3}, {NVN_FORMAT_R8_UI2F, 0x1D, 5}, {NVN_FORMAT_RGB10A2, 0x30, 2},
        {NVN_FORMAT_BGRA8, 0x0A, 2},
    };
    const u32 n = sizeof cases / sizeof *cases;
    NvnPBindVAttr va = {0};
    NvnPDraw r = {0};
    va.count = (i32)n;
    for (u32 i = 0; i < n; i++) va.st[i] = (u32)nvn_set_field(0, NVN_VA_FORMAT, cases[i].format);
    memcpy(g_mem + VATTR, &va, sizeof va);
    r.count = 3;
    r.instances = 1;
    memcpy(g_mem + DRAW, &r, sizeof r);
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    check(nvn_draw_record(&d, OP_BIND_VATTR, VATTR, sizeof va) == 1, "OP_BIND_VATTR was not taken");
    check(nvn_draw_record(&d, OP_DRAW, DRAW, sizeof r) == 1, "OP_DRAW was not taken");
    for (u32 i = 0; i < n; i++)
        check(d.ds.vfmt.size[i] == cases[i].size && d.ds.vfmt.type[i] == cases[i].type,
              "attribute format %#x: expected size %#x type %u, found size %#x type %u", cases[i].format, cases[i].size,
              cases[i].type, d.ds.vfmt.size[i], d.ds.vfmt.type[i]);
}

/* OP_BIND_SSBO: storage buffer r of a graphics stage goes to that stage's bank 0, address then size, at its table's
   base + 16 r: the vertex stage's from 0x110, the geometry stage's from 0x410 (nxnative's dmsh_abi.h), the fragment
   stage's from 0x510. A tessellation stage, whose table was never measured, writes no bank */
#define SSBO 0x4000u
static void test_storage_tables(void) {
    static const struct {
        i32 nvn;
        int gfx;
        u32 base;
    } cases[] = {
        {NVN_SHADER_STAGE_VERTEX, 0, 0x110},          {NVN_SHADER_STAGE_TESS_CONTROL, 1, 0x210},
        {NVN_SHADER_STAGE_TESS_EVALUATION, 2, 0x310}, {NVN_SHADER_STAGE_GEOMETRY, 3, 0x410},
        {NVN_SHADER_STAGE_FRAGMENT, 4, 0x510},
    };
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) {
        NvnPBindBuf r = {.stage = cases[i].nvn, .index = 3, .address = 0x4f0001000ull + 0x100 * i, .size = 0x340 + i};
        memcpy(g_mem + SSBO, &r, sizeof r);
        check(nvn_draw_record(&d, OP_BIND_SSBO, SSBO, sizeof r) == 1, "OP_BIND_SSBO was not taken");
        const u8* at = d.inl.bank0[cases[i].gfx] + cases[i].base + 16 * 3;
        u64 address = (u64)snx_ld_le32(at) | (u64)snx_ld_le32(at + 4) << 32;
        u32 size = snx_ld_le32(at + 8);
        check(address == r.address && size == (u32)r.size,
              "NVN stage %d storage buffer 3 at bank 0 %#x: expected %#llx, %#x bytes; found %#llx, %#x bytes",
              cases[i].nvn, cases[i].base + 16 * 3, (unsigned long long)r.address, (u32)r.size,
              (unsigned long long)address, size);
    }
    NvnPDraw draw = {.count = 3, .instances = 1};
    memcpy(GP(DRAW), &draw, sizeof draw);
    check(nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw), "draw after all-stage storage binds accepted");
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) {
        const u8* at = d.ds.inl.bank0[cases[i].gfx] + cases[i].base + 16 * 3;
        check(snx_ld_le64(at) == 0x4f0001000ull + 0x100 * i && snx_ld_le32(at + 8) == 0x340 + i,
              "stage %d storage descriptor reaches the renderer's inline bank", cases[i].nvn);
    }
}

/* Sparse writes to distant stages and both inline banks must preserve every other byte. A generation changes only
   when the bytes that the renderer sees change, even if a record writes and then restores a value before the draw. */
static void test_inline_updates(void) {
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    NvnPDrawIdx r = {.count = 3,
                     .instances = 1,
                     .index_type = NVN_INDEX_TYPE_UNSIGNED_SHORT,
                     .index_buffer = 0x70000,
                     .base_vertex = -17,
                     .base_instance = 91};
    memcpy(GP(DRAW), &r, sizeof r);
    DsInline expected = d.inl;
    for (u32 s = 0; s < DS_GFX_STAGES; s++) {
        snx_st_le32(expected.bank0[s] + SH_CB0_BASE_VERTEX, (u32)r.base_vertex);
        snx_st_le32(expected.bank0[s] + SH_CB0_BASE_INSTANCE, (u32)r.base_instance);
    }
    check(nvn_draw_record(&d, OP_DRAW_IDX, DRAW, sizeof r), "first indexed draw accepted");
    check(!memcmp(&d.ds.inl, &expected, sizeof expected), "first draw copies all initial inline blocks and parameters");
    u64 gen = d.ds.gen[DS_INLINE];
    check(nvn_draw_record(&d, OP_DRAW_IDX, DRAW, sizeof r), "repeated indexed draw accepted");
    check(d.ds.gen[DS_INLINE] == gen, "equal draw parameters keep the inline generation");

    static const i32 stages[] = {NVN_SHADER_STAGE_VERTEX, NVN_SHADER_STAGE_FRAGMENT};
    static const u32 gfx[] = {0, 4};
    for (u32 i = 0; i < 2; i++) {
        NvnPBindTex1 t = {.stage = stages[i], .slot = 31, .h = 0x1234567800ull + i};
        memcpy(GP(SSBO), &t, sizeof t);
        check(nvn_draw_record(&d, OP_BIND_TEX1, SSBO, sizeof t), "sparse texture handle accepted");
        snx_st_le64(expected.bank2[gfx[i]] + 0x20 + 8 * 31, t.h);
        NvnPBindBuf b = {.stage = stages[i], .index = 15, .address = 0x4a0000000ull + 0x1000 * i, .size = 512 + i};
        memcpy(GP(SSBO), &b, sizeof b);
        check(nvn_draw_record(&d, OP_BIND_SSBO, SSBO, sizeof b), "sparse storage buffer accepted");
        u32 off = (i ? 0x510u : 0x110u) + 16 * 15;
        snx_st_le64(expected.bank0[gfx[i]] + off, b.address);
        snx_st_le32(expected.bank0[gfx[i]] + off + 8, (u32)b.size);
    }
    check(nvn_draw_record(&d, OP_DRAW_IDX, DRAW, sizeof r), "draw after sparse bindings accepted");
    check(!memcmp(&d.ds.inl, &expected, sizeof expected), "sparse bindings preserve every unchanged inline byte");
    check(d.ds.gen[DS_INLINE] != gen, "sparse bindings change the inline generation");
    gen = d.ds.gen[DS_INLINE];

    NvnPBindTex1 t = {.stage = NVN_SHADER_STAGE_FRAGMENT, .slot = 31, .h = 0xdeadbeef};
    memcpy(GP(SSBO), &t, sizeof t);
    nvn_draw_record(&d, OP_BIND_TEX1, SSBO, sizeof t);
    t.h = snx_ld_le64(expected.bank2[4] + 0x20 + 8 * 31);
    memcpy(GP(SSBO), &t, sizeof t);
    nvn_draw_record(&d, OP_BIND_TEX1, SSBO, sizeof t);
    check(nvn_draw_record(&d, OP_DRAW_IDX, DRAW, sizeof r), "draw after reverted handle accepted");
    check(d.ds.gen[DS_INLINE] == gen, "a reverted handle keeps the inline generation");
    check(!memcmp(&d.ds.inl, &expected, sizeof expected), "a reverted handle preserves every inline byte");

    t.stage = NVN_SHADER_STAGE_COMPUTE;
    t.h = 0x87654321;
    memcpy(GP(SSBO), &t, sizeof t);
    nvn_draw_record(&d, OP_BIND_TEX1, SSBO, sizeof t);
    check(nvn_draw_record(&d, OP_DRAW_IDX, DRAW, sizeof r), "draw after compute handle accepted");
    check(d.ds.gen[DS_INLINE] == gen, "a compute handle keeps the graphics inline generation");
    check(snx_ld_le64(d.dx.bank2 + 0x20 + 8 * 31) == t.h, "the compute handle reaches its own bank");

    r.base_vertex = 19;
    r.base_instance = 5;
    memcpy(GP(DRAW), &r, sizeof r);
    for (u32 s = 0; s < DS_GFX_STAGES; s++) {
        snx_st_le32(expected.bank0[s] + SH_CB0_BASE_VERTEX, (u32)r.base_vertex);
        snx_st_le32(expected.bank0[s] + SH_CB0_BASE_INSTANCE, (u32)r.base_instance);
    }
    check(nvn_draw_record(&d, OP_DRAW_IDX, DRAW, sizeof r), "changed draw parameters accepted");
    check(!memcmp(&d.ds.inl, &expected, sizeof expected), "changed parameters preserve all bound handles and storage");
    check(d.ds.gen[DS_INLINE] != gen, "changed draw parameters change the inline generation");
}

/* CPU front-end cost only. These records use no Vulkan driver or game data. Each workload varies one draw parameter
   in every stage, then checks all bytes of the result against the expected inline state. */
static void bench_inline_updates(void) {
    const u32 draws = 1000000;
    for (u32 instance = 0; instance < 2; instance++) {
        memset(&d, 0, sizeof d);
        nvn_draw_init(&d, 0);
        NvnPDrawIdx r = {
            .count = 3, .instances = 1, .index_type = NVN_INDEX_TYPE_UNSIGNED_SHORT, .index_buffer = 0x70000};
        memcpy(GP(DRAW), &r, sizeof r);
        nvn_draw_record(&d, OP_DRAW_IDX, DRAW, sizeof r);
        DsInline expected = d.ds.inl;
        clock_t start = clock();
        for (u32 i = 1; i <= draws; i++) {
            if (instance)
                r.base_instance = (i32)i;
            else
                r.base_vertex = (i32)i;
            memcpy(GP(DRAW), &r, sizeof r);
            nvn_draw_record(&d, OP_DRAW_IDX, DRAW, sizeof r);
        }
        clock_t ticks = clock() - start;
        for (u32 s = 0; s < DS_GFX_STAGES; s++) {
            snx_st_le32(expected.bank0[s] + SH_CB0_BASE_VERTEX, (u32)r.base_vertex);
            snx_st_le32(expected.bank0[s] + SH_CB0_BASE_INSTANCE, (u32)r.base_instance);
        }
        int exact = !memcmp(&d.ds.inl, &expected, sizeof expected);
        check(exact, "the inline benchmark preserves every inline byte");
        printf("NVN_INLINE_BENCH parameter=%s draws=%u ns=%llu exact=%d\n", instance ? "instance" : "vertex", draws,
               (unsigned long long)((u64)ticks * 1000000000ull / CLOCKS_PER_SEC), exact);
    }
}

/* Equal state records have no external effect. Rebinding a texture still changes no bytes when its handle is
   equal; equal UBO sizes are compared after the driver's 256-byte rounding and 64 KiB encoding. */
static void binding_records(void) {
    NvnPBindState state[6] = {{0}};
    memcpy(state[0].bytes, &d.blend[0], sizeof d.blend[0]);
    memcpy(state[1].bytes, &d.color, sizeof d.color);
    memcpy(state[2].bytes, &d.chmask, sizeof d.chmask);
    memcpy(state[3].bytes, &d.ms, sizeof d.ms);
    memcpy(state[4].bytes, &d.poly, sizeof d.poly);
    memcpy(state[5].bytes, &d.dss, sizeof d.dss);
    for (u32 i = 0; i < 6; i++) {
        state[i].kind = (i32)i;
        memcpy(GP(0x8000 + i * 64), &state[i], sizeof state[i]);
    }
    NvnPBindBuf uniform = {.stage = NVN_SHADER_STAGE_VERTEX, .index = 3, .address = 0x50000, .size = 1};
    NvnPBindBuf storage = {.stage = NVN_SHADER_STAGE_FRAGMENT, .index = 15, .address = 0x60000, .size = 512};
    NvnPBindTex1 texture = {.stage = NVN_SHADER_STAGE_FRAGMENT, .slot = 31, .h = 0x12345678};
    memcpy(GP(0x8200), &uniform, sizeof uniform);
    memcpy(GP(0x8240), &storage, sizeof storage);
    memcpy(GP(0x8280), &texture, sizeof texture);
}
static void apply_binding_records(int state) {
    if (state) {
        for (u32 kind = 0; kind < 6; kind++)
            nvn_draw_record(&d, OP_BIND_STATE, 0x8000 + kind * 64, sizeof(NvnPBindState));
    } else {
        nvn_draw_record(&d, OP_BIND_UBO, 0x8200, sizeof(NvnPBindBuf));
        nvn_draw_record(&d, OP_BIND_SSBO, 0x8240, sizeof(NvnPBindBuf));
        nvn_draw_record(&d, OP_BIND_TEX1, 0x8280, sizeof(NvnPBindTex1));
    }
}
static void test_equal_bindings(void) {
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    NvnPDraw draw = {.count = 3, .instances = 1};
    memcpy(GP(DRAW), &draw, sizeof draw);
    binding_records();
    apply_binding_records(1);
    apply_binding_records(0);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    DsState expected = d.ds;
    for (u32 i = 0; i < 4; i++) {
        apply_binding_records(1);
        apply_binding_records(0);
        nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
        check(!memcmp(&d.ds, &expected, sizeof expected),
              "equal state, buffer and texture binds preserve all draw state");
    }
    NvnPBindBuf uniform;
    memcpy(&uniform, GP(0x8200), sizeof uniform);
    uniform.size = 255; /* the same 256-byte range after rounding */
    memcpy(GP(0x8200), &uniform, sizeof uniform);
    apply_binding_records(0);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(!memcmp(&d.ds, &expected, sizeof expected), "equal rounded UBO ranges preserve the draw state");

    NvnPBindState mask = {.kind = 2};
    u32 changed = d.chmask ^ 1u;
    memcpy(mask.bytes, &changed, sizeof changed);
    memcpy(GP(0x8400), &mask, sizeof mask);
    nvn_draw_record(&d, OP_BIND_STATE, 0x8400, sizeof mask);
    /* Equal records must not clear work marked dirty by a preceding changed record. */
    for (u32 kind = 3; kind < 6; kind++) nvn_draw_record(&d, OP_BIND_STATE, 0x8000 + kind * 64, sizeof(NvnPBindState));
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(d.ds.gen[DS_BLEND] != expected.gen[DS_BLEND], "a changed channel mask survives later equal state binds");
    check(d.ds.gen[DS_INLINE] == expected.gen[DS_INLINE] && d.ds.gen[DS_BIND] == expected.gen[DS_BIND],
          "changed packed state leaves inline and binding generations alone");
    uniform.size = 0;
    memcpy(GP(0x8200), &uniform, sizeof uniform);
    apply_binding_records(0);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(d.ds.bind.cb_addr[0][6] == 0 && d.ds.bind.cb_size[0][6] == 0, "zero size unbinds a previously bound UBO");
}
static void test_depth_mode_equal_bind(void) {
    u32 origin = __atomic_load_n(&g_nvn.draw.origin, __ATOMIC_RELAXED);
    u32 old_mode = __atomic_load_n(&g_nvn.draw.depth_mode, __ATOMIC_RELAXED);
    u32 mode = old_mode == NVN_DEPTH_MODE_NEAR_IS_ZERO ? NVN_DEPTH_MODE_NEAR_IS_MINUS_W : NVN_DEPTH_MODE_NEAR_IS_ZERO;
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    NvnPDraw draw = {.count = 3, .instances = 1};
    memcpy(GP(DRAW), &draw, sizeof draw);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    NvnPBindState depth = {.kind = 5};
    memcpy(depth.bytes, &d.dss, sizeof d.dss);
    memcpy(GP(0x8400), &depth, sizeof depth);
    nvn_draw_device(origin, mode);
    nvn_draw_record(&d, OP_BIND_STATE, 0x8400, sizeof depth);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(d.ds.dyn.depth_mode == (mode == NVN_DEPTH_MODE_NEAR_IS_ZERO),
          "an equal packed depth-state bind still observes the device's changed depth convention");
    nvn_draw_device(origin, old_mode);
}
/* Each operation is one draw with all six packed states rebound, or one draw with a UBO, SSBO and texture rebound.
   Both modes check the complete final draw state and generations against the first draw. */
static void bench_equal_bindings(void) {
    const u32 operations = 1000000;
    for (u32 state = 0; state < 2; state++) {
        memset(&d, 0, sizeof d);
        nvn_draw_init(&d, 0);
        NvnPDraw draw = {.count = 3, .instances = 1};
        memcpy(GP(DRAW), &draw, sizeof draw);
        binding_records();
        apply_binding_records((int)state);
        nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
        DsState expected = d.ds;
        clock_t start = clock();
        for (u32 i = 0; i < operations; i++) {
            apply_binding_records((int)state);
            nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
        }
        clock_t ticks = clock() - start;
        int exact = !memcmp(&d.ds, &expected, sizeof expected);
        check(exact, "equal-binding benchmark preserves every draw-state byte and generation");
        printf("GPU_HOTPATH_BENCH workload=%s operations=%u ns=%llu exact=%d\n",
               state ? "equal-state" : "equal-bindings", operations,
               (unsigned long long)((u64)ticks * 1000000000ull / CLOCKS_PER_SEC), exact);
    }
    /* A changed-state control keeps the rebuild path hot instead of rewarding only repeated inputs. */
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    NvnPDraw draw = {.count = 3, .instances = 1};
    memcpy(GP(DRAW), &draw, sizeof draw);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    DsState expected = d.ds;
    NvnPBindState mask = {.kind = 2};
    clock_t start = clock();
    for (u32 i = 0; i < operations; i++) {
        u32 value = 0xffffffffu ^ (i & 1u);
        memcpy(mask.bytes, &value, sizeof value);
        memcpy(GP(0x8400), &mask, sizeof mask);
        nvn_draw_record(&d, OP_BIND_STATE, 0x8400, sizeof mask);
        nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    }
    clock_t ticks = clock() - start;
    expected.blend.mask[0] &= (u8)~1u;
    int generation_changed = expected.gen[DS_BLEND] != d.ds.gen[DS_BLEND];
    expected.gen[DS_BLEND] = d.ds.gen[DS_BLEND];
    int exact = generation_changed && !memcmp(&d.ds, &expected, sizeof expected);
    check(exact, "changed-state benchmark preserves all other state and advances blending");
    printf("GPU_HOTPATH_BENCH workload=changing-state operations=%u ns=%llu exact=%d\n", operations,
           (unsigned long long)((u64)ticks * 1000000000ull / CLOCKS_PER_SEC), exact);
}

/* Draws often repeat a viewport and vertex layout. Measure that case and a control that changes one viewport.
   The control returns to its initial coordinates and checks every state byte except the changed generation. */
static void geometry_records(void) {
    NvnPViewports views = {.count = 1, .v = {{10, 20, 320, 240}}};
    NvnPScissor scissor = {0, 0, 640, 480};
    NvnPDepthRange depth = {0.125f, 0.875f};
    NvnPBindVAttr attr = {.count = 1, .st = {NVN_FORMAT_RGBA32F}};
    NvnPBindVStream stream = {.count = 1, .st = {{16, 0}}};
    NvnPBindVBuf buffer = {.count = 1, .r = {{0x40000, 4096}}};
    memcpy(GP(0xb000), &views, sizeof views);
    memcpy(GP(0xb200), &scissor, sizeof scissor);
    memcpy(GP(0xb240), &depth, sizeof depth);
    memcpy(GP(0xb280), &attr, sizeof attr);
    memcpy(GP(0xb300), &stream, sizeof stream);
    memcpy(GP(0xb400), &buffer, sizeof buffer);
}
static void apply_geometry_records(void) {
    nvn_draw_record(&d, OP_VIEWPORTS, 0xb000, sizeof(NvnPViewports));
    nvn_draw_record(&d, OP_SCISSOR, 0xb200, sizeof(NvnPScissor));
    nvn_draw_record(&d, OP_DEPTH_RANGE, 0xb240, sizeof(NvnPDepthRange));
    nvn_draw_record(&d, OP_BIND_VATTR, 0xb280, sizeof(NvnPBindVAttr));
    nvn_draw_record(&d, OP_BIND_VSTREAM, 0xb300, sizeof(NvnPBindVStream));
    nvn_draw_record(&d, OP_BIND_VBUF, 0xb400, sizeof(NvnPBindVBuf));
}
static void bench_geometry(void) {
    const u32 operations = 1000000;
    for (u32 change = 0; change < 2; change++) {
        memset(&d, 0, sizeof d);
        nvn_draw_init(&d, 0);
        NvnPDraw draw = {.count = 3, .instances = 1};
        memcpy(GP(DRAW), &draw, sizeof draw);
        geometry_records();
        apply_geometry_records();
        nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
        DsState expected = d.ds;
        clock_t start = clock();
        for (u32 i = 1; i <= operations; i++) {
            if (change) {
                float x = 10.0f + (float)(i & 1u);
                memcpy(GP(0xb000 + offsetof(NvnPViewports, v)), &x, sizeof x);
                nvn_draw_record(&d, OP_VIEWPORTS, 0xb000, sizeof(NvnPViewports));
            } else
                apply_geometry_records();
            nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
        }
        clock_t ticks = clock() - start;
        int generation_changed = expected.gen[DS_VIEWPORTS] != d.ds.gen[DS_VIEWPORTS];
        if (change) expected.gen[DS_VIEWPORTS] = d.ds.gen[DS_VIEWPORTS];
        int exact = !memcmp(&d.ds, &expected, sizeof expected) && (change ? generation_changed : !generation_changed);
        check(exact, "geometry benchmark preserves every state byte and the required generations");
        printf("GPU_HOTPATH_BENCH workload=%s operations=%u ns=%llu exact=%d\n",
               change ? "changing-viewport" : "equal-geometry", operations,
               (unsigned long long)((u64)ticks * 1000000000ull / CLOCKS_PER_SEC), exact);
    }
}

static void test_equal_geometry(void) {
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    NvnPDraw draw = {.count = 3, .instances = 1};
    memcpy(GP(DRAW), &draw, sizeof draw);
    geometry_records();
    apply_geometry_records();
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    DsState expected = d.ds;
    apply_geometry_records();
    check(!d.dirty && !d.vp_dirty, "equal geometry records need no state conversion");
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(!memcmp(&d.ds, &expected, sizeof expected), "equal geometry preserves every state byte and generation");

    NvnPViewports views = {.first = 15, .count = 1, .v = {{20, 30, 40, 50}}};
    memcpy(GP(0xb000), &views, sizeof views);
    nvn_draw_record(&d, OP_VIEWPORTS, 0xb000, sizeof views);
    apply_geometry_records();
    check(d.vp_dirty == (1u << 15), "equal records retain an earlier changed viewport");
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(d.ds.vp.scale[15][0] == 20 && d.ds.vp.translate[15][0] == 40,
          "the pending viewport reaches the next draw");

    NvnPBindVAttr attr = {0};
    NvnPBindVStream stream = {0};
    memcpy(GP(0xb280), &attr, sizeof attr);
    memcpy(GP(0xb300), &stream, sizeof stream);
    nvn_draw_record(&d, OP_BIND_VATTR, 0xb280, sizeof attr);
    nvn_draw_record(&d, OP_BIND_VSTREAM, 0xb300, sizeof stream);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check((d.ds.vfmt.constant & 1) && !d.ds.vfmt.stride[0], "zero counts remove the vertex attribute and stream");
    geometry_records();
    apply_geometry_records();
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(!(d.ds.vfmt.constant & 1) && d.ds.vfmt.stride[0] == 16,
          "larger counts restore retained vertex state bytes");

    u32 origin = __atomic_load_n(&g_nvn.draw.origin, __ATOMIC_RELAXED);
    u32 depth = __atomic_load_n(&g_nvn.draw.depth_mode, __ATOMIC_RELAXED);
    u32 other = origin == NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT ? NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT
                                                         : NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT;
    u64 generation = d.ds.gen[DS_VIEWPORTS];
    nvn_draw_device(other, depth);
    apply_geometry_records();
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(d.ds.vp.scale[0][1] == (other == NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT ? -120 : 120) &&
              d.ds.vp.scale[15][1] == (other == NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT ? -25 : 25) &&
              d.ds.gen[DS_VIEWPORTS] != generation,
          "equal geometry still observes a changed device origin");
    nvn_draw_device(origin, depth);
}

/* Compare incremental conversions with a fresh conversion of all sixteen slots after each kind of update. */
static void test_partial_viewports(void) {
    static NvnDraw reference;
    NvnPViewports views = {.count = 16};
    NvnPScissors scissors = {.count = 16};
    NvnPDepthRanges ranges = {.count = 16};
    NvnPVpSwizzle swizzles = {.count = 16};
    for (u32 i = 0; i < 16; i++) {
        views.v[i][0] = (float)i - 8.25f;
        views.v[i][1] = (float)i + 2.5f;
        views.v[i][2] = 64 + (float)i;
        views.v[i][3] = 128 - (float)i;
        scissors.rect[i][0] = (i32)i - 8;
        scissors.rect[i][1] = (i32)i - 4;
        scissors.rect[i][2] = 128;
        scissors.rect[i][3] = 256;
        ranges.range[i][0] = (float)i / 64;
        ranges.range[i][1] = 1.0f - (float)i / 64;
    }
    NvnPDraw draw = {.count = 3, .instances = 1};
    memcpy(GP(DRAW), &draw, sizeof draw);
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    for (u32 step = 0; step <= 64; step++) {
        u32 slot = step / 4 * 7 % 16;
        if (!step) {
            memcpy(GP(0xc000), &views, sizeof views);
            memcpy(GP(0xc400), &scissors, sizeof scissors);
            memcpy(GP(0xc600), &ranges, sizeof ranges);
            memcpy(GP(0xc700), &swizzles, sizeof swizzles);
            nvn_draw_record(&d, OP_VIEWPORTS, 0xc000, sizeof views);
            nvn_draw_record(&d, OP_SCISSORS, 0xc400, sizeof scissors);
            nvn_draw_record(&d, OP_DEPTH_RANGES, 0xc600, sizeof ranges);
            nvn_draw_record(&d, OP_VP_SWIZZLE, 0xc700, sizeof swizzles);
        } else if (step % 4 == 0) {
            views.v[slot][0] += 0.25f;
            NvnPViewports one = {.first = (i32)slot, .count = 1};
            memcpy(one.v[0], views.v[slot], sizeof one.v[0]);
            memcpy(GP(0xb000), &one, sizeof one);
            nvn_draw_record(&d, OP_VIEWPORTS, 0xb000, sizeof one);
        } else if (step % 4 == 1) {
            scissors.rect[slot][0]++;
            NvnPScissors one = {.first = (i32)slot, .count = 1};
            memcpy(one.rect[0], scissors.rect[slot], sizeof one.rect[0]);
            memcpy(GP(0xb000), &one, sizeof one);
            nvn_draw_record(&d, OP_SCISSORS, 0xb000, sizeof one);
        } else if (step % 4 == 2) {
            ranges.range[slot][0] += 0.01f;
            NvnPDepthRanges one = {.first = (i32)slot, .count = 1};
            memcpy(one.range[0], ranges.range[slot], sizeof one.range[0]);
            memcpy(GP(0xb000), &one, sizeof one);
            nvn_draw_record(&d, OP_DEPTH_RANGES, 0xb000, sizeof one);
        } else {
            swizzles.sw[slot][step / 4 % 4] ^= 1;
            NvnPVpSwizzle one = {.first = (i32)slot, .count = 1};
            memcpy(one.sw[0], swizzles.sw[slot], sizeof one.sw[0]);
            memcpy(GP(0xb000), &one, sizeof one);
            nvn_draw_record(&d, OP_VP_SWIZZLE, 0xb000, sizeof one);
        }
        nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
        memset(&reference, 0, sizeof reference);
        nvn_draw_init(&reference, 0);
        memcpy(GP(0xc000), &views, sizeof views);
        memcpy(GP(0xc400), &scissors, sizeof scissors);
        memcpy(GP(0xc600), &ranges, sizeof ranges);
        memcpy(GP(0xc700), &swizzles, sizeof swizzles);
        nvn_draw_record(&reference, OP_VIEWPORTS, 0xc000, sizeof views);
        nvn_draw_record(&reference, OP_SCISSORS, 0xc400, sizeof scissors);
        nvn_draw_record(&reference, OP_DEPTH_RANGES, 0xc600, sizeof ranges);
        nvn_draw_record(&reference, OP_VP_SWIZZLE, 0xc700, sizeof swizzles);
        nvn_draw_record(&reference, OP_DRAW, DRAW, sizeof draw);
        check(!memcmp(&d.ds.vp, &reference.ds.vp, sizeof d.ds.vp),
              "partial viewport conversion matches a full conversion at step %u", step);
    }
}

/* Compute stores requested UBO sizes in the VPC but rounds the directly bound constant-buffer window to 256 bytes. */
static void test_compute_uniform_ranges(void) {
    static const u32 sizes[] = {0, 1, 255, 256, 257, 65535, 65536};
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    for (u32 index = 0; index < 2; index++)
        for (u32 i = 0; i < sizeof sizes / sizeof *sizes; i++) {
            NvnPBindBuf r = {.stage = NVN_SHADER_STAGE_COMPUTE,
                             .index = (i32)index,
                             .address = 0x4a0010000ull + 0x1000 * index,
                             .size = sizes[i]};
            memcpy(GP(SSBO), &r, sizeof r);
            check(nvn_draw_record(&d, OP_BIND_UBO, SSBO, sizeof r), "compute UBO bind accepted");
            const u8* vpc = d.dx.bank0 + 0x210 + index * 16;
            u32 rounded = (sizes[i] + 255u) & ~255u;
            check(snx_ld_le64(vpc) == r.address && snx_ld_le32(vpc + 8) == sizes[i],
                  "compute UBO %u keeps requested address and size in the VPC", index);
            check(d.dx.cb_addr[3 + index] == r.address && d.dx.cb_size[3 + index] == (rounded == 65536 ? 0 : rounded),
                  "compute UBO %u binds its rounded direct constant-buffer size", index);
            check(!!(d.dx.cb_on & (1u << (3 + index))) == !!sizes[i], "compute UBO zero size disables its direct slot");
        }
}

static u32 instance_draws, captured_instances;
static u64 captured_indirect;
static void capture_instance_draw(const DsState* ds, const DrawCall* call, DrawOwner* owner) {
    (void)ds;
    (void)owner;
    instance_draws++;
    captured_instances = call->instances;
    captured_indirect = call->indirect;
}
/* A zero-instance draw has no raster or shader work. Indirect draws keep their counts on the GPU. */
static void test_direct_instances(void) {
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    instance_draws = 0;
    g_none.draw = capture_instance_draw;
    NvnPDraw a = {.count = 3};
    memcpy(GP(DRAW), &a, sizeof a);
    check(nvn_draw_record(&d, OP_DRAW, DRAW, sizeof a) && instance_draws == 0,
          "zero-instance arrays emit no renderer draw");
    a.instances = 1;
    memcpy(GP(DRAW), &a, sizeof a);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof a);
    check(instance_draws == 1 && captured_instances == 1, "one-instance arrays emit one renderer draw");
    a.instances = 4;
    memcpy(GP(DRAW), &a, sizeof a);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof a);
    check(instance_draws == 2 && captured_instances == 4, "array draws preserve their instance count");
    NvnPDrawIdx b = {.count = 3, .index_type = NVN_INDEX_TYPE_UNSIGNED_SHORT, .index_buffer = 0x70000};
    memcpy(GP(DRAW), &b, sizeof b);
    check(nvn_draw_record(&d, OP_DRAW_IDX, DRAW, sizeof b) && instance_draws == 2,
          "zero-instance indexed draws emit no renderer draw");
    b.instances = 2;
    memcpy(GP(DRAW), &b, sizeof b);
    nvn_draw_record(&d, OP_DRAW_IDX, DRAW, sizeof b);
    check(instance_draws == 3 && captured_instances == 2, "indexed draws preserve their instance count");
    a.instances = 0;
    a.ind = 0x90000;
    memcpy(GP(DRAW), &a, sizeof a);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof a);
    check(instance_draws == 4 && captured_indirect == a.ind, "indirect arrays retain GPU-owned instance arguments");
    b.instances = 0;
    b.ind = 0x90020;
    memcpy(GP(DRAW), &b, sizeof b);
    nvn_draw_record(&d, OP_DRAW_IDX, DRAW, sizeof b);
    check(instance_draws == 5 && captured_indirect == b.ind, "indirect indexed draws retain GPU-owned arguments");
    g_none.draw = NULL;
}

static void test_patch_size(void) {
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    NvnPDraw draw = {.count = 3, .instances = 1};
    memcpy(GP(DRAW), &draw, sizeof draw);
    static const u32 sizes[] = {1, 3, 32, 3};
    for (u32 i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        snx_st_le32(GP(SSBO), sizes[i]);
        check(nvn_draw_record(&d, OP_PATCH_SIZE, SSBO, 4), "patch-size record accepted");
        nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
        check(d.ds.rkey.patch_vertices == sizes[i], "patch size %u reaches the renderer key", sizes[i]);
        u64 generation = d.ds.gen[DS_RASTER_KEY];
        nvn_draw_record(&d, OP_PATCH_SIZE, SSBO, 4);
        nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
        check(d.ds.gen[DS_RASTER_KEY] == generation, "equal patch size keeps the raster generation");
    }
}

static DrawCall fixed_call;
static u32 fixed_draws, fixed_controls, fixed_clears;
static DrawClear fixed_clear;
static u32 fixed_control_owner, fixed_control_what, fixed_control_stride;
static u64 fixed_control_address;
static void capture_fixed_draw(const DsState* state, const DrawCall* call, DrawOwner* owner) {
    (void)state;
    (void)owner;
    fixed_call = *call;
    fixed_draws++;
}
static int capture_fixed_control(u32 owner_id, u64 control, u32 what, u32 stride, DrawOwner* owner) {
    (void)owner;
    fixed_control_owner = owner_id;
    fixed_control_address = control;
    fixed_control_what = what;
    fixed_control_stride = stride;
    fixed_controls++;
    return 1;
}
static void capture_fixed_clear(const DsState* state, const DrawClear* clear, const DrawOwner* owner) {
    (void)state;
    (void)owner;
    fixed_clear = *clear;
    fixed_clears++;
}
static void test_fixed_state(void) {
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0x80000022u);
    g_none.draw = capture_fixed_draw;
    g_none.xfb_control = capture_fixed_control;
    g_none.clear = capture_fixed_clear;
    NvnPDraw draw = {.count = 3, .instances = 1};
    memcpy(GP(DRAW), &draw, sizeof draw);
    NvnPBindState color = {.kind = 1};
    u32 word = (u32)nvn_set_field(d.color, NVN_COLOR_ALPHA_FUNC, NVN_ALPHA_FUNC_GREATER);
    memcpy(color.bytes, &word, sizeof word);
    memcpy(GP(0xb000), &color, sizeof color);
    nvn_draw_record(&d, OP_BIND_STATE, 0xb000, sizeof color);
    NvnPSetState state = {.kind = NVN_SET_ALPHA_REF, .value = {0.375f}};
    memcpy(GP(0xb100), &state, sizeof state);
    nvn_draw_record(&d, OP_SET_STATE, 0xb100, sizeof state);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(d.ds.dyn.alpha_test && d.ds.dyn.alpha_func == DS_CMP_GREATER && d.ds.dyn.alpha_ref == 0.375f,
          "the recorded alpha reference reaches an enabled alpha test");
    u64 generation = d.ds.gen[DS_DYNAMIC];
    nvn_draw_record(&d, OP_SET_STATE, 0xb100, sizeof state);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(d.ds.gen[DS_DYNAMIC] == generation, "an equal alpha reference retains its generation");
    const NvnPSetState cases[] = {{.kind = NVN_SET_DEPTH_BOUNDS, .enable = 1, .value = {0.2f, 0.8f}},
                                  {.kind = NVN_SET_POINT_SIZE, .value = {4.0f}},
                                  {.kind = NVN_SET_TESS_INNER, .value = {2.0f, 3.0f}},
                                  {.kind = NVN_SET_TESS_OUTER, .value = {4.0f, 5.0f, 6.0f, 7.0f}}};
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) {
        memcpy(GP(0xb100), &cases[i], sizeof cases[i]);
        nvn_draw_record(&d, OP_SET_STATE, 0xb100, sizeof cases[i]);
    }
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(fixed_call.state_flags == (DS_DRAW_DEPTH_BOUNDS | DS_DRAW_POINT_SIZE | DS_DRAW_TESS_LEVELS) &&
              fixed_call.point_size == 4.0f && fixed_call.depth_bounds[0] == 0.2f &&
              fixed_call.depth_bounds[1] == 0.8f &&
              !memcmp(fixed_call.tess_levels, (const f32[6]){4, 5, 6, 7, 2, 3}, sizeof fixed_call.tess_levels),
          "depth bounds, fixed point size and independent tessellation levels accompany the draw");
    check(d.ds.gen[DS_DYNAMIC] == generation, "draw-only state preserves the serialized dynamic-state ABI");
    state = (NvnPSetState){.kind = NVN_SET_DEPTH_BOUNDS};
    memcpy(GP(0xb100), &state, sizeof state);
    nvn_draw_record(&d, OP_SET_STATE, 0xb100, sizeof state);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(!(fixed_call.state_flags & DS_DRAW_DEPTH_BOUNDS) && d.depth_bounds[0] == 0.2f && d.depth_bounds[1] == 0.8f,
          "disabling depth bounds preserves its last range without enabling the test");
    state = (NvnPSetState){.kind = NVN_SET_COVERAGE_TABLE,
                           .enable = 1,
                           .value = {-1, 0, 0.03124f, 0.03125f, 0.06249f, 0.0625f, 0.09374f, 0.09375f, 0.25f, 0.5f,
                                     0.75f, 0.9375f, 0.96874f, 0.96875f, 1, 2}};
    const u8 modulation[16] = {0, 0, 0, 1, 1, 1, 1, 2, 4, 8, 12, 15, 15, 16, 16, 16};
    memcpy(GP(0xb100), &state, sizeof state);
    nvn_draw_record(&d, OP_SET_STATE, 0xb100, sizeof state);
    d.ms.samples = 1;
    d.ms.raster_samples = 4;
    d.ms.coverage_mod = 3;
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(fixed_call.raster_samples == 4 && fixed_call.coverage_mode == 3 && fixed_call.coverage_table_enable &&
              !memcmp(fixed_call.coverage_table, modulation, sizeof modulation),
          "coverage tables clamp and round half steps to inclusive1/16 coefficients");
    state.enable = 0;
    memcpy(GP(0xb100), &state, sizeof state);
    nvn_draw_record(&d, OP_SET_STATE, 0xb100, offsetof(NvnPSetState, value));
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(!fixed_call.coverage_table_enable && fixed_call.raster_samples == 4 && fixed_call.coverage_mode == 3,
          "a NULL coverage table selects linear modulation without changing the multisample mode");
    d.ms.samples = d.ms.raster_samples = 4;
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(fixed_call.raster_samples == 4 && !fixed_call.coverage_table_enable,
          "equal raster and color counts retain explicit target-independent state");
    d.coverage_table_enable = 1;
    d.coverage_table[0] = 5;
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(fixed_call.raster_samples == 4 && fixed_call.coverage_mode == 3 &&
              fixed_call.coverage_table_enable && fixed_call.coverage_table[0] == 5,
          "ratio-one coverage retains its custom coefficient");
    d.ms.flags = NVN_MS_GRID;
    d.ms.cov_to_color_out = 7;
    memset(d.ms.location, 0x12, sizeof d.ms.location);
    NvnPConservative precision = {.which = 2, .on = 3, .dilate = 8};
    memcpy(GP(0xb200), &precision, sizeof precision);
    nvn_draw_record(&d, OP_CONSERVATIVE, 0xb200, sizeof precision);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(!fixed_call.multisample_flags && !fixed_call.coverage_to_color_output &&
              !fixed_call.subpixel_bias_x && !fixed_call.subpixel_bias_y &&
              !memcmp(fixed_call.sample_locations, (const u8[16]){0}, sizeof fixed_call.sample_locations),
          "disabled sample features and conservative rasterization keep dormant state out of draws");
    d.conservative = 1;
    d.ms.flags = NVN_MS_A2C_DITHER | NVN_MS_COV_TO_COLOR | NVN_MS_LOCATIONS | NVN_MS_GRID;
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(fixed_call.multisample_flags ==
                  (DS_MS_A2C_DITHER | DS_MS_COVERAGE_TO_COLOR | DS_MS_SAMPLE_LOCATIONS | DS_MS_SAMPLE_GRID) &&
              fixed_call.coverage_to_color_output == 7 && fixed_call.subpixel_bias_x == 3 &&
              fixed_call.subpixel_bias_y == 8 &&
              !memcmp(fixed_call.sample_locations, d.ms.location, sizeof fixed_call.sample_locations),
          "active sample and conservative precision state reaches direct draws");
    precision.on = -1;
    memcpy(GP(0xb200), &precision, sizeof precision);
    nvn_draw_record(&d, OP_CONSERVATIVE, 0xb200, sizeof precision);
    check(d.subpixel_bias_x == 3 && d.subpixel_bias_y == 8, "invalid replay precision values preserve state");
    d.xfb.stride[0] = 20;
    for (u32 what = 0; what < 4; what++) {
        NvnPTfState tf = {.what = (i32)what, .control = what < 2 ? 0x70000 : 0};
        memcpy(GP(0xb200), &tf, sizeof tf);
        nvn_draw_record(&d, OP_TF_STATE, 0xb200, sizeof tf);
        check(fixed_control_owner == 0x80000022u && fixed_control_address == tf.control && fixed_control_what == what &&
                  fixed_control_stride == 20 && fixed_controls == what + 1,
              "transform-feedback control %u forwards owner, address, operation and stride", what);
    }
    NvnPTfState tf = {.what = 4, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .control = 0x70000};
    memcpy(GP(0xb200), &tf, sizeof tf);
    nvn_draw_record(&d, OP_TF_STATE, 0xb200, sizeof tf);
    check(fixed_call.xfb_control == tf.control && fixed_call.topology == NVN_DRAW_PRIMITIVE_TRIANGLES &&
              fixed_call.point_size == 4.0f,
          "transform-feedback draws keep the GPU control address and fixed state");
    u32 draws = fixed_draws;
    tf.control = 0;
    memcpy(GP(0xb200), &tf, sizeof tf);
    nvn_draw_record(&d, OP_TF_STATE, 0xb200, sizeof tf);
    check(fixed_draws == draws, "transform-feedback draw with no control emits no draw");
    const u32 masks[] = {0, 0x0f, 0xff, 0x100};
    for (u32 i = 0; i < sizeof masks / sizeof *masks; i++) {
        NvnPClearDS clear = {.depth = 0.5f, .depth_mask = 1, .stencil = 0x35, .stencil_mask = (i32)masks[i]};
        memcpy(GP(0xb300), &clear, sizeof clear);
        nvn_draw_record(&d, OP_CLEAR_DS, 0xb300, sizeof clear);
        check(fixed_clears == i + 1 && fixed_clear.aspects == (masks[i] & 255 ? 3u : 1u) &&
                  fixed_clear.stencil_preserve == ((~masks[i]) & 255) && fixed_clear.depth == 0.5f,
              "depth/stencil clear mask %#x forwards only the low stencil bits", masks[i]);
    }
    g_none.draw = NULL;
    g_none.xfb_control = NULL;
    g_none.clear = NULL;
}

static void test_channel_masks(void) {
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    NvnPDraw draw = {.count = 3, .instances = 1};
    memcpy(GP(DRAW), &draw, sizeof draw);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    static const u32 masks[] = {0, 0xffffffffu, 0x76543210, 0x89abcdef, 0x13579bdf};
    for (u32 i = 0; i < sizeof masks / sizeof *masks; i++) {
        DsState expected = d.ds;
        NvnPBindState state = {.kind = 2};
        memcpy(state.bytes, &masks[i], sizeof masks[i]);
        memcpy(GP(0x8400), &state, sizeof state);
        nvn_draw_record(&d, OP_BIND_STATE, 0x8400, sizeof state);
        nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
        for (u32 target = 0; target < DS_TARGETS_MAX; target++)
            expected.blend.mask[target] = (u8)((masks[i] >> (4 * target)) & 15);
        check(expected.gen[DS_BLEND] != d.ds.gen[DS_BLEND], "changed channel masks advance the blend generation");
        expected.gen[DS_BLEND] = d.ds.gen[DS_BLEND];
        check(!memcmp(&expected, &d.ds, sizeof expected),
              "all target channel masks change without altering other state");
    }
    for (u32 order = 0; order < 2; order++) {
        DsState expected = d.ds;
        NvnPBindState state[2] = {{.kind = 2}, {.kind = 0}};
        u32 mask = order ? 0xabcdef01 : 0x12345678;
        u64 blend = nvn_set_field(d.blend[0], NVN_BLEND_DST_RGB, order ? NVN_BLEND_FUNC_ZERO : NVN_BLEND_FUNC_ONE);
        memcpy(state[0].bytes, &mask, sizeof mask);
        memcpy(state[1].bytes, &blend, sizeof blend);
        for (u32 i = 0; i < 2; i++) {
            memcpy(GP(0x8400), &state[i ^ order], sizeof state[0]);
            nvn_draw_record(&d, OP_BIND_STATE, 0x8400, sizeof state[0]);
        }
        nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
        expected.blend.dc[0] = order ? DS_FACTOR_ZERO : DS_FACTOR_ONE;
        for (u32 target = 0; target < DS_TARGETS_MAX; target++)
            expected.blend.mask[target] = (u8)((mask >> (4 * target)) & 15);
        expected.gen[DS_BLEND] = d.ds.gen[DS_BLEND];
        check(!memcmp(&expected, &d.ds, sizeof expected),
              "mask and blend-factor updates compose in either record order");
    }
}

static void test_indexed_viewports(void) {
    memset(&d, 0, sizeof d);
    nvn_draw_device(NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT, NVN_DEPTH_MODE_NEAR_IS_MINUS_W);
    nvn_draw_init(&d, 0);
    NvnPDraw draw = {.count = 3, .instances = 1};
    memcpy(GP(DRAW), &draw, sizeof draw);
    NvnPViewports views = {.first = 3, .count = 2};
    const float rects[2][4] = {{-1.25f, 2.75f, 4.5f, 3.5f}, {65534.5f, -2.5f, 7, 5}};
    for (u32 i = 0; i < 2; i++) memcpy(views.v[i], rects[i], sizeof rects[i]);
    memcpy(GP(0xa000), &views, sizeof views);
    nvn_draw_record(&d, OP_VIEWPORTS, 0xa000, sizeof views);
    NvnPScissors scissors = {.first = 3, .count = 2, .rect = {{-2, 3, 8, 9}, {65534, -5, 20, 8}}};
    memcpy(GP(0xa200), &scissors, sizeof scissors);
    nvn_draw_record(&d, OP_SCISSORS, 0xa200, sizeof scissors);
    NvnPDepthRanges ranges = {.first = 3, .count = 2, .range = {{0.25f, 0.75f}, {0.8f, 0.2f}}};
    memcpy(GP(0xa400), &ranges, sizeof ranges);
    nvn_draw_record(&d, OP_DEPTH_RANGES, 0xa400, sizeof ranges);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(d.ds.vp.scissor_on == 0xffff, "each viewport has its own enabled scissor");
    check(d.ds.vp.scissor_x[0] == 0xffff0000u && d.ds.vp.z_near[0] == 0 && d.ds.vp.z_far[0] == 1,
          "indexed setters preserve viewport zero defaults");
    check(d.ds.vp.scissor_x[3] == 6u << 16 && d.ds.vp.scissor_y[3] == (3u | 12u << 16),
          "nonzero first scissor intersects negative coordinates correctly");
    check(d.ds.vp.scissor_x[4] == (65534u | 65535u << 16) && d.ds.vp.scissor_y[4] == 3u << 16,
          "scissor endpoints clip to the hardware window");
    check(d.ds.vp.clip_x[3] == 4u << 16 && d.ds.vp.clip_y[3] == (2u | 5u << 16),
          "fractional viewport envelope uses floor and ceil before clipping");
    check(d.ds.vp.clip_x[4] == (65534u | 1u << 16) && d.ds.vp.clip_y[4] == 3u << 16,
          "viewport envelope intersects both hardware edges");
    check(d.ds.vp.z_near[3] == 0.25f && d.ds.vp.z_far[3] == 0.75f && d.ds.vp.scale[3][2] == 0.25f &&
              d.ds.vp.translate[3][2] == 0.5f && d.ds.vp.z_near[4] == 0.8f && d.ds.vp.z_far[4] == 0.2f,
          "independent and reversed depth ranges reach their selected viewport");
    u64 generation = d.ds.gen[DS_VIEWPORTS];
    nvn_draw_record(&d, OP_SCISSORS, 0xa200, sizeof scissors);
    nvn_draw_record(&d, OP_DEPTH_RANGES, 0xa400, sizeof ranges);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(d.ds.gen[DS_VIEWPORTS] == generation, "repeated equal indexed state retains the viewport generation");
    ranges.first = 15;
    ranges.count = 1;
    ranges.range[0][0] = 0.125f;
    ranges.range[0][1] = 0.875f;
    memcpy(GP(0xa400), &ranges, sizeof ranges);
    nvn_draw_record(&d, OP_DEPTH_RANGES, 0xa400, sizeof ranges);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(d.ds.vp.z_near[15] == 0.125f && d.ds.vp.z_far[15] == 0.875f && d.ds.vp.z_near[3] == 0.25f,
          "the last viewport updates without changing earlier slots");
    NvnPDepthRange single = {0.1f, 0.9f};
    memcpy(GP(0xa500), &single, sizeof single);
    nvn_draw_record(&d, OP_DEPTH_RANGE, 0xa500, sizeof single);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(d.ds.vp.z_near[0] == single.n && d.ds.vp.z_far[0] == single.f && d.ds.vp.z_near[3] == 0.25f,
          "a single setter changes only slot zero");
    nvn_draw_device(NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT, NVN_DEPTH_MODE_NEAR_IS_ZERO);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(d.ds.vp.scale[3][2] == 0.5f && d.ds.vp.translate[3][2] == 0.25f,
          "device depth convention refreshes every indexed depth range");
    views.first = 8;
    views.count = 1;
    views.v[0][0] = 100.0f;
    views.v[0][2] = 0.000001f;
    memcpy(GP(0xa000), &views, sizeof views);
    nvn_draw_record(&d, OP_VIEWPORTS, 0xa000, sizeof views);
    nvn_draw_record(&d, OP_DRAW, DRAW, sizeof draw);
    check(d.ds.vp.clip_x[8] == 0, "viewport endpoints add in guest float precision before rounding");
}

static int barrier_kinds[3], barrier_count;
static void capture_barrier(int kind) {
    if (barrier_count < 3) barrier_kinds[barrier_count] = kind;
    barrier_count++;
}
static void test_barriers(void) {
    NvnDraw draw;
    nvn_draw_init(&draw, 0x80000000u);
    g_none.method_barrier = capture_barrier;
    for (int mode = 0; mode <= 2; mode++)
        for (u32 bits = 0; bits < 256; bits++) {
            g_settings.nvn_storage_ordering = mode;
            snx_st_le32(GP(0x9000), bits);
            barrier_count = 0;
            check(nvn_draw_record(&draw, OP_BARRIER, 0x9000, 4) == 1, "barrier record accepted");
            int n = 0;
            if (mode && (bits & 15u))
                check(barrier_kinds[n++] == 6, "execution order precedes shader invalidation");
            else if (mode && (bits & 32u))
                check(barrier_kinds[n++] == 7, "shader-data visibility precedes invalidation");
            if (bits & 32u) check(barrier_kinds[n++] == 5, "shader instructions are invalidated");
            if (bits & 18u) check(barrier_kinds[n++] == 0, "upstream fragment and texture ordering is retained");
            check(barrier_count == n, "barrier flags select only their required operations");
        }
    g_none.method_barrier = NULL;
}

/* a compressed texture bound as a render target (one game encodes its BC7 textures' levels so): an integer target
   whose texels are its blocks, 16 bytes RGBA32UI and 8 RG32UI, as many across and down as the level has blocks */
#define TEXSNAP 0x4000u
static void test_compressed_targets(void) {
    static const struct {
        u32 format, rt, w, h;
    } cases[] = {{NVN_FORMAT_BPTC_UNORM_SRGB, 0xC2, 16, 16},
                 {NVN_FORMAT_RGBA_DXT1, 0xCD, 16, 16},
                 {NVN_FORMAT_RGBA8, 0xD5, 64, 64}};
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) {
        NvnTexture t = {0};
        t.magic = NVN_MAGIC('T', 'E', 'X', 0);
        t.gpu = 0x100000;
        t.d.format = (u16)cases[i].format;
        t.d.target = NVN_TEXTURE_TARGET_2D;
        t.d.width = t.d.height = 256;
        t.d.depth = 1;
        t.d.levels = 9;
        memcpy(g_mem + TEXSNAP, &t, NVN_TEX_SNAP);
        NvnView v = {0};
        v.set = NVN_VIEW_LEVELS;
        v.base_level = 2;
        DsSurf ds;
        int depth = 1;
        u32 msaa;
        check(nvn_draw_surface(TEXSNAP, &v, &ds, &depth, &msaa) && ds.format == cases[i].rt && !depth &&
                  ds.width == cases[i].w && ds.height == cases[i].h,
              "format %#x as a target: %#x, %ux%u", cases[i].format, ds.format, ds.width, ds.height);
    }
}

static void test_depth_sample_layouts(void) {
    NvnDraw draw;
    nvn_draw_init(&draw, 7);
    NvnPDraw call = {.count = 3, .instances = 1};
    memcpy(GP(0x5800), &call, sizeof call);
    const u32 color_samples[] = {1, 4, 4, 0}, depth_samples[] = {4, 1, 4, 4};
    for (u32 test = 0; test < 4; test++) {
        NvnTexture color = {.magic = NVN_MAGIC('T', 'E', 'X', 0), .gpu = 0x100000};
        color.d.format = NVN_FORMAT_RGBA8;
        color.d.target = color_samples[test] > 1 ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE : NVN_TEXTURE_TARGET_2D;
        color.d.width = 8; color.d.height = 6; color.d.depth = color.d.levels = 1;
        color.d.samples = (u8)color_samples[test];
        NvnTexture depth = color;
        depth.gpu = 0x200000;
        depth.d.format = NVN_FORMAT_DEPTH32F;
        depth.d.target = depth_samples[test] > 1 ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE : NVN_TEXTURE_TARGET_2D;
        depth.d.samples = (u8)depth_samples[test];
        memcpy(GP(0x4000), &color, NVN_TEX_SNAP);
        memcpy(GP(0x4800), &depth, NVN_TEX_SNAP);
        NvnPSetRT targets = {.count = color_samples[test] != 0, .color = {0x4000}, .depth = 0x4800};
        memcpy(GP(0x5000), &targets, sizeof targets);
        nvn_draw_record(&draw, OP_SET_RT, 0x5000, sizeof targets);
        nvn_draw_record(&draw, OP_DRAW, 0x5800, sizeof call);
        u32 color_mode = nvn_tex_msaa((int)(color_samples[test] ? color_samples[test] : depth_samples[test]));
        u32 depth_mode = nvn_tex_msaa((int)depth_samples[test]);
        check(draw.ds.tgt.msaa == color_mode && ds_depth_msaa(&draw.ds.tgt) == depth_mode &&
                  draw.ds.tgt.depth_msaa == (color_mode == depth_mode ? 0 : depth_mode + 1),
              "color/depth sample layouts retain their independent counts, case %u", test);
        check(draw.ds.tgt.depth.gva == depth.gpu &&
                  draw.ds.tgt.depth.width == 8u << ds_msaa_log2_x(depth_mode) &&
                  draw.ds.tgt.depth.height == 6u << ds_msaa_log2_y(depth_mode),
              "depth dimensions retain their own stored sample grid, case %u", test);
        check(draw.ds.dyn.clip_height == 6, "lower-left clip height counts pixels, case %u", test);
    }
    DsTargets inherited = {.msaa = DS_MSAA_2X2};
    check(ds_depth_msaa(&inherited) == DS_MSAA_2X2 && offsetof(DsTargets, color) == 8 &&
              sizeof(DsTargets) == 8 + 9 * sizeof(DsSurf),
          "zero depth-layout sentinel retains legacy construction and target ABI offsets");
}

/* the samples-passed counter runs on the console from the start: a report with no ResetCounter before it (one game
   reports at an address and 16 bytes on, around a depth pass, and draws its colour only when the two differ) turns
   the renderer's counter on and resets it once, then reports the count; a later report only reports */
#define SAMPLES_OWNER 0x80000001u
static struct {
    NvnDraw q; /* big: not on the stack */
    u32 on, enables, resets, reports;
    u64 at;
} g_samples;
static void count_samples_enable(u32 owner, int on) {
    check(owner == SAMPLES_OWNER, "samples_enable for owner %#x, not the queue's %#x", owner, SAMPLES_OWNER);
    g_samples.on = (u32)on;
    g_samples.enables++;
}
static void count_samples_reset(u32 owner) {
    check(owner == SAMPLES_OWNER, "samples_reset for owner %#x, not the queue's %#x", owner, SAMPLES_OWNER);
    g_samples.resets++;
}
static int count_samples_report(u32 owner, u64 gva, u32 len, u64 ticks) {
    check(owner == SAMPLES_OWNER && len == 16, "samples_report for owner %#x of %u bytes", owner, len);
    (void)ticks;
    g_samples.at = gva;
    g_samples.reports++;
    return 1;
}
static void test_samples_without_reset(void) {
    NvnDraw* q = &g_samples.q; /* all zero, as nvn_draw_init wants it */
    nvn_draw_init(q, SAMPLES_OWNER);
    g_none.samples_enable = count_samples_enable;
    g_none.samples_reset = count_samples_reset;
    g_none.samples_report = count_samples_report;
    int first = nvn_draw_samples(q, 0x5000, 7);
    check(first == 1 && g_samples.on && g_samples.enables == 1 && g_samples.resets == 1 && g_samples.reports == 1 &&
              g_samples.at == 0x5000,
          "a report before any reset: returned %d, counter on %u, %u enables, %u resets, %u reports, at %#llx", first,
          g_samples.on, g_samples.enables, g_samples.resets, g_samples.reports, (unsigned long long)g_samples.at);
    int second = nvn_draw_samples(q, 0x5010, 8);
    check(second == 1 && g_samples.enables == 1 && g_samples.resets == 1 && g_samples.reports == 2 &&
              g_samples.at == 0x5010,
          "the next report: returned %d, %u enables, %u resets, %u reports, at %#llx", second, g_samples.enables,
          g_samples.resets, g_samples.reports, (unsigned long long)g_samples.at);
    nvn_draw_free(q);
    check(!g_samples.on, "the queue's end turns its counter off");
    g_none.samples_enable = NULL;
    g_none.samples_reset = NULL;
    g_none.samples_report = NULL;
}

int main(int argc, char** argv) {
    nvn_init();
    test_samples_without_reset();
    test_compressed_targets();
    test_depth_sample_layouts();
    test_init();
    test_barriers();
    test_bind_program();
    test_window_origin();
    test_stencil_faces();
    test_vertex_formats();
    test_storage_tables();
    test_compute_uniform_ranges();
    test_direct_instances();
    test_patch_size();
    test_inline_updates();
    test_equal_bindings();
    test_equal_geometry();
    test_partial_viewports();
    test_fixed_state();
    test_channel_masks();
    test_indexed_viewports();
    test_depth_mode_equal_bind();
    if (argc == 2 && !strcmp(argv[1], "--bench")) bench_inline_updates();
    if (argc == 2 && !strcmp(argv[1], "--bench-state")) bench_equal_bindings();
    if (argc == 2 && !strcmp(argv[1], "--bench-geometry")) bench_geometry();
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    printf("nvn_draw_banks: all checks pass\n");
    return 0;
}
