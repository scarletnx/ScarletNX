/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ETC2 and EAC encoding: texconv.comp built with SNX_TC_ETC2 (vk/texconv.c runs it) decodes a BCn block with
   texconv.glsl's decoders and encodes it again as ETC2 or EAC, for a GPU that samples ETC2 but not BCn (Mali, Adreno's
   own driver, PowerVR): the same bytes a texel as the game's blocks, where decoding them takes four to eight times
   that. BC1 becomes ETC2 RGB8A1 (punch-through alpha holds BC1's transparent black), BC2, BC3 and BC7 ETC2 RGBA8 (an
   EAC alpha block, then an ETC2 RGB one), BC4 and BC5 EAC R11 and RG11, unsigned or signed as they are. BC6H has no
   ETC2 counterpart and is decoded as before.

   ETC2 RGB: every block is tried in the modes that suit it and the one whose decoded texels are nearest the block's
   (squared error, summed) is written. Each candidate's builder works out that error as the decoder decodes (clamped
   to 0 and 255 alike), which tests/glsl/bcn_etc2_glsl.cpp holds to the reference decoder's. The candidates: ETC1's
   differential (5 bit colours and a 3 bit difference) and, where that difference can't hold the two subblocks,
   individual (two 4 bit colours) modes, each subblock split side by side and one above the other, each subblock's
   base colour and table the pair of the eight tables that brings its texels nearest (et1_build); the H mode, two 4 bit
   colours each with a distance added and taken away, and the T mode, one colour alone and one with the distance
   either way, both from the two clusters the block's texels fall in (2-means from its two farthest texels); and the
   planar mode, three colours at the corners interpolated, a least squares fit of the texels, what gradients hold.
   A block with transparent texels (BC1's three colour blocks) clears its opaque bit: index 2 is then transparent, and
   only the differential, T and H modes are tried, every transparent texel taking index 2 and no opaque one.

   EAC: each of the sixteen tables with the multipliers around the one that spans the block's values, the base that
   centres it, and the best of them with the base a step either way, each texel taking its nearest modifier. An 8 bit
   alpha block never takes the multiplier 0, which the 11 bit blocks read as a finer step.

   Integer arithmetic only, so tests/glsl/bcn_etc2_glsl.cpp runs this file on the CPU (tests/glsl/glsl.hpp) and gets
   the GPU's bytes. As in bc7_encode.glsl, each body is called from one place (glslang inlines every call): the
   candidates are one loop, each built by et_build. */

/* the block's texels are texconv.glsl's tx_px, RGBA8 words (R in the low byte), row by row: texel (x, y) is y * 4 + x.
   An EAC block's values (an 8 bit alpha, or an 11 bit red or green, signed for the snorm kinds) are tx_v, in the
   same order */
int tx_v[16];
/* the block being built: bits 63 to 32 and 31 to 0, as the specification numbers them */
uint tx_hi;
uint tx_lo;

/* ---- ETC2 RGB ---- */

/* the two clusters the T and H modes start from: their means as 4 bit colours, R G B a nibble each (R the top) */
uint et_ta;
uint et_tb;
/* the error of the candidate et_build made last, its texels' squared error summed as the decoder will decode it */
uint et_err;
/* the best candidate so far and its error */
uint et_best_hi;
uint et_best_lo;
uint et_best_err;

int et_ch(uint p, uint c) { return int((p >> (8u * c)) & 255u); }
/* texel i is transparent (an opaque bit's block only: BC1 gives 0 or 255) */
bool et_clear(uint i) { return (tx_px[i] >> 24) < 128u; }
/* the squared distance of texel i's colour from (r, g, b), each clamped to 0 to 255 first as the decoder does */
uint et_dist(uint i, int r, int g, int b) {
    uint p = tx_px[i];
    int dr = clamp(r, 0, 255) - et_ch(p, 0u), dg = clamp(g, 0, 255) - et_ch(p, 1u);
    int db = clamp(b, 0, 255) - et_ch(p, 2u);
    return uint(dr * dr + dg * dg + db * db);
}
/* texel i's place in the index bits: ETC2 numbers its texels down the columns */
uint et_pos(uint i) { return (i & 3u) * 4u + (i >> 2); }
/* the subblock of texel i: side by side (flip 0), or one above the other */
uint et_sub(uint flip, uint i) { return (i >> (flip != 0u ? 3u : 1u)) & 1u; } /* i is below 16 */
/* index k (0 to 3) of texel i in the index bits */
uint et_index_bits(uint i, uint k) {
    uint p = et_pos(i);
    return ((k >> 1) << (16u + p)) | ((k & 1u) << p);
}
/* a channel's mean over n texels (sum their sum) quantized to top + 1 levels, rounded */
int et_quant(int sum, int n, int top) { return (sum * top * 2 + n * 255) / (n * 510); }
/* 12 bit colours of three 4 bit channels: each channel expanded */
int et_c4(uint c, uint ch) { return tc_ext4((c >> (8u - 4u * ch)) & 15u); }

/* ETC1's modifier of index k of table t. see: a see-through punch-through block, whose index 0 adds nothing (its index
   2 is transparent and never asked for) */
int et1_mod(uint t, uint k, bool see) {
    int a = tc_byte(ETC1_A0, ETC1_A1, t), b = tc_byte(ETC1_B0, ETC1_B1, t);
    int m = k == 0u ? a : k == 1u ? b : k == 2u ? -a : -b;
    return see && k == 0u ? 0 : m;
}
/* the index of texel i nearest base (r, g, b) with table t's modifiers; 2 for a transparent texel of a see-through
   block. Its error is et_dist of that index's colour */
uint et1_index(uint i, int r, int g, int b, uint t, bool see) {
    if (see && et_clear(i)) return 2u;
    uint best = 0xFFFFFFFFu, bk = 0u;
    for (uint k = 0u; k < 4u; k++) {
        if (see && k == 2u) continue;
        int m = et1_mod(t, k, see);
        uint d = et_dist(i, r + m, g + m, b + m);
        if (d < best) {
            best = d;
            bk = k;
        }
    }
    return bk;
}
/* the table of eight whose modifiers bring subblock s's opaque texels nearest base (r, g, b), and its indices for
   them in the index bits (in et_best_lo's place: tx_lo) */
uint et1_table(uint flip, uint s, int r, int g, int b, bool see) {
    uint best = 0xFFFFFFFFu;
    for (uint t = 0u; t < 8u; t++) {
        uint e = 0u;
        for (uint i = 0u; i < 16u; i++) {
            if (et_sub(flip, i) != s || (see && et_clear(i))) continue;
            uint k = et1_index(i, r, g, b, t, see);
            int m = et1_mod(t, k, see);
            e += et_dist(i, r + m, g + m, b + m);
        }
        /* the error is below 2^21 (8 texels of 3 channels of 255 squared): the table in the bits below it */
        best = min(best, (e << 3) | t);
    }
    return best & 7u;
}
/* whether the differential mode's 3 bit difference holds the 5 bit means of flip's two subblocks: if so the
   individual mode (two 4 bit colours, for subblocks too far apart for that) isn't tried */
bool et1_diff_holds(uint flip) {
    int sum[6];
    for (uint k = 0u; k < 6u; k++) sum[k] = 0;
    for (uint i = 0u; i < 16u; i++)
        for (uint c = 0u; c < 3u; c++) sum[et_sub(flip, i) * 3u + c] += et_ch(tx_px[i], c);
    bool holds = true;
    for (uint c = 0u; c < 3u; c++) {
        int d = et_quant(sum[3u + c], 8, 31) - et_quant(sum[c], 8, 31);
        holds = holds && d >= -4 && d <= 3;
    }
    return holds;
}
/* ETC1's individual (diff false) or differential mode with its subblocks split as flip says, into tx_hi, tx_lo and
   et_err. Each subblock starts from its texels' mean; then each table takes the base its indices from there want
   (each texel less the modifier its index adds, the mean of that), and the table and base that bring the texels
   nearest are kept: a table's modifiers are seldom used evenly, so the mean is off by what they add. A subblock of
   transparent texels alone takes the other's colour, and the differential mode's second colour is held within the 3
   bit difference it stores, its table chosen again for it then. In a punch-through block the differential bit is the
   opaque bit, see a see-through one (et_build clears it) */
void et1_build(uint flip, bool diff, bool see) {
    int top = diff ? 31 : 15;
    int q[6];
    int n[2];
    uint tab[2];
    for (uint s = 0u; s < 2u; s++) {
        n[s] = 0;
        int sum[3];
        sum[0] = 0;
        sum[1] = 0;
        sum[2] = 0;
        for (uint i = 0u; i < 16u; i++) {
            if (et_sub(flip, i) != s || (see && et_clear(i))) continue;
            n[s]++;
            for (uint c = 0u; c < 3u; c++) sum[c] += et_ch(tx_px[i], c);
        }
        tab[s] = 0u;
        for (uint c = 0u; c < 3u; c++) q[s * 3u + c] = 0;
        if (n[s] == 0) continue;
        int m0[3];
        for (uint c = 0u; c < 3u; c++)
            m0[c] = diff ? tc_ext5(uint(et_quant(sum[c], n[s], top))) : tc_ext4(uint(et_quant(sum[c], n[s], top)));
        uint best = 0xFFFFFFFFu;
        for (uint t = 0u; t < 8u; t++) {
            int l[3];
            l[0] = 0;
            l[1] = 0;
            l[2] = 0;
            for (uint i = 0u; i < 16u; i++) {
                if (et_sub(flip, i) != s || (see && et_clear(i))) continue;
                int m = et1_mod(t, et1_index(i, m0[0], m0[1], m0[2], t, see), see);
                for (uint c = 0u; c < 3u; c++) l[c] += et_ch(tx_px[i], c) - m;
            }
            for (uint c = 0u; c < 3u; c++) l[c] = clamp(et_quant(max(l[c], 0), n[s], top), 0, top);
            int r = diff ? tc_ext5(uint(l[0])) : tc_ext4(uint(l[0]));
            int g = diff ? tc_ext5(uint(l[1])) : tc_ext4(uint(l[1]));
            int b = diff ? tc_ext5(uint(l[2])) : tc_ext4(uint(l[2]));
            uint e = 0u;
            for (uint i = 0u; i < 16u; i++) {
                if (et_sub(flip, i) != s || (see && et_clear(i))) continue;
                int m = et1_mod(t, et1_index(i, r, g, b, t, see), see);
                e += et_dist(i, r + m, g + m, b + m);
            }
            if (e < best) {
                best = e;
                tab[s] = t;
                for (uint c = 0u; c < 3u; c++) q[s * 3u + c] = l[c];
            }
        }
    }
    bool moved = false;
    for (uint c = 0u; c < 3u; c++) {
        if (n[0] == 0) q[c] = q[3u + c];
        if (n[1] == 0) q[3u + c] = q[c];
        if (diff) {
            int d = clamp(q[3u + c] - q[c], -4, 3);
            moved = moved || q[c] + d != q[3u + c];
            q[3u + c] = q[c] + d;
        }
    }
    tx_hi = flip | (diff ? 2u : 0u);
    tx_lo = 0u;
    et_err = 0u;
    for (uint s = 0u; s < 2u; s++) {
        int r = diff ? tc_ext5(uint(q[s * 3u])) : tc_ext4(uint(q[s * 3u]));
        int g = diff ? tc_ext5(uint(q[s * 3u + 1u])) : tc_ext4(uint(q[s * 3u + 1u]));
        int b = diff ? tc_ext5(uint(q[s * 3u + 2u])) : tc_ext4(uint(q[s * 3u + 2u]));
        /* the second subblock's colour held in the difference wants its table chosen again */
        uint t = s == 1u && moved ? et1_table(flip, s, r, g, b, see) : tab[s];
        tx_hi |= t << (s == 0u ? 5u : 2u);
        for (uint i = 0u; i < 16u; i++) {
            if (et_sub(flip, i) != s) continue;
            uint k = et1_index(i, r, g, b, t, see);
            tx_lo |= et_index_bits(i, k);
            int m = et1_mod(t, k, see);
            if (!(see && et_clear(i))) et_err += et_dist(i, r + m, g + m, b + m);
        }
    }
    if (diff) {
        tx_hi |= (uint(q[0]) << 27) | (uint(q[3] - q[0]) & 7u) << 24 | (uint(q[1]) << 19) |
                 (uint(q[4] - q[1]) & 7u) << 16 | (uint(q[2]) << 11) | (uint(q[5] - q[2]) & 7u) << 8;
    } else {
        tx_hi |= (uint(q[0]) << 28) | (uint(q[3]) << 24) | (uint(q[1]) << 20) | (uint(q[4]) << 16) |
                 (uint(q[2]) << 12) | (uint(q[5]) << 8);
    }
}

/* the two clusters of the block's opaque texels for the T and H modes: from its two farthest texels, each texel to
   the nearer mean, twice; their means as 4 bit colours into et_ta and et_tb */
void et_cluster(bool see) {
    uint far = 0u, i0 = 0u, i1 = 0u;
    for (uint i = 0u; i < 16u; i++) {
        if (see && et_clear(i)) continue;
        if (far == 0u) i0 = i1 = i; /* a block of one colour keeps them equal */
        for (uint j = i + 1u; j < 16u; j++) {
            if (see && et_clear(j)) continue;
            uint d = et_dist(i, et_ch(tx_px[j], 0u), et_ch(tx_px[j], 1u), et_ch(tx_px[j], 2u));
            if (d > far) {
                far = d;
                i0 = i;
                i1 = j;
            }
        }
    }
    int c[6];
    for (uint ch = 0u; ch < 3u; ch++) {
        c[ch] = et_ch(tx_px[i0], ch);
        c[3u + ch] = et_ch(tx_px[i1], ch);
    }
    int sum[6];
    int n0 = 0, n1 = 0;
    for (uint round = 0u; round < 2u; round++) {
        for (uint k = 0u; k < 6u; k++) sum[k] = 0;
        n0 = 0;
        n1 = 0;
        for (uint i = 0u; i < 16u; i++) {
            if (see && et_clear(i)) continue;
            bool second = et_dist(i, c[3], c[4], c[5]) < et_dist(i, c[0], c[1], c[2]);
            for (uint ch = 0u; ch < 3u; ch++) sum[(second ? 3u : 0u) + ch] += et_ch(tx_px[i], ch);
            if (second)
                n1++;
            else
                n0++;
        }
        for (uint ch = 0u; ch < 3u; ch++) {
            if (n0 != 0) c[ch] = (sum[ch] + n0 / 2) / n0;
            if (n1 != 0) c[3u + ch] = (sum[3u + ch] + n1 / 2) / n1;
        }
    }
    et_ta = 0u;
    et_tb = 0u;
    for (uint ch = 0u; ch < 3u; ch++) {
        et_ta |= uint(et_quant(c[ch], 1, 15)) << (8u - 4u * ch);
        et_tb |= uint(et_quant(c[3u + ch], 1, 15)) << (8u - 4u * ch);
    }
}

/* the T mode's colours: index 0 the first base colour a, 1 to 3 the second b with the distance added, nothing, and
   taken away. In the H mode indices 0 and 1 are a with it added and taken away, 2 and 3 b */
int et_th_offset(bool h, uint k, int d) {
    if (h) return (k & 1u) != 0u ? -d : d;
    return k == 1u ? d : k == 3u ? -d : 0;
}
/* index k's colour channel ch of the T (h false) or H mode with base colours a and b (12 bits each) */
int et_th_colour(bool h, uint a, uint b, uint k, int d, uint ch) {
    bool first = h ? k < 2u : k == 0u;
    return et_c4(first ? a : b, ch) + (first && !h ? 0 : et_th_offset(h, k, d));
}
/* the nearest index of texel i in the T or H mode, its transparent ones index 2 */
uint et_th_index(uint i, bool h, uint a, uint b, int d, bool see) {
    if (see && et_clear(i)) return 2u;
    uint best = 0xFFFFFFFFu, bk = 0u;
    for (uint k = 0u; k < 4u; k++) {
        if (see && k == 2u) continue;
        uint e = et_dist(i, et_th_colour(h, a, b, k, d, 0u), et_th_colour(h, a, b, k, d, 1u),
                         et_th_colour(h, a, b, k, d, 2u));
        if (e < best) {
            best = e;
            bk = k;
        }
    }
    return bk;
}
/* the T mode with base colour a alone (swap: the clusters the other way round), or the H mode, into tx_hi, tx_lo and
   et_err. Its distance is the
   one of eight that brings the opaque texels nearest. The H mode stores its distance's low bit as which colour is the
   larger, so each distance takes its colours in the order that says it, and an even one isn't open to two equal
   colours */
void et_th_build(bool h, bool swap, bool see) {
    uint best = 0xFFFFFFFFu, ba = 0u, bb = 0u, bdi = 0u;
    for (uint di = 0u; di < 8u; di++) {
        uint a = swap ? et_tb : et_ta, b = swap ? et_ta : et_tb;
        if (h) {
            uint big = max(et_ta, et_tb), small = min(et_ta, et_tb);
            if ((di & 1u) == 0u && big == small) continue;
            a = (di & 1u) != 0u ? big : small;
            b = (di & 1u) != 0u ? small : big;
        }
        int d = tc_byte(ETC2_D0, ETC2_D1, di);
        uint e = 0u;
        for (uint i = 0u; i < 16u; i++) {
            if (see && et_clear(i)) continue;
            uint k = et_th_index(i, h, a, b, d, see);
            e += et_dist(i, et_th_colour(h, a, b, k, d, 0u), et_th_colour(h, a, b, k, d, 1u),
                         et_th_colour(h, a, b, k, d, 2u));
        }
        if (e < best) {
            best = e;
            ba = a;
            bb = b;
            bdi = di;
        }
    }
    int d = tc_byte(ETC2_D0, ETC2_D1, bdi);
    et_err = best;
    tx_lo = 0u;
    for (uint i = 0u; i < 16u; i++) tx_lo |= et_index_bits(i, et_th_index(i, h, ba, bb, d, see));
    uint r1 = ba >> 8, g1 = (ba >> 4) & 15u, b1 = ba & 15u, r2 = bb >> 8, g2 = (bb >> 4) & 15u, b2 = bb & 15u;
    /* bit 33, the differential (or opaque) bit, set: et_build clears it for a see-through block */
    tx_hi = 2u;
    if (h) {
        /* R (bits 63 to 59) with its difference (58 to 56, g1's top) kept in range; G (55 to 51) and its difference
           (50 to 48) made to overflow by their free bits, 55 to 53 and 50, whichever way they can */
        tx_hi |= (((g1 >> 3) & 1u) << 31) | (r1 << 27) | ((g1 >> 1) << 24) | ((g1 & 1u) << 20) | ((b1 >> 3) << 19) |
                ((b1 & 7u) << 15) | (r2 << 11) | (g2 << 7) | (b2 << 3) | ((bdi >> 2) << 2) | ((bdi >> 1) & 1u);
        uint ga = ((g1 & 1u) << 1) | (b1 >> 3), gc = (b1 >> 1) & 3u;
        tx_hi |= ga + gc <= 3u ? 1u << 18 : 7u << 21;
    } else {
        /* R (bits 63 to 59) and its difference (58 to 56) made to overflow by their free bits, 63 to 61 and 58 */
        tx_hi |= ((r1 >> 2) << 27) | ((r1 & 3u) << 24) | (g1 << 20) | (b1 << 16) | (r2 << 12) | (g2 << 8) | (b2 << 4) |
                ((bdi >> 1) << 2) | (bdi & 1u);
        tx_hi |= (r1 >> 2) + (r1 & 3u) <= 3u ? 1u << 26 : 7u << 29;
    }
}

/* the planar mode: the colours at the corners (0, 0), (4, 0) and (0, 4), 6, 7 and 6 bits a channel. A channel's
   decoded values depend on its own three alone, so each channel starts from the texels' least squares fit and moves
   to the best of its three a level either way (27 tries), up to four times while that is better: the fit is of values
   the decoder rounds, the levels round them again, and a gradient the decoder clamps (the corner at (3, 3) lies past
   the three, so it can pass 255) pulls the fit off by more than a level. The fit's weights are the normal equations'
   inverse over the 4x4 grid, in 80ths: the origin's 23 - 6x - 6y, the horizontal corner's 10x - 6y - 1, the vertical
   one's 10y - 6x - 1. Into tx_hi, tx_lo and et_err */
void et_planar_build() {
    int o[3];
    int hz[3];
    int vt[3];
    et_err = 0u;
    for (uint ch = 0u; ch < 3u; ch++) {
        int so = 0, sh = 0, sv = 0;
        for (uint i = 0u; i < 16u; i++) {
            int x = int(i & 3u), y = int(i >> 2), p = et_ch(tx_px[i], ch);
            so += (23 - 6 * x - 6 * y) * p;
            sh += (10 * x - 6 * y - 1) * p;
            sv += (10 * y - 6 * x - 1) * p;
        }
        int top = ch == 1u ? 127 : 63;
        /* the fit over 80 quantized to top + 1 levels; a negative one is 0 whichever way it rounds */
        int q0 = clamp((so * top * 2 + 80 * 255) / (80 * 510), 0, top);
        int q1 = clamp((sh * top * 2 + 80 * 255) / (80 * 510), 0, top);
        int q2 = clamp((sv * top * 2 + 80 * 255) / (80 * 510), 0, top);
        uint best = 0xFFFFFFFFu;
        o[ch] = q0;
        hz[ch] = q1;
        vt[ch] = q2;
        for (int k = 0; k < 4 * 27; k++) {
            if (k % 27 == 0 && k != 0) {
                /* the pass's best in the middle: no level nearby is better */
                if (o[ch] == q0 && hz[ch] == q1 && vt[ch] == q2) break;
                q0 = o[ch];
                q1 = hz[ch];
                q2 = vt[ch];
            }
            int qo = clamp(q0 + k % 3 - 1, 0, top), qh = clamp(q1 + (k / 3) % 3 - 1, 0, top);
            int qv = clamp(q2 + (k / 9) % 3 - 1, 0, top);
            int eo = ch == 1u ? tc_ext7(uint(qo)) : tc_ext6(uint(qo));
            int eh = ch == 1u ? tc_ext7(uint(qh)) : tc_ext6(uint(qh));
            int ev = ch == 1u ? tc_ext7(uint(qv)) : tc_ext6(uint(qv));
            uint e = 0u;
            for (uint i = 0u; i < 16u; i++) {
                int x = int(i & 3u), y = int(i >> 2);
                int d = clamp((x * (eh - eo) + y * (ev - eo) + 4 * eo + 2) >> 2, 0, 255) - et_ch(tx_px[i], ch);
                e += uint(d * d);
            }
            if (e < best) {
                best = e;
                o[ch] = qo;
                hz[ch] = qh;
                vt[ch] = qv;
            }
        }
        et_err += best;
    }
    uint o0 = uint(o[0]), o1 = uint(o[1]), o2 = uint(o[2]);
    /* R and G (bits 63 to 59, 55 to 51) kept from overflowing by their free bits, 63 and 55, the signs of their
       differences; B (47 to 43) and its difference made to overflow by theirs, 47 to 45 and 42 */
    tx_hi = (((o0 >> 1) & 1u) << 31) | (o0 << 25) | ((o1 >> 6) << 24) | (((o1 >> 1) & 1u) << 23) |
            ((o1 & 63u) << 17) | ((o2 >> 5) << 16) | (((o2 >> 3) & 3u) << 11) | ((o2 & 7u) << 7) |
            ((uint(hz[0]) >> 1) << 2) | 2u | (uint(hz[0]) & 1u);
    tx_hi |= ((o2 >> 3) & 3u) + ((o2 >> 1) & 3u) <= 3u ? 1u << 10 : 7u << 13;
    tx_lo = (uint(hz[1]) << 25) | (uint(hz[2]) << 19) | (uint(vt[0]) << 13) | (uint(vt[1]) << 6) | uint(vt[2]);
}

/* candidate c into tx_hi, tx_lo and et_err: 0 and 1 individual, 2 and 3 differential (flips 0 and 1), 4 H, 5 and 6 T,
   7 planar. punch: an RGB8A1 block, whose differential bit (bit 33) is the opaque bit, clear when see */
void et_build(uint c, bool punch, bool see) {
    if (c < 4u)
        et1_build(c & 1u, c >= 2u, see);
    else if (c < 7u)
        et_th_build(c == 4u, c == 6u, see);
    else
        et_planar_build();
    if (punch && see) tx_hi &= ~2u;
}
/* tx_px's colours as an ETC2 RGB block (punch: RGB8A1, opaque bit and all) into tx_hi and tx_lo */
void et_encode(bool punch) {
    bool see = false, any = false;
    if (punch)
        for (uint i = 0u; i < 16u; i++) {
            if (et_clear(i))
                see = true;
            else
                any = true;
        }
    if (punch && !any) {
        /* every texel transparent: a see-through differential block whose indices are all 2 */
        tx_hi = 0u;
        tx_lo = 0xFFFF0000u;
        return;
    }
    et_cluster(see);
    et_best_err = 0xFFFFFFFFu;
    et_best_hi = 0u;
    et_best_lo = 0u;
    for (uint c = 0u; c < 8u && et_best_err != 0u; c++) {
        /* punch-through has no individual mode (its bit is the opaque bit), nor a transparent texel in the planar
           mode; the individual mode is for subblocks the differential one can't hold */
        if ((c < 2u && (punch || et1_diff_holds(c))) || (see && c == 7u)) continue;
        et_build(c, punch, see);
        if (et_err < et_best_err) {
            et_best_err = et_err;
            et_best_hi = tx_hi;
            et_best_lo = tx_lo;
        }
    }
    tx_hi = et_best_hi;
    tx_lo = et_best_lo;
}

/* ---- EAC ---- */

/* the kinds of EAC block: 8 bit alpha (ETC2 RGBA8's), and 11 bit unsigned and signed */
#define EX_ALPHA 0u
#define EX_R11 1u
#define EX_R11S 2u

/* the modifier of index k of table t (texconv.glsl's eac_mod) */
int ex_mod(uint t, uint k) {
    int p = int((EAC_P[t >> 1] >> ((t & 1u) * 16u + (k & 3u) * 4u)) & 15u);
    return k < 4u ? -p - 1 : p;
}
/* the value base, multiplier m and modifier mod decode to, in the kind's units (as eac_alpha and eac11 decode) */
int ex_value(uint kind, int base, int m, int mod) {
    if (kind == EX_ALPHA) return clamp(base + mod * m, 0, 255);
    int step = m != 0 ? mod * m * 8 : mod;
    if (kind == EX_R11) return clamp(base * 8 + 4 + step, 0, 2047);
    return clamp(base * 8 + step, -1023, 1023);
}
/* the nearest index of value v */
uint ex_index(uint kind, int base, int m, uint t, int v) {
    uint best = 0xFFFFFFFFu, bk = 0u;
    for (uint k = 0u; k < 8u; k++) {
        int d = ex_value(kind, base, m, ex_mod(t, k)) - v;
        if (uint(d * d) < best) {
            best = uint(d * d);
            bk = k;
        }
    }
    return bk;
}
/* n / d rounded to nearest, halves away from 0 (d > 0) */
int ex_round(int n, int d) { return n >= 0 ? (n + d / 2) / d : -((d / 2 - n) / d); }
/* tx_v as an EAC block of kind into tx_hi and tx_lo */
void ex_encode(uint kind) {
    int lo = tx_v[0], hi = tx_v[0];
    for (uint i = 1u; i < 16u; i++) {
        lo = min(lo, tx_v[i]);
        hi = max(hi, tx_v[i]);
    }
    if (kind == EX_ALPHA && lo == hi) {
        /* one alpha, an opaque block's most often: table 13's index 4 adds nothing to the base */
        tx_hi = (uint(lo) << 24) | (1u << 20) | (13u << 16) | 0x9249u;
        tx_lo = 0x24924924u;
        return;
    }
    int unit = kind == EX_ALPHA ? 1 : 8, off = kind == EX_R11 ? 4 : 0;
    int bmin = kind == EX_R11S ? -127 : 0, bmax = kind == EX_R11S ? 127 : 255;
    uint best = 0xFFFFFFFFu;
    int bb = 0, bm = 1;
    uint bt = 0u;
    /* candidates 0 to 31: table c / 2, the multiplier that spans the values or the one above; 32 and 33: the best
       with its base a step down and up */
    for (uint c = 0u; c < 34u && best != 0u; c++) {
        uint t = c < 32u ? c >> 1 : bt;
        int m = bm, base = bb + (c == 32u ? -1 : 1);
        if (c < 32u) {
            int tmin = ex_mod(t, 3u), tmax = ex_mod(t, 7u);
            m = clamp((hi - lo) / ((tmax - tmin) * unit) + int(c & 1u), kind == EX_ALPHA ? 1 : 0, 15);
            /* the base that puts the table's middle at the values' middle */
            int mid = m != 0 ? (tmin + tmax) * m * unit : tmin + tmax;
            base = ex_round(lo + hi - 2 * off - mid, 2 * unit);
        }
        base = clamp(base, bmin, bmax);
        uint e = 0u;
        for (uint i = 0u; i < 16u; i++) {
            int d = ex_value(kind, base, m, ex_mod(t, ex_index(kind, base, m, t, tx_v[i]))) - tx_v[i];
            e += uint(d * d);
        }
        if (e < best) {
            best = e;
            bb = base;
            bm = m;
            bt = t;
        }
    }
    tx_hi = ((uint(bb) & 255u) << 24) | (uint(bm) << 20) | (bt << 16);
    tx_lo = 0u;
    for (uint i = 0u; i < 16u; i++) {
        /* three bits a texel, down the columns from bit 47 */
        uint k = ex_index(kind, bb, bm, bt, tx_v[i]), p = 45u - 3u * et_pos(i);
        if (p >= 32u) {
            tx_hi |= k << (p - 32u);
        } else {
            tx_lo |= k << p;
            if (p > 29u) tx_hi |= k >> (32u - p);
        }
    }
}
