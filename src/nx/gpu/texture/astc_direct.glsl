/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* BCn blocks made ASTC 4x4 blocks without decoding a texel: the part of texconv.glsl's ASTC build that runs first,
   after astc_encode.glsl. Some BCn blocks are an ASTC layout already, field for field, and for those nothing is
   fitted and nothing is measured: the ends are read out of the block and written in ASTC's encoding, and the weights
   are moved as the 32 or 64 bits they are.

   - BC7 mode 6, one line through RGBA with 16 weights of 4 bits. An opaque block (both ends' alpha 255) is ASTC's one
     partition of RGB ends with 4 bit weights: the same 16 indices (ASTC's 4 bit weights are BC7's within 1 of 64),
     the ends in 192 values a channel, which is within 1 of any byte. One with alpha has RGBA ends, and a block's
     bits then buy 4 bit weights with ends of 48 values or 3 bit weights with ends of 192: the first where the ends
     lie far apart (what a weight's last bit is worth grows with the line's length, an end's precision does not), the
     second where they lie near, a 4 bit index's top three bits being the nearest 3 bit weight for every one of the 16.
   - BC7 modes 4 and 5 with one alpha and no rotation: their colour is one line of 2 or 3 bit weights, the same
     weights ASTC has, and the ends fit as they are (Direct45). Mode 5 with a rotation onto a channel that is 255 at
     both ends is an opaque block of two planes, 2 bit weights each, which ASTC has too.
   - BC1's colour block of four colours, alone or under BC2's or BC3's alpha where that is 255 throughout: one
     partition of RGB ends with 2 bit weights and 8 bit ends, which is the block as it stands, BC1's two ends as
     they decode and its indices in ASTC's order (BC1 counts its ends 0 and 1 and the colours between 2 and 3).
   - A block whose ends are one colour, or whose texels all take one index, is one colour: a void extent, exact.

   BC7's anchor index, texel 0's, is stored a bit short; the indices are moved whole and that one bit put back. The
   decoder swaps a block's ends when the first sums higher than the second: the ends are then written the other way
   round and the weights with every bit turned, which is each index counted from the other end.

   No arrays. This is the pass nearly every block of a game's texture takes, on GPUs whose drivers each have a kind
   of memory they are slow with: an array of the invocation's own indexed by a value (scratch memory on Adreno and
   Mali), or the workgroup's shared memory (fewer workgroups in flight on NVIDIA). So everything here is a word in a
   register: the block is its four words and a field is cut out of them at a bit that is a constant of its mode, an
   end is the word of its four channels, the ends' codes are two words of a byte each, and the ASTC block is four
   words put together with shifts. And no table: an encoding with a trit or a quint is worked out (astc_quant.glsl),
   with the encoding a constant of the block's kind, so a few multiplies a channel.

   What these blocks decode to is not measured here, since no texel is at hand: tests/glsl/astc_encode_glsl.cpp holds
   them to the BCn blocks' own texels, a few steps at most a channel. Every other block is left to the passes that
   decode it (texconv.glsl's main). */

/* SNX_ASTC_DIRECT_NEAR: a mode 6 block with alpha takes 3 bit weights and ends of 192 values while no channel's ends
   are further apart than this, and 4 bit weights with ends of 48 past it */
#ifndef SNX_ASTC_DIRECT_NEAR
#define SNX_ASTC_DIRECT_NEAR 40u
#endif

/* the block a Direct function made */
uvec4 dx_block;

/* n < 32 bits from bit at < 32 of the 64 that lo and hi are */
uint DxCut(uint lo, uint hi, uint at, uint n) {
    return ((lo >> at) | ((hi << 1u) << (31u - at))) & ((1u << n) - 1u);
}

/* byte k of the eight that lo and hi are */
uint DxByte(uint lo, uint hi, uint k) {
    return ((k < 4u ? lo : hi) >> ((k & 3u) * 8u)) & 255u;
}

/* a sequence being written, 64 bits from bit 0 up: v, of bits that end under bit 64, at bit pos */
uint dx_lo;
uint dx_hi;
void DxPut(uint pos, uint v) {
    const bool low = pos < 32u;
    const uint s = pos & 31u;
    dx_lo |= low ? v << s : 0u;
    dx_hi |= low ? (v >> 1u) >> (31u - s) : v << s;
}

/* The ends e0 and e1 (RGBA8 words) as the block's colour values, n of them (6 for RGB ends, 8 for RGBA), in the colour
   encoding range, into dx_lo and dx_hi as the integer sequence the block holds from its bit 17 on. 1 when the ends
   went in the other way round, as the decoder wants the second to sum the higher (red, green and blue as they
   decode): the weights are then turned */
bool DxEnds(uint e0, uint e1, uint n, uint range) {
    /* each channel as the encoding holds it nearest (q) and that value's code (k): the byte itself for 8 bits */
    uint q0 = e0, q1 = e1, k0 = e0, k1 = e1;
    if (range != AS_B8) {
        q0 = 0u;
        q1 = 0u;
        k0 = 0u;
        k1 = 0u;
        for (uint c = 0u; c < 4u; c++) {
            const uint ta = AsQuant(range, Ch(e0, c)), tb = AsQuant(range, Ch(e1, c));
            q0 |= (ta & 255u) << (c * 8u);
            q1 |= (tb & 255u) << (c * 8u);
            k0 |= ((ta >> 8u) & 255u) << (c * 8u);
            k1 |= ((tb >> 8u) & 255u) << (c * 8u);
        }
    }
    const bool turn = Dot4(q1 & 0xFFFFFFu, 0x010101u) < Dot4(q0 & 0xFFFFFFu, 0x010101u);
    const uint f = turn ? k1 : k0, s = turn ? k0 : k1;
    /* red of both ends, then green, blue and alpha */
    const uint lo = (f & 255u) | ((s & 255u) << 8u) | ((f & 0xFF00u) << 8u) | ((s & 0xFF00u) << 16u);
    const uint hi = ((f >> 16u) & 255u) | ((s >> 8u) & 0xFF00u) | ((f >> 8u) & 0xFF0000u) | (s & 0xFF000000u);
    dx_lo = lo;
    dx_hi = n == 8u ? hi : hi & 0xFFFFu;
    if (range == AS_B8) return turn;
    /* a value is a trit or a quint above b bits; five trits share 8 bits and three quints 7, laid between their
       values' bits as the decoder reads them (astc_encode.glsl's AsColours has the same layout) */
    const bool trits = (range & 1u) == 0u;
    const uint b = 6u - ((range + 1u) >> 1u), group = trits ? 5u : 3u, low = (1u << b) - 1u;
    dx_lo = 0u;
    dx_hi = 0u;
    for (uint g = 0u; g * group < n; g++) {
        uint m0 = 0u, m1 = 0u, m2 = 0u, m3 = 0u, m4 = 0u, d0 = 0u, d1 = 0u, d2 = 0u, d3 = 0u, d4 = 0u;
        for (uint k = 0u; k < group; k++) {
            const uint at = g * group + k, code = at < n ? DxByte(lo, hi, at) : 0u;
            m0 = k == 0u ? code & low : m0;
            m1 = k == 1u ? code & low : m1;
            m2 = k == 2u ? code & low : m2;
            m3 = k == 3u ? code & low : m3;
            m4 = k == 4u ? code & low : m4;
            d0 = k == 0u ? code >> b : d0;
            d1 = k == 1u ? code >> b : d1;
            d2 = k == 2u ? code >> b : d2;
            d3 = k == 3u ? code >> b : d3;
            d4 = k == 4u ? code >> b : d4;
        }
        if (trits) {
            const uint at = g * (5u * b + 8u), T = AsTritBits(d0, d1, d2, d3, d4);
            DxPut(at, m0 | ((T & 3u) << b) | (m1 << (b + 2u)));
            DxPut(at + 2u * b + 2u, ((T >> 2u) & 3u) | (m2 << 2u) | (((T >> 4u) & 1u) << (b + 2u)));
            DxPut(at + 3u * b + 5u, m3 | (((T >> 5u) & 3u) << b) | (m4 << (b + 2u)) | ((T >> 7u) << (2u * b + 2u)));
        } else {
            const uint at = g * (3u * b + 7u), Q = AsQuintBits(d0, d1, d2);
            DxPut(at, m0 | ((Q & 7u) << b) | (m1 << (b + 3u)));
            DxPut(at + 2u * b + 3u, ((Q >> 3u) & 3u) | (m2 << 2u) | ((Q >> 5u) << (b + 2u)));
        }
    }
    return turn;
}

/* one partition of ends e0 and e1 (RGBA8 words), endpoint mode cem (8 RGB, 12 RGBA), colour encoding range, weights
   of ib bits: the 16 indices in wlo and whi from bit 0 up, texel after texel. The block mode (a 4x4 grid of weights
   of ib bits) and the endpoint mode from bit 13, the colours from bit 17, and the weights from the top bit down */
uvec4 DxOne(uint e0, uint e1, uint cem, uint ib, uint range, uint wlo, uint whi) {
    const bool turn = DxEnds(e0, e1, cem == 12u ? 8u : 6u, range);
    const uint high = ib == 2u ? 0u : ib == 3u ? 0xFFFFu : 0xFFFFFFFFu;
    return uvec4((ib == 2u ? 0x042u : ib == 3u ? 0x053u : 0x242u) | (cem << 13u) | (dx_lo << 17u),
                 (dx_lo >> 15u) | (dx_hi << 17u), (dx_hi >> 15u) | bitfieldReverse((turn ? ~whi : whi) & high),
                 bitfieldReverse(turn ? ~wlo : wlo));
}

/* 16 indices of 2 bits, a nibble each: the 8 of v's low half, every pair of bits two bits apart */
uint DxSpread(uint v) {
    v = (v | (v << 8u)) & 0x00FF00FFu;
    v = (v | (v << 4u)) & 0x0F0F0F0Fu;
    return (v | (v << 2u)) & 0x33333333u;
}

/* two planes of an opaque block: RGB ends e0 and e1 (160 values a channel), channel plane with the 2 bit weights
   w1, the other two with w0, each 16 indices from bit 0 up. The plane's channel in the two bits under the weights
   (bits 62 and 63); a texel's two weights side by side, the first plane's the lower */
uvec4 DxDual(uint e0, uint e1, uint plane, uint w0, uint w1) {
    const bool turn = DxEnds(e0, e1, 6u, AS_Q5);
    const uint wlo = DxSpread(w0 & 0xFFFFu) | (DxSpread(w1 & 0xFFFFu) << 2u);
    const uint whi = DxSpread(w0 >> 16u) | (DxSpread(w1 >> 16u) << 2u);
    return uvec4(0x442u | (8u << 13u) | (dx_lo << 17u), (dx_lo >> 15u) | (dx_hi << 17u) | (plane << 30u),
                 bitfieldReverse(turn ? ~whi : whi), bitfieldReverse(turn ? ~wlo : wlo));
}

/* a void extent of colour c */
uvec4 DxVoid(uint c) {
    return uvec4(0xFFFFFDFCu, 0xFFFFFFFFu, (Ch(c, 0u) * 257u) | ((Ch(c, 1u) * 257u) << 16u),
                 (Ch(c, 2u) * 257u) | ((Ch(c, 3u) * 257u) << 16u));
}

/* the colour at weight w of 64 between e0 and e1, as BC7 interpolates it (two channels a word, as bc7_encode.glsl's
   LineError) */
uint DxLerp(uint e0, uint e1, uint w) {
    const uint ae = e0 & 0x00FF00FFu, ao = (e0 >> 8u) & 0x00FF00FFu;
    const uint xe = (ae << 6u) + 0x00200020u + ((e1 & 0x00FF00FFu) - ae) * w;
    const uint xo = (ao << 6u) + 0x00200020u + (((e1 >> 8u) & 0x00FF00FFu) - ao) * w;
    return ((xe >> 6u) & 0x00FF00FFu) | ((xo << 2u) & 0xFF00FF00u);
}

/* a BC7 block of mode 6 (its four words) into dx_block: 0 for a block of another mode */
bool Direct7(uint b0, uint b1, uint b2, uint b3) {
    if ((b0 & 0x7Fu) != 0x40u) return false;
    /* red of both ends, then green, blue and alpha, 7 bits each from bit 7; each end's p-bit (bits 63 and 64) under
       its channels */
    const uint p0 = (b1 >> 31u) * 0x01010101u, p1 = (b2 & 1u) * 0x01010101u;
    const uint e0 = ((((b0 >> 7u) & 127u) | (((b0 >> 21u) & 127u) << 8u) | (((b1 >> 3u) & 127u) << 16u) |
                      (((b1 >> 17u) & 127u) << 24u))
                     << 1u) |
                    p0;
    const uint e1 = ((((b0 >> 14u) & 127u) | (DxCut(b0, b1, 28u, 7u) << 8u) | (((b1 >> 10u) & 127u) << 16u) |
                      (((b1 >> 24u) & 127u) << 24u))
                     << 1u) |
                    p1;
    /* the indices, a nibble a texel from bit 65: texel 0's is 3 bits, its top one nought, so texel k's lies at bit
       64 + 4 k but for that one, a bit up */
    const uint lo = (b2 & ~0xFu) | ((b2 >> 1u) & 7u), hi = b3;
    const uint first = lo & 15u;
    if (e0 == e1 || (lo == first * 0x11111111u && hi == first * 0x11111111u)) {
        /* one colour: the ends', or the one every texel's index gives */
        dx_block = DxVoid(DxLerp(e0, e1, (first * 273u + 32u) >> 6u));
        return true;
    }
    if ((e0 & e1) >= 0xFF000000u) {
        dx_block = DxOne(e0, e1, 8u, 4u, AS_T6, lo, hi);
        return true;
    }
    uint apart = 0u;
    for (uint c = 0u; c < 4u; c++) apart = max(apart, uint(abs(int(Ch(e1, c)) - int(Ch(e0, c)))));
    if (apart > SNX_ASTC_DIRECT_NEAR) {
        dx_block = DxOne(e0, e1, 12u, 4u, AS_T4, lo, hi);
        return true;
    }
    /* each nibble's top three bits, the 16 of them closed up: two nibbles to 6 bits, two of those to 12, two to 24 */
    uint tl = (lo >> 1u) & 0x77777777u, th = (hi >> 1u) & 0x77777777u;
    tl = (tl & 0x07070707u) | ((tl >> 1u) & 0x38383838u);
    th = (th & 0x07070707u) | ((th >> 1u) & 0x38383838u);
    tl = (tl & 0x003F003Fu) | ((tl >> 2u) & 0x0FC00FC0u);
    th = (th & 0x003F003Fu) | ((th >> 2u) & 0x0FC00FC0u);
    tl = (tl & 0xFFFu) | ((tl >> 4u) & 0xFFF000u);
    th = (th & 0xFFFu) | ((th >> 4u) & 0xFFF000u);
    dx_block = DxOne(e0, e1, 12u, 3u, AS_T6, tl | (th << 24u), th >> 8u);
    return true;
}

/* A BC7 block of mode 4 or 5 whose alpha is one value and which has no rotation into dx_block: its colour is then one
   line of 2 or 3 bit weights, which ASTC's one partition holds with the same weights (they are BC7's) and, for an
   opaque block's RGB ends or 2 bit weights, ends of 8 bits: the block as it stands. Every block of one colour a BC7
   encoder writes this way is among them. And a mode 5 block of two planes that is opaque (below). 0 for any other
   block: two planes with alpha are RGBA ends of 48 values a channel, too coarse to take unmeasured */
bool Direct45(uint b0, uint b1, uint b2, uint b3) {
    const bool m5 = (b0 & 0x3Fu) == 0x20u;
    if (!m5 && (b0 & 0x1Fu) != 0x10u) return false;
    /* Mode 5: a rotation of 2 bits from bit 6, colour ends of 7 bits from bit 8 (red of both, then green and blue),
       alpha ends of 8 from bit 50, then two streams of 2 bit indices, 31 bits each, from bits 66 and 97.
       Mode 4: the rotation from bit 5, an index selection bit, colour ends of 5 bits from bit 8, alpha ends of 6 from
       bit 38, a stream of 2 bit indices (31 bits from bit 50) and one of 3 bit ones (47 from bit 81), which the
       selection bit gives the colour. The ends widened to 8 bits by their top bits */
    uint rot, e0, e1, a0, a1, s2, t2, t3lo, t3hi;
    if (m5) {
        rot = (b0 >> 6u) & 3u;
        e0 = ((b0 >> 8u) & 127u) | (((b0 >> 22u) & 127u) << 8u) | (((b1 >> 4u) & 127u) << 16u);
        e1 = ((b0 >> 15u) & 127u) | (DxCut(b0, b1, 29u, 7u) << 8u) | (((b1 >> 11u) & 127u) << 16u);
        e0 = (e0 << 1u) | ((e0 >> 6u) & 0x010101u);
        e1 = (e1 << 1u) | ((e1 >> 6u) & 0x010101u);
        a0 = (b1 >> 18u) & 255u;
        a1 = DxCut(b1, b2, 26u, 8u);
        s2 = DxCut(b2, b3, 2u, 31u);
        t2 = b3 >> 1u;
        t3lo = 0u;
        t3hi = 0u;
    } else {
        rot = (b0 >> 5u) & 3u;
        e0 = ((b0 >> 8u) & 31u) | (((b0 >> 18u) & 31u) << 8u) | (DxCut(b0, b1, 28u, 5u) << 16u);
        e1 = ((b0 >> 13u) & 31u) | (((b0 >> 23u) & 31u) << 8u) | (((b1 >> 1u) & 31u) << 16u);
        e0 = (e0 << 3u) | ((e0 >> 2u) & 0x070707u);
        e1 = (e1 << 3u) | ((e1 >> 2u) & 0x070707u);
        a0 = (b1 >> 6u) & 63u;
        a1 = (b1 >> 12u) & 63u;
        a0 = (a0 << 2u) | (a0 >> 4u);
        a1 = (a1 << 2u) | (a1 >> 4u);
        s2 = DxCut(b1, b2, 18u, 31u);
        t2 = 0u;
        /* the 3 bit stream's 24 bits from bit 81 and 23 from bit 105, texel 0's index 2 bits: a nought put above it */
        const uint p0 = DxCut(b2, b3, 17u, 24u), p1 = b3 >> 9u;
        t3lo = (p0 & 3u) | ((p0 & ~3u) << 1u) | (p1 << 25u);
        t3hi = p1 >> 7u;
    }
    /* a stream of 2 bit indices, texel 0's 1 bit: a nought put above it */
    const uint w2 = (s2 & 1u) | ((s2 & ~1u) << 1u);
    if (rot != 0u) {
        /* A rotation swaps the fourth channel with red, green or blue as the texels are decoded. Mode 5 with that
           colour channel 255 at both ends is then an opaque block of two planes: the swapped channel takes the fourth
           one's ends and weights, and the other two keep the colour's. Both streams are 2 bit indices, ASTC's own
           weights, and RGB ends of two planes have 160 values a channel, within 1 of any byte */
        const uint sh = (rot - 1u) * 8u;
        if (!m5 || (((e0 & e1) >> sh) & 0xFFu) != 255u) return false;
        dx_block = DxDual((e0 & ~(0xFFu << sh)) | (a0 << sh), (e1 & ~(0xFFu << sh)) | (a1 << sh), rot - 1u, w2,
                          (t2 & 1u) | ((t2 & ~1u) << 1u));
        return true;
    }
    if (a0 != a1) return false;
    e0 |= a0 << 24u;
    e1 |= a0 << 24u;
    /* the colour's weights: mode 4's 3 bit ones where its selection bit says */
    const bool three = !m5 && (b0 & 0x80u) != 0u;
    const uint wlo = three ? t3lo : w2, whi = three ? t3hi : 0u;
    /* every index alike: an index every 3 bits, 16 of them 48 bits, the eleventh's top bit the high word's first */
    const uint i = wlo & (three ? 7u : 3u);
    const bool same = three ? wlo == i * 0x49249249u && whi == ((i >> 2u) | (i * 0x2492u)) : wlo == i * 0x55555555u;
    if ((e0 & 0xFFFFFFu) == (e1 & 0xFFFFFFu) || same) {
        dx_block = DxVoid(DxLerp(e0, e1, (i * (three ? 585u : 1365u) + 32u) >> 6u));
        return true;
    }
    /* RGBA ends with 3 bit weights have 192 values a channel, not 8 bits: the block's bits don't reach */
    dx_block = DxOne(e0, e1, a0 == 255u ? 8u : 12u, three ? 3u : 2u, a0 != 255u && three ? AS_T6 : AS_B8, wlo, whi);
    return true;
}

/* a BC1 colour block (its two ends in ce, its indices in ci) of four colours into dx_block; four: under BC2's or
   BC3's alpha, which takes every block as four colours. 0 for a BC1 block of three colours and transparent black */
bool DirectBc1(uint ce, uint ci, bool four) {
    const uint a = ce & 0xFFFFu, b = ce >> 16u;
    if (!four && a <= b) return false;
    const uint e0 = tc_rgba(bc_expand(a >> 11u, 5u), bc_expand((a >> 5u) & 63u, 6u), bc_expand(a & 31u, 5u), 255u);
    const uint e1 = tc_rgba(bc_expand(b >> 11u, 5u), bc_expand((b >> 5u) & 63u, 6u), bc_expand(b & 31u, 5u), 255u);
    if (a == b || ci == (ci & 3u) * 0x55555555u) {
        /* one colour: the texel every index names, as BC1 decodes it */
        dx_block = DxVoid(bc1_pixel(ce, ci, false, 0u));
        return true;
    }
    /* BC1's index to ASTC's weight: 0 and 1 are the ends (weights 0 and 3), 2 and 3 the colours a third and two
       thirds along (1 and 2). Of an index's two bits, the low one becomes the high, and their difference the low */
    const uint w = ((ci & 0x55555555u) << 1u) | ((ci ^ (ci >> 1u)) & 0x55555555u);
    dx_block = DxOne(e0, e1, 8u, 2u, AS_B8, w, 0u);
    return true;
}

/* 1 where a BC3 alpha block (its two words) is 255 for every texel: both its ends 255, which makes it the kind of six
   values and 0 and 255, and no texel's index the 0's (6). The 16 indices are 3 bits each from bit 16 */
bool Bc3Opaque(uint lo, uint hi) {
    if ((lo & 0xFFFFu) != 0xFFFFu) return false;
    /* ten indices, then six: each xor 6 is nought where the index is 6 */
    const uint ten = (((lo >> 16u) | (hi << 16u)) & 0x3FFFFFFFu) ^ 0x36DB6DB6u;
    const uint six = (hi >> 14u) ^ 0x36DB6u;
    return (((ten | (ten >> 1u) | (ten >> 2u)) & 0x09249249u) == 0x09249249u) &&
           (((six | (six >> 1u) | (six >> 2u)) & 0x09249u) == 0x09249u);
}
