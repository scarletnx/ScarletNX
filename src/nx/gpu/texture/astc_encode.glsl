/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ASTC 4x4 encoding: AstcEncode makes an LDR block of the 16 RGBA8 texels in px, for a GPU that samples ASTC and not
   BCn (texconv.comp's ASTC build decodes a BCn block and encodes it again, as its ETC2 build does). Included after
   astc_encode_tables.glsl and bc7_encode.glsl built with SNX_ASTC_ENC 1.

   The fitting is bc7_encode.glsl's: a subset's texels are fitted a line (FitSubset: the box, then least squares, each
   texel at the best of the three weights around its place), and ASTC interpolates as BC7 does, ((64 - w) a + w b +
   32) >> 6, with the same weights of 2 and 3 bits. What differs is what a block's 128 bits buy, which is what the
   candidates are:

   - a block of one colour: a void extent, the colour in 16 bits a channel, exact;
   - one partition, the weights a 4x4 grid (one a texel): an opaque block as RGB ends (endpoint mode 8) with 3 bit
     weights and ends of 8 bits, or 4 bit weights and ends of 192 values; any other as RGBA ends (mode 12) with 2 bit
     weights and ends of 8 bits, 3 bit weights and 192 values, or 4 bit weights and 48;
   - two planes, alpha with weights of its own (mode 12, 2 bit weights each, ends of 48 values): what BC7's modes 4
     and 5 hold;
   - two partitions of an opaque block, RGB ends each (2 bit weights, ends of 40 values): the partition of ASTC_P2's
     64 whose subsets spread least, BC7's 64 shapes as near as ASTC's seeds come.

   How many values an end's channel takes is not written in the block: the decoder works it out from the bits the
   weights and the header leave, as the largest encoding that fits, so each layout above has exactly one, and the ends
   are quantized to it while they are fitted (bc7_encode.glsl's Quant under SNX_ASTC_ENC), not after. The ones with a
   trit or a quint come from tables made by turning the decoder round (tools/astc_tables.py).

   The candidate whose squared error is least is written; a block stops trying once one is SNX_ASTC_GOOD or under.
   Integer arithmetic but for the fitter's reciprocal, so tests/glsl/astc_encode_glsl.cpp runs this file on the CPU and
   gets the GPU's bytes, and holds each block's error to the reference decoder's (tests/support/astc.c). */

/* the colour encodings with a trit or a quint, ASTC_NEAR's and ASTC_CODE's order: trits over 6, 5 and 4 bits are the
   even ones, quints over 5, 4 and 3 the odd ones; then 8 bits as they are */
#define AS_T6 0u
#define AS_Q5 1u
#define AS_T5 2u
#define AS_Q4 3u
#define AS_T4 4u
#define AS_Q3 5u
#define AS_B8 6u

/* A block stops trying candidates once its squared error over its texels and channels is this or less (bc7_encode.
   glsl's SNX_BC7_GOOD_ENOUGH: a mean squared error of a quarter, 54 dB) */
#ifndef SNX_ASTC_GOOD
#define SNX_ASTC_GOOD 16
#endif

/* the best candidate so far: its error; partitions, endpoint mode, colour encoding and weight bits; each subset's
   ends as they decode (RGBA8 words); the weights, a nibble a texel; with two planes (as_dual) alpha's weights apart;
   with two partitions which of ASTC_P2's */
uint as_err;
uint as_np;
uint as_cem;
uint as_enc;
uint as_ib;
uint as_s0[2];
uint as_s1[2];
uint as_lo;
uint as_hi;
uint as_dual;
uint as_lo2;
uint as_hi2;
uint as_part;

/* the fitter's shape for a candidate: ns subsets of partition part, weights of ib bits, ends of colour encoding
   range, alpha fitted or 255, the channels of mask weighed. An end is the 8 bits it decodes to, with no p-bit */
void AsShape(uint ns, uint part, uint ib, uint range, uint alpha, uint mask) {
    SetShape(ns, part);
    sh_cb = 8u;
    sh_pk = 0u;
    sh_ib = ib;
    sh_alpha = alpha;
    sh_mask = mask;
    as_range = range;
}

/* one partition, RGB ends (cem 8) or RGBA (12): kept when it is the best so far */
void AsTryOne(uint cem, uint ib, uint range) {
    AsShape(1u, 0u, ib, range, cem == 12u ? 1u : 0u, 15u);
    FitSubset(0u);
    if (fit_err >= as_err) return;
    as_err = fit_err;
    as_np = 1u;
    as_cem = cem;
    as_enc = range;
    as_ib = ib;
    as_s0[0] = fit_s0;
    as_s1[0] = fit_s1;
    as_lo = fit_lo;
    as_hi = fit_hi;
    as_dual = 0u;
}

/* two planes: the colour's line, then alpha's unless the colour's error already passes the best */
void AsTryDual() {
    AsShape(1u, 0u, 2u, AS_T4, 0u, 7u);
    FitSubset(0u);
    if (fit_err >= as_err) return;
    const uint err = fit_err, s0 = fit_s0, s1 = fit_s1, lo = fit_lo, hi = fit_hi;
    sh_mask = 8u;
    sh_alpha = 1u;
    FitSubset(0u);
    if (err + fit_err >= as_err) return;
    as_err = err + fit_err;
    as_np = 1u;
    as_cem = 12u;
    as_enc = AS_T4;
    as_ib = 2u;
    as_s0[0] = (s0 & 0xFFFFFFu) | (fit_s0 & 0xFF000000u);
    as_s1[0] = (s1 & 0xFFFFFFu) | (fit_s1 & 0xFF000000u);
    as_lo = lo;
    as_hi = hi;
    as_dual = 1u;
    as_lo2 = fit_lo;
    as_hi2 = fit_hi;
}

/* two partitions of an opaque block, part of ASTC_P2's: each subset's line, left once their error passes the best */
void AsTryTwo(uint part) {
    AsShape(2u, part, 2u, AS_Q3, 0u, 15u);
    uint err = 0u, lo = 0u, hi = 0u, s00 = 0u, s01 = 0u;
    for (uint s = 0u; s < 2u; s++) {
        FitSubset(s);
        err += fit_err;
        if (err >= as_err) return;
        lo |= fit_lo;
        hi |= fit_hi;
        if (s == 0u) {
            s00 = fit_s0;
            s01 = fit_s1;
        }
    }
    as_err = err;
    as_np = 2u;
    as_cem = 8u;
    as_enc = AS_Q3;
    as_ib = 2u;
    as_s0[0] = s00;
    as_s1[0] = s01;
    as_s0[1] = fit_s0;
    as_s1[1] = fit_s1;
    as_lo = lo;
    as_hi = hi;
    as_dual = 0u;
    as_part = part;
}

/* the weights, which the block holds from its top bit down: written here from bit 0 up, and turned round at the end */
uvec4 as_w;
void PutW(uint pos, uint value, uint count) {
    const uint w = pos >> 5u, b = pos & 31u;
    as_w[w] |= value << b;
    if (b + count > 32u) as_w[w + 1u] |= value >> (32u - b);
}

/* a table's byte */
uint AsByte(uint word, uint i) {
    return (word >> ((i & 3u) * 8u)) & 255u;
}

/* the colour values of the block, in the order the endpoint modes read them, and how many */
uint as_v[12];
uint as_nv;
/* a subset's ends as mode 8's or 12's values: red of both ends, then green, blue and (12) alpha. The decoder takes
   the second end for the one whose red, green and blue sum higher, and swaps them (with a blue contraction this
   encoder has no use for) otherwise: 1 when the ends go in the other way round, the subset's weights then turned */
bool AsEnds(uint s) {
    const uint a = as_s0[s], b = as_s1[s];
    const bool turn = Ch(b, 0u) + Ch(b, 1u) + Ch(b, 2u) < Ch(a, 0u) + Ch(a, 1u) + Ch(a, 2u);
    const uint e0 = turn ? b : a, e1 = turn ? a : b;
    for (uint c = 0u; c < (as_cem == 12u ? 4u : 3u); c++) {
        as_v[as_nv] = Ch(e0, c);
        as_v[as_nv + 1u] = Ch(e1, c);
        as_nv += 2u;
    }
    return turn;
}

/* as_v's values as an integer sequence of the block's colour encoding, from bit pos. With a trit or a quint a value
   is that and some bits (ASTC_CODE); five trits share 8 bits and three quints 7, laid between their values' bits as
   the decoder reads them (astc_decode.glsl's DecodeSequence). A last group of fewer values is cut short: its missing
   digits are 0, which leaves the cut bits 0 (the tables keep each group's smallest code) */
void AsColours(uint pos) {
    if (as_enc == AS_B8) {
        for (uint k = 0u; k < as_nv; k++) Put(pos + 8u * k, as_v[k], 8u);
        return;
    }
    const bool trits = (as_enc & 1u) == 0u;
    const uint b = 6u - ((as_enc + 1u) >> 1u), group = trits ? 5u : 3u;
    for (uint g = 0u; g * group < as_nv; g++) {
        uint m0 = 0u, m1 = 0u, m2 = 0u, m3 = 0u, m4 = 0u, digits = 0u, scale = 1u;
        for (uint k = 0u; k < group; k++) {
            uint code = 0u;
            if (g * group + k < as_nv) {
                const uint v = as_v[g * group + k];
                code = AsByte(ASTC_CODE[as_enc * 64u + (v >> 2u)], v);
            }
            const uint m = code & ((1u << b) - 1u);
            digits += (code >> b) * scale;
            scale *= trits ? 3u : 5u;
            if (k == 0u) m0 = m;
            if (k == 1u) m1 = m;
            if (k == 2u) m2 = m;
            if (k == 3u) m3 = m;
            if (k == 4u) m4 = m;
        }
        if (trits) {
            const uint at = pos + g * (5u * b + 8u), T = AsByte(ASTC_TRITS[digits >> 2u], digits);
            Put(at, m0, b);
            Put(at + b, T & 3u, 2u);
            Put(at + b + 2u, m1, b);
            Put(at + 2u * b + 2u, (T >> 2u) & 3u, 2u);
            Put(at + 2u * b + 4u, m2, b);
            Put(at + 3u * b + 4u, (T >> 4u) & 1u, 1u);
            Put(at + 3u * b + 5u, m3, b);
            Put(at + 4u * b + 5u, (T >> 5u) & 3u, 2u);
            Put(at + 4u * b + 7u, m4, b);
            Put(at + 5u * b + 7u, T >> 7u, 1u);
        } else {
            const uint at = pos + g * (3u * b + 7u), Q = AsByte(ASTC_QUINTS[digits >> 2u], digits);
            Put(at, m0, b);
            Put(at + b, Q & 7u, 3u);
            Put(at + b + 3u, m1, b);
            Put(at + 2u * b + 3u, (Q >> 3u) & 3u, 2u);
            Put(at + 2u * b + 5u, m2, b);
            Put(at + 3u * b + 5u, Q >> 5u, 2u);
        }
    }
}

/* the block the best candidate is */
uvec4 AsPack() {
    const uint top = (1u << as_ib) - 1u, planes = 1u + as_dual;
    enc = uvec4(0u);
    as_w = uvec4(0u);
    as_nv = 0u;
    /* the block mode: a 4x4 grid of weights, of 2, 3 or 4 bits (the last with the mode's precision bit), two planes */
    Put(0u, (as_ib == 2u ? 0x042u : as_ib == 3u ? 0x053u : 0x242u) | (as_dual << 10u), 11u);
    Put(11u, as_np - 1u, 2u);
    uint turned = 0u, pos = 17u, flip = 0u;
    if (as_np == 1u) {
        Put(13u, as_cem, 4u);
        turned = AsEnds(0u) ? 1u : 0u;
        sh_pbits = 0u;
    } else {
        /* the partition's seed, then the endpoint mode of both; ASTC's partition 0 first, which is subset 1 where the
           table says so */
        const uint seed = ASTC_SEED2[as_part];
        flip = seed >> 10u;
        Put(13u, seed & 1023u, 10u);
        Put(23u, as_cem << 2u, 6u);
        pos = 29u;
        SetShape(2u, as_part);
        const bool t_first = AsEnds(flip), t_second = AsEnds(1u - flip);
        turned = flip != 0u ? (t_first ? 2u : 0u) | (t_second ? 1u : 0u) : (t_first ? 1u : 0u) | (t_second ? 2u : 0u);
    }
    AsColours(pos);
    /* the weights, texel after texel (the two planes' side by side), a subset's turned with its ends; and with two
       planes the channel that has the second, alpha, in the two bits under them */
    for (uint k = 0u; k < 16u; k++) {
        const bool turn = ((turned >> SubsetOf(k)) & 1u) != 0u;
        const uint w0 = Index(as_lo, as_hi, k);
        PutW(k * planes * as_ib, turn ? top - w0 : w0, as_ib);
        if (as_dual != 0u) {
            const uint w1 = Index(as_lo2, as_hi2, k);
            PutW((k * 2u + 1u) * as_ib, turn ? top - w1 : w1, as_ib);
        }
    }
    if (as_dual != 0u) Put(126u - 32u * as_ib, 3u, 2u);
    enc.x |= bitfieldReverse(as_w.w);
    enc.y |= bitfieldReverse(as_w.z);
    enc.z |= bitfieldReverse(as_w.y);
    enc.w |= bitfieldReverse(as_w.x);
    return enc;
}

uvec4 AstcEncode() {
    bool all_opaque = true, solid = true;
    for (uint k = 0u; k < 16u; k++) {
        all_opaque = all_opaque && Ch(px[k], 3u) == 255u;
        solid = solid && px[k] == px[0];
    }
    as_err = 0xFFFFFFFFu;
    if (solid) {
        /* a void extent with no extent (its coordinates all ones): the colour, each channel 16 bits */
        const uint c = px[0];
        as_err = 0u;
        return uvec4(0xFFFFFDFCu, 0xFFFFFFFFu, (Ch(c, 0u) * 257u) | ((Ch(c, 1u) * 257u) << 16u),
                     (Ch(c, 2u) * 257u) | ((Ch(c, 3u) * 257u) << 16u));
    }
    /* the candidates, in turn, until one is good enough: one partition in each layout the block's alpha allows, then
       two planes, or two partitions for an opaque block */
    for (uint j = 0u; j < 4u && as_err > uint(SNX_ASTC_GOOD); j++) {
        if (j < 3u) {
            if (all_opaque) {
                if (j == 2u) continue;
                AsTryOne(8u, j == 0u ? 4u : 3u, j == 0u ? AS_T6 : AS_B8);
            } else {
                AsTryOne(12u, j == 0u ? 3u : j == 1u ? 4u : 2u, j == 0u ? AS_T6 : j == 1u ? AS_T4 : AS_B8);
            }
        } else if (!all_opaque) {
            AsTryDual();
        } else {
            BlockSums();
            uint part = 0u, cost = 0xFFFFFFFFu;
            for (uint p = 0u; p < 64u; p++) {
                const uint c = Spread(2u, p, 0u);
                if (c < cost) {
                    part = p;
                    cost = c;
                }
            }
            AsTryTwo(part);
        }
    }
    return AsPack();
}
