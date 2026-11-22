/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's AArch32 blocks: what cpu/arm/llvm.c's arm_block makes of an A32 block (the Cortex-A57, flat
   memory), without LLVM. Each word is sailgen's emitter of the AArch32 Sail where its clauses take it (a32_x64.inc:
   the data processing and branches), else one written here over the same SSA region, as llvm.c's emitters are
   written over LLVM's builder (the loads and stores, the multiplies, the exclusives...). The region is assembled where
   a condition's skip ends it, where it grows long, and at the block's end; the guest's registers are in the state at
   each region's end, as each write stores them.

   The function is a block's: void name(SnxArmCPU*), System V x86-64, the state in RDI kept in RBX (pushed), RSP
   16-aligned after the prologue (the push and a frame of spill slots, its size patched in at the end). Its entry
   checks are llvm.c's block_begin's (exit and syndrome cleared, R15 the block's address, CPSR.T and E clear), and it
   ends where arm_block ends: after an instruction that branches, one that leaves for the backend, or the last word.
   Where the block goes on to a known address in A32 or Thumb state and the caller names that block a successor, the
   code jumps to it as chain_block's tail call does (reading the thread's poll first where it is at or before the
   block's start); anywhere else it returns, with R15 the next instruction's */
#include "common/cpu/recompiler/x64/x64_arm.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/cpu/arm/arm.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) && !defined(_WIN32) && defined(SNX_HAVE_X64)
#include <elf.h>

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* a word the emitters can't make (bits.h's sg_fail) refuses the block, which LLVM makes; why, for the caller */
static _Thread_local jmp_buf* g_fail;
static _Thread_local const char* t_why;
#define SG_FAIL_JUMP             g_fail
#define SG_FAIL_NOTE(what, a, b) (t_why = (what), (void)(a), (void)(b))

#include "a32_x64.inc"

/* a relocation of the block being made, at its code's offset */
typedef struct {
    size_t at;
    char sym[24];
    unsigned type;
    long long addend;
} Rel;

struct SnxX64Arm {
    SgCtx g;
    SgXAsm a;
    SgXBuf b;
    Rel* rel;
    int nrel, cap_rel;
    SnxXElf* elf;
    size_t blocks;
    unsigned features;
    int pool;
    const char* why;
};

int snx_x64_arm_available(void) { return snx_x64_available(); }

SnxX64Arm* snx_x64_arm_new(void) {
    SnxX64Arm* c;
    if (!snx_x64_available()) return NULL;
    c = (SnxX64Arm*)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->b.cap = 1u << 16;
    c->b.p = (unsigned char*)malloc(c->b.cap);
    c->elf = snx_xelf_new();
    if (!c->b.p || !c->elf) {
        snx_x64_arm_free(c);
        return NULL;
    }
    c->features = snx_x64_features();
    return c;
}
void snx_x64_arm_free(SnxX64Arm* c) {
    if (!c) return;
    free(c->b.p);
    free(c->rel);
    snx_xelf_free(c->elf);
    free(c->g.label_at), free(c->g.fix), free(c->g.exits), free(c->g.monitors);
    free(c->a.cold.p), free(c->a.pool), free(c->a.rips), free(c->a.cjumps), free(c->a.ccalls);
    free(c);
}
void snx_x64_arm_limit(SnxX64Arm* c, int pool, unsigned features) {
    c->pool = pool;
    c->features = snx_x64_features() & features;
}
const char* snx_x64_arm_why(const SnxX64Arm* c) { return c->why ? c->why : ""; }

/* a call of a helper (sgx_a32_call's), the chain's jumps and the poll's load: relocations of the block */
static void add_rel(SnxX64Arm* c, size_t at, const char* sym, unsigned type, long long addend) {
    size_t n = strlen(sym);
    if (n >= sizeof c->rel[0].sym) sg_fail("a symbol's name too long", (long long)n, 0);
    c->rel = (Rel*)sgx_a32_grow(c->rel, &c->cap_rel, c->nrel + 1, sizeof *c->rel);
    c->rel[c->nrel].at = at;
    memcpy(c->rel[c->nrel].sym, sym, n + 1);
    c->rel[c->nrel].type = type;
    c->rel[c->nrel++].addend = addend;
}
static void unit_reloc(void* machine, size_t at, const char* sym, long long addend) {
    SnxX64Arm* c = (SnxX64Arm*)((char*)machine - offsetof(SnxX64Arm, g));
    add_rel(c, at, sym, R_X86_64_PLT32, addend);
}

/* the machine's instructions are x64_a32.h's own */
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r) {
    (void)a, (void)r;
    sg_fail("a machine instruction of no kind", at, 0);
}

/* ---- regions ---- */

/* the region so far assembled (what its fields wrote stored first), and what the block knows forgotten */
static void flush(SnxX64Arm* c) {
    SgCtx* g = &c->g;
    int k;
    sg_flags_flush(g);
    if (g->x.n) sgx_assemble_region(&c->a, &g->x);
    for (k = 0; k < 15; ++k) g->gpr[k] = 0;
    g->cpsr = 0;
    g->limit = g->mem = 0;
    g->origin = -1;
    g->x.flags_ins = -1;
}

/* ---- the instructions the Sail has no clause for: llvm.c's, on the region's SSA ---- */

/* R[r] as an operand: the PC reads as the instruction's address plus 8 */
static SgD reg(SgCtx* g, unsigned r) { return r == 15 ? sgx_const(32, (SnxU32)g->pc + 8) : sg_GPR_read(g, r); }
/* a branch to value in ARM state on the Cortex-A57 (llvm.c's branch_exchange): CPSR.T from bit 0, R15 the rest */
static void write_pc(SgCtx* g, SgD value) {
    sg_PSTATE_write(g, SG_PSTATE_T, 1, sg_d_slice(g, value, 0, 0));
    g->branched = 1;
    g->target = sg_d_and(g, value, sgx_const(32, ~1u));
}
/* a shift by an immediate (llvm.c's shift with immediate set), its carry not taken */
static SgD shift_imm(SgCtx* g, SgD v, unsigned type, unsigned imm) {
    if (!imm && type == 0) return v;
    if (!imm && type == 3) /* RRX */
        return sg_d_or(g, sg_d_lsl(g, sg_d_zext(g, sg_PSTATE_read(g, SG_PSTATE_C), 32), 31), sg_d_lsr(g, v, 1));
    if (!imm) imm = 32;
    switch (type) {
        case 0: return sg_d_lsl(g, v, imm);
        case 1: return imm == 32 ? sgx_const(32, 0) : sg_d_lsr(g, v, imm);
        case 2: return sg_d_asr(g, v, imm == 32 ? 31 : imm);
        default: return sg_d_ror(g, v, imm);
    }
}
/* the instruction's condition, its skip made: what follows runs where it holds */
static void condition(SgCtx* g) {
    sg_when(g, sgx_a32_condition(g, g->word >> 28));
    sgx_a32_commit(g);
}

/* LDR, STR, LDRB, STRB and their T forms, LDRH, STRH, LDRSB, LDRSH, LDRD, STRD (llvm.c's memory) */
static int memory(SgCtx* g, SnxA32Kind kind) {
    SnxU32 w = g->word;
    unsigned n = (w >> 16) & 15, d = (w >> 12) & 15, op = (w >> 5) & 3;
    int pre = !!(w & (1u << 24)), up = !!(w & (1u << 23));
    int wb = !pre || !!(w & (1u << 21)), load = !!(w & (1u << 20));
    unsigned bytes = 4, alignment;
    int sign = 0, pair = 0;
    SgD base, offset, adjusted, addr, value, second;
    condition(g);
    base = reg(g, n);
    if (kind == SNX_A32_SINGLE) {
        bytes = w & (1u << 22) ? 1 : 4;
        offset = w & (1u << 25) ? shift_imm(g, reg(g, w & 15), (w >> 5) & 3, (w >> 7) & 31) : sgx_const(32, w & 4095);
    } else {
        offset = w & (1u << 22) ? sgx_const(32, ((w >> 4) & 0xf0) | (w & 15)) : reg(g, w & 15);
        if (!load && op >= 2) {
            pair = 1;
            load = op == 2;
        } else {
            bytes = op == 2 ? 1 : 2;
            sign = op >= 2;
        }
    }
    adjusted = up ? sg_d_add(g, base, offset) : sg_d_sub(g, base, offset);
    addr = pre ? adjusted : base;
    /* LDRD and STRD, and a load of the PC, word aligned (llvm.c's: unaligned support on, as ARMv6's SCTLR.U) */
    alignment = pair || (load && d == 15) ? 4 : 1;
    sgx_a32_bounds(g, addr, pair ? 8 : bytes, alignment);
    if (load) {
        value = sgx_a32_load(g, addr, (int)bytes, 0);
        if (sign)
            value = sg_d_sext(g, value, 32);
        else if (bytes < 4)
            value = sg_d_zext(g, value, 32);
        if (pair) second = sgx_a32_load(g, addr, 4, 4); /* the range checked holds both: no wrap at 4 */
        if (d == 15)
            write_pc(g, value);
        else
            sg_GPR_write(g, d, value);
        if (pair) sg_GPR_write(g, d + 1, second);
    } else {
        value = reg(g, d); /* the Cortex-A57's PCStoreValue: the address plus 8 */
        if (pair) second = reg(g, d + 1);
        sgx_a32_store(g, addr, (int)bytes, 0, bytes < 4 ? sg_d_slice(g, value, 8 * bytes - 1, 0) : value);
        if (pair) sgx_a32_store(g, addr, 4, 4, second);
        /* one check of the monitor for the bytes stored: their range overlaps the reservation exactly where one of
           its words does (llvm.c checks each store) */
        sgx_a32_monitor(g, addr, pair ? 8 : bytes);
    }
    if (wb) sg_GPR_write(g, n, adjusted);
    return d == 15 && load;
}

/* LDM, STM and their stack forms (llvm.c's multiple, on the Cortex-A57) */
static int multiple(SgCtx* g) {
    SnxU32 w = g->word, list = w & 0xFFFF;
    unsigned n = (w >> 16) & 15, count = 0, index = 0, i;
    int pre = !!(w & (1u << 24)), up = !!(w & (1u << 23)), load = !!(w & (1u << 20));
    SgD base, adjusted, start, values[16];
    condition(g);
    for (i = 0; i < 16; ++i) count += (list >> i) & 1;
    base = reg(g, n);
    adjusted = up ? sg_d_add(g, base, sgx_const(32, count * 4)) : sg_d_sub(g, base, sgx_const(32, count * 4));
    start = up ? sg_d_add(g, base, sgx_const(32, pre ? 4 : 0)) : sg_d_add(g, adjusted, sgx_const(32, pre ? 0 : 4));
    sgx_reg(sgx(g), &start);
    sgx_a32_bounds(g, start, count * 4, 4);
    /* every source read before any update, the base and the PC among them */
    for (i = 0; i < 16; ++i)
        if (list >> i & 1) {
            values[i] = load ? sgx_a32_load(g, start, 4, (int)(index * 4)) : reg(g, i);
            ++index;
        }
    if (load && (list >> 15 & 1)) write_pc(g, values[15]);
    index = 0;
    for (i = 0; i < 16; ++i)
        if (list >> i & 1) {
            if (load) {
                if (i != 15) sg_GPR_write(g, i, values[i]);
            } else
                sgx_a32_store(g, start, 4, (int)(index * 4), values[i]);
            ++index;
        }
    if (!load) sgx_a32_monitor(g, start, count * 4); /* one check for the words stored, as memory's */
    /* writeback after the stores (STM stored the old base), but not over a loaded base */
    if ((w & (1u << 21)) && !(load && (list >> n & 1))) sg_GPR_write(g, n, adjusted);
    return load && (list >> 15 & 1);
}

/* N and Z of a result (llvm.c's nz without a carry or an overflow), w its width */
static void nz(SgCtx* g, SgD r, unsigned w) {
    sg_PSTATE_write(g, SG_PSTATE_N, 1, sg_d_slice(g, r, w - 1, w - 1));
    sg_PSTATE_write(g, SG_PSTATE_Z, 1, sg_d_is_zero_bit(g, r));
}

/* MUL, MLA, MLS, UMULL, UMLAL, SMULL, SMLAL, UMAAL (llvm.c's multiply) */
static void multiply(SgCtx* g, SnxA32Kind kind) {
    SgX* x = sgx(g);
    SnxU32 w = g->word;
    unsigned n = (w >> 16) & 15, d = (w >> 12) & 15;
    SgD a, b, r;
    condition(g);
    a = reg(g, w & 15), b = reg(g, (w >> 8) & 15);
    if (kind == SNX_A32_MULL) {
        int umaal = (w & 0x00e00000) == 0x00400000, sign = !umaal && (w & (1u << 22));
        a = sign ? sg_d_sext(g, a, 64) : sg_d_zext(g, a, 64);
        b = sign ? sg_d_sext(g, b, 64) : sg_d_zext(g, b, 64);
        r = SGX_OP(x, mul, 64, a, b);
        if (umaal) /* Rn * Rm + RdHi + RdLo, unsigned: never past 64 bits */
            r = sg_d_add(g, r, sg_d_add(g, sg_d_zext(g, reg(g, d), 64), sg_d_zext(g, reg(g, n), 64)));
        else if (w & (1u << 21))
            r = sg_d_add(g, r, sg_d_concat(g, reg(g, n), reg(g, d)));
        sg_GPR_write(g, d, sg_d_slice(g, r, 31, 0));
        sg_GPR_write(g, n, sg_d_slice(g, r, 63, 32));
        if (!umaal && (w & (1u << 20))) nz(g, r, 64);
        return;
    }
    r = SGX_OP(x, mul, 32, a, b);
    if (kind == SNX_A32_MLS)
        r = sg_d_sub(g, reg(g, d), r);
    else if (w & (1u << 21))
        r = sg_d_add(g, r, reg(g, d));
    if (w & (1u << 20)) nz(g, r, 32);
    sg_GPR_write(g, n, r);
}

/* SDIV, UDIV: Rd 19:16, Rm 11:8, Rn 3:0; by 0 they give 0 (llvm.c's miscellaneous) */
static void divide(SgCtx* g) {
    SgX* x = sgx(g);
    SnxU32 w = g->word;
    int u = !!(w & (1u << 21));
    SgD n, m, q;
    SgB zero;
    condition(g);
    n = reg(g, w & 15), m = reg(g, (w >> 8) & 15);
    /* 64 bits: the most negative over -1 needs no care there, and its quotient's low word is the Arm ARM's */
    n = u ? sg_d_zext(g, n, 64) : sg_d_sext(g, n, 64);
    m = u ? sg_d_zext(g, m, 64) : sg_d_sext(g, m, 64);
    zero = sg_d_is_zero(g, m);
    m = sg_d_select(g, zero, sgx_const(64, 1), m);
    q = u ? SGX_OP(x, udiv, 64, n, m) : SGX_OP(x, sdiv, 64, n, sg_d_asr(g, n, 63), m);
    sg_GPR_write(g, (w >> 16) & 15, sg_d_select(g, zero, sgx_const(32, 0), sg_d_slice(g, q, 31, 0)));
}

/* CLZ, REV, REV16, REVSH, RBIT, the extends, the bitfields, MRS and MSR (llvm.c's miscellaneous) */
static int miscellaneous(SgCtx* g, SnxA32Kind kind) {
    SgX* x = sgx(g);
    SnxU32 w = g->word;
    unsigned d = (w >> 12) & 15, n = (w >> 16) & 15;
    SgD v, r;
    condition(g);
    v = reg(g, w & 15);
    switch (kind) {
        case SNX_A32_CLZ: r = sg_i_slice(g, sg_i_clz(g, v), 31, 0); break;
        case SNX_A32_REV:
            if ((w & 0x00400080) == 0x00400000)
                r = sg_d_bitreverse(g, v);
            else if (!(w & 0x80))
                r = SGX_OP(x, rev, 32, v);
            else {
                r = sg_d_or(g, sg_d_and(g, sg_d_lsr(g, v, 8), sgx_const(32, 0x00ff00ff)),
                            sg_d_lsl(g, sg_d_and(g, v, sgx_const(32, 0x00ff00ff)), 8));
                if (w & (1u << 22)) r = sg_d_sext(g, sg_d_slice(g, r, 15, 0), 32); /* REVSH */
            }
            break;
        case SNX_A32_EXTEND: {
            SgD t = sg_d_ror(g, v, ((w >> 10) & 3) * 8);
            int sign = !(w & (1u << 22));
            if (!(w & (3u << 20))) { /* SXTB16, UXTB16, with add: bytes 0 and 2 */
                SgD lo = sign ? sg_d_sext(g, sg_d_slice(g, t, 7, 0), 32) : sg_d_zext(g, sg_d_slice(g, t, 7, 0), 32);
                SgD hi = sign ? sg_d_sext(g, sg_d_slice(g, t, 23, 16), 32) : sg_d_zext(g, sg_d_slice(g, t, 23, 16), 32);
                if (n != 15) {
                    SgD rn = reg(g, n);
                    lo = sg_d_add(g, lo, rn);
                    hi = sg_d_add(g, hi, sg_d_lsr(g, rn, 16));
                }
                r = sg_d_or(g, sg_d_and(g, lo, sgx_const(32, 0xffff)), sg_d_lsl(g, hi, 16));
            } else {
                unsigned bits = w & (1u << 20) ? 16 : 8;
                SgD e = sg_d_slice(g, t, bits - 1, 0);
                r = sign ? sg_d_sext(g, e, 32) : sg_d_zext(g, e, 32);
                if (n != 15) r = sg_d_add(g, r, reg(g, n));
            }
            break;
        }
        case SNX_A32_BITFIELD: {
            unsigned lsb = (w >> 7) & 31, width;
            SnxU32 mask;
            if ((w & 0x0fe00070) == 0x07c00010) { /* BFI, BFC */
                width = ((w >> 16) & 31) - lsb + 1;
                mask = UINT32_MAX >> (32 - width);
                r = (w & 15) == 15 ? sgx_const(32, 0) : sg_d_lsl(g, sg_d_and(g, v, sgx_const(32, mask)), lsb);
                r = sg_d_or(g, sg_d_and(g, reg(g, d), sgx_const(32, ~(mask << lsb))), r);
            } else { /* SBFX, UBFX */
                width = ((w >> 16) & 31) + 1;
                r = sg_d_slice(g, v, lsb + width - 1, lsb);
                r = w & (1u << 22) ? sg_d_zext(g, r, 32) : sg_d_sext(g, r, 32);
            }
            break;
        }
        case SNX_A32_MRS: /* the execution state bits (IT, J, T) read as zero */
            sg_flags_flush(g);
            r = sg_d_and(g, sgx_a32_cpsr(g), sgx_const(32, ~(0x0600FC00u | (1u << 24) | SNX_ARM_T)));
            break;
        case SNX_A32_MSR: {
            SnxU32 mask = (n & 8 ? 0xf8000000u : 0) | (n & 4 ? 0x000f0000u : 0) | (n & 2 ? SNX_ARM_E : 0);
            SgD c;
            int k;
            if (w & (1u << 25)) {
                unsigned rot = ((w >> 8) & 15) * 2;
                SnxU32 imm = w & 255;
                if (rot) imm = (imm >> rot) | (imm << (32 - rot));
                v = sgx_const(32, imm);
            }
            sg_flags_flush(g);
            c = sg_d_or(g, sg_d_and(g, sgx_a32_cpsr(g), sgx_const(32, ~mask)), sg_d_and(g, v, sgx_const(32, mask)));
            k = sgx_reg(x, &c);
            {
                SgXIns* st = sgx_append(x, SGX_K_STORE);
                st->w = 32;
                st->in[0] = k;
                st->disp = (int)offsetof(SnxArmCPU, cpsr);
            }
            g->cpsr = k;
            g->origin = -1;
            /* a block reads E only on entry: one that may have changed it ends here */
            return (w & (1u << 17)) ? 1 : 0;
        }
        default: sg_fail("an A32 instruction this back end does not make", kind, 0);
    }
    sg_GPR_write(g, d, r);
    return 0;
}

/* LDREX, STREX and their byte, halfword and doubleword forms, with acquire or not (llvm.c's exclusive, A32: no offset):
   the CPU's value-based monitor, the store a compare-and-swap of the value the load read */
static void exclusive(SgCtx* g) {
    SgX* x = sgx(g);
    SnxU32 w = g->word;
    unsigned op = (w >> 20) & 7, n = (w >> 16) & 15, d = (w >> 12) & 15, t = w & 15;
    static const unsigned widths[] = {4, 8, 1, 2};
    unsigned bytes = widths[op >> 1];
    SgD addr, value;
    condition(g);
    addr = reg(g, n);
    sgx_reg(x, &addr);
    sgx_a32_bounds(g, addr, bytes, bytes);
    if (op & 1) {
        SgXIns* i;
        int k;
        value = sgx_a32_load(g, addr, (int)bytes, 0);
        value = sg_d_zext(g, value, 64);
        k = sgx_reg(x, &value);
        i = sgx_append(x, SGX_K_STORE);
        i->w = 64, i->in[0] = k, i->disp = (int)offsetof(SnxArmCPU, exclusive_value);
        k = sgx_reg(x, &addr);
        i = sgx_append(x, SGX_K_STORE);
        i->w = 32, i->in[0] = k, i->disp = (int)offsetof(SnxArmCPU, exclusive_address);
        {
            SgD size = sgx_const(32, bytes), one = sgx_const(32, 1);
            k = sgx_reg(x, &size);
            i = sgx_append(x, SGX_K_STORE);
            i->w = 32, i->in[0] = k, i->disp = (int)offsetof(SnxArmCPU, exclusive_size);
            k = sgx_reg(x, &one);
            i = sgx_append(x, SGX_K_STORE);
            i->w = 32, i->in[0] = k, i->disp = (int)offsetof(SnxArmCPU, exclusive_valid);
        }
        sg_GPR_write(g, d, sg_d_slice(g, value, 31, 0));
        if (bytes == 8) sg_GPR_write(g, d + 1, sg_d_slice(g, value, 63, 32));
    } else {
        SgD in[3];
        SgXIns* i;
        static const signed char fix[3] = {6, 2, 10}; /* RSI, RDX, R10 */
        int k, regs[3];
        value = reg(g, t);
        if (bytes == 8) value = sg_d_concat(g, reg(g, t + 1), value);
        in[0] = sgx_zext(x, addr, 64);
        in[1] = value;
        in[2] = sgx_val(64, sgx_a32_mem(g));
        for (k = 0; k < 3; ++k) regs[k] = sgx_reg(x, &in[k]);
        i = sgx_a32_machine(g, SGX_A32_STREX, 1);
        for (k = 0; k < 3; ++k) i->in[k] = regs[k], i->fixed[k] = fix[k];
        i->imm = bytes;
        i->mclobber = 1u << 1; /* RCX */
        i->side = 1;
        i->def = sgx_vreg(x, x->n - 1, 32);
        sg_GPR_write(g, d, sgx_val(32, i->def));
    }
}

/* LDA, STL and their byte and halfword forms: a sequentially consistent access (an x86 load is one; a store is MOV
   then MFENCE, as the store-release the LLVM back end makes) */
static void acquire_release(SgCtx* g) {
    SgX* x = sgx(g);
    SnxU32 w = g->word;
    unsigned op = (w >> 20) & 7, bytes = op >= 6 ? 2 : op >= 4 ? 1 : 4;
    SgD addr;
    condition(g);
    addr = reg(g, (w >> 16) & 15);
    sgx_reg(x, &addr);
    sgx_a32_bounds(g, addr, bytes, bytes);
    if (op & 1) {
        SgD v = sgx_a32_load(g, addr, (int)bytes, 0);
        sg_GPR_write(g, (w >> 12) & 15, bytes < 4 ? sg_d_zext(g, v, 32) : v);
    } else {
        SgD v = reg(g, w & 15);
        sgx_a32_store(g, addr, (int)bytes, 0, bytes < 4 ? sg_d_slice(g, v, 8 * bytes - 1, 0) : v);
        sgx_a32_machine(g, SGX_A32_FENCE, 0);
        sgx_a32_monitor(g, addr, bytes);
    }
}

/* a state field of 32 bits = the constant v */
static void state_set(SgCtx* g, size_t field, SnxU32 v) {
    SgX* x = sgx(g);
    SgD k = sgx_const(32, v);
    int r = sgx_reg(x, &k);
    SgXIns* i = sgx_append(x, SGX_K_STORE);
    i->w = 32;
    i->in[0] = r;
    i->disp = (int)field;
}

/* llvm.c's instruction() for the kinds the Sail's clauses don't take: 1 where the block ends after it, 0 where it
   goes on; a kind this back end doesn't make refuses the block */
static int instruction(SgCtx* g, SnxA32Kind kind) {
    SnxU32 w = g->word;
    switch (kind) {
        case SNX_A32_SINGLE:
        case SNX_A32_EXTRA: return memory(g, kind);
        case SNX_A32_MULTIPLE: return multiple(g);
        case SNX_A32_MUL:
        case SNX_A32_MULL:
        case SNX_A32_MLS: multiply(g, kind); return 0;
        case SNX_A32_DIV: divide(g); return 0;
        case SNX_A32_CLZ:
        case SNX_A32_REV:
        case SNX_A32_EXTEND:
        case SNX_A32_BITFIELD:
        case SNX_A32_MRS:
        case SNX_A32_MSR: return miscellaneous(g, kind);
        case SNX_A32_EXCLUSIVE: exclusive(g); return 0;
        case SNX_A32_ACQUIRE_RELEASE: acquire_release(g); return 0;
        case SNX_A32_CLREX:
            condition(g);
            state_set(g, offsetof(SnxArmCPU, exclusive_valid), 0);
            return 0;
        case SNX_A32_BARRIER: /* stronger than each valid DMB and DSB scope, never just a compiler barrier */
            condition(g);
            sgx_a32_machine(g, SGX_A32_FENCE, 0);
            return 0;
        case SNX_A32_HINT: return 0; /* hints may complete without waiting */
        case SNX_A32_COPROC: { /* the backend's (SnxArmRt.coproc): it reads or writes the registers, or stops here */
            SgD word = sgx_const(32, w);
            SgXIns* i;
            condition(g);
            sgx_a32_pc(g, sgx_const(32, (SnxU32)g->pc));
            sgx_a32_call(g, "snx_arm_coproc", &word, 1, 0);
            sgx_a32_machine(g, SGX_A32_CHECK, 1); /* the block returns where the backend stopped */
            return 0;
        }
        case SNX_A32_SVC:
        case SNX_A32_BKPT:
            condition(g);
            sg_exception(g, kind == SNX_A32_SVC ? SNX_ARM_SVC : SNX_ARM_BREAKPOINT,
                         sgx_const(32, kind == SNX_A32_SVC ? w & 0xffffff : ((w >> 4) & 0xfff0) | (w & 15)));
            return 1;
        case SNX_A32_UNDEFINED:
            condition(g);
            sg_undefined(g);
            return 1;
        default: sg_fail("an A32 instruction this back end does not make", kind, 0);
    }
}

/* ---- the block ---- */

static void name_of(char* out, size_t size, SnxU32 key) {
    snprintf(out, size, "%c_%08lx", key & 1 ? 'T' : 'A', (unsigned long)(key & ~1u));
}

/* the prologue: RBX the state, the frame (patched), and block_begin's checks (they set the exit and return, R15 left
   as it is) */
static void prologue(SnxX64Arm* c, SnxU32 pc, SnxU32 first, int* bad_pc, int* bad_state) {
    SgXBuf* b = &c->b;
    SgCtx* g = &c->g;
    sgx_bytes(b, "53 48 89 FB 48 81 EC"); /* push rbx; mov rbx, rdi; sub rsp, imm32 */
    c->a.frame_at[c->a.nframe++] = b->n;
    sgx_imm(b, 0, 4);
    sgx_bytes(b, "48 C7 83"), sgx_imm(b, (long long)offsetof(SnxArmCPU, exit), 4),
        sgx_imm(b, 0, 4); /* exit, syndrome */
    sgx_bytes(b, "81 BB"), sgx_imm(b, (long long)offsetof(SnxArmCPU, r) + 60, 4), sgx_imm(b, pc, 4); /* cmp r15, pc */
    *bad_pc = sgx_a32_label(g);
    sgx_a32_jcc(g, b, 5, *bad_pc);
    sgx_bytes(b, "F7 83"), sgx_imm(b, (long long)offsetof(SnxArmCPU, cpsr), 4), sgx_imm(b, SNX_ARM_T | SNX_ARM_E, 4);
    *bad_state = sgx_a32_label(g);
    sgx_a32_jcc(g, b, 5, *bad_state);
    (void)first;
}
/* the prologue's exits: BAD_ENTRY, its syndrome the block's address, where R15 isn't it; where CPSR.T is set
   BAD_ENTRY, else (E) ENDIAN_UNSUPPORTED, their syndrome the first word */
static void prologue_exits(SnxX64Arm* c, SnxU32 pc, SnxU32 first, int bad_pc, int bad_state) {
    SgXBuf* b = &c->b;
    SgCtx* g = &c->g;
    size_t skip;
    sgx_a32_bind(g, b, bad_pc);
    sgx_a32_state_imm(b, offsetof(SnxArmCPU, exit), SNX_ARM_BAD_ENTRY);
    sgx_a32_state_imm(b, offsetof(SnxArmCPU, syndrome), pc);
    sgx_a32_jmp(g, b, g->ret);
    sgx_a32_bind(g, b, bad_state);
    sgx_a32_state_imm(b, offsetof(SnxArmCPU, exit), SNX_ARM_BAD_ENTRY);
    sgx_bytes(b, "F7 83"), sgx_imm(b, (long long)offsetof(SnxArmCPU, cpsr), 4), sgx_imm(b, SNX_ARM_T, 4);
    sgx_bytes(b, "75"), skip = b->n, sgx_byte(b, 0); /* jnz: T, BAD_ENTRY */
    sgx_a32_state_imm(b, offsetof(SnxArmCPU, exit), SNX_ARM_ENDIAN_UNSUPPORTED);
    sgx_a32_patch8(b, skip);
    sgx_a32_state_imm(b, offsetof(SnxArmCPU, syndrome), first);
    sgx_a32_jmp(g, b, g->ret);
}
/* the frame given back and RBX the caller's */
static void epilogue(SnxX64Arm* c) {
    SgXBuf* b = &c->b;
    if (c->a.nframe >= 256) sg_fail("a block of too many epilogues", c->a.nframe, 256);
    sgx_bytes(b, "48 81 C4"); /* add rsp, imm32 */
    c->a.frame_at[c->a.nframe++] = b->n;
    sgx_imm(b, 0, 4);
    sgx_bytes(b, "5B"); /* pop rbx */
}
/* where the block ends with R15 key's address (stored) in key's state: a jump to key's block where it is one of the
   successors (after the poll, for one at or before the block's start), else the return */
static void go_on(SnxX64Arm* c, SnxU32 key, SnxU32 block, const SnxU32* next, size_t nnext) {
    SgXBuf* b = &c->b;
    SgCtx* g = &c->g;
    size_t k;
    char sym[24];
    for (k = 0; k < nnext && next[k] != key; ++k) {}
    if (k == nnext) {
        sgx_a32_jmp(g, b, g->ret);
        return;
    }
    if ((key & ~1u) <= block) {
        /* mov rax, [rip + snx_arm_poll_offset]; cmp dword [rbx + rax], 0; jne ret: a thread of higher priority waits */
        sgx_bytes(b, "48 8B 05");
        add_rel(c, b->n, "snx_arm_poll_offset", R_X86_64_PC32, -4);
        sgx_imm(b, 0, 4);
        sgx_bytes(b, "83 3C 03 00");
        sgx_a32_jcc(g, b, 5, g->ret);
    }
    sgx_bytes(b, "48 89 DF"); /* mov rdi, rbx */
    epilogue(c);
    name_of(sym, sizeof sym, key);
    sgx_byte(b, 0xE9);
    add_rel(c, b->n, sym, R_X86_64_PLT32, -4);
    sgx_imm(b, 0, 4);
}

/* the code's word i (little-endian) */
static SnxU32 word_at(const unsigned char* code, size_t i) {
    const unsigned char* p = code + 4 * i;
    return (SnxU32)p[0] | (SnxU32)p[1] << 8 | (SnxU32)p[2] << 16 | (SnxU32)p[3] << 24;
}

/* one block into c->b: its size, 0 where refused (t_why says why) */
static size_t block(SnxX64Arm* c, const unsigned char* code, size_t count, SnxU32 pc, size_t* consumed,
                    SnxX64ArmNext succ, void* user) {
    SgCtx* g = &c->g;
    SgX* x = &g->x;
    SgXBuf* b = &c->b;
    jmp_buf jb;
    jmp_buf* saved = g_fail;
    volatile size_t i = 0;
    size_t size = 0;
    int bad_pc, bad_state, last_stop = 0;
    SnxU32 nexts[2];
    size_t nnext = 0;
    t_why = NULL;
    g_fail = &jb;
    if (setjmp(jb)) {
        g_fail = saved;
        return 0;
    }
    b->n = 0;
    b->disp = 0;
    b->index = -1;
    c->nrel = 0;
    /* the assembler's state of a function (its buffers kept) */
    c->a.nframe = 0;
    c->a.slots_max = 0;
    c->a.cold.n = 0;
    c->a.ncjumps = c->a.nccalls = c->a.npool = c->a.nrips = 0;
    c->a.b = b;
    c->a.machine = g;
    c->a.reloc = unit_reloc;
    g->nlabels = g->nfix = g->nexits = g->nmonitors = 0;
    x->n = 0;
    x->nv = 1;
    x->nb = 1;
    x->features = c->features;
    x->flags_ins = -1;
    x->base = 3; /* RBX */
    x->pool = c->pool;
    x->unmade = 0;
    g->pending = 0;
    g->origin = -1;
    g->skip = -1;
    g->when = 0;
    g->group = 0;
    g->deferred = 0;
    g->ret = sgx_a32_label(g);
    flush(c);
    prologue(c, pc, word_at(code, 0), &bad_pc, &bad_state);
    for (i = 0; i < count; ++i) {
        SnxU32 w = word_at(code, i);
        int stop;
        if (x->n > SGX_MAXI - SGX_MAXI / 4 || x->nv > SGX_MAXV - SGX_MAXV / 4 || x->nb > SGX_MAXB - SGX_MAXB / 4)
            flush(c);
        g->word = w;
        g->pc = pc + 4 * (SnxU32)i;
        g->next = (SnxU32)g->pc + 4;
        g->start = x->n;
        g->branched = g->undefined = 0;
        g->iset = 0;
        g->skip = -1;
        g->when = 0;
        if (sg_decode_a32(g)) /* the Sail's: its condition and its end its own */
            stop = g->branched || (g->undefined && g->skip < 0);
        else
            stop = instruction(g, snx_a32_decode(w));
        if (x->unmade) sg_fail("a primitive this host has no form of", 0, 0);
        sgx_a32_finish(g);
        last_stop = stop;
        if (stop || i + 1 == count) break;
        if (g->skip >= 0) {
            flush(c);
            sgx_a32_bind(g, b, g->skip);
        }
    }
    *consumed = i + 1;
    if (succ) nnext = succ(user, (SnxU32)g->pc, g->word, nexts);
    /* the end: the last instruction's way (a branch's target, the backend's exit, or on), then, past its condition's
       skip, the next instruction's */
    if (g->branched) {
        SgD t = g->target;
        int tconst = sgx_is_const(&t), thumb = 0;
        if (g->pending >> SG_PSTATE_T & 1) {
            SgD f = g->field[SG_PSTATE_T];
            if (sgx_is_const(&f))
                thumb = (int)(f.k & 1);
            else
                tconst = 0;
        }
        sgx_a32_pc(g, t);
        flush(c);
        if (tconst)
            go_on(c, (SnxU32)t.k | (SnxU32)thumb, pc, nexts, nnext);
        else
            sgx_a32_jmp(g, b, g->ret);
    } else if (!g->undefined) {
        /* on, both ways: R15 the next instruction's past the skip */
        flush(c);
        if (g->skip >= 0) sgx_a32_bind(g, b, g->skip), g->skip = -1;
        sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, g->next);
        go_on(c, g->next, pc, nexts, nnext);
    } else
        flush(c); /* its own code left */
    if (g->skip >= 0) {
        sgx_a32_bind(g, b, g->skip);
        sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, g->next);
        go_on(c, g->next, pc, nexts, nnext);
    }
    (void)last_stop;
    sgx_a32_stubs(g, b);
    prologue_exits(c, pc, word_at(code, 0), bad_pc, bad_state);
    sgx_a32_bind(g, b, g->ret);
    epilogue(c);
    sgx_byte(b, 0xC3); /* ret */
    sgx_cold_emit(&c->a);
    sgx_pool_emit(&c->a);
    sgx_patch_frame(&c->a);
    if (sgx_a32_patch(g, b)) sg_fail("a jump to a label never bound", 0, 0);
    size = b->n;
    g_fail = saved;
    return size;
}

int snx_x64_arm_block(SnxX64Arm* c, const char* name, const unsigned char* code, size_t count, SnxU32 pc,
                      size_t* consumed, SnxX64ArmNext next, void* user) {
    size_t size, at;
    int k;
    char own[24];
    *consumed = 0;
    c->why = NULL;
    if (!count || count > 65536 || (pc & 3)) {
        c->why = "a block of no words, too many, or at a misaligned address";
        return 0;
    }
    for (;;) {
        size = block(c, code, count, pc, consumed, next, user);
        if (!size) {
            c->why = t_why ? t_why : "unknown";
            *consumed = 0;
            return 0;
        }
        if (size <= c->b.cap) break;
        /* past the buffer: grown, and made again */
        c->b.cap = size * 2;
        c->b.p = (unsigned char*)realloc(c->b.p, c->b.cap);
        if (!c->b.p) {
            c->b.cap = 0;
            c->why = "out of memory";
            return 0;
        }
    }
    if (!name) name_of(own, sizeof own, pc), name = own;
    at = snx_xelf_put(c->elf, SNX_ELF_TEXT, c->b.p, size, 16);
    snx_xelf_symbol(c->elf, name, SNX_ELF_TEXT, at, size, SNX_ELF_GLOBAL | SNX_ELF_HIDDEN | SNX_ELF_FUNC);
    for (k = 0; k < c->nrel; ++k)
        snx_xelf_reloc(c->elf, SNX_ELF_TEXT, at + c->rel[k].at, c->rel[k].sym, c->rel[k].type, c->rel[k].addend);
    ++c->blocks;
    return 1;
}

int snx_x64_arm_object(SnxX64Arm* c, unsigned char** object, size_t* size, size_t* blocks) {
    int rc = snx_xelf_write(c->elf, object, size);
    if (blocks) *blocks = c->blocks;
    snx_xelf_free(c->elf);
    c->elf = snx_xelf_new();
    c->blocks = 0;
    return rc || !c->elf ? -1 : 0;
}

#else
int snx_x64_arm_available(void) { return 0; }
SnxX64Arm* snx_x64_arm_new(void) { return NULL; }
void snx_x64_arm_free(SnxX64Arm* c) { (void)c; }
int snx_x64_arm_block(SnxX64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                      size_t* consumed, SnxX64ArmNext next, void* user) {
    (void)c, (void)name, (void)code, (void)count, (void)pc, (void)next, (void)user;
    *consumed = 0;
    return 0;
}
const char* snx_x64_arm_why(const SnxX64Arm* c) {
    (void)c;
    return "no x86-64 back end in this build";
}
int snx_x64_arm_object(SnxX64Arm* c, unsigned char** object, size_t* size, size_t* blocks) {
    (void)c;
    *object = NULL;
    *size = 0;
    if (blocks) *blocks = 0;
    return -1;
}
void snx_x64_arm_limit(SnxX64Arm* c, int pool, unsigned features) { (void)c, (void)pool, (void)features; }
#endif
