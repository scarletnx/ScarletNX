/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* BC7 encoding, mode 6 only: the body of bc7.comp, which declares the push constants (width, height, blocks_x,
   blocks_y), the buffers (rgba, bc7_blocks) and the workgroup. One invocation encodes one 4x4 block of the RGBA8
   texels the ASTC decoder wrote, for a GPU that samples BC7 but not ASTC: a quarter of RGBA8's memory.

   Mode 6 is one line through RGBA per block, endpoints of seven bits a channel and a bit of their own below them,
   and a 4-bit weight a texel: what a block of one gradient, the usual case, holds closely. The line starts from the
   block's bounding box, each channel turned to run with the widest one (the sign of their covariance) and pulled in
   by a sixteenth from each end; then a least squares fit to the weights chosen, twice. Each candidate's four p-bit
   pairs are tried, every texel takes the nearest weight of the three around its projection, and a candidate is kept
   only when the block's squared error falls. Integer arithmetic only, so the tests run this file on the CPU
   (tests/glsl/glsl.hpp) and get the GPU's bytes. */

/* the block's texels, RGBA8 words (R in the low byte), row by row */
uint px[16];
/* the weights the last Eval chose, a nibble a texel: texels 0 to 7, then 8 to 15 */
uint cur_lo;
uint cur_hi;
/* the best block so far: its endpoints as 8-bit RGBA words (seven bits and the p-bit), its weights, its error */
uint best_q0;
uint best_q1;
uint best_lo;
uint best_hi;
uint best_err;
/* the p-bit pairs a block may take: 0 to 3, or only 3 (both ends' bit 1) for a block whose every alpha is 255, only
   0 for one whose every alpha is 0, so that it keeps that alpha exactly: a nearly opaque texel blends, and a nearly
   transparent one passes an alpha test of more than nought */
uint pp_first;
uint pp_last;

uint Ch(uint word, uint c) {
    return (word >> (c * 8u)) & 0xFFu;
}

/* BC7's 4-bit weights, 0 4 9 13 17 21 26 30 34 38 43 47 51 55 60 64 */
uint Weight(uint i) {
    return (i * 64u + 7u) / 15u;
}

uint TexelError(uint p, uint q0, uint q1, uint w) {
    uint e = 0u;
    for (uint c = 0u; c < 4u; c++) {
        const int v = int(((64u - w) * Ch(q0, c) + w * Ch(q1, c) + 32u) >> 6u) - int(Ch(p, c));
        e += uint(v * v);
    }
    return e;
}

/* the block's squared error with endpoints q0 and q1, each texel at its best weight, which go to cur_lo and cur_hi */
uint Eval(uint q0, uint q1) {
    const int dr = int(Ch(q1, 0u)) - int(Ch(q0, 0u));
    const int dg = int(Ch(q1, 1u)) - int(Ch(q0, 1u));
    const int db = int(Ch(q1, 2u)) - int(Ch(q0, 2u));
    const int da = int(Ch(q1, 3u)) - int(Ch(q0, 3u));
    const int dd = dr * dr + dg * dg + db * db + da * da;
    uint err = 0u;
    cur_lo = 0u;
    cur_hi = 0u;
    for (uint k = 0u; k < 16u; k++) {
        const uint p = px[k];
        uint i = 0u;
        if (dd > 0) {
            /* the texel's place along the line, 0 to 64, then the nearest of the sixteen weights */
            const int t = clamp((int(Ch(p, 0u)) - int(Ch(q0, 0u))) * dr + (int(Ch(p, 1u)) - int(Ch(q0, 1u))) * dg +
                                    (int(Ch(p, 2u)) - int(Ch(q0, 2u))) * db + (int(Ch(p, 3u)) - int(Ch(q0, 3u))) * da,
                                0, dd);
            const uint t64 = uint((t * 64 + dd / 2) / dd);
            i = min((t64 * 15u + 32u) / 64u, 15u);
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
        if (i < 15u) {
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

/* an endpoint channel of 0 to 255 as seven bits under the p-bit p: the nearest of 2 e + p */
uint Quant(int v, uint p) {
    const int e = clamp((v - int(p) + 1) >> 1, 0, 127);
    return (uint(e) << 1u) | p;
}

/* a candidate line, a0 to a3 one end and b0 to b3 the other (RGBA, 0 to 255): its p-bit pairs, the best kept */
void Try(int a0, int a1, int a2, int a3, int b0, int b1, int b2, int b3) {
    for (uint pp = pp_first; pp <= pp_last; pp++) {
        const uint p0 = pp & 1u, p1 = pp >> 1u;
        const uint q0 = Quant(a0, p0) | (Quant(a1, p0) << 8u) | (Quant(a2, p0) << 16u) | (Quant(a3, p0) << 24u);
        const uint q1 = Quant(b0, p1) | (Quant(b1, p1) << 8u) | (Quant(b2, p1) << 16u) | (Quant(b3, p1) << 24u);
        const uint err = Eval(q0, q1);
        if (err < best_err) {
            best_err = err;
            best_q0 = q0;
            best_q1 = q1;
            best_lo = cur_lo;
            best_hi = cur_hi;
        }
    }
}

uint BestIndex(uint k) {
    return ((k < 8u ? best_lo : best_hi) >> ((k & 7u) * 4u)) & 15u;
}

/* the least squares fit to the best block's weights: sums of (64 - w)^2, (64 - w) w and w^2, a sixteenth of each so
   that the products in Solve stay under 2^31, and their determinant, nought when every texel took one weight */
int fit_a;
int fit_b;
int fit_d;
int Fit() {
    fit_a = 0;
    fit_b = 0;
    fit_d = 0;
    for (uint k = 0u; k < 16u; k++) {
        const int w = int(Weight(BestIndex(k)));
        fit_a += (64 - w) * (64 - w);
        fit_b += (64 - w) * w;
        fit_d += w * w;
    }
    fit_a >>= 4;
    fit_b >>= 4;
    fit_d >>= 4;
    return fit_a * fit_d - fit_b * fit_b;
}

/* channel c's end (side 0 or 1) of the fit whose determinant det is positive, rounded and held to 0 to 255 */
int Solve(uint c, uint side, int det) {
    int x = 0, y = 0;
    for (uint k = 0u; k < 16u; k++) {
        const int w = int(Weight(BestIndex(k)));
        const int v = int(Ch(px[k], c));
        x += (64 - w) * v;
        y += w * v;
    }
    const int num = side == 0u ? fit_d * x - fit_b * y : fit_a * y - fit_b * x;
    const int den = det * 16;
    return clamp(num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den), 0, 255);
}

uvec4 Encode() {
    /* the bounding box, the channel the block spans most, and each channel's sign against it */
    int lo0 = 255, lo1 = 255, lo2 = 255, lo3 = 255, hi0 = 0, hi1 = 0, hi2 = 0, hi3 = 0;
    int s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (uint k = 0u; k < 16u; k++) {
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
    pp_first = lo3 == 255 ? 3u : 0u;
    pp_last = hi3 == 0 ? 0u : 3u;
    /* covariances with the widest channel, sixteen times each value against sixteen times the mean: under 2^28 */
    int c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    for (uint k = 0u; k < 16u; k++) {
        const uint p = px[k];
        const int vw = 16 * int(Ch(p, wide)) - sw;
        c0 += (16 * int(Ch(p, 0u)) - s0) * vw;
        c1 += (16 * int(Ch(p, 1u)) - s1) * vw;
        c2 += (16 * int(Ch(p, 2u)) - s2) * vw;
        c3 += (16 * int(Ch(p, 3u)) - s3) * vw;
    }
    int a0 = c0 < 0 ? hi0 : lo0, b0 = c0 < 0 ? lo0 : hi0;
    int a1 = c1 < 0 ? hi1 : lo1, b1 = c1 < 0 ? lo1 : hi1;
    int a2 = c2 < 0 ? hi2 : lo2, b2 = c2 < 0 ? lo2 : hi2;
    int a3 = c3 < 0 ? hi3 : lo3, b3 = c3 < 0 ? lo3 : hi3;
    /* a sixteenth in from each end: the box's corners lie past the texels on any line not along an axis */
    const int i0 = (b0 - a0) / 16, i1 = (b1 - a1) / 16, i2 = (b2 - a2) / 16, i3 = (b3 - a3) / 16;
    best_err = 0xFFFFFFFFu;
    Try(a0 + i0, a1 + i1, a2 + i2, a3 + i3, b0 - i0, b1 - i1, b2 - i2, b3 - i3);
    Try(a0, a1, a2, a3, b0, b1, b2, b3);
    for (uint n = 0u; n < 2u && best_err > 0u; n++) {
        const int det = Fit();
        if (det <= 0) break;
        Try(Solve(0u, 0u, det), Solve(1u, 0u, det), Solve(2u, 0u, det), Solve(3u, 0u, det), Solve(0u, 1u, det),
            Solve(1u, 1u, det), Solve(2u, 1u, det), Solve(3u, 1u, det));
    }

    /* the first texel's weight is stored in three bits, its top bit nought: swap the ends when it is set */
    uint q0 = best_q0, q1 = best_q1, lo = best_lo, hi = best_hi;
    if ((lo & 8u) != 0u) {
        q0 = best_q1;
        q1 = best_q0;
        lo = ~lo;
        hi = ~hi;
    }
    /* mode 6: bit 6 set, then R0 R1 G0 G1 B0 B1 A0 A1 of seven bits, P0, P1, the first weight in three bits and the
       other fifteen in four */
    const uint r0 = Ch(q0, 0u) >> 1u, r1 = Ch(q1, 0u) >> 1u, g0 = Ch(q0, 1u) >> 1u, g1 = Ch(q1, 1u) >> 1u;
    const uint bb0 = Ch(q0, 2u) >> 1u, bb1 = Ch(q1, 2u) >> 1u, aa0 = Ch(q0, 3u) >> 1u, aa1 = Ch(q1, 3u) >> 1u;
    const uint w0 = 0x40u | (r0 << 7u) | (r1 << 14u) | (g0 << 21u) | (g1 << 28u);
    const uint w1 = (g1 >> 4u) | (bb0 << 3u) | (bb1 << 10u) | (aa0 << 17u) | (aa1 << 24u) | ((q0 & 1u) << 31u);
    const uint w2 = (q1 & 1u) | ((lo & 7u) << 1u) | (lo & 0xFFFFFFF0u);
    return uvec4(w0, w1, w2, hi);
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
