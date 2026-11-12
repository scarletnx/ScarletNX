/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ASTC's colour encodings with a trit or a quint, the encoder's way round, worked out and not looked up: the value an
   encoding holds nearest a colour with its code, and the bits five trits or three quints share. No table: 768 words
   of one read at a colour is a cache line a read, on every GPU, and an array a shader indexes is memory mobile GPUs
   are slow with besides. Included before bc7_encode.glsl by the ASTC builds (its Quant under SNX_ASTC_ENC),
   astc_encode.glsl and astc_direct.glsl. tests/glsl/astc_encode_glsl.cpp holds every answer here to the decoder's
   rules turned round by search.

   A value is a digit d (a trit or a quint) over b bits m. The decoder (astc_decode.glsl's ColorParams and
   UnquantizeTQ) makes of it (d C + ((m >> 1) M >> S)) >> 2 where m's lowest bit is 0, and 255 less that where it is
   1: C, M and S are constants of the encoding. So the values come in two halves that mirror each other about 127.5,
   and in the lower half they rise with (m >> 1) D + d, D being 3 or 5: that number is a value's rank r among its
   half's, and the value of rank r is a multiply and a shift. The values are not quite evenly spaced, but nearly
   enough that the rank of the one nearest a colour u under 128 is (u A + B) >> 10 for an A and a B of the encoding,
   found by search over every u, in all but one encoding (trits over 5 bits, which no block layout of the encoder's
   comes to: there the rank is that or the one beside it, and both are worked out). A colour of 128 or more is 255
   less one under 128. Of two values as near, the one further from the middle is the answer. */

/* the colour encodings with a trit or a quint: trits over 6, 5 and 4 bits are the even ones (192, 96 and 48 values),
   quints over 5, 4 and 3 the odd ones (160, 80 and 40); then 8 bits as they are */
#define AS_T6 0u
#define AS_Q5 1u
#define AS_T5 2u
#define AS_Q4 3u
#define AS_T4 4u
#define AS_Q3 5u
#define AS_B8 6u

/* An encoding's constants as two words, for a caller that quantizes to one encoding many times (the fitter): A in
   x's low half and B in its high; in y C, then M from bit 8, S from bit 20, b from bit 24, bit 28 on for trits and
   bit 29 where the rank may be the one beside */
uvec2 AsKey(uint range) {
    const uint b = 6u - ((range + 1u) >> 1u);
    const uint A = range == AS_T6 ? 768u : range == AS_Q5 ? 640u : range == AS_T5 ? 381u : range == AS_Q4 ? 318u :
                   range == AS_T4 ? 189u : 157u;
    const uint B = range < 3u ? 512u : range == AS_Q4 ? 560u : range == AS_T4 ? 525u : 534u;
    const uint C = ((range < 4u ? 0x0D0B0605u : 0x1A16u) >> ((range & 3u) * 8u)) & 255u;
    const uint M = range < 2u ? 257u : range < 4u ? 129u : range == 4u ? 65u : 261u;
    const uint S = (0x101234u >> (range * 4u)) & 15u;
    return uvec2(A | (B << 16u), C | (M << 8u) | (S << 20u) | (b << 24u) | ((~range & 1u) << 28u) |
                                     (range == AS_T5 ? 1u << 29u : 0u));
}

/* the value of rank r in the lower half of the encoding of constants y, with its code d << b | m above it (bit 8 on) */
uint AsRank(uint y, uint r) {
    const bool trit = (y & (1u << 28u)) != 0u;
    /* r / 3 is r 171 >> 9 and r / 5 is r 205 >> 10 for r under 256 */
    const uint j = trit ? (r * 171u) >> 9u : (r * 205u) >> 10u, d = r - j * (trit ? 3u : 5u);
    return ((d * (y & 255u) + ((j * ((y >> 8u) & 0xFFFu)) >> ((y >> 20u) & 15u))) >> 2u) |
           (((d << ((y >> 24u) & 15u)) | (j << 1u)) << 8u);
}

/* The value the encoding of key (AsKey) holds nearest t, 0 to 255, in the low byte, and that value's code, d << b | m,
   in the next */
uint AsQuantKey(uvec2 key, uint t) {
    const bool high = t >= 128u;
    const uint u = high ? 255u - t : t;
    const uint r = (u * (key.x & 0xFFFFu) + (key.x >> 16u)) >> 10u;
    uint q = AsRank(key.y, r);
    if ((key.y & (1u << 29u)) != 0u) {
        /* the rank beside it on u's side (a value over u is never at rank 0; 47 is the half's last) */
        const uint v = q & 255u, other = AsRank(key.y, v > u ? r - 1u : min(r + 1u, 47u)), w = other & 255u;
        const uint f0 = v > u ? v - u : u - v, f1 = w > u ? w - u : u - w;
        if (f1 < f0 || (f1 == f0 && w < v)) q = other;
    }
    return high ? (255u - (q & 255u)) | (q & 0xFF00u) | 0x100u : q;
}

/* the same for encoding range (AS_T6 to AS_Q3). Where range is a constant, as in every Direct function, this is three
   multiplies */
uint AsQuant(uint range, uint t) {
    return AsQuantKey(AsKey(range), t);
}

/* The 8 bits five trits share, as the decoder takes them apart (astc_decode.glsl's DecodeSequence, turned round). The
   low five bits hold t0 to t2: two bits each for t0 and t1 and one for t2 while t2 is under 2, and the pairs that a
   trit never makes (3) say otherwise. The top three hold t3 and t4 the same way, around them. A sequence's last group
   of fewer values has 0 for the trits it lacks, and those leave the bits it lacks 0 */
uint AsTritBits(uint t0, uint t1, uint t2, uint t3, uint t4) {
    const uint C = t2 == 2u ? (t1 == 2u ? 12u | t0 : 3u | (t0 << 2u) | (t1 << 4u)) : t0 | (t1 << 2u) | (t2 << 4u);
    return t4 == 2u ? (t3 == 2u ? (C & 3u) | 28u | ((C >> 2u) << 5u) : C | 0x60u | (t3 << 7u))
                    : C | (t3 << 5u) | (t4 << 7u);
}

/* the 7 bits three quints share, likewise: three bits for q0, two each for q1 and q2 while those are under 4 */
uint AsQuintBits(uint q0, uint q1, uint q2) {
    if (q0 == 4u && q1 == 4u) return q2 == 4u ? 7u : 6u | ((q2 & 1u) << 3u) | ((q2 >> 1u) << 4u);
    const uint C = q1 == 4u ? (q0 << 3u) | 5u : (q1 << 3u) | q0;
    return q2 == 4u ? (C & 1u) | 6u | ((C >> 3u) << 3u) | ((~(C >> 1u) & 3u) << 5u) : C | (q2 << 5u);
}
