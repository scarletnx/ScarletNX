/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BCn blocks texconv.glsl's ASTC build decodes in its second pass, decoded with no arrays: the BC7 blocks whose
   own lines the ASTC encoder takes (modes 1 and 3, two subsets; modes 4 and 5, a colour line and an alpha line), into
   the encoder's texels with what the block says of itself (astc_encode.glsl's as_hint). The same texels and the same
   hints as texconv.glsl's bc7_block, which reads any mode from tables of the modes' fields into arrays of ends; here
   each mode's fields are cut at bits that are constants, an end is the word of its channels, a stream of indices is
   two words with its anchors' missing bits put in (so texel k's index lies at k times its bits), and a texel is two
   multiplications (astc_direct.glsl's DxLerp). Included after astc_direct.glsl, whose Fields45 it shares.

   The two tables a block of two subsets needs, its partition and its second anchor, are read with selects (mux.glsl),
   once a block. */

/* 64 bits (x the low word): n < 32 bits from bit at, which end under bit 64 */
uint RgCut(uvec2 v, uint at, uint n) {
    return at < 32u ? DxCut(v.x, v.y, at, n) : (v.y >> (at - 32u)) & ((1u << n) - 1u);
}
/* 64 bits with a nought put in at bit pos: the bits from there up move one up */
uvec2 RgOpen(uvec2 v, uint pos) {
    const uint m = (1u << (pos & 31u)) - 1u;
    if (pos < 32u) return uvec2((v.x & m) | ((v.x & ~m) << 1u), (v.y << 1u) | (v.x >> 31u));
    return uvec2(v.x, (v.y & m) | ((v.y & ~m) << 1u));
}

/* the block being decoded: each subset's ends as they decode (RGBA8 words; b the second subset's), a bit a texel for
   its subset, the colour's indices of rg_cb bits and, where alpha has its own (rg_ab not 0), alpha's; the rotation */
uint rg_e0a;
uint rg_e1a;
uint rg_e0b;
uint rg_e1b;
uint rg_mask;
uvec2 rg_ci;
uint rg_cb;
uvec2 rg_ai;
uint rg_ab;
uint rg_rot;

/* BC7's weight of an index of 2 or 3 bits (bc7_encode.glsl's Weight has the identity) */
uint RgWeight(uint i, uint bits) {
    return (i * (bits == 2u ? 1365u : 585u) + 32u) >> 6u;
}

/* texel k of the block rg_ holds */
uint RgTexel(uint k) {
    const bool second = ((rg_mask >> k) & 1u) != 0u;
    const uint e0 = second ? rg_e0b : rg_e0a, e1 = second ? rg_e1b : rg_e1a;
    uint c = DxLerp(e0, e1, RgWeight(RgCut(rg_ci, k * rg_cb, rg_cb), rg_cb));
    if (rg_ab != 0u) {
        const uint w = RgWeight(RgCut(rg_ai, k * rg_ab, rg_ab), rg_ab);
        c = (c & 0xFFFFFFu) | ((((e0 >> 24u) * (64u - w) + (e1 >> 24u) * w + 32u) >> 6u) << 24u);
    }
    if (rg_rot != 0u) { /* the fourth channel's values swapped with red's, green's or blue's */
        const uint sh = (rg_rot - 1u) * 8u;
        c = (c & ~(0xFF000000u | (0xFFu << sh))) | ((c >> 24u) << sh) | (((c >> sh) & 0xFFu) << 24u);
    }
    return c;
}

/* the encoder's 16 texels from F, a function of the texel's number */
#undef RG_FILL
#if SNX_NO_ARRAYS
#define RG_FILL(F) \
    px_0 = F(0u), px_1 = F(1u), px_2 = F(2u), px_3 = F(3u), px_4 = F(4u), px_5 = F(5u), px_6 = F(6u), px_7 = F(7u), \
    px_8 = F(8u), px_9 = F(9u), px_10 = F(10u), px_11 = F(11u), px_12 = F(12u), px_13 = F(13u), px_14 = F(14u), \
    px_15 = F(15u)
#else
#define RG_FILL(F) \
    for (uint rg_k = 0u; rg_k < 16u; rg_k++) PX_SET(rg_k, F(rg_k))
#endif

/* A BC7 block of mode 1, 3, 4 or 5 (its four words) into the encoder's texels, with its lines as the encoder's hints:
   0 for a block of another mode, which has none */
bool Rg7(uint b0, uint b1, uint b2, uint b3) {
    rg_mask = 0u;
    rg_ab = 0u;
    rg_rot = 0u;
    rg_ai = uvec2(0u);
    if (Fields45(b0, b1, b2, b3)) {
        /* the colour's line and alpha's, each with a stream: the 2 bit one is the colour's but for mode 4 with its
           selection bit, where the colour has the 3 bit one */
        const uvec2 two = uvec2(f45_w2, 0u), other = f45_m5 ? uvec2(f45_x2, 0u) : uvec2(f45_t3lo, f45_t3hi);
        rg_e0a = f45_e0 | (f45_a0 << 24u);
        rg_e1a = f45_e1 | (f45_a1 << 24u);
        rg_ci = f45_three ? other : two;
        rg_cb = f45_three ? 3u : 2u;
        rg_ai = f45_three ? two : other;
        rg_ab = f45_m5 || f45_three ? 2u : 3u;
        rg_rot = f45_rot;
        /* the ends as the texels have them, the fourth channel's swapped with the rotation's */
        uint h0 = rg_e0a, h1 = rg_e1a;
        if (rg_rot != 0u) {
            const uint sh = (rg_rot - 1u) * 8u, keep = ~(0xFF000000u | (0xFFu << sh));
            h0 = (h0 & keep) | ((h0 >> 24u) << sh) | (((h0 >> sh) & 0xFFu) << 24u);
            h1 = (h1 & keep) | ((h1 >> 24u) << sh) | (((h1 >> sh) & 0xFFu) << 24u);
        }
        as_h0a = h0;
        as_h1a = h1;
        as_hint = 2u;
        as_hpart = 0u;
        as_hplane = rg_rot == 0u ? 3u : rg_rot - 1u;
        as_hib = rg_cb;
    } else {
        const bool m1 = (b0 & 3u) == 2u;
        if (!m1 && (b0 & 15u) != 8u) return false;
        uint part, k0, k1, k2, k3;
        if (m1) {
            /* mode 1: a partition of 6 bits from bit 2; four ends of 6 bits a channel, red of all four from bit 8,
               green from 32 and blue from 56; a p-bit a subset at bits 80 and 81; indices of 3 bits from bit 82 */
            part = (b0 >> 2u) & 63u;
            k0 = ((b0 >> 8u) & 63u) | ((b1 & 63u) << 8u) | (((b1 >> 24u) & 63u) << 16u);
            k1 = ((b0 >> 14u) & 63u) | (((b1 >> 6u) & 63u) << 8u) | (DxCut(b1, b2, 30u, 6u) << 16u);
            k2 = ((b0 >> 20u) & 63u) | (((b1 >> 12u) & 63u) << 8u) | (((b2 >> 4u) & 63u) << 16u);
            k3 = (b0 >> 26u) | (((b1 >> 18u) & 63u) << 8u) | (((b2 >> 10u) & 63u) << 16u);
            /* 7 bits with the subset's p-bit, widened to 8 by the top one */
            const uint p0 = ((b2 >> 16u) & 1u) * 0x010101u, p1 = ((b2 >> 17u) & 1u) * 0x010101u;
            k0 = (k0 << 1u) | p0;
            k1 = (k1 << 1u) | p0;
            k2 = (k2 << 1u) | p1;
            k3 = (k3 << 1u) | p1;
            k0 = (k0 << 1u) | ((k0 >> 6u) & 0x010101u);
            k1 = (k1 << 1u) | ((k1 >> 6u) & 0x010101u);
            k2 = (k2 << 1u) | ((k2 >> 6u) & 0x010101u);
            k3 = (k3 << 1u) | ((k3 >> 6u) & 0x010101u);
            rg_ci = uvec2((b2 >> 18u) | (b3 << 14u), b3 >> 18u);
            rg_cb = 3u;
        } else {
            /* mode 3: a partition of 6 bits from bit 4; four ends of 7 bits a channel, red from bit 10, green from 38
               and blue from 66; a p-bit an end from bit 94; indices of 2 bits from bit 98 */
            part = (b0 >> 4u) & 63u;
            k0 = ((b0 >> 10u) & 127u) | (((b1 >> 6u) & 127u) << 8u) | (((b2 >> 2u) & 127u) << 16u);
            k1 = ((b0 >> 17u) & 127u) | (((b1 >> 13u) & 127u) << 8u) | (((b2 >> 9u) & 127u) << 16u);
            k2 = ((b0 >> 24u) & 127u) | (((b1 >> 20u) & 127u) << 8u) | (((b2 >> 16u) & 127u) << 16u);
            k3 = DxCut(b0, b1, 31u, 7u) | (DxCut(b1, b2, 27u, 7u) << 8u) | (((b2 >> 23u) & 127u) << 16u);
            const uint p = DxCut(b2, b3, 30u, 4u);
            k0 = (k0 << 1u) | ((p & 1u) * 0x010101u);
            k1 = (k1 << 1u) | (((p >> 1u) & 1u) * 0x010101u);
            k2 = (k2 << 1u) | (((p >> 2u) & 1u) * 0x010101u);
            k3 = (k3 << 1u) | ((p >> 3u) * 0x010101u);
            rg_ci = uvec2(b3 >> 2u, 0u);
            rg_cb = 2u;
        }
        rg_e0a = k0 | 0xFF000000u;
        rg_e1a = k1 | 0xFF000000u;
        rg_e0b = k2 | 0xFF000000u;
        rg_e1b = k3 | 0xFF000000u;
        /* the partition's bit a texel, two partitions a word, and the second subset's anchor, a nibble a partition */
        rg_mask = (Mux32(part >> 1u, BC_PART2_WORDS) >> ((part & 1u) * 16u)) & 0xFFFFu;
        const uint anchor = (Mux8(part >> 3u, BC_ANCHOR2_WORDS) >> ((part & 7u) * 4u)) & 15u;
        /* texel 0's index and the anchor's are a bit short: the first's top bit put in, then the anchor's, which the
           first has moved one up */
        rg_ci = RgOpen(RgOpen(rg_ci, rg_cb - 1u), anchor * rg_cb + rg_cb - 1u);
        as_h0a = rg_e0a;
        as_h1a = rg_e1a;
        as_h0b = rg_e0b;
        as_h1b = rg_e1b;
        as_hint = 3u;
        as_hpart = part;
        as_hplane = 3u;
        as_hib = rg_cb;
    }
    RG_FILL(RgTexel);
    return true;
}
