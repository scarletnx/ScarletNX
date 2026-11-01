/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* BC1 to BC7 decoding, from the Khronos Data Format Specification's S3TC, RGTC and BPTC chapters. A block is
   little-endian; pixel (x, y) of a 4x4 block is number y * 4 + x (rows first). Where the specification leaves the
   precision of an interpolated value to the implementation (BC1 to BC5) it is NVIDIA's, or rounded to nearest */
#include "bcn.h"
#include "common/utilities/endian.h"
#include <string.h>

/* a 128 bit block as two words, and its bits from bit at on, n of them (n up to 32) */
typedef struct {
    u64 w[2];
    u32 at; /* the next bit a read takes */
} Bits;
static u32 peek(const Bits* b, u32 at, u32 n) {
    u64 v = at >= 64 ? b->w[1] >> (at - 64) : at == 0 ? b->w[0] : b->w[0] >> at | b->w[1] << (64 - at);
    return (u32)(v & ((1ull << n) - 1));
}
static u32 take(Bits* b, u32 n) {
    u32 v = peek(b, b->at, n);
    b->at += n;
    return v;
}
static u64 le64(const u8* p) {
    u64 v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

/* ---- BC1 to BC5 ----
   The specification leaves the precision of the interpolants to the implementation. The Switch's GPU is NVIDIA's,
   and its decode is the one a game's textures were made for: an NVIDIA GPU here (an RTX 2080) gives these values for
   every pair of endpoints, in each mode (tests/astc_vulkan.c's native case). Its BC1 red and blue interpolate the 5
   bit endpoints, its green the 8 bit ones by a weight of 321/1024 in place of 1/3 (as rgbcx's BC1 decoder for
   NVIDIA). Its alpha and BC4 weights are not sevenths or fifths: BC4 interpolates in 257ths (258ths signed), BC3's
   alpha in 2048ths from the nearer endpoint. Other GPUs round the exact value (spec: the decode of
   bcn_decode_image_spec) */

static int expand(u32 v, int bits) { return (int)(v << (8 - bits) | v >> (2 * bits - 8)); }

/* a BC1 color block into out[pixel][0..2], and alpha 0 or 255. three: a block whose first endpoint isn't the larger
   has three colors and a transparent black (BC1); BC2 and BC3 take every block as four colors */
static void bc1_block(u64 b, int three, int spec, u8 out[16][4]) {
    u32 e0 = (u32)b & 0xFFFF, e1 = (u32)(b >> 16) & 0xFFFF, idx = (u32)(b >> 32);
    int c[4][4];
    int q[2][3] = {{(int)(e0 >> 11), (int)(e0 >> 5 & 63), (int)(e0 & 31)},
                   {(int)(e1 >> 11), (int)(e1 >> 5 & 63), (int)(e1 & 31)}};
    int ends[2][3] = {{expand(e0 >> 11, 5), expand(e0 >> 5 & 63, 6), expand(e0 & 31, 5)},
                      {expand(e1 >> 11, 5), expand(e1 >> 5 & 63, 6), expand(e1 & 31, 5)}};
    int d = ends[1][1] - ends[0][1], four = e0 > e1 || !three;
    for (int k = 0; k < 3; k++) {
        c[0][k] = ends[0][k];
        c[1][k] = ends[1][k];
        c[3][k] = 0;
        if (spec && four) {
            c[2][k] = (2 * ends[0][k] + ends[1][k] + 1) / 3;
            c[3][k] = (ends[0][k] + 2 * ends[1][k] + 1) / 3;
        } else if (spec)
            c[2][k] = (ends[0][k] + ends[1][k] + 1) / 2;
        else if (four && k == 1) {
            c[2][k] = (256 * ends[0][1] + d / 4 + 128 + d * 80) / 256;
            c[3][k] = (256 * ends[1][1] - d / 4 + 128 - d * 80) / 256;
        } else if (four) {
            c[2][k] = (2 * q[0][k] + q[1][k]) * 22 / 8;
            c[3][k] = (q[0][k] + 2 * q[1][k]) * 22 / 8;
        } else
            c[2][k] = k == 1 ? (256 * ends[0][1] + d / 4 + 128 + d * 128) / 256 : (q[0][k] + q[1][k]) * 33 / 8;
    }
    c[0][3] = c[1][3] = c[2][3] = 255;
    c[3][3] = four ? 255 : 0;
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 4; k++) out[i][k] = (u8)c[idx >> 2 * i & 3][k];
}
/* NVIDIA's BC4 weights of the second endpoint for indices 2 to 7: eight values (the first endpoint the larger), then
   six. Unsigned they are 257ths, signed 258ths (each signed one but the low two is one more) */
static const int bc4_w[2][2][6] = {{{36, 72, 113, 144, 185, 221}, {48, 96, 161, 209}},
                                   {{36, 72, 114, 144, 186, 222}, {48, 96, 162, 210}}};

/* a BC4 block's 16 values as 16 bit unorm or (sn) snorm. spec: the interpolants are taken to 16 bits from their exact
   value (a sum of sevenths or fifths of the endpoints), rounded to nearest. NVIDIA's unsigned ones are exact in
   257ths, its signed ones within one step of these 258ths rounded */
static void bc4_block(u64 b, int sn, int spec, u16 out[16]) {
    int a0 = (int)(b & 0xFF), a1 = (int)(b >> 8 & 0xFF);
    if (sn) {
        a0 = (int)(i8)a0;
        a1 = (int)(i8)a1;
        if (a0 == -128) a0 = -127;
        if (a1 == -128) a1 = -127;
    }
    int eight = sn ? a0 > a1 : (u32)a0 > (u32)a1;
    for (int i = 0; i < 16; i++) {
        int k = (int)(b >> (16 + 3 * i) & 7), x, d; /* the value is x / d of the endpoints' range */
        if (k == 0)
            x = a0, d = 1;
        else if (k == 1)
            x = a1, d = 1;
        else if (!eight && k >= 6)
            x = k == 6 ? (sn ? -127 : 0) : (sn ? 127 : 255), d = 1;
        else if (!spec) {
            int w = bc4_w[sn][!eight][k - 2];
            x = (sn ? 258 - w : 257 - w) * a0 + w * a1, d = sn ? 258 : 257;
        } else if (eight)
            x = (8 - k) * a0 + (k - 1) * a1, d = 7;
        else
            x = (6 - k) * a0 + (k - 1) * a1, d = 5;
        if (!sn)
            out[i] = (u16)((x * 257 + d / 2) / d);
        else {
            int m = x < 0 ? -x : x, v = (m * 32767 + 127 * d / 2) / (127 * d);
            out[i] = (u16)(i16)(x < 0 ? -v : v);
        }
    }
}
/* NVIDIA's BC3 alpha weights in 2048ths, by the steps from the nearer endpoint: eight values, then six */
static const int bc3_w[2][4] = {{0, 289, 578, 892}, {0, 385, 770}};

/* BC3's alpha: a BC4 block to 8 bits. spec: rounded to nearest */
static void bc3_alpha(u64 b, int spec, u8 out[16][4]) {
    int a0 = (int)(b & 0xFF), a1 = (int)(b >> 8 & 0xFF), n = a0 > a1 ? 7 : 5;
    for (int i = 0; i < 16; i++) {
        int k = (int)(b >> (16 + 3 * i) & 7), v;
        if (k < 2)
            v = k ? a1 : a0;
        else if (n == 5 && k >= 6)
            v = k == 6 ? 0 : 255;
        else if (!spec && 2 * (k - 1) < n) /* the nearer one is a0, k - 1 steps away; never negative */
            v = (2048 * a0 + 1024 + bc3_w[n == 5][k - 1] * (a1 - a0)) / 2048;
        else if (!spec)
            v = (2048 * a1 + 1024 + bc3_w[n == 5][n + 1 - k] * (a0 - a1)) / 2048;
        else if (n == 7)
            v = ((8 - k) * a0 + (k - 1) * a1 + 3) / 7;
        else
            v = ((6 - k) * a0 + (k - 1) * a1 + 2) / 5;
        out[i][3] = (u8)v;
    }
}

/* ---- BPTC: the partitions and weights BC6H and BC7 share ---- */

/* the subset (1) of each pixel of the 64 two subset partitions, a bit a pixel */
static const u16 part2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8,
    0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110,
    0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696,
    0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720,
    0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};
/* the subset of each pixel of the 64 three subset partitions */
static const u8 part3[64][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2},
    {0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2}, {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2},
    {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2}, {0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2},
    {0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2}, {0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0},
    {0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2}, {0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0},
    {0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0}, {0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2},
    {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0}, {0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1},
    {0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2}, {0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2},
    {0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1}, {0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2}, {0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0}, {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0},
    {0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0}, {0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1}, {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1}, {0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2},
    {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1}, {0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1}, {0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2}, {0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1},
    {0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2}, {0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2}, {0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2},
    {0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2},
    {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1}, {0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0}};
/* the anchor pixel of subset 1 of each two subset partition, and of subsets 1 and 2 of each three subset one (subset
   0's is pixel 0). An anchor's index has its top bit left out, 0 */
static const u8 anchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2, 8,
                               8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8,  2, 2,
                               2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15};
static const u8 anchor3[2][64] = {
    { 3,  3, 15, 15,  8,  3, 15, 15,  8,  8,  6,  6,  6,  5,  3,  3,
      3,  3,  8, 15,  3,  3,  6, 10,  5,  8,  8,  6,  8,  5, 15, 15,
      8, 15,  3,  5,  6, 10,  8, 15, 15,  3, 15,  5, 15, 15, 15, 15,
      3, 15,  5,  5,  5,  8,  5, 10,  5, 10,  8, 13, 15, 12,  3,  3},
    {15,  8,  8,  3, 15, 15,  3,  8, 15, 15, 15, 15, 15, 15, 15,  8,
     15,  8, 15,  3, 15,  8, 15,  8,  3, 15,  6, 10, 15, 15, 10,  8,
     15,  3, 15, 10, 10,  8,  9, 10,  6, 15,  8, 15,  3,  6,  6,  8,
     15,  3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  3, 15, 15,  8}};
static const u8 weights2[4] = {0, 21, 43, 64};
static const u8 weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
static const u8 weights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
static const u8* weights(u32 bits) { return bits == 2 ? weights2 : bits == 3 ? weights3 : weights4; }
static int lerp64(int a, int b, int w) { return (a * (64 - w) + b * w + 32) >> 6; }

/* the subset of pixel i of partition p of a block of ns subsets, and whether i is its subset's anchor */
static u32 subset(u32 ns, u32 p, u32 i) { return ns == 1 ? 0 : ns == 2 ? (u32)(part2[p] >> i & 1) : part3[p][i]; }
static int is_anchor(u32 ns, u32 p, u32 i) {
    return i == 0 || (ns == 2 && i == anchor2[p]) || (ns == 3 && (i == anchor3[0][p] || i == anchor3[1][p]));
}
/* 16 indices of bits bits each (anchors one less) from bit at on */
static void indices(Bits* b, u32 ns, u32 p, u32 bits, u8 out[16]) {
    for (u32 i = 0; i < 16; i++) out[i] = (u8)take(b, bits - (u32)is_anchor(ns, p, i));
}

/* ---- BC6H ---- */

/* each mode after its mode bits, the endpoints' fields in the order the block holds them: endpoint (r0 to b3: color
   r, g or b of endpoint 0 to 3), then [hi:lo], bit hi at the end of the field and lo at its start (a reversed field,
   [10:15], begins with its high bit), or one bit as [n] */
typedef struct {
    u8 code, code_bits, regions, transformed, epb, delta[3];
    const char* fields;
} Bc6Mode;
static const Bc6Mode bc6_modes[14] = {
    {0, 2, 2, 1, 10, {5, 5, 5},
     "g2[4] b2[4] b3[4] r0[9:0] g0[9:0] b0[9:0] r1[4:0] g3[4] g2[3:0] g1[4:0] b3[0] g3[3:0] b1[4:0] b3[1] b2[3:0] "
     "r2[4:0] b3[2] r3[4:0] b3[3]"},
    {1, 2, 2, 1, 7, {6, 6, 6},
     "g2[5] g3[4] g3[5] r0[6:0] b3[0] b3[1] b2[4] g0[6:0] b2[5] b3[2] g2[4] b0[6:0] b3[3] b3[5] b3[4] r1[5:0] "
     "g2[3:0] g1[5:0] g3[3:0] b1[5:0] b2[3:0] r2[5:0] r3[5:0]"},
    {2, 5, 2, 1, 11, {5, 4, 4},
     "r0[9:0] g0[9:0] b0[9:0] r1[4:0] r0[10] g2[3:0] g1[3:0] g0[10] b3[0] g3[3:0] b1[3:0] b0[10] b3[1] b2[3:0] "
     "r2[4:0] b3[2] r3[4:0] b3[3]"},
    {6, 5, 2, 1, 11, {4, 5, 4},
     "r0[9:0] g0[9:0] b0[9:0] r1[3:0] r0[10] g3[4] g2[3:0] g1[4:0] g0[10] g3[3:0] b1[3:0] b0[10] b3[1] b2[3:0] "
     "r2[3:0] b3[0] b3[2] r3[3:0] g2[4] b3[3]"},
    {10, 5, 2, 1, 11, {4, 4, 5},
     "r0[9:0] g0[9:0] b0[9:0] r1[3:0] r0[10] b2[4] g2[3:0] g1[3:0] g0[10] b3[0] g3[3:0] b1[4:0] b0[10] b2[3:0] "
     "r2[3:0] b3[1] b3[2] r3[3:0] b3[4] b3[3]"},
    {14, 5, 2, 1, 9, {5, 5, 5},
     "r0[8:0] b2[4] g0[8:0] g2[4] b0[8:0] b3[4] r1[4:0] g3[4] g2[3:0] g1[4:0] b3[0] g3[3:0] b1[4:0] b3[1] b2[3:0] "
     "r2[4:0] b3[2] r3[4:0] b3[3]"},
    {18, 5, 2, 1, 8, {6, 5, 5},
     "r0[7:0] g3[4] b2[4] g0[7:0] b3[2] g2[4] b0[7:0] b3[3] b3[4] r1[5:0] g2[3:0] g1[4:0] b3[0] g3[3:0] b1[4:0] "
     "b3[1] b2[3:0] r2[5:0] r3[5:0]"},
    {22, 5, 2, 1, 8, {5, 6, 5},
     "r0[7:0] b3[0] b2[4] g0[7:0] g2[5] g2[4] b0[7:0] g3[5] b3[4] r1[4:0] g3[4] g2[3:0] g1[5:0] g3[3:0] b1[4:0] "
     "b3[1] b2[3:0] r2[4:0] b3[2] r3[4:0] b3[3]"},
    {26, 5, 2, 1, 8, {5, 5, 6},
     "r0[7:0] b3[1] b2[4] g0[7:0] b2[5] g2[4] b0[7:0] b3[5] b3[4] r1[4:0] g3[4] g2[3:0] g1[4:0] b3[0] g3[3:0] "
     "b1[5:0] b2[3:0] r2[4:0] b3[2] r3[4:0] b3[3]"},
    {30, 5, 2, 0, 6, {6, 6, 6},
     "r0[5:0] g3[4] b3[0] b3[1] b2[4] g0[5:0] g2[5] b2[5] b3[2] g2[4] b0[5:0] g3[5] b3[3] b3[5] b3[4] r1[5:0] "
     "g2[3:0] g1[5:0] g3[3:0] b1[5:0] b2[3:0] r2[5:0] r3[5:0]"},
    {3, 5, 1, 0, 10, {10, 10, 10}, "r0[9:0] g0[9:0] b0[9:0] r1[9:0] g1[9:0] b1[9:0]"},
    {7, 5, 1, 1, 11, {9, 9, 9}, "r0[9:0] g0[9:0] b0[9:0] r1[8:0] r0[10] g1[8:0] g0[10] b1[8:0] b0[10]"},
    {11, 5, 1, 1, 12, {8, 8, 8}, "r0[9:0] g0[9:0] b0[9:0] r1[7:0] r0[10:11] g1[7:0] g0[10:11] b1[7:0] b0[10:11]"},
    {15, 5, 1, 1, 16, {4, 4, 4}, "r0[9:0] g0[9:0] b0[9:0] r1[3:0] r0[10:15] g1[3:0] g0[10:15] b1[3:0] b0[10:15]"},
};

static int num(const char** s) {
    int v = 0;
    while (**s >= '0' && **s <= '9') v = v * 10 + *(*s)++ - '0';
    return v;
}
/* the mode's endpoints, e[endpoint][channel], from the block's fields */
static void bc6_fields(Bits* b, const char* s, int e[4][3]) {
    memset(e, 0, sizeof(int[4][3]));
    while (*s) {
        int ch = *s == 'r' ? 0 : *s == 'g' ? 1 : 2;
        s++;
        int ep = num(&s);
        s++; /* [ */
        int hi = num(&s), lo = hi;
        if (*s == ':') {
            s++;
            lo = num(&s);
        }
        s += *s == ']' ? 1 : 0;
        while (*s == ' ') s++;
        /* the field's first bit is lo, its last hi: lo above hi is a field read from its high bit down */
        for (int bit = lo;; bit += hi >= lo ? 1 : -1) {
            e[ep][ch] |= (int)take(b, 1) << bit;
            if (bit == hi) break;
        }
    }
}
static int sext(int v, int bits) { return (int)((u32)v << (32 - bits)) >> (32 - bits); }
static int bc6_unquantize(int v, int epb, int sf) {
    if (!sf) {
        if (epb >= 15) return v;
        if (v == 0) return 0;
        if (v == (1 << epb) - 1) return 0xFFFF;
        return ((v << 16) + 0x8000) >> epb;
    }
    if (epb >= 16) return v;
    int neg = v < 0, m = neg ? -v : v, u;
    if (m == 0)
        u = 0;
    else if (m >= (1 << (epb - 1)) - 1)
        u = 0x7FFF;
    else
        u = ((m << 15) + 0x4000) >> (epb - 1);
    return neg ? -u : u;
}
/* an interpolated value to the bits of a half float: 31/64 of the unsigned range, 31/32 of the signed one's
   magnitude */
static u16 bc6_finish(int v, int sf) {
    if (!sf) return (u16)(v * 31 >> 6);
    return (u16)(v < 0 ? 0x8000 | (-v * 31 >> 5) : v * 31 >> 5);
}
/* a BC6H block into out[pixel][r, g, b, a] as half floats, alpha 1.0. A reserved mode is black */
static void bc6_block(const u8* blk, int sf, u16 out[16][4]) {
    Bits b = {{le64(blk), le64(blk + 8)}, 0};
    u32 code = peek(&b, 0, 2) < 2 ? peek(&b, 0, 2) : peek(&b, 0, 5);
    const Bc6Mode* m = NULL;
    for (int i = 0; i < 14; i++)
        if (bc6_modes[i].code == code && (bc6_modes[i].code_bits == 2) == (code < 2)) m = &bc6_modes[i];
    for (int i = 0; i < 16; i++) {
        out[i][0] = out[i][1] = out[i][2] = 0;
        out[i][3] = 0x3C00;
    }
    if (!m) return;
    b.at = m->code_bits;
    int e[4][3];
    bc6_fields(&b, m->fields, e);
    u32 ns = m->regions, p = ns == 2 ? take(&b, 5) : 0, nend = ns * 2;
    for (int c = 0; c < 3; c++) {
        if (sf) e[0][c] = sext(e[0][c], m->epb);
        for (u32 k = 1; k < nend; k++) {
            if (sf || m->transformed) e[k][c] = sext(e[k][c], m->transformed ? m->delta[c] : m->epb);
            if (m->transformed) {
                e[k][c] = (e[0][c] + e[k][c]) & ((1 << m->epb) - 1);
                if (sf) e[k][c] = sext(e[k][c], m->epb);
            }
        }
        for (u32 k = 0; k < nend; k++) e[k][c] = bc6_unquantize(e[k][c], m->epb, sf);
    }
    u32 ib = ns == 2 ? 3 : 4;
    u8 idx[16];
    indices(&b, ns, p, ib, idx);
    for (u32 i = 0; i < 16; i++) {
        u32 s = subset(ns, p, i);
        for (int c = 0; c < 3; c++)
            out[i][c] = bc6_finish(lerp64(e[2 * s][c], e[2 * s + 1][c], weights(ib)[idx[i]]), sf);
    }
}

/* ---- BC7 ---- */

typedef struct {
    u8 ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2;
} Bc7Mode;
/* subsets, partition bits, rotation bits, index selection bits, color and alpha bits, endpoint and shared p-bits,
   index bits and the second index's */
static const Bc7Mode bc7_modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};

/* a BC7 block into out[pixel]. A block without a mode (its first byte 0) is transparent black */
static void bc7_block(const u8* blk, u8 out[16][4]) {
    Bits b = {{le64(blk), le64(blk + 8)}, 0};
    u32 mode = 0;
    while (mode < 8 && !take(&b, 1)) mode++;
    if (mode == 8) {
        memset(out, 0, 16 * 4);
        return;
    }
    const Bc7Mode* m = &bc7_modes[mode];
    u32 p = take(&b, m->pb), rot = take(&b, m->rb), isb = take(&b, m->isb), nend = m->ns * 2u;
    int e[6][4];
    for (int c = 0; c < 4; c++)
        for (u32 k = 0; k < nend; k++) {
            u32 bits = c < 3 ? m->cb : m->ab;
            e[k][c] = bits ? (int)take(&b, bits) : 255;
        }
    /* p-bits: one an endpoint, or one a subset its two share, below each channel's bits */
    u32 prec[2] = {m->cb, m->ab};
    if (m->epb || m->spb) {
        u32 pbit[6];
        for (u32 k = 0; k < nend; k++) pbit[k] = m->epb ? take(&b, 1) : 0;
        if (m->spb)
            for (u32 s = 0; s < m->ns; s++) pbit[2 * s] = pbit[2 * s + 1] = take(&b, 1);
        for (u32 k = 0; k < nend; k++)
            for (int c = 0; c < 4; c++)
                if (c < 3 || m->ab) e[k][c] = e[k][c] << 1 | (int)pbit[k];
        prec[0]++;
        if (m->ab) prec[1]++;
    }
    for (u32 k = 0; k < nend; k++)
        for (int c = 0; c < 4; c++) {
            u32 bits = prec[c < 3 ? 0 : 1];
            if (c < 3 || m->ab) e[k][c] = expand((u32)e[k][c], (int)bits);
        }
    u8 idx[16], idx2[16];
    indices(&b, m->ns, p, m->ib, idx);
    if (m->ib2) indices(&b, 1, 0, m->ib2, idx2);
    for (u32 i = 0; i < 16; i++) {
        u32 s = subset(m->ns, p, i);
        int px[4];
        /* modes 4 and 5: color from the first indices and alpha from the second, swapped by the index selection */
        u32 ci = isb ? idx2[i] : idx[i], cbits = isb ? m->ib2 : m->ib;
        u32 ai = m->ib2 ? (isb ? idx[i] : idx2[i]) : idx[i], abits = m->ib2 ? (isb ? m->ib : m->ib2) : m->ib;
        for (int c = 0; c < 4; c++) {
            u32 k = c < 3 ? ci : ai, bits = c < 3 ? cbits : abits;
            px[c] = lerp64(e[2 * s][c], e[2 * s + 1][c], weights(bits)[k]);
        }
        if (rot) { /* the alpha channel's values swapped with red's, green's or blue's */
            int t = px[3];
            px[3] = px[rot - 1];
            px[rot - 1] = t;
        }
        for (int c = 0; c < 4; c++) out[i][c] = (u8)px[c];
    }
}

u32 bcn_block_bytes(int kind) { return kind == BCN_BC1 || kind == BCN_BC4 || kind == BCN_BC4_SNORM ? 8 : 16; }
u32 bcn_pixel_bytes(int kind) {
    return kind == BCN_BC4 || kind == BCN_BC4_SNORM ? 2 : kind == BCN_BC6H_UF || kind == BCN_BC6H_SF ? 8 : 4;
}

static void decode_image(const u8* src, u32 w, u32 h, int kind, int spec, u8* dst) {
    u32 bw = (w + 3) / 4, bh = (h + 3) / 4, bb = bcn_block_bytes(kind), pb = bcn_pixel_bytes(kind);
    for (u32 by = 0; by < bh; by++)
        for (u32 bx = 0; bx < bw; bx++) {
            const u8* blk = src + ((u64)by * bw + bx) * bb;
            u8 rgba[16][4];
            u16 ch[4][16], half[16][4];
            switch (kind) {
                case BCN_BC1: bc1_block(le64(blk), 1, spec, rgba); break;
                case BCN_BC2: {
                    u64 a = le64(blk);
                    bc1_block(le64(blk + 8), 0, spec, rgba);
                    for (int i = 0; i < 16; i++) rgba[i][3] = (u8)((a >> 4 * i & 15) * 17);
                    break;
                }
                case BCN_BC3:
                    bc1_block(le64(blk + 8), 0, spec, rgba);
                    bc3_alpha(le64(blk), spec, rgba);
                    break;
                case BCN_BC4:
                case BCN_BC4_SNORM: bc4_block(le64(blk), kind == BCN_BC4_SNORM, spec, ch[0]); break;
                case BCN_BC5:
                case BCN_BC5_SNORM:
                    bc4_block(le64(blk), kind == BCN_BC5_SNORM, spec, ch[0]);
                    bc4_block(le64(blk + 8), kind == BCN_BC5_SNORM, spec, ch[1]);
                    break;
                case BCN_BC6H_UF:
                case BCN_BC6H_SF: bc6_block(blk, kind == BCN_BC6H_SF, half); break;
                default: bc7_block(blk, rgba); break;
            }
            for (u32 i = 0; i < 16; i++) {
                u32 px = bx * 4 + i % 4, py = by * 4 + i / 4;
                if (px >= w || py >= h) continue;
                u8* o = dst + ((u64)py * w + px) * pb;
                if (pb == 8)
                    for (int c = 0; c < 4; c++) snx_st_le16(o + 2 * c, half[i][c]);
                else if (kind == BCN_BC4 || kind == BCN_BC4_SNORM)
                    snx_st_le16(o, ch[0][i]);
                else if (kind == BCN_BC5 || kind == BCN_BC5_SNORM) {
                    snx_st_le16(o, ch[0][i]);
                    snx_st_le16(o + 2, ch[1][i]);
                } else
                    memcpy(o, rgba[i], 4);
            }
        }
}
void bcn_decode_image(const u8* src, u32 w, u32 h, int kind, u8* dst) { decode_image(src, w, h, kind, 0, dst); }
void bcn_decode_image_spec(const u8* src, u32 w, u32 h, int kind, u8* dst) { decode_image(src, w, h, kind, 1, dst); }
