/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vic.c's CPU composition, through vic_execute and the registers and config a game writes: the samples copied as
   they are without a color matrix, a BT.601 matrix, the planar alpha, NV12 out with and without its chroma plane,
   the target rectangle (clipping, an odd left edge, wholly outside the surface), and the GPU asked only once the
   renderer is up. The stand-in renderer declines every job, so the CPU composes; surfaces are pitch linear */
#include "nx/gpu/video/vic.c"

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ what vic.c reaches outside itself */
static int g_asked; /* jobs the renderer was handed */
static int decline(const VicJob* j) {
    (void)j;
    g_asked++;
    return 0;
}
static const Renderer stand_in = {.name = "stand-in", .vic_compose = decline};
const Renderer* g_renderer = &stand_in;
int g_renderer_up = 1;
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
#define BASE  0x1000000ull
#define ARENA (4u << 20)
static u8 arena[ARENA];
u8* iova_ptr(u64 iova, u64 len) {
    if (iova < BASE || iova - BASE + len > ARENA) return NULL;
    return arena + (iova - BASE);
}
u64 bl_offset(u32 x, u32 y, u32 z, u32 wb, u32 h, u32 bh, u32 bd) {
    (void)x;
    (void)y;
    (void)z;
    (void)wb;
    (void)h;
    (void)bh;
    (void)bd;
    fprintf(stderr, "bl_offset: every surface here is pitch linear\n");
    exit(1);
}

/* ------------------------------------------------ a job: one slot (0), its config at CFG */
#define CFG   0x0000u
#define IN_Y  0x10000u
#define IN_C  0x40000u
#define OUT_Y 0x100000u
#define OUT_C 0x300000u
#define SENT  0xEE /* what the output holds before the job: a byte still SENT was not written */

static u32 regs[0x800 / 4];
static void w32(u32 off, u32 v) { memcpy(arena + off, &v, 4); }
static void w64(u32 off, u64 v) { memcpy(arena + off, &v, 8); }
static u32 pitch(u32 wbytes) { return (wbytes + 255) & ~255u; }

typedef struct {
    u32 fmt, sw, sh;        /* the output surface */
    u32 tl, tr, tt, tb;     /* the target rectangle, inclusive */
    u32 iw, ih;             /* the NV12 input */
    u32 sx0, sx1, sy0, sy1; /* the source rectangle, whole pixels, inclusive */
    u32 dx0, dx1, dy0, dy1; /* the destination rectangle, inclusive */
    u32 clo, chi, alpha;    /* the soft clamp and the planar alpha, 10 bit */
    int matrix;             /* the BT.601 matrix below */
    int no_out_chroma;      /* the output chroma register left at 0 */
    u32 bg[4];              /* the background colour, R G B A, 10 bit */
    int slot_off;           /* the slot isn't enabled */
    u32 in_fmt;             /* the input's format: 0 for NV12 (0x44) */
} Job;
/* a 16x16 A8B8G8R8 surface, all of it the target, a 16x16 picture onto all of it */
static Job job16(void) {
    Job j = {0x1F, 16, 16, 0, 15, 0, 15, 16, 16, 0, 15, 0, 15, 0, 15, 0, 15, 0, 0x3FF, 0x3FF, 0, 0, {0}, 0, 0};
    return j;
}
/* BT.601 limited range in the 10 bit domain, S12.8, shift 0: out = (Y*c0 + U*c1 + V*c2 + c3) >> 8 */
static const int M601[3][4] = {
    {298, 0, 409, -(298 * 64 + 409 * 512)},
    {298, -100, -208, -(298 * 64) + (100 + 208) * 512},
    {298, 516, 0, -(298 * 64 + 516 * 512)},
};
static u64 f20(int v) { return (u64)(u32)v & 0xFFFFF; }

static void job_setup(const Job* j) {
    memset(arena, 0, ARENA);
    memset(regs, 0, sizeof regs);
    w64(CFG + 0x10, (u64)j->bg[3] << 6 | (u64)j->bg[0] << 16 | (u64)j->bg[1] << 26 | (u64)j->bg[2] << 36);
    w32(CFG + 0x18, j->tl | j->tr << 16);
    w32(CFG + 0x1C, j->tt | j->tb << 16);
    w64(CFG + 0x20, (u64)j->fmt | (u64)(j->sw - 1) << 32 | (u64)(j->sh - 1) << 46);
    /* the padded sizes are the strides, as libdrm's vic40 writes them: OutLumaWidth = stride - 1, in pixels */
    int yuv = j->fmt == 0x43 || j->fmt == 0x44;
    u32 bpp = yuv ? 1 : 4, olw = pitch(j->sw * bpp) / bpp, ocw = pitch(j->sw) / 2;
    w64(CFG + 0x28, (u64)(olw - 1) | (u64)(j->sh - 1) << 14 | (u64)(ocw - 1) << 32 | (u64)((j->sh + 1) / 2 - 1) << 46);
    u32 sl = CFG + 0x90;
    w64(sl + 0x00, !j->slot_off);
    w64(sl + 0x10, (u64)j->clo | (u64)j->chi << 10 | (u64)j->alpha << 32);
    w64(sl + 0x20, (u64)(j->sx0 << 16) | (u64)(j->sx1 << 16) << 32);
    w64(sl + 0x28, (u64)(j->sy0 << 16) | (u64)(j->sy1 << 16) << 32);
    w64(sl + 0x30, (u64)j->dx0 | (u64)j->dx1 << 16 | (u64)j->dy0 << 32 | (u64)j->dy1 << 48);
    w64(sl + 0x40, (j->in_fmt ? j->in_fmt : 0x44) | (u64)(j->iw - 1) << 32 | (u64)(j->ih - 1) << 46);
    w64(sl + 0x48, (u64)(pitch(j->iw) - 1) | (u64)(j->ih - 1) << 14 | (u64)(pitch(j->iw) / 2 - 1) << 32 |
                       (u64)((j->ih + 1) / 2 - 1) << 46);
    if (j->matrix) {
        w64(sl + 0x60, f20(M601[0][0]) | f20(M601[1][0]) << 20 | f20(M601[2][0]) << 40);
        w64(sl + 0x68, f20(M601[0][1]) | f20(M601[1][1]) << 20 | f20(M601[2][1]) << 40 | 1ull << 63);
        w64(sl + 0x70, f20(M601[0][2]) | f20(M601[1][2]) << 20 | f20(M601[2][2]) << 40);
        w64(sl + 0x78, f20(M601[0][3]) | f20(M601[1][3]) << 20 | f20(M601[2][3]) << 40);
    }
    regs[0x708 / 4] = (u32)((BASE + CFG) >> 8);
    regs[0x720 / 4] = (u32)((BASE + OUT_Y) >> 8);
    regs[0x724 / 4] = j->no_out_chroma ? 0 : (u32)((BASE + OUT_C) >> 8);
    regs[0x400 / 4] = (u32)((BASE + IN_Y) >> 8);
    regs[0x404 / 4] = (u32)((BASE + IN_C) >> 8);
    memset(arena + OUT_Y, SENT, (size_t)pitch(j->sw * bpp) * j->sh);
    if (yuv) memset(arena + OUT_C, SENT, (size_t)pitch(j->sw) * ((j->sh + 1) / 2));
}
static void in_fill(const Job* j, u8 y, u8 u, u8 v) {
    for (u32 row = 0; row < j->ih; row++) memset(arena + IN_Y + row * pitch(j->iw), y, j->iw);
    for (u32 row = 0; row < (j->ih + 1) / 2; row++)
        for (u32 x = 0; x < j->iw; x += 2) {
            arena[IN_C + row * pitch(j->iw) + x] = u;
            arena[IN_C + row * pitch(j->iw) + x + 1] = v;
        }
}
static u8* px(const Job* j, u32 x, u32 y) { return arena + OUT_Y + y * pitch(j->sw * 4) + x * 4; }
static int written(const Job* j, u32 x, u32 y) {
    const u8* p = px(j, x, y);
    return p[0] != SENT || p[1] != SENT || p[2] != SENT || p[3] != SENT;
}
static u32 count_written(const Job* j) {
    u32 n = 0;
    for (u32 y = 0; y < j->sh; y++)
        for (u32 x = 0; x < j->sw; x++) n += (u32)written(j, x, y);
    return n;
}
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ------------------------------------------------ the cases */
/* no color matrix: Y, U, V land in R, G, B as they are, the planar alpha in A */
static void samples_as_they_are(void) {
    static const u8 yuv[2][3] = {{235, 128, 128}, {81, 90, 240}};
    for (int i = 0; i < 2; i++) {
        Job j = job16();
        job_setup(&j);
        in_fill(&j, yuv[i][0], yuv[i][1], yuv[i][2]);
        vic_execute(regs);
        const u8* p = px(&j, 7, 9);
        CHECK(p[0] == yuv[i][0] && p[1] == yuv[i][1] && p[2] == yuv[i][2] && p[3] == 255);
        CHECK(count_written(&j) == 16 * 16);
    }
}
/* a BT.601 limited range matrix in the config gives BT.601's RGB, within one step of rounding */
static void bt601_matrix(void) {
    int worst = 0;
    for (int y = 16; y <= 235; y += 9)
        for (int u = 16; u <= 240; u += 14)
            for (int v = 16; v <= 240; v += 14) {
                Job j = job16();
                j.iw = j.ih = 2, j.sx1 = j.sy1 = j.dx1 = j.dy1 = 1;
                j.matrix = 1;
                job_setup(&j);
                in_fill(&j, (u8)y, (u8)u, (u8)v);
                vic_execute(regs);
                const u8* p = px(&j, 0, 0);
                int c = (y - 16) * 298, d = u - 128, e = v - 128;
                int ref[3] = {clamp8((c + 409 * e + 128) >> 8), clamp8((c - 100 * d - 208 * e + 128) >> 8),
                              clamp8((c + 516 * d + 128) >> 8)};
                for (int k = 0; k < 3; k++) {
                    int diff = abs((int)p[k] - ref[k]);
                    if (diff > worst) worst = diff;
                }
            }
    CHECK(worst <= 1);
}
/* the planar alpha, 10 bit, comes out at 8 */
static void planar_alpha(void) {
    static const u32 alpha[3] = {0x3FF, 0x200, 0}, out[3] = {255, 128, 0};
    for (int i = 0; i < 3; i++) {
        Job j = job16();
        j.alpha = alpha[i];
        j.matrix = 1;
        job_setup(&j);
        in_fill(&j, 128, 128, 128);
        vic_execute(regs);
        CHECK(px(&j, 3, 3)[3] == out[i]);
    }
}
/* NV12 in and out without a matrix: the samples as they are, a chroma row per two luma rows */
static void nv12_out(void) {
    Job j = job16();
    j.fmt = 0x44;
    job_setup(&j);
    in_fill(&j, 100, 110, 120);
    vic_execute(regs);
    u32 crows = 0;
    for (u32 row = 0; row < 8; row++) crows += arena[OUT_C + row * pitch(16)] != SENT;
    CHECK(arena[OUT_Y] == 100 && arena[OUT_Y + 15 * pitch(16) + 15] == 100);
    CHECK(arena[OUT_C] == 110 && arena[OUT_C + 1] == 120 && crows == 8);
}
/* NV12 out with no chroma plane mapped: the luma is written, the chroma rows are skipped (not written through NULL) */
static void nv12_out_without_chroma(void) {
    Job j = job16();
    j.fmt = 0x44;
    j.no_out_chroma = 1;
    job_setup(&j);
    in_fill(&j, 100, 110, 120);
    vic_execute(regs);
    CHECK(arena[OUT_Y] == 100 && arena[OUT_Y + 15 * pitch(16) + 15] == 100);
}
/* the target rectangle 4..11 clips the slot's 0..15 */
static void target_clips(void) {
    Job j = job16();
    j.tl = j.tt = 4, j.tr = j.tb = 11;
    job_setup(&j);
    in_fill(&j, 100, 128, 128);
    vic_execute(regs);
    CHECK(count_written(&j) == 8 * 8 && !written(&j, 3, 4) && written(&j, 4, 4) && written(&j, 11, 11) &&
          !written(&j, 12, 11));
}
/* an odd left edge (5): the picture starts there, as RGBA keeps odd starts, and still reaches the right edge */
static void target_odd_left(void) {
    Job j = job16();
    j.tl = 5;
    job_setup(&j);
    in_fill(&j, 100, 128, 128);
    vic_execute(regs);
    CHECK(count_written(&j) == 11 * 16 && !written(&j, 4, 0) && written(&j, 5, 0) && written(&j, 15, 0));
}
/* a target wholly right of a 64x4 surface: nothing is written, no row lands a row down */
static void target_outside(void) {
    Job j = {0x1F, 64, 4, 80, 120, 0, 3, 128, 4, 0, 127, 0, 3, 0, 127, 0, 3, 0, 0x3FF, 0x3FF, 0, 0, {0}, 0, 0};
    job_setup(&j);
    for (u32 row = 0; row < 4; row++) memset(arena + IN_Y + row * pitch(128), 100 + 20 * (int)row, 128);
    memset(arena + IN_C, 128, pitch(128) * 2);
    vic_execute(regs);
    CHECK(count_written(&j) == 0);
}
/* with no slot enabled, the target rectangle 2..13 takes the background colour (10 bit 1023, 512, 0, 1023 is 8 bit
   255, 128, 0, 255) and the surface around it is left alone */
static const u32 ORANGE[4] = {1023, 512, 0, 1023};
static void background_without_slot(void) {
    Job j = job16();
    j.tl = j.tt = 2, j.tr = j.tb = 13;
    j.slot_off = 1;
    memcpy(j.bg, ORANGE, sizeof j.bg);
    job_setup(&j);
    vic_execute(regs);
    const u8* p = px(&j, 2, 2);
    CHECK(p[0] == 255 && p[1] == 128 && p[2] == 0 && p[3] == 255);
    CHECK(count_written(&j) == 12 * 12 && !written(&j, 1, 2) && written(&j, 13, 13) && !written(&j, 14, 13));
}
/* a slot onto 4..11 of a 0..15 target: the background around it, the picture inside */
static void background_ring(void) {
    Job j = job16();
    j.sx1 = j.sy1 = 7;
    j.dx0 = j.dy0 = 4, j.dx1 = j.dy1 = 11;
    memcpy(j.bg, ORANGE, sizeof j.bg);
    job_setup(&j);
    in_fill(&j, 100, 110, 120);
    vic_execute(regs);
    CHECK(px(&j, 0, 0)[0] == 255 && px(&j, 3, 4)[1] == 128 && px(&j, 12, 11)[2] == 0);
    CHECK(px(&j, 4, 4)[0] == 100 && px(&j, 11, 11)[1] == 110 && px(&j, 11, 11)[2] == 120);
    CHECK(count_written(&j) == 16 * 16);
}
/* input format 0x43 (NV21): V in each chroma pair's even byte, U in the odd one */
static void nv21_in(void) {
    Job j = job16();
    j.in_fmt = 0x43;
    job_setup(&j);
    in_fill(&j, 100, 110, 120);
    vic_execute(regs);
    const u8* p = px(&j, 5, 5);
    CHECK(p[0] == 100 && p[1] == 120 && p[2] == 110);
}
/* output format 0x43 (NV21): V written to each chroma pair's even byte */
static void nv21_out(void) {
    Job j = job16();
    j.fmt = 0x43;
    job_setup(&j);
    in_fill(&j, 100, 110, 120);
    vic_execute(regs);
    CHECK(arena[OUT_Y] == 100 && arena[OUT_C] == 120 && arena[OUT_C + 1] == 110);
}
/* the renderer is handed the job only once it is up; before, the CPU composes it */
static void gpu_only_once_up(void) {
    Job j = job16();
    g_renderer_up = 0;
    g_asked = 0;
    job_setup(&j);
    in_fill(&j, 50, 60, 70);
    vic_execute(regs);
    CHECK(g_asked == 0 && px(&j, 0, 0)[0] == 50);
    g_renderer_up = 1;
    job_setup(&j);
    in_fill(&j, 50, 60, 70);
    vic_execute(regs);
    CHECK(g_asked == 1 && px(&j, 0, 0)[0] == 50);
}

int main(void) {
    samples_as_they_are();
    bt601_matrix();
    planar_alpha();
    nv12_out();
    nv12_out_without_chroma();
    target_clips();
    target_odd_left();
    target_outside();
    gpu_only_once_up();
    background_without_slot();
    background_ring();
    nv21_in();
    nv21_out();
    printf("vic_compose: %u checks passed\n", checks);
    return 0;
}
