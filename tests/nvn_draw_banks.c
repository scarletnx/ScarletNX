/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's constant banks and point sprite origin, which the game's driver's must match (nvn_verify):
   nvn_draw_init's bank 0 point size and bank 2 words, a program's bank 1 and the sprite origin of a draw's dynamic
   state. The records go in through nvn_draw_record */
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/nvn/nvn_verify.h"
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
Settings g_settings;    /* nvn_verify off */
static Renderer g_none; /* a renderer with no draw: a draw only builds the state */
const Renderer* g_renderer = &g_none;
void pm_init(PMutex* m) { (void)m; }
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
void nvn_verify_live_draw(const DsState* ds, const DrawCall* dc) {
    (void)ds;
    (void)dc;
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

/* a draw's dynamic state, built by sync on the draw: the window origin the driver writes (the flip when one of a
   lower left origin and near = -w is set but not both) and its sprite origin bit, set in all four */
#define DRAW 0x2000u
static void draw_with(u32 origin, u32 depth_mode) {
    NvnPDraw r;
    memset(&r, 0, sizeof r);
    r.count = 3;
    memcpy(g_mem + DRAW, &r, sizeof r);
    memset(&d, 0, sizeof d);
    nvn_draw_init(&d, 0);
    nvn_draw_device(origin, depth_mode);
    check(nvn_draw_record(&d, OP_DRAW, DRAW, sizeof r) == 1, "OP_DRAW was not taken");
}
/* the drivers of NVN 53.105 on flip for a lower left origin alone, the 53.13 one for one of it and near = -w */
static void test_sprite_origin(void) {
    static const struct {
        u32 api, origin, depth_mode, window;
        const char* what;
    } cases[] = {
        {55u << 16 | 15, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT, NVN_DEPTH_MODE_NEAR_IS_MINUS_W, 0,
         "55.15 upper left, near = -w"},
        {55u << 16 | 15, NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT, NVN_DEPTH_MODE_NEAR_IS_MINUS_W, DS_ORIGIN_FLIP_Y,
         "55.15 lower left, near = -w"},
        {53u << 16 | 105, NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT, NVN_DEPTH_MODE_NEAR_IS_MINUS_W, DS_ORIGIN_FLIP_Y,
         "53.105 lower left, near = -w"},
        {55u << 16 | 15, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT, NVN_DEPTH_MODE_NEAR_IS_ZERO, 0,
         "55.15 upper left, near = 0"},
        {55u << 16 | 15, NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT, NVN_DEPTH_MODE_NEAR_IS_ZERO, DS_ORIGIN_FLIP_Y,
         "55.15 lower left, near = 0"},
        {53u << 16 | 13, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT, NVN_DEPTH_MODE_NEAR_IS_MINUS_W, DS_ORIGIN_FLIP_Y,
         "53.13 upper left, near = -w"},
        {53u << 16 | 13, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT, NVN_DEPTH_MODE_NEAR_IS_ZERO, 0,
         "53.13 upper left, near = 0"},
        {53u << 16 | 13, NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT, NVN_DEPTH_MODE_NEAR_IS_ZERO, DS_ORIGIN_FLIP_Y,
         "53.13 lower left, near = 0"},
    };
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) {
        g_nvn.direct.api = cases[i].api;
        draw_with(cases[i].origin, cases[i].depth_mode);
        check(d.ds.dyn.window_origin == cases[i].window, "%s window_origin: expected %#x, found %#x", cases[i].what,
              cases[i].window, d.ds.dyn.window_origin);
        check(d.ds.dyn.sprite_origin == 1, "%s sprite_origin: expected 1, found %u", cases[i].what,
              d.ds.dyn.sprite_origin);
    }
}

int main(void) {
    nvn_init();
    test_init();
    test_bind_program();
    test_sprite_origin();
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    printf("nvn_draw_banks: all checks pass\n");
    return 0;
}
