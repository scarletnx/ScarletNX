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
extern const u32 snx_x64_crc32_table[256];
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

/* CRC32's (not CRC32C's) reflected table of the bytes: the x86-64 back end's CRC32 a byte at a time (x64_a64.h's
   SGX_M_CRC), RIP-relative from the code */
const u32 snx_x64_crc32_table[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu,
    0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
    0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
    0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
    0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u, 0x01DB7106u,
    0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du,
    0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u,
    0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu,
    0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u,
    0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
    0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
    0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u,
    0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u,
    0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
    0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u,
    0x616BFFD3u, 0x166CCF45u, 0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
    0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u,
    0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
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
