/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/crypto_helper.c), LGPL-2.1-or-later:
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org> */
/* Compiled into every game module's runtime library through cpu_helpers.c (the LLVM backend's AES rounds where the
   host has no AES instructions, SHA-1, SHA-256, PMULL without carry-less multiplication), and again for 32-bit
   games by arm/crypto_helpers.c under other names. The guard keeps a second inclusion in the same unit out. */
#ifndef SNX_HELPERS_CRYPTO_C
#define SNX_HELPERS_CRYPTO_C
#include "common/utilities/bits.h"
/* The Cortex-A57's crypto extension: AESE/AESD/AESMC/AESIMC, SHA-1, SHA-256 and the 64-bit PMULL, on the vector
   registers in plain C. */

/* FIPS-197's forward and inverse S-boxes, constant: an inverse table built on first use raced when guest threads
   ran AES at once */
static const u8 aes_sbox[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
};
static const u8 aes_inv_sbox[256] = {
    0x52, 0x09, 0x6A, 0xD5, 0x30, 0x36, 0xA5, 0x38, 0xBF, 0x40, 0xA3, 0x9E, 0x81, 0xF3, 0xD7, 0xFB,
    0x7C, 0xE3, 0x39, 0x82, 0x9B, 0x2F, 0xFF, 0x87, 0x34, 0x8E, 0x43, 0x44, 0xC4, 0xDE, 0xE9, 0xCB,
    0x54, 0x7B, 0x94, 0x32, 0xA6, 0xC2, 0x23, 0x3D, 0xEE, 0x4C, 0x95, 0x0B, 0x42, 0xFA, 0xC3, 0x4E,
    0x08, 0x2E, 0xA1, 0x66, 0x28, 0xD9, 0x24, 0xB2, 0x76, 0x5B, 0xA2, 0x49, 0x6D, 0x8B, 0xD1, 0x25,
    0x72, 0xF8, 0xF6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xD4, 0xA4, 0x5C, 0xCC, 0x5D, 0x65, 0xB6, 0x92,
    0x6C, 0x70, 0x48, 0x50, 0xFD, 0xED, 0xB9, 0xDA, 0x5E, 0x15, 0x46, 0x57, 0xA7, 0x8D, 0x9D, 0x84,
    0x90, 0xD8, 0xAB, 0x00, 0x8C, 0xBC, 0xD3, 0x0A, 0xF7, 0xE4, 0x58, 0x05, 0xB8, 0xB3, 0x45, 0x06,
    0xD0, 0x2C, 0x1E, 0x8F, 0xCA, 0x3F, 0x0F, 0x02, 0xC1, 0xAF, 0xBD, 0x03, 0x01, 0x13, 0x8A, 0x6B,
    0x3A, 0x91, 0x11, 0x41, 0x4F, 0x67, 0xDC, 0xEA, 0x97, 0xF2, 0xCF, 0xCE, 0xF0, 0xB4, 0xE6, 0x73,
    0x96, 0xAC, 0x74, 0x22, 0xE7, 0xAD, 0x35, 0x85, 0xE2, 0xF9, 0x37, 0xE8, 0x1C, 0x75, 0xDF, 0x6E,
    0x47, 0xF1, 0x1A, 0x71, 0x1D, 0x29, 0xC5, 0x89, 0x6F, 0xB7, 0x62, 0x0E, 0xAA, 0x18, 0xBE, 0x1B,
    0xFC, 0x56, 0x3E, 0x4B, 0xC6, 0xD2, 0x79, 0x20, 0x9A, 0xDB, 0xC0, 0xFE, 0x78, 0xCD, 0x5A, 0xF4,
    0x1F, 0xDD, 0xA8, 0x33, 0x88, 0x07, 0xC7, 0x31, 0xB1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xEC, 0x5F,
    0x60, 0x51, 0x7F, 0xA9, 0x19, 0xB5, 0x4A, 0x0D, 0x2D, 0xE5, 0x7A, 0x9F, 0x93, 0xC9, 0x9C, 0xEF,
    0xA0, 0xE0, 0x3B, 0x4D, 0xAE, 0x2A, 0xF5, 0xB0, 0xC8, 0xEB, 0xBB, 0x3C, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2B, 0x04, 0x7E, 0xBA, 0x77, 0xD6, 0x26, 0xE1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0C, 0x7D,
};

/* multiplication by x in GF(2^8) modulo x^8 + x^4 + x^3 + x + 1 */
static u8 aes_xtime(u8 a) { return (u8)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }
static u8 aes_mul(u8 a, u8 k) {
    u8 r = 0;
    while (k) {
        if (k & 1) r ^= a;
        a = aes_xtime(a);
        k >>= 1;
    }
    return r;
}

/* The state's byte i is row i % 4 of column i / 4, so ShiftRows takes byte i from the column i / 4 + i % 4 to its
   right (to its left for the inverse). AESE: SubBytes(ShiftRows(d ^ k)); AESD: InvSubBytes(InvShiftRows(d ^ k)). */
void v_aes(V128* d, const V128* k, int dec) {
    static const u8 shift[16] = {0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11},
                    inv_shift[16] = {0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3};
    V128 t;
    for (int i = 0; i < 16; ++i) t.b[i] = d->b[i] ^ k->b[i];
    for (int i = 0; i < 16; ++i) d->b[i] = dec ? aes_inv_sbox[t.b[inv_shift[i]]] : aes_sbox[t.b[shift[i]]];
}

/* MixColumns (circulant 2 3 1 1) or InvMixColumns (14 11 13 9) of each column */
void v_aesmc(V128* d, const V128* s, int inv) {
    static const u8 forward[4] = {2, 3, 1, 1}, inverse[4] = {14, 11, 13, 9};
    const u8* m = inv ? inverse : forward;
    V128 t = *s;
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            u8 r = 0;
            for (int k = 0; k < 4; ++k) r ^= aes_mul(t.b[col * 4 + k], m[(k - row + 4) & 3]);
            d->b[col * 4 + row] = r;
        }
}

static u32 sha_rol(u32 x, u32 s) { return (x << s) | (x >> (32 - s)); }
static u32 sha_ror(u32 x, u32 s) { return (x >> s) | (x << (32 - s)); }
static u32 sha_choose(u32 x, u32 y, u32 z) { return (x & (y ^ z)) ^ z; }
static u32 sha_parity(u32 x, u32 y, u32 z) { return x ^ y ^ z; }
static u32 sha_majority(u32 x, u32 y, u32 z) { return (x & y) | ((x | y) & z); }

/* the register's 32-bit words as numbers in place (SHA works on them): a swap of each on a big-endian host, where
   V128 holds the guest's bytes, and nothing on a little-endian one. Its own inverse */
static V128 sha_words(V128 v) {
    for (u32 i = 0; i < 4; ++i) v.s[i] = SNX_VS(v, i);
    return v;
}

/* SHA1C/SHA1P/SHA1M: four rounds on the hash in Qd (words 0-3) and e in Sn, with the schedule words of Vm;
   SHA1SU0, SHA1H, SHA1SU1: the schedule updates. Operands are read before Vd is written (they may alias). */
void v_sha1(CPU* c, u32 op, u32 d, u32 n, u32 m) {
    V128 x = sha_words(c->v[d]), y = sha_words(c->v[n]), z = sha_words(c->v[m]);
    u32 e;
    switch (op) {
        case 0:
        case 1:
        case 2:
            e = y.s[0];
            for (u32 i = 0; i < 4; ++i) {
                u32 t = op == 0   ? sha_choose(x.s[1], x.s[2], x.s[3])
                    : op == 1 ? sha_parity(x.s[1], x.s[2], x.s[3])
                              : sha_majority(x.s[1], x.s[2], x.s[3]);
                t += sha_rol(x.s[0], 5) + e + z.s[i];
                e = x.s[3];
                x.s[3] = x.s[2];
                x.s[2] = sha_ror(x.s[1], 2);
                x.s[1] = x.s[0];
                x.s[0] = t;
            }
            break;
        case 3: /* SHA1SU0: (Vn<63:0>:Vd<127:64>) ^ Vd ^ Vm */
            x.d[0] = x.d[1] ^ x.d[0] ^ z.d[0];
            x.d[1] = y.d[0] ^ x.d[1] ^ z.d[1];
            break;
        case 0x10: /* SHA1H: ROL(Sn, 30) */
            x.s[0] = sha_ror(y.s[0], 2);
            x.s[1] = x.s[2] = x.s[3] = 0;
            break;
        case 0x11: /* SHA1SU1 */
            x.s[0] = sha_rol(x.s[0] ^ y.s[1], 1);
            x.s[1] = sha_rol(x.s[1] ^ y.s[2], 1);
            x.s[2] = sha_rol(x.s[2] ^ y.s[3], 1);
            x.s[3] = sha_rol(x.s[3] ^ x.s[0], 1);
            break;
        default: return;
    }
    c->v[d] = sha_words(x);
}

static u32 sha_sigma0(u32 x) { return sha_ror(x, 2) ^ sha_ror(x, 13) ^ sha_ror(x, 22); }
static u32 sha_sigma1(u32 x) { return sha_ror(x, 6) ^ sha_ror(x, 11) ^ sha_ror(x, 25); }
static u32 sha_small0(u32 x) { return sha_ror(x, 7) ^ sha_ror(x, 18) ^ (x >> 3); }
static u32 sha_small1(u32 x) { return sha_ror(x, 17) ^ sha_ror(x, 19) ^ (x >> 10); }

/* SHA256H (Qd = a..d, Qn = e..h), SHA256H2 (Qd = e..h, Qn = a..d): four rounds with Vm's words; SHA256SU0 and
   SHA256SU1: the schedule updates */
void v_sha256(CPU* c, u32 op, u32 d, u32 n, u32 m) {
    V128 x = sha_words(c->v[d]), y = sha_words(c->v[n]), z = sha_words(c->v[m]);
    switch (op) {
        case 4: /* SHA256H: Vd gets the new a..d */
            for (u32 i = 0; i < 4; ++i) {
                u32 t = sha_choose(y.s[0], y.s[1], y.s[2]) + y.s[3] + sha_sigma1(y.s[0]) + z.s[i];
                y.s[3] = y.s[2];
                y.s[2] = y.s[1];
                y.s[1] = y.s[0];
                y.s[0] = x.s[3] + t;
                t += sha_majority(x.s[0], x.s[1], x.s[2]) + sha_sigma0(x.s[0]);
                x.s[3] = x.s[2];
                x.s[2] = x.s[1];
                x.s[1] = x.s[0];
                x.s[0] = t;
            }
            break;
        case 5: /* SHA256H2: Vd gets the new e..h */
            for (u32 i = 0; i < 4; ++i) {
                u32 t = sha_choose(x.s[0], x.s[1], x.s[2]) + x.s[3] + sha_sigma1(x.s[0]) + z.s[i];
                x.s[3] = x.s[2];
                x.s[2] = x.s[1];
                x.s[1] = x.s[0];
                x.s[0] = y.s[3 - i] + t;
            }
            break;
        case 6: /* SHA256SU1 */
            x.s[0] += sha_small1(z.s[2]) + y.s[1];
            x.s[1] += sha_small1(z.s[3]) + y.s[2];
            x.s[2] += sha_small1(x.s[0]) + y.s[3];
            x.s[3] += sha_small1(x.s[1]) + z.s[0];
            break;
        case 0x12: /* SHA256SU0 */
            x.s[0] += sha_small0(x.s[1]);
            x.s[1] += sha_small0(x.s[2]);
            x.s[2] += sha_small0(x.s[3]);
            x.s[3] += sha_small0(y.s[0]);
            break;
        default: return;
    }
    c->v[d] = sha_words(x);
}

/* PMULL/PMULL2 of 64-bit lanes: the carry-less product of the low (or high) doublewords, 128 bits. With x split into
   two 32-bit halves and y into four classes (one bit in four), each 8-by-16-bit product's output bit counts at most 8
   pairs, so a class's counts fit in four bits and no carry reaches the next position of that class; the classes and
   halves are XORed back. 32 multiplies (64 by 64 to 128 bits, bits.h's), no data-dependent branches */
void v_pmull(V128* d, const V128* a, const V128* b, int hi) {
    static const u64 m = 0x1111111111111111ULL;
    u64 x = SNX_VD(*a, hi ? 1 : 0), y = SNX_VD(*b, hi ? 1 : 0), r_lo = 0, r_hi = 0;
    for (int h = 0; h < 2; ++h) {
        u64 half = h ? x >> 32 : x & 0xFFFFFFFFULL, part_lo = 0, part_hi = 0;
        for (int c = 0; c < 4; ++c) {
            u64 z_lo = 0, z_hi = 0;
            for (int i = 0; i < 4; ++i) {
                u64 p_hi, p_lo = snx_mul_wide(half & (m << i), y & (m << ((c - i) & 3)), &p_hi);
                z_lo ^= p_lo;
                z_hi ^= p_hi;
            }
            part_lo |= z_lo & (m << c);
            part_hi |= z_hi & (m << c);
        }
        r_lo ^= h ? part_lo << 32 : part_lo;
        r_hi ^= h ? part_hi << 32 | part_lo >> 32 : part_hi;
    }
    SNX_VSET_D(*d, 0, r_lo);
    SNX_VSET_D(*d, 1, r_hi);
}

#endif
