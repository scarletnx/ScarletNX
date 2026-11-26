/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the x86-64 back end's code calls (tools/sailgen/x64_a64.h) rather than writes inline: the exclusive monitor
   (cpu.h's ldx, stx, ldxp, stxp), the LSE atomics, CRC32 and CRC32C, DC ZVA. The LLVM back end builds its own
   helpers into each module as IR (ir_helpers.c); these are C, in the runtime library every module links (rtlib.c
   includes this), and in the x86-64 back end's test directly. Each takes the state first, as its code has it in RBX */
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/bits.h"
#include <stdatomic.h>
#include <string.h>


u64 snx_x64_fp_binary(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_fma(CPU* c, u64 add, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_unary(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_minmax(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_compare(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_convert(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_round(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_round_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_to_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_from_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_jcvt(CPU* c, u64 a, u64 fpcr);
u64 snx_x64_fp_bfcvt(CPU* c, u64 a, u64 fpcr);
void snx_x64_fp_vec(CPU* c, void* out, const void* x, const void* y, const void* z, u32 desc);
void snx_x64_crypto(CPU* c, void* out, const void* x, const void* y, const void* z, u32 op);
void snx_x64_sse(CPU* c, void* out, const void* x, const void* y, const void* z, u32 desc);
u64 snx_x64_aes(u64 x, u32 op);
u64 snx_x64_estimate(u64 a, u32 sqrt);
u64 snx_x64_half(u64 a, u64 fpcr);
u64 snx_x64_bfdot(u64 sum, u64 a, u64 b);
u64 snx_x64_sm4_box(u64 a);
u64 snx_x64_fp_scale(CPU* c, u64 a, u64 by, u64 fpcr, u32 desc);
u64 snx_x64_fp8_widen(CPU* c, u64 a, u32 bfloat);
u64 snx_x64_fp8_narrow(CPU* c, u64 a, u32 width);
u64 snx_x64_fp8_dot(u64 add, u64 a, u64 b, u32 desc);
void snx_x64_set(CPU* c, u64 a, u64 size, u64 value);
void snx_x64_copy(CPU* c, u64 to, u64 from, u64 size);
extern const u32 snx_x64_crc32_table[8 * 256];
extern const u32 snx_x64_crc32c_table[256];

/* ---- floating point: cpu.h's helpers, which LLVM's slow paths call, with the state ----
   Each takes the guest's FPCR as the code read it (fpcr, bits 0-31), ORs the FPSR flags the operation raised into
   CPU.fpsr and gives the value. Bit 32 of fpcr says the instruction's Sail doesn't run the operation here (gen.c's
   run-time path: FCCMP's compare where the condition fails): then nothing happens, and the value is 0. The rest of
   the operation's parameters are constants of the instruction, packed into desc: width bits 0-7, the rest per
   helper. A rounding is rm + 1 (FPRounding's 0-4), 0 for FPCR's mode */
static inline int snx_x64_rm(u32 desc_rm, u64 fpcr) { return desc_rm ? (int)desc_rm - 1 : (int)((fpcr >> 22) & 3); }
static inline u64 snx_x64_fp_done(CPU* c, SnxFpReply r) {
    c->fpsr |= r.flags;
    return r.value;
}
/* desc: width, op << 8 */
u64 snx_x64_fp_binary(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_host_binary(a, b, (int)(desc & 0xFF), (int)(desc >> 8), (u32)fpcr, c->fpsr));
}
/* add + a * b; desc: width */
u64 snx_x64_fp_fma(CPU* c, u64 add, u64 a, u64 b, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_host_fma(add, a, b, (int)(desc & 0xFF), (u32)fpcr));
}
/* desc: width, op << 8 */
u64 snx_x64_fp_unary(CPU* c, u64 a, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_host_unary(a, (int)(desc & 0xFF), (int)(desc >> 8), (u32)fpcr));
}
/* desc: width, minimum << 8, numeric << 9 */
u64 snx_x64_fp_minmax(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_host_minmax(a, b, (int)(desc & 0xFF), (int)(desc >> 8 & 1), (int)(desc >> 9 & 1),
                                                 (u32)fpcr));
}
/* the NZCV nibble, or (desc bit 9) the flags the native compare's cold path loads; desc: width, signal << 8 */
u64 snx_x64_fp_compare(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc) {
    u64 nzcv;
    if (fpcr >> 32) return 0;
    nzcv = snx_x64_fp_done(c, snx_fp_host_compare(a, b, (int)(desc & 0xFF), (int)(desc >> 8 & 1), (u32)fpcr));
    if (!(desc >> 9 & 1)) return nzcv;
    /* RFLAGS as UCOMISx b, a leaves them for this NZCV (x64_a64.h's sg_FPCompare): CF where C and not Z (a > b, or
       unordered), ZF where Z or V, PF where V; bits 1 and 9 (IF) set as they always are here */
    return 0x202u | (nzcv >> 1 & ~nzcv >> 2 & 1) | (nzcv & 1) << 2 | ((nzcv >> 2 | nzcv) & 1) << 6;
}
/* desc: from, to << 8, rm + 1 << 16 */
u64 snx_x64_fp_convert(CPU* c, u64 a, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_convert(a, (int)(desc & 0xFF), (int)(desc >> 8 & 0xFF),
                                             snx_x64_rm(desc >> 16 & 7, fpcr), (u32)fpcr));
}
/* FRINT*; desc: width, rm + 1 << 8, exact << 12 */
u64 snx_x64_fp_round(CPU* c, u64 a, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_round(a, (int)(desc & 0xFF), snx_x64_rm(desc >> 8 & 7, fpcr),
                                           (int)(desc >> 12 & 1), (u32)fpcr));
}
/* FRINT32*, FRINT64*; desc: width, rm + 1 << 8, intsize << 16 */
u64 snx_x64_fp_round_int(CPU* c, u64 a, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_round_int(a, (int)(desc & 0xFF), snx_x64_rm(desc >> 8 & 7, fpcr),
                                               (int)(desc >> 16 & 0xFF), (u32)fpcr));
}
/* desc: fp width, int width << 8, uns << 16, fbits << 17 (7 bits), rm + 1 << 24 */
u64 snx_x64_fp_to_int(CPU* c, u64 a, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_to_int(a, (int)(desc & 0xFF), (int)(desc >> 8 & 0xFF), (int)(desc >> 16 & 1),
                                            (int)(desc >> 17 & 0x7F), snx_x64_rm(desc >> 24 & 7, fpcr), (u32)fpcr));
}
/* desc: int width, fp width << 8, uns << 16, fbits << 17 (7 bits) */
u64 snx_x64_fp_from_int(CPU* c, u64 a, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_from_int(a, (int)(desc & 0xFF), (int)(desc >> 8 & 0xFF), (int)(desc >> 16 & 1),
                                              (int)(desc >> 17 & 0x7F), (u32)fpcr));
}
/* FJCVTZS: the W result, Z in bit 32 */
u64 snx_x64_fp_jcvt(CPU* c, u64 a, u64 fpcr) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_jcvt(a, (u32)fpcr));
}
u64 snx_x64_fp_bfcvt(CPU* c, u64 a, u64 fpcr) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_bfcvt(a, (u32)fpcr));
}

/* a vector's lanes, the cold path of x64_vec.c's: desc's kind (bits 0-3) 0 to 3 are snx_fp_host_lanes' (its desc as it
   takes it); the others lane by lane through the helpers LLVM's lanes call (ir_vec_fp.c's map), of bytes (bits 4-7)
   and lanes (8-11): 4 a compare, cmp (12-13: 0 EQ, 1 GE, 2 GT) of the magnitudes where abs (16), all ones where it
   holds; 5 a conversion to the width of bytes (12-15); 6 to integers of the lanes' width, uns (12), fbits (16-22),
   rm + 1 (24-26, 0 FPCR's); 7 from integers, uns (12), fbits (16-22); 8 FRINT, rm + 1 (12-14), exact (16) */
static inline u64 snx_x64_lane_get(const void* p, unsigned i, unsigned bytes) {
    u64 v = 0;
    memcpy(&v, (const char*)p + i * bytes, bytes);
    return v;
}
static inline void snx_x64_lane_put(void* p, unsigned i, unsigned bytes, u64 v) { memcpy((char*)p + i * bytes, &v, bytes); }
void snx_x64_fp_vec(CPU* c, void* out, const void* x, const void* y, const void* z, u32 desc) {
    unsigned kind = desc & 15, bytes = desc >> 4 & 15, lanes = desc >> 8 & 15, i, w = bytes * 8;
    unsigned char r[16];
    if (kind <= 3) {
        c->fpsr = snx_fp_host_lanes(out, x, y, z, desc, c->fpcr, c->fpsr);
        return;
    }
    memset(r, 0, sizeof r);
    for (i = 0; i < lanes; ++i) {
        u64 a = snx_x64_lane_get(x, i, bytes), b = snx_x64_lane_get(y, i, bytes);
        SnxFpReply rep;
        switch (kind) {
        case 4: {
            unsigned cmp = desc >> 12 & 3;
            u64 nz;
            if (desc >> 16 & 1) {
                a &= ~(1ull << (w - 1));
                b &= ~(1ull << (w - 1));
            }
            rep = snx_fp_compare(a, b, (int)w, cmp != 0, c->fpcr);
            nz = rep.value;
            rep.value = (cmp == 0 ? nz == 6 : cmp == 1 ? (nz & 0xB) == 2 : nz == 2) ? ~0ull : 0;
            snx_x64_lane_put(r, i, bytes, rep.value);
            break;
        }
        case 5: {
            unsigned to = desc >> 12 & 15;
            rep = snx_fp_convert(a, (int)w, (int)to * 8, (int)((c->fpcr >> 22) & 3), c->fpcr);
            snx_x64_lane_put(r, i, to, rep.value);
            break;
        }
        case 6: {
            unsigned rm1 = desc >> 24 & 7;
            rep = snx_fp_to_int(a, (int)w, (int)w, (int)(desc >> 12 & 1), (int)(desc >> 16 & 127),
                                rm1 ? (int)rm1 - 1 : (int)((c->fpcr >> 22) & 3), c->fpcr);
            snx_x64_lane_put(r, i, bytes, rep.value);
            break;
        }
        case 7:
            rep = snx_fp_from_int(a, (int)w, (int)w, (int)(desc >> 12 & 1), (int)(desc >> 16 & 127), c->fpcr);
            snx_x64_lane_put(r, i, bytes, rep.value);
            break;
        default: {
            unsigned rm1 = desc >> 12 & 7;
            rep = snx_fp_round(a, (int)w, rm1 ? (int)rm1 - 1 : (int)((c->fpcr >> 22) & 3), (int)(desc >> 16 & 1),
                               c->fpcr);
            snx_x64_lane_put(r, i, bytes, rep.value);
            break;
        }
        }
        c->fpsr |= rep.flags;
        (void)b;
    }
    memcpy(out, r, 16);
}

/* the cryptographic instructions where the host has none for them: out from Vd's, Vn's and Vm's values (x, y, z); op
   v_sha1's or v_sha256's, or from 0x20 AESE, AESD (x with the key y), AESMC, AESIMC (of x) */
void snx_x64_crypto(CPU* c, void* out, const void* x, const void* y, const void* z, u32 op) {
    CPU s; /* the three registers alone */
    (void)c;
    memcpy(&s.v[0], x, 16);
    memcpy(&s.v[1], y, 16);
    memcpy(&s.v[2], z, 16);
    if (op >= 0x22)
        v_aesmc(&s.v[0], &s.v[0], (int)op - 0x22);
    else if (op >= 0x20)
        v_aes(&s.v[0], &s.v[1], (int)op - 0x20);
    else if (op == 0x12 || (op >= 4 && op <= 6))
        v_sha256(&s, op, 0, 1, 2);
    else
        v_sha1(&s, op, 0, 1, 2);
    memcpy(out, &s.v[0], 16);
}

/* an SSE instruction where the host has none of it (tools/sailgen/x64.h's sgx_vf_helped_forms, whose places are the
   numbers below): out = the instruction of x (its first operand) and y (its second), desc its number and its
   immediate (bits 8 to 15) */
enum {
    SSE_PCLMULQDQ,
    SSE_PCMPGTQ,
    SSE_PSHUFB,
    SSE_PALIGNR,
    SSE_PABSB,
    SSE_PABSW,
    SSE_PABSD,
    SSE_PMULHRSW,
    SSE_PMULLD,
    SSE_PMULDQ,
    SSE_PCMPEQQ,
    SSE_PMAXSB,
    SSE_PMAXSD,
    SSE_PMAXUW,
    SSE_PMAXUD,
    SSE_PMINSB,
    SSE_PMINSD,
    SSE_PMINUW,
    SSE_PMINUD,
    SSE_PMOVSXBW,
    SSE_PMOVSXWD,
    SSE_PMOVSXDQ,
    SSE_PMOVZXBW,
    SSE_PMOVZXWD,
    SSE_PMOVZXDQ,
    SSE_ROUNDPS,
    SSE_ROUNDPD
};
/* element k of bits bits of v; v with it e */
static u64 sse_get(const u64* v, unsigned bits, unsigned k) {
    return bits == 64 ? v[k] : v[k * bits / 64] >> (k * bits % 64) & ((1ull << bits) - 1);
}
static void sse_put(u64* v, unsigned bits, unsigned k, u64 e) {
    if (bits == 64)
        v[k] = e;
    else
        v[k * bits / 64] |= (e & ((1ull << bits) - 1)) << (k * bits % 64);
}
/* e of bits bits sign extended, as unsigned; biased so that unsigned order is the signed one */
static u64 sse_sext(u64 e, unsigned bits) { return (e ^ 1ull << (bits - 1)) - (1ull << (bits - 1)); }
static u64 sse_biased(u64 e, unsigned bits) { return sse_sext(e, bits) ^ 1ull << 63; }
/* ROUNDPS's and ROUNDPD's element v of a binary format (fraction bits f, exponent bits x) to an integer in mode: 0 to
   nearest even, 1 down, 2 up, 3 toward zero; a NaN quieted, as the instruction does */
static u64 sse_round(u64 v, unsigned f, unsigned x, unsigned mode) {
    const unsigned bias = (1u << (x - 1)) - 1, e = (unsigned)(v >> f) & ((1u << x) - 1);
    const u64 sign = v >> (f + x) & 1, one = (u64)bias << f, magnitude = v & ((1ull << (f + x)) - 1);
    if (e == (1u << x) - 1) return v & ((1ull << f) - 1) ? v | 1ull << (f - 1) : v;
    if (e >= bias + f) return v; /* no fraction */
    /* a magnitude a unit up: past a half to nearest (a half to an even one), any part away from zero down or up */
    if (e < bias) { /* below 1: a zero of its sign, or one */
        const int half_up = e == bias - 1 && (v & ((1ull << f) - 1));
        const int up = mode == 0 ? half_up : mode != 3 && magnitude && sign == (mode == 1);
        return sign << (f + x) | (up ? one : 0);
    }
    const unsigned below = bias + f - e;
    const u64 mask = (1ull << below) - 1, part = v & mask, half = 1ull << (below - 1);
    u64 r = v & ~mask;
    const int up = mode == 0 ? part > half || (part == half && (r >> below & 1)) : mode != 3 && sign == (mode == 1);
    if (part && up) r += 1ull << below; /* the fraction's carry goes into the exponent */
    return r;
}
void snx_x64_sse(CPU* c, void* out, const void* x, const void* y, const void* z, u32 desc) {
    const unsigned imm = desc >> 8 & 0xFF;
    u64 a[2], b[2], r[2] = {0, 0};
    (void)c, (void)z;
    memcpy(a, x, 16);
    memcpy(b, y, 16);
    switch (desc & 0xFF) {
        case SSE_PCLMULQDQ: { /* the carryless product of x's and y's doublewords imm bit 0 and bit 4 choose */
            const u64 p = a[imm & 1], q = b[imm >> 4 & 1];
            for (unsigned k = 0; k < 64; ++k)
                if (q >> k & 1) {
                    r[0] ^= p << k;
                    if (k) r[1] ^= p >> (64 - k);
                }
            break;
        }
        case SSE_PCMPGTQ: /* each doubleword of x above y's, signed (both biased by the sign bit): all ones */
            for (unsigned k = 0; k < 2; ++k) r[k] = (a[k] ^ 1ull << 63) > (b[k] ^ 1ull << 63) ? ~0ull : 0;
            break;
        case SSE_PSHUFB: { /* each byte x's that y's byte names (its low 4 bits), 0 where its top bit is set */
            unsigned char p[16], q[16], o[16];
            memcpy(p, a, 16), memcpy(q, b, 16);
            for (unsigned k = 0; k < 16; ++k) o[k] = q[k] & 0x80 ? 0 : p[q[k] & 15];
            memcpy(r, o, 16);
            break;
        }
        case SSE_PALIGNR: { /* x above y, 32 bytes, imm bytes right, the low 16 */
            unsigned char t[32], o[16];
            memcpy(t, b, 16), memcpy(t + 16, a, 16);
            for (unsigned k = 0; k < 16; ++k) o[k] = k + imm < 32 ? t[k + imm] : 0;
            memcpy(r, o, 16);
            break;
        }
        case SSE_PABSB: /* each element of y made positive (the most negative stays itself, as unsigned) */
        case SSE_PABSW:
        case SSE_PABSD: {
            const unsigned bits = 8u << ((desc & 0xFF) - SSE_PABSB), lanes = 128 / bits;
            const u64 mask = bits == 64 ? ~0ull : (1ull << bits) - 1, sign = 1ull << (bits - 1);
            for (unsigned k = 0; k < lanes; ++k) {
                const unsigned at = k * bits % 64;
                const u64 e = b[k * bits / 64] >> at & mask;
                r[k * bits / 64] |= (e & sign ? (0 - e) & mask : e) << at;
            }
            break;
        }
        case SSE_PMULLD: /* each word pair's product, its low word */
            for (unsigned k = 0; k < 4; ++k) sse_put(r, 32, k, sse_get(a, 32, k) * sse_get(b, 32, k));
            break;
        case SSE_PMULDQ: /* words 0 and 2's signed product, a doubleword each */
            for (unsigned k = 0; k < 2; ++k)
                r[k] = sse_sext(sse_get(a, 32, 2 * k), 32) * sse_sext(sse_get(b, 32, 2 * k), 32);
            break;
        case SSE_PCMPEQQ:
            for (unsigned k = 0; k < 2; ++k) r[k] = a[k] == b[k] ? ~0ull : 0;
            break;
        case SSE_PMAXSB: /* the greater of each pair, signed (the bias) or not */
        case SSE_PMAXSD:
        case SSE_PMAXUW:
        case SSE_PMAXUD:
        case SSE_PMINSB:
        case SSE_PMINSD:
        case SSE_PMINUW:
        case SSE_PMINUD: {
            static const unsigned char bits[8] = {8, 32, 16, 32, 8, 32, 16, 32};
            const unsigned op = (desc & 0xFF) - SSE_PMAXSB, e = bits[op], sign = op % 4 < 2, max = op < 4;
            for (unsigned k = 0; k < 128 / e; ++k) {
                const u64 x = sse_get(a, e, k), y = sse_get(b, e, k);
                const u64 kx = sign ? sse_biased(x, e) : x, ky = sign ? sse_biased(y, e) : y;
                sse_put(r, e, k, (kx > ky) == max ? x : y);
            }
            break;
        }
        case SSE_PMOVSXBW: /* the low elements of y, each widened, sign extended or not */
        case SSE_PMOVSXWD:
        case SSE_PMOVSXDQ:
        case SSE_PMOVZXBW:
        case SSE_PMOVZXWD:
        case SSE_PMOVZXDQ: {
            const unsigned op = (desc & 0xFF) - SSE_PMOVSXBW, e = 8u << op % 3;
            for (unsigned k = 0; k < 64 / e; ++k) {
                const u64 v = sse_get(b, e, k);
                sse_put(r, 2 * e, k, op < 3 ? sse_sext(v, e) : v);
            }
            break;
        }
        case SSE_ROUNDPS: /* each element of y to an integer, in imm's mode (bit 2: MXCSR's, the default's nearest) */
        case SSE_ROUNDPD: {
            const unsigned d = (desc & 0xFF) == SSE_ROUNDPD, e = d ? 64 : 32, mode = imm & 4 ? 0 : imm & 3;
            for (unsigned k = 0; k < 128 / e; ++k)
                sse_put(r, e, k, sse_round(sse_get(b, e, k), d ? 52 : 23, d ? 11 : 8, mode));
            break;
        }
        case SSE_PMULHRSW: /* each halfword pair's signed product, 2^-15 of it rounded: ((x * y >> 14) + 1) >> 1 */
            for (unsigned k = 0; k < 8; ++k) {
                const u32 x16 = (u32)(a[k / 4] >> (k % 4 * 16)) & 0xFFFF,
                          y16 = (u32)(b[k / 4] >> (k % 4 * 16)) & 0xFFFF;
                const u32 p = (u32)(((x16 ^ 0x8000u) - 0x8000u) * ((y16 ^ 0x8000u) - 0x8000u)); /* two's complement */
                r[k / 4] |= (u64)((snx_asr32(p, 14) + 1) >> 1 & 0xFFFF) << (k % 4 * 16);
            }
            break;
        default: break;
    }
    memcpy(out, r, 16);
}

/* AES's SubBytes (op 0) and its inverse (1) of eight bytes, MixColumns (2) and its inverse (3) of two columns: cpu.h's
   rounds, whose ShiftRows (the bytes go where it brings them from) is undone */
u64 snx_x64_aes(u64 x, u32 op) {
    static const u8 from[2][8] = {{0, 5, 10, 15, 4, 9, 14, 3}, {0, 13, 10, 7, 4, 1, 14, 11}};
    V128 t = {0}, key = {0};
    u64 r = 0;
    for (unsigned i = 0; i < 8; ++i) t.b[op < 2 ? from[op][i] : i] = (u8)(x >> (8 * i));
    if (op < 2) v_aes(&t, &key, (int)op);
    else
        v_aesmc(&t, &t, (int)op - 2);
    for (unsigned i = 0; i < 8; ++i) r |= (u64)t.b[i] << (8 * i);
    return r;
}
/* URECPE, URSQRTE (sqrt) of a word */
u64 snx_x64_estimate(u64 a, u32 sqrt) { return sqrt ? ursqrte32((u32)a) : urecpe32((u32)a); }
/* a half's single as FMLAL multiplies it: exact, a signalling NaN's bits kept, a denormal a zero under FPCR.FZ16 */
u64 snx_x64_half(u64 a, u64 fpcr) {
    u64 sign = (a >> 15 & 1) << 31, exp = a >> 10 & 31, frac = a & 0x3FF;
    if (exp == 31) return sign | 0xFFull << 23 | frac << 13;
    if (!exp) {
        if (!frac || fpcr >> 19 & 1) return sign;
        for (exp = 1; !(frac >> 10); --exp) frac <<= 1;
        frac &= 0x3FF;
    }
    return sign | ((exp + 127 - 15) & 0xFF) << 23 | frac << 13;
}
/* BFDOT's step: sum plus the products of two pairs of bfloat16s */
u64 snx_x64_bfdot(u64 sum, u64 a, u64 b) { return snx_vec_bfdot(NULL, (u32)sum, (u32)a, (u32)b); }
/* a times 2^by, by of a's width signed; desc: width */
u64 snx_x64_fp_scale(CPU* c, u64 a, u64 by, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_scale(a, snx_sext64(by, desc), (int)desc, (u32)fpcr));
}
/* FEAT_FP8's: a byte's half or bfloat16, a half's or a single's byte; the sum of add and products, desc: width,
   products << 8 */
u64 snx_x64_fp8_widen(CPU* c, u64 a, u32 bfloat) { return snx_x64_fp_done(c, snx_fp8_widen(a, (int)bfloat)); }
u64 snx_x64_fp8_narrow(CPU* c, u64 a, u32 width) { return snx_x64_fp_done(c, snx_fp8_narrow(a, (int)width)); }
u64 snx_x64_fp8_dot(u64 add, u64 a, u64 b, u32 desc) {
    return snx_fp8_dot(add, a, b, (int)(desc >> 8), (int)(desc & 0xFF));
}
/* SM4's substitution of a byte */
u64 snx_x64_sm4_box(u64 a) {
    static const u8 box[256] = {
        0xd6, 0x90, 0xe9, 0xfe, 0xcc, 0xe1, 0x3d, 0xb7, 0x16, 0xb6, 0x14, 0xc2, 0x28, 0xfb, 0x2c, 0x05, 0x2b, 0x67,
        0x9a, 0x76, 0x2a, 0xbe, 0x04, 0xc3, 0xaa, 0x44, 0x13, 0x26, 0x49, 0x86, 0x06, 0x99, 0x9c, 0x42, 0x50, 0xf4,
        0x91, 0xef, 0x98, 0x7a, 0x33, 0x54, 0x0b, 0x43, 0xed, 0xcf, 0xac, 0x62, 0xe4, 0xb3, 0x1c, 0xa9, 0xc9, 0x08,
        0xe8, 0x95, 0x80, 0xdf, 0x94, 0xfa, 0x75, 0x8f, 0x3f, 0xa6, 0x47, 0x07, 0xa7, 0xfc, 0xf3, 0x73, 0x17, 0xba,
        0x83, 0x59, 0x3c, 0x19, 0xe6, 0x85, 0x4f, 0xa8, 0x68, 0x6b, 0x81, 0xb2, 0x71, 0x64, 0xda, 0x8b, 0xf8, 0xeb,
        0x0f, 0x4b, 0x70, 0x56, 0x9d, 0x35, 0x1e, 0x24, 0x0e, 0x5e, 0x63, 0x58, 0xd1, 0xa2, 0x25, 0x22, 0x7c, 0x3b,
        0x01, 0x21, 0x78, 0x87, 0xd4, 0x00, 0x46, 0x57, 0x9f, 0xd3, 0x27, 0x52, 0x4c, 0x36, 0x02, 0xe7, 0xa0, 0xc4,
        0xc8, 0x9e, 0xea, 0xbf, 0x8a, 0xd2, 0x40, 0xc7, 0x38, 0xb5, 0xa3, 0xf7, 0xf2, 0xce, 0xf9, 0x61, 0x15, 0xa1,
        0xe0, 0xae, 0x5d, 0xa4, 0x9b, 0x34, 0x1a, 0x55, 0xad, 0x93, 0x32, 0x30, 0xf5, 0x8c, 0xb1, 0xe3, 0x1d, 0xf6,
        0xe2, 0x2e, 0x82, 0x66, 0xca, 0x60, 0xc0, 0x29, 0x23, 0xab, 0x0d, 0x53, 0x4e, 0x6f, 0xd5, 0xdb, 0x37, 0x45,
        0xde, 0xfd, 0x8e, 0x2f, 0x03, 0xff, 0x6a, 0x72, 0x6d, 0x6c, 0x5b, 0x51, 0x8d, 0x1b, 0xaf, 0x92, 0xbb, 0xdd,
        0xbc, 0x7f, 0x11, 0xd9, 0x5c, 0x41, 0x1f, 0x10, 0x5a, 0xd8, 0x0a, 0xc1, 0x31, 0x88, 0xa5, 0xcd, 0x7b, 0xbd,
        0x2d, 0x74, 0xd0, 0x12, 0xb8, 0xe5, 0xb4, 0xb0, 0x89, 0x69, 0x97, 0x4a, 0x0c, 0x96, 0x77, 0x7e, 0x65, 0xb9,
        0xf1, 0x09, 0xc5, 0x6e, 0xc6, 0x84, 0x18, 0xf0, 0x7d, 0xec, 0x3a, 0xdc, 0x4d, 0x20, 0x79, 0xee, 0x5f, 0x3e,
        0xd7, 0xcb, 0x39, 0x48};
    return box[a & 255];
}

/* FEAT_MOPS's SET and CPY: size bytes of guest memory set to value's low byte, or copied as if through another place */
void snx_x64_set(CPU* c, u64 a, u64 size, u64 value) {
    u8* M = c->mem;
    if (size) memset(MP(a), (int)(value & 255), (size_t)size);
}
void snx_x64_copy(CPU* c, u64 to, u64 from, u64 size) {
    u8* M = c->mem;
    if (size) memmove(MP(to), MP(from), (size_t)size);
}

/* CRC32's (not CRC32C's) reflected tables: [k * 256 + b] the CRC of the byte b followed by k zero bytes. The x86-64
   back end's CRC32 of a byte reads the first (x64_a64.h's SGX_M_CRC); of a halfword, a word or a doubleword, the first
   two, four or eight at once, one lookup a byte; RIP-relative from the code */
const u32 snx_x64_crc32_table[8 * 256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u, 0x0EDB8832u,
    0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u, 0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
    0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u, 0x136C9856u, 0x646BA8C0u, 0xFD62F97Au,
    0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u,
    0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u, 0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
    0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u, 0x2F6F7C87u, 0x58684C11u, 0xC1611DABu,
    0xB6662D3Du, 0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u, 0x6B6B51F4u,
    0x1C6C6162u, 0x856530D8u, 0xF262004Eu, 0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u, 0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u,
    0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u, 0x5768B525u,
    0x206F85B3u, 0xB966D409u, 0xCE61E49Fu, 0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au, 0xEAD54739u, 0x9DD277AFu, 0x04DB2615u,
    0x73DC1683u, 0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u, 0xFED41B76u,
    0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu, 0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
    0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu, 0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u,
    0x41047A60u, 0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u,
    0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du, 0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
    0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u, 0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u,
    0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u, 0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u, 0xA00AE278u,
    0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u, 0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
    0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u, 0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u,
    0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u, 0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du, 0x00000000u, 0x191B3141u, 0x32366282u, 0x2B2D53C3u, 0x646CC504u,
    0x7D77F445u, 0x565AA786u, 0x4F4196C7u, 0xC8D98A08u, 0xD1C2BB49u, 0xFAEFE88Au, 0xE3F4D9CBu, 0xACB54F0Cu, 0xB5AE7E4Du,
    0x9E832D8Eu, 0x87981CCFu, 0x4AC21251u, 0x53D92310u, 0x78F470D3u, 0x61EF4192u, 0x2EAED755u, 0x37B5E614u, 0x1C98B5D7u,
    0x05838496u, 0x821B9859u, 0x9B00A918u, 0xB02DFADBu, 0xA936CB9Au, 0xE6775D5Du, 0xFF6C6C1Cu, 0xD4413FDFu, 0xCD5A0E9Eu,
    0x958424A2u, 0x8C9F15E3u, 0xA7B24620u, 0xBEA97761u, 0xF1E8E1A6u, 0xE8F3D0E7u, 0xC3DE8324u, 0xDAC5B265u, 0x5D5DAEAAu,
    0x44469FEBu, 0x6F6BCC28u, 0x7670FD69u, 0x39316BAEu, 0x202A5AEFu, 0x0B07092Cu, 0x121C386Du, 0xDF4636F3u, 0xC65D07B2u,
    0xED705471u, 0xF46B6530u, 0xBB2AF3F7u, 0xA231C2B6u, 0x891C9175u, 0x9007A034u, 0x179FBCFBu, 0x0E848DBAu, 0x25A9DE79u,
    0x3CB2EF38u, 0x73F379FFu, 0x6AE848BEu, 0x41C51B7Du, 0x58DE2A3Cu, 0xF0794F05u, 0xE9627E44u, 0xC24F2D87u, 0xDB541CC6u,
    0x94158A01u, 0x8D0EBB40u, 0xA623E883u, 0xBF38D9C2u, 0x38A0C50Du, 0x21BBF44Cu, 0x0A96A78Fu, 0x138D96CEu, 0x5CCC0009u,
    0x45D73148u, 0x6EFA628Bu, 0x77E153CAu, 0xBABB5D54u, 0xA3A06C15u, 0x888D3FD6u, 0x91960E97u, 0xDED79850u, 0xC7CCA911u,
    0xECE1FAD2u, 0xF5FACB93u, 0x7262D75Cu, 0x6B79E61Du, 0x4054B5DEu, 0x594F849Fu, 0x160E1258u, 0x0F152319u, 0x243870DAu,
    0x3D23419Bu, 0x65FD6BA7u, 0x7CE65AE6u, 0x57CB0925u, 0x4ED03864u, 0x0191AEA3u, 0x188A9FE2u, 0x33A7CC21u, 0x2ABCFD60u,
    0xAD24E1AFu, 0xB43FD0EEu, 0x9F12832Du, 0x8609B26Cu, 0xC94824ABu, 0xD05315EAu, 0xFB7E4629u, 0xE2657768u, 0x2F3F79F6u,
    0x362448B7u, 0x1D091B74u, 0x04122A35u, 0x4B53BCF2u, 0x52488DB3u, 0x7965DE70u, 0x607EEF31u, 0xE7E6F3FEu, 0xFEFDC2BFu,
    0xD5D0917Cu, 0xCCCBA03Du, 0x838A36FAu, 0x9A9107BBu, 0xB1BC5478u, 0xA8A76539u, 0x3B83984Bu, 0x2298A90Au, 0x09B5FAC9u,
    0x10AECB88u, 0x5FEF5D4Fu, 0x46F46C0Eu, 0x6DD93FCDu, 0x74C20E8Cu, 0xF35A1243u, 0xEA412302u, 0xC16C70C1u, 0xD8774180u,
    0x9736D747u, 0x8E2DE606u, 0xA500B5C5u, 0xBC1B8484u, 0x71418A1Au, 0x685ABB5Bu, 0x4377E898u, 0x5A6CD9D9u, 0x152D4F1Eu,
    0x0C367E5Fu, 0x271B2D9Cu, 0x3E001CDDu, 0xB9980012u, 0xA0833153u, 0x8BAE6290u, 0x92B553D1u, 0xDDF4C516u, 0xC4EFF457u,
    0xEFC2A794u, 0xF6D996D5u, 0xAE07BCE9u, 0xB71C8DA8u, 0x9C31DE6Bu, 0x852AEF2Au, 0xCA6B79EDu, 0xD37048ACu, 0xF85D1B6Fu,
    0xE1462A2Eu, 0x66DE36E1u, 0x7FC507A0u, 0x54E85463u, 0x4DF36522u, 0x02B2F3E5u, 0x1BA9C2A4u, 0x30849167u, 0x299FA026u,
    0xE4C5AEB8u, 0xFDDE9FF9u, 0xD6F3CC3Au, 0xCFE8FD7Bu, 0x80A96BBCu, 0x99B25AFDu, 0xB29F093Eu, 0xAB84387Fu, 0x2C1C24B0u,
    0x350715F1u, 0x1E2A4632u, 0x07317773u, 0x4870E1B4u, 0x516BD0F5u, 0x7A468336u, 0x635DB277u, 0xCBFAD74Eu, 0xD2E1E60Fu,
    0xF9CCB5CCu, 0xE0D7848Du, 0xAF96124Au, 0xB68D230Bu, 0x9DA070C8u, 0x84BB4189u, 0x03235D46u, 0x1A386C07u, 0x31153FC4u,
    0x280E0E85u, 0x674F9842u, 0x7E54A903u, 0x5579FAC0u, 0x4C62CB81u, 0x8138C51Fu, 0x9823F45Eu, 0xB30EA79Du, 0xAA1596DCu,
    0xE554001Bu, 0xFC4F315Au, 0xD7626299u, 0xCE7953D8u, 0x49E14F17u, 0x50FA7E56u, 0x7BD72D95u, 0x62CC1CD4u, 0x2D8D8A13u,
    0x3496BB52u, 0x1FBBE891u, 0x06A0D9D0u, 0x5E7EF3ECu, 0x4765C2ADu, 0x6C48916Eu, 0x7553A02Fu, 0x3A1236E8u, 0x230907A9u,
    0x0824546Au, 0x113F652Bu, 0x96A779E4u, 0x8FBC48A5u, 0xA4911B66u, 0xBD8A2A27u, 0xF2CBBCE0u, 0xEBD08DA1u, 0xC0FDDE62u,
    0xD9E6EF23u, 0x14BCE1BDu, 0x0DA7D0FCu, 0x268A833Fu, 0x3F91B27Eu, 0x70D024B9u, 0x69CB15F8u, 0x42E6463Bu, 0x5BFD777Au,
    0xDC656BB5u, 0xC57E5AF4u, 0xEE530937u, 0xF7483876u, 0xB809AEB1u, 0xA1129FF0u, 0x8A3FCC33u, 0x9324FD72u, 0x00000000u,
    0x01C26A37u, 0x0384D46Eu, 0x0246BE59u, 0x0709A8DCu, 0x06CBC2EBu, 0x048D7CB2u, 0x054F1685u, 0x0E1351B8u, 0x0FD13B8Fu,
    0x0D9785D6u, 0x0C55EFE1u, 0x091AF964u, 0x08D89353u, 0x0A9E2D0Au, 0x0B5C473Du, 0x1C26A370u, 0x1DE4C947u, 0x1FA2771Eu,
    0x1E601D29u, 0x1B2F0BACu, 0x1AED619Bu, 0x18ABDFC2u, 0x1969B5F5u, 0x1235F2C8u, 0x13F798FFu, 0x11B126A6u, 0x10734C91u,
    0x153C5A14u, 0x14FE3023u, 0x16B88E7Au, 0x177AE44Du, 0x384D46E0u, 0x398F2CD7u, 0x3BC9928Eu, 0x3A0BF8B9u, 0x3F44EE3Cu,
    0x3E86840Bu, 0x3CC03A52u, 0x3D025065u, 0x365E1758u, 0x379C7D6Fu, 0x35DAC336u, 0x3418A901u, 0x3157BF84u, 0x3095D5B3u,
    0x32D36BEAu, 0x331101DDu, 0x246BE590u, 0x25A98FA7u, 0x27EF31FEu, 0x262D5BC9u, 0x23624D4Cu, 0x22A0277Bu, 0x20E69922u,
    0x2124F315u, 0x2A78B428u, 0x2BBADE1Fu, 0x29FC6046u, 0x283E0A71u, 0x2D711CF4u, 0x2CB376C3u, 0x2EF5C89Au, 0x2F37A2ADu,
    0x709A8DC0u, 0x7158E7F7u, 0x731E59AEu, 0x72DC3399u, 0x7793251Cu, 0x76514F2Bu, 0x7417F172u, 0x75D59B45u, 0x7E89DC78u,
    0x7F4BB64Fu, 0x7D0D0816u, 0x7CCF6221u, 0x798074A4u, 0x78421E93u, 0x7A04A0CAu, 0x7BC6CAFDu, 0x6CBC2EB0u, 0x6D7E4487u,
    0x6F38FADEu, 0x6EFA90E9u, 0x6BB5866Cu, 0x6A77EC5Bu, 0x68315202u, 0x69F33835u, 0x62AF7F08u, 0x636D153Fu, 0x612BAB66u,
    0x60E9C151u, 0x65A6D7D4u, 0x6464BDE3u, 0x662203BAu, 0x67E0698Du, 0x48D7CB20u, 0x4915A117u, 0x4B531F4Eu, 0x4A917579u,
    0x4FDE63FCu, 0x4E1C09CBu, 0x4C5AB792u, 0x4D98DDA5u, 0x46C49A98u, 0x4706F0AFu, 0x45404EF6u, 0x448224C1u, 0x41CD3244u,
    0x400F5873u, 0x4249E62Au, 0x438B8C1Du, 0x54F16850u, 0x55330267u, 0x5775BC3Eu, 0x56B7D609u, 0x53F8C08Cu, 0x523AAABBu,
    0x507C14E2u, 0x51BE7ED5u, 0x5AE239E8u, 0x5B2053DFu, 0x5966ED86u, 0x58A487B1u, 0x5DEB9134u, 0x5C29FB03u, 0x5E6F455Au,
    0x5FAD2F6Du, 0xE1351B80u, 0xE0F771B7u, 0xE2B1CFEEu, 0xE373A5D9u, 0xE63CB35Cu, 0xE7FED96Bu, 0xE5B86732u, 0xE47A0D05u,
    0xEF264A38u, 0xEEE4200Fu, 0xECA29E56u, 0xED60F461u, 0xE82FE2E4u, 0xE9ED88D3u, 0xEBAB368Au, 0xEA695CBDu, 0xFD13B8F0u,
    0xFCD1D2C7u, 0xFE976C9Eu, 0xFF5506A9u, 0xFA1A102Cu, 0xFBD87A1Bu, 0xF99EC442u, 0xF85CAE75u, 0xF300E948u, 0xF2C2837Fu,
    0xF0843D26u, 0xF1465711u, 0xF4094194u, 0xF5CB2BA3u, 0xF78D95FAu, 0xF64FFFCDu, 0xD9785D60u, 0xD8BA3757u, 0xDAFC890Eu,
    0xDB3EE339u, 0xDE71F5BCu, 0xDFB39F8Bu, 0xDDF521D2u, 0xDC374BE5u, 0xD76B0CD8u, 0xD6A966EFu, 0xD4EFD8B6u, 0xD52DB281u,
    0xD062A404u, 0xD1A0CE33u, 0xD3E6706Au, 0xD2241A5Du, 0xC55EFE10u, 0xC49C9427u, 0xC6DA2A7Eu, 0xC7184049u, 0xC25756CCu,
    0xC3953CFBu, 0xC1D382A2u, 0xC011E895u, 0xCB4DAFA8u, 0xCA8FC59Fu, 0xC8C97BC6u, 0xC90B11F1u, 0xCC440774u, 0xCD866D43u,
    0xCFC0D31Au, 0xCE02B92Du, 0x91AF9640u, 0x906DFC77u, 0x922B422Eu, 0x93E92819u, 0x96A63E9Cu, 0x976454ABu, 0x9522EAF2u,
    0x94E080C5u, 0x9FBCC7F8u, 0x9E7EADCFu, 0x9C381396u, 0x9DFA79A1u, 0x98B56F24u, 0x99770513u, 0x9B31BB4Au, 0x9AF3D17Du,
    0x8D893530u, 0x8C4B5F07u, 0x8E0DE15Eu, 0x8FCF8B69u, 0x8A809DECu, 0x8B42F7DBu, 0x89044982u, 0x88C623B5u, 0x839A6488u,
    0x82580EBFu, 0x801EB0E6u, 0x81DCDAD1u, 0x8493CC54u, 0x8551A663u, 0x8717183Au, 0x86D5720Du, 0xA9E2D0A0u, 0xA820BA97u,
    0xAA6604CEu, 0xABA46EF9u, 0xAEEB787Cu, 0xAF29124Bu, 0xAD6FAC12u, 0xACADC625u, 0xA7F18118u, 0xA633EB2Fu, 0xA4755576u,
    0xA5B73F41u, 0xA0F829C4u, 0xA13A43F3u, 0xA37CFDAAu, 0xA2BE979Du, 0xB5C473D0u, 0xB40619E7u, 0xB640A7BEu, 0xB782CD89u,
    0xB2CDDB0Cu, 0xB30FB13Bu, 0xB1490F62u, 0xB08B6555u, 0xBBD72268u, 0xBA15485Fu, 0xB853F606u, 0xB9919C31u, 0xBCDE8AB4u,
    0xBD1CE083u, 0xBF5A5EDAu, 0xBE9834EDu, 0x00000000u, 0xB8BC6765u, 0xAA09C88Bu, 0x12B5AFEEu, 0x8F629757u, 0x37DEF032u,
    0x256B5FDCu, 0x9DD738B9u, 0xC5B428EFu, 0x7D084F8Au, 0x6FBDE064u, 0xD7018701u, 0x4AD6BFB8u, 0xF26AD8DDu, 0xE0DF7733u,
    0x58631056u, 0x5019579Fu, 0xE8A530FAu, 0xFA109F14u, 0x42ACF871u, 0xDF7BC0C8u, 0x67C7A7ADu, 0x75720843u, 0xCDCE6F26u,
    0x95AD7F70u, 0x2D111815u, 0x3FA4B7FBu, 0x8718D09Eu, 0x1ACFE827u, 0xA2738F42u, 0xB0C620ACu, 0x087A47C9u, 0xA032AF3Eu,
    0x188EC85Bu, 0x0A3B67B5u, 0xB28700D0u, 0x2F503869u, 0x97EC5F0Cu, 0x8559F0E2u, 0x3DE59787u, 0x658687D1u, 0xDD3AE0B4u,
    0xCF8F4F5Au, 0x7733283Fu, 0xEAE41086u, 0x525877E3u, 0x40EDD80Du, 0xF851BF68u, 0xF02BF8A1u, 0x48979FC4u, 0x5A22302Au,
    0xE29E574Fu, 0x7F496FF6u, 0xC7F50893u, 0xD540A77Du, 0x6DFCC018u, 0x359FD04Eu, 0x8D23B72Bu, 0x9F9618C5u, 0x272A7FA0u,
    0xBAFD4719u, 0x0241207Cu, 0x10F48F92u, 0xA848E8F7u, 0x9B14583Du, 0x23A83F58u, 0x311D90B6u, 0x89A1F7D3u, 0x1476CF6Au,
    0xACCAA80Fu, 0xBE7F07E1u, 0x06C36084u, 0x5EA070D2u, 0xE61C17B7u, 0xF4A9B859u, 0x4C15DF3Cu, 0xD1C2E785u, 0x697E80E0u,
    0x7BCB2F0Eu, 0xC377486Bu, 0xCB0D0FA2u, 0x73B168C7u, 0x6104C729u, 0xD9B8A04Cu, 0x446F98F5u, 0xFCD3FF90u, 0xEE66507Eu,
    0x56DA371Bu, 0x0EB9274Du, 0xB6054028u, 0xA4B0EFC6u, 0x1C0C88A3u, 0x81DBB01Au, 0x3967D77Fu, 0x2BD27891u, 0x936E1FF4u,
    0x3B26F703u, 0x839A9066u, 0x912F3F88u, 0x299358EDu, 0xB4446054u, 0x0CF80731u, 0x1E4DA8DFu, 0xA6F1CFBAu, 0xFE92DFECu,
    0x462EB889u, 0x549B1767u, 0xEC277002u, 0x71F048BBu, 0xC94C2FDEu, 0xDBF98030u, 0x6345E755u, 0x6B3FA09Cu, 0xD383C7F9u,
    0xC1366817u, 0x798A0F72u, 0xE45D37CBu, 0x5CE150AEu, 0x4E54FF40u, 0xF6E89825u, 0xAE8B8873u, 0x1637EF16u, 0x048240F8u,
    0xBC3E279Du, 0x21E91F24u, 0x99557841u, 0x8BE0D7AFu, 0x335CB0CAu, 0xED59B63Bu, 0x55E5D15Eu, 0x47507EB0u, 0xFFEC19D5u,
    0x623B216Cu, 0xDA874609u, 0xC832E9E7u, 0x708E8E82u, 0x28ED9ED4u, 0x9051F9B1u, 0x82E4565Fu, 0x3A58313Au, 0xA78F0983u,
    0x1F336EE6u, 0x0D86C108u, 0xB53AA66Du, 0xBD40E1A4u, 0x05FC86C1u, 0x1749292Fu, 0xAFF54E4Au, 0x322276F3u, 0x8A9E1196u,
    0x982BBE78u, 0x2097D91Du, 0x78F4C94Bu, 0xC048AE2Eu, 0xD2FD01C0u, 0x6A4166A5u, 0xF7965E1Cu, 0x4F2A3979u, 0x5D9F9697u,
    0xE523F1F2u, 0x4D6B1905u, 0xF5D77E60u, 0xE762D18Eu, 0x5FDEB6EBu, 0xC2098E52u, 0x7AB5E937u, 0x680046D9u, 0xD0BC21BCu,
    0x88DF31EAu, 0x3063568Fu, 0x22D6F961u, 0x9A6A9E04u, 0x07BDA6BDu, 0xBF01C1D8u, 0xADB46E36u, 0x15080953u, 0x1D724E9Au,
    0xA5CE29FFu, 0xB77B8611u, 0x0FC7E174u, 0x9210D9CDu, 0x2AACBEA8u, 0x38191146u, 0x80A57623u, 0xD8C66675u, 0x607A0110u,
    0x72CFAEFEu, 0xCA73C99Bu, 0x57A4F122u, 0xEF189647u, 0xFDAD39A9u, 0x45115ECCu, 0x764DEE06u, 0xCEF18963u, 0xDC44268Du,
    0x64F841E8u, 0xF92F7951u, 0x41931E34u, 0x5326B1DAu, 0xEB9AD6BFu, 0xB3F9C6E9u, 0x0B45A18Cu, 0x19F00E62u, 0xA14C6907u,
    0x3C9B51BEu, 0x842736DBu, 0x96929935u, 0x2E2EFE50u, 0x2654B999u, 0x9EE8DEFCu, 0x8C5D7112u, 0x34E11677u, 0xA9362ECEu,
    0x118A49ABu, 0x033FE645u, 0xBB838120u, 0xE3E09176u, 0x5B5CF613u, 0x49E959FDu, 0xF1553E98u, 0x6C820621u, 0xD43E6144u,
    0xC68BCEAAu, 0x7E37A9CFu, 0xD67F4138u, 0x6EC3265Du, 0x7C7689B3u, 0xC4CAEED6u, 0x591DD66Fu, 0xE1A1B10Au, 0xF3141EE4u,
    0x4BA87981u, 0x13CB69D7u, 0xAB770EB2u, 0xB9C2A15Cu, 0x017EC639u, 0x9CA9FE80u, 0x241599E5u, 0x36A0360Bu, 0x8E1C516Eu,
    0x866616A7u, 0x3EDA71C2u, 0x2C6FDE2Cu, 0x94D3B949u, 0x090481F0u, 0xB1B8E695u, 0xA30D497Bu, 0x1BB12E1Eu, 0x43D23E48u,
    0xFB6E592Du, 0xE9DBF6C3u, 0x516791A6u, 0xCCB0A91Fu, 0x740CCE7Au, 0x66B96194u, 0xDE0506F1u, 0x00000000u, 0x3D6029B0u,
    0x7AC05360u, 0x47A07AD0u, 0xF580A6C0u, 0xC8E08F70u, 0x8F40F5A0u, 0xB220DC10u, 0x30704BC1u, 0x0D106271u, 0x4AB018A1u,
    0x77D03111u, 0xC5F0ED01u, 0xF890C4B1u, 0xBF30BE61u, 0x825097D1u, 0x60E09782u, 0x5D80BE32u, 0x1A20C4E2u, 0x2740ED52u,
    0x95603142u, 0xA80018F2u, 0xEFA06222u, 0xD2C04B92u, 0x5090DC43u, 0x6DF0F5F3u, 0x2A508F23u, 0x1730A693u, 0xA5107A83u,
    0x98705333u, 0xDFD029E3u, 0xE2B00053u, 0xC1C12F04u, 0xFCA106B4u, 0xBB017C64u, 0x866155D4u, 0x344189C4u, 0x0921A074u,
    0x4E81DAA4u, 0x73E1F314u, 0xF1B164C5u, 0xCCD14D75u, 0x8B7137A5u, 0xB6111E15u, 0x0431C205u, 0x3951EBB5u, 0x7EF19165u,
    0x4391B8D5u, 0xA121B886u, 0x9C419136u, 0xDBE1EBE6u, 0xE681C256u, 0x54A11E46u, 0x69C137F6u, 0x2E614D26u, 0x13016496u,
    0x9151F347u, 0xAC31DAF7u, 0xEB91A027u, 0xD6F18997u, 0x64D15587u, 0x59B17C37u, 0x1E1106E7u, 0x23712F57u, 0x58F35849u,
    0x659371F9u, 0x22330B29u, 0x1F532299u, 0xAD73FE89u, 0x9013D739u, 0xD7B3ADE9u, 0xEAD38459u, 0x68831388u, 0x55E33A38u,
    0x124340E8u, 0x2F236958u, 0x9D03B548u, 0xA0639CF8u, 0xE7C3E628u, 0xDAA3CF98u, 0x3813CFCBu, 0x0573E67Bu, 0x42D39CABu,
    0x7FB3B51Bu, 0xCD93690Bu, 0xF0F340BBu, 0xB7533A6Bu, 0x8A3313DBu, 0x0863840Au, 0x3503ADBAu, 0x72A3D76Au, 0x4FC3FEDAu,
    0xFDE322CAu, 0xC0830B7Au, 0x872371AAu, 0xBA43581Au, 0x9932774Du, 0xA4525EFDu, 0xE3F2242Du, 0xDE920D9Du, 0x6CB2D18Du,
    0x51D2F83Du, 0x167282EDu, 0x2B12AB5Du, 0xA9423C8Cu, 0x9422153Cu, 0xD3826FECu, 0xEEE2465Cu, 0x5CC29A4Cu, 0x61A2B3FCu,
    0x2602C92Cu, 0x1B62E09Cu, 0xF9D2E0CFu, 0xC4B2C97Fu, 0x8312B3AFu, 0xBE729A1Fu, 0x0C52460Fu, 0x31326FBFu, 0x7692156Fu,
    0x4BF23CDFu, 0xC9A2AB0Eu, 0xF4C282BEu, 0xB362F86Eu, 0x8E02D1DEu, 0x3C220DCEu, 0x0142247Eu, 0x46E25EAEu, 0x7B82771Eu,
    0xB1E6B092u, 0x8C869922u, 0xCB26E3F2u, 0xF646CA42u, 0x44661652u, 0x79063FE2u, 0x3EA64532u, 0x03C66C82u, 0x8196FB53u,
    0xBCF6D2E3u, 0xFB56A833u, 0xC6368183u, 0x74165D93u, 0x49767423u, 0x0ED60EF3u, 0x33B62743u, 0xD1062710u, 0xEC660EA0u,
    0xABC67470u, 0x96A65DC0u, 0x248681D0u, 0x19E6A860u, 0x5E46D2B0u, 0x6326FB00u, 0xE1766CD1u, 0xDC164561u, 0x9BB63FB1u,
    0xA6D61601u, 0x14F6CA11u, 0x2996E3A1u, 0x6E369971u, 0x5356B0C1u, 0x70279F96u, 0x4D47B626u, 0x0AE7CCF6u, 0x3787E546u,
    0x85A73956u, 0xB8C710E6u, 0xFF676A36u, 0xC2074386u, 0x4057D457u, 0x7D37FDE7u, 0x3A978737u, 0x07F7AE87u, 0xB5D77297u,
    0x88B75B27u, 0xCF1721F7u, 0xF2770847u, 0x10C70814u, 0x2DA721A4u, 0x6A075B74u, 0x576772C4u, 0xE547AED4u, 0xD8278764u,
    0x9F87FDB4u, 0xA2E7D404u, 0x20B743D5u, 0x1DD76A65u, 0x5A7710B5u, 0x67173905u, 0xD537E515u, 0xE857CCA5u, 0xAFF7B675u,
    0x92979FC5u, 0xE915E8DBu, 0xD475C16Bu, 0x93D5BBBBu, 0xAEB5920Bu, 0x1C954E1Bu, 0x21F567ABu, 0x66551D7Bu, 0x5B3534CBu,
    0xD965A31Au, 0xE4058AAAu, 0xA3A5F07Au, 0x9EC5D9CAu, 0x2CE505DAu, 0x11852C6Au, 0x562556BAu, 0x6B457F0Au, 0x89F57F59u,
    0xB49556E9u, 0xF3352C39u, 0xCE550589u, 0x7C75D999u, 0x4115F029u, 0x06B58AF9u, 0x3BD5A349u, 0xB9853498u, 0x84E51D28u,
    0xC34567F8u, 0xFE254E48u, 0x4C059258u, 0x7165BBE8u, 0x36C5C138u, 0x0BA5E888u, 0x28D4C7DFu, 0x15B4EE6Fu, 0x521494BFu,
    0x6F74BD0Fu, 0xDD54611Fu, 0xE03448AFu, 0xA794327Fu, 0x9AF41BCFu, 0x18A48C1Eu, 0x25C4A5AEu, 0x6264DF7Eu, 0x5F04F6CEu,
    0xED242ADEu, 0xD044036Eu, 0x97E479BEu, 0xAA84500Eu, 0x4834505Du, 0x755479EDu, 0x32F4033Du, 0x0F942A8Du, 0xBDB4F69Du,
    0x80D4DF2Du, 0xC774A5FDu, 0xFA148C4Du, 0x78441B9Cu, 0x4524322Cu, 0x028448FCu, 0x3FE4614Cu, 0x8DC4BD5Cu, 0xB0A494ECu,
    0xF704EE3Cu, 0xCA64C78Cu, 0x00000000u, 0xCB5CD3A5u, 0x4DC8A10Bu, 0x869472AEu, 0x9B914216u, 0x50CD91B3u, 0xD659E31Du,
    0x1D0530B8u, 0xEC53826Du, 0x270F51C8u, 0xA19B2366u, 0x6AC7F0C3u, 0x77C2C07Bu, 0xBC9E13DEu, 0x3A0A6170u, 0xF156B2D5u,
    0x03D6029Bu, 0xC88AD13Eu, 0x4E1EA390u, 0x85427035u, 0x9847408Du, 0x531B9328u, 0xD58FE186u, 0x1ED33223u, 0xEF8580F6u,
    0x24D95353u, 0xA24D21FDu, 0x6911F258u, 0x7414C2E0u, 0xBF481145u, 0x39DC63EBu, 0xF280B04Eu, 0x07AC0536u, 0xCCF0D693u,
    0x4A64A43Du, 0x81387798u, 0x9C3D4720u, 0x57619485u, 0xD1F5E62Bu, 0x1AA9358Eu, 0xEBFF875Bu, 0x20A354FEu, 0xA6372650u,
    0x6D6BF5F5u, 0x706EC54Du, 0xBB3216E8u, 0x3DA66446u, 0xF6FAB7E3u, 0x047A07ADu, 0xCF26D408u, 0x49B2A6A6u, 0x82EE7503u,
    0x9FEB45BBu, 0x54B7961Eu, 0xD223E4B0u, 0x197F3715u, 0xE82985C0u, 0x23755665u, 0xA5E124CBu, 0x6EBDF76Eu, 0x73B8C7D6u,
    0xB8E41473u, 0x3E7066DDu, 0xF52CB578u, 0x0F580A6Cu, 0xC404D9C9u, 0x4290AB67u, 0x89CC78C2u, 0x94C9487Au, 0x5F959BDFu,
    0xD901E971u, 0x125D3AD4u, 0xE30B8801u, 0x28575BA4u, 0xAEC3290Au, 0x659FFAAFu, 0x789ACA17u, 0xB3C619B2u, 0x35526B1Cu,
    0xFE0EB8B9u, 0x0C8E08F7u, 0xC7D2DB52u, 0x4146A9FCu, 0x8A1A7A59u, 0x971F4AE1u, 0x5C439944u, 0xDAD7EBEAu, 0x118B384Fu,
    0xE0DD8A9Au, 0x2B81593Fu, 0xAD152B91u, 0x6649F834u, 0x7B4CC88Cu, 0xB0101B29u, 0x36846987u, 0xFDD8BA22u, 0x08F40F5Au,
    0xC3A8DCFFu, 0x453CAE51u, 0x8E607DF4u, 0x93654D4Cu, 0x58399EE9u, 0xDEADEC47u, 0x15F13FE2u, 0xE4A78D37u, 0x2FFB5E92u,
    0xA96F2C3Cu, 0x6233FF99u, 0x7F36CF21u, 0xB46A1C84u, 0x32FE6E2Au, 0xF9A2BD8Fu, 0x0B220DC1u, 0xC07EDE64u, 0x46EAACCAu,
    0x8DB67F6Fu, 0x90B34FD7u, 0x5BEF9C72u, 0xDD7BEEDCu, 0x16273D79u, 0xE7718FACu, 0x2C2D5C09u, 0xAAB92EA7u, 0x61E5FD02u,
    0x7CE0CDBAu, 0xB7BC1E1Fu, 0x31286CB1u, 0xFA74BF14u, 0x1EB014D8u, 0xD5ECC77Du, 0x5378B5D3u, 0x98246676u, 0x852156CEu,
    0x4E7D856Bu, 0xC8E9F7C5u, 0x03B52460u, 0xF2E396B5u, 0x39BF4510u, 0xBF2B37BEu, 0x7477E41Bu, 0x6972D4A3u, 0xA22E0706u,
    0x24BA75A8u, 0xEFE6A60Du, 0x1D661643u, 0xD63AC5E6u, 0x50AEB748u, 0x9BF264EDu, 0x86F75455u, 0x4DAB87F0u, 0xCB3FF55Eu,
    0x006326FBu, 0xF135942Eu, 0x3A69478Bu, 0xBCFD3525u, 0x77A1E680u, 0x6AA4D638u, 0xA1F8059Du, 0x276C7733u, 0xEC30A496u,
    0x191C11EEu, 0xD240C24Bu, 0x54D4B0E5u, 0x9F886340u, 0x828D53F8u, 0x49D1805Du, 0xCF45F2F3u, 0x04192156u, 0xF54F9383u,
    0x3E134026u, 0xB8873288u, 0x73DBE12Du, 0x6EDED195u, 0xA5820230u, 0x2316709Eu, 0xE84AA33Bu, 0x1ACA1375u, 0xD196C0D0u,
    0x5702B27Eu, 0x9C5E61DBu, 0x815B5163u, 0x4A0782C6u, 0xCC93F068u, 0x07CF23CDu, 0xF6999118u, 0x3DC542BDu, 0xBB513013u,
    0x700DE3B6u, 0x6D08D30Eu, 0xA65400ABu, 0x20C07205u, 0xEB9CA1A0u, 0x11E81EB4u, 0xDAB4CD11u, 0x5C20BFBFu, 0x977C6C1Au,
    0x8A795CA2u, 0x41258F07u, 0xC7B1FDA9u, 0x0CED2E0Cu, 0xFDBB9CD9u, 0x36E74F7Cu, 0xB0733DD2u, 0x7B2FEE77u, 0x662ADECFu,
    0xAD760D6Au, 0x2BE27FC4u, 0xE0BEAC61u, 0x123E1C2Fu, 0xD962CF8Au, 0x5FF6BD24u, 0x94AA6E81u, 0x89AF5E39u, 0x42F38D9Cu,
    0xC467FF32u, 0x0F3B2C97u, 0xFE6D9E42u, 0x35314DE7u, 0xB3A53F49u, 0x78F9ECECu, 0x65FCDC54u, 0xAEA00FF1u, 0x28347D5Fu,
    0xE368AEFAu, 0x16441B82u, 0xDD18C827u, 0x5B8CBA89u, 0x90D0692Cu, 0x8DD55994u, 0x46898A31u, 0xC01DF89Fu, 0x0B412B3Au,
    0xFA1799EFu, 0x314B4A4Au, 0xB7DF38E4u, 0x7C83EB41u, 0x6186DBF9u, 0xAADA085Cu, 0x2C4E7AF2u, 0xE712A957u, 0x15921919u,
    0xDECECABCu, 0x585AB812u, 0x93066BB7u, 0x8E035B0Fu, 0x455F88AAu, 0xC3CBFA04u, 0x089729A1u, 0xF9C19B74u, 0x329D48D1u,
    0xB4093A7Fu, 0x7F55E9DAu, 0x6250D962u, 0xA90C0AC7u, 0x2F987869u, 0xE4C4ABCCu, 0x00000000u, 0xA6770BB4u, 0x979F1129u,
    0x31E81A9Du, 0xF44F2413u, 0x52382FA7u, 0x63D0353Au, 0xC5A73E8Eu, 0x33EF4E67u, 0x959845D3u, 0xA4705F4Eu, 0x020754FAu,
    0xC7A06A74u, 0x61D761C0u, 0x503F7B5Du, 0xF64870E9u, 0x67DE9CCEu, 0xC1A9977Au, 0xF0418DE7u, 0x56368653u, 0x9391B8DDu,
    0x35E6B369u, 0x040EA9F4u, 0xA279A240u, 0x5431D2A9u, 0xF246D91Du, 0xC3AEC380u, 0x65D9C834u, 0xA07EF6BAu, 0x0609FD0Eu,
    0x37E1E793u, 0x9196EC27u, 0xCFBD399Cu, 0x69CA3228u, 0x582228B5u, 0xFE552301u, 0x3BF21D8Fu, 0x9D85163Bu, 0xAC6D0CA6u,
    0x0A1A0712u, 0xFC5277FBu, 0x5A257C4Fu, 0x6BCD66D2u, 0xCDBA6D66u, 0x081D53E8u, 0xAE6A585Cu, 0x9F8242C1u, 0x39F54975u,
    0xA863A552u, 0x0E14AEE6u, 0x3FFCB47Bu, 0x998BBFCFu, 0x5C2C8141u, 0xFA5B8AF5u, 0xCBB39068u, 0x6DC49BDCu, 0x9B8CEB35u,
    0x3DFBE081u, 0x0C13FA1Cu, 0xAA64F1A8u, 0x6FC3CF26u, 0xC9B4C492u, 0xF85CDE0Fu, 0x5E2BD5BBu, 0x440B7579u, 0xE27C7ECDu,
    0xD3946450u, 0x75E36FE4u, 0xB044516Au, 0x16335ADEu, 0x27DB4043u, 0x81AC4BF7u, 0x77E43B1Eu, 0xD19330AAu, 0xE07B2A37u,
    0x460C2183u, 0x83AB1F0Du, 0x25DC14B9u, 0x14340E24u, 0xB2430590u, 0x23D5E9B7u, 0x85A2E203u, 0xB44AF89Eu, 0x123DF32Au,
    0xD79ACDA4u, 0x71EDC610u, 0x4005DC8Du, 0xE672D739u, 0x103AA7D0u, 0xB64DAC64u, 0x87A5B6F9u, 0x21D2BD4Du, 0xE47583C3u,
    0x42028877u, 0x73EA92EAu, 0xD59D995Eu, 0x8BB64CE5u, 0x2DC14751u, 0x1C295DCCu, 0xBA5E5678u, 0x7FF968F6u, 0xD98E6342u,
    0xE86679DFu, 0x4E11726Bu, 0xB8590282u, 0x1E2E0936u, 0x2FC613ABu, 0x89B1181Fu, 0x4C162691u, 0xEA612D25u, 0xDB8937B8u,
    0x7DFE3C0Cu, 0xEC68D02Bu, 0x4A1FDB9Fu, 0x7BF7C102u, 0xDD80CAB6u, 0x1827F438u, 0xBE50FF8Cu, 0x8FB8E511u, 0x29CFEEA5u,
    0xDF879E4Cu, 0x79F095F8u, 0x48188F65u, 0xEE6F84D1u, 0x2BC8BA5Fu, 0x8DBFB1EBu, 0xBC57AB76u, 0x1A20A0C2u, 0x8816EAF2u,
    0x2E61E146u, 0x1F89FBDBu, 0xB9FEF06Fu, 0x7C59CEE1u, 0xDA2EC555u, 0xEBC6DFC8u, 0x4DB1D47Cu, 0xBBF9A495u, 0x1D8EAF21u,
    0x2C66B5BCu, 0x8A11BE08u, 0x4FB68086u, 0xE9C18B32u, 0xD82991AFu, 0x7E5E9A1Bu, 0xEFC8763Cu, 0x49BF7D88u, 0x78576715u,
    0xDE206CA1u, 0x1B87522Fu, 0xBDF0599Bu, 0x8C184306u, 0x2A6F48B2u, 0xDC27385Bu, 0x7A5033EFu, 0x4BB82972u, 0xEDCF22C6u,
    0x28681C48u, 0x8E1F17FCu, 0xBFF70D61u, 0x198006D5u, 0x47ABD36Eu, 0xE1DCD8DAu, 0xD034C247u, 0x7643C9F3u, 0xB3E4F77Du,
    0x1593FCC9u, 0x247BE654u, 0x820CEDE0u, 0x74449D09u, 0xD23396BDu, 0xE3DB8C20u, 0x45AC8794u, 0x800BB91Au, 0x267CB2AEu,
    0x1794A833u, 0xB1E3A387u, 0x20754FA0u, 0x86024414u, 0xB7EA5E89u, 0x119D553Du, 0xD43A6BB3u, 0x724D6007u, 0x43A57A9Au,
    0xE5D2712Eu, 0x139A01C7u, 0xB5ED0A73u, 0x840510EEu, 0x22721B5Au, 0xE7D525D4u, 0x41A22E60u, 0x704A34FDu, 0xD63D3F49u,
    0xCC1D9F8Bu, 0x6A6A943Fu, 0x5B828EA2u, 0xFDF58516u, 0x3852BB98u, 0x9E25B02Cu, 0xAFCDAAB1u, 0x09BAA105u, 0xFFF2D1ECu,
    0x5985DA58u, 0x686DC0C5u, 0xCE1ACB71u, 0x0BBDF5FFu, 0xADCAFE4Bu, 0x9C22E4D6u, 0x3A55EF62u, 0xABC30345u, 0x0DB408F1u,
    0x3C5C126Cu, 0x9A2B19D8u, 0x5F8C2756u, 0xF9FB2CE2u, 0xC813367Fu, 0x6E643DCBu, 0x982C4D22u, 0x3E5B4696u, 0x0FB35C0Bu,
    0xA9C457BFu, 0x6C636931u, 0xCA146285u, 0xFBFC7818u, 0x5D8B73ACu, 0x03A0A617u, 0xA5D7ADA3u, 0x943FB73Eu, 0x3248BC8Au,
    0xF7EF8204u, 0x519889B0u, 0x6070932Du, 0xC6079899u, 0x304FE870u, 0x9638E3C4u, 0xA7D0F959u, 0x01A7F2EDu, 0xC400CC63u,
    0x6277C7D7u, 0x539FDD4Au, 0xF5E8D6FEu, 0x647E3AD9u, 0xC209316Du, 0xF3E12BF0u, 0x55962044u, 0x90311ECAu, 0x3646157Eu,
    0x07AE0FE3u, 0xA1D90457u, 0x579174BEu, 0xF1E67F0Au, 0xC00E6597u, 0x66796E23u, 0xA3DE50ADu, 0x05A95B19u, 0x34414184u,
    0x92364A30u, 0x00000000u, 0xCCAA009Eu, 0x4225077Du, 0x8E8F07E3u, 0x844A0EFAu, 0x48E00E64u, 0xC66F0987u, 0x0AC50919u,
    0xD3E51BB5u, 0x1F4F1B2Bu, 0x91C01CC8u, 0x5D6A1C56u, 0x57AF154Fu, 0x9B0515D1u, 0x158A1232u, 0xD92012ACu, 0x7CBB312Bu,
    0xB01131B5u, 0x3E9E3656u, 0xF23436C8u, 0xF8F13FD1u, 0x345B3F4Fu, 0xBAD438ACu, 0x767E3832u, 0xAF5E2A9Eu, 0x63F42A00u,
    0xED7B2DE3u, 0x21D12D7Du, 0x2B142464u, 0xE7BE24FAu, 0x69312319u, 0xA59B2387u, 0xF9766256u, 0x35DC62C8u, 0xBB53652Bu,
    0x77F965B5u, 0x7D3C6CACu, 0xB1966C32u, 0x3F196BD1u, 0xF3B36B4Fu, 0x2A9379E3u, 0xE639797Du, 0x68B67E9Eu, 0xA41C7E00u,
    0xAED97719u, 0x62737787u, 0xECFC7064u, 0x205670FAu, 0x85CD537Du, 0x496753E3u, 0xC7E85400u, 0x0B42549Eu, 0x01875D87u,
    0xCD2D5D19u, 0x43A25AFAu, 0x8F085A64u, 0x562848C8u, 0x9A824856u, 0x140D4FB5u, 0xD8A74F2Bu, 0xD2624632u, 0x1EC846ACu,
    0x9047414Fu, 0x5CED41D1u, 0x299DC2EDu, 0xE537C273u, 0x6BB8C590u, 0xA712C50Eu, 0xADD7CC17u, 0x617DCC89u, 0xEFF2CB6Au,
    0x2358CBF4u, 0xFA78D958u, 0x36D2D9C6u, 0xB85DDE25u, 0x74F7DEBBu, 0x7E32D7A2u, 0xB298D73Cu, 0x3C17D0DFu, 0xF0BDD041u,
    0x5526F3C6u, 0x998CF358u, 0x1703F4BBu, 0xDBA9F425u, 0xD16CFD3Cu, 0x1DC6FDA2u, 0x9349FA41u, 0x5FE3FADFu, 0x86C3E873u,
    0x4A69E8EDu, 0xC4E6EF0Eu, 0x084CEF90u, 0x0289E689u, 0xCE23E617u, 0x40ACE1F4u, 0x8C06E16Au, 0xD0EBA0BBu, 0x1C41A025u,
    0x92CEA7C6u, 0x5E64A758u, 0x54A1AE41u, 0x980BAEDFu, 0x1684A93Cu, 0xDA2EA9A2u, 0x030EBB0Eu, 0xCFA4BB90u, 0x412BBC73u,
    0x8D81BCEDu, 0x8744B5F4u, 0x4BEEB56Au, 0xC561B289u, 0x09CBB217u, 0xAC509190u, 0x60FA910Eu, 0xEE7596EDu, 0x22DF9673u,
    0x281A9F6Au, 0xE4B09FF4u, 0x6A3F9817u, 0xA6959889u, 0x7FB58A25u, 0xB31F8ABBu, 0x3D908D58u, 0xF13A8DC6u, 0xFBFF84DFu,
    0x37558441u, 0xB9DA83A2u, 0x7570833Cu, 0x533B85DAu, 0x9F918544u, 0x111E82A7u, 0xDDB48239u, 0xD7718B20u, 0x1BDB8BBEu,
    0x95548C5Du, 0x59FE8CC3u, 0x80DE9E6Fu, 0x4C749EF1u, 0xC2FB9912u, 0x0E51998Cu, 0x04949095u, 0xC83E900Bu, 0x46B197E8u,
    0x8A1B9776u, 0x2F80B4F1u, 0xE32AB46Fu, 0x6DA5B38Cu, 0xA10FB312u, 0xABCABA0Bu, 0x6760BA95u, 0xE9EFBD76u, 0x2545BDE8u,
    0xFC65AF44u, 0x30CFAFDAu, 0xBE40A839u, 0x72EAA8A7u, 0x782FA1BEu, 0xB485A120u, 0x3A0AA6C3u, 0xF6A0A65Du, 0xAA4DE78Cu,
    0x66E7E712u, 0xE868E0F1u, 0x24C2E06Fu, 0x2E07E976u, 0xE2ADE9E8u, 0x6C22EE0Bu, 0xA088EE95u, 0x79A8FC39u, 0xB502FCA7u,
    0x3B8DFB44u, 0xF727FBDAu, 0xFDE2F2C3u, 0x3148F25Du, 0xBFC7F5BEu, 0x736DF520u, 0xD6F6D6A7u, 0x1A5CD639u, 0x94D3D1DAu,
    0x5879D144u, 0x52BCD85Du, 0x9E16D8C3u, 0x1099DF20u, 0xDC33DFBEu, 0x0513CD12u, 0xC9B9CD8Cu, 0x4736CA6Fu, 0x8B9CCAF1u,
    0x8159C3E8u, 0x4DF3C376u, 0xC37CC495u, 0x0FD6C40Bu, 0x7AA64737u, 0xB60C47A9u, 0x3883404Au, 0xF42940D4u, 0xFEEC49CDu,
    0x32464953u, 0xBCC94EB0u, 0x70634E2Eu, 0xA9435C82u, 0x65E95C1Cu, 0xEB665BFFu, 0x27CC5B61u, 0x2D095278u, 0xE1A352E6u,
    0x6F2C5505u, 0xA386559Bu, 0x061D761Cu, 0xCAB77682u, 0x44387161u, 0x889271FFu, 0x825778E6u, 0x4EFD7878u, 0xC0727F9Bu,
    0x0CD87F05u, 0xD5F86DA9u, 0x19526D37u, 0x97DD6AD4u, 0x5B776A4Au, 0x51B26353u, 0x9D1863CDu, 0x1397642Eu, 0xDF3D64B0u,
    0x83D02561u, 0x4F7A25FFu, 0xC1F5221Cu, 0x0D5F2282u, 0x079A2B9Bu, 0xCB302B05u, 0x45BF2CE6u, 0x89152C78u, 0x50353ED4u,
    0x9C9F3E4Au, 0x121039A9u, 0xDEBA3937u, 0xD47F302Eu, 0x18D530B0u, 0x965A3753u, 0x5AF037CDu, 0xFF6B144Au, 0x33C114D4u,
    0xBD4E1337u, 0x71E413A9u, 0x7B211AB0u, 0xB78B1A2Eu, 0x39041DCDu, 0xF5AE1D53u, 0x2C8E0FFFu, 0xE0240F61u, 0x6EAB0882u,
    0xA201081Cu, 0xA8C40105u, 0x646E019Bu, 0xEAE10678u, 0x264B06E6u,
};
/* CRC32C's (Castagnoli's, 0x82F63B78 reflected): CRC32C a byte at a time where the host has no SSE4.2 */
const u32 snx_x64_crc32c_table[256] = {
    0x00000000u, 0xF26B8303u, 0xE13B70F7u, 0x1350F3F4u, 0xC79A971Fu, 0x35F1141Cu, 0x26A1E7E8u, 0xD4CA64EBu, 0x8AD958CFu,
    0x78B2DBCCu, 0x6BE22838u, 0x9989AB3Bu, 0x4D43CFD0u, 0xBF284CD3u, 0xAC78BF27u, 0x5E133C24u, 0x105EC76Fu, 0xE235446Cu,
    0xF165B798u, 0x030E349Bu, 0xD7C45070u, 0x25AFD373u, 0x36FF2087u, 0xC494A384u, 0x9A879FA0u, 0x68EC1CA3u, 0x7BBCEF57u,
    0x89D76C54u, 0x5D1D08BFu, 0xAF768BBCu, 0xBC267848u, 0x4E4DFB4Bu, 0x20BD8EDEu, 0xD2D60DDDu, 0xC186FE29u, 0x33ED7D2Au,
    0xE72719C1u, 0x154C9AC2u, 0x061C6936u, 0xF477EA35u, 0xAA64D611u, 0x580F5512u, 0x4B5FA6E6u, 0xB93425E5u, 0x6DFE410Eu,
    0x9F95C20Du, 0x8CC531F9u, 0x7EAEB2FAu, 0x30E349B1u, 0xC288CAB2u, 0xD1D83946u, 0x23B3BA45u, 0xF779DEAEu, 0x05125DADu,
    0x1642AE59u, 0xE4292D5Au, 0xBA3A117Eu, 0x4851927Du, 0x5B016189u, 0xA96AE28Au, 0x7DA08661u, 0x8FCB0562u, 0x9C9BF696u,
    0x6EF07595u, 0x417B1DBCu, 0xB3109EBFu, 0xA0406D4Bu, 0x522BEE48u, 0x86E18AA3u, 0x748A09A0u, 0x67DAFA54u, 0x95B17957u,
    0xCBA24573u, 0x39C9C670u, 0x2A993584u, 0xD8F2B687u, 0x0C38D26Cu, 0xFE53516Fu, 0xED03A29Bu, 0x1F682198u, 0x5125DAD3u,
    0xA34E59D0u, 0xB01EAA24u, 0x42752927u, 0x96BF4DCCu, 0x64D4CECFu, 0x77843D3Bu, 0x85EFBE38u, 0xDBFC821Cu, 0x2997011Fu,
    0x3AC7F2EBu, 0xC8AC71E8u, 0x1C661503u, 0xEE0D9600u, 0xFD5D65F4u, 0x0F36E6F7u, 0x61C69362u, 0x93AD1061u, 0x80FDE395u,
    0x72966096u, 0xA65C047Du, 0x5437877Eu, 0x4767748Au, 0xB50CF789u, 0xEB1FCBADu, 0x197448AEu, 0x0A24BB5Au, 0xF84F3859u,
    0x2C855CB2u, 0xDEEEDFB1u, 0xCDBE2C45u, 0x3FD5AF46u, 0x7198540Du, 0x83F3D70Eu, 0x90A324FAu, 0x62C8A7F9u, 0xB602C312u,
    0x44694011u, 0x5739B3E5u, 0xA55230E6u, 0xFB410CC2u, 0x092A8FC1u, 0x1A7A7C35u, 0xE811FF36u, 0x3CDB9BDDu, 0xCEB018DEu,
    0xDDE0EB2Au, 0x2F8B6829u, 0x82F63B78u, 0x709DB87Bu, 0x63CD4B8Fu, 0x91A6C88Cu, 0x456CAC67u, 0xB7072F64u, 0xA457DC90u,
    0x563C5F93u, 0x082F63B7u, 0xFA44E0B4u, 0xE9141340u, 0x1B7F9043u, 0xCFB5F4A8u, 0x3DDE77ABu, 0x2E8E845Fu, 0xDCE5075Cu,
    0x92A8FC17u, 0x60C37F14u, 0x73938CE0u, 0x81F80FE3u, 0x55326B08u, 0xA759E80Bu, 0xB4091BFFu, 0x466298FCu, 0x1871A4D8u,
    0xEA1A27DBu, 0xF94AD42Fu, 0x0B21572Cu, 0xDFEB33C7u, 0x2D80B0C4u, 0x3ED04330u, 0xCCBBC033u, 0xA24BB5A6u, 0x502036A5u,
    0x4370C551u, 0xB11B4652u, 0x65D122B9u, 0x97BAA1BAu, 0x84EA524Eu, 0x7681D14Du, 0x2892ED69u, 0xDAF96E6Au, 0xC9A99D9Eu,
    0x3BC21E9Du, 0xEF087A76u, 0x1D63F975u, 0x0E330A81u, 0xFC588982u, 0xB21572C9u, 0x407EF1CAu, 0x532E023Eu, 0xA145813Du,
    0x758FE5D6u, 0x87E466D5u, 0x94B49521u, 0x66DF1622u, 0x38CC2A06u, 0xCAA7A905u, 0xD9F75AF1u, 0x2B9CD9F2u, 0xFF56BD19u,
    0x0D3D3E1Au, 0x1E6DCDEEu, 0xEC064EEDu, 0xC38D26C4u, 0x31E6A5C7u, 0x22B65633u, 0xD0DDD530u, 0x0417B1DBu, 0xF67C32D8u,
    0xE52CC12Cu, 0x1747422Fu, 0x49547E0Bu, 0xBB3FFD08u, 0xA86F0EFCu, 0x5A048DFFu, 0x8ECEE914u, 0x7CA56A17u, 0x6FF599E3u,
    0x9D9E1AE0u, 0xD3D3E1ABu, 0x21B862A8u, 0x32E8915Cu, 0xC083125Fu, 0x144976B4u, 0xE622F5B7u, 0xF5720643u, 0x07198540u,
    0x590AB964u, 0xAB613A67u, 0xB831C993u, 0x4A5A4A90u, 0x9E902E7Bu, 0x6CFBAD78u, 0x7FAB5E8Cu, 0x8DC0DD8Fu, 0xE330A81Au,
    0x115B2B19u, 0x020BD8EDu, 0xF0605BEEu, 0x24AA3F05u, 0xD6C1BC06u, 0xC5914FF2u, 0x37FACCF1u, 0x69E9F0D5u, 0x9B8273D6u,
    0x88D28022u, 0x7AB90321u, 0xAE7367CAu, 0x5C18E4C9u, 0x4F48173Du, 0xBD23943Eu, 0xF36E6F75u, 0x0105EC76u, 0x12551F82u,
    0xE03E9C81u, 0x34F4F86Au, 0xC69F7B69u, 0xD5CF889Du, 0x27A40B9Eu, 0x79B737BAu, 0x8BDCB4B9u, 0x988C474Du, 0x6AE7C44Eu,
    0xBE2DA0A5u, 0x4C4623A6u, 0x5F16D052u, 0xAD7D5351u,
};

/* ---- for the back ends without inline code of their own for these (risc_a64.h's) ---- */

/* CRC32 or (desc bit 4) CRC32C of the low (desc & 15) bytes of value into acc, a bit at a time (the reflected
   polynomials, no inversion: the instructions' own) */
u64 snx_x64_crc32(u64 acc, u64 value, u32 desc) {
    u32 crc = (u32)acc, poly = desc & 16 ? 0x82F63B78u : 0xEDB88320u;
    unsigned k, bit;
    for (k = 0; k < (desc & 15); ++k) {
        crc ^= (u8)(value >> (8 * k));
        for (bit = 0; bit < 8; ++bit) crc = crc & 1 ? (crc >> 1) ^ poly : crc >> 1;
    }
    return crc;
}
/* STXP of two doublewords at a (cpu.h's stxp: the reservation's pair compared and swapped by snx_cas128) */
u64 snx_x64_stxp(CPU* c, u64 a, u64 v1, u64 v2) {
    u8* M = c->mem;
    return stxp(c, M, a, 3, v1, v2);
}
/* CASP of doublewords at a: the old pair (low, high), the desired stored where it was the expected. The pair is read
   by snx_cas128 (a compare-and-swap of what was read with itself), so the old value is one atomic snapshot. The low one
   is in the first result register on either byte order, where an __int128's is in the second of a big-endian one */
typedef struct {
    u64 lo, hi;
} SnxX64Pair;
SnxX64Pair snx_x64_casp(CPU* c, u64 a, u64 e0, u64 e1, u64 d0, u64 d1) {
    u8* M = c->mem;
    u64* p = (u64*)MP(a);
    for (;;) {
        u64 r0 = atomic_load_explicit((_Atomic u64*)p, memory_order_relaxed);
        u64 r1 = atomic_load_explicit((_Atomic u64*)(p + 1), memory_order_relaxed);
        u64 o0 = SNX_GUEST64(r0), o1 = SNX_GUEST64(r1);
        int match = o0 == e0 && o1 == e1;
        if (snx_cas128(c, p, o0, o1, match ? d0 : o0, match ? d1 : o1)) /* its values the guest's */
            return (SnxX64Pair){o0, o1};
    }
}
