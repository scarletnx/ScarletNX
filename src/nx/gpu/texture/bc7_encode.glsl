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
   around their means, a pass each. Last, every block takes modes 4 and 5, one subset whose colour and alpha are
   lines of their own with weights of their own (ASTC's second plane), in each of the four rotations: alpha apart,
   or red, green or blue apart in alpha's place.

   A subset's line starts from its bounding box, each channel turned to run with the widest one (the sign of their
   covariance), pulled in by a sixteenth from each end and tried as it is too; then a least squares fit to the
   weights chosen, twice. The ends' p-bits are tried in each combination the mode allows, but a subset whose every
   alpha is 255 (or 0) keeps that alpha exactly; modes 4 and 5 have no p-bits and hold 255 and 0 at any precision.
   Each texel takes the best of the three weights around its projection. Integer arithmetic only, so the tests run
   this file on the CPU (tests/glsl/glsl.hpp) and get the GPU's bytes.

   Written for the code the GPU runs (docs/layers/gpu/astc.md): a partition is read from its table once a shape (SetShape), and
   each body is called from one place, in a loop (the jobs of Encode, the lines of FitSubset), as glslang inlines
   every call and the copies came to 1.2 MB of NVIDIA machine code. SNX_BC7_FAST 1 builds the encoder for large levels
   (vk/astc.c), which keeps mode 6 where it is close and tries the other modes only where it is not, and 2 mode 6
   alone, for the largest. */

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
/* the channels the shape's lines weigh, a bit each (red the lowest): all four, but modes 4 and 5 fit their colour
   (7) and their alpha (8) apart */
uint sh_mask;

/* the fast build (1), for large levels (vk/astc.c): a block whose mode 6 error is SNX_BC7_ENOUGH or less keeps it, the
   others try every mode; 2 is mode 6 alone, for the largest. The full build keeps mode 6 only where it is exact. 256 is
   the squared error summed over the 16 texels and 4 channels, 2 a channel as a root mean square: on one game's ASTC
   textures 96% of the blocks kept mode 6, and the worst error of mode 6 alone (147 to 239 a channel) came down to that
   of the full build (50 to 68). 3 is mode 6 from its first line alone, the box pulled in (SNX_BC7_LINES): the quick
   encode of a very large level, which the frame that first needs it waits for, until the background's fast build
   takes its place (vk/astc.c) */
#ifndef SNX_BC7_FAST
#define SNX_BC7_FAST 0
#endif
#if SNX_BC7_FAST == 3
#define SNX_BC7_LINES 1u
#else
#define SNX_BC7_LINES 4u
#endif
#define SNX_BC7_ENOUGH (SNX_BC7_FAST == 1 ? 256u : 0u)
/* modes 4 and 5 are tried unless this is 0: tests/glsl/bc7_glsl.cpp builds the encoder without them as well, to
   measure what they add */
#ifndef SNX_BC7_MODES_4_5
#define SNX_BC7_MODES_4_5 1
#endif
/* A block whose squared error, summed over its texels and channels, is this or less takes no more modes. 16 is a mean
   squared error of a quarter over 16 texels of 4 channels, 54 dB, for each block that stops, so what a block looks like
   doesn't bear on it: a hard one still gets the whole search. It is the most that keeps every PSNR tests/glsl/bc7_glsl.cpp
   holds the encoder to. That test builds the encoder with 0 too, for the checks that a block each mode holds comes back
   exact */
#ifndef SNX_BC7_GOOD_ENOUGH
#define SNX_BC7_GOOD_ENOUGH 16
#endif

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

/* the shape's subsets as fitted, and the best block so far: mode, partition, each subset's stored ends, weights;
   for modes 4 and 5 the rotation, the index selection bit, the colour's weights in best_lo and best_hi and the
   alpha's in best_lo2 and best_hi2 */
uint cand_s0[3];
uint cand_s1[3];
uint best_mode;
uint best_part;
uint best_s0[3];
uint best_s1[3];
uint best_lo;
uint best_hi;
uint best_err;
uint best_rot;
uint best_isb;
uint best_lo2;
uint best_hi2;

uint Ch(uint word, uint c) {
    return (word >> (c * 8u)) & 0xFFu;
}

/* the shape's partition read from the tables once (SetShape): each texel's subset, two bits a texel, and the anchors
   of subsets 1 and 2 (16, no texel, where the shape has no such subset). The tables are arrays indexed by a value
   that differs between invocations, which a GPU keeps in local memory: read for each texel of each fit, they made a
   block with subsets take hundreds of times longer than one without */
uint sh_pbits;
uint sh_anchor1;
uint sh_anchor2;
void SetShape(uint ns, uint part) {
    sh_ns = ns;
    sh_part = part;
    sh_anchor1 = 16u;
    sh_anchor2 = 16u;
    if (ns == 1u) {
        sh_pbits = 0u;
    } else if (ns == 2u) {
        /* a bit a texel spread to two: bit k to bit 2k */
        uint v = P2[part];
        v = (v | (v << 8u)) & 0x00FF00FFu;
        v = (v | (v << 4u)) & 0x0F0F0F0Fu;
        v = (v | (v << 2u)) & 0x33333333u;
        v = (v | (v << 1u)) & 0x55555555u;
        sh_pbits = v;
        sh_anchor1 = ANCHORS[part] & 15u;
    } else {
        sh_pbits = P3[part];
        const uint an = ANCHORS[part];
        sh_anchor1 = (an >> 4u) & 15u;
        sh_anchor2 = an >> 8u; /* ANCHORS holds 12 bits, so no mask (ucas) */
    }
}

uint SubsetOf(uint k) {
    return (sh_pbits >> (k * 2u)) & 3u;
}

/* the anchor texel of subset s: texel 0 for the first */
uint AnchorOf(uint s) {
    return s == 0u ? 0u : s == 1u ? sh_anchor1 : sh_anchor2;
}

bool IsAnchor(uint k) {
    return k == 0u || k == sh_anchor1 || k == sh_anchor2;
}

/* the weight of index i: BC7's 2, 3 and 4-bit tables (0 21 43 64; 0 9 18 27 37 46 55 64; 0 4 9 13 ... 60 64). They
   are (i * 64 + top / 2) / top for top = 2^bits - 1, and equal (i * K + 32) >> 6 with K = 4095 / top (1365, 585, 273;
   4095 = 3^2 5 7 13 is a multiple of each top): one multiply where the encoder calls this, for every texel of every
   line it tries. K depends on sh_ib alone, so the select leaves the loops that call this (ucas: exhaustive equiv
   against the tables, every i < 2^bits of bits 2, 3 and 4). It relies on i <= top: Eval's weights are in 0 to top,
   and fit_lo and fit_hi hold only the weights of an Eval that ran to its end */
uint Weight(uint i) {
    const uint k = sh_ib == 4u ? 273u : sh_ib == 3u ? 585u : 1365u;
    return (i * k + 32u) >> 6u;
}

uint Index(uint lo, uint hi, uint k) {
    return ((k < 8u ? lo : hi) >> ((k & 7u) * 4u)) & 15u;
}

/* bits of an end's channel as stored (with the p-bit), and alpha's */
uint StoredBits() {
    return sh_cb + (sh_pk != 0u ? 1u : 0u);
}

/* an n-bit stored channel as the decoder widens it to 8 bits: its top bits again below it. For n = 8 the second term is
   v >> 8 = 0, and v < 2^n keeps the result under 256, so neither a branch nor a mask is needed (ucas: exhaustive equiv
   over n = 5 to 8, every v < 2^n) */
uint Widen(uint v, uint n) {
    return (v << (8u - n)) | (v >> (2u * n - 8u));
}

/* the stored ends as the decoder reads them, an RGBA8 word, alpha 255 where the shape stores none */
uint Decoded(uint stored) {
    const uint n = StoredBits();
    uint word = Widen(Ch(stored, 0u), n) | (Widen(Ch(stored, 1u), n) << 8u) | (Widen(Ch(stored, 2u), n) << 16u);
    return word | ((sh_alpha != 0u ? Widen(Ch(stored, 3u), n) : 255u) << 24u);
}

/* the stored value of n bits (its lowest the p-bit p when the shape has p-bits) that widens nearest to t, the lower on
   a tie. It is guess - 1 or guess without p-bits; with them guess when its parity is p, else guess - 1 or guess + 1:
   two candidates, compared once (ucas: exhaustive equiv with the search of guess - 2 to guess + 2, over t 0 to 255, n 5
   to 8, both p and with and without p-bits) */
uint Quant(int t, uint p) {
    const uint n = StoredBits();
    const int top = int((1u << n) - 1u);
    const int guess = (t * top + 127) / 255;
    const bool other = sh_pk != 0u && (uint(guess) & 1u) != p;
    int lo = sh_pk == 0u || other ? guess - 1 : guess;
    const int hi = other ? guess + 1 : guess;
    lo = lo < 0 ? hi : lo;
    /* hi = top + 1 (guess = top of the wrong parity) widens past 255 and never wins, so it needs no clamp (ucas: the
       function-level pass, exhaustive over Quant's every call) */
    const int el = int(Widen(uint(lo), n)) - t, eh = int(Widen(uint(hi), n)) - t;
    return uint(abs(el) <= abs(eh) ? lo : hi);
}

uint QuantWord(int r, int g, int b, int a, uint p) {
    return Quant(r, p) | (Quant(g, p) << 8u) | (Quant(b, p) << 16u) | (sh_alpha != 0u ? Quant(a, p) << 24u : 0u);
}

uint TexelError(uint p, uint q0, uint q1, uint w) {
    uint e = 0u;
    for (uint c = 0u; c < 4u; c++) {
        if (((sh_mask >> c) & 1u) == 0u) continue;
        /* ((64 - w) a + w b + 32) >> 6 = a + ((w (b - a) + 32) >> 6), the shift arithmetic: one multiply (ucas:
           exhaustive equiv over a, b 0 to 255 and w 0 to 64) */
        const int a0 = int(Ch(q0, c));
        const int v = a0 + ((int(w) * (int(Ch(q1, c)) - a0) + 32) >> 6) - int(Ch(p, c));
        e += uint(v * v);
    }
    return e;
}

/* channel c of q1 less q0's, nought for a channel the shape does not weigh */
int Step(uint q0, uint q1, uint c) { return ((sh_mask >> c) & 1u) != 0u ? int(Ch(q1, c)) - int(Ch(q0, c)) : 0; }

/* subset s's squared error with decoded ends q0 and q1, each of its texels at its best weight, which go to cur_lo
   and cur_hi */
uint Eval(uint s, uint q0, uint q1) {
    const int dr = Step(q0, q1, 0u);
    const int dg = Step(q0, q1, 1u);
    const int db = Step(q0, q1, 2u);
    const int da = Step(q0, q1, 3u);
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
            /* t <= dd keeps t64 <= 64 ((64 dd + dd / 2) / dd = 64), so (t64 top + 32) / 64 <= top: no min */
            const uint t64 = uint((t * 64 + dd / 2) / dd);
            i = (t64 * top + 32u) / 64u;
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
        if (err >= fit_err) return err; /* Try keeps only a smaller one */
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
   shape and the subset allow (allowed, PAllowed(s): FitSubset reads it once, as nothing it depends on changes between
   its lines), the best kept in fit_ */
void Try(uint s, uint allowed, int a0, int a1, int a2, int a3, int b0, int b1, int b2, int b3) {
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

/* the least squares fit of subset s's ends to the weights fit_ holds, the line v = e0 + (e1 - e0) w / 64 nearest its
   texels: how many there are, the sums of their weights and of the weights' squares, and the determinant n W2 - W W,
   nought when every texel took one weight. Whole sums: each product in Solve stays under 2^28, its numerator under
   2^29 */
int fit_n;
int fit_w;
int fit_w2;
int Fit(uint s) {
    fit_n = 0;
    fit_w = 0;
    fit_w2 = 0;
    for (uint k = 0u; k < 16u; k++) {
        if (SubsetOf(k) != s) continue;
        const int w = int(Weight(Index(fit_lo, fit_hi, k)));
        fit_n++;
        fit_w += w;
        fit_w2 += w * w;
    }
    return fit_n * fit_w2 - fit_w * fit_w;
}

/* channel c's end (side 0 or 1) of subset s's fit whose determinant det is positive, rounded and held to 0 to 255:
   from the sums of the values (V) and of each by its weight (WV), e0 = (V W2 - W WV) / det and e1 = e0 + 64 (n WV -
   W V) / det. A channel of one value gets that value at both ends; one the shape does not weigh, nought */
int SolveEnd(uint c, uint side, int det, int v, int wv) {
    if (((sh_mask >> c) & 1u) == 0u) return 0;
    int num = v * fit_w2 - fit_w * wv;
    if (side != 0u) num += 64 * (fit_n * wv - fit_w * v);
    return clamp(num >= 0 ? (num + det / 2) / det : -((-num + det / 2) / det), 0, 255);
}
int Solve(uint s, uint c, uint side, int det) {
    int v = 0, wv = 0;
    for (uint k = 0u; k < 16u; k++) {
        if (SubsetOf(k) != s) continue;
        const int w = int(Weight(Index(fit_lo, fit_hi, k)));
        const int x = int(Ch(px[k], c));
        v += x;
        wv += w * x;
    }
    return SolveEnd(c, side, det, v, wv);
}

/* fits subset s of the shape: fit_err and the rest */
void FitSubset(uint s) {
    /* the bounding box, the channel the shape weighs that the subset spans most, and each channel's sign against
       it */
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
    int span = (sh_mask & 1u) != 0u ? hi0 - lo0 : -1;
    if ((sh_mask & 2u) != 0u && hi1 - lo1 > span) {
        wide = 1u;
        span = hi1 - lo1;
    }
    if ((sh_mask & 4u) != 0u && hi2 - lo2 > span) {
        wide = 2u;
        span = hi2 - lo2;
    }
    if ((sh_mask & 8u) != 0u && hi3 - lo3 > span) wide = 3u;
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
    const uint allowed = PAllowed(s);
    uint seen_lo = 0u, seen_hi = 0u;
    /* the lines tried, in turn: the box pulled in, the box, then a least squares fit to the weights chosen, twice.
       One call of Try in a loop, not one a line: the compiler inlines each call, and the copies' registers (the
       whole encoder's, as each shape inlines this too) made the GPU spill and run fewer blocks at once */
    for (uint step = 0u; step < SNX_BC7_LINES; step++) {
        int e0 = a0 + i0, e1 = a1 + i1, e2 = a2 + i2, e3 = a3 + i3;
        int f0 = b0 - i0, f1 = b1 - i1, f2 = b2 - i2, f3 = b3 - i3;
        if (step == 1u) {
            e0 = a0, e1 = a1, e2 = a2, e3 = a3;
            f0 = b0, f1 = b1, f2 = b2, f3 = b3;
        } else if (step >= 2u) {
            if (fit_err == 0u) break;
            /* the same weights give the same line */
            if (step == 3u && fit_lo == seen_lo && fit_hi == seen_hi) break;
            seen_lo = fit_lo;
            seen_hi = fit_hi;
            /* Fit's sums and Solve's for every channel in one pass over the texels */
            fit_n = 0;
            fit_w = 0;
            fit_w2 = 0;
            int v0 = 0, v1 = 0, v2 = 0, v3 = 0, w0 = 0, w1 = 0, w2 = 0, w3 = 0;
            for (uint k = 0u; k < 16u; k++) {
                if (SubsetOf(k) != s) continue;
                const int w = int(Weight(Index(fit_lo, fit_hi, k)));
                const uint p = px[k];
                fit_n++;
                fit_w += w;
                fit_w2 += w * w;
                v0 += int(Ch(p, 0u));
                v1 += int(Ch(p, 1u));
                v2 += int(Ch(p, 2u));
                v3 += int(Ch(p, 3u));
                w0 += w * int(Ch(p, 0u));
                w1 += w * int(Ch(p, 1u));
                w2 += w * int(Ch(p, 2u));
                w3 += w * int(Ch(p, 3u));
            }
            const int det = fit_n * fit_w2 - fit_w * fit_w;
            if (det <= 0) break;
            e0 = SolveEnd(0u, 0u, det, v0, w0);
            e1 = SolveEnd(1u, 0u, det, v1, w1);
            e2 = SolveEnd(2u, 0u, det, v2, w2);
            e3 = SolveEnd(3u, 0u, det, v3, w3);
            f0 = SolveEnd(0u, 1u, det, v0, w0);
            f1 = SolveEnd(1u, 1u, det, v1, w1);
            f2 = SolveEnd(2u, 1u, det, v2, w2);
            f3 = SolveEnd(3u, 1u, det, v3, w3);
        }
        Try(s, allowed, e0, e1, e2, e3, f0, f1, f2, f3);
    }
}

/* fits every subset of a shape (mode, subsets, partition, colour bits, alpha stored, p-bits, index bits) and keeps
   it as best_ when its error is the least so far; it stops at a subset whose error already passes the best */
void TryShape(uint mode, uint ns, uint part, uint cb, uint alpha, uint pk, uint ib) {
    SetShape(ns, part);
    sh_cb = cb;
    sh_alpha = alpha;
    sh_pk = pk;
    sh_ib = ib;
    sh_mask = 15u;
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

/* swaps each texel's alpha with red (rot 1), green (2) or blue (3), as the decoder of modes 4 and 5 does after
   interpolating: a second swap undoes the first */
void Rotate(uint rot) {
    if (rot == 0u) return;
    const uint sh = (rot - 1u) * 8u;
    for (uint k = 0u; k < 16u; k++) {
        const uint p = px[k];
        px[k] = (p & ~(0xFF000000u | (0xFFu << sh))) | ((p >> 24u) << sh) | (((p >> sh) & 0xFFu) << 24u);
    }
}

/* mode 4 or 5 in rotation rot, kept as best_ when its error is the least so far: the colour's line (red, green and
   blue, mode 4's 5 bits or mode 5's 7) fitted first, then the alpha's (6 bits or 8) unless the colour's error already
   passes the best. Mode 5's weights are 2-bit; mode 4 gives one of the two 3-bit weights, the colour when isb is 1 */
void TryDual(uint mode, uint rot, uint isb) {
    Rotate(rot);
    SetShape(1u, 0u);
    sh_pk = 0u;
    uint err = 0u, s0 = 0u, s1 = 0u, lo = 0u, hi = 0u;
    /* the colour, then the alpha: one call of FitSubset (see FitSubset) */
    for (uint pass = 0u; pass < 2u; pass++) {
        const bool colour = pass == 0u;
        sh_mask = colour ? 7u : 8u;
        sh_alpha = colour ? 0u : 1u;
        sh_cb = mode == 4u ? (colour ? 5u : 6u) : (colour ? 7u : 8u);
        sh_ib = mode == 4u && (isb != 0u) == colour ? 3u : 2u;
        FitSubset(0u);
        if (!colour) break;
        err = fit_err;
        s0 = fit_s0;
        s1 = fit_s1;
        lo = fit_lo;
        hi = fit_hi;
        if (err >= best_err) break;
    }
    if (err < best_err && err + fit_err < best_err) {
        best_err = err + fit_err;
        best_mode = mode;
        best_part = 0u;
        best_rot = rot;
        best_isb = isb;
        best_s0[0] = (s0 & 0xFFFFFFu) | (fit_s0 & 0xFF000000u);
        best_s1[0] = (s1 & 0xFFFFFFu) | (fit_s1 & 0xFF000000u);
        best_lo = lo;
        best_hi = hi;
        best_lo2 = fit_lo;
        best_hi2 = fit_hi;
    }
    Rotate(rot);
}

/* the block's sums for Spread, over every texel: the count is 16; the sums of squares, then of each channel (alpha
   counted, and its square apart, as Spread leaves it out where the shape has no alpha) */
int blk_q;
int blk_qa;
int blk_r;
int blk_g;
int blk_b;
int blk_a;
void BlockSums() {
    blk_q = 0;
    blk_qa = 0;
    blk_r = 0;
    blk_g = 0;
    blk_b = 0;
    blk_a = 0;
    for (uint k = 0u; k < 16u; k++) {
        const uint p = px[k];
        const int vr = int(Ch(p, 0u)), vg = int(Ch(p, 1u)), vb = int(Ch(p, 2u)), va = int(Ch(p, 3u));
        blk_q += vr * vr + vg * vg + vb * vb;
        blk_qa += va * va;
        blk_r += vr;
        blk_g += vg;
        blk_b += vb;
        blk_a += va;
    }
}
/* one subset's sum of squared distances around its mean, from its count and sums */
uint SpreadOf(int n, int q, int r, int g, int b, int a) {
    return n > 0 ? uint((n * q - r * r - g * g - b * b - a * a) / n) : 0u;
}
/* how far partition part's subsets spread around their means: each subset's sum of squared distances, from its
   sums (n Q - S^2 for each channel, over n), alpha included when alpha says so. One pass over the texels sums
   subsets 1 and 2; subset 0's are the block's (BlockSums) less theirs */
uint Spread(uint ns, uint part, uint alpha) {
    SetShape(ns, part);
    int n1 = 0, q1 = 0, r1 = 0, g1 = 0, b1 = 0, a1 = 0;
    int n2 = 0, q2 = 0, r2 = 0, g2 = 0, b2 = 0, a2 = 0;
    for (uint k = 0u; k < 16u; k++) {
        const uint sub = SubsetOf(k);
        if (sub == 0u) continue;
        const uint p = px[k];
        const int vr = int(Ch(p, 0u)), vg = int(Ch(p, 1u)), vb = int(Ch(p, 2u));
        const int va = alpha != 0u ? int(Ch(p, 3u)) : 0;
        const int q = vr * vr + vg * vg + vb * vb + va * va;
        if (sub == 1u) {
            n1++;
            q1 += q;
            r1 += vr;
            g1 += vg;
            b1 += vb;
            a1 += va;
        } else {
            n2++;
            q2 += q;
            r2 += vr;
            g2 += vg;
            b2 += vb;
            a2 += va;
        }
    }
    const int q = blk_q + (alpha != 0u ? blk_qa : 0), a = alpha != 0u ? blk_a : 0;
    return SpreadOf(16 - n1 - n2, q - q1 - q2, blk_r - r1 - r2, blk_g - g1 - g2, blk_b - b1 - b2, a - a1 - a2) +
           SpreadOf(n1, q1, r1, g1, b1, a1) + SpreadOf(n2, q2, r2, g2, b2, a2);
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

/* the block of mode 4 or 5 that best_ holds. Texel 0's weight has its top bit nought in each of its two sets: where it
   is set, that set's ends swap (the colour's or the alpha's alone) and its weights turn round */
uvec4 PackDual() {
    const uint mode = best_mode;
    const uint cb = mode == 4u ? 5u : 7u, ab = mode == 4u ? 6u : 8u;
    const uint ctop = mode == 4u && best_isb != 0u ? 7u : 3u, atop = mode == 4u && best_isb == 0u ? 7u : 3u;
    uint e0 = best_s0[0], e1 = best_s1[0], clo = best_lo, chi = best_hi, alo = best_lo2, ahi = best_hi2;
    if ((clo & 15u) > ctop / 2u) {
        const uint t = e0;
        e0 = (e1 & 0xFFFFFFu) | (e0 & 0xFF000000u);
        e1 = (t & 0xFFFFFFu) | (e1 & 0xFF000000u);
        clo = ctop * 0x11111111u - clo;
        chi = ctop * 0x11111111u - chi;
    }
    if ((alo & 15u) > atop / 2u) {
        const uint t = e0;
        e0 = (e0 & 0xFFFFFFu) | (e1 & 0xFF000000u);
        e1 = (e1 & 0xFFFFFFu) | (t & 0xFF000000u);
        alo = atop * 0x11111111u - alo;
        ahi = atop * 0x11111111u - ahi;
    }
    blk = uvec4(0u);
    Put(mode, 1u, 1u);
    uint pos = mode + 1u;
    Put(pos, best_rot, 2u);
    pos += 2u;
    if (mode == 4u) {
        Put(pos, best_isb, 1u);
        pos++;
    }
    /* red of both ends, then green, blue and alpha */
    for (uint c = 0u; c < 4u; c++) {
        const uint bits = c < 3u ? cb : ab;
        Put(pos, Ch(e0, c), bits);
        Put(pos + bits, Ch(e1, c), bits);
        pos += bits * 2u;
    }
    /* the 2-bit weights first (the colour's, but in mode 4 with isb 1), then the others, texel 0's a bit shorter */
    const bool colour_first = mode == 5u || best_isb == 0u;
    for (uint set = 0u; set < 2u; set++) {
        const bool colour = (set == 0u) == colour_first;
        const uint lo = colour ? clo : alo, hi = colour ? chi : ahi, top = colour ? ctop : atop;
        const uint ib = top == 7u ? 3u : 2u;
        for (uint k = 0u; k < 16u; k++) {
            const uint bits = k == 0u ? ib - 1u : ib;
            Put(pos, Index(lo, hi, k), bits);
            pos += bits;
        }
    }
    return blk;
}

uvec4 Encode() {
    opaque = true;
    for (uint k = 0u; k < 16u; k++) opaque = opaque && Ch(px[k], 3u) == 255u;
    best_err = 0xFFFFFFFFu;
    /* The shapes tried, in turn, one call of TryShape and one of TryDual for all of them (see FitSubset):
       job 0, mode 6: one subset, RGBA, 7 bits and a p-bit an end, 4-bit weights. Unless it is exact, then the two
       partitions of two subsets that spread least (and the one of three) in the modes with subsets: an opaque block
       mode 1 (two subsets, RGB, 6 bits an end and a p-bit a subset, 3-bit weights) and mode 3 (7 bits and a p-bit an
       end, 2-bit weights) in each of the two, then mode 2 (three subsets, RGB, 5 bits an end, 2-bit weights); any
       other block mode 7 (two subsets, RGBA, 5 bits and a p-bit an end, 2-bit weights) in each. Last modes 5, 4 and 4
       with the other index selection, in each rotation */
    const uint shapes = 1u + (opaque ? 5u : 2u);
    const uint jobs = shapes + (SNX_BC7_MODES_4_5 != 0 ? 12u : 0u);
    uint first = 0u, second = 1u, third = 0u;
    for (uint j = 0u; j < jobs; j++) {
        if (best_err <= uint(SNX_BC7_GOOD_ENOUGH)) break;
        if (j == 1u) {
            if (SNX_BC7_FAST == 2 || SNX_BC7_FAST == 3 || best_err <= SNX_BC7_ENOUGH) break;
            BlockSums();
            const uint alpha = opaque ? 0u : 1u;
            uint f_cost = 0xFFFFFFFFu, s_cost = 0xFFFFFFFFu;
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
                uint t_cost = 0xFFFFFFFFu;
                for (uint p = 0u; p < 64u; p++) {
                    const uint c = Spread(3u, p, 0u);
                    if (c < t_cost) {
                        third = p;
                        t_cost = c;
                    }
                }
            }
        }
        /* the job: a shape of ns subsets fitted in turn, or mode 4 or 5 in a rotation, its colour then its alpha
           fitted (one call of FitSubset for both, see FitSubset). Either stops once its error passes the best */
        const bool dual = j >= shapes;
        uint mode, part = 0u, rot = 0u, isb = 0u, steps;
        if (!dual) {
            /* the job's mode and partition: opaque 1, 1, 3, 3, 2; else 7, 7; the first, second, first, second,
               third */
            const uint i = j - 1u;
            mode = j == 0u ? 6u : opaque ? (0x23311u >> (i * 4u)) & 15u : 7u;
            part = j == 0u ? 0u : i == 4u ? third : (i & 1u) == 0u ? first : second;
            steps = mode == 6u ? 1u : mode == 2u ? 3u : 2u;
            SetShape(steps, part);
            sh_cb = mode == 1u ? 6u : mode == 2u || mode == 7u ? 5u : 7u;
            sh_alpha = mode == 6u || mode == 7u ? 1u : 0u;
            sh_pk = mode == 2u ? 0u : mode == 1u ? 1u : 2u;
            sh_ib = mode == 6u ? 4u : mode == 1u ? 3u : 2u;
            sh_mask = 15u;
        } else {
            /* rotation, then modes 5, 4 and 4 with the other index selection */
            const uint d = j - shapes, m = d % 3u;
            mode = m == 0u ? 5u : 4u;
            rot = d / 3u;
            isb = m == 2u ? 1u : 0u;
            steps = 2u;
            Rotate(rot);
            SetShape(1u, 0u);
            sh_pk = 0u;
        }
        uint err = 0u, lo = 0u, hi = 0u, s0 = 0u, s1 = 0u;
        bool better = true;
        for (uint s = 0u; s < steps; s++) {
            if (dual) {
                const bool colour = s == 0u;
                sh_mask = colour ? 7u : 8u;
                sh_alpha = colour ? 0u : 1u;
                sh_cb = mode == 4u ? (colour ? 5u : 6u) : (colour ? 7u : 8u);
                sh_ib = mode == 4u && (isb != 0u) == colour ? 3u : 2u;
            }
            FitSubset(dual ? 0u : s);
            err += fit_err;
            if (err >= best_err) {
                better = false;
                break;
            }
            if (!dual) {
                cand_s0[s] = fit_s0;
                cand_s1[s] = fit_s1;
                lo |= fit_lo;
                hi |= fit_hi;
            } else if (s == 0u) {
                s0 = fit_s0;
                s1 = fit_s1;
                lo = fit_lo;
                hi = fit_hi;
            }
        }
        if (better) {
            best_err = err;
            best_mode = mode;
            best_part = part;
            best_lo = lo;
            best_hi = hi;
            if (!dual) {
                for (uint t = 0u; t < 3u; t++) {
                    best_s0[t] = cand_s0[t];
                    best_s1[t] = cand_s1[t];
                }
            } else {
                best_rot = rot;
                best_isb = isb;
                /* the colour's ends store no alpha (sh_alpha 0), so they need no mask (ucas) */
                best_s0[0] = s0 | (fit_s0 & 0xFF000000u);
                best_s1[0] = s1 | (fit_s1 & 0xFF000000u);
                best_lo2 = fit_lo;
                best_hi2 = fit_hi;
            }
        }
        if (dual) Rotate(rot);
    }
    if (best_mode == 4u || best_mode == 5u) return PackDual();
    /* the best's shape again, for Pack */
    SetShape(best_mode == 6u ? 1u : best_mode == 2u ? 3u : 2u, best_part);
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
