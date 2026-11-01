/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/vec_internal.h: do_sqrshl_bhs, do_uqrshl_bhs, do_suqrshl_bhs, do_sqrshl_d,
   do_uqrshl_d; target/arm/tcg/crypto_helper.c: the SHA-512, SM3 and SM4 helpers), LGPL-2.1-or-later:
   Copyright (c) 2020 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org>
   The SM4 S-box is GB/T 32907-2016's, as OpenSSL's crypto/sm4/sm4.c tabulates it (Apache-2.0):
   Copyright 2017-2024 The OpenSSL Project Authors. All Rights Reserved.
   Copyright 2017 Ribose Inc. All Rights Reserved. */
/* The AdvSIMD operations the LLVM backend (llvm/ir_vec.c, ir_vec_crypto.c) calls out of line: the shifts by register
   that round or saturate, and the SHA-512, SM3 and SM4 rounds. Registers by number in c->v, read and written as
   numbers through SNX_V* (cpu.h), so a big-endian host sees the guest's lanes. */

/* lane i of size esz of v, and its store */
static u64 vec_get(const V128* v, u32 esz, u32 i) {
    switch (esz) {
        case 0: return SNX_VB(*v, i);
        case 1: return SNX_VH(*v, i);
        case 2: return SNX_VS(*v, i);
        default: return SNX_VD(*v, i);
    }
}
static void vec_set(V128* v, u32 esz, u32 i, u64 x) {
    switch (esz) {
        case 0: SNX_VSET_B(*v, i, x); break;
        case 1: SNX_VSET_H(*v, i, x); break;
        case 2: SNX_VSET_S(*v, i, x); break;
        default: SNX_VSET_D(*v, i, x); break;
    }
}

/* ---- shifts by register (vec_internal.h) ---- */

static i32 vec_sqrshl_bhs(i32 src, i32 shift, int bits, int round, u32* sat) {
    if (shift <= -bits) return round ? 0 : src >> 31;
    if (shift < 0) {
        if (round) {
            src >>= -shift - 1;
            return (src >> 1) + (src & 1);
        }
        return src >> -shift;
    }
    if (shift < bits) {
        i32 val = (i32)((u32)src << shift);
        if (bits == 32) {
            if (!sat || val >> shift == src) return val;
        } else {
            i32 extval = (i32)((u32)val << (32 - bits)) >> (32 - bits);
            if (!sat || val == extval) return extval;
        }
    } else if (!sat || src == 0)
        return 0;
    *sat = 1;
    return (i32)((1u << (bits - 1)) - (src >= 0));
}

static u32 vec_uqrshl_bhs(u32 src, i32 shift, int bits, int round, u32* sat) {
    if (shift <= -(bits + round)) return 0;
    if (shift < 0) {
        if (round) {
            src >>= -shift - 1;
            return (src >> 1) + (src & 1);
        }
        return src >> -shift;
    }
    if (shift < bits) {
        u32 val = src << shift;
        if (bits == 32) {
            if (!sat || val >> shift == src) return val;
        } else {
            u32 extval = val & ((1u << bits) - 1);
            if (!sat || val == extval) return extval;
        }
    } else if (!sat || src == 0)
        return 0;
    *sat = 1;
    return bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
}

static i64 vec_sqrshl_d(i64 src, i64 shift, int round, u32* sat) {
    if (shift <= -64) return round ? 0 : src >> 63;
    if (shift < 0) {
        if (round) {
            src >>= -shift - 1;
            return (src >> 1) + (src & 1);
        }
        return src >> -shift;
    }
    if (shift < 64) {
        i64 val = (i64)((u64)src << shift);
        if (!sat || val >> shift == src) return val;
    } else if (!sat || src == 0)
        return 0;
    *sat = 1;
    return src < 0 ? (i64)0x8000000000000000ull : (i64)0x7FFFFFFFFFFFFFFFull;
}

static u64 vec_uqrshl_d(u64 src, i64 shift, int round, u32* sat) {
    if (shift <= -(64 + round)) return 0;
    if (shift < 0) {
        if (round) {
            src >>= -shift - 1;
            return (src >> 1) + (src & 1);
        }
        return src >> -shift;
    }
    if (shift < 64) {
        u64 val = src << shift;
        if (!sat || val >> shift == src) return val;
    } else if (!sat || src == 0)
        return 0;
    *sat = 1;
    return ~(u64)0;
}

/* SRSHL, URSHL, SQSHL, UQSHL, SQRSHL, UQRSHL (vector and scalar): desc is esz | lanes << 2 | flags << 8, flags 1
   unsigned, 2 rounding, 4 saturating; the signed low byte of each lane of Vm shifts the lane of Vn, left or (negative)
   right; the lanes above the operation's are zeroed */
void snx_vec_shl(CPU* c, u32 d, u32 n, u32 m, u32 desc) {
    u32 esz = desc & 3, lanes = (desc >> 2) & 63, flags = desc >> 8, sat = 0, bits = 8u << esz;
    int uns = flags & 1, round = (flags >> 1) & 1;
    u32* qc = (flags & 4) ? &sat : NULL;
    V128 r = {0};
    for (u32 i = 0; i < lanes; ++i) {
        u64 x = vec_get(&c->v[n], esz, i), y;
        i32 shift = (i8)vec_get(&c->v[m], esz, i);
        if (esz == 3)
            y = uns ? vec_uqrshl_d(x, shift, round, qc) : (u64)vec_sqrshl_d((i64)x, shift, round, qc);
        else if (uns)
            y = vec_uqrshl_bhs((u32)x, shift, (int)bits, round, qc);
        else {
            i32 sx = (i32)((u32)x << (32 - bits)) >> (32 - bits);
            y = (u32)vec_sqrshl_bhs(sx, shift, (int)bits, round, qc);
        }
        vec_set(&r, esz, i, y);
    }
    c->v[d] = r;
    if (sat) c->fpsr |= 1u << 27;
}

/* ---- SHA-512 (crypto_helper.c) ---- */

static u64 vec_ror64(u64 x, u32 s) { return (x >> s) | (x << (64 - s)); }
static u64 sha512_ch(u64 x, u64 y, u64 z) { return (x & (y ^ z)) ^ z; }
static u64 sha512_maj(u64 x, u64 y, u64 z) { return (x & y) | ((x | y) & z); }
static u64 sha512_S0(u64 x) { return vec_ror64(x, 28) ^ vec_ror64(x, 34) ^ vec_ror64(x, 39); }
static u64 sha512_S1(u64 x) { return vec_ror64(x, 14) ^ vec_ror64(x, 18) ^ vec_ror64(x, 41); }
static u64 sha512_s0(u64 x) { return vec_ror64(x, 1) ^ vec_ror64(x, 8) ^ (x >> 7); }
static u64 sha512_s1(u64 x) { return vec_ror64(x, 19) ^ vec_ror64(x, 61) ^ (x >> 6); }

/* SHA512H (op 0), SHA512H2 (1), SHA512SU0 (2), SHA512SU1 (3) */
void snx_vec_sha512(CPU* c, u32 op, u32 d, u32 n, u32 m) {
    u64 d0 = SNX_VD(c->v[d], 0), d1 = SNX_VD(c->v[d], 1), n0 = SNX_VD(c->v[n], 0), n1 = SNX_VD(c->v[n], 1);
    u64 m0 = SNX_VD(c->v[m], 0), m1 = SNX_VD(c->v[m], 1);
    switch (op) {
        case 0:
            d1 += sha512_S1(m1) + sha512_ch(m1, n0, n1);
            d0 += sha512_S1(d1 + m0) + sha512_ch(d1 + m0, m1, n0);
            break;
        case 1:
            d1 += sha512_S0(m0) + sha512_maj(n0, m1, m0);
            d0 += sha512_S0(d1) + sha512_maj(d1, m0, m1);
            break;
        case 2:
            d0 += sha512_s0(d1);
            d1 += sha512_s0(n0);
            break;
        default:
            d0 += sha512_s1(n0) + m0;
            d1 += sha512_s1(n1) + m1;
            break;
    }
    SNX_VSET_D(c->v[d], 0, d0);
    SNX_VSET_D(c->v[d], 1, d1);
}

/* ---- SM3 (crypto_helper.c) ---- */

static u32 vec_ror32(u32 x, u32 s) { return (x >> s) | (x << ((32 - s) & 31)); }
static u32 vec_rol32(u32 x, u32 s) { return vec_ror32(x, (32 - s) & 31); }

/* SM3PARTW1 (op 0), SM3PARTW2 (1), SM3TT1A, SM3TT1B, SM3TT2A, SM3TT2B (2-5, with imm2) */
void snx_vec_sm3(CPU* c, u32 op, u32 d, u32 n, u32 m, u32 imm2) {
    u32 x[4], y[4], z[4], t;
    for (u32 i = 0; i < 4; ++i) {
        x[i] = SNX_VS(c->v[d], i);
        y[i] = SNX_VS(c->v[n], i);
        z[i] = SNX_VS(c->v[m], i);
    }
    switch (op) {
        case 0:
            for (u32 i = 0; i < 4; ++i) {
                t = x[i] ^ y[i] ^ vec_ror32(i < 3 ? z[i + 1] : x[0], 17);
                x[i] = t ^ vec_ror32(t, 17) ^ vec_ror32(t, 9);
            }
            break;
        case 1:
            t = y[0] ^ vec_ror32(z[0], 25);
            x[0] ^= t;
            x[1] ^= y[1] ^ vec_ror32(z[1], 25);
            x[2] ^= y[2] ^ vec_ror32(z[2], 25);
            x[3] ^= y[3] ^ vec_ror32(z[3], 25) ^ vec_ror32(t, 17) ^ vec_ror32(t, 2) ^ vec_ror32(t, 26);
            break;
        default: {
            u32 tt = op - 2;
            if (tt == 0 || tt == 2)
                t = x[3] ^ x[2] ^ x[1];
            else if (tt == 1)
                t = (x[3] & x[2]) | ((x[3] | x[2]) & x[1]);
            else
                t = (x[3] & (x[2] ^ x[1])) ^ x[1];
            t += x[0] + z[imm2 & 3];
            x[0] = x[1];
            if (tt < 2) {
                t += y[3] ^ vec_ror32(x[3], 20);
                x[1] = vec_ror32(x[2], 23);
            } else {
                t += y[3];
                t ^= vec_rol32(t, 9) ^ vec_rol32(t, 17);
                x[1] = vec_ror32(x[2], 13);
            }
            x[2] = x[3];
            x[3] = t;
            break;
        }
    }
    for (u32 i = 0; i < 4; ++i) SNX_VSET_S(c->v[d], i, x[i]);
}

/* ---- SM4 ---- */

static const u8 sm4_sbox[256] = {
    0xD6, 0x90, 0xE9, 0xFE, 0xCC, 0xE1, 0x3D, 0xB7, 0x16, 0xB6, 0x14, 0xC2, 0x28, 0xFB, 0x2C, 0x05, 0x2B, 0x67, 0x9A,
    0x76, 0x2A, 0xBE, 0x04, 0xC3, 0xAA, 0x44, 0x13, 0x26, 0x49, 0x86, 0x06, 0x99, 0x9C, 0x42, 0x50, 0xF4, 0x91, 0xEF,
    0x98, 0x7A, 0x33, 0x54, 0x0B, 0x43, 0xED, 0xCF, 0xAC, 0x62, 0xE4, 0xB3, 0x1C, 0xA9, 0xC9, 0x08, 0xE8, 0x95, 0x80,
    0xDF, 0x94, 0xFA, 0x75, 0x8F, 0x3F, 0xA6, 0x47, 0x07, 0xA7, 0xFC, 0xF3, 0x73, 0x17, 0xBA, 0x83, 0x59, 0x3C, 0x19,
    0xE6, 0x85, 0x4F, 0xA8, 0x68, 0x6B, 0x81, 0xB2, 0x71, 0x64, 0xDA, 0x8B, 0xF8, 0xEB, 0x0F, 0x4B, 0x70, 0x56, 0x9D,
    0x35, 0x1E, 0x24, 0x0E, 0x5E, 0x63, 0x58, 0xD1, 0xA2, 0x25, 0x22, 0x7C, 0x3B, 0x01, 0x21, 0x78, 0x87, 0xD4, 0x00,
    0x46, 0x57, 0x9F, 0xD3, 0x27, 0x52, 0x4C, 0x36, 0x02, 0xE7, 0xA0, 0xC4, 0xC8, 0x9E, 0xEA, 0xBF, 0x8A, 0xD2, 0x40,
    0xC7, 0x38, 0xB5, 0xA3, 0xF7, 0xF2, 0xCE, 0xF9, 0x61, 0x15, 0xA1, 0xE0, 0xAE, 0x5D, 0xA4, 0x9B, 0x34, 0x1A, 0x55,
    0xAD, 0x93, 0x32, 0x30, 0xF5, 0x8C, 0xB1, 0xE3, 0x1D, 0xF6, 0xE2, 0x2E, 0x82, 0x66, 0xCA, 0x60, 0xC0, 0x29, 0x23,
    0xAB, 0x0D, 0x53, 0x4E, 0x6F, 0xD5, 0xDB, 0x37, 0x45, 0xDE, 0xFD, 0x8E, 0x2F, 0x03, 0xFF, 0x6A, 0x72, 0x6D, 0x6C,
    0x5B, 0x51, 0x8D, 0x1B, 0xAF, 0x92, 0xBB, 0xDD, 0xBC, 0x7F, 0x11, 0xD9, 0x5C, 0x41, 0x1F, 0x10, 0x5A, 0xD8, 0x0A,
    0xC1, 0x31, 0x88, 0xA5, 0xCD, 0x7B, 0xBD, 0x2D, 0x74, 0xD0, 0x12, 0xB8, 0xE5, 0xB4, 0xB0, 0x89, 0x69, 0x97, 0x4A,
    0x0C, 0x96, 0x77, 0x7E, 0x65, 0xB9, 0xF1, 0x09, 0xC5, 0x6E, 0xC6, 0x84, 0x18, 0xF0, 0x7D, 0xEC, 0x3A, 0xDC, 0x4D,
    0x20, 0x79, 0xEE, 0x5F, 0x3E, 0xD7, 0xCB, 0x39, 0x48};

static u32 sm4_subword(u32 x) {
    return (u32)sm4_sbox[x >> 24] << 24 | (u32)sm4_sbox[(x >> 16) & 0xFF] << 16 | (u32)sm4_sbox[(x >> 8) & 0xFF] << 8 |
           sm4_sbox[x & 0xFF];
}

/* SM4E (op 0: Vd's state, Vn's round keys) and SM4EKEY (1: Vn's key, Vm's constants) */
void snx_vec_sm4(CPU* c, u32 op, u32 d, u32 n, u32 m) {
    u32 x[4], k[4];
    for (u32 i = 0; i < 4; ++i) {
        x[i] = SNX_VS(c->v[op ? n : d], i);
        k[i] = SNX_VS(c->v[op ? m : n], i);
    }
    for (u32 i = 0; i < 4; ++i) {
        u32 t = sm4_subword(x[(i + 1) % 4] ^ x[(i + 2) % 4] ^ x[(i + 3) % 4] ^ k[i]);
        if (op)
            x[i] ^= t ^ vec_rol32(t, 13) ^ vec_rol32(t, 23);
        else
            x[i] ^= t ^ vec_rol32(t, 2) ^ vec_rol32(t, 10) ^ vec_rol32(t, 18) ^ vec_rol32(t, 24);
    }
    for (u32 i = 0; i < 4; ++i) SNX_VSET_S(c->v[d], i, x[i]);
}

/* ---- BFDOT, BFMMLA (vec_helper.c's bfdotadd with FPCR.EBF 0) ---- */

/* sum + e1.lo * e2.lo + e1.hi * e2.hi of bfloat16 pairs, as singles: unfused, every product and sum rounded to odd
   (overflowing to infinity), denormals flushed, default NaNs, whatever FPCR says, and no FPSR flag */
u32 snx_vec_bfdot(CPU* c, u32 sum, u32 e1, u32 e2) {
    SnxFpEnv fixed;
    (void)c;
    fixed.fpcr = FPCR_DN | FPCR_FZ;
    fixed.fpsr = 0;
    u64 t1 = fp_binary_rm(&fixed, (u64)(e1 << 16), (u64)(e2 << 16), 32, SNX_FP_MUL, 6);
    u64 t2 = fp_binary_rm(&fixed, (u64)(e1 & 0xFFFF0000u), (u64)(e2 & 0xFFFF0000u), 32, SNX_FP_MUL, 6);
    t1 = fp_binary_rm(&fixed, t1, t2, 32, SNX_FP_ADD, 6);
    return (u32)fp_binary_rm(&fixed, sum, t1, 32, SNX_FP_ADD, 6);
}
