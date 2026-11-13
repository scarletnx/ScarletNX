/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the 2D engine's blit from memory (fermi2d.c), the renderer's image blit declining it: the source pixels the blit
   samples, which fermi2d.c works out, handed to the renderer's blit_mem (here the CPU reference, fermi2d_ref.c, as
   vk/blit.c is held to it on a device by vulkan_headless). A block linear source scaled down by two into a block
   linear destination, read back pixel by pixel through the block linear layout itself (bl_offset below, the whole
   formula); an odd destination window, and nothing written outside it; a source whose rows end inside a 16 byte run;
   the renderer's blit asked once, and the render target over the destination reloaded once. Between float formats:
   half floats to floats and back for every half and the rounding of floats to halves (half.c, the reference
   texconv.glsl's TC_BLIT_FLOAT is held to), and a pitch linear RGBA32F source into a block linear RGBA16F window, the
   renderer's blit asked with the conversion. Between other color formats, as edenprobe's copy suite blits them:
   RGBA8 into BGRA8, RGB565 and RGBA16F and RGBA16F into RGBA8, channels of nought and full, the blit asked with the
   pixel conversion and run through its shader's CPU build (blit_convert_cpu.cpp), each pixel held to what the format
   holds: red and blue swapped, red in the top five bits of the 565 word, one as 0x3c00. They copied the bytes (RGBA8
   and BGRA8) or were not done */
#include "nx/gpu/maxwell/fermi2d.c"
#include "support/fermi2d_ref.h"
#include "support/half.h"
#include "blit_convert_cpu.h"
#include "common/utilities/endian.h"

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ guest memory */
#define MEM (1u << 20)
static u8 mem[MEM];
void gmem_read_raw(u64 gva, void* dst, u64 size) {
    CHECK(gva + size <= MEM);
    memcpy(dst, mem + gva, size);
}
void gmem_write_raw(u64 gva, const void* src, u64 size) {
    CHECK(gva + size <= MEM);
    memcpy(mem + gva, src, size);
}
/* the block linear layout: GOBs of 64 bytes by 8 rows in blocks of 1 << bh GOBs, the GOB's 16 byte groups in
   their order */
u64 bl_offset(u32 x, u32 y, u32 z, u32 wb, u32 h, u32 bh_log2, u32 bd_log2) {
    u32 gob_w = (wb + 63) / 64, bh = 1u << bh_log2, bd = 1u << bd_log2;
    u32 blocks_h = ((h + 7) / 8 + bh - 1) / bh, gy = y / 8, xb = x % 64, yb = y % 8;
    u64 block = 512ull * bh * bd;
    u64 o = ((u64)(z / bd) * blocks_h * gob_w + (u64)(gy / bh) * gob_w + x / 64) * block +
            ((u64)(z % bd) * bh + gy % bh) * 512;
    return o + (xb / 32) * 256 + (yb / 2) * 64 + ((xb % 32) / 16) * 32 + (yb % 2) * 16 + xb % 16;
}
int gmem_read(u64 gva, void* dst, u64 size) {
    gmem_read_raw(gva, dst, size);
    return 0;
}
int gmem_write(u64 gva, const void* src, u64 size) {
    gmem_write_raw(gva, src, size);
    return 0;
}
u32 gpu_color_bpp(u32 format) { return format == 0xC0 ? 16 : format == 0xCA ? 8 : format == 0xE8 ? 2 : 4; }
int r3d_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
             double sdy) {
    (void)s, (void)d, (void)dx, (void)dy, (void)dw, (void)dh, (void)sx, (void)sy, (void)sdx, (void)sdy;
    return 0; /* the renderer declines: the blit runs in guest memory */
}
static unsigned blits, reloads;
static void reloaded(u64 gva) {
    (void)gva;
    reloads++;
}
/* a pixel conversion's blit (GPU_BLIT_CONVERT) a pixel at a time through the shader's CPU build: the pixel the tables
   name staged with a one entry column and row table, its converted bytes written where the destination has them */
static u32 last_fmt;
static void blit_convert_ref(const GpuBlit* b) {
    u32 bd = GPU_BLIT_DST_BPP(b);
    for (u32 y = b->y0; y < b->y1; y++)
        for (u32 x = b->x0; x < b->x1; x++) {
            u8 in[32] = {0}, out[16] = {0};
            u32 sx = b->scol[x - b->x0], sy = b->srow[y - b->y0];
            CHECK(!b->src.bl && !b->dst.bl && b->bpp <= 16 && bd <= 12);
            gmem_read_raw(b->src.addr + (u64)sy * b->src.pitch + (u64)sx * b->bpp, in, b->bpp);
            blit_convert_cpu(b->fmt, bd, 1, in, sizeof in, 0, 16, 16, 20, out, 16);
            gmem_write_raw(b->dst.addr + (u64)y * b->dst.pitch + (u64)x * bd, out, bd);
        }
}
static int blit_ref(const GpuBlit* b) {
    blits++;
    last_fmt = b->fmt;
    if (b->fmt & GPU_BLIT_CONVERT_BIT)
        blit_convert_ref(b);
    else
        blit_mem_ref(b);
    return 1;
}
static const Renderer stand_in = {.name = "stand-in", .rt_reload = reloaded, .blit_mem = blit_ref};
const Renderer* g_renderer = &stand_in;
int g_renderer_up = 1;
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
void fatal(const char* fmt, ...) {
    (void)fmt;
    exit(1);
}

/* ------------------------------------------------ the blit */
#define SRC 0x00000
#define DST 0x40000
enum { SW = 128, SH = 32, DW = 64, DH = 16 };
static u32 src_px(u32 x, u32 y) { return 0x10000000u | y << 12 | x; }
static u32 at(u64 base, u32 x, u32 y, u32 w, u32 h) {
    u32 v;
    memcpy(&v, mem + base + bl_offset(x * 4, y, 0, w * 4, h, 1, 0), 4);
    return v;
}
static void surface(GpuChannel* ch, int b, u64 addr, u32 w, u32 h) {
    u32* r = ch->r2d;
    r[b] = 0xd5;        /* A8B8G8R8 */
    r[b + 1] = 0;       /* block linear */
    r[b + 2] = 1u << 4; /* two GOBs a block */
    r[b + 3] = 1;       /* depth */
    r[b + 4] = 0;       /* layer */
    r[b + 6] = w;
    r[b + 7] = h;
    r[b + 8] = (u32)(addr >> 32);
    r[b + 9] = (u32)addr;
}
/* dw x dh at dx, dy from a sw-wide source scaled down by two (corner origin, no half texel) */
static void blit_from(u32 sw, i32 dx, i32 dy, i32 dw, i32 dh) {
    static GpuChannel ch;
    memset(&ch, 0, sizeof ch);
    surface(&ch, 0x80, DST, DW, DH);
    surface(&ch, 0x8C, SRC, sw, SH);
    u32* r = ch.r2d;
    r[0x223] = 1;
    r[0x22C] = (u32)dx;
    r[0x22D] = (u32)dy;
    r[0x22E] = (u32)dw;
    r[0x22F] = (u32)dh;
    r[0x231] = 2; /* du/dx 2.0 */
    r[0x233] = 2; /* dv/dy 2.0 */
    blits = reloads = 0;
    fermi2d_method(&ch, 0x237, 0); /* src_y0 starts it */
    CHECK(blits == 1 && reloads == 1);
}
static void blit(i32 dx, i32 dy, i32 dw, i32 dh) { blit_from(SW, dx, dy, dw, dh); }
static void fill_from(u32 sw) {
    memset(mem, 0xEE, sizeof mem);
    for (u32 y = 0; y < SH; y++)
        for (u32 x = 0; x < sw; x++) {
            u32 v = src_px(x, y);
            memcpy(mem + SRC + bl_offset(x * 4, y, 0, sw * 4, SH, 1, 0), &v, 4);
        }
}
static void fill(void) { fill_from(SW); }
static void whole(void) {
    fill();
    blit(0, 0, DW, DH);
    for (u32 y = 0; y < DH; y++)
        for (u32 x = 0; x < DW; x++) CHECK(at(DST, x, y, DW, DH) == src_px(2 * x, 2 * y));
}
static void window(void) {
    fill();
    blit(5, 3, 21, 9); /* runs start mid group at 20 bytes, end mid group at 104 */
    for (u32 y = 0; y < DH; y++)
        for (u32 x = 0; x < DW; x++) {
            int inside = x >= 5 && x < 26 && y >= 3 && y < 12;
            u32 v = at(DST, x, y, DW, DH);
            CHECK(inside ? v == src_px(2 * (x - 5), 2 * (y - 3)) : v == 0xEEEEEEEEu);
        }
}

/* a source 37 pixels wide: its rows end 4 bytes into a 16 byte run */
static void odd_source(void) {
    fill_from(37);
    blit_from(37, 0, 0, 19, 16);
    for (u32 y = 0; y < DH; y++)
        for (u32 x = 0; x < DW; x++) {
            u32 v = at(DST, x, y, DW, DH);
            CHECK(x < 19 ? v == src_px(2 * x, 2 * y) : v == 0xEEEEEEEEu);
        }
}

/* ------------------------------------------------ between float formats */
static void halves(void) {
    for (u32 h = 0; h < 0x10000; h++) {
        float f = half_to_float((u16)h);
        if ((h & 0x7C00) == 0x7C00 && (h & 0x3FF))
            CHECK(f != f && (float_to_half(f) & 0x7FFF) > 0x7C00); /* a NaN stays one */
        else
            CHECK(float_to_half(f) == h);
    }
    CHECK(half_to_float(0x3C00) == 1.0f && half_to_float(0xC000) == -2.0f && half_to_float(0x0001) == 0x1p-24f);
    CHECK(float_to_half(0.1f) == 0x2E66);
    CHECK(float_to_half(2049.0f) == 0x6800 && float_to_half(2051.0f) == 0x6802); /* ties to even */
    CHECK(float_to_half(65504.0f) == 0x7BFF && float_to_half(65520.0f) == 0x7C00 && float_to_half(-1e9f) == 0xFC00);
    CHECK(float_to_half(1e-7f) == 0x0002 && float_to_half(1e-9f) == 0 && float_to_half(-0.0f) == 0x8000);
}
enum { FW = 24, FH = 12 };
static float float_px(u32 x, u32 y, u32 k) { return (float)((i32)(x * 7 + y * 131 + k * 1000) - 2000) / 64.0f; }
static void convert(void) {
    static GpuChannel ch;
    u32* r = ch.r2d;
    memset(&ch, 0, sizeof ch);
    memset(mem, 0xEE, sizeof mem);
    for (u32 y = 0; y < FH; y++)
        for (u32 x = 0; x < FW; x++)
            for (u32 k = 0; k < 4; k++) {
                float v = float_px(x, y, k);
                memcpy(mem + SRC + (u64)y * FW * 16 + x * 16 + k * 4, &v, 4);
            }
    surface(&ch, 0x80, DST, FW, FH);
    surface(&ch, 0x8C, SRC, FW, FH);
    r[0x80] = 0xCA;        /* R16G16B16A16_FLOAT, block linear */
    r[0x8C] = 0xC0;        /* R32G32B32A32_FLOAT */
    r[0x8C + 1] = 1;       /* pitch linear */
    r[0x8C + 5] = FW * 16; /* the pitch */
    r[0x223] = 1;
    r[0x22C] = 3;
    r[0x22D] = 2;
    r[0x22E] = 17;
    r[0x22F] = 7;
    r[0x231] = 1; /* du/dx 1.0 */
    r[0x233] = 1;
    blits = reloads = 0;
    fermi2d_method(&ch, 0x237, 0);
    CHECK(blits == 1 && reloads == 1);
    for (u32 y = 0; y < FH; y++)
        for (u32 x = 0; x < FW; x++)
            for (u32 k = 0; k < 4; k++) {
                u16 v;
                memcpy(&v, mem + DST + bl_offset(x * 8, y, 0, FW * 8, FH, 1, 0) + k * 2, 2);
                int inside = x >= 3 && x < 20 && y >= 2 && y < 9;
                CHECK(inside ? v == float_to_half(float_px(x - 3, y - 2, k)) : v == 0xEEEE);
            }
}

/* ------------------------------------------------ between other color formats */
/* pixel (x, y) of a 4x4 RGBA8 source: red full in odd columns, green in columns 2 and 3, blue in odd rows, alpha in
   rows 2 and 3, as edenprobe's CopyFullTexel */
static int full(u32 x, u32 y, u32 k) { return k == 0 ? x & 1 : k == 1 ? x & 2 : k == 2 ? y & 1 : y & 2; }
/* the 4x4 source of format sf, pitch linear, blitted 1:1 into a pitch linear destination of format df */
static void formats_blit(u32 sf, u32 sb, u32 df, u32 db) {
    static GpuChannel ch;
    memset(&ch, 0, sizeof ch);
    memset(mem, 0xEE, sizeof mem);
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            for (u32 k = 0; k < 4; k++) {
                u8* p = mem + SRC + y * 64 + x * sb;
                if (sb == 4)
                    p[k] = full(x, y, k) ? 0xFF : 0;
                else
                    snx_st_le16(p + 2 * k, full(x, y, k) ? 0x3C00 : 0);
            }
    surface(&ch, 0x80, DST, 4, 4);
    surface(&ch, 0x8C, SRC, 4, 4);
    u32* r = ch.r2d;
    r[0x80] = df, r[0x81] = 1, r[0x85] = 64; /* pitch linear, 64 bytes a row */
    r[0x8C] = sf, r[0x8D] = 1, r[0x91] = 64;
    r[0x223] = 1;
    r[0x22E] = 4;
    r[0x22F] = 4;
    r[0x231] = 1; /* du/dx 1.0 */
    r[0x233] = 1;
    blits = reloads = 0;
    fermi2d_method(&ch, 0x237, 0);
    CHECK(blits == 1 && reloads == 1);
    CHECK((last_fmt & GPU_BLIT_CONVERT_BIT) && GPU_PIXEL_BYTES(last_fmt >> 16 & 0xFFFF) == db);
}
static void formats(void) {
    formats_blit(0xD5, 4, 0xCF, 4); /* RGBA8 into BGRA8: blue first, red third */
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            const u8* p = mem + DST + y * 64 + x * 4;
            CHECK(p[0] == (full(x, y, 2) ? 0xFF : 0) && p[1] == (full(x, y, 1) ? 0xFF : 0) &&
                  p[2] == (full(x, y, 0) ? 0xFF : 0) && p[3] == (full(x, y, 3) ? 0xFF : 0));
        }
    formats_blit(0xD5, 4, 0xE8, 2); /* RGBA8 into R5G6B5: red the top five bits */
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            u32 want =
                (full(x, y, 0) ? 0x1Fu << 11 : 0) | (full(x, y, 1) ? 0x3Fu << 5 : 0) | (full(x, y, 2) ? 0x1Fu : 0);
            CHECK(snx_ld_le16(mem + DST + y * 64 + x * 2) == want);
        }
    formats_blit(0xD5, 4, 0xCA, 8); /* RGBA8 into RGBA16F: full is one */
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            for (u32 k = 0; k < 4; k++)
                CHECK(snx_ld_le16(mem + DST + y * 64 + x * 8 + 2 * k) == (full(x, y, k) ? 0x3C00 : 0));
    formats_blit(0xCA, 8, 0xD5, 4); /* RGBA16F into RGBA8: one is full */
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            for (u32 k = 0; k < 4; k++) CHECK(mem[DST + y * 64 + x * 4 + k] == (full(x, y, k) ? 0xFF : 0));
}

int main(void) {
    whole();
    window();
    odd_source();
    halves();
    convert();
    formats();
    printf("fermi2d_blit: %u checks passed\n", checks);
    return 0;
}
