/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/host1x/vic.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project */
/* The VIC composition: the body of vic.comp, which declares the parameters (job, slots: vic_params.glsl), the input
   planes (inputs, read only) and the output planes (outputs). It gives the bytes gpu/video/vic.c's compose_cpu
   writes, which tests/glsl/vic_glsl.cpp checks by running this file on the CPU (tests/glsl/glsl.hpp has the rules
   of the GLSL that is also C++).

   An invocation owns a block of 4x2 output pixels: two luma words and a chroma word of NV12, or eight RGBA words.
   Every output word has one owner, so nothing is shared, staged or waited for: there is no shared memory and no
   barrier. Each input word a pixel needs is loaded once, by the invocation that needs it, straight from the plane
   (the neighbors' loads of the same cache lines coalesce).

   Written for what the machine runs, not for reading (docs/layers/media/vic.md):
   - The winner first, then the work. The last enabled slot covering a pixel composes it. The slots are walked from
     the last down, each claims the pixels still unclaimed (a 4 bit mask per row from two shifts), and the walk ends
     when none are left: one slot over the picture costs one pass, where composing every covering slot in turn and
     overwriting paid for all of them.
   - No matrix, no arithmetic. A sample goes to 10 bit (<< 2) and back to 8 (>> 2) unchanged, so without the colour
     matrix NV12 out is the input's luma and chroma words as they are (the chroma pair's bytes swapped when the
     input is NV21 and the output NV12, or the other way), and RGBA out is the bytes moved into place.
   - The matrix's shift folded: (Y << 2) * c0 is Y * (c0 * 4) exactly, so vic_gpu_params scales the coefficients
     and the samples stay 8 bit. The soft clamp is the CPU's, compares on the 10 bit value then >> 2, which holds for
     a clamp whose low end is above its high one too (below lo it gives lo, from there on hi), where clamping the
     byte (floor and clamp commuting) would not.
   - Chroma terms per pair. U * c1 + V * c2 is the same for a pair's pixels and for the two rows of the block that
     share a chroma row, so it is computed once per pair: 3 multiplications a pixel for RGBA, and for NV12 out only
     the R row for every pixel and G and B for the two pixels a chroma word takes them from.
   - Branches only where a whole slot, job or plane decides. The column alignment of a slot's source, its chroma
     row parity, its matrix, the plane's layout and the output format are the same for every invocation, so their
     branches never diverge; the per-pixel choices (which slot, which bytes) are masks.
   - Words, not bytes. A source window that isn't word aligned is two loads funneled into one word; a byte of a
     4 bit pixel mask becomes a byte mask with one multiplication ((m * 0x204081) & 0x01010101) * 0xFF; a partial
     word is merged with a ^ ((a ^ b) & mask); a fully owned word is stored without reading it.

   The -DVIC_UNALIGNED build is for planes whose rows aren't word aligned (a pitch linear pitch not a multiple of 4,
   which the host checks): its loads funnel two aligned words, its stores insert with atomics, as invocations can
   then share a word. */

/* byte x of a plane's row, and row y: block linear offsets split into a column and a row part that add up
   (gpu/gpu.c's bl_x and bl_yz with z 0), pitch linear the row's start */
uint xpart(VicGpuPlane p, uint x) {
    if (p.blk == 0u) return x;
    return ((x >> 6) << p.bsh) + (x & 31u) + (x & 16u) + ((x & 32u) << 3);
}
uint ypart(VicGpuPlane p, uint y) {
    if (p.blk == 0u) return y * p.pitch;
    return (y >> p.ysh) * p.bstride + ((y << 6) & p.gm) + ((y & 6u) << 5) + ((y & 1u) << 4);
}
/* the row after y, whose part is yp: an even row's odd neighbor is the next 16 bytes of its GOB */
uint ynext(VicGpuPlane p, uint y, uint yp) {
    if (p.blk == 0u) return yp + p.pitch;
    if ((y & 1u) == 0u) return yp + 16u;
    return ypart(p, y + 1u);
}

/* the input word at byte off of a plane: 0 past its end, as compose_cpu's plane_rd reads it. A word that starts
   inside and ends outside isn't masked: its bytes past the end are columns past a pitch linear row's (block linear
   planes end on whole GOBs), which no slot covers. The load stays in the binding whatever off is (the host pads it
   by a word for the funnel) */
uint fetch(VicGpuPlane p, uint off) {
    uint a = p.base + min(off, p.lim - 1u);
#ifdef VIC_UNALIGNED
    uint s = (a & 3u) << 3, w = inputs[a >> 2] >> s;
    if (s != 0u) w |= inputs[(a >> 2) + 1u] << (32u - s);
#else
    uint w = inputs[a >> 2]; /* off is word aligned: the host only picks this build when every row is */
#endif
    return off < p.lim ? w : 0u;
}
/* bytes x to x + 3 of the row whose part is yp: one aligned word, or two funneled */
uint read4(VicGpuPlane p, uint x, uint yp) {
    uint s = (x & 3u) << 3, w = fetch(p, xpart(p, x & ~3u) + yp);
    if (s != 0u) w = (w >> s) | (fetch(p, xpart(p, (x & ~3u) + 4u) + yp) << (32u - s));
    return w;
}

/* the bytes m selects of v into the output word at byte off, leaving the others alone. Words past the plane's end
   (an output cut short by its mapping) take none; m never selects a byte past it in a word that starts inside, as
   only pixels of the target rectangle are owned */
void store(VicGpuPlane p, uint off, uint v, uint m) {
    if (m == 0u || off >= p.lim) return;
    uint a = p.base + off;
#ifdef VIC_UNALIGNED
    uint i = a >> 2, s = (a & 3u) << 3;
    atomicAnd(outputs[i], ~(m << s));
    atomicOr(outputs[i], (v & m) << s);
    if (s != 0u && (m >> (32u - s)) != 0u) {
        atomicAnd(outputs[i + 1u], ~(m >> (32u - s)));
        atomicOr(outputs[i + 1u], (v & m) >> (32u - s));
    }
#else
    if (m != ~0u) v ^= (outputs[a >> 2] ^ v) & ~m;
    outputs[a >> 2] = v;
#endif
}

/* the pixels of the block at (px, py) inside columns x0 to x1 - 1 and h rows from y0: bits 0-3 the first row's
   four, 4-7 the second's */
uint cover(uint x0, uint x1, uint y0, uint h, uint px, uint py) {
    uint lo = min(max(x0, px) - px, 4u), hi = min(max(x1, px) - px, 4u), t = py - y0;
    return ((1u << hi) - (1u << lo)) * (uint(t < h) | (uint(t + 1u < h) << 4));
}
/* a 4 bit pixel mask as a byte mask */
uint bytes(uint m) { return ((m * 0x204081u) & 0x01010101u) * 0xFFu; }

/* a 10 bit matrix value t soft clamped as the CPU does it (lo first: below lo is lo even when lo > hi), as a byte */
uint vic_clamp(int t, int lo, int hi) { return uint(t < lo ? lo >> 2 : (t > hi ? hi >> 2 : t >> 2)); }
/* a matrix channel's output byte, from the sum of the products */
#define VIC_F(v, c3) vic_clamp((((v) >> msh) + (c3)) >> 8, lo, hi)
/* byte i of word w, as an int */
#define VIC_B(w, i) int(((w) >> (8u * (i))) & 0xFFu)
/* the chroma terms of pair word q (U at ush, V at vsh), as ints t##R, t##G, t##B */
#define VIC_TERMS(t, q)                                                                                                \
    {                                                                                                                  \
        int u_ = int(((q) >> ush) & 0xFFu), v_ = int(((q) >> vsh) & 0xFFu);                                            \
        t##R = u_ * ru + v_ * rv;                                                                                      \
        t##G = u_ * gu + v_ * gv;                                                                                      \
        t##B = u_ * bu + v_ * bv;                                                                                      \
    }
/* an RGBA word without the matrix, from luma byte y and pair word q */
#define VIC_PIX(y, q) (((y) << rpos) | ((((q) >> ush) & 0xFFu) << 8) | ((((q) >> vsh) & 0xFFu) << bpos) | aw)
/* an RGBA word with it, from luma y (an int) and the pair's terms t */
#define VIC_MAT(y, t)                                                                                                  \
    ((VIC_F((y) * ry + t##R, r3) << rpos) | (VIC_F((y) * gy + t##G, g3) << 8) | (VIC_F((y) * by + t##B, b3) << bpos) | aw)

void main() {
    uint px = job.gx0 + (gl_GlobalInvocationID.x << 2), py = job.gy0 + (gl_GlobalInvocationID.y << 1);
    /* the pixels the invocation writes: the target's. The slots lie inside it (vic.c's slot_resolve clips them),
       and it inside the surface */
    uint m = cover(job.tx0, job.tx1, job.ty0, job.th, px, py), rem = m;
    if (m == 0u) return;
    uint oyp = ypart(job.ol, py), oyq = ynext(job.ol, py, oyp);
    uint rpos = job.rpos, gpos = job.gpos, bpos = job.bpos;

    if (job.nv12 != 0u) {
        /* NV12 out: a slot's source column and row are even (slot_resolve rounds both rectangles' starts down to
           a pair), so the block's chroma is one pair-aligned row for both its rows */
        uint l0 = job.bgy, l1 = job.bgy, c0 = job.bgc;
        for (uint s = job.nslots; s-- > 0u && rem != 0u;) {
            uint c = cover(slots[s].dx0, slots[s].dx1, slots[s].dy0, slots[s].h, px, py) & rem;
            if (c == 0u) continue;
            rem &= ~c;
            uint sx = px + slots[s].ox, sy = py + slots[s].oy, yp = ypart(slots[s].iy, sy);
            uint y0 = read4(slots[s].iy, sx, yp), y1 = read4(slots[s].iy, sx, ynext(slots[s].iy, sy, yp));
            uint ch = read4(slots[s].ic, sx, ypart(slots[s].ic, sy >> 1)), cc;
            if (slots[s].mat == 0u)
                cc = slots[s].cswap != 0u ? ((ch & 0x00FF00FFu) << 8) | ((ch >> 8) & 0x00FF00FFu) : ch;
            else {
                int msh = int(slots[s].msh), lo = slots[s].lo, hi = slots[s].hi;
                int ry = slots[s].ry, ru = slots[s].ru, rv = slots[s].rv, r3 = slots[s].r3;
                int gy = slots[s].gy, gu = slots[s].gu, gv = slots[s].gv, g3 = slots[s].g3;
                int by = slots[s].by, bu = slots[s].bu, bv = slots[s].bv, b3 = slots[s].b3;
                uint ush = slots[s].ush, vsh = slots[s].vsh;
                int aR, aG, aB, bR, bG, bB;
                VIC_TERMS(a, ch)
                VIC_TERMS(b, ch >> 16)
                /* G and B (U and V) from the pairs' even pixels of the first row */
                cc = (VIC_F(VIC_B(y0, 0u) * gy + aG, g3) << gpos) | (VIC_F(VIC_B(y0, 0u) * by + aB, b3) << bpos) |
                     (VIC_F(VIC_B(y0, 2u) * gy + bG, g3) << (gpos + 16u)) |
                     (VIC_F(VIC_B(y0, 2u) * by + bB, b3) << (bpos + 16u));
                y0 = VIC_F(VIC_B(y0, 0u) * ry + aR, r3) | (VIC_F(VIC_B(y0, 1u) * ry + aR, r3) << 8) |
                     (VIC_F(VIC_B(y0, 2u) * ry + bR, r3) << 16) | (VIC_F(VIC_B(y0, 3u) * ry + bR, r3) << 24);
                y1 = VIC_F(VIC_B(y1, 0u) * ry + aR, r3) | (VIC_F(VIC_B(y1, 1u) * ry + aR, r3) << 8) |
                     (VIC_F(VIC_B(y1, 2u) * ry + bR, r3) << 16) | (VIC_F(VIC_B(y1, 3u) * ry + bR, r3) << 24);
            }
            /* a pair's chroma comes from its even pixel's slot */
            l0 ^= (l0 ^ y0) & bytes(c & 15u);
            l1 ^= (l1 ^ y1) & bytes(c >> 4);
            c0 ^= (c0 ^ cc) & bytes((c & 5u) * 3u);
        }
        uint ox = xpart(job.ol, px);
        store(job.ol, ox + oyp, l0, bytes(m & 15u));
        store(job.ol, ox + oyq, l1, bytes(m >> 4));
        /* an unmapped chroma plane has lim 0: store writes nothing */
        store(job.oc, xpart(job.oc, px) + ypart(job.oc, py >> 1), c0, bytes((m & 5u) * 3u));
        return;
    }

    /* RGBA out: a word a pixel. A slot's source column or row may be odd here, so a block's pixels can span three
       chroma pairs and its rows two chroma rows */
    uint w0 = job.bgw, w1 = job.bgw, w2 = job.bgw, w3 = job.bgw, w4 = job.bgw, w5 = job.bgw, w6 = job.bgw,
         w7 = job.bgw;
    for (uint s = job.nslots; s-- > 0u && rem != 0u;) {
        uint c = cover(slots[s].dx0, slots[s].dx1, slots[s].dy0, slots[s].h, px, py) & rem;
        if (c == 0u) continue;
        rem &= ~c;
        uint sx = px + slots[s].ox, sy = py + slots[s].oy, yp = ypart(slots[s].iy, sy), odd = sx & 1u;
        uint y0 = read4(slots[s].iy, sx, yp), y1 = read4(slots[s].iy, sx, ynext(slots[s].iy, sy, yp));
        /* the chroma pairs from pixel 0's on: ca (and past it cd, for an odd column) for the first row, cb and ce
           for the second, the same row unless sy is odd */
        uint cx = sx & ~1u, cyp = ypart(slots[s].ic, sy >> 1);
        uint ca = read4(slots[s].ic, cx, cyp), cd = 0u;
        if (odd != 0u) cd = read4(slots[s].ic, cx + 4u, cyp);
        uint cb = ca, ce = cd;
        if ((sy & 1u) != 0u) {
            cyp = ypart(slots[s].ic, (sy + 1u) >> 1);
            cb = read4(slots[s].ic, cx, cyp);
            if (odd != 0u) ce = read4(slots[s].ic, cx + 4u, cyp);
        }
        uint aw = slots[s].aw, ush = slots[s].ush, vsh = slots[s].vsh, n0, n1, n2, n3, n4, n5, n6, n7;
        if (slots[s].mat == 0u) {
            /* pixel 1 takes pixel 0's pair, or the next one for an odd column; pixel 3 pixel 2's, or the one after */
            n0 = VIC_PIX(y0 & 0xFFu, ca);
            n1 = VIC_PIX((y0 >> 8) & 0xFFu, odd != 0u ? ca >> 16 : ca);
            n2 = VIC_PIX((y0 >> 16) & 0xFFu, ca >> 16);
            n3 = VIC_PIX(y0 >> 24, odd != 0u ? cd : ca >> 16);
            n4 = VIC_PIX(y1 & 0xFFu, cb);
            n5 = VIC_PIX((y1 >> 8) & 0xFFu, odd != 0u ? cb >> 16 : cb);
            n6 = VIC_PIX((y1 >> 16) & 0xFFu, cb >> 16);
            n7 = VIC_PIX(y1 >> 24, odd != 0u ? ce : cb >> 16);
        } else {
            int msh = int(slots[s].msh), lo = slots[s].lo, hi = slots[s].hi;
            int ry = slots[s].ry, ru = slots[s].ru, rv = slots[s].rv, r3 = slots[s].r3;
            int gy = slots[s].gy, gu = slots[s].gu, gv = slots[s].gv, g3 = slots[s].g3;
            int by = slots[s].by, bu = slots[s].bu, bv = slots[s].bv, b3 = slots[s].b3;
            /* the terms of the first row's pairs: a pixel 0's, c pixel 2's, d pixel 3's, b pixel 1's; then the
               second row's, e to h, computed again only when it has its own chroma row */
            int aR, aG, aB, bR, bG, bB, cR, cG, cB, dR, dG, dB;
            VIC_TERMS(a, ca)
            VIC_TERMS(c, ca >> 16)
            dR = cR, dG = cG, dB = cB;
            if (odd != 0u) VIC_TERMS(d, cd)
            bR = odd != 0u ? cR : aR, bG = odd != 0u ? cG : aG, bB = odd != 0u ? cB : aB;
            n0 = VIC_MAT(VIC_B(y0, 0u), a);
            n1 = VIC_MAT(VIC_B(y0, 1u), b);
            n2 = VIC_MAT(VIC_B(y0, 2u), c);
            n3 = VIC_MAT(VIC_B(y0, 3u), d);
            if ((sy & 1u) != 0u) {
                VIC_TERMS(a, cb)
                VIC_TERMS(c, cb >> 16)
                dR = cR, dG = cG, dB = cB;
                if (odd != 0u) VIC_TERMS(d, ce)
                bR = odd != 0u ? cR : aR, bG = odd != 0u ? cG : aG, bB = odd != 0u ? cB : aB;
            }
            n4 = VIC_MAT(VIC_B(y1, 0u), a);
            n5 = VIC_MAT(VIC_B(y1, 1u), b);
            n6 = VIC_MAT(VIC_B(y1, 2u), c);
            n7 = VIC_MAT(VIC_B(y1, 3u), d);
        }
        w0 = (c & 1u) != 0u ? n0 : w0;
        w1 = (c & 2u) != 0u ? n1 : w1;
        w2 = (c & 4u) != 0u ? n2 : w2;
        w3 = (c & 8u) != 0u ? n3 : w3;
        w4 = (c & 16u) != 0u ? n4 : w4;
        w5 = (c & 32u) != 0u ? n5 : w5;
        w6 = (c & 64u) != 0u ? n6 : w6;
        w7 = (c & 128u) != 0u ? n7 : w7;
    }
    /* the block's four words of a row are 16 contiguous bytes in both layouts (px * 4 is a multiple of 16), and a
       pixel is the whole word: owned ones are stored without a read */
    uint ox = xpart(job.ol, px << 2), a = ox + oyp, b = ox + oyq;
    store(job.ol, a, w0, 0u - (m & 1u));
    store(job.ol, a + 4u, w1, 0u - ((m >> 1) & 1u));
    store(job.ol, a + 8u, w2, 0u - ((m >> 2) & 1u));
    store(job.ol, a + 12u, w3, 0u - ((m >> 3) & 1u));
    store(job.ol, b, w4, 0u - ((m >> 4) & 1u));
    store(job.ol, b + 4u, w5, 0u - ((m >> 5) & 1u));
    store(job.ol, b + 8u, w6, 0u - ((m >> 6) & 1u));
    store(job.ol, b + 12u, w7, 0u - (m >> 7));
}

#undef VIC_F
#undef VIC_B
#undef VIC_TERMS
#undef VIC_PIX
#undef VIC_MAT
