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

   BC7's anchor index, texel 0's, is stored a bit short; the indices are moved whole and that one bit put back
   (Direct7). The decoder swaps a block's ends when the first sums higher than the second (astc_encode.glsl's
   AsEnds): the weights are then every bit turned, which is each index counted from the other end.

   What these blocks decode to is not measured here, since no texel is at hand: tests/glsl/astc_encode_glsl.cpp holds
   them to the BCn blocks' own texels, a few steps at most a channel. Every other block is left to the passes that
   decode it (texconv.glsl's main). */

/* SNX_ASTC_DIRECT_NEAR: a mode 6 block with alpha takes 3 bit weights and ends of 192 values while no channel's ends
   are further apart than this, and 4 bit weights with ends of 48 past it */
#ifndef SNX_ASTC_DIRECT_NEAR
#define SNX_ASTC_DIRECT_NEAR 40u
#endif

/* the block Direct7 or DirectBc1 made */
uvec4 dx_block;

/* one partition of ends e0 and e1 (RGBA8 words), endpoint mode cem (8 RGB, 12 RGBA), colour encoding range, weights
   of ib bits: the 16 indices in wlo and whi from bit 0 up, texel after texel */
uvec4 DxOne(uint e0, uint e1, uint cem, uint ib, uint range, uint wlo, uint whi) {
    uint q0 = e0, q1 = e1;
    if (range != AS_B8) {
        /* each end's channel as the encoding holds it nearest */
        q0 = 0u;
        q1 = 0u;
        for (uint c = 0u; c < 4u; c++) {
            const uint a = Ch(e0, c), b = Ch(e1, c);
            q0 |= AsByte(ASTC_NEAR[range * 64u + (a >> 2u)], a) << (c * 8u);
            q1 |= AsByte(ASTC_NEAR[range * 64u + (b >> 2u)], b) << (c * 8u);
        }
    }
    as_cem = cem;
    as_enc = range;
    as_nv = 0u;
    as_s0[0] = q0;
    as_s1[0] = q1;
    enc = uvec4(0u);
    Put(0u, ib == 2u ? 0x042u : ib == 3u ? 0x053u : 0x242u, 11u);
    Put(13u, cem, 4u);
    const bool turn = AsEnds(0u);
    AsColours(17u);
    /* the weights from the block's top bit down; turned with the ends, each index counted from the other end */
    const uint high = ib == 2u ? 0u : ib == 3u ? 0xFFFFu : 0xFFFFFFFFu;
    enc.w |= bitfieldReverse(turn ? ~wlo : wlo);
    enc.z |= bitfieldReverse((turn ? ~whi : whi) & high);
    return enc;
}

/* 16 indices of 2 bits, a nibble each: the 8 of v's low half, every pair of bits two bits apart */
uint DxSpread(uint v) {
    v = (v | (v << 8u)) & 0x00FF00FFu;
    v = (v | (v << 4u)) & 0x0F0F0F0Fu;
    return (v | (v << 2u)) & 0x33333333u;
}

/* two planes of an opaque block: RGB ends e0 and e1 (160 values a channel), channel plane with the 2 bit weights
   w1, the other two with w0, each 16 indices from bit 0 up */
uvec4 DxDual(uint e0, uint e1, uint plane, uint w0, uint w1) {
    uint q0 = 0u, q1 = 0u;
    for (uint c = 0u; c < 3u; c++) {
        const uint a = Ch(e0, c), b = Ch(e1, c);
        q0 |= AsByte(ASTC_NEAR[AS_Q5 * 64u + (a >> 2u)], a) << (c * 8u);
        q1 |= AsByte(ASTC_NEAR[AS_Q5 * 64u + (b >> 2u)], b) << (c * 8u);
    }
    as_cem = 8u;
    as_enc = AS_Q5;
    as_nv = 0u;
    as_s0[0] = q0;
    as_s1[0] = q1;
    enc = uvec4(0u);
    Put(0u, 0x442u, 11u);
    Put(13u, 8u, 4u);
    const bool turn = AsEnds(0u);
    AsColours(17u);
    Put(62u, plane, 2u);
    /* a texel's two weights side by side, the first plane's the lower */
    const uint wlo = DxSpread(w0 & 0xFFFFu) | (DxSpread(w1 & 0xFFFFu) << 2u);
    const uint whi = DxSpread(w0 >> 16u) | (DxSpread(w1 >> 16u) << 2u);
    enc.w |= bitfieldReverse(turn ? ~wlo : wlo);
    enc.z |= bitfieldReverse(turn ? ~whi : whi);
    return enc;
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

/* a BC7 block of mode 6 into dx_block: 0 for a block of another mode */
bool Direct7(uint b[4]) {
    if ((b[0] & 0x7Fu) != 0x40u) return false;
    /* red of both ends, then green, blue and alpha, 7 bits each from bit 7; each end's p-bit under its channels */
    const uint p0 = b[1] >> 31u, p1 = b[2] & 1u;
    uint e0 = 0u, e1 = 0u;
    for (uint c = 0u; c < 4u; c++) {
        e0 |= ((bc_bits(b, 7u + 14u * c, 7u) << 1u) | p0) << (c * 8u);
        e1 |= ((bc_bits(b, 14u + 14u * c, 7u) << 1u) | p1) << (c * 8u);
    }
    /* the indices, a nibble a texel from bit 65: texel 0's is 3 bits, its top one nought, so texel k's lies at bit
       64 + 4 k but for that one, a bit up */
    const uint lo = (b[2] & ~0xFu) | ((b[2] >> 1u) & 7u), hi = b[3];
    const uint first = lo & 15u;
    if (e0 == e1 || (lo == first * 0x11111111u && hi == first * 0x11111111u)) {
        /* one colour: the ends', or the one every texel's index gives, as BC7 interpolates it (two channels a word,
           as bc7_encode.glsl's LineError) */
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
bool Direct45(uint b[4]) {
    const bool m5 = (b[0] & 0x3Fu) == 0x20u;
    if (!m5 && (b[0] & 0x1Fu) != 0x10u) return false;
    /* mode 5: a rotation of 2 bits from bit 6, colour ends of 7 bits from bit 8, alpha ends of 8, then two streams
       of 2 bit indices. Mode 4: the rotation from bit 5, an index selection bit, colour ends of 5 bits and alpha ends
       of 6, a stream of 2 bit indices and one of 3 bit ones, which the selection bit gives the colour */
    const uint rot = bc_bits(b, m5 ? 6u : 5u, 2u);
    const uint cb = m5 ? 7u : 5u, ab = m5 ? 8u : 6u, at = 8u + 6u * cb;
    const uint a0 = uint(bc_expand(bc_bits(b, at, ab), ab)), a1 = uint(bc_expand(bc_bits(b, at + ab, ab), ab));
    uint e0 = a0 << 24u, e1 = a1 << 24u;
    for (uint c = 0u; c < 3u; c++) {
        e0 |= uint(bc_expand(bc_bits(b, 8u + 2u * cb * c, cb), cb)) << (c * 8u);
        e1 |= uint(bc_expand(bc_bits(b, 8u + cb + 2u * cb * c, cb), cb)) << (c * 8u);
    }
    const uint first = at + 2u * ab; /* the 2 bit stream, 31 bits; the other follows it */
    if (rot != 0u) {
        /* A rotation swaps the fourth channel with red, green or blue as the texels are decoded. Mode 5 with that
           colour channel 255 at both ends is then an opaque block of two planes: the swapped channel takes the fourth
           one's ends and weights, and the other two keep the colour's. Both streams are 2 bit indices, ASTC's own
           weights, and RGB ends of two planes have 160 values a channel, within 1 of any byte */
        const uint sh = (rot - 1u) * 8u;
        if (!m5 || ((e0 & e1) >> sh & 0xFFu) != 255u) return false;
        const uint p0 = bc_bits(b, first, 31u), p1 = bc_bits(b, first + 31u, 31u);
        dx_block = DxDual((e0 & ~(0xFFu << sh)) | (a0 << sh), (e1 & ~(0xFFu << sh)) | (a1 << sh), rot - 1u,
                          (p0 & 1u) | ((p0 & ~1u) << 1u), (p1 & 1u) | ((p1 & ~1u) << 1u));
        return true;
    }
    if (a0 != a1) return false;
    /* the colour's indices, texel 0's a bit short: the bit put back as a nought above it */
    const bool three = !m5 && (b[0] & 0x80u) != 0u;
    uint wlo, whi = 0u, same;
    if (three) {
        const uint p0 = bc_bits(b, first + 31u, 24u), p1 = bc_bits(b, first + 55u, 23u);
        wlo = (p0 & 3u) | ((p0 & ~3u) << 1u) | (p1 << 25u);
        whi = p1 >> 7u;
        /* an index every 3 bits, 16 of them 48 bits: the eleventh's top bit is the high word's first */
        const uint i = wlo & 7u;
        same = wlo == i * 0x49249249u && whi == ((i >> 2u) | (i * 0x2492u)) ? 1u : 0u;
    } else {
        const uint p = bc_bits(b, first, 31u);
        wlo = (p & 1u) | ((p & ~1u) << 1u);
        same = wlo == (wlo & 3u) * 0x55555555u ? 1u : 0u;
    }
    if ((e0 & 0xFFFFFFu) == (e1 & 0xFFFFFFu) || same != 0u) {
        const uint i = wlo & (three ? 7u : 3u);
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
