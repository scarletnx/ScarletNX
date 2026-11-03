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
#include "common/filesystem/host.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 memory[1 << 20];
u8* g_mem = memory; /* guest address a is memory + a */

/* ---- what the file needs of the runtime: stubs, none of them reached by the cases below */
Settings g_settings;
static Renderer g_none; /* a renderer with no draw: a draw only builds the state */
const Renderer* g_renderer = &g_none;
void pm_init(PMutex* m) { (void)m; }
int g_shader_sm70; /* shader.c's: Maxwell programs */
const char* g_shader_sm70_dump; /* shader.c's: no dumps */
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
/* the texture layout and formats: only the render target records use them */
u32 nvn_rt_format(u32 format, int* depth) {
    (void)format;
    (void)depth;
    return 0;
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
u32 nvn_tex_msaa(int samples) {
    (void)samples;
    return 0;
}
int nvn_fmt_bpb(int format, int* bw, int* bh) {
    (void)format;
    *bw = *bh = 1;
    return 0;
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

int main(void) {
    nvn_init();
    test_init();
    test_bind_program();
    test_window_origin();
    test_stencil_faces();
    test_vertex_formats();
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    printf("nvn_draw_banks: all checks pass\n");
    return 0;
}
