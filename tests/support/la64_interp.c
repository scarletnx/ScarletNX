/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* LoongArch64's instructions the loongarch64 back end writes, interpreted (la64_interp.h) */
#include "support/la64_interp.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <string.h>

#include "../../tools/sailgen/td/la64_dec.inc"
#include "../../tools/sailgen/td/la64_enc.inc"

static int64_t sext(uint64_t v, unsigned bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }
static uint64_t s32(uint64_t v) { return (uint64_t)(int64_t)(int32_t)(uint32_t)v; }
static uint64_t mulh_u(uint64_t j, uint64_t k) {
    uint64_t hi;
    snx_mul_wide(j, k, &hi);
    return hi;
}
static uint64_t mulh_s(uint64_t j, uint64_t k) { return mulh_u(j, k) - (j >> 63 ? k : 0) - (k >> 63 ? j : 0); }

/* the fields: the manual's names and places */
#define RD(w) ((w) & 31)
#define RJ(w) (((w) >> 5) & 31)
#define RK(w) (((w) >> 10) & 31)
#define SI12(w) sext(((w) >> 10) & 0xfff, 12)
#define UI12(w) (((w) >> 10) & 0xfff)
#define UI5(w) (((w) >> 10) & 31)
#define UI6(w) (((w) >> 10) & 63)
#define SI14(w) sext(((w) >> 10) & 0x3fff, 14)
#define SI20(w) sext(((w) >> 5) & 0xfffff, 20)
#define MSBD(w) (((w) >> 16) & 63)
#define LSBD(w) (((w) >> 10) & 63)
#define OFFS16(w) sext(((w) >> 10) & 0xffff, 16)
#define OFFS21(w) sext((((w) >> 10) & 0xffff) | (uint64_t)((w) & 31) << 16, 21)
#define OFFS26(w) sext((((w) >> 10) & 0xffff) | (uint64_t)((w) & 0x3ff) << 16, 26)

static int decode(uint32_t w) {
    int k;
    for (k = 0; k < SGD_LA64_COUNT; ++k)
        if ((w & sgd_la64[k].mask) == sgd_la64[k].match) return k;
    return -1;
}

int la64_selftest(La64* c) {
    struct {
        uint32_t w;
        int op;
        int64_t got, want;
    } t[] = {
        {sge_la64_add_d(3, 7, 11), SGD_LA64_ADD_D, RD(sge_la64_add_d(3, 7, 11)) | RJ(sge_la64_add_d(3, 7, 11)) << 5 |
                                                       RK(sge_la64_add_d(3, 7, 11)) << 10,
         3 | 7 << 5 | 11 << 10},
        {sge_la64_addi_d(1, 2, 0x923), SGD_LA64_ADDI_D, SI12(sge_la64_addi_d(1, 2, 0x923)), -0x6dd},
        {sge_la64_andi(1, 2, 0x923), SGD_LA64_ANDI, (int64_t)UI12(sge_la64_andi(1, 2, 0x923)), 0x923},
        {sge_la64_slli_w(1, 2, 29), SGD_LA64_SLLI_W, (int64_t)UI5(sge_la64_slli_w(1, 2, 29)), 29},
        {sge_la64_srai_d(1, 2, 61), SGD_LA64_SRAI_D, (int64_t)UI6(sge_la64_srai_d(1, 2, 61)), 61},
        {sge_la64_bstrpick_d(1, 2, 47, 5), SGD_LA64_BSTRPICK_D,
         (int64_t)(MSBD(sge_la64_bstrpick_d(1, 2, 47, 5)) << 8 | LSBD(sge_la64_bstrpick_d(1, 2, 47, 5))), 47 << 8 | 5},
        {sge_la64_lu12i_w(4, 0x87654), SGD_LA64_LU12I_W, SI20(sge_la64_lu12i_w(4, 0x87654)), -0x789ac},
        {sge_la64_lu32i_d(4, 0x12345), SGD_LA64_LU32I_D, SI20(sge_la64_lu32i_d(4, 0x12345)), 0x12345},
        {sge_la64_lu52i_d(4, 5, 0xabc), SGD_LA64_LU52I_D, (int64_t)UI12(sge_la64_lu52i_d(4, 5, 0xabc)), 0xabc},
        {sge_la64_ll_d(4, 5, 0x2abc), SGD_LA64_LL_D, SI14(sge_la64_ll_d(4, 5, 0x2abc)), -0x1544},
        {sge_la64_sc_w(4, 5, 7), SGD_LA64_SC_W, RD(sge_la64_sc_w(4, 5, 7)) | RJ(sge_la64_sc_w(4, 5, 7)) << 5, 4 | 5 << 5},
        {sge_la64_beq(6, 9, 0x8123), SGD_LA64_BEQ,
         OFFS16(sge_la64_beq(6, 9, 0x8123)) * 1024 + RJ(sge_la64_beq(6, 9, 0x8123)) * 32 + RD(sge_la64_beq(6, 9, 0x8123)),
         -0x7edd * 1024 + 6 * 32 + 9},
        {sge_la64_beqz(6, 0x1f2345), SGD_LA64_BEQZ, OFFS21(sge_la64_beqz(6, 0x1f2345)), -0xdcbb},
        {sge_la64_bnez(6, 0x0abcd), SGD_LA64_BNEZ, OFFS21(sge_la64_bnez(6, 0x0abcd)), 0xabcd},
        {sge_la64_b(0x2345678), SGD_LA64_B, OFFS26(sge_la64_b(0x2345678)), 0x2345678 - (1 << 26)},
        {sge_la64_bl(0x0345678), SGD_LA64_BL, OFFS26(sge_la64_bl(0x0345678)), 0x0345678},
        {sge_la64_jirl(1, 20, 0xfffe), SGD_LA64_JIRL, OFFS16(sge_la64_jirl(1, 20, 0xfffe)) * 1024 + RJ(sge_la64_jirl(1, 20, 0xfffe)) * 32 + RD(sge_la64_jirl(1, 20, 0xfffe)), -2 * 1024 + 20 * 32 + 1},
        {sge_la64_pcaddu18i(20, 0xfffff), SGD_LA64_PCADDU18I, SI20(sge_la64_pcaddu18i(20, 0xfffff)), -1},
        {sge_la64_dbar(0), SGD_LA64_DBAR, 0, 0},
        {sge_la64_stx_d(1, 2, 3), SGD_LA64_STX_D, 0, 0},
        {sge_la64_revb_2h(1, 2), SGD_LA64_REVB_2H, 0, 0},
    };
    size_t k;
    for (k = 0; k < sizeof t / sizeof *t; ++k)
        if (decode(t[k].w) != t[k].op || t[k].got != t[k].want) {
            snprintf(c->why, sizeof c->why, "self test %zu (%08x): decoded %d (want %d), field %lld (want %lld)", k,
                     t[k].w, decode(t[k].w), t[k].op, (long long)t[k].got, (long long)t[k].want);
            return -1;
        }
    return 0;
}

/* the access of n bytes at a inside a region */
static int mapped(La64* c, uint64_t a, unsigned n) {
    size_t k;
    for (k = 0; k < c->nregions; ++k)
        if (a >= c->regions[k].lo && a + n <= c->regions[k].hi && a + n >= a) return 1;
    snprintf(c->why, sizeof c->why, "an access of %u bytes at %llx outside the memory", n, (unsigned long long)a);
    return 0;
}
static int load(La64* c, uint64_t a, unsigned n, int sign, uint64_t* v) {
    uint64_t x = 0;
    if (!mapped(c, a, n)) return -1;
    memcpy(&x, (const void*)(uintptr_t)a, n); /* a little-endian host's, as LoongArch's */
    *v = sign && n < 8 ? (uint64_t)sext(x, 8 * n) : x;
    return 0;
}
static int store(La64* c, uint64_t a, unsigned n, uint64_t v) {
    if (!mapped(c, a, n)) return -1;
    memcpy((void*)(uintptr_t)a, &v, n);
    if (c->ll_on && a < c->ll_addr + 8 && c->ll_addr < a + n) c->ll_on = 0;
    return 0;
}
static uint64_t bswap_in(uint64_t v, unsigned unit) {
    uint64_t r = 0;
    unsigned k, j;
    for (k = 0; k < 64; k += unit)
        for (j = 0; j < unit; j += 8) r |= ((v >> (k + j)) & 0xff) << (k + unit - 8 - j);
    return r;
}

int la64_run(La64* c, uint64_t limit) {
    uint64_t* r = c->r;
    while (limit--) {
        uint32_t w;
        uint64_t pc = c->pc, next = pc + 4, a, v, j, k;
        int op;
        r[0] = 0;
        if (pc >= c->trap_lo && pc < c->trap_hi) {
            int t = c->trap(c, c->user);
            if (t <= 0) return t;
            continue;
        }
        if ((pc & 3) || !mapped(c, pc, 4)) {
            snprintf(c->why, sizeof c->why, "pc %llx outside the code", (unsigned long long)pc);
            return -1;
        }
        memcpy(&w, (const void*)(uintptr_t)pc, 4);
        op = decode(w);
        j = r[RJ(w)], k = r[RK(w)];
        switch (op) {
            case SGD_LA64_ADD_D: v = j + k; break;
            case SGD_LA64_ADD_W: v = s32(j + k); break;
            case SGD_LA64_SUB_D: v = j - k; break;
            case SGD_LA64_SUB_W: v = s32(j - k); break;
            case SGD_LA64_AND: v = j & k; break;
            case SGD_LA64_OR: v = j | k; break;
            case SGD_LA64_XOR: v = j ^ k; break;
            case SGD_LA64_NOR: v = ~(j | k); break;
            case SGD_LA64_ANDN: v = j & ~k; break;
            case SGD_LA64_ORN: v = j | ~k; break;
            case SGD_LA64_ADDI_D: v = j + (uint64_t)SI12(w); break;
            case SGD_LA64_ADDI_W: v = s32(j + (uint64_t)SI12(w)); break;
            case SGD_LA64_ANDI: v = j & UI12(w); break;
            case SGD_LA64_ORI: v = j | UI12(w); break;
            case SGD_LA64_XORI: v = j ^ UI12(w); break;
            case SGD_LA64_LU12I_W: v = (uint64_t)(SI20(w) * 4096); break;
            case SGD_LA64_LU32I_D: v = (r[RD(w)] & 0xffffffffu) | (uint64_t)SI20(w) << 32; break;
            case SGD_LA64_LU52I_D: v = (j & 0xFFFFFFFFFFFFFull) | (uint64_t)UI12(w) << 52; break;
            case SGD_LA64_SLL_D: v = j << (k & 63); break;
            case SGD_LA64_SRL_D: v = j >> (k & 63); break;
            case SGD_LA64_SRA_D: v = (uint64_t)((int64_t)j >> (k & 63)); break;
            case SGD_LA64_SLL_W: v = s32((uint32_t)j << (k & 31)); break;
            case SGD_LA64_SRL_W: v = s32((uint32_t)j >> (k & 31)); break;
            case SGD_LA64_SRA_W: v = s32((uint32_t)((int32_t)j >> (k & 31))); break;
            case SGD_LA64_SLLI_D: v = j << UI6(w); break;
            case SGD_LA64_SRLI_D: v = j >> UI6(w); break;
            case SGD_LA64_SRAI_D: v = (uint64_t)((int64_t)j >> UI6(w)); break;
            case SGD_LA64_SLLI_W: v = s32((uint32_t)j << UI5(w)); break;
            case SGD_LA64_SRLI_W: v = s32((uint32_t)j >> UI5(w)); break;
            case SGD_LA64_SRAI_W: v = s32((uint32_t)((int32_t)j >> UI5(w))); break;
            case SGD_LA64_MUL_D: v = j * k; break;
            case SGD_LA64_MUL_W: v = s32(j * k); break;
            case SGD_LA64_MULH_D: v = mulh_s(j, k); break;
            case SGD_LA64_MULH_DU: v = mulh_u(j, k); break;
            /* a quotient by zero, or of the most negative by -1, is UNPREDICTABLE (no trap): the code mustn't ask */
            case SGD_LA64_DIV_D:
                if (!k || ((int64_t)j == INT64_MIN && (int64_t)k == -1)) {
                    snprintf(c->why, sizeof c->why, "DIV.D of %llx by %llx at %llx", (unsigned long long)j,
                             (unsigned long long)k, (unsigned long long)pc);
                    return -1;
                }
                v = (uint64_t)((int64_t)j / (int64_t)k);
                break;
            case SGD_LA64_DIV_DU:
                if (!k) {
                    snprintf(c->why, sizeof c->why, "DIV.DU by 0 at %llx", (unsigned long long)pc);
                    return -1;
                }
                v = j / k;
                break;
            case SGD_LA64_SLT: v = (int64_t)j < (int64_t)k; break;
            case SGD_LA64_SLTU: v = j < k; break;
            case SGD_LA64_SLTI: v = (int64_t)j < SI12(w); break;
            case SGD_LA64_SLTUI: v = j < (uint64_t)SI12(w); break;
            case SGD_LA64_EXT_W_B: v = (uint64_t)sext(j, 8); break;
            case SGD_LA64_EXT_W_H: v = (uint64_t)sext(j, 16); break;
            case SGD_LA64_BSTRPICK_D:
                if (MSBD(w) < LSBD(w)) goto undefined;
                v = (j >> LSBD(w)) & (~0ull >> (63 - MSBD(w) + LSBD(w)));
                break;
            case SGD_LA64_MASKEQZ: v = k ? j : 0; break;
            case SGD_LA64_MASKNEZ: v = k ? 0 : j; break;
            case SGD_LA64_REVB_D: v = bswap_in(j, 64); break;
            case SGD_LA64_REVB_2W: v = bswap_in(j, 32); break;
            case SGD_LA64_REVB_2H: v = s32(bswap_in(j, 16)); break;
            case SGD_LA64_LD_B: case SGD_LA64_LD_BU: case SGD_LA64_LD_H: case SGD_LA64_LD_HU: case SGD_LA64_LD_W:
            case SGD_LA64_LD_WU: case SGD_LA64_LD_D: {
                static const unsigned char n[] = {[SGD_LA64_LD_B] = 1, [SGD_LA64_LD_BU] = 1, [SGD_LA64_LD_H] = 2,
                                                  [SGD_LA64_LD_HU] = 2, [SGD_LA64_LD_W] = 4, [SGD_LA64_LD_WU] = 4,
                                                  [SGD_LA64_LD_D] = 8};
                int sign = op == SGD_LA64_LD_B || op == SGD_LA64_LD_H || op == SGD_LA64_LD_W;
                if (load(c, j + (uint64_t)SI12(w), n[op], sign, &v)) return -1;
                break;
            }
            case SGD_LA64_LDX_B: case SGD_LA64_LDX_BU: case SGD_LA64_LDX_H: case SGD_LA64_LDX_HU: case SGD_LA64_LDX_W:
            case SGD_LA64_LDX_WU: case SGD_LA64_LDX_D: {
                static const unsigned char n[] = {[SGD_LA64_LDX_B] = 1, [SGD_LA64_LDX_BU] = 1, [SGD_LA64_LDX_H] = 2,
                                                  [SGD_LA64_LDX_HU] = 2, [SGD_LA64_LDX_W] = 4, [SGD_LA64_LDX_WU] = 4,
                                                  [SGD_LA64_LDX_D] = 8};
                int sign = op == SGD_LA64_LDX_B || op == SGD_LA64_LDX_H || op == SGD_LA64_LDX_W;
                if (load(c, j + k, n[op], sign, &v)) return -1;
                break;
            }
            case SGD_LA64_ST_B: case SGD_LA64_ST_H: case SGD_LA64_ST_W: case SGD_LA64_ST_D: {
                unsigned n = op == SGD_LA64_ST_B ? 1 : op == SGD_LA64_ST_H ? 2 : op == SGD_LA64_ST_W ? 4 : 8;
                if (store(c, j + (uint64_t)SI12(w), n, r[RD(w)])) return -1;
                c->pc = next;
                continue;
            }
            case SGD_LA64_STX_B: case SGD_LA64_STX_H: case SGD_LA64_STX_W: case SGD_LA64_STX_D: {
                unsigned n = op == SGD_LA64_STX_B ? 1 : op == SGD_LA64_STX_H ? 2 : op == SGD_LA64_STX_W ? 4 : 8;
                if (store(c, j + k, n, r[RD(w)])) return -1;
                c->pc = next;
                continue;
            }
            /* LL and SC: one thread's, the reservation lost to any store over it */
            case SGD_LA64_LL_W: case SGD_LA64_LL_D:
                a = j + (uint64_t)(SI14(w) * 4);
                if (a & (op == SGD_LA64_LL_D ? 7 : 3)) goto misaligned;
                if (load(c, a, op == SGD_LA64_LL_D ? 8 : 4, 1, &v)) return -1;
                c->ll_addr = a, c->ll_on = 1;
                break;
            case SGD_LA64_SC_W: case SGD_LA64_SC_D:
                a = j + (uint64_t)(SI14(w) * 4);
                if (a & (op == SGD_LA64_SC_D ? 7 : 3)) goto misaligned;
                v = c->ll_on && c->ll_addr == a;
                if (v && store(c, a, op == SGD_LA64_SC_D ? 8 : 4, r[RD(w)])) return -1;
                c->ll_on = 0;
                break;
            case SGD_LA64_DBAR: c->pc = next; continue;
            case SGD_LA64_BEQ: if (j == r[RD(w)]) next = pc + (uint64_t)(OFFS16(w) * 4); c->pc = next; continue;
            case SGD_LA64_BNE: if (j != r[RD(w)]) next = pc + (uint64_t)(OFFS16(w) * 4); c->pc = next; continue;
            case SGD_LA64_BEQZ: if (!j) next = pc + (uint64_t)(OFFS21(w) * 4); c->pc = next; continue;
            case SGD_LA64_BNEZ: if (j) next = pc + (uint64_t)(OFFS21(w) * 4); c->pc = next; continue;
            case SGD_LA64_B: c->pc = pc + (uint64_t)(OFFS26(w) * 4); continue;
            case SGD_LA64_BL: r[1] = next; c->pc = pc + (uint64_t)(OFFS26(w) * 4); continue;
            case SGD_LA64_JIRL:
                a = j + (uint64_t)(OFFS16(w) * 4);
                if (RD(w)) r[RD(w)] = next;
                c->pc = a;
                continue;
            case SGD_LA64_PCADDU18I: v = pc + (uint64_t)(SI20(w) * (1 << 18)); break;
            case SGD_LA64_PCADDI: v = pc + (uint64_t)(SI20(w) * 4); break;
            default:
            undefined:
                snprintf(c->why, sizeof c->why, "word %08x at %llx: none the back end writes", w, (unsigned long long)pc);
                return -1;
            misaligned:
                snprintf(c->why, sizeof c->why, "a misaligned LL or SC at %llx", (unsigned long long)pc);
                return -1;
        }
        if (RD(w)) r[RD(w)] = v;
        c->pc = next;
    }
    snprintf(c->why, sizeof c->why, "out of instructions at %llx", (unsigned long long)c->pc);
    return -1;
}
