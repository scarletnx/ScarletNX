/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/textures/astc.cpp, astc.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
   SPDX-FileCopyrightText: 2016 The University of North Carolina at Chapel Hill
   The University of North Carolina at Chapel Hill's part (FasTC's decoder, http://gamma.cs.unc.edu/FasTC/):
   SPDX-License-Identifier: Apache-2.0 */
/* ASTC decoding on the CPU, the reference for the compute shader astc.comp. It follows Eden's CPU decoder, and
   differs from it where Eden's shader and CPU decoder disagree, so that both give the same bytes: a block whose
   mode and weights leave no room for its color data is an error (as in the shader), and texels are converted to
   8 bits the way astc.comp does it (as a UNORM16 or half value rounded to nearest), in integers. */
#include "nx/gpu/texture/astc.h"
#include <string.h>

/* a little-endian bit stream over one 16 byte block: bits past the end read as zero */
typedef struct {
    const u8* data;
    u32 pos;
} BitIn;

static u32 bits_read(BitIn* s, u32 n) {
    u32 r = 0, i;
    for (i = 0; i < n; i++, s->pos++)
        if (s->pos < 128) r |= (u32)((s->data[s->pos >> 3] >> (s->pos & 7)) & 1) << i;
    return r;
}

/* writes n bits at the stream's position into a zeroed 16 byte buffer */
static void bits_write(u8* data, u32* pos, u32 v, u32 n) {
    u32 i;
    for (i = 0; i < n && *pos < 128; i++, (*pos)++)
        if ((v >> i) & 1) data[*pos >> 3] |= (u8)(1u << (*pos & 7));
}

enum { ISE_BITS, ISE_QUINT, ISE_TRIT };

/* one value of a bounded integer sequence */
typedef struct {
    u32 encoding, num_bits, bit_value, tq; /* tq: the trit or quint */
} IseValue;

/* enough for 64 weights or 32 color values, and the last trit or quint block's spare values */
#define ISE_MAX 72

typedef struct {
    IseValue v[ISE_MAX];
    u32 n;
} IseList;

static u32 popcount(u32 v) {
    u32 n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

/* the encoding of a sequence whose values are at most max_value */
static IseValue ise_encoding(u32 max_value) {
    IseValue e;
    memset(&e, 0, sizeof e);
    while (max_value > 0) {
        u32 check = max_value + 1;
        if (!(check & (check - 1))) {
            e.encoding = ISE_BITS;
            e.num_bits = popcount(max_value);
            return e;
        }
        if (check % 3 == 0 && !((check / 3) & ((check / 3) - 1))) {
            e.encoding = ISE_TRIT;
            e.num_bits = popcount(check / 3 - 1);
            return e;
        }
        if (check % 5 == 0 && !((check / 5) & ((check / 5) - 1))) {
            e.encoding = ISE_QUINT;
            e.num_bits = popcount(check / 5 - 1);
            return e;
        }
        max_value--;
    }
    return e;
}

static u32 ise_bit_length(IseValue e, u32 n) {
    u32 total = e.num_bits * n;
    if (e.encoding == ISE_TRIT)
        total += (n * 8 + 4) / 5;
    else if (e.encoding == ISE_QUINT)
        total += (n * 7 + 2) / 3;
    return total;
}

static void ise_push(IseList* l, u32 encoding, u32 num_bits, u32 bit_value, u32 tq) {
    if (l->n >= ISE_MAX) return;
    l->v[l->n].encoding = encoding;
    l->v[l->n].num_bits = num_bits;
    l->v[l->n].bit_value = bit_value;
    l->v[l->n].tq = tq;
    l->n++;
}

static u32 bit(u32 v, u32 pos) { return (v >> pos) & 1; }
static u32 field(u32 v, u32 start, u32 end) { return (v >> start) & ((1u << (end - start + 1)) - 1); }

/* five values from a trit block (the ASTC specification's integer sequence encoding) */
static void decode_trit_block(BitIn* s, IseList* l, u32 nb) {
    u32 m[5], t[5], T, C, i;
    m[0] = bits_read(s, nb);
    T = bits_read(s, 2);
    m[1] = bits_read(s, nb);
    T |= bits_read(s, 2) << 2;
    m[2] = bits_read(s, nb);
    T |= bits_read(s, 1) << 4;
    m[3] = bits_read(s, nb);
    T |= bits_read(s, 2) << 5;
    m[4] = bits_read(s, nb);
    T |= bits_read(s, 1) << 7;
    if (field(T, 2, 4) == 7) {
        C = (field(T, 5, 7) << 2) | field(T, 0, 1);
        t[4] = t[3] = 2;
    } else {
        C = field(T, 0, 4);
        if (field(T, 5, 6) == 3) {
            t[4] = 2;
            t[3] = bit(T, 7);
        } else {
            t[4] = bit(T, 7);
            t[3] = field(T, 5, 6);
        }
    }
    if (field(C, 0, 1) == 3) {
        t[2] = 2;
        t[1] = bit(C, 4);
        t[0] = (bit(C, 3) << 1) | (bit(C, 2) & ~bit(C, 3) & 1);
    } else if (field(C, 2, 3) == 3) {
        t[2] = 2;
        t[1] = 2;
        t[0] = field(C, 0, 1);
    } else {
        t[2] = bit(C, 4);
        t[1] = field(C, 2, 3);
        t[0] = (bit(C, 1) << 1) | (bit(C, 0) & ~bit(C, 1) & 1);
    }
    for (i = 0; i < 5; i++) ise_push(l, ISE_TRIT, nb, m[i], t[i]);
}

/* three values from a quint block */
static void decode_quint_block(BitIn* s, IseList* l, u32 nb) {
    u32 m[3], q[3], Q, C, i;
    m[0] = bits_read(s, nb);
    Q = bits_read(s, 3);
    m[1] = bits_read(s, nb);
    Q |= bits_read(s, 2) << 3;
    m[2] = bits_read(s, nb);
    Q |= bits_read(s, 2) << 5;
    if (field(Q, 1, 2) == 3 && field(Q, 5, 6) == 0) {
        q[0] = q[1] = 4;
        q[2] = (bit(Q, 0) << 2) | ((bit(Q, 4) & ~bit(Q, 0) & 1) << 1) | (bit(Q, 3) & ~bit(Q, 0) & 1);
    } else {
        if (field(Q, 1, 2) == 3) {
            q[2] = 4;
            C = (field(Q, 3, 4) << 3) | ((~field(Q, 5, 6) & 3) << 1) | bit(Q, 0);
        } else {
            q[2] = field(Q, 5, 6);
            C = field(Q, 0, 4);
        }
        if (field(C, 0, 2) == 5) {
            q[1] = 4;
            q[0] = field(C, 3, 4);
        } else {
            q[1] = field(C, 3, 4);
            q[0] = field(C, 0, 2);
        }
    }
    for (i = 0; i < 3; i++) ise_push(l, ISE_QUINT, nb, m[i], q[i]);
}

static void decode_integer_sequence(IseList* l, BitIn* s, u32 max_range, u32 count) {
    IseValue e = ise_encoding(max_range);
    u32 decoded = 0;
    l->n = 0;
    while (decoded < count) {
        if (e.encoding == ISE_QUINT) {
            decode_quint_block(s, l, e.num_bits);
            decoded += 3;
        } else if (e.encoding == ISE_TRIT) {
            decode_trit_block(s, l, e.num_bits);
            decoded += 5;
        } else {
            ise_push(l, ISE_BITS, e.num_bits, bits_read(s, e.num_bits), 0);
            decoded++;
        }
    }
}

/* the low num_bits of v repeated down from bit to_bit - 1 */
static u32 replicate(u32 v, u32 num_bits, u32 to_bit) {
    u32 res, reslen;
    if (num_bits == 0 || to_bit == 0) return 0;
    v &= (1u << num_bits) - 1;
    res = v;
    reslen = num_bits;
    while (reslen < to_bit) {
        u32 comp = 0;
        if (num_bits > to_bit - reslen) {
            u32 newshift = to_bit - reslen;
            comp = num_bits - newshift;
            num_bits = newshift;
        }
        res = (res << num_bits) | (v >> comp);
        reslen += num_bits;
    }
    return res;
}

typedef struct {
    u32 width, height; /* the weight grid */
    u32 max_weight;
    int dual_plane, error, void_extent_ldr, void_extent_hdr;
} WeightParams;

static u32 weight_count(const WeightParams* p) { return p->width * p->height * (p->dual_plane ? 2 : 1); }

static WeightParams decode_block_info(BitIn* s) {
    static const u32 low_weights[6] = {1, 2, 3, 4, 5, 7}, high_weights[6] = {9, 11, 15, 19, 23, 31};
    WeightParams p;
    u32 mode = bits_read(s, 11), layout, R, A, B;
    memset(&p, 0, sizeof p);
    if ((mode & 0x1FF) == 0x1FC) {
        if (mode & 0x200)
            p.void_extent_hdr = 1;
        else
            p.void_extent_ldr = 1;
        /* the two bits after it must be one */
        if (!(mode & 0x400) || !bits_read(s, 1)) p.error = 1;
        return p;
    }
    if ((mode & 0xF) == 0 || ((mode & 0x3) == 0 && (mode & 0x1C0) == 0x1C0)) {
        p.error = 1;
        return p;
    }
    /* the block mode layouts, in the ASTC specification's table order */
    if (mode & 0x3)
        layout = mode & 0x8 ? (mode & 0x4 ? (mode & 0x100 ? 4 : 3) : 2) : (mode & 0x4 ? 1 : 0);
    else
        layout = mode & 0x100 ? (mode & 0x80 ? (mode & 0x20 ? 8 : 7) : 9) : (mode & 0x80 ? 6 : 5);
    R = (mode >> 4) & 1;
    R |= layout < 5 ? (mode & 0x3) << 1 : (mode & 0xC) >> 1;
    A = (mode >> 5) & 0x3;
    B = (mode >> 7) & 0x3;
    switch (layout) {
    case 0:
        p.width = B + 4;
        p.height = A + 2;
        break;
    case 1:
        p.width = B + 8;
        p.height = A + 2;
        break;
    case 2:
        p.width = A + 2;
        p.height = B + 8;
        break;
    case 3:
        p.width = A + 2;
        p.height = (B & 1) + 6;
        break;
    case 4:
        p.width = (B & 1) + 2;
        p.height = A + 2;
        break;
    case 5:
        p.width = 12;
        p.height = A + 2;
        break;
    case 6:
        p.width = A + 2;
        p.height = 12;
        break;
    case 7:
        p.width = 6;
        p.height = 10;
        break;
    case 8:
        p.width = 10;
        p.height = 6;
        break;
    default:
        p.width = A + 6;
        p.height = ((mode >> 9) & 0x3) + 6;
        break;
    }
    p.dual_plane = layout != 9 && (mode & 0x400);
    p.max_weight = layout != 9 && (mode & 0x200) ? high_weights[R - 2] : low_weights[R - 2];
    return p;
}

/* the color values of the block's partitions, unquantized to [0, 255] */
static void decode_color_values(u32 out[32], const u8 data[16], const u32 modes[4], u32 partitions, u32 color_bits) {
    IseList values;
    BitIn s;
    u8 bits[16];
    u32 count = 0, range = 256, out_idx = 0, i;
    for (i = 0; i < partitions; i++) count += ((modes[i] >> 2) + 1) << 1;
    /* the largest range whose values fit, then the smallest range with the same encoding */
    while (--range > 0) {
        IseValue e = ise_encoding(range);
        if (ise_bit_length(e, count) <= color_bits) {
            while (--range > 0) {
                IseValue other = ise_encoding(range);
                if (other.encoding != e.encoding || other.num_bits != e.num_bits) break;
            }
            range++;
            break;
        }
    }
    /* bits past the sequence's length read as zero (the encoder may leave anything there): Eden decodes the
       last trit or quint block's spare values from them, which can change the values that are used */
    memcpy(bits, data, sizeof bits);
    for (i = ise_bit_length(ise_encoding(range), count); i < 128; i++) bits[i >> 3] &= (u8)~(1u << (i & 7));
    s.data = bits;
    s.pos = 0;
    decode_integer_sequence(&values, &s, range, count);
    for (i = 0; i < values.n && out_idx < count; i++) {
        const IseValue* v = &values.v[i];
        u32 bitval = v->bit_value, A = (bitval & 1) * 0x1FF, B = 0, C = 0, D = v->tq, T;
        if (v->encoding == ISE_BITS) {
            out[out_idx++] = replicate(bitval, v->num_bits, 8);
            continue;
        }
        if (v->encoding == ISE_TRIT) {
            switch (v->num_bits) {
            case 1:
                C = 204;
                break;
            case 2:
                C = 93;
                T = (bitval >> 1) & 1; /* B = b000b0bb0 */
                B = (T << 8) | (T << 4) | (T << 2) | (T << 1);
                break;
            case 3:
                C = 44;
                T = (bitval >> 1) & 3; /* B = cb000cbcb */
                B = (T << 7) | (T << 2) | T;
                break;
            case 4:
                C = 22;
                T = (bitval >> 1) & 7; /* B = dcb000dcb */
                B = (T << 6) | T;
                break;
            case 5:
                C = 11;
                T = (bitval >> 1) & 0xF; /* B = edcb000ed */
                B = (T << 5) | (T >> 2);
                break;
            case 6:
                C = 5;
                T = (bitval >> 1) & 0x1F; /* B = fedcb000f */
                B = (T << 4) | (T >> 4);
                break;
            }
        } else {
            switch (v->num_bits) {
            case 1:
                C = 113;
                break;
            case 2:
                C = 54;
                T = (bitval >> 1) & 1; /* B = b0000bb00 */
                B = (T << 8) | (T << 3) | (T << 2);
                break;
            case 3:
                C = 26;
                T = (bitval >> 1) & 3; /* B = cb0000cbc */
                B = (T << 7) | (T << 1) | (T >> 1);
                break;
            case 4:
                C = 13;
                T = (bitval >> 1) & 7; /* B = dcb0000dc */
                B = (T << 6) | (T >> 1);
                break;
            case 5:
                C = 6;
                T = (bitval >> 1) & 0xF; /* B = edcb0000e */
                B = (T << 5) | (T >> 3);
                break;
            }
        }
        T = (D * C + B) ^ A;
        out[out_idx++] = (A & 0x80) | (T >> 2);
    }
}

/* a weight to [0, 64] */
static u32 unquantize_weight(const IseValue* v) {
    u32 bitval = v->bit_value, A = (bitval & 1) * 0x7F, B = 0, C = 0, D = v->tq, b, result;
    if (v->encoding == ISE_BITS) {
        result = replicate(bitval, v->num_bits, 6);
    } else if (v->num_bits == 0) {
        static const u32 trits[3] = {0, 32, 63}, quints[5] = {0, 16, 32, 47, 63};
        result = v->encoding == ISE_TRIT ? trits[D] : quints[D];
    } else {
        if (v->encoding == ISE_TRIT) {
            switch (v->num_bits) {
            case 1:
                C = 50;
                break;
            case 2:
                C = 23;
                b = (bitval >> 1) & 1;
                B = (b << 6) | (b << 2) | b;
                break;
            case 3:
                C = 11;
                b = (bitval >> 1) & 3;
                B = (b << 5) | b;
                break;
            }
        } else if (v->num_bits == 1) {
            C = 28;
        } else {
            C = 13;
            b = (bitval >> 1) & 1;
            B = (b << 6) | (b << 1);
        }
        result = (A & 0x20) | (((D * C + B) ^ A) >> 2);
    }
    return result > 32 ? result + 1 : result;
}

/* the weight grid unquantized and infilled to every texel of the block, per plane */
static void unquantize_weights(u32 out[2][144], const IseList* weights, const WeightParams* p, u32 bw, u32 bh) {
    u32 grid[2][64], area = p->width * p->height, planes = p->dual_plane ? 2 : 1, n = 0, i;
    u32 Ds = (1024 + bw / 2) / (bw - 1), Dt = (1024 + bh / 2) / (bh - 1), plane, s, t;
    memset(grid, 0, sizeof grid);
    for (i = 0; i + planes <= weights->n && n < area; i += planes, n++) {
        grid[0][n] = unquantize_weight(&weights->v[i]);
        if (planes == 2) grid[1][n] = unquantize_weight(&weights->v[i + 1]);
    }
    for (plane = 0; plane < planes; plane++)
        for (t = 0; t < bh; t++)
            for (s = 0; s < bw; s++) {
                u32 gs = (Ds * s * (p->width - 1) + 32) >> 6, gt = (Dt * t * (p->height - 1) + 32) >> 6;
                u32 js = gs >> 4, fs = gs & 0xF, jt = gt >> 4, ft = gt & 0xF;
                u32 w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
                u32 v0 = js + jt * p->width;
                u32 p00 = v0 < area ? grid[plane][v0] : 0;
                u32 p01 = v0 + 1 < area ? grid[plane][v0 + 1] : 0;
                u32 p10 = v0 + p->width < area ? grid[plane][v0 + p->width] : 0;
                u32 p11 = v0 + p->width + 1 < area ? grid[plane][v0 + p->width + 1] : 0;
                out[plane][t * bw + s] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
            }
}

/* moves a bit from b to a, a becoming a signed 6 bit value */
static void bit_transfer_signed(int* a, int* b) {
    *b >>= 1;
    *b |= *a & 0x80;
    *a >>= 1;
    *a &= 0x3F;
    if (*a & 0x20) *a -= 0x40;
}

/* the partition of texel (x, y) */
static u32 select_partition(u32 seed, u32 x, u32 y, u32 count, int small_block) {
    u32 rnum, sd[8], sh1, sh2, a, b, c, d, i;
    if (count == 1) return 0;
    if (small_block) {
        x <<= 1;
        y <<= 1;
    }
    seed += (count - 1) * 1024;
    rnum = seed;
    rnum ^= rnum >> 15;
    rnum -= rnum << 17;
    rnum += rnum << 7;
    rnum += rnum << 4;
    rnum ^= rnum >> 5;
    rnum += rnum << 16;
    rnum ^= rnum >> 7;
    rnum ^= rnum >> 3;
    rnum ^= rnum << 6;
    rnum ^= rnum >> 17;
    for (i = 0; i < 8; i++) {
        sd[i] = (rnum >> (i * 4)) & 0xF;
        sd[i] *= sd[i];
    }
    if (seed & 1) {
        sh1 = seed & 2 ? 4 : 5;
        sh2 = count == 3 ? 6 : 5;
    } else {
        sh1 = count == 3 ? 6 : 5;
        sh2 = seed & 2 ? 4 : 5;
    }
    for (i = 0; i < 8; i++) sd[i] >>= i & 1 ? sh2 : sh1;
    a = (sd[0] * x + sd[1] * y + (rnum >> 14)) & 0x3F;
    b = (sd[2] * x + sd[3] * y + (rnum >> 10)) & 0x3F;
    c = (sd[4] * x + sd[5] * y + (rnum >> 6)) & 0x3F;
    d = (sd[6] * x + sd[7] * y + (rnum >> 2)) & 0x3F;
    if (count < 4) d = 0;
    if (count < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

static int is_hdr_mode(u32 cem) { return cem == 2 || cem == 3 || cem == 7 || cem == 11 || cem == 14 || cem == 15; }

static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* the low bits of v as a signed number: Eden keeps the bits above them, which mode 11 shares with x2 to x5 */
static int sign_extend(int v, u32 bits) {
    int sign = 1 << (bits - 1);
    return ((v & ((1 << bits) - 1)) ^ sign) - sign;
}

/* HDR endpoint mode 7 (RGB, base and scale): rgb[0..2] the first endpoint, rgb[3..5] the second */
static void hdr_mode7(u32 v0, u32 v1, u32 v2, u32 v3, int rgb[6]) {
    static const int shamts[6] = {1, 1, 2, 3, 4, 5};
    u32 modeval = ((v0 & 0xC0) >> 6) | ((v1 & 0x80) >> 5) | ((v2 & 0x80) >> 4), majcomp, mode, ohm;
    int red = (int)(v0 & 0x3F), green = (int)(v1 & 0x1F), blue = (int)(v2 & 0x1F), scale = (int)(v3 & 0x1F), t;
    int x0 = (int)((v1 >> 6) & 1), x1 = (int)((v1 >> 5) & 1), x2 = (int)((v2 >> 6) & 1), x3 = (int)((v2 >> 5) & 1);
    int x4 = (int)((v3 >> 7) & 1), x5 = (int)((v3 >> 6) & 1), x6 = (int)((v3 >> 5) & 1);
    if ((modeval & 0xC) != 0xC) {
        majcomp = modeval >> 2;
        mode = modeval & 3;
    } else if (modeval != 0xF) {
        majcomp = modeval & 3;
        mode = 4;
    } else {
        majcomp = 0;
        mode = 5;
    }
    ohm = 1u << mode;
    if (ohm & 0x30) green |= x0 << 6;
    if (ohm & 0x3A) green |= x1 << 5;
    if (ohm & 0x30) blue |= x2 << 6;
    if (ohm & 0x3A) blue |= x3 << 5;
    if (ohm & 0x3D) scale |= x6 << 5;
    if (ohm & 0x2D) scale |= x5 << 6;
    if (ohm & 0x04) scale |= x4 << 7;
    if (ohm & 0x3B) red |= x4 << 6;
    if (ohm & 0x04) red |= x3 << 6;
    if (ohm & 0x10) red |= x5 << 7;
    if (ohm & 0x0F) red |= x2 << 7;
    if (ohm & 0x05) red |= x1 << 8;
    if (ohm & 0x0A) red |= x0 << 8;
    if (ohm & 0x05) red |= x0 << 9;
    if (ohm & 0x02) red |= x6 << 9;
    if (ohm & 0x01) red |= x3 << 10;
    if (ohm & 0x02) red |= x5 << 10;
    red <<= shamts[mode];
    green <<= shamts[mode];
    blue <<= shamts[mode];
    scale <<= shamts[mode];
    if (mode != 5) {
        green = red - green;
        blue = red - blue;
    }
    if (majcomp == 1) {
        t = red;
        red = green;
        green = t;
    }
    if (majcomp == 2) {
        t = red;
        red = blue;
        blue = t;
    }
    rgb[3] = clamp_i(red, 0, 0xFFF);
    rgb[4] = clamp_i(green, 0, 0xFFF);
    rgb[5] = clamp_i(blue, 0, 0xFFF);
    rgb[0] = clamp_i(red - scale, 0, 0xFFF);
    rgb[1] = clamp_i(green - scale, 0, 0xFFF);
    rgb[2] = clamp_i(blue - scale, 0, 0xFFF);
}

/* HDR endpoint mode 11 (RGB, direct), also the RGB of modes 14 and 15 */
static void hdr_mode11(const u32 v[6], int rgb[6]) {
    static const u32 dbits[8] = {7, 6, 7, 6, 5, 6, 5, 6};
    u32 majcomp = ((v[4] & 0x80) >> 7) | ((v[5] & 0x80) >> 6), mode, ohm, i;
    int va, vb0, vb1, vc, vd0, vd1, x0, x1, x2, x3, x4, x5, shamt, t;
    if (majcomp == 3) {
        rgb[0] = (int)(v[0] << 4);
        rgb[1] = (int)(v[2] << 4);
        rgb[2] = (int)((v[4] & 0x7F) << 5);
        rgb[3] = (int)(v[1] << 4);
        rgb[4] = (int)(v[3] << 4);
        rgb[5] = (int)((v[5] & 0x7F) << 5);
        return;
    }
    mode = ((v[1] & 0x80) >> 7) | ((v[2] & 0x80) >> 6) | ((v[3] & 0x80) >> 5);
    va = (int)(v[0] | ((v[1] & 0x40) << 2));
    vb0 = (int)(v[2] & 0x3F);
    vb1 = (int)(v[3] & 0x3F);
    vc = (int)(v[1] & 0x3F);
    vd0 = sign_extend((int)(v[4] & 0x7F), dbits[mode]);
    vd1 = sign_extend((int)(v[5] & 0x7F), dbits[mode]);
    x0 = (int)((v[2] >> 6) & 1);
    x1 = (int)((v[3] >> 6) & 1);
    x2 = (int)((v[4] >> 6) & 1);
    x3 = (int)((v[5] >> 6) & 1);
    x4 = (int)((v[4] >> 5) & 1);
    x5 = (int)((v[5] >> 5) & 1);
    ohm = 1u << mode;
    if (ohm & 0xA4) va |= x0 << 9;
    if (ohm & 0x08) va |= x2 << 9;
    if (ohm & 0x50) va |= x4 << 9;
    if (ohm & 0x50) va |= x5 << 10;
    if (ohm & 0xA0) va |= x1 << 10;
    if (ohm & 0xC0) va |= x2 << 11;
    if (ohm & 0x04) vc |= x1 << 6;
    if (ohm & 0xE8) vc |= x3 << 6;
    if (ohm & 0x20) vc |= x2 << 7;
    if (ohm & 0x5B) vb0 |= x0 << 6;
    if (ohm & 0x5B) vb1 |= x1 << 6;
    if (ohm & 0x12) vb0 |= x2 << 7;
    if (ohm & 0x12) vb1 |= x3 << 7;
    /* shifted as multiplications: vd0 and vd1 can be negative */
    shamt = 1 << (((int)mode >> 1) ^ 3);
    va *= shamt;
    vb0 *= shamt;
    vb1 *= shamt;
    vc *= shamt;
    vd0 *= shamt;
    vd1 *= shamt;
    rgb[3] = clamp_i(va, 0, 0xFFF);
    rgb[4] = clamp_i(va - vb0, 0, 0xFFF);
    rgb[5] = clamp_i(va - vb1, 0, 0xFFF);
    rgb[0] = clamp_i(va - vc, 0, 0xFFF);
    rgb[1] = clamp_i(va - vb0 - vc - vd0, 0, 0xFFF);
    rgb[2] = clamp_i(va - vb1 - vc - vd1, 0, 0xFFF);
    if (majcomp == 1 || majcomp == 2)
        for (i = 0; i < 6; i += 3) {
            t = rgb[i];
            rgb[i] = rgb[i + majcomp];
            rgb[i + majcomp] = t;
        }
}

static void set4(int e[4], int a, int r, int g, int b) {
    e[0] = a;
    e[1] = r;
    e[2] = g;
    e[3] = b;
}

/* blue contraction: more precision for blue */
static void blue_contract(int e[4], int a, int r, int g, int b) { set4(e, a, (r + b) >> 1, (g + b) >> 1, b); }

static void clamp_byte(int e[4]) {
    u32 i;
    for (i = 0; i < 4; i++) e[i] = clamp_i(e[i], 0, 255);
}

/* the two endpoints of a partition, in A, R, G, B order: 8 bit for the LDR modes, 12 bit (more for mode 15's
   alpha) for HDR. vals advances past the color values the mode takes */
static void compute_endpoints(int e0[4], int e1[4], const u32** vals, u32 cem) {
    static const u32 counts[16] = {2, 2, 2, 2, 4, 4, 4, 4, 6, 6, 6, 6, 8, 8, 8, 8};
    u32 u[8], i;
    int v[8], rgb[6];
    for (i = 0; i < counts[cem]; i++) {
        u[i] = (*vals)[i];
        v[i] = (int)u[i];
    }
    *vals += counts[cem];
    switch (cem) {
    case 0:
        set4(e0, 0xFF, v[0], v[0], v[0]);
        set4(e1, 0xFF, v[1], v[1], v[1]);
        break;
    case 1: {
        int l0 = (int)((u[0] >> 2) | (u[1] & 0xC0)), l1 = l0 + (int)(u[1] & 0x3F);
        if (l1 > 0xFF) l1 = 0xFF;
        set4(e0, 0xFF, l0, l0, l0);
        set4(e1, 0xFF, l1, l1, l1);
        break;
    }
    case 4:
        set4(e0, v[2], v[0], v[0], v[0]);
        set4(e1, v[3], v[1], v[1], v[1]);
        break;
    case 5:
        bit_transfer_signed(&v[1], &v[0]);
        bit_transfer_signed(&v[3], &v[2]);
        set4(e0, v[2], v[0], v[0], v[0]);
        set4(e1, v[2] + v[3], v[0] + v[1], v[0] + v[1], v[0] + v[1]);
        clamp_byte(e0);
        clamp_byte(e1);
        break;
    case 6:
        set4(e0, 0xFF, v[0] * v[3] >> 8, v[1] * v[3] >> 8, v[2] * v[3] >> 8);
        set4(e1, 0xFF, v[0], v[1], v[2]);
        break;
    case 8:
        if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) {
            set4(e0, 0xFF, v[0], v[2], v[4]);
            set4(e1, 0xFF, v[1], v[3], v[5]);
        } else {
            blue_contract(e0, 0xFF, v[1], v[3], v[5]);
            blue_contract(e1, 0xFF, v[0], v[2], v[4]);
        }
        break;
    case 9:
        bit_transfer_signed(&v[1], &v[0]);
        bit_transfer_signed(&v[3], &v[2]);
        bit_transfer_signed(&v[5], &v[4]);
        if (v[1] + v[3] + v[5] >= 0) {
            set4(e0, 0xFF, v[0], v[2], v[4]);
            set4(e1, 0xFF, v[0] + v[1], v[2] + v[3], v[4] + v[5]);
        } else {
            blue_contract(e0, 0xFF, v[0] + v[1], v[2] + v[3], v[4] + v[5]);
            blue_contract(e1, 0xFF, v[0], v[2], v[4]);
        }
        clamp_byte(e0);
        clamp_byte(e1);
        break;
    case 10:
        set4(e0, v[4], v[0] * v[3] >> 8, v[1] * v[3] >> 8, v[2] * v[3] >> 8);
        set4(e1, v[5], v[0], v[1], v[2]);
        break;
    case 12:
        if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) {
            set4(e0, v[6], v[0], v[2], v[4]);
            set4(e1, v[7], v[1], v[3], v[5]);
        } else {
            blue_contract(e0, v[7], v[1], v[3], v[5]);
            blue_contract(e1, v[6], v[0], v[2], v[4]);
        }
        break;
    case 13:
        bit_transfer_signed(&v[1], &v[0]);
        bit_transfer_signed(&v[3], &v[2]);
        bit_transfer_signed(&v[5], &v[4]);
        bit_transfer_signed(&v[7], &v[6]);
        if (v[1] + v[3] + v[5] >= 0) {
            set4(e0, v[6], v[0], v[2], v[4]);
            set4(e1, v[7] + v[6], v[0] + v[1], v[2] + v[3], v[4] + v[5]);
        } else {
            blue_contract(e0, v[6] + v[7], v[0] + v[1], v[2] + v[3], v[4] + v[5]);
            blue_contract(e1, v[6], v[0], v[2], v[4]);
        }
        clamp_byte(e0);
        clamp_byte(e1);
        break;
    case 2: {
        int y0, y1;
        if (v[1] >= v[0]) {
            y0 = v[0] << 4;
            y1 = v[1] << 4;
        } else {
            y0 = (v[1] << 4) + 8;
            y1 = (v[0] << 4) - 8;
        }
        set4(e0, 0x780, y0, y0, y0);
        set4(e1, 0x780, y1, y1, y1);
        break;
    }
    case 3: {
        int y0, d, y1;
        if (u[0] & 0x80) {
            y0 = (int)(((u[1] & 0xE0) << 4) | ((u[0] & 0x7F) << 2));
            d = (int)((u[1] & 0x1F) << 2);
        } else {
            y0 = (int)(((u[1] & 0xF0) << 4) | ((u[0] & 0x7F) << 1));
            d = (int)((u[1] & 0x0F) << 1);
        }
        y1 = y0 + d > 0xFFF ? 0xFFF : y0 + d;
        set4(e0, 0x780, y0, y0, y0);
        set4(e1, 0x780, y1, y1, y1);
        break;
    }
    case 7:
        hdr_mode7(u[0], u[1], u[2], u[3], rgb);
        set4(e0, 0x780, rgb[0], rgb[1], rgb[2]);
        set4(e1, 0x780, rgb[3], rgb[4], rgb[5]);
        break;
    case 11:
        hdr_mode11(u, rgb);
        set4(e0, 0x780, rgb[0], rgb[1], rgb[2]);
        set4(e1, 0x780, rgb[3], rgb[4], rgb[5]);
        break;
    case 14:
        hdr_mode11(u, rgb);
        set4(e0, v[6], rgb[0], rgb[1], rgb[2]);
        set4(e1, v[7], rgb[3], rgb[4], rgb[5]);
        break;
    default: { /* 15: HDR RGB with HDR alpha */
        u32 mode = ((u[6] >> 7) & 1) | ((u[7] >> 6) & 2);
        int a6 = (int)(u[6] & 0x7F), a7 = (int)(u[7] & 0x7F), alpha0, alpha1;
        hdr_mode11(u, rgb);
        if (mode == 3) {
            alpha0 = a6 << 5;
            alpha1 = a7 << 5;
        } else {
            a6 |= (a7 << (mode + 1)) & 0x780;
            a7 &= 0x3F >> mode;
            a7 ^= 0x20 >> mode;
            a7 -= 0x20 >> mode;
            /* as multiplications: a7 can be negative */
            a6 *= 1 << (4 - mode);
            a7 *= 1 << (4 - mode);
            a7 += a6;
            alpha0 = a6;
            alpha1 = clamp_i(a7, 0, 0xFFF);
        }
        set4(e0, alpha0, rgb[0], rgb[1], rgb[2]);
        set4(e1, alpha1, rgb[3], rgb[4], rgb[5]);
        break;
    }
    }
}

/* a UNORM16 value to 8 bits, rounded to nearest (astc.comp's Unorm16To8) */
static u32 unorm16_to_8(u32 c) { return (c * 255 + 32767) / 65535; }

/* a non-negative half below infinity, clamped to [0, 1], to 8 bits rounded to nearest (astc.comp's HalfToByte) */
static u32 half_to_byte(u32 h) {
    u32 e = h >> 10, sig, shift;
    if (h >= 0x3C00) return 255;
    sig = e == 0 ? h : (h & 0x3FF) | 0x400;
    shift = e == 0 ? 24 : 25 - e;
    return (sig * 255 + (1u << (shift - 1))) >> shift;
}

static u32 pack(u32 a, u32 r, u32 g, u32 b) { return r | g << 8 | b << 16 | a << 24; }

static void fill(u32 out[144], u32 n, u32 rgba) {
    u32 i;
    for (i = 0; i < n; i++) out[i] = rgba;
}

/* one block to bw x bh texels, packed as astc.comp packs them (R in the low byte); errors are transparent
   black. srgb selects the specification's sRGB decode mode for R, G and B: the LDR endpoints expand to 16 bits as
   (e << 8) | 0x80 instead of by bit replication, and the top 8 bits of the result are the decoded value (also
   those of a void extent's color). Alpha and HDR endpoints decode the same in both modes */
static void decompress_block(const u8 in[16], u32 bw, u32 bh, int srgb, u32 out[144]) {
    BitIn strm;
    WeightParams wp;
    IseList weights;
    u32 partitions, partition_index = 0, cem[4] = {0, 0, 0, 0}, base_cem = 0, base_mode, weight_bits;
    u32 extra_cem_bits = 0, plane_bits, color_bits, plane_idx, color_values[32], i, j, c, pos = 0;
    u32 weight_grid[2][144];
    int remaining, endpoints[4][2][4];
    u8 color_data[16], weight_data[16];
    const u32* vals;

    strm.data = in;
    strm.pos = 0;
    wp = decode_block_info(&strm);
    if (wp.error || wp.void_extent_hdr) {
        fill(out, bw * bh, 0);
        return;
    }
    if (wp.void_extent_ldr) {
        u32 s_min, s_max, t_min, t_max, r, g, b, a;
        /* the extent's coordinates are not needed, but unless they are all ones the specification requires each
           minimum below its maximum, which Eden does not check */
        strm.pos = 12;
        s_min = bits_read(&strm, 13);
        s_max = bits_read(&strm, 13);
        t_min = bits_read(&strm, 13);
        t_max = bits_read(&strm, 13);
        if ((s_min & s_max & t_min & t_max) != 0x1FFF && (s_min >= s_max || t_min >= t_max)) {
            fill(out, bw * bh, 0);
            return;
        }
        r = bits_read(&strm, 16);
        g = bits_read(&strm, 16);
        b = bits_read(&strm, 16);
        a = unorm16_to_8(bits_read(&strm, 16));
        if (srgb) {
            r >>= 8;
            g >>= 8;
            b >>= 8;
        } else {
            r = unorm16_to_8(r);
            g = unorm16_to_8(g);
            b = unorm16_to_8(b);
        }
        fill(out, bw * bh, pack(a, r, g, b));
        return;
    }
    if (wp.width > bw || wp.height > bh || weight_count(&wp) > 64) {
        fill(out, bw * bh, 0);
        return;
    }
    partitions = bits_read(&strm, 2) + 1;
    if (partitions == 4 && wp.dual_plane) {
        fill(out, bw * bh, 0);
        return;
    }
    if (partitions == 1) {
        cem[0] = bits_read(&strm, 4);
    } else {
        partition_index = bits_read(&strm, 10);
        base_cem = bits_read(&strm, 6);
    }
    base_mode = base_cem & 3;
    weight_bits = ise_bit_length(ise_encoding(wp.max_weight), weight_count(&wp));
    if (weight_bits < 24 || weight_bits > 96) {
        fill(out, bw * bh, 0);
        return;
    }
    if (base_mode) extra_cem_bits = partitions * 3 - 4; /* 2, 5 or 8 */
    plane_bits = wp.dual_plane ? 2 : 0;
    remaining = 128 - (int)weight_bits - (int)strm.pos - (int)extra_cem_bits - (int)plane_bits;
    if (remaining < 0) {
        /* more weight and mode bits than the block holds: an error, as in astc.comp */
        fill(out, bw * bh, 0);
        return;
    }
    color_bits = (u32)remaining;
    memset(color_data, 0, sizeof color_data);
    for (i = 0; i < color_bits; i += 8) {
        u32 nb = color_bits - i < 8 ? color_bits - i : 8;
        bits_write(color_data, &pos, bits_read(&strm, nb), nb);
    }
    plane_idx = bits_read(&strm, plane_bits);
    if (base_mode) {
        u32 bits = ((bits_read(&strm, extra_cem_bits) << 6) | base_cem) >> 2, C[4], M[4];
        for (i = 0; i < partitions; i++, bits >>= 1) C[i] = bits & 1;
        for (i = 0; i < partitions; i++, bits >>= 2) M[i] = bits & 3;
        for (i = 0; i < partitions; i++) cem[i] = ((base_mode - (C[i] ? 0 : 1)) << 2) | M[i];
    } else if (partitions > 1) {
        for (i = 0; i < partitions; i++) cem[i] = base_cem >> 2;
    }

    /* the specification's illegal encodings that Eden decodes anyway: more than 18 color values, or too few bits
       for them to have at least 6 levels */
    for (i = 0, j = 0; i < partitions; i++) j += ((cem[i] >> 2) + 1) << 1;
    if (j > 18 || ise_bit_length(ise_encoding(5), j) > color_bits) {
        fill(out, bw * bh, 0);
        return;
    }
    decode_color_values(color_values, color_data, cem, partitions, color_bits);
    vals = color_values;
    for (i = 0; i < partitions; i++) compute_endpoints(endpoints[i][0], endpoints[i][1], &vals, cem[i]);

    /* the weights run from the top of the block down: reverse the bits, then clear those above the weights */
    for (i = 0; i < 16; i++) {
        u32 v = in[15 - i], r = 0, k;
        for (k = 0; k < 8; k++) r |= ((v >> k) & 1) << (7 - k);
        weight_data[i] = (u8)r;
    }
    i = (weight_bits >> 3) + 1;
    weight_data[i - 1] &= (u8)((1u << (weight_bits % 8)) - 1);
    if (i < 16) memset(weight_data + i, 0, 16 - i);
    strm.data = weight_data;
    strm.pos = 0;
    decode_integer_sequence(&weights, &strm, wp.max_weight, weight_count(&wp));
    unquantize_weights(weight_grid, &weights, &wp, bw, bh);

    for (j = 0; j < bh; j++)
        for (i = 0; i < bw; i++) {
            u32 part = select_partition(partition_index, i, j, partitions, bw * bh < 32), px[4];
            for (c = 0; c < 4; c++) {
                u32 plane = wp.dual_plane && ((plane_idx + 1) & 3) == c;
                u32 w = weight_grid[plane][j * bw + i];
                u32 C0 = (u32)endpoints[part][0][c], C1 = (u32)endpoints[part][1][c];
                if (is_hdr_mode(cem[part]) && !(cem[part] == 14 && c == 0)) {
                    u32 C = ((C0 << 4) * (64 - w) + (C1 << 4) * w + 32) / 64;
                    u32 E = (C & 0xF800) >> 11, M = C & 0x7FF, Mt, Cf;
                    Mt = M < 512 ? 3 * M : M >= 1536 ? 5 * M - 2048 : 4 * M - 512;
                    Cf = (E << 10) + (Mt >> 3);
                    px[c] = half_to_byte(Cf >= 0x7C00 ? 0x7BFF : Cf);
                } else if (srgb && c != 0) {
                    px[c] = (((C0 << 8) | 0x80) * (64 - w) + ((C1 << 8) | 0x80) * w + 32) / 64 >> 8;
                } else {
                    px[c] = unorm16_to_8((C0 * 0x101 * (64 - w) + C1 * 0x101 * w + 32) / 64);
                }
            }
            out[j * bw + i] = pack(px[0], px[1], px[2], px[3]);
        }
}

void astc_decode_image(const u8* src, u32 w, u32 h, u32 bw, u32 bh, u8* dst, int srgb) {
    u32 cols = (w + bw - 1) / bw, rows = (h + bh - 1) / bh, bx, by, x, y;
    u32 texels[144];
    for (by = 0; by < rows; by++)
        for (bx = 0; bx < cols; bx++) {
            decompress_block(src + ((size_t)by * cols + bx) * 16, bw, bh, srgb, texels);
            for (y = 0; y < bh && by * bh + y < h; y++)
                for (x = 0; x < bw && bx * bw + x < w; x++) {
                    u32 v = texels[y * bw + x];
                    u8* p = dst + ((size_t)(by * bh + y) * w + bx * bw + x) * 4;
                    p[0] = (u8)v;
                    p[1] = (u8)(v >> 8);
                    p[2] = (u8)(v >> 16);
                    p[3] = (u8)(v >> 24);
                }
        }
}
