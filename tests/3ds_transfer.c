/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Golden tests of the PICA200 transfer engine (src/3ds/gpu/transfer.c). The expected values are written out from
   the layouts Azahar documents (the 8x8 Morton tile in video_core/utils.h, the pixel codecs of common/color.h), not
   derived from the engine's own helpers. No guest image, no address space. */
#include "3ds/gpu/transfer.h"
#include <stdio.h>
#include <string.h>

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

/* VideoCore::GetMortonOffset's comment: texel indices of one 8x8 tile, rows from y = 7 (top) to y = 0 */
static const unsigned tile[8][8] = {{42, 43, 46, 47, 58, 59, 62, 63}, {40, 41, 44, 45, 56, 57, 60, 61},
                                    {34, 35, 38, 39, 50, 51, 54, 55}, {32, 33, 36, 37, 48, 49, 52, 53},
                                    {10, 11, 14, 15, 26, 27, 30, 31}, {8, 9, 12, 13, 24, 25, 28, 29},
                                    {2, 3, 6, 7, 18, 19, 22, 23},     {0, 1, 4, 5, 16, 17, 20, 21}};
static unsigned texel(unsigned x, unsigned y) { return tile[7 - (y % 8)][x % 8]; }

static void morton(void) {
    unsigned x, y, ok = 1;
    for (y = 0; y < 8; ++y)
        for (x = 0; x < 8; ++x) ok &= snx_ctr_morton_offset(x, y, 1) == texel(x, y);
    check(ok, "the 8x8 tile is Z-ordered as utils.h draws it");
    /* the next tile to the right starts 64 texels on, bytes scale with the pixel size */
    check(snx_ctr_morton_offset(8, 0, 1) == 64 && snx_ctr_morton_offset(9, 1, 4) == (64 + 3) * 4,
          "tiles are 64 texels apart, offsets in bytes");
}

static void fills(void) {
    unsigned char b[12];
    static const unsigned char rgb[12] = {0x11, 0x22, 0x33, 0x11, 0x22, 0x33, 0x11, 0x22, 0x33, 0x11, 0x22, 0x33};
    static const unsigned char w32[8] = {0x44, 0x33, 0x22, 0x11, 0x44, 0x33, 0x22, 0x11};
    static const unsigned char w16[8] = {0x44, 0x33, 0x44, 0x33, 0x44, 0x33, 0x44, 0x33};
    memset(b, 0xee, sizeof b);
    snx_ctr_fill(b, 12, 0x00332211, SNX_CTR_FILL_24BIT | 1);
    check(!memcmp(b, rgb, 12), "24-bit fill writes R, G, B from the low bytes");
    memset(b, 0xee, sizeof b);
    snx_ctr_fill(b, 8, 0x11223344, SNX_CTR_FILL_32BIT | 1);
    check(!memcmp(b, w32, 8) && b[8] == 0xee, "32-bit fill is little-endian and stops at the end");
    memset(b, 0xee, sizeof b);
    snx_ctr_fill(b, 8, 0x11223344, 1);
    check(!memcmp(b, w16, 8) && b[8] == 0xee, "16-bit fill takes the value's low halfword");
    memset(b, 0xee, sizeof b);
    snx_ctr_fill(b, 7, 0x00332211, SNX_CTR_FILL_24BIT);
    check(b[6] == 0x11 && b[7] == 0xee, "a 24-bit fill cut by the end writes nothing past it");
}

/* an 8x8 tiled RGBA8 image whose pixel (x, y) is R = x, G = y, B = 0x80, A = 0xff, written through the documented
   tile (RGBA8 is stored A, B, G, R) */
static void tiled_image(unsigned char* in) {
    unsigned x, y;
    for (y = 0; y < 8; ++y)
        for (x = 0; x < 8; ++x) {
            unsigned char* p = in + texel(x, y) * 4;
            p[0] = 0xff, p[1] = 0x80, p[2] = (unsigned char)y, p[3] = (unsigned char)x;
        }
}

static void display(void) {
    unsigned char in[8 * 8 * 4], out[8 * 8 * 4];
    size_t need_in = 0, need_out = 0;
    unsigned x, y, ok;
    SnxCtrDisplayTransfer t;
    tiled_image(in);
    /* tiled RGBA8 in, linear RGB8 out (stored B, G, R) */
    t.input_size = 8 | (8u << 16);
    t.output_size = 8 | (8u << 16);
    t.flags = (SNX_CTR_PIX_RGBA8 << SNX_CTR_XFER_IN_FORMAT) | (SNX_CTR_PIX_RGB8 << SNX_CTR_XFER_OUT_FORMAT);
    check(snx_ctr_display_spans(&t, &need_in, &need_out) && need_in == 256 && need_out == 192,
          "spans of an 8x8 RGBA8 to RGB8 transfer");
    memset(out, 0, sizeof out);
    check(snx_ctr_display_transfer(&t, in, need_in, out, need_out) == SNX_CTR_XFER_OK, "tiled to linear runs");
    ok = 1;
    for (y = 0; y < 8; ++y)
        for (x = 0; x < 8; ++x) {
            const unsigned char* p = out + (y * 8 + x) * 3;
            ok &= p[0] == 0x80 && p[1] == y && p[2] == x;
        }
    check(ok, "tiled RGBA8 becomes linear BGR bytes, row by row");
    /* the same, flipped vertically: output row y is input row 7 - y */
    t.flags |= SNX_CTR_XFER_FLIP;
    check(snx_ctr_display_transfer(&t, in, need_in, out, need_out) == SNX_CTR_XFER_OK, "flipped transfer runs");
    ok = 1;
    for (y = 0; y < 8; ++y)
        for (x = 0; x < 8; ++x) ok &= out[(y * 8 + x) * 3 + 1] == 7 - y;
    check(ok, "FLIP reverses the rows");
    /* RGB565 out: R = x (5 bits: x >> 3 == 0), G = y >> 2, B = 0x80 >> 3 = 16 */
    t.flags = (SNX_CTR_PIX_RGBA8 << SNX_CTR_XFER_IN_FORMAT) | (SNX_CTR_PIX_RGB565 << SNX_CTR_XFER_OUT_FORMAT);
    check(snx_ctr_display_transfer(&t, in, sizeof in, out, sizeof out) == SNX_CTR_XFER_OK, "RGB565 transfer runs");
    check(out[(7 * 8 + 3) * 2] == (unsigned char)(((7 >> 2) << 5) | 16) && out[(7 * 8 + 3) * 2 + 1] == 0,
          "RGB565 packs R:5 G:6 B:5 little-endian");
    /* ScaleX: two pixels averaged with truncation, half the width. R of pixels x=0,1 is 0 and 1: (0+1)/2 = 0;
       pixels 2,3 give (2+3)/2 = 2 */
    t.output_size = 8 | (8u << 16);
    t.flags = (SNX_CTR_PIX_RGBA8 << SNX_CTR_XFER_IN_FORMAT) | (SNX_CTR_PIX_RGBA8 << SNX_CTR_XFER_OUT_FORMAT) |
              (1u << SNX_CTR_XFER_SCALE_SHIFT);
    check(snx_ctr_display_spans(&t, &need_in, &need_out) && need_out == 4 * 8 * 4, "ScaleX halves the width");
    check(snx_ctr_display_transfer(&t, in, sizeof in, out, sizeof out) == SNX_CTR_XFER_OK, "ScaleX runs");
    check(out[3] == 0 && out[4 + 3] == 2 && out[2] == 0, "ScaleX box filter truncates (R of x=0,1 and 2,3; G of y=0)");
    /* ScaleXY: a 2x2 block. Pixels (0,0),(1,0),(0,1),(1,1): G = 0,0,1,1 -> 2/4 = 0; R = 0,1,0,1 -> 0 */
    t.flags = (t.flags & ~(3u << SNX_CTR_XFER_SCALE_SHIFT)) | (2u << SNX_CTR_XFER_SCALE_SHIFT);
    check(snx_ctr_display_spans(&t, &need_in, &need_out) && need_out == 4 * 4 * 4, "ScaleXY halves both sides");
    check(snx_ctr_display_transfer(&t, in, sizeof in, out, sizeof out) == SNX_CTR_XFER_OK, "ScaleXY runs");
    /* output (1,1) averages input (2,2),(3,2),(2,3),(3,3): R = (2+3+2+3)/4 = 2, G = (2+2+3+3)/4 = 2 */
    check(out[(1 * 4 + 1) * 4 + 3] == 2 && out[(1 * 4 + 1) * 4 + 2] == 2, "ScaleXY averages a 2x2 block");
    /* the spans are remembered by the transfer's sizes and flags (eight of them): more transfers than that, asked
       again in another order, get the same spans, an unsupported one (scaling mode 3) refused each time */
    {
        size_t first[10][2], again[2];
        for (unsigned i = 0; i < 10; ++i) {
            SnxCtrDisplayTransfer u = {(8 + 8 * i) | (16u << 16), (8 + 8 * i) | (8u << 16),
                                       (SNX_CTR_PIX_RGBA8 << SNX_CTR_XFER_IN_FORMAT) |
                                           ((i & 1 ? SNX_CTR_PIX_RGB565 : SNX_CTR_PIX_RGB8) << SNX_CTR_XFER_OUT_FORMAT) |
                                           (i & 2 ? SNX_CTR_XFER_FLIP : 0u)};
            check(snx_ctr_display_spans(&u, &first[i][0], &first[i][1]), "spans of a transfer to remember");
        }
        ok = 1;
        for (unsigned i = 10; i-- > 0;) {
            SnxCtrDisplayTransfer u = {(8 + 8 * i) | (16u << 16), (8 + 8 * i) | (8u << 16),
                                       (SNX_CTR_PIX_RGBA8 << SNX_CTR_XFER_IN_FORMAT) |
                                           ((i & 1 ? SNX_CTR_PIX_RGB565 : SNX_CTR_PIX_RGB8) << SNX_CTR_XFER_OUT_FORMAT) |
                                           (i & 2 ? SNX_CTR_XFER_FLIP : 0u)};
            ok &= snx_ctr_display_spans(&u, &again[0], &again[1]) && again[0] == first[i][0] &&
                  again[1] == first[i][1];
        }
        check(ok, "remembered spans are the ones walked");
        SnxCtrDisplayTransfer bad = {8 | (8u << 16), 8 | (8u << 16), 3u << SNX_CTR_XFER_SCALE_SHIFT};
        check(!snx_ctr_display_spans(&bad, &again[0], &again[1]) && !snx_ctr_display_spans(&bad, &again[0], &again[1]),
              "an unsupported transfer has no spans, remembered or not");
    }
    /* linear in, tiled out reverses the first transfer */
    {
        unsigned char lin[8 * 8 * 4], back[8 * 8 * 4];
        for (y = 0; y < 8; ++y)
            for (x = 0; x < 8; ++x) {
                unsigned char* p = lin + (y * 8 + x) * 4;
                p[0] = 0xff, p[1] = 0x80, p[2] = (unsigned char)y, p[3] = (unsigned char)x;
            }
        t.flags = SNX_CTR_XFER_LINEAR | (SNX_CTR_PIX_RGBA8 << SNX_CTR_XFER_IN_FORMAT) |
                  (SNX_CTR_PIX_RGBA8 << SNX_CTR_XFER_OUT_FORMAT);
        check(snx_ctr_display_transfer(&t, lin, sizeof lin, back, sizeof back) == SNX_CTR_XFER_OK, "linear to tiled");
        check(!memcmp(back, in, sizeof in), "linear to tiled is the inverse of tiled to linear");
    }
    /* refusals: scaling mode 3, scaled linear input, zero sizes, a buffer too small */
    t.flags = 3u << SNX_CTR_XFER_SCALE_SHIFT;
    check(snx_ctr_display_transfer(&t, in, sizeof in, out, sizeof out) == SNX_CTR_XFER_UNSUPPORTED,
          "scaling mode 3 is refused");
    t.flags = SNX_CTR_XFER_LINEAR | (1u << SNX_CTR_XFER_SCALE_SHIFT);
    check(snx_ctr_display_transfer(&t, in, sizeof in, out, sizeof out) == SNX_CTR_XFER_UNSUPPORTED,
          "scaled linear input is refused");
    t.flags = 0;
    t.input_size = 8;
    check(snx_ctr_display_transfer(&t, in, sizeof in, out, sizeof out) == SNX_CTR_XFER_EMPTY, "zero height is empty");
    t.input_size = 8 | (8u << 16);
    check(snx_ctr_display_transfer(&t, in, 100, out, sizeof out) == SNX_CTR_XFER_BOUNDS,
          "a short input buffer stops the transfer");
    t.flags = 5u << SNX_CTR_XFER_IN_FORMAT;
    check(snx_ctr_display_transfer(&t, in, sizeof in, out, sizeof out) == SNX_CTR_XFER_UNSUPPORTED,
          "an undefined pixel format is refused");
    /* FLIP with CROP moves the output by (in_w - out_w) * (out_h - 1) pixels: 16 wide in, 8 out, 8 high, RGB8 */
    t.input_size = 16 | (8u << 16);
    t.output_size = 8 | (8u << 16);
    t.flags = SNX_CTR_XFER_FLIP | SNX_CTR_XFER_CROP | (SNX_CTR_PIX_RGB8 << SNX_CTR_XFER_OUT_FORMAT);
    check(snx_ctr_display_spans(&t, &need_in, &need_out) && need_out == (16 - 8) * 7 * 3 + 8 * 8 * 3,
          "FLIP with CROP skews the output as the hardware does");
}

static void texture_copy(void) {
    unsigned char in[96], out[96], expect[96];
    size_t need_in, need_out, i;
    SnxCtrTextureCopy t;
    for (i = 0; i < sizeof in; ++i) in[i] = (unsigned char)i;
    /* 64 bytes: input lines of 16 bytes with a 16-byte gap, output contiguous (gap 0) */
    t.size = 64 | 0xf; /* the low 4 bits are ignored */
    t.input_line = 1 | (1u << 16);
    t.output_line = 0;
    check(snx_ctr_texture_spans(&t, &need_in, &need_out) && need_in == 16 * 7 && need_out == 64,
          "a gapped input reaches past its data by the gaps");
    memset(out, 0xee, sizeof out);
    check(snx_ctr_texture_copy(&t, in, sizeof in, out, sizeof out) == SNX_CTR_XFER_BOUNDS,
          "an input buffer short of the last line stops");
    {
        unsigned char big[112];
        for (i = 0; i < sizeof big; ++i) big[i] = (unsigned char)i;
        check(snx_ctr_texture_copy(&t, big, sizeof big, out, sizeof out) == SNX_CTR_XFER_OK, "gapped copy runs");
        for (i = 0; i < 64; ++i) expect[i] = (unsigned char)((i / 16) * 32 + i % 16);
        check(!memcmp(out, expect, 64) && out[64] == 0xee, "every other 16-byte line, packed");
    }
    /* output gaps are skipped, not written: output lines of 32 bytes with a 16-byte gap */
    t.size = 64;
    t.input_line = 0;
    t.output_line = 2 | (1u << 16);
    memset(out, 0xee, sizeof out);
    check(snx_ctr_texture_copy(&t, in, sizeof in, out, sizeof out) == SNX_CTR_XFER_OK, "gapped output runs");
    check(!memcmp(out, in, 32) && out[32] == 0xee && out[47] == 0xee && !memcmp(out + 48, in + 32, 32),
          "the output gap keeps its bytes");
    t.size = 8;
    check(snx_ctr_texture_copy(&t, in, sizeof in, out, sizeof out) == SNX_CTR_XFER_EMPTY,
          "a size under 16 bytes is empty (hardware freezes)");
    t.size = 64;
    t.input_line = 1u << 16; /* a gap with a zero width */
    check(snx_ctr_texture_copy(&t, in, sizeof in, out, sizeof out) == SNX_CTR_XFER_EMPTY, "zero width with a gap");
}

int main(void) {
    check(snx_ctr_pixel_bytes(0) == 4 && snx_ctr_pixel_bytes(1) == 3 && snx_ctr_pixel_bytes(4) == 2 &&
              !snx_ctr_pixel_bytes(5),
          "pixel sizes");
    morton();
    fills();
    display();
    texture_copy();
    if (failures) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    puts("3DS transfer engine: ok");
    return 0;
}
