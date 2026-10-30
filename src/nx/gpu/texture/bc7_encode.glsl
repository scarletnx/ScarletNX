/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* BC7 encoding: the body of bc7.comp, which declares the push constants (width, height, blocks_x, blocks_y), the
   buffers (rgba, bc7_blocks) and the workgroup. One invocation encodes one 4x4 block of the RGBA8 texels the ASTC
   decoder wrote, for a GPU that samples BC7 but not ASTC: a quarter of RGBA8's memory.

   A block is tried in the modes that suit it and the one with the least squared error is written. Every block:
   mode 6, one line through RGBA, 4-bit weights, what a block of one gradient holds closely. An opaque block (every
   alpha 255) also takes the modes whose alpha is 255 by construction: modes 1 and 3, two subsets of the format's
   64 partitions each with a line of its own (3-bit weights and 6-bit ends, or 2-bit and 7-bit), for an edge between
   two colours, and mode 2, three subsets, for three colours meeting. Any other block takes mode 7, two subsets with
   alpha. The partitions tried are the two of 64 (the one of 64 with three subsets) whose subsets spread least
   around their means, a pass each.

   A subset's line starts from its bounding box, each channel turned to run with the widest one (the sign of their
   covariance), pulled in by a sixteenth from each end and tried as it is too; then a least squares fit to the
   weights chosen, twice. The ends' p-bits are tried in each combination the mode allows, but a subset whose every
   alpha is 255 (or 0) keeps that alpha exactly. Each texel takes the best of the three weights around its
   projection. Integer arithmetic only, so the tests run this file on the CPU (tests/glsl/glsl.hpp) and get the
   GPU's bytes. */

/* BC7's partitions: two subsets (a bit a texel, the texel's subset), three subsets (two bits a texel); and the
   anchors, the texel of each subset after the first whose weight drops its top bit (two: bits 0 to 3; three: the
   second subset's bits 4 to 7, the third's 8 to 11). The Khronos Data Format Specification's BPTC tables */
const uint P2[64] = {
    0xCCCCu, 0x8888u, 0xEEEEu, 0xECC8u, 0xC880u, 0xFEECu, 0xFEC8u, 0xEC80u,
    0xC800u, 0xFFECu, 0xFE80u, 0xE800u, 0xFFE8u, 0xFF00u, 0xFFF0u, 0xF000u,
    0xF710u, 0x008Eu, 0x7100u, 0x08CEu, 0x008Cu, 0x7310u, 0x3100u, 0x8CCEu,
    0x088Cu, 0x3110u, 0x6666u, 0x366Cu, 0x17E8u, 0x0FF0u, 0x718Eu, 0x399Cu,
    0xAAAAu, 0xF0F0u, 0x5A5Au, 0x33CCu, 0x3C3Cu, 0x55AAu, 0x9696u, 0xA55Au,
    0x73CEu, 0x13C8u, 0x324Cu, 0x3BDCu, 0x6996u, 0xC33Cu, 0x9966u, 0x0660u,
    0x0272u, 0x04E4u, 0x4E40u, 0x2720u, 0xC936u, 0x936Cu, 0x39C6u, 0x639Cu,
    0x9336u, 0x9CC6u, 0x817Eu, 0xE718u, 0xCCF0u, 0x0FCCu, 0x7744u, 0xEE22u};
const uint P3[64] = {
    0xAA685050u, 0x6A5A5040u, 0x5A5A4200u, 0x5450A0A8u, 0xA5A50000u, 0xA0A05050u, 0x5555A0A0u, 0x5A5A5050u,
    0xAA550000u, 0xAA555500u, 0xAAAA5500u, 0x90909090u, 0x94949494u, 0xA4A4A4A4u, 0xA9A59450u, 0x2A0A4250u,
    0xA5945040u, 0x0A425054u, 0xA5A5A500u, 0x55A0A0A0u, 0xA8A85454u, 0x6A6A4040u, 0xA4A45000u, 0x1A1A0500u,
    0x0050A4A4u, 0xAAA59090u, 0x14696914u, 0x69691400u, 0xA08585A0u, 0xAA821414u, 0x50A4A450u, 0x6A5A0200u,
    0xA9A58000u, 0x5090A0A8u, 0xA8A09050u, 0x24242424u, 0x00AA5500u, 0x24924924u, 0x24499224u, 0x50A50A50u,
    0x500AA550u, 0xAAAA4444u, 0x66660000u, 0xA5A0A5A0u, 0x50A050A0u, 0x69286928u, 0x44AAAA44u, 0x66666600u,
    0xAA444444u, 0x54A854A8u, 0x95809580u, 0x96969600u, 0xA85454A8u, 0x80959580u, 0xAA141414u, 0x96960000u,
    0xAAAA1414u, 0xA05050A0u, 0xA0A5A5A0u, 0x96000000u, 0x40804080u, 0xA9A8A9A8u, 0xAAAAAA44u, 0x2A4A5254u};
const uint ANCHORS[64] = {
    0xF3Fu, 0x83Fu, 0x8FFu, 0x3FFu, 0xF8Fu, 0xF3Fu, 0x3FFu, 0x8FFu,
    0xF8Fu, 0xF8Fu, 0xF6Fu, 0xF6Fu, 0xF6Fu, 0xF5Fu, 0xF3Fu, 0x83Fu,
    0xF3Fu, 0x832u, 0xF88u, 0x3F2u, 0xF32u, 0x838u, 0xF68u, 0x8AFu,
    0x352u, 0xF88u, 0x682u, 0xA62u, 0xF88u, 0xF58u, 0xAF2u, 0x8F2u,
    0xF8Fu, 0x3FFu, 0xF36u, 0xA58u, 0xA62u, 0x8A8u, 0x98Fu, 0xAFFu,
    0x6F2u, 0xF38u, 0x8F2u, 0xF52u, 0x3F2u, 0x6FFu, 0x6FFu, 0x8F6u,
    0xF36u, 0x3F2u, 0xF56u, 0xF58u, 0xF5Fu, 0xF8Fu, 0xF52u, 0xFA2u,
    0xF5Fu, 0xFAFu, 0xF8Fu, 0xFDFu, 0x3FFu, 0xFC2u, 0xF32u, 0x83Fu};
/* the block's texels, RGBA8 words (R in the low byte), row by row */
uint px[16];

/* the shape being tried: its subsets, partition, bits an end's colour channel, whether alpha is stored (else 255),
   p-bits (0 none, 1 one shared by a subset's two ends, 2 one an end), index bits */
uint sh_ns;
uint sh_part;
uint sh_cb;
uint sh_alpha;
uint sh_pk;
uint sh_ib;

/* the weights the last Eval chose for its subset's texels, a nibble a texel (texels 0 to 7, then 8 to 15), nought
   for the other texels */
uint cur_lo;
uint cur_hi;

/* the best fit of the subset being fitted: its error, its ends as stored (each channel's bits, the p-bit the lowest),
   its weights */
uint fit_err;
uint fit_s0;
uint fit_s1;
uint fit_lo;
uint fit_hi;

/* the shape's subsets as fitted, and the best block so far: mode, partition, each subset's stored ends, weights */
uint cand_s0[3];
uint cand_s1[3];
uint best_mode;
uint best_part;
uint best_s0[3];
uint best_s1[3];
uint best_lo;
uint best_hi;
uint best_err;

uint Ch(uint word, uint c) {
    return (word >> (c * 8u)) & 0xFFu;
}

uint SubsetOf(uint k) {
    if (sh_ns == 1u) return 0u;
    if (sh_ns == 2u) return (P2[sh_part] >> k) & 1u;
    return (P3[sh_part] >> (k * 2u)) & 3u;
}

/* the anchor texel of subset s: texel 0 for the first */
uint AnchorOf(uint s) {
    if (s == 0u) return 0u;
    return (ANCHORS[sh_part] >> (sh_ns == 2u ? 0u : s * 4u)) & 15u;
}

bool IsAnchor(uint k) {
    if (k == 0u) return true;
    if (sh_ns >= 2u && k == AnchorOf(1u)) return true;
    return sh_ns == 3u && k == AnchorOf(2u);
}

/* the weight of index i: BC7's 2, 3 and 4-bit tables (0 21 43 64; 0 9 18 27 37 46 55 64; 0 4 9 13 ... 60 64) */
uint Weight(uint i) {
    const uint top = (1u << sh_ib) - 1u;
    return (i * 64u + top / 2u) / top;
}

uint Index(uint lo, uint hi, uint k) {
    return ((k < 8u ? lo : hi) >> ((k & 7u) * 4u)) & 15u;
}

/* bits of an end's channel as stored (with the p-bit), and alpha's */
uint StoredBits() {
    return sh_cb + (sh_pk != 0u ? 1u : 0u);
}

/* an n-bit stored channel as the decoder widens it to 8 bits: its top bits again below it */
uint Widen(uint v, uint n) {
    if (n == 8u) return v;
    return ((v << (8u - n)) | (v >> (2u * n - 8u))) & 0xFFu;
}

/* the stored ends as the decoder reads them, an RGBA8 word, alpha 255 where the shape stores none */
uint Decoded(uint stored) {
    const uint n = StoredBits();
    uint word = Widen(Ch(stored, 0u), n) | (Widen(Ch(stored, 1u), n) << 8u) | (Widen(Ch(stored, 2u), n) << 16u);
    return word | ((sh_alpha != 0u ? Widen(Ch(stored, 3u), n) : 255u) << 24u);
}

/* the stored value of n bits (its lowest the p-bit p when the shape has p-bits) that widens nearest to t */
uint Quant(int t, uint p) {
    const uint n = StoredBits();
    const uint top = (1u << n) - 1u;
    const int guess = (t * int(top) + 127) / 255;
    uint best = 0u;
    int best_d = 1000;
    for (int d = -2; d <= 2; d++) {
        const int c = guess + d;
        if (c < 0 || c > int(top)) continue;
        if (sh_pk != 0u && (uint(c) & 1u) != p) continue;
        const int e = int(Widen(uint(c), n)) - t;
        const int a = e < 0 ? -e : e;
        if (a < best_d) {
            best_d = a;
            best = uint(c);
        }
    }
    return best;
}

uint QuantWord(int r, int g, int b, int a, uint p) {
    return Quant(r, p) | (Quant(g, p) << 8u) | (Quant(b, p) << 16u) | (sh_alpha != 0u ? Quant(a, p) << 24u : 0u);
}

uint TexelError(uint p, uint q0, uint q1, uint w) {
    uint e = 0u;
    for (uint c = 0u; c < 4u; c++) {
        const int v = int(((64u - w) * Ch(q0, c) + w * Ch(q1, c) + 32u) >> 6u) - int(Ch(p, c));
        e += uint(v * v);
    }
    return e;
}

/* subset s's squared error with decoded ends q0 and q1, each of its texels at its best weight, which go to cur_lo
   and cur_hi */
uint Eval(uint s, uint q0, uint q1) {
    const int dr = int(Ch(q1, 0u)) - int(Ch(q0, 0u));
    const int dg = int(Ch(q1, 1u)) - int(Ch(q0, 1u));
    const int db = int(Ch(q1, 2u)) - int(Ch(q0, 2u));
    const int da = int(Ch(q1, 3u)) - int(Ch(q0, 3u));
    const int dd = dr * dr + dg * dg + db * db + da * da;
    const uint top = (1u << sh_ib) - 1u;
    uint err = 0u;
    cur_lo = 0u;
    cur_hi = 0u;
    for (uint k = 0u; k < 16u; k++) {
        if (SubsetOf(k) != s) continue;
        const uint p = px[k];
        uint i = 0u;
        if (dd > 0) {
            /* the texel's place along the line, 0 to 64, then the nearest weight */
            const int t = clamp((int(Ch(p, 0u)) - int(Ch(q0, 0u))) * dr + (int(Ch(p, 1u)) - int(Ch(q0, 1u))) * dg +
                                    (int(Ch(p, 2u)) - int(Ch(q0, 2u))) * db + (int(Ch(p, 3u)) - int(Ch(q0, 3u))) * da,
                                0, dd);
            const uint t64 = uint((t * 64 + dd / 2) / dd);
            i = min((t64 * top + 32u) / 64u, top);
        }
        uint bi = i;
        uint be = TexelError(p, q0, q1, Weight(i));
        if (i > 0u) {
            const uint e = TexelError(p, q0, q1, Weight(i - 1u));
            if (e < be) {
                be = e;
                bi = i - 1u;
            }
        }
        if (i < top) {
            const uint e = TexelError(p, q0, q1, Weight(i + 1u));
            if (e < be) {
                be = e;
                bi = i + 1u;
            }
        }
        err += be;
        if (k < 8u)
            cur_lo |= bi << (k * 4u);
        else
            cur_hi |= bi << ((k - 8u) * 4u);
    }
    return err;
}

/* the p-bits subset s's ends may take, a bit for each: 1 allows 0, 2 allows 1. A subset whose every alpha is 255
   keeps it only with the p-bit 1, one whose every alpha is 0 only with 0: a nearly opaque texel blends, and a nearly
   transparent one passes an alpha test of more than nought */
uint PAllowed(uint s) {
    if (sh_pk == 0u) return 1u;
    if (sh_alpha == 0u) return 3u;
    uint lo = 255u, hi = 0u;
    for (uint k = 0u; k < 16u; k++) {
        if (SubsetOf(k) != s) continue;
        lo = min(lo, Ch(px[k], 3u));
        hi = max(hi, Ch(px[k], 3u));
    }
    return lo == 255u ? 2u : hi == 0u ? 1u : 3u;
}

/* a candidate line for subset s, a0 to a3 one end and b0 to b3 the other (RGBA, 0 to 255): each p-bit choice the
   shape and the subset allow, the best kept in fit_ */
void Try(uint s, int a0, int a1, int a2, int a3, int b0, int b1, int b2, int b3) {
    const uint allowed = PAllowed(s);
    for (uint pp = 0u; pp < 4u; pp++) {
        const uint p0 = pp & 1u, p1 = sh_pk == 2u ? pp >> 1u : p0;
        if (sh_pk != 2u && (pp >> 1u) != 0u) continue;
        if (sh_pk == 0u && pp != 0u) continue;
        if ((allowed & (1u << p0)) == 0u || (allowed & (1u << p1)) == 0u) continue;
        const uint s0 = QuantWord(a0, a1, a2, a3, p0), s1 = QuantWord(b0, b1, b2, b3, p1);
        const uint err = Eval(s, Decoded(s0), Decoded(s1));
        if (err < fit_err) {
            fit_err = err;
            fit_s0 = s0;
            fit_s1 = s1;
            fit_lo = cur_lo;
            fit_hi = cur_hi;
        }
    }
}

/* the least squares fit of subset s's ends to the weights fit_ holds: sums of (64 - w)^2, (64 - w) w and w^2, a
   sixteenth of each so that the products in Solve stay under 2^31, and their determinant, nought when every texel
   took one weight */
int fit_a;
int fit_b;
int fit_d;
int Fit(uint s) {
    fit_a = 0;
    fit_b = 0;
    fit_d = 0;
    for (uint k = 0u; k < 16u; k++) {
        if (SubsetOf(k) != s) continue;
        const int w = int(Weight(Index(fit_lo, fit_hi, k)));
        fit_a += (64 - w) * (64 - w);
        fit_b += (64 - w) * w;
        fit_d += w * w;
    }
    fit_a >>= 4;
    fit_b >>= 4;
    fit_d >>= 4;
    return fit_a * fit_d - fit_b * fit_b;
}

/* channel c's end (side 0 or 1) of subset s's fit whose determinant det is positive, rounded and held to 0 to 255 */
int Solve(uint s, uint c, uint side, int det) {
    int x = 0, y = 0;
    for (uint k = 0u; k < 16u; k++) {
        if (SubsetOf(k) != s) continue;
        const int w = int(Weight(Index(fit_lo, fit_hi, k)));
        const int v = int(Ch(px[k], c));
        x += (64 - w) * v;
        y += w * v;
    }
    const int num = side == 0u ? fit_d * x - fit_b * y : fit_a * y - fit_b * x;
    const int den = det * 16;
    return clamp(num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den), 0, 255);
}

/* fits subset s of the shape: fit_err and the rest */
void FitSubset(uint s) {
    /* the bounding box, the channel the subset spans most, and each channel's sign against it */
    int lo0 = 255, lo1 = 255, lo2 = 255, lo3 = 255, hi0 = 0, hi1 = 0, hi2 = 0, hi3 = 0;
    int s0 = 0, s1 = 0, s2 = 0, s3 = 0, n = 0;
    for (uint k = 0u; k < 16u; k++) {
        if (SubsetOf(k) != s) continue;
        const uint p = px[k];
        lo0 = min(lo0, int(Ch(p, 0u)));
        lo1 = min(lo1, int(Ch(p, 1u)));
        lo2 = min(lo2, int(Ch(p, 2u)));
        lo3 = min(lo3, int(Ch(p, 3u)));
        hi0 = max(hi0, int(Ch(p, 0u)));
        hi1 = max(hi1, int(Ch(p, 1u)));
        hi2 = max(hi2, int(Ch(p, 2u)));
        hi3 = max(hi3, int(Ch(p, 3u)));
        s0 += int(Ch(p, 0u));
        s1 += int(Ch(p, 1u));
        s2 += int(Ch(p, 2u));
        s3 += int(Ch(p, 3u));
        n++;
    }
    uint wide = 0u;
    int span = hi0 - lo0;
    if (hi1 - lo1 > span) {
        wide = 1u;
        span = hi1 - lo1;
    }
    if (hi2 - lo2 > span) {
        wide = 2u;
        span = hi2 - lo2;
    }
    if (hi3 - lo3 > span) wide = 3u;
    const int sw = wide == 0u ? s0 : wide == 1u ? s1 : wide == 2u ? s2 : s3;
    /* covariances with the widest channel, n times each value against n times the mean: under 2^28 */
    int c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    for (uint k = 0u; k < 16u; k++) {
        if (SubsetOf(k) != s) continue;
        const uint p = px[k];
        const int vw = n * int(Ch(p, wide)) - sw;
        c0 += (n * int(Ch(p, 0u)) - s0) * vw;
        c1 += (n * int(Ch(p, 1u)) - s1) * vw;
        c2 += (n * int(Ch(p, 2u)) - s2) * vw;
        c3 += (n * int(Ch(p, 3u)) - s3) * vw;
    }
    const int a0 = c0 < 0 ? hi0 : lo0, b0 = c0 < 0 ? lo0 : hi0;
    const int a1 = c1 < 0 ? hi1 : lo1, b1 = c1 < 0 ? lo1 : hi1;
    const int a2 = c2 < 0 ? hi2 : lo2, b2 = c2 < 0 ? lo2 : hi2;
    const int a3 = c3 < 0 ? hi3 : lo3, b3 = c3 < 0 ? lo3 : hi3;
    /* a sixteenth in from each end: the box's corners lie past the texels on any line not along an axis */
    const int i0 = (b0 - a0) / 16, i1 = (b1 - a1) / 16, i2 = (b2 - a2) / 16, i3 = (b3 - a3) / 16;
    fit_err = 0xFFFFFFFFu;
    Try(s, a0 + i0, a1 + i1, a2 + i2, a3 + i3, b0 - i0, b1 - i1, b2 - i2, b3 - i3);
    Try(s, a0, a1, a2, a3, b0, b1, b2, b3);
    for (uint r = 0u; r < 2u && fit_err > 0u; r++) {
        const int det = Fit(s);
        if (det <= 0) break;
        Try(s, Solve(s, 0u, 0u, det), Solve(s, 1u, 0u, det), Solve(s, 2u, 0u, det), Solve(s, 3u, 0u, det),
            Solve(s, 0u, 1u, det), Solve(s, 1u, 1u, det), Solve(s, 2u, 1u, det), Solve(s, 3u, 1u, det));
    }
}

/* fits every subset of a shape (mode, subsets, partition, colour bits, alpha stored, p-bits, index bits) and keeps
   it as best_ when its error is the least so far; it stops at a subset whose error already passes the best */
void TryShape(uint mode, uint ns, uint part, uint cb, uint alpha, uint pk, uint ib) {
    sh_ns = ns;
    sh_part = part;
    sh_cb = cb;
    sh_alpha = alpha;
    sh_pk = pk;
    sh_ib = ib;
    uint err = 0u, lo = 0u, hi = 0u;
    for (uint s = 0u; s < ns; s++) {
        FitSubset(s);
        err += fit_err;
        if (err >= best_err) return;
        cand_s0[s] = fit_s0;
        cand_s1[s] = fit_s1;
        lo |= fit_lo;
        hi |= fit_hi;
    }
    best_err = err;
    best_mode = mode;
    best_part = part;
    best_lo = lo;
    best_hi = hi;
    for (uint s = 0u; s < 3u; s++) {
        best_s0[s] = cand_s0[s];
        best_s1[s] = cand_s1[s];
    }
}

/* how far partition part's subsets spread around their means: each subset's sum of squared distances, from its
   sums (n Q - S^2 for each channel, over n), alpha included when alpha says so */
uint Spread(uint ns, uint part, uint alpha) {
    sh_ns = ns;
    sh_part = part;
    uint total = 0u;
    for (uint s = 0u; s < ns; s++) {
        int n = 0, q = 0, r = 0, g = 0, b = 0, a = 0;
        for (uint k = 0u; k < 16u; k++) {
            if (SubsetOf(k) != s) continue;
            const uint p = px[k];
            const int vr = int(Ch(p, 0u)), vg = int(Ch(p, 1u)), vb = int(Ch(p, 2u));
            const int va = alpha != 0u ? int(Ch(p, 3u)) : 0;
            n++;
            q += vr * vr + vg * vg + vb * vb + va * va;
            r += vr;
            g += vg;
            b += vb;
            a += va;
        }
        if (n > 0) total += uint((n * q - r * r - g * g - b * b - a * a) / n);
    }
    return total;
}

/* the block's error with every alpha 255 by construction, for the modes that store none */
bool opaque;

/* writes count bits of value at bit pos of the block */
uvec4 blk;
void Put(uint pos, uint value, uint count) {
    const uint w = pos >> 5u, b = pos & 31u;
    blk[w] |= value << b;
    if (b + count > 32u) blk[w + 1u] |= value >> (32u - b);
}

uvec4 Pack() {
    const uint ns = sh_ns, ib = sh_ib, top = (1u << ib) - 1u;
    /* each subset's anchor weight has its top bit nought: where it is set, the subset's ends swap and its weights
       turn round */
    uint lo = best_lo, hi = best_hi;
    for (uint s = 0u; s < ns; s++) {
        const uint anchor = AnchorOf(s);
        if (Index(lo, hi, anchor) <= top / 2u) continue;
        const uint t = best_s0[s];
        best_s0[s] = best_s1[s];
        best_s1[s] = t;
        for (uint k = 0u; k < 16u; k++) {
            if (SubsetOf(k) != s) continue;
            const uint turned = top - Index(lo, hi, k);
            if (k < 8u)
                lo = (lo & ~(15u << (k * 4u))) | (turned << (k * 4u));
            else
                hi = (hi & ~(15u << ((k - 8u) * 4u))) | (turned << ((k - 8u) * 4u));
        }
    }
    blk = uvec4(0u);
    const uint mode = best_mode;
    /* the mode: as many noughts as its number, then a one */
    Put(mode, 1u, 1u);
    uint pos = mode + 1u;
    if (mode != 6u) {
        Put(pos, best_part, 6u);
        pos += 6u;
    }
    /* the ends' channels: R of every end, then G, B and, where stored, A; each end's p-bit below its bits */
    const uint pb = sh_pk != 0u ? 1u : 0u;
    const uint channels = sh_alpha != 0u ? 4u : 3u;
    for (uint c = 0u; c < channels; c++)
        for (uint e = 0u; e < ns * 2u; e++) {
            const uint stored = Ch((e & 1u) == 0u ? best_s0[e >> 1u] : best_s1[e >> 1u], c);
            Put(pos, stored >> pb, sh_cb);
            pos += sh_cb;
        }
    /* the p-bits: one an end (modes 3, 6 and 7), or one a subset (mode 1) */
    if (sh_pk == 2u)
        for (uint e = 0u; e < ns * 2u; e++) {
            Put(pos, ((e & 1u) == 0u ? best_s0[e >> 1u] : best_s1[e >> 1u]) & 1u, 1u);
            pos++;
        }
    if (sh_pk == 1u)
        for (uint s = 0u; s < ns; s++) {
            Put(pos, best_s0[s] & 1u, 1u);
            pos++;
        }
    /* the weights, texel by texel, an anchor's a bit shorter */
    for (uint k = 0u; k < 16u; k++) {
        const uint bits = IsAnchor(k) ? ib - 1u : ib;
        Put(pos, Index(lo, hi, k), bits);
        pos += bits;
    }
    return blk;
}

uvec4 Encode() {
    opaque = true;
    for (uint k = 0u; k < 16u; k++) opaque = opaque && Ch(px[k], 3u) == 255u;
    best_err = 0xFFFFFFFFu;
    /* mode 6: one subset, RGBA, 7 bits and a p-bit an end, 4-bit weights */
    TryShape(6u, 1u, 0u, 7u, 1u, 2u, 4u);
    if (best_err > 0u) {
        /* the two partitions of two subsets that spread least, and the one of three */
        const uint alpha = opaque ? 0u : 1u;
        uint first = 0u, second = 1u, f_cost = 0xFFFFFFFFu, s_cost = 0xFFFFFFFFu;
        for (uint p = 0u; p < 64u; p++) {
            const uint c = Spread(2u, p, alpha);
            if (c < f_cost) {
                second = first;
                s_cost = f_cost;
                first = p;
                f_cost = c;
            } else if (c < s_cost) {
                second = p;
                s_cost = c;
            }
        }
        if (opaque) {
            /* mode 1: two subsets, RGB, 6 bits an end and a p-bit a subset, 3-bit weights; mode 3: 7 bits and a
               p-bit an end, 2-bit weights */
            TryShape(1u, 2u, first, 6u, 0u, 1u, 3u);
            TryShape(1u, 2u, second, 6u, 0u, 1u, 3u);
            TryShape(3u, 2u, first, 7u, 0u, 2u, 2u);
            TryShape(3u, 2u, second, 7u, 0u, 2u, 2u);
            uint third = 0u, t_cost = 0xFFFFFFFFu;
            for (uint p = 0u; p < 64u; p++) {
                const uint c = Spread(3u, p, 0u);
                if (c < t_cost) {
                    third = p;
                    t_cost = c;
                }
            }
            /* mode 2: three subsets, RGB, 5 bits an end, 2-bit weights */
            TryShape(2u, 3u, third, 5u, 0u, 0u, 2u);
        } else {
            /* mode 7: two subsets, RGBA, 5 bits and a p-bit an end, 2-bit weights */
            TryShape(7u, 2u, first, 5u, 1u, 2u, 2u);
            TryShape(7u, 2u, second, 5u, 1u, 2u, 2u);
        }
    }
    /* the best's shape again, for Pack */
    sh_ns = best_mode == 6u ? 1u : best_mode == 2u ? 3u : 2u;
    sh_part = best_part;
    sh_cb = best_mode == 6u ? 7u : best_mode == 1u ? 6u : best_mode == 3u ? 7u : 5u;
    sh_alpha = best_mode == 6u || best_mode == 7u ? 1u : 0u;
    sh_pk = best_mode == 2u ? 0u : best_mode == 1u ? 1u : 2u;
    sh_ib = best_mode == 6u ? 4u : best_mode == 1u ? 3u : 2u;
    return Pack();
}

void main() {
    const uint bx = gl_GlobalInvocationID.x, by = gl_GlobalInvocationID.y, z = gl_WorkGroupID.z;
    if (bx >= blocks_x || by >= blocks_y) return;
    /* a block past the image's edge repeats its last texels, which adds no colour to the line */
    for (uint k = 0u; k < 16u; k++) {
        const uint x = min(bx * 4u + (k & 3u), width - 1u), y = min(by * 4u + (k >> 2u), height - 1u);
        px[k] = rgba[(z * height + y) * width + x];
    }
    bc7_blocks[(z * blocks_y + by) * blocks_x + bx] = Encode();
}
