/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What an ASTC texture's blocks can hold, read from each block's header and color endpoint modes (CEMs) without
   decoding a texel, and the BCn format that holds it on a GPU that samples BCn but not ASTC (astc_mode's Auto,
   texture.c): luminance alone as BC4 (8 bytes a 4x4 block, an eighth of RGBA8), luminance and alpha as BC5 (16), opaque
   colour on one line a block as BC1 (8, half of BC7). The rest stays as astc_mode holds it.

   The kind is what the decoded texels may be, by the ASTC specification's decode (tests/support/astc.c, the GPU's
   astc_decode.glsl, which the tests hold to it byte for byte), never less: a texture said to be luminance decodes to
   R, G and B within SNX_ASTC_GREY_SPREAD of each other and A = 255 in every texel. A partition of an RGB or RGBA mode
   counts as grey where its two endpoints, as the decoders make them from its colour values, are grey within that
   (snx_astc_rgb_spread): an encoder writes grey in those modes too, as astcenc does a normal map's X in R, G and B */
#ifndef SNX_TEXTURE_ASTC_KIND_H
#define SNX_TEXTURE_ASTC_KIND_H
#include "nx/gpu/texture/blocklinear.h"

/* the kind's bits, or'd over a texture's blocks */
#define SNX_ASTC_CHROMA 1u /* R, G and B may differ in a texel */
#define SNX_ASTC_ALPHA  2u /* alpha may be under 255 */
/* R, G and B may lie off one line in a 4x4 block, BC1's one line: two partitions or more of colour (each is a line of
   its own), a second plane on R, G or B (that channel's weights apart from the others'), or a footprint whose sides
   are not multiples of 4, where a 4x4 block takes the texels of two to four ASTC blocks, each a line of its own. On
   random blocks of one line each, BC1 erred by up to 141 levels in 256 at those footprints and 36 at 4x4, 8x8 and
   12x12 (tests/glsl/astc_bcn_glsl.cpp) */
#define SNX_ASTC_LINES  4u
/* the error colour (an illegal block, a reserved bit, too few bits for its values) or HDR, which the LDR profile
   decodes as the error colour too: nothing BCn is chosen for */
#define SNX_ASTC_OTHER  8u

/* The most an RGB or RGBA partition's endpoints may have R, G and B apart, in levels of 256, and still count as grey.
   astcenc's -normal encodings of two normal maps wrote the RGBA mode (12) in 17% and 0.1% of their blocks, X in R, G
   and B; decoded, 8 and 16 texels of a million and four were a level apart, the endpoints' rounding, which
   counted as colour kept the maps from BC5 (tests/glsl/astc_bcn_glsl.cpp --measure). A texel is then within 2 levels
   of grey, the BC5 view giving R for G and B: the tests hold every decoded texel of such blocks to that */
#define SNX_ASTC_GREY_SPREAD 1u

/* the format snx_astc_kind_as picks: none (as astc_mode holds them), BC4, BC5, BC1 */
enum { SNX_ASTC_AS_USUAL, SNX_ASTC_AS_BC4, SNX_ASTC_AS_BC5, SNX_ASTC_AS_BC1 };

/* n bits (n <= 16) at bit at of a block held as two little-endian words */
static inline u32 snx_astc_bits(u64 lo, u64 hi, u32 at, u32 n) {
    const u64 v = at >= 64 ? hi >> (at - 64) : at + n <= 64 ? lo >> at : (lo >> at) | (hi << (64 - at));
    return (u32)(v & ((1ull << n) - 1));
}
/* an UNORM16 value as 8 bits, as the decoders round it */
static inline u32 snx_astc_unorm8(u32 c) { return (c * 255u + 32767u) / 65535u; }

/* the bits of n values of the bounded integer sequence whose values go up to max_value, for the weight ranges ASTC
   has (1 to 31): their trits or quints, and plain bits */
static inline u32 snx_astc_ise_bits(u32 max_value, u32 n) {
    /* by max_value: 1, 2, 3, 4, 5, 7, 9, 11, 15, 19, 23, 31 */
    static const u8 plain[32] = {0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 0, 0, 4,
                                 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 5};
    static const u8 tq[32] = {0, 0, 3, 0, 5, 3, 0, 0, 0, 5, 0, 3, 0, 0, 0, 0,
                              0, 0, 0, 5, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0};
    const u32 m = max_value & 31u;
    return plain[m] * n + (tq[m] == 3 ? (n * 8 + 4) / 5 : tq[m] == 5 ? (n * 7 + 2) / 3 : 0);
}

/* the bits of n values of the integer sequence encoded with trits (tq 3), quints (5) or neither (0), and nb bits each */
static inline u32 snx_astc_seq_bits(u32 tq, u32 nb, u32 n) {
    return nb * n + (tq == 3 ? (n * 8 + 4) / 5 : tq == 5 ? (n * 7 + 2) / 3 : 0);
}
/* k bits (k <= 8) of a block from *pos on, those at or past end read as zero, as the decoders read a sequence */
static inline u32 snx_astc_seq_read(u64 lo, u64 hi, u32* pos, u32 k, u32 end) {
    u32 v = 0;
    for (u32 i = 0; i < k; i++, (*pos)++)
        if (*pos < end && *pos < 128) v |= snx_astc_bits(lo, hi, *pos, 1) << i;
    return v;
}
/* a colour value of nb bits m, and its trit or quint t (tq 3 or 5), unquantized to 8 bits as the decoders do it */
static inline u32 snx_astc_colour_value(u32 tq, u32 nb, u32 m, u32 t) {
    if (!tq) { /* the bits repeated down to 8 */
        u32 r = 0, have = 0;
        if (!nb) return 0;
        while (have < 8) {
            const u32 take = nb < 8 - have ? nb : 8 - have;
            r = (r << take) | (m >> (nb - take));
            have += take;
        }
        return r;
    }
    const u32 a = (m & 1) * 0x1FF, b = m >> 1;
    u32 c = 0, bb = 0;
    if (tq == 3) switch (nb) {
            case 1: c = 204; break;
            case 2: c = 93, bb = (b & 1) << 8 | (b & 1) << 4 | (b & 1) << 2 | (b & 1) << 1; break;
            case 3: c = 44, bb = (b & 3) << 7 | (b & 3) << 2 | (b & 3); break;
            case 4: c = 22, bb = (b & 7) << 6 | (b & 7); break;
            case 5: c = 11, bb = (b & 15) << 5 | (b & 15) >> 2; break;
            case 6: c = 5, bb = (b & 31) << 4 | (b & 31) >> 4; break;
            default: break;
        }
    else
        switch (nb) {
            case 1: c = 113; break;
            case 2: c = 54, bb = (b & 1) << 8 | (b & 1) << 3 | (b & 1) << 2; break;
            case 3: c = 26, bb = (b & 3) << 7 | (b & 3) << 1 | (b & 3) >> 1; break;
            case 4: c = 13, bb = (b & 7) << 6 | (b & 7) >> 1; break;
            case 5: c = 6, bb = (b & 15) << 5 | (b & 15) >> 3; break;
            default: break;
        }
    const u32 x = (t * c + bb) ^ a;
    return (a & 0x80) | (x >> 2);
}
/* the encoding of n colour values in color_bits bits: the specification's largest range whose values fit, of these,
   largest first (trits 3, quints 5 or plain bits 0, and bits a value), as tq << 8 | nb; its smallest where none fits */
static inline u32 snx_astc_colour_range(u32 n, u32 color_bits) {
    static const u8 ranges[21][2] = {{0, 8}, {3, 6}, {5, 5}, {0, 7}, {3, 5}, {5, 4}, {0, 6},
                                     {3, 4}, {5, 3}, {0, 5}, {3, 3}, {5, 2}, {0, 4}, {3, 2},
                                     {5, 1}, {0, 3}, {3, 1}, {5, 0}, {0, 2}, {3, 0}, {0, 1}};
    u32 r = 0;
    while (r < 20 && snx_astc_seq_bits(ranges[r][0], ranges[r][1], n) > color_bits) r++;
    return (u32)ranges[r][0] << 8 | ranges[r][1];
}
/* The n colour values (18 at most) of a block, its colour data color_bits bits from bit at, as the decoders read and
   unquantize them (tests/support/astc.c's decode_color_values), in snx_astc_colour_range's encoding */
static inline void snx_astc_colour_values(u64 lo, u64 hi, u32 at, u32 color_bits, u32 n, u32* v) {
    const u32 range = snx_astc_colour_range(n, color_bits), tq = range >> 8, nb = range & 255;
    const u32 end = at + snx_astc_seq_bits(tq, nb, n);
    u32 pos = at, got = 0;
    while (got < n) {
        u32 m[5] = {0, 0, 0, 0, 0}, t[5] = {0, 0, 0, 0, 0}, k = 1;
        if (tq == 0) {
            m[0] = snx_astc_seq_read(lo, hi, &pos, nb, end);
        } else if (tq == 3) { /* five values: their bits and the 8 bits of their trits between them */
            u32 tt;
            m[0] = snx_astc_seq_read(lo, hi, &pos, nb, end);
            tt = snx_astc_seq_read(lo, hi, &pos, 2, end);
            m[1] = snx_astc_seq_read(lo, hi, &pos, nb, end);
            tt |= snx_astc_seq_read(lo, hi, &pos, 2, end) << 2;
            m[2] = snx_astc_seq_read(lo, hi, &pos, nb, end);
            tt |= snx_astc_seq_read(lo, hi, &pos, 1, end) << 4;
            m[3] = snx_astc_seq_read(lo, hi, &pos, nb, end);
            tt |= snx_astc_seq_read(lo, hi, &pos, 2, end) << 5;
            m[4] = snx_astc_seq_read(lo, hi, &pos, nb, end);
            tt |= snx_astc_seq_read(lo, hi, &pos, 1, end) << 7;
            u32 cc;
            if ((tt >> 2 & 7) == 7) {
                cc = (tt >> 5 & 7) << 2 | (tt & 3);
                t[4] = t[3] = 2;
            } else {
                cc = tt & 31;
                if ((tt >> 5 & 3) == 3)
                    t[4] = 2, t[3] = tt >> 7 & 1;
                else
                    t[4] = tt >> 7 & 1, t[3] = tt >> 5 & 3;
            }
            if ((cc & 3) == 3)
                t[2] = 2, t[1] = cc >> 4 & 1, t[0] = (cc >> 3 & 1) << 1 | ((cc >> 2 & 1) & ~(cc >> 3) & 1);
            else if ((cc >> 2 & 3) == 3)
                t[2] = 2, t[1] = 2, t[0] = cc & 3;
            else
                t[2] = cc >> 4 & 1, t[1] = cc >> 2 & 3, t[0] = (cc >> 1 & 1) << 1 | ((cc & 1) & ~(cc >> 1) & 1);
            k = 5;
        } else { /* three values: their bits and the 7 bits of their quints between them */
            u32 qq;
            m[0] = snx_astc_seq_read(lo, hi, &pos, nb, end);
            qq = snx_astc_seq_read(lo, hi, &pos, 3, end);
            m[1] = snx_astc_seq_read(lo, hi, &pos, nb, end);
            qq |= snx_astc_seq_read(lo, hi, &pos, 2, end) << 3;
            m[2] = snx_astc_seq_read(lo, hi, &pos, nb, end);
            qq |= snx_astc_seq_read(lo, hi, &pos, 2, end) << 5;
            if ((qq >> 1 & 3) == 3 && (qq >> 5 & 3) == 0) {
                t[0] = t[1] = 4;
                t[2] = (qq & 1) << 2 | ((qq >> 4 & 1) & ~qq & 1) << 1 | ((qq >> 3 & 1) & ~qq & 1);
            } else {
                u32 cc;
                if ((qq >> 1 & 3) == 3)
                    t[2] = 4, cc = (qq >> 3 & 3) << 3 | (~(qq >> 5) & 3) << 1 | (qq & 1);
                else
                    t[2] = qq >> 5 & 3, cc = qq & 31;
                if ((cc & 7) == 5)
                    t[1] = 4, t[0] = cc >> 3 & 3;
                else
                    t[1] = cc >> 3 & 3, t[0] = cc & 7;
            }
            k = 3;
        }
        for (u32 i = 0; i < k && got < n; i++) v[got++] = snx_astc_colour_value(tq, nb, m[i], t[i]);
    }
}
/* the most R, G and B lie apart in either endpoint of a partition of LDR mode cem (6, 8, 9, 10, 12 or 13) from its
   colour values v, as the decoders make the endpoints (tests/support/astc.c's compute_endpoints): blue contraction,
   the base and offset's bit transfer and their clamp */
static inline u32 snx_astc_rgb_spread(u32 cem, const u32* u) {
    int v[8], e[2][3];
    for (u32 i = 0; i < 8; i++)
        v[i] = (int)(i < (cem == 6 ? 4u : cem == 10 || cem == 8 || cem == 9 ? 6u : 8u) ? u[i] : 0u);
    if (cem == 9 || cem == 13) /* each pair's bit transfer: a base of 8 bits, a signed offset of 6 */
        for (u32 i = 0; i < (cem == 9 ? 6u : 8u); i += 2) {
            v[i] >>= 1;
            v[i] |= v[i + 1] & 0x80;
            v[i + 1] = (v[i + 1] >> 1) & 0x3F;
            if (v[i + 1] & 0x20) v[i + 1] -= 0x40;
        }
    if (cem == 6 || cem == 10) {
        for (u32 c = 0; c < 3; c++) e[0][c] = v[c] * v[3] >> 8, e[1][c] = v[c];
    } else if (cem == 8 || cem == 12) {
        const int contract = v[1] + v[3] + v[5] < v[0] + v[2] + v[4];
        for (u32 c = 0; c < 3; c++) e[0][c] = v[2 * c + contract], e[1][c] = v[2 * c + 1 - contract];
        if (contract)
            for (u32 k = 0; k < 2; k++) e[k][0] = (e[k][0] + e[k][2]) >> 1, e[k][1] = (e[k][1] + e[k][2]) >> 1;
    } else { /* 9 and 13: a base and an offset */
        const int contract = v[1] + v[3] + v[5] < 0;
        for (u32 c = 0; c < 3; c++) {
            const int base = v[2 * c], sum = v[2 * c] + v[2 * c + 1];
            e[0][c] = contract ? sum : base, e[1][c] = contract ? base : sum;
        }
        if (contract)
            for (u32 k = 0; k < 2; k++) e[k][0] = (e[k][0] + e[k][2]) >> 1, e[k][1] = (e[k][1] + e[k][2]) >> 1;
        for (u32 k = 0; k < 2; k++)
            for (u32 c = 0; c < 3; c++) e[k][c] = e[k][c] < 0 ? 0 : e[k][c] > 255 ? 255 : e[k][c];
    }
    u32 spread = 0;
    for (u32 k = 0; k < 2; k++) {
        const int hi = e[k][0] > e[k][1]   ? (e[k][0] > e[k][2] ? e[k][0] : e[k][2])
                       : e[k][1] > e[k][2] ? e[k][1]
                                           : e[k][2];
        const int lo = e[k][0] < e[k][1]   ? (e[k][0] < e[k][2] ? e[k][0] : e[k][2])
                       : e[k][1] < e[k][2] ? e[k][1]
                                           : e[k][2];
        if ((u32)(hi - lo) > spread) spread = (u32)(hi - lo);
    }
    return spread;
}

/* What snx_astc_block_read read of a block past its header, for the tests to hold to the decoder: its partitions (0
   where it stopped before them: a void extent, or a block of the error colour), their endpoint modes and, for those of
   RGB and RGBA, the most R, G and B lie apart in their endpoints (snx_astc_rgb_spread; else 0) */
typedef struct {
    u32 parts, cem[4], spread[4];
} SnxAstcBlockRead;
/* The kind of one block of a bw x bh footprint (16 bytes, little-endian), decoded in the sRGB decode mode or not
   (srgb: a void extent's R, G and B round otherwise), and what it read into *read where read is not NULL. Every block the
   decoders make the error colour of is OTHER, by the same tests in the same order (tests/support/astc.c's
   decompress_block) */
static inline u32 snx_astc_block_read(const u8* blk, u32 bw, u32 bh, int srgb, SnxAstcBlockRead* read) {
    static const u8 low_weights[6] = {1, 2, 3, 4, 5, 7}, high_weights[6] = {9, 11, 15, 19, 23, 31};
    u64 lo = 0, hi = 0;
    if (read) read->parts = 0;
    for (u32 i = 0; i < 8; i++) {
        lo |= (u64)blk[i] << (8 * i);
        hi |= (u64)blk[8 + i] << (8 * i);
    }
    const u32 mode = (u32)(lo & 0x7FF);
    /* a void extent: one colour, the error colour where it is HDR or a reserved bit is not one */
    if ((mode & 0x1FF) == 0x1FC) {
        if (mode & 0x200 || !(mode & 0x400) || !(lo >> 11 & 1)) return SNX_ASTC_OTHER;
        const u32 s0 = snx_astc_bits(lo, hi, 12, 13), s1 = snx_astc_bits(lo, hi, 25, 13);
        const u32 t0 = snx_astc_bits(lo, hi, 38, 13), t1 = snx_astc_bits(lo, hi, 51, 13);
        if ((s0 & s1 & t0 & t1) != 0x1FFF && (s0 >= s1 || t0 >= t1)) return SNX_ASTC_OTHER;
        u32 c[3];
        for (u32 k = 0; k < 3; k++) {
            const u32 v = snx_astc_bits(lo, hi, 64 + 16 * k, 16);
            c[k] = srgb ? v >> 8 : snx_astc_unorm8(v);
        }
        const u32 chroma =
            c[0] != c[1] || c[1] != c[2] ? SNX_ASTC_CHROMA | (bw % 4 || bh % 4 ? SNX_ASTC_LINES : 0u) : 0u;
        return chroma | (snx_astc_unorm8(snx_astc_bits(lo, hi, 112, 16)) != 255 ? SNX_ASTC_ALPHA : 0u);
    }
    if ((mode & 0xF) == 0 || ((mode & 0x3) == 0 && (mode & 0x1C0) == 0x1C0)) return SNX_ASTC_OTHER;
    /* the block mode's layout, its weight grid and range (the specification's table, in its order) */
    u32 layout;
    if (mode & 0x3)
        layout = mode & 0x8 ? (mode & 0x4 ? (mode & 0x100 ? 4u : 3u) : 2u) : (mode & 0x4 ? 1u : 0u);
    else
        layout = mode & 0x100 ? (mode & 0x80 ? (mode & 0x20 ? 8u : 7u) : 9u) : (mode & 0x80 ? 6u : 5u);
    const u32 r = ((mode >> 4) & 1) | (layout < 5 ? (mode & 0x3) << 1 : (mode & 0xC) >> 1);
    const u32 a = (mode >> 5) & 0x3, b = (mode >> 7) & 0x3;
    static const u8 gw[10][2] = {{4, 2}, {8, 2}, {2, 8}, {2, 6}, {2, 2}, {12, 2}, {2, 12}, {6, 10}, {10, 6}, {6, 6}};
    u32 width = gw[layout][0], height = gw[layout][1];
    switch (layout) {
        case 0:
        case 1: width += b, height += a; break;
        case 2: width += a, height += b; break;
        case 3: width += a, height += b & 1; break;
        case 4: width += b & 1, height += a; break;
        case 5: height += a; break;
        case 6: width += a; break;
        case 9: width += a, height += (mode >> 9) & 0x3; break;
        default: break;
    }
    const int dual = layout != 9 && (mode & 0x400);
    const u32 max_weight = layout != 9 && (mode & 0x200) ? high_weights[r - 2] : low_weights[r - 2];
    const u32 count = width * height * (dual ? 2u : 1u);
    if (width > bw || height > bh || count > 64) return SNX_ASTC_OTHER;
    const u32 parts = (u32)(lo >> 11 & 3) + 1;
    if (parts == 4 && dual) return SNX_ASTC_OTHER;
    const u32 weight_bits = snx_astc_ise_bits(max_weight, count);
    if (weight_bits < 24 || weight_bits > 96) return SNX_ASTC_OTHER;
    const u32 base_cem = parts == 1 ? 0u : (u32)(lo >> 23 & 0x3F), base_mode = base_cem & 3;
    const u32 extra = base_mode ? parts * 3 - 4 : 0, plane_bits = dual ? 2u : 0u, at = parts == 1 ? 17u : 29u;
    if (weight_bits + at + extra + plane_bits > 128) return SNX_ASTC_OTHER;
    const u32 color_bits = 128 - weight_bits - at - extra - plane_bits;
    /* the second plane's channel (R, G, B, A), below the extra CEM bits, which are just below the weights */
    const u32 ccs = dual ? snx_astc_bits(lo, hi, at + color_bits, 2) : 3u;
    u32 cem[4] = {0, 0, 0, 0};
    if (parts == 1) {
        cem[0] = (u32)(lo >> 13 & 0xF);
    } else if (base_mode) {
        u32 bits = ((snx_astc_bits(lo, hi, at + color_bits + plane_bits, extra) << 6) | base_cem) >> 2;
        u32 c[4];
        for (u32 i = 0; i < parts; i++, bits >>= 1) c[i] = bits & 1;
        for (u32 i = 0; i < parts; i++, bits >>= 2) cem[i] = ((base_mode - (c[i] ? 0u : 1u)) << 2) | (bits & 3);
    } else {
        for (u32 i = 0; i < parts; i++) cem[i] = base_cem >> 2;
    }
    /* more than 18 colour values, or too few bits for them to have 6 levels each: the error colour */
    u32 values = 0;
    for (u32 i = 0; i < parts; i++) values += ((cem[i] >> 2) + 1) << 1;
    if (values > 18 || snx_astc_ise_bits(5, values) > color_bits) return SNX_ASTC_OTHER;
    u32 kind = 0, colour = 0, v[18], at_value = 0, spread[4] = {0, 0, 0, 0};
    snx_astc_colour_values(lo, hi, at, color_bits, values, v);
    for (u32 i = 0; i < parts; i++) {
        if ((cem[i] == 6 || cem[i] >= 8) && cem[i] <= 13 && cem[i] != 11)
            spread[i] = snx_astc_rgb_spread(cem[i], v + at_value);
        at_value += ((cem[i] >> 2) + 1) << 1;
    }
    if (read) {
        read->parts = parts;
        for (u32 i = 0; i < 4; i++) read->cem[i] = cem[i], read->spread[i] = spread[i];
    }
    for (u32 i = 0; i < parts; i++) {
        const u32 grey = spread[i] <= SNX_ASTC_GREY_SPREAD;
        switch (cem[i]) {
            case 0: /* luminance, direct and base+offset: R = G = B, alpha 255 */
            case 1: break;
            case 4: /* luminance and alpha */
            case 5: kind |= SNX_ASTC_ALPHA; break;
            case 6: /* RGB, base+scale, direct, base+offset: alpha 255; grey ends are luminance */
            case 8:
            case 9: colour += !grey; break;
            case 10: /* RGB base+scale with two alphas, RGBA direct and base+offset; grey ends are luminance and alpha */
            case 12:
            case 13:
                colour += !grey;
                kind |= SNX_ASTC_ALPHA;
                break;
            default: return SNX_ASTC_OTHER; /* HDR: the error colour on that partition's texels */
        }
    }
    if (colour) kind |= SNX_ASTC_CHROMA | (parts > 1 || bw % 4 || bh % 4 ? SNX_ASTC_LINES : 0u);
    /* a second plane on R, G or B weighs that channel apart: no longer grey, nor one line */
    if (ccs != 3) kind |= SNX_ASTC_CHROMA | SNX_ASTC_LINES;
    return kind;
}
static inline u32 snx_astc_block_kind(const u8* blk, u32 bw, u32 bh, int srgb) {
    return snx_astc_block_read(blk, bw, bh, srgb, NULL);
}

/* the format a texture of kind is held in (SNX_ASTC_AS_*), decoded as sRGB or not: BC4 and BC5 have no sRGB twin, so
   an sRGB texture of luminance goes to BC1, which holds the grey line in sRGB, and one with alpha stays as it is */
static inline int snx_astc_kind_as(u32 kind, int srgb) {
    if (kind & SNX_ASTC_OTHER) return SNX_ASTC_AS_USUAL;
    if (!(kind & SNX_ASTC_CHROMA))
        return kind & SNX_ASTC_ALPHA ? (srgb ? SNX_ASTC_AS_USUAL : SNX_ASTC_AS_BC5)
               : srgb                ? SNX_ASTC_AS_BC1
                                     : SNX_ASTC_AS_BC4;
    return kind & (SNX_ASTC_ALPHA | SNX_ASTC_LINES) ? SNX_ASTC_AS_USUAL : SNX_ASTC_AS_BC1;
}
/* a kind past which no format but the usual is left: the walk over a level stops there */
static inline int snx_astc_kind_done(u32 kind, int srgb) { return snx_astc_kind_as(kind, srgb) == SNX_ASTC_AS_USUAL; }

/* The kind of a level's blocks, or'd over all of them, nx x ny blocks a slice, nz slices, in size bytes at p: block
   linear (gobs_x GOBs a row, blocks of 2^lbh GOBs high and 2^lbd deep, the guest's layout) or, where pitch is not 0,
   rows of blocks pitch bytes apart. Only the level's own blocks are read, not the layout's padding past its rows,
   columns and slices, which holds whatever memory held. It stops once no BCn format is left (snx_astc_kind_done).
   OTHER when the layout reaches past size bytes */
static inline u32 snx_astc_level_kind(const u8* p, u64 size, u32 nx, u32 ny, u32 nz, u32 pitch, u32 gobs_x, u32 lbh,
                                      u32 lbd, u32 bw, u32 bh, int srgb) {
    u32 kind = 0;
    if (pitch) {
        if (nz != 1 || (u64)pitch * (ny - 1) + (u64)nx * 16 > size) return SNX_ASTC_OTHER;
        for (u32 y = 0; y < ny; y++)
            for (u32 x = 0; x < nx; x++) {
                kind |= snx_astc_block_kind(p + (u64)y * pitch + (u64)x * 16, bw, bh, srgb);
                if (snx_astc_kind_done(kind, srgb)) return kind;
            }
        return kind;
    }
    /* a GOB at a time: 4 blocks across and 8 rows, the 32 at the offsets gob_x_offset and gob_y_offset give */
    u32 in_gob[32];
    for (u32 j = 0; j < 8; j++)
        for (u32 i = 0; i < 4; i++) in_gob[j * 4 + i] = gob_x_offset(i * 16) + gob_y_offset(j);
    const u64 block = 512ull << (lbh + lbd), rows = ((((u64)ny + 7) >> 3) + (1u << lbh) - 1) >> lbh;
    if ((u64)(nx + 3) / 4 > gobs_x) return SNX_ASTC_OTHER;
    for (u32 z = 0; z < nz; z++)
        for (u32 gy = 0; gy < (ny + 7) / 8; gy++)
            for (u32 gx = 0; gx < (nx + 3) / 4; gx++) {
                const u64 at = (((u64)(z >> lbd) * rows + (gy >> lbh)) * gobs_x + gx) * block +
                               ((((u64)z & ((1u << lbd) - 1)) << lbh) + (gy & ((1u << lbh) - 1))) * 512;
                if (at + 512 > size) return SNX_ASTC_OTHER;
                for (u32 j = 0; j < 8 && gy * 8 + j < ny; j++)
                    for (u32 i = 0; i < 4 && gx * 4 + i < nx; i++) {
                        kind |= snx_astc_block_kind(p + at + in_gob[j * 4 + i], bw, bh, srgb);
                        if (snx_astc_kind_done(kind, srgb)) return kind;
                    }
            }
    return kind;
}

#endif
