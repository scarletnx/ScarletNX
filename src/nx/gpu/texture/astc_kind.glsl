/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What an ASTC block can hold, read from its header and colour endpoint modes on the GPU: the body of astc_kind.comp,
   gpu/texture/astc_kind.h's snx_astc_block_read for the GPU, which the tests hold to it block for block
   (tests/glsl/astc_kind_glsl.cpp). An invocation reads one block of a level the texture keeps (vk/astc.c's
   vk_astc_keep: a slice after another, rows of blocks), and the workgroup keeps, for each kind bit, the first block in
   the order snx_astc_level_kind walks the level that has it: the CPU then finds where that walk stops
   (snx_astc_level_kind_firsts), so the kind is the walk's, its early stop too, with no block read on the CPU.

   The walk's order is the guest's layout of the level: rows of blocks for a pitch linear one, and for a block linear
   one GOBs of 4 blocks across and 8 rows, a GOB row after another, each GOB's blocks row by row. A block's place in it
   is its number: its GOB's, 32 blocks each, and its own in the GOB. The numbers of a level are below 2^25 (16384 texels
   a side, 4 at least a block), so ~0u means no block */

/* the most R, G and B of an RGB or RGBA partition's endpoints may lie apart and count as grey: astc_kind.h's
   SNX_ASTC_GREY_SPREAD, which the tests hold this to */
#define SNX_ASTC_KIND_GREY_SPREAD 1u

/* the block: four little-endian words, as the decoders read its 128 bits */
uvec4 kind_blk;
/* the colour values of the block (18 at most), the bit snx_astc_seq_read reads next, and the values and their trits or
   quints of a group of the sequence */
uint kind_v[18] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
uint kind_pos;
uint kind_m[5] = {0u, 0u, 0u, 0u, 0u};
uint kind_t[5] = {0u, 0u, 0u, 0u, 0u};
/* a partition's colour values as ints, and its two endpoints' R, G and B (snx_astc_rgb_spread's e) */
int kind_iv[8] = {0, 0, 0, 0, 0, 0, 0, 0};
int kind_e[6] = {0, 0, 0, 0, 0, 0};

/* n bits (n <= 16) of the block at bit at (< 128); those past its end read as zero */
uint KindBits(uint at, uint n) {
    const uint w = at >> 5u, s = at & 31u;
    uint v = kind_blk[w] >> s;
    if (s + n > 32u && w < 3u) v |= kind_blk[w + 1u] << (32u - s);
    return v & ((1u << n) - 1u);
}
/* an UNORM16 value as 8 bits, as the decoders round it */
uint KindUnorm8(uint c) { return (c * 255u + 32767u) / 65535u; }

/* The bits of n values (64 at most) of the integer sequence of a range, nb bits each and fc its trits' or quints':
   nb * n plus ceil(8n / 5) for trits and ceil(7n / 3) for quints, which are (8n + 4) / 5 = (205n + 102) >> 7 and
   (7n + 2) / 3 = (299n + 85) >> 7 for n <= 64, and nb * n is (nb << 7) * n >> 7 with no bits below the shift: one
   multiply-add and a shift. fc is F << 7 | C of those, KIND_TRITS or KIND_QUINTS (0: neither). ucas proved each step
   equal to snx_astc_seq_bits on every input (docs/layers/gpu/astc.md, "The kind's arithmetic") */
#define KIND_TRITS  26342u /* 205 << 7 | 102 */
#define KIND_QUINTS 38357u /* 299 << 7 | 85 */
uint KindSeqBits(uint fc, uint nb, uint n) { return (n * ((nb << 7u) + (fc >> 7u)) + (fc & 127u)) >> 7u; }
/* k bits (k <= 8) of the block from kind_pos on, those at or past end read as zero, as the decoders read a sequence
   (end <= 128): the bits before end at once, where the decoders read one at a time (ucas: equal for every window of
   the block's bits, k and end) */
uint KindSeqRead(uint k, uint end) {
    const uint n = kind_pos < end ? min(k, end - kind_pos) : 0u;
    const uint v = n != 0u ? KindBits(kind_pos, n) : 0u;
    kind_pos += k;
    return v;
}
/* a colour value of nb bits m, and its trit or quint t (tq 3 or 5), unquantized to 8 bits as the decoders do it */
uint KindColourValue(uint tq, uint nb, uint m, uint t) {
    if (tq == 0u) /* the bits repeated down to 8: a multiply and a shift, those of 1 to 3 bits by a table in a word
                     (ucas: equal to the repeat for every nb, m) */
        return nb < 4u ? m * ((0x4955FF00u >> (nb << 3u)) & 255u) >> ((8u >> nb) & 1u)
                       : m * ((1u << nb) + 1u) >> (2u * nb - 8u);
    const uint a = (m & 1u) * 0x1FFu, b = m >> 1u;
    uint c = 0u, bb = 0u;
    if (tq == 3u) {
        if (nb == 1u) {
            c = 204u;
        } else if (nb == 2u) {
            c = 93u;
            bb = (b & 1u) << 8u | (b & 1u) << 4u | (b & 1u) << 2u | (b & 1u) << 1u;
        } else if (nb == 3u) {
            c = 44u;
            bb = (b & 3u) << 7u | (b & 3u) << 2u | (b & 3u);
        } else if (nb == 4u) {
            c = 22u;
            bb = (b & 7u) << 6u | (b & 7u);
        } else if (nb == 5u) {
            c = 11u;
            bb = (b & 15u) << 5u | (b & 15u) >> 2u;
        } else if (nb == 6u) {
            c = 5u;
            bb = (b & 31u) << 4u | (b & 31u) >> 4u;
        }
    } else {
        if (nb == 1u) {
            c = 113u;
        } else if (nb == 2u) {
            c = 54u;
            bb = (b & 1u) << 8u | (b & 1u) << 3u | (b & 1u) << 2u;
        } else if (nb == 3u) {
            c = 26u;
            bb = (b & 3u) << 7u | (b & 3u) << 1u | (b & 3u) >> 1u;
        } else if (nb == 4u) {
            c = 13u;
            bb = (b & 7u) << 6u | (b & 7u) >> 1u;
        } else if (nb == 5u) {
            c = 6u;
            bb = (b & 15u) << 5u | (b & 15u) >> 3u;
        }
    }
    const uint x = (t * c + bb) ^ a;
    return (a & 0x80u) | (x >> 2u);
}
/* the encoding of n colour values (2 to 18) in color_bits bits, as tq << 8 | nb (snx_astc_colour_range): the
   specification's ranges, largest first, are a plain one of 8 - k bits, a trit's of 6 - k and a quint's of 5 - k for
   k = 0 to 6 but the last, which is a plain one of 1 bit. The first that fits follows the plain ranges' bits a value
   that fit, p = color_bits / n: p = 8 or more is the first range; else plain p + 1 does not fit, nor does any range
   before it, and the trit range of p - 1 bits, the quint one of p - 2 and plain p come next, a trit's or a quint's
   bits a value lying between p and p + 1 (ucas: equal to the search over every range for every n and color_bits) */
uint KindColourRange(uint n, uint color_bits) {
    const uint p = color_bits / n;
    if (p >= 8u) return 8u;
    if (p == 0u) return 1u;
    if (KindSeqBits(KIND_TRITS, p - 1u, n) <= color_bits) return 3u << 8u | (p - 1u);
    if (p >= 2u && KindSeqBits(KIND_QUINTS, p - 2u, n) <= color_bits) return 5u << 8u | (p - 2u);
    return p;
}
/* the block's n colour values (18 at most) into kind_v, its colour data color_bits bits from bit at, as the decoders
   read and unquantize them (snx_astc_colour_values) */
void KindColourValues(uint at, uint color_bits, uint n) {
    const uint range = KindColourRange(n, color_bits), tq = range >> 8u, nb = range & 255u;
    const uint end = at + KindSeqBits(tq == 3u ? KIND_TRITS : tq == 5u ? KIND_QUINTS : 0u, nb, n);
    uint got = 0u;
    kind_pos = at;
    while (got < n) {
        uint k = 1u;
        for (uint i = 0u; i < 5u; i++) kind_m[i] = 0u, kind_t[i] = 0u;
        if (tq == 0u) {
            kind_m[0] = KindSeqRead(nb, end);
        } else if (tq == 3u) { /* five values: their bits and the 8 bits of their trits between them */
            kind_m[0] = KindSeqRead(nb, end);
            uint tt = KindSeqRead(2u, end);
            kind_m[1] = KindSeqRead(nb, end);
            tt |= KindSeqRead(2u, end) << 2u;
            kind_m[2] = KindSeqRead(nb, end);
            tt |= KindSeqRead(1u, end) << 4u;
            kind_m[3] = KindSeqRead(nb, end);
            tt |= KindSeqRead(2u, end) << 5u;
            kind_m[4] = KindSeqRead(nb, end);
            tt |= KindSeqRead(1u, end) << 7u;
            uint cc;
            if ((tt >> 2u & 7u) == 7u) {
                cc = (tt >> 5u & 7u) << 2u | (tt & 3u);
                kind_t[4] = 2u;
                kind_t[3] = 2u;
            } else {
                cc = tt & 31u;
                if ((tt >> 5u & 3u) == 3u) {
                    kind_t[4] = 2u;
                    kind_t[3] = tt >> 7u & 1u;
                } else {
                    kind_t[4] = tt >> 7u & 1u;
                    kind_t[3] = tt >> 5u & 3u;
                }
            }
            if ((cc & 3u) == 3u) {
                kind_t[2] = 2u;
                kind_t[1] = cc >> 4u & 1u;
                kind_t[0] = (cc >> 3u & 1u) << 1u | ((cc >> 2u & 1u) & ~(cc >> 3u) & 1u);
            } else if ((cc >> 2u & 3u) == 3u) {
                kind_t[2] = 2u;
                kind_t[1] = 2u;
                kind_t[0] = cc & 3u;
            } else {
                kind_t[2] = cc >> 4u & 1u;
                kind_t[1] = cc >> 2u & 3u;
                kind_t[0] = (cc >> 1u & 1u) << 1u | ((cc & 1u) & ~(cc >> 1u) & 1u);
            }
            k = 5u;
        } else { /* three values: their bits and the 7 bits of their quints between them */
            kind_m[0] = KindSeqRead(nb, end);
            uint qq = KindSeqRead(3u, end);
            kind_m[1] = KindSeqRead(nb, end);
            qq |= KindSeqRead(2u, end) << 3u;
            kind_m[2] = KindSeqRead(nb, end);
            qq |= KindSeqRead(2u, end) << 5u;
            if ((qq >> 1u & 3u) == 3u && (qq >> 5u & 3u) == 0u) {
                kind_t[0] = 4u;
                kind_t[1] = 4u;
                kind_t[2] = (qq & 1u) << 2u | ((qq >> 4u & 1u) & ~qq & 1u) << 1u | ((qq >> 3u & 1u) & ~qq & 1u);
            } else {
                uint cc;
                if ((qq >> 1u & 3u) == 3u) {
                    kind_t[2] = 4u;
                    cc = (qq >> 3u & 3u) << 3u | (~(qq >> 5u) & 3u) << 1u | (qq & 1u);
                } else {
                    kind_t[2] = qq >> 5u & 3u;
                    cc = qq & 31u;
                }
                if ((cc & 7u) == 5u) {
                    kind_t[1] = 4u;
                    kind_t[0] = cc >> 3u & 3u;
                } else {
                    kind_t[1] = cc >> 3u & 3u;
                    kind_t[0] = cc & 7u;
                }
            }
            k = 3u;
        }
        for (uint i = 0u; i < k && got < n; i++) kind_v[got++] = KindColourValue(tq, nb, kind_m[i], kind_t[i]);
    }
}
/* the most R, G and B lie apart in either endpoint of a partition of LDR mode cem (6, 8, 9, 10, 12 or 13) from its
   colour values from kind_v[at] on, as the decoders make the endpoints (snx_astc_rgb_spread) */
uint KindRgbSpread(uint cem, uint at) {
    /* cem is 6, 8, 9, 10, 12 or 13: a set of them is a table in a word (ucas: equal on each) */
    const uint used = cem == 6u ? 4u : (0x700u >> cem & 1u) != 0u ? 6u : 8u;
    for (uint i = 0u; i < 8u; i++) kind_iv[i] = i < used ? int(kind_v[at + i]) : 0;
    if ((0x2200u >> cem & 1u) != 0u) /* 9 and 13, each pair's bit transfer: a base of 8 bits, a signed offset of 6 */
        for (uint i = 0u; i < (cem == 9u ? 6u : 8u); i += 2u) {
            kind_iv[i] >>= 1;
            kind_iv[i] |= kind_iv[i + 1u] & 0x80;
            kind_iv[i + 1u] = (kind_iv[i + 1u] >> 1) & 0x3F;
            if ((kind_iv[i + 1u] & 0x20) != 0) kind_iv[i + 1u] -= 0x40;
        }
    if ((0x440u >> cem & 1u) != 0u) { /* 6 and 10 */
        for (uint c = 0u; c < 3u; c++) {
            kind_e[c] = (kind_iv[c] * kind_iv[3]) >> 8;
            kind_e[3u + c] = kind_iv[c];
        }
    } else if ((0x1100u >> cem & 1u) != 0u) { /* 8 and 12 */
        const uint contract = kind_iv[1] + kind_iv[3] + kind_iv[5] < kind_iv[0] + kind_iv[2] + kind_iv[4] ? 1u : 0u;
        for (uint c = 0u; c < 3u; c++) {
            kind_e[c] = kind_iv[2u * c + contract];
            kind_e[3u + c] = kind_iv[2u * c + 1u - contract];
        }
        if (contract != 0u)
            for (uint k = 0u; k < 6u; k += 3u) {
                kind_e[k] = (kind_e[k] + kind_e[k + 2u]) >> 1;
                kind_e[k + 1u] = (kind_e[k + 1u] + kind_e[k + 2u]) >> 1;
            }
    } else { /* 9 and 13: a base and an offset */
        const bool contract = kind_iv[1] + kind_iv[3] + kind_iv[5] < 0;
        for (uint c = 0u; c < 3u; c++) {
            const int base = kind_iv[2u * c], sum = kind_iv[2u * c] + kind_iv[2u * c + 1u];
            kind_e[c] = contract ? sum : base;
            kind_e[3u + c] = contract ? base : sum;
        }
        if (contract)
            for (uint k = 0u; k < 6u; k += 3u) {
                kind_e[k] = (kind_e[k] + kind_e[k + 2u]) >> 1;
                kind_e[k + 1u] = (kind_e[k + 1u] + kind_e[k + 2u]) >> 1;
            }
        for (uint i = 0u; i < 6u; i++) kind_e[i] = kind_e[i] < 0 ? 0 : kind_e[i] > 255 ? 255 : kind_e[i];
    }
    uint spread = 0u;
    for (uint k = 0u; k < 6u; k += 3u) {
        const int hi = max(max(kind_e[k], kind_e[k + 1u]), kind_e[k + 2u]);
        const int lo = min(min(kind_e[k], kind_e[k + 1u]), kind_e[k + 2u]);
        spread = max(spread, uint(hi - lo));
    }
    return spread;
}

/* The kind of kind_blk, a block of a block_w x block_h footprint, decoded in the sRGB decode mode or not (srgb_mode):
   snx_astc_block_read's, by the same tests in the same order (SNX_ASTC_CHROMA 1, ALPHA 2, LINES 4, OTHER 8) */
const uint kind_grid[20] = {4u, 2u, 8u, 2u, 2u, 8u, 2u, 6u, 2u, 2u, 12u, 2u, 2u, 12u, 6u, 10u, 10u, 6u, 6u, 6u};
uint BlockKind(uint bw, uint bh, uint srgb_mode) {
    const uint mode = kind_blk.x & 0x7FFu;
    const uint lines = (bw & 3u) != 0u || (bh & 3u) != 0u ? 4u : 0u;
    /* a void extent: one colour, the error colour where it is HDR or a reserved bit is not one */
    if ((mode & 0x1FFu) == 0x1FCu) {
        if ((mode & 0x200u) != 0u || (mode & 0x400u) == 0u || KindBits(11u, 1u) == 0u) return 8u;
        const uint s0 = KindBits(12u, 13u), s1 = KindBits(25u, 13u), t0 = KindBits(38u, 13u), t1 = KindBits(51u, 13u);
        if ((s0 & s1 & t0 & t1) != 0x1FFFu && (s0 >= s1 || t0 >= t1)) return 8u;
        const uint r = KindBits(64u, 16u), g = KindBits(80u, 16u), b = KindBits(96u, 16u);
        const uint r8 = srgb_mode != 0u ? r >> 8u : KindUnorm8(r), g8 = srgb_mode != 0u ? g >> 8u : KindUnorm8(g),
                   b8 = srgb_mode != 0u ? b >> 8u : KindUnorm8(b);
        const uint chroma = r8 != g8 || g8 != b8 ? 1u | lines : 0u;
        return chroma | (KindUnorm8(KindBits(112u, 16u)) != 255u ? 2u : 0u);
    }
    if ((mode & 0xFu) == 0u || ((mode & 0x3u) == 0u && (mode & 0x1C0u) == 0x1C0u)) return 8u;
    /* the block mode's layout (grid), its weight grid and range (the specification's table, in its order). The layout
       is a nibble table on the bits that tell them apart: bits 2, 3 and 8 where bits 0 and 1 are not both 0, else
       bits 5, 7 and 8 (ucas: equal to the table's tests for every mode) */
    const uint grid = (mode & 0x3u) != 0u ? (0x42103210u >> (4u * (((mode >> 2u) & 3u) | ((mode >> 6u) & 4u)))) & 15u
                                          : (0x87996655u >> (4u * (((mode >> 5u) & 1u) | ((mode >> 6u) & 6u)))) & 15u;
    const uint r = ((mode >> 4u) & 1u) | (grid < 5u ? (mode & 0x3u) << 1u : (mode & 0xCu) >> 1u);
    const uint a = (mode >> 5u) & 0x3u, b = (mode >> 7u) & 0x3u;
    uint width = kind_grid[grid * 2u], height = kind_grid[grid * 2u + 1u];
    if (grid == 0u || grid == 1u) {
        width += b;
        height += a;
    } else if (grid == 2u) {
        width += a;
        height += b;
    } else if (grid == 3u) {
        width += a;
        height += b & 1u;
    } else if (grid == 4u) {
        width += b & 1u;
        height += a;
    } else if (grid == 5u) {
        height += a;
    } else if (grid == 6u) {
        width += a;
    } else if (grid == 9u) {
        width += a;
        height += (mode >> 9u) & 0x3u;
    }
    const bool dual = grid != 9u && (mode & 0x400u) != 0u;
    /* the weights' range (r, and the high ranges where the mode's bit 9 says so) as its bits a value and its trits or
       quints (0 none, 1, 2), 5 bits a range in a word for the low ranges and one for the high (ucas: the bits of every
       count of weights are snx_astc_ise_bits' of the range's largest weight) */
    const uint range = ((grid != 9u && (mode & 0x200u) != 0u ? 0xAB91151u : 0x6980901u) >> (5u * (r - 2u))) & 31u;
    const uint weight_fc = range >> 3u == 1u ? KIND_TRITS : range >> 3u == 2u ? KIND_QUINTS : 0u;
    const uint count = width * height * (dual ? 2u : 1u);
    if (width > bw || height > bh || count > 64u) return 8u;
    const uint parts = KindBits(11u, 2u) + 1u;
    if (parts == 4u && dual) return 8u;
    const uint weight_bits = KindSeqBits(weight_fc, range & 7u, count);
    if (weight_bits < 24u || weight_bits > 96u) return 8u;
    const uint base_cem = parts == 1u ? 0u : KindBits(23u, 6u), base_mode = base_cem & 3u;
    const uint extra = base_mode != 0u ? parts * 3u - 4u : 0u, plane_bits = dual ? 2u : 0u,
               at = parts == 1u ? 17u : 29u;
    if (weight_bits + at + extra + plane_bits > 128u) return 8u;
    const uint color_bits = 128u - weight_bits - at - extra - plane_bits;
    /* the second plane's channel (R, G, B, A), below the extra CEM bits, which are just below the weights */
    const uint ccs = dual ? KindBits(at + color_bits, 2u) : 3u;
    uint cem0 = 0u, cem1 = 0u, cem2 = 0u, cem3 = 0u;
    if (parts == 1u) {
        cem0 = KindBits(13u, 4u);
    } else if (base_mode != 0u) {
        uint bits = ((KindBits(at + color_bits + plane_bits, extra) << 6u) | base_cem) >> 2u;
        const uint c = bits; /* a bit a partition: its CEM's class is the base's, or the one below */
        bits >>= parts;
        const uint top = base_mode << 2u;
        cem0 = (top - ((c & 1u) != 0u ? 0u : 4u)) | (bits & 3u);
        cem1 = (top - ((c & 2u) != 0u ? 0u : 4u)) | (bits >> 2u & 3u);
        cem2 = parts > 2u ? (top - ((c & 4u) != 0u ? 0u : 4u)) | (bits >> 4u & 3u) : 0u;
        cem3 = parts > 3u ? (top - ((c & 8u) != 0u ? 0u : 4u)) | (bits >> 6u & 3u) : 0u;
    } else {
        cem0 = base_cem >> 2u;
        cem1 = cem0;
        cem2 = parts > 2u ? cem0 : 0u;
        cem3 = parts > 3u ? cem0 : 0u;
    }
    /* more than 18 colour values, or too few bits for them to have 6 levels each: the error colour */
    const uint values = (((cem0 >> 2u) + 1u) << 1u) + (((cem1 >> 2u) + 1u) << 1u) * (parts > 1u ? 1u : 0u) +
                        (((cem2 >> 2u) + 1u) << 1u) * (parts > 2u ? 1u : 0u) +
                        (((cem3 >> 2u) + 1u) << 1u) * (parts > 3u ? 1u : 0u);
    if (values > 18u || KindSeqBits(KIND_TRITS, 1u, values) > color_bits) return 8u;
    KindColourValues(at, color_bits, values);
    uint kind = 0u, colour = 0u, at_value = 0u;
    for (uint i = 0u; i < parts; i++) {
        const uint cem = i == 0u ? cem0 : i == 1u ? cem1 : i == 2u ? cem2 : cem3;
        if (cem == 0u || cem == 1u) { /* luminance, direct and base+offset: R = G = B, alpha 255 */
        } else if (cem == 4u || cem == 5u) { /* luminance and alpha */
            kind |= 2u;
        } else if (cem == 6u || cem == 8u || cem == 9u) { /* RGB: alpha 255; grey ends are luminance */
            colour += KindRgbSpread(cem, at_value) <= SNX_ASTC_KIND_GREY_SPREAD ? 0u : 1u;
        } else if (cem == 10u || cem == 12u || cem == 13u) { /* RGB(A) with alpha; grey ends: luminance and alpha */
            colour += KindRgbSpread(cem, at_value) <= SNX_ASTC_KIND_GREY_SPREAD ? 0u : 1u;
            kind |= 2u;
        } else {
            return 8u; /* HDR: the error colour on that partition's texels */
        }
        at_value += ((cem >> 2u) + 1u) << 1u;
    }
    if (colour != 0u) kind |= 1u | (parts > 1u ? 4u : lines);
    /* a second plane on R, G or B weighs that channel apart: no longer grey, nor one line */
    if (ccs != 3u) kind |= 1u | 4u;
    return kind;
}

#ifndef SNX_ASTC_KIND_NO_MAIN
/* One block of the level a dispatch reads (blocks_x x blocks_y a slice, from in_base), its slice the workgroup's z: its
   kind's bits, each at the block's number in the walk's order (walk_bl: block linear) the slice's least so far in the
   workgroup's shared words (KIND_FIRST), and those least into the level's four words of the slice in first_words
   (out_base + slice * out_layer) */
void main() {
    if (gl_LocalInvocationIndex < 4u) KIND_FIRST(gl_LocalInvocationIndex) = ~0u;
    memoryBarrierShared();
    barrier();
    const uint bx = gl_GlobalInvocationID.x, by = gl_GlobalInvocationID.y, z = gl_WorkGroupID.z;
    if (bx < blocks_x && by < blocks_y) {
        kind_blk = astc_data[in_base + (z * blocks_y + by) * blocks_x + bx];
        const uint kind = BlockKind(block_w, block_h, srgb);
        const uint gob = (by >> 3u) * ((blocks_x + 3u) >> 2u) + (bx >> 2u);
        const uint at = walk_bl != 0u ? (gob << 5u) | ((by & 7u) << 2u) | (bx & 3u) : by * blocks_x + bx;
        for (uint k = 0u; k < 4u; k++)
            if ((kind >> k & 1u) != 0u) atomicMin(KIND_FIRST(k), at);
    }
    memoryBarrierShared();
    barrier();
    if (gl_LocalInvocationIndex < 4u) {
        const uint first = KIND_FIRST(gl_LocalInvocationIndex);
        if (first != ~0u) atomicMin(first_words[out_base + z * out_layer + gl_LocalInvocationIndex], first);
    }
}
#endif
