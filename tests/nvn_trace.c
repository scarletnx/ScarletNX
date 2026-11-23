/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu_hook_trace (gpu/nvn/nvn_trace.c) over synthetic NVN records, with no device:
   - a program's hash is the FNV-1a of its bytes, and none (0) for one whose first page is unmapped.
   - off, or before gpu_hook_trace_from presents went by, it isn't live (the replay then calls nothing of it), and it
     goes off again once its lines are used up.
   - each present marked while it logs, each draw with its fragment program, target and viewport and the words of a
     constant buffer (as before when they are), each dispatch with its program, call site in main, grid, block and the
     image its packed c[0] handle names (nvn_trace_image), the records between that aren't binds or barriers, and the
     waits.
   - a line past the log's 2048 bytes goes on in a line of its own */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/utilities/endian.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/gpu/nvn/nvn_trace.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define MAIN      0x10000ull
#define TONEMAP   0x24000ull /* a draw's fragment program */
#define OUTPASS   0x26000ull /* a dispatch's compute program */
#define HASHED    0x27000ull /* a program whose hash is checked byte by byte */
#define RECORDS   0x30000ull /* the dispatch's record */
#define CAMERA    0xeff00ull /* a constant buffer of the draw's */
#define WORDS     0xe0000ull /* 128 words of another */
#define TEX_POOL  0xf0000ull /* the texture pool, 16 entries */
#define OUTPUT_RT 0x800000ull
enum { OW = 128, OH = 72, HX_HANDLE_OFF = 0x40 };

/* ---- the guest: its memory and its main module */
static u8 memory[1 << 20];
u8* g_mem = memory;
static const SnxModInfo g_mods[1] = {{.name = "main", .base = MAIN, .size = 0x10000}};
static const SnxModule g_module = {.nmods = 1, .mods = g_mods};
const SnxModule* g_mod = &g_module;
Settings g_settings;
int gmem_read(u64 gva, void* dst, u64 size) {
    if (gva >= sizeof memory || size > sizeof memory - gva) return -1;
    memcpy(dst, memory + gva, size);
    return 0;
}
void gmem_read_code(u64 gva, void* dst, u64 size) {
    u64 n = gva < sizeof memory ? sizeof memory - gva : 0;
    if (n > size) n = size;
    if (n) memcpy(dst, memory + gva, n);
    memset((u8*)dst + n, 0, size - n);
}
u32 gpu_color_bpp(u32 format) {
    (void)format;
    return 4;
}
/* nvn_draw.c's two functions the trace calls, as the queue's state of this test makes them */
int nvn_draw_dispatch_state(NvnDraw* d, u64 payload, DispatchState* x) {
    const NvnPDispatch* r = NVN_OBJ(const NvnPDispatch, payload);
    if (!d->dx.code) return 0;
    d->dx.grid[0] = (u32)r->x, d->dx.grid[1] = (u32)r->y, d->dx.grid[2] = (u32)r->z;
    *x = d->dx;
    return 1;
}
/* an Ounce program's packed c[0] while the queue has a texture pool: one image handle, at HX_HANDLE_OFF */
int nvn_draw_c0_layout(const NvnDraw* d, u32 at[NVN_C0_LISTS], u32 count[NVN_C0_LISTS]) {
    if (!d->dx.tex_pool) return 0;
    for (u32 l = 0; l < NVN_C0_LISTS; l++) at[l] = 0x10, count[l] = 0;
    at[NVN_C0_IMAGES] = HX_HANDLE_OFF, count[NVN_C0_IMAGES] = 1;
    return 1;
}
static char g_log[16384];
void logf_(int lvl, const char* fmt, ...) {
    va_list a;
    (void)lvl;
    va_start(a, fmt);
    size_t n = strlen(g_log);
    va_list b;
    va_copy(b, a);
    vsnprintf(g_log + n, sizeof g_log - n, fmt, b);
    va_end(b);
    if (getenv("SNX_TEST_LOG")) vprintf(fmt, a);
    va_end(a);
}

static NvnDraw g_queue;
static void draw(void) {
    if (g_nvn_trace_live) nvn_trace_record(&g_queue, OP_DRAW, 0);
}
static void dispatch(void) {
    NvnPDispatch r = {.x = 8, .y = 8, .z = 1, .caller = MAIN + 0x900};
    memcpy(memory + RECORDS, &r, sizeof r);
    if (g_nvn_trace_live) nvn_trace_record(&g_queue, OP_DISPATCH, RECORDS);
}

int main(void) {
    /* a program's hash */
    u64 want = 0xcbf29ce484222325ull;
    for (int i = 0; i < 64; i++) memory[HASHED + i] = (u8)(i * 13 + 5);
    for (int i = 0; i < 64; i++) want = (want ^ memory[HASHED + i]) * 0x100000001b3ull;
    CHECK(nvn_trace_program_hash(HASHED, 64) == want && nvn_trace_program_hash(sizeof memory, 64) == 0);

    /* the queue: a tonemap draw into a B10G11R11 target, its constant buffer 3 of the vertex stage the camera, and a
       dispatch of block 64 x 1 x 1 whose packed c[0] names texture pool entry 5, the output image */
    for (int i = 0; i < 256; i++) memory[TONEMAP + i] = (u8)(i * 13 + 5), memory[OUTPASS + i] = (u8)(i * 19 + 7);
    g_queue.code[DS_PROGRAM_SLOTS - 1] = TONEMAP;
    g_queue.ds.tgt.count = 1;
    g_queue.ds.tgt.color[0].format = 0xe0, g_queue.ds.tgt.color[0].width = 64, g_queue.ds.tgt.color[0].height = 36;
    g_queue.ds.tgt.color[0].gva = 0x400000;
    g_queue.vp[0][2] = 64, g_queue.vp[0][3] = 36;
    static const float camera[4] = {1.5f, -2, 0.25f, 4};
    memcpy(memory + CAMERA + 0x10, camera, sizeof camera);
    g_queue.bind.cb_addr[0][3] = CAMERA;
    g_queue.dx.code = OUTPASS, g_queue.dx.code_bytes = 256;
    g_queue.dx.block[0] = 64, g_queue.dx.block[1] = 1, g_queue.dx.block[2] = 1;
    g_queue.dx.cb_inline[0] = DS_CB_BANK0;
    g_queue.dx.tex_pool = TEX_POOL, g_queue.dx.tex_max = 15;
    snx_st_le32(g_queue.dx.bank0 + HX_HANDLE_OFF, 5);
    {
        u32 tic[8];
        NvnTexture t = {0};
        t.d.format = 0x3F, t.d.target = NVN_TEXTURE_TARGET_2D, t.d.width = OW, t.d.height = OH, t.d.depth = 1;
        t.d.levels = 1, t.gpu = OUTPUT_RT;
        CHECK(nvn_tic_encode(&t, NULL, 1, 0, tic));
        memcpy(memory + TEX_POOL + 32 * 5, tic, sizeof tic);
    }

    /* off: never live */
    nvn_trace_start();
    nvn_trace_present();
    CHECK(!g_nvn_trace_live && !g_log[0]);

    /* from the second present, with 4 words of the vertex stage's constant buffer 3 at 0x10 */
    g_settings.gpu_hook_trace = 64;
    g_settings.gpu_hook_trace_from = 2;
    snprintf(g_settings.gpu_hook_trace_words, sizeof g_settings.gpu_hook_trace_words, "0,3,0x10,4");
    nvn_trace_arm();
    nvn_trace_present();
    draw();
    CHECK(!g_nvn_trace_live && !g_log[0]);
    nvn_trace_present();
    CHECK(g_nvn_trace_live && strstr(g_log, "hooks: trace present 2"));
    draw();
    draw();
    if (g_nvn_trace_live) nvn_trace_record(&g_queue, OP_CLEAR_COLOR, 0);
    if (g_nvn_trace_live) nvn_trace_record(&g_queue, OP_BIND_TEX, 0);
    dispatch();
    if (g_nvn_trace_live) nvn_trace_flush(&g_queue);
    printf("trace:\n%s", g_log);
    char line[160];
    snprintf(line, sizeof line,
             "hooks: trace queue 0 draw fragment program %016llx, target 0 format 0xe0 64x36 at 400000",
             (unsigned long long)nvn_trace_program_hash(TONEMAP, 0));
    CHECK(strstr(g_log, line));
    CHECK(strstr(g_log, "viewport 0 0 64 36, constant buffer 0.3 at eff00 +0x10: 1.5 -2 0.25 4"));
    CHECK(strstr(g_log, "constant buffer 0.3 at eff00 as before"));
    snprintf(line, sizeof line, "dispatch program %016llx (256 bytes at 26000) caller main+900 grid 8x8x1 block 64x1x1",
             (unsigned long long)nvn_trace_program_hash(OUTPASS, 256));
    CHECK(strstr(g_log, line));
    CHECK(strstr(g_log, ", img c0+40 5: 800000 format 0xe0 128x72"));
    snprintf(line, sizeof line, ", after records %u\n", (unsigned)OP_CLEAR_COLOR);
    CHECK(strstr(g_log, line)); /* the bind isn't listed */
    CHECK(strstr(g_log, "hooks: trace queue 0 at a wait or the end of a submission"));

    /* a line past the log's 2048 bytes goes on in a line of its own: 128 words, each up to 15 characters */
    g_log[0] = 0;
    snprintf(g_settings.gpu_hook_trace_words, sizeof g_settings.gpu_hook_trace_words, "0,3,0,128");
    g_settings.gpu_hook_trace_from = 0;
    g_queue.bind.cb_addr[0][3] = WORDS;
    for (int i = 0; i < 128; i++) memcpy(memory + WORDS + 4 * i, &(float){-1.2345678e-30f}, 4);
    nvn_trace_arm();
    draw();
    size_t longest = 0;
    for (const char* p = g_log; *p;) {
        const char* e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n > longest) longest = n;
        p += n + (e != NULL);
    }
    printf("trace, 128 words: longest line %zu bytes\n", longest);
    CHECK(longest < 2000 && strstr(g_log, "hooks: trace, the line before goes on"));

    /* its lines used up: off again */
    g_settings.gpu_hook_trace = 1;
    g_settings.gpu_hook_trace_words[0] = 0;
    nvn_trace_arm();
    CHECK(g_nvn_trace_live);
    draw();
    CHECK(!g_nvn_trace_live);
    puts("nvn_trace: passed");
    return 0;
}
