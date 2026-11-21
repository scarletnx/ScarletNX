/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the x86-64 back end's code calls (tools/sailgen/x64_a64.h) rather than writes inline: the exclusive monitor
   (cpu.h's ldx, stx, ldxp, stxp), the LSE atomics, CRC32 and CRC32C, DC ZVA. The LLVM back end builds its own
   helpers into each module as IR (ir_helpers.c); these are C, in the runtime library every module links (rtlib.c
   includes this), and in the x86-64 back end's test directly. Each takes the state first, as its code has it in RBX */
#include "common/cpu/aarch64/cpu.h"
#include <stdatomic.h>

#define SNX_X64_M(c) ((u8*)(c)->mem)

u64 snx_x64_ldx(CPU* c, u64 a, u32 sz);
u32 snx_x64_stx(CPU* c, u64 a, u32 sz, u64 v);
u64 snx_x64_ldxp(CPU* c, u64 a, u32 sz);
u32 snx_x64_stxp(CPU* c, u64 a, u32 sz, u64 v1, u64 v2);
u64 snx_x64_rmw(CPU* c, u64 a, u64 v, u32 op);
u64 snx_x64_cas(CPU* c, u64 a, u64 expected, u64 desired, u32 sz);
u64 snx_x64_cas128(CPU* c, u64 a, u64 e_lo, u64 e_hi, u64 d_lo, u64 d_hi);
u32 snx_x64_crc32(u32 acc, u64 value, u32 bytes, u32 castagnoli);
void snx_x64_dc_zva(CPU* c, u64 a);
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

u64 snx_x64_ldx(CPU* c, u64 a, u32 sz) { return ldx(c, SNX_X64_M(c), a, sz); }
u32 snx_x64_stx(CPU* c, u64 a, u32 sz, u64 v) { return stx(c, SNX_X64_M(c), a, sz, v); }
/* LDXP: the first register's value; the second's is the state's excl_val2 for a pair of doublewords, the high word
   of excl_val for a pair of words (ldxp's), which the code reads after */
u64 snx_x64_ldxp(CPU* c, u64 a, u32 sz) {
    u64 v1, v2;
    ldxp(c, SNX_X64_M(c), a, sz, &v1, &v2);
    return v1;
}
u32 snx_x64_stxp(CPU* c, u64 a, u32 sz, u64 v1, u64 v2) { return stxp(c, SNX_X64_M(c), a, sz, v1, v2); }

/* an atomic read-modify-write, sequentially consistent, of 1 << (op & 3) bytes: op >> 2 is 0 ADD, 1 AND (LDCLR's
   operand already inverted), 2 XOR, 3 OR, 4 SMAX, 5 SMIN, 6 UMAX, 7 UMIN, 8 SWP. The old value */
#define SNX_X64_RMW(T, ST)                                                                                             \
    do {                                                                                                               \
        _Atomic T* p = (_Atomic T*)(SNX_X64_M(c) + a);                                                                 \
        T x = (T)v, old = atomic_load_explicit(p, memory_order_seq_cst), nv;                                           \
        switch (op >> 2) {                                                                                             \
            case 0: return (u64)atomic_fetch_add_explicit(p, x, memory_order_seq_cst);                                 \
            case 1: return (u64)atomic_fetch_and_explicit(p, x, memory_order_seq_cst);                                 \
            case 2: return (u64)atomic_fetch_xor_explicit(p, x, memory_order_seq_cst);                                 \
            case 3: return (u64)atomic_fetch_or_explicit(p, x, memory_order_seq_cst);                                  \
            case 8: return (u64)atomic_exchange_explicit(p, x, memory_order_seq_cst);                                  \
            default: break;                                                                                            \
        }                                                                                                              \
        do {                                                                                                           \
            switch (op >> 2) {                                                                                         \
                case 4: nv = (ST)old > (ST)x ? old : x; break;                                                         \
                case 5: nv = (ST)old < (ST)x ? old : x; break;                                                         \
                case 6: nv = old > x ? old : x; break;                                                                 \
                default: nv = old < x ? old : x; break;                                                                \
            }                                                                                                          \
        } while (!atomic_compare_exchange_weak_explicit(p, &old, nv, memory_order_seq_cst, memory_order_seq_cst));     \
        return (u64)old;                                                                                               \
    } while (0)
u64 snx_x64_rmw(CPU* c, u64 a, u64 v, u32 op) {
    switch (op & 3) {
        case 0: SNX_X64_RMW(u8, int8_t);
        case 1: SNX_X64_RMW(u16, int16_t);
        case 2: SNX_X64_RMW(u32, int32_t);
        default: SNX_X64_RMW(u64, int64_t);
    }
}
/* CAS of 1 << sz bytes: the old value */
u64 snx_x64_cas(CPU* c, u64 a, u64 expected, u64 desired, u32 sz) {
    switch (sz) {
        case 0: {
            u8 e = (u8)expected;
            atomic_compare_exchange_strong_explicit((_Atomic u8*)(SNX_X64_M(c) + a), &e, (u8)desired,
                                                    memory_order_seq_cst, memory_order_seq_cst);
            return e;
        }
        case 1: {
            u16 e = (u16)expected;
            atomic_compare_exchange_strong_explicit((_Atomic u16*)(SNX_X64_M(c) + a), &e, (u16)desired,
                                                    memory_order_seq_cst, memory_order_seq_cst);
            return e;
        }
        case 2: {
            u32 e = (u32)expected;
            atomic_compare_exchange_strong_explicit((_Atomic u32*)(SNX_X64_M(c) + a), &e, (u32)desired,
                                                    memory_order_seq_cst, memory_order_seq_cst);
            return e;
        }
        default: {
            u64 e = expected;
            atomic_compare_exchange_strong_explicit((_Atomic u64*)(SNX_X64_M(c) + a), &e, desired, memory_order_seq_cst,
                                                    memory_order_seq_cst);
            return e;
        }
    }
}
/* CASP of doublewords: the old low half; the old high half is left in the state's excl_val2 (a CASP clears no
   monitor and arms none: the field is the code's to read straight after) */
u64 snx_x64_cas128(CPU* c, u64 a, u64 e_lo, u64 e_hi, u64 d_lo, u64 d_hi) {
    u64* p = (u64*)(SNX_X64_M(c) + a);
    for (;;) {
        u64 lo = atomic_load_explicit((_Atomic u64*)p, memory_order_seq_cst);
        u64 hi = atomic_load_explicit((_Atomic u64*)(p + 1), memory_order_seq_cst);
        int equal = lo == e_lo && hi == e_hi;
        if (snx_cas128(c, p, lo, hi, equal ? d_lo : lo, equal ? d_hi : hi)) {
            c->excl_val2 = hi;
            return lo;
        }
    }
}
/* the reflected CRC update of bytes (1, 2, 4 or 8) of value: CRC32's polynomial, or CRC32C's */
u32 snx_x64_crc32(u32 acc, u64 value, u32 bytes, u32 castagnoli) {
    u32 poly = castagnoli ? 0x82F63B78u : 0xEDB88320u, i, k;
    for (i = 0; i < bytes; ++i) {
        acc ^= (u32)(value >> (8 * i)) & 0xFF;
        for (k = 0; k < 8; ++k) acc = (acc >> 1) ^ (poly & (0u - (acc & 1)));
    }
    return acc;
}
/* DC ZVA: the 64 bytes of the block at a zeroed */
void snx_x64_dc_zva(CPU* c, u64 a) {
    u8* p = SNX_X64_M(c) + (a & ~(u64)63);
    for (unsigned i = 0; i < 64; ++i) p[i] = 0;
}

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
/* the NZCV nibble; desc: width, signal << 8 */
u64 snx_x64_fp_compare(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc) {
    if (fpcr >> 32) return 0;
    return snx_x64_fp_done(c, snx_fp_host_compare(a, b, (int)(desc & 0xFF), (int)(desc >> 8 & 1), (u32)fpcr));
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
