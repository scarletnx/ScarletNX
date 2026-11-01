/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* BC1 to BC7 blocks built by hand, against values worked out from the Khronos Data Format Specification's S3TC, RGTC
   and BPTC chapters, with BC1 to BC5's interpolants as NVIDIA's GPUs decode them (support/bcn.c) */
#include "support/bcn.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <string.h>

static int failures;

/* a block written a field at a time from bit 0 up, as BPTC's are laid out */
typedef struct {
    u8 b[16];
    u32 at;
} Put;
static void put(Put* p, u32 v, u32 n) {
    for (u32 i = 0; i < n; i++, p->at++)
        if (v >> i & 1) p->b[p->at / 8] |= (u8)(1u << p->at % 8);
}
static void put_le(u8* p, u64 v) {
    for (int i = 0; i < 8; i++) p[i] = (u8)(v >> 8 * i);
}

/* the decoded pixel (x, y), as bcn_pixel_bytes bytes, against want's bytes */
static void pixel(const u8* blk, int kind, u32 x, u32 y, const u8* want, const char* what) {
    u8 px[16 * 8];
    bcn_decode_image(blk, 4, 4, kind, px);
    u32 n = bcn_pixel_bytes(kind);
    const u8* p = px + (y * 4 + x) * n;
    if (memcmp(p, want, n)) {
        fprintf(stderr, "FAIL: %s: (%u,%u) is", what, x, y);
        for (u32 i = 0; i < n; i++) fprintf(stderr, " %02x", p[i]);
        fprintf(stderr, ", expected");
        for (u32 i = 0; i < n; i++) fprintf(stderr, " %02x", want[i]);
        fprintf(stderr, "\n");
        failures++;
    }
}
static void rgba(const u8* blk, int kind, u32 x, u32 y, int r, int g, int b, int a, const char* what) {
    u8 want[4] = {(u8)r, (u8)g, (u8)b, (u8)a};
    pixel(blk, kind, x, y, want, what);
}
/* 16 bit channels (BC4 and BC5's, BC6H's halves) little-endian */
static void ch16(const u8* blk, int kind, u32 x, u32 y, const u16* want, const char* what) {
    u8 w[8];
    for (u32 i = 0; i < bcn_pixel_bytes(kind) / 2; i++) snx_st_le16(w + 2 * i, want[i]);
    pixel(blk, kind, x, y, w, what);
}

int main(void) {
    u8 blk[16];

    /* BC1, four colors: red (0xF800) above blue (0x001F); pixels 0 to 3 take indices 0 to 3, the interpolants of the
       5 bit endpoints (2 * 31 + 0) * 22 / 8 = 170 and 31 * 22 / 8 = 85 */
    put_le(blk, 0xE4ull << 32 | 0x001Fu << 16 | 0xF800u);
    rgba(blk, BCN_BC1, 0, 0, 255, 0, 0, 255, "BC1 four colors, index 0");
    rgba(blk, BCN_BC1, 1, 0, 0, 0, 255, 255, "BC1 four colors, index 1");
    rgba(blk, BCN_BC1, 2, 0, 170, 0, 85, 255, "BC1 four colors, index 2");
    rgba(blk, BCN_BC1, 3, 0, 85, 0, 170, 255, "BC1 four colors, index 3");
    /* three colors: the first endpoint the smaller; index 2 the midpoint, (0 + 31) * 33 / 8 = 127, index 3
       transparent black */
    put_le(blk, 0xE4ull << 32 | 0xF800u << 16 | 0x001Fu);
    rgba(blk, BCN_BC1, 2, 0, 127, 0, 127, 255, "BC1 three colors, index 2");
    rgba(blk, BCN_BC1, 3, 0, 0, 0, 0, 0, "BC1 three colors, index 3");

    /* BC2: 4 bit alpha (15 and 8, times 17), and the same color block taken as four colors */
    put_le(blk, 0x8Full);
    put_le(blk + 8, 0xE4ull << 32 | 0xF800u << 16 | 0x001Fu);
    rgba(blk, BCN_BC2, 0, 0, 0, 0, 255, 255, "BC2 alpha 15, index 0");
    rgba(blk, BCN_BC2, 3, 0, 170, 0, 85, 0, "BC2 alpha 0, four colors in a block BC1 takes as three");
    put_le(blk, 0x8Full);
    rgba(blk, BCN_BC2, 1, 0, 255, 0, 0, 136, "BC2 alpha 8");

    /* BC3 alpha: 255 above 0, eight values: index 2 is 255 less 289/2048 of it, 219, index 7 289/2048 of it, 36 */
    put_le(blk, 7ull << 19 | 2ull << 16 | 0x00FFu);
    put_le(blk + 8, 0xF800u);
    rgba(blk, BCN_BC3, 0, 0, 255, 0, 0, 219, "BC3 eight alphas, index 2");
    rgba(blk, BCN_BC3, 1, 0, 255, 0, 0, 36, "BC3 eight alphas, index 7");
    /* 0 below 255, six values: index 2 is 385/2048 of 255, (1024 + 385 * 255) / 2048 = 48, 6 is 0 and 7 is 255 */
    put_le(blk, 7ull << 22 | 6ull << 19 | 2ull << 16 | 0xFF00u);
    rgba(blk, BCN_BC3, 0, 0, 255, 0, 0, 48, "BC3 six alphas, index 2");
    rgba(blk, BCN_BC3, 1, 0, 255, 0, 0, 0, "BC3 six alphas, index 6");
    rgba(blk, BCN_BC3, 2, 0, 255, 0, 0, 255, "BC3 six alphas, index 7");

    /* BC4: 221/257 of 255 * 257 (NVIDIA's weight in place of 6/7), 56355 */
    put_le(blk, 2ull << 16 | 0x00FFu);
    ch16(blk, BCN_BC4, 0, 0, (const u16[]){56355}, "BC4 unorm, index 2");
    ch16(blk, BCN_BC4, 1, 0, (const u16[]){65535}, "BC4 unorm, index 0");
    /* signed: 127 and -128 (read as -127); index 2 is (222 * 127 - 36 * 127) / 258, 186/258 of 32767 rounded: 23623 */
    put_le(blk, 1ull << 19 | 2ull << 16 | 0x807Fu);
    ch16(blk, BCN_BC4_SNORM, 0, 0, (const u16[]){23623}, "BC4 snorm, index 2");
    ch16(blk, BCN_BC4_SNORM, 1, 0, (const u16[]){0x8001}, "BC4 snorm, index 1: -32767");
    /* BC5: the second block is green */
    put_le(blk, 0x00FFu);
    put_le(blk + 8, 0xFF00u);
    ch16(blk, BCN_BC5, 2, 2, (const u16[]){65535, 0}, "BC5 unorm");

    /* BC6H mode 11 (code 3, one region of two 10 bit endpoints, not deltas): 0 and 1023, the largest, which
       unquantizes to 0xFFFF. Index 15 is the second endpoint: 0xFFFF * 31 / 64 is 0x7BFF, the largest finite half.
       Index 8 weighs it 34/64: (0xFFFF * 34 + 32) >> 6 = 34815, and 34815 * 31 >> 6 = 16863 (0x41DF) */
    {
        Put p = {{0}, 0};
        put(&p, 3, 5);
        put(&p, 0, 30);
        for (int c = 0; c < 3; c++) put(&p, 1023, 10);
        put(&p, 0, 3);  /* pixel 0, the anchor: 3 bits */
        put(&p, 15, 4); /* pixel 1 */
        put(&p, 8, 4);  /* pixel 2 */
        ch16(p.b, BCN_BC6H_UF, 0, 0, (const u16[]){0, 0, 0, 0x3C00}, "BC6H mode 11, index 0");
        ch16(p.b, BCN_BC6H_UF, 1, 0, (const u16[]){0x7BFF, 0x7BFF, 0x7BFF, 0x3C00}, "BC6H mode 11, index 15");
        ch16(p.b, BCN_BC6H_UF, 2, 0, (const u16[]){0x41DF, 0x41DF, 0x41DF, 0x3C00}, "BC6H mode 11, index 8");
        /* signed: -512 and 511 are past the largest magnitude, 511: -0x7FFF and 0x7FFF, which finish as the
           largest finite halves, -65504 (0xFBFF) and 65504 */
        memset(p.b, 0, 16);
        p.at = 0;
        put(&p, 3, 5);
        for (int c = 0; c < 3; c++) put(&p, 0x200, 10);
        for (int c = 0; c < 3; c++) put(&p, 0x1FF, 10);
        put(&p, 0, 3);
        put(&p, 15, 4);
        ch16(p.b, BCN_BC6H_SF, 0, 0, (const u16[]){0xFBFF, 0xFBFF, 0xFBFF, 0x3C00}, "BC6H signed, index 0");
        ch16(p.b, BCN_BC6H_SF, 1, 0, (const u16[]){0x7BFF, 0x7BFF, 0x7BFF, 0x3C00}, "BC6H signed, index 15");
    }
    /* a reserved BC6H mode (code 19) is black, alpha 1 */
    memset(blk, 0xFF, 16);
    blk[0] = 19;
    ch16(blk, BCN_BC6H_UF, 0, 0, (const u16[]){0, 0, 0, 0x3C00}, "BC6H reserved mode");

    /* BC7 mode 6: 7 bit endpoints and a p-bit each, 1: red 255 and 1, green 1 and 255, blue 1, alpha 255. Index 8
       weighs the second endpoint 34/64: red (255 * 30 + 34 + 32) >> 6 = 120, green 136 */
    {
        Put p = {{0}, 0};
        put(&p, 1u << 6, 7);
        put(&p, 127, 7), put(&p, 0, 7);   /* R */
        put(&p, 0, 7), put(&p, 127, 7);   /* G */
        put(&p, 0, 7), put(&p, 0, 7);     /* B */
        put(&p, 127, 7), put(&p, 127, 7); /* A */
        put(&p, 1, 1), put(&p, 1, 1);     /* p-bits */
        put(&p, 0, 3), put(&p, 15, 4), put(&p, 8, 4);
        rgba(p.b, BCN_BC7, 0, 0, 255, 1, 1, 255, "BC7 mode 6, index 0");
        rgba(p.b, BCN_BC7, 1, 0, 1, 255, 1, 255, "BC7 mode 6, index 15");
        rgba(p.b, BCN_BC7, 2, 0, 120, 136, 1, 255, "BC7 mode 6, index 8");
    }
    /* BC7 mode 5 with rotation 1: alpha (8 bits, 200 for both endpoints) swapped with red (7 bits, 0 and 127 to
       0 and 255); index 0 for color, alpha's own indices 0 */
    {
        Put p = {{0}, 0};
        put(&p, 1u << 5, 6);
        put(&p, 1, 2);                  /* rotation: A and R */
        put(&p, 0, 7), put(&p, 127, 7); /* R */
        put(&p, 0, 14), put(&p, 0, 14); /* G, B */
        put(&p, 200, 8), put(&p, 200, 8);
        rgba(p.b, BCN_BC7, 0, 0, 200, 0, 0, 0, "BC7 mode 5, A and R swapped");
    }
    /* a BC7 block without a mode is transparent black */
    memset(blk, 0, 16);
    blk[5] = 0xFF;
    rgba(blk, BCN_BC7, 1, 1, 0, 0, 0, 0, "BC7 without a mode");

    if (failures) return 1;
    puts("BC1 three and four colors, BC2, BC3 alpha, BC4 and BC5 unsigned and signed, BC6H unsigned, signed and "
         "reserved, BC7 modes 5 and 6 and none: as specified");
    return 0;
}
