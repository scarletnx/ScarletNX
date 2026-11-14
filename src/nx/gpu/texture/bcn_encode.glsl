/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* BC1, BC4 and BC5 encoding of the 16 RGBA8 texels in px (R in the low byte, row by row): astc_bcn.comp includes it
   after astc_decode.glsl, and astc_bcn.glsl fills px for each 4x4 block from the ASTC blocks under it, for an ASTC
   texture whose blocks hold no more than these formats do (gpu/texture/astc_kind.h): luminance alone as BC4,
   luminance and alpha as BC5, opaque colour on one line a block as BC1. Integer arithmetic only, so the tests run this
   file on the CPU (tests/glsl/glsl.hpp) and get the GPU's bytes.

   BC4 (one channel): the block's least and greatest values as the ends, eight values between them, and the same pulled
   in by a sixteenth from each end; then a least squares fit to the values the texels took. Each texel takes the
   nearest value. BC1 (opaque, four colours, the first end the greater): the line through the block's bounding box,
   each channel turned to run with the widest one (the sign of their covariance), as it is and pulled in by a
   sixteenth; then a least squares fit to the colours chosen. A block of one colour takes the pair of ends whose
   colour a third of the way between comes nearest it, channel by channel. The candidate with the least squared error
   is written. The palette is the specification's (Khronos Data Format, S3TC and RGTC): BC1's two colours between at a
   third and two thirds, BC4's six at sevenths. */

/* the block's texels, RGBA8 words (R in the low byte), row by row */
uint px[16];

/* ---- BC4: one channel, shift's byte of each texel */
/* the best ends tried, their index bits (three a texel, texel 0 lowest: bits 0 to 31, then 32 to 47) and squared
   error, 49 times (in sevenths) */
uint bc4_a0;
uint bc4_a1;
uint bc4_lo;
uint bc4_hi;
uint bc4_err;
/* the value of texel k at the ends a0 > a1: 0 is a0, 7 is a1, eight in all */
uint Bc4Step(uint v, uint a0, uint a1) {
    const uint d = a0 - a1;
    return v >= a0 ? 0u : v <= a1 ? 7u : min((14u * (a0 - v) + d) / (2u * d), 7u);
}
void Bc4Try(uint shift, uint a0, uint a1) {
    uint lo = 0u, hi = 0u, err = 0u;
    for (uint k = 0u; k < 16u; k++) {
        const uint v = (px[k] >> shift) & 255u, q = Bc4Step(v, a0, a1);
        const int e = int(7u * v) - int(a0 * (7u - q) + a1 * q);
        err += uint(e * e);
        /* the format's index of step q: 0 and 1 the ends, 2 to 7 the six between from a0's side */
        const uint idx = q == 0u ? 0u : q == 7u ? 1u : q + 1u, at = 3u * k;
        if (at < 32u) lo |= idx << at;
        if (at + 3u > 32u) hi |= at >= 32u ? idx << (at - 32u) : idx >> (32u - at);
    }
    if (err < bc4_err) {
        bc4_err = err;
        bc4_a0 = a0;
        bc4_a1 = a1;
        bc4_lo = lo;
        bc4_hi = hi;
    }
}
/* ends fitted by least squares to the steps the best ends gave the texels: a0 and a1 weighed 7 - q and q */
void Bc4Fit(uint shift) {
    int a = 0, b = 0, c = 0, x = 0, y = 0;
    for (uint k = 0u; k < 16u; k++) {
        const uint v = (px[k] >> shift) & 255u, q = Bc4Step(v, bc4_a0, bc4_a1);
        const int al = int(7u - q), be = int(q);
        a += al * al;
        b += al * be;
        c += be * be;
        x += al * int(v);
        y += be * int(v);
    }
    const int det = a * c - b * b, n0 = c * x - b * y, n1 = a * y - b * x;
    if (det <= 0) return;
    const uint a0 = n0 <= 0 ? 0u : min((7u * uint(n0) + uint(det) / 2u) / uint(det), 255u);
    const uint a1 = n1 <= 0 ? 0u : min((7u * uint(n1) + uint(det) / 2u) / uint(det), 255u);
    if (a0 > a1) Bc4Try(shift, a0, a1);
}
/* the BC4 block of shift's channel: two words, little-endian */
uvec2 Bc4Block(uint shift) {
    uint mn = 255u, mx = 0u;
    for (uint k = 0u; k < 16u; k++) {
        const uint v = (px[k] >> shift) & 255u;
        mn = min(mn, v);
        mx = max(mx, v);
    }
    /* one value: both ends it, every index 0 */
    if (mn == mx) return uvec2(mn | (mn << 8u), 0u);
    bc4_err = 0xFFFFFFFFu;
    Bc4Try(shift, mx, mn);
    const uint in16 = (mx - mn) >> 4u;
    if (in16 != 0u) Bc4Try(shift, mx - in16, mn + in16);
    Bc4Fit(shift);
    return uvec2(bc4_a0 | (bc4_a1 << 8u) | (bc4_lo << 16u), (bc4_lo >> 16u) | (bc4_hi << 16u));
}

/* ---- BC1: R, G and B, opaque */
/* channel c (0 R, 1 G, 2 B) of an RGB8 word */
uint Ch8(uint w, uint c) { return (w >> (8u * c)) & 255u; }
/* an 8 bit channel to 5 or 6 bits (nearest), and back as the decoder widens it */
uint To565(uint v, uint bits) { return bits == 6u ? (v * 63u + 127u) / 255u : (v * 31u + 127u) / 255u; }
uint From565(uint q, uint bits) { return bits == 6u ? (q << 2u) | (q >> 4u) : (q << 3u) | (q >> 2u); }
uint Pack565(uint e) { return (To565(Ch8(e, 0u), 5u) << 11u) | (To565(Ch8(e, 1u), 6u) << 5u) | To565(Ch8(e, 2u), 5u); }
/* a 565 colour widened to RGB8 */
uint Wide565(uint c) {
    return From565(c >> 11u, 5u) | (From565((c >> 5u) & 63u, 6u) << 8u) | (From565(c & 31u, 5u) << 16u);
}
/* the best block tried: its ends (c0 | c1 << 16), indices and squared error */
uint bc1_c;
uint bc1_idx;
uint bc1_err;
/* the block with 565 ends c0 and c1 as they come (c0 > c1: four colours; equal: every index 0) */
void Bc1Try565(uint c0, uint c1) {
    if (c0 < c1) {
        const uint t = c0;
        c0 = c1;
        c1 = t;
    }
    const uint p0 = Wide565(c0), p1 = Wide565(c1);
    uint idx = 0u, err = 0u;
    for (uint k = 0u; k < 16u; k++) {
        uint best = 0xFFFFFFFFu, bi = 0u;
        for (uint i = 0u; i < (c0 == c1 ? 1u : 4u); i++) {
            /* the palette: 0 and 1 the ends, 2 and 3 a third and two thirds of the way from c0 */
            uint e = 0u;
            for (uint c = 0u; c < 3u; c++) {
                const uint a = Ch8(p0, c), b = Ch8(p1, c);
                const uint p = i == 0u ? 3u * a : i == 1u ? 3u * b : i == 2u ? 2u * a + b : a + 2u * b;
                const int d = int(3u * Ch8(px[k], c)) - int(p);
                e += uint(d * d);
            }
            if (e < best) {
                best = e;
                bi = i;
            }
        }
        err += best;
        idx |= bi << (2u * k);
    }
    if (err < bc1_err) {
        bc1_err = err;
        bc1_c = c0 | (c1 << 16u);
        bc1_idx = idx;
    }
}
void Bc1Try(uint e0, uint e1) { Bc1Try565(Pack565(e0), Pack565(e1)); }
/* ends fitted by least squares to the colours the best block gave the texels: c0 and c1 weighed 3 - t and t, t the
   thirds from c0 of the colour chosen */
void Bc1Fit() {
    int a = 0, b = 0, c = 0, x0 = 0, x1 = 0, x2 = 0, y0 = 0, y1 = 0, y2 = 0;
    const uint c0 = bc1_c & 0xFFFFu, c1 = bc1_c >> 16u;
    if (c0 == c1) return;
    for (uint k = 0u; k < 16u; k++) {
        const uint i = (bc1_idx >> (2u * k)) & 3u, t = i == 0u ? 0u : i == 1u ? 3u : i == 2u ? 1u : 2u;
        const int al = int(3u - t), be = int(t);
        a += al * al;
        b += al * be;
        c += be * be;
        x0 += al * int(Ch8(px[k], 0u));
        x1 += al * int(Ch8(px[k], 1u));
        x2 += al * int(Ch8(px[k], 2u));
        y0 += be * int(Ch8(px[k], 0u));
        y1 += be * int(Ch8(px[k], 1u));
        y2 += be * int(Ch8(px[k], 2u));
    }
    const int det = a * c - b * b;
    if (det <= 0) return;
    uint e0 = 0u, e1 = 0u;
    for (uint ch = 0u; ch < 3u; ch++) {
        const int x = ch == 0u ? x0 : ch == 1u ? x1 : x2, y = ch == 0u ? y0 : ch == 1u ? y1 : y2;
        const int n0 = c * x - b * y, n1 = a * y - b * x;
        e0 |= (n0 <= 0 ? 0u : min((3u * uint(n0) + uint(det) / 2u) / uint(det), 255u)) << (8u * ch);
        e1 |= (n1 <= 0 ? 0u : min((3u * uint(n1) + uint(det) / 2u) / uint(det), 255u)) << (8u * ch);
    }
    Bc1Try(e0, e1);
}
/* a block of one colour v: per channel, the 565 ends whose colour a third of the way from the first (index 2) is
   nearest v, which 565 alone holds only to 4 levels in 256 */
uint Bc1One(uint v, uint bits) {
    const uint top = (1u << bits) - 1u;
    uint best = 0xFFFFFFFFu, pair = 0u;
    for (uint a = 0u; a <= top; a++) {
        const int want = int(3u * v) - int(2u * From565(a, bits));
        const uint b0 = want <= 0 ? 0u : min(To565(min(uint(want), 255u), bits), top);
        for (uint b = b0 > 0u ? b0 - 1u : 0u; b <= min(b0 + 1u, top); b++) {
            const int d = int(2u * From565(a, bits) + From565(b, bits)) - int(3u * v);
            if (uint(d * d) < best) {
                best = uint(d * d);
                pair = a | (b << 8u);
            }
        }
    }
    return pair;
}
/* the BC1 block of the texels' R, G and B: two words, little-endian */
uvec2 Bc1Block() {
    uint lo = 0xFFFFFFu, hi = 0u;
    int s0 = 0, s1 = 0, s2 = 0;
    for (uint k = 0u; k < 16u; k++) {
        for (uint c = 0u; c < 3u; c++) {
            const uint v = Ch8(px[k], c);
            lo = (lo & ~(255u << (8u * c))) | (min(Ch8(lo, c), v) << (8u * c));
            hi = (hi & ~(255u << (8u * c))) | (max(Ch8(hi, c), v) << (8u * c));
        }
        s0 += int(Ch8(px[k], 0u));
        s1 += int(Ch8(px[k], 1u));
        s2 += int(Ch8(px[k], 2u));
    }
    bc1_err = 0xFFFFFFFFu;
    if (lo == hi) {
        /* one colour: index 2 of each channel's pair (Bc1Try565 orders the ends, and 2 becomes 3 where it swaps) */
        const uint r = Bc1One(Ch8(lo, 0u), 5u), g = Bc1One(Ch8(lo, 1u), 6u), b = Bc1One(Ch8(lo, 2u), 5u);
        const uint c0 = ((r & 255u) << 11u) | ((g & 255u) << 5u) | (b & 255u), c1 = ((r >> 8u) << 11u) |
                                                                                 ((g >> 8u) << 5u) | (b >> 8u);
        Bc1Try565(c0, c1);
        Bc1Try(lo, lo);
        return uvec2(bc1_c, bc1_idx);
    }
    /* the widest channel, and each channel's direction against it: the sign of their covariance, 16 times */
    const uint w0 = Ch8(hi, 0u) - Ch8(lo, 0u), w1 = Ch8(hi, 1u) - Ch8(lo, 1u), w2 = Ch8(hi, 2u) - Ch8(lo, 2u);
    const uint m = w1 >= w0 && w1 >= w2 ? 1u : w0 >= w2 ? 0u : 2u;
    const int sm = m == 0u ? s0 : m == 1u ? s1 : s2;
    int cov0 = 0, cov1 = 0, cov2 = 0;
    for (uint k = 0u; k < 16u; k++) {
        const int vm = 16 * int(Ch8(px[k], m)) - sm;
        cov0 += vm * (16 * int(Ch8(px[k], 0u)) - s0);
        cov1 += vm * (16 * int(Ch8(px[k], 1u)) - s1);
        cov2 += vm * (16 * int(Ch8(px[k], 2u)) - s2);
    }
    /* the box's corners on the line: a channel that falls as the widest rises swaps its ends */
    uint e0 = 0u, e1 = 0u, f0 = 0u, f1 = 0u;
    for (uint c = 0u; c < 3u; c++) {
        const int cov = c == 0u ? cov0 : c == 1u ? cov1 : cov2;
        const uint l = Ch8(lo, c), h = Ch8(hi, c), in16 = (h - l) >> 4u;
        e0 |= (cov < 0 ? l : h) << (8u * c);
        e1 |= (cov < 0 ? h : l) << (8u * c);
        f0 |= (cov < 0 ? l + in16 : h - in16) << (8u * c);
        f1 |= (cov < 0 ? h - in16 : l + in16) << (8u * c);
    }
    Bc1Try(e0, e1);
    Bc1Try(f0, f1);
    Bc1Fit();
    return uvec2(bc1_c, bc1_idx);
}
