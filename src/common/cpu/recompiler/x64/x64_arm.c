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
#include "common/cpu/arm/simd.h"
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

/* the thread's: where a word the emitters can't make (bits.h's sg_fail) refuses the block, which LLVM makes, and why,
   for the caller */
static _Thread_local struct {
    jmp_buf* fail;
    const char* why;
} t_x64_arm;
#define SG_FAIL_JUMP             t_x64_arm.fail
#define SG_FAIL_NOTE(what, a, b) (t_x64_arm.why = (what), (void)(a), (void)(b))

#include "a32_x64.inc"

/* a relocation of the block being made, at its code's offset */
typedef struct {
    size_t at;
    char sym[24];
    unsigned type;
    long long addend;
} Rel;

/* a block made, kept until the object: its name and address, its code in text, its relocations in rels */
typedef struct {
    SnxU32 pc;
    char name[24];
    size_t at, size, rel0, nrel;
    size_t own; /* its entry past the checks (X and its name), from its start; (size_t)-1 for none (entry's) */
} Made;

struct SnxX64Arm {
    SgCtx g;
    SgXAsm a;
    SgXBuf b;
    Rel* rel; /* the block being made's */
    int nrel, cap_rel;
    /* the blocks made since the last object, which it writes in their addresses' order */
    unsigned char* text;
    size_t ntext, cap_text;
    Rel* rels;
    size_t nrels, cap_rels;
    Made* made;
    size_t nmade, cap_made;
    unsigned features;
    int pool;
    const char* why;
    int big;    /* the block being made has 32-bit frame sizes (frame) */
    size_t own; /* its entry past the checks, from its start (prologue) */
};

int snx_x64_arm_available(void) { return snx_x64_available(); }

SnxX64Arm* snx_x64_arm_new(void) {
    if (!snx_x64_available()) return NULL;
    SnxX64Arm* c = (SnxX64Arm*)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->b.cap = 1u << 16;
    c->b.p = (unsigned char*)malloc(c->b.cap);
    if (!c->b.p) {
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
    free(c->text), free(c->rels), free(c->made);
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
    sg_flags_flush(g);
    if (g->x.n) sgx_assemble_region(&c->a, &g->x);
    for (int k = 0; k < 15; ++k) g->gpr[k] = 0;
    g->cpsr = 0;
    g->limit = g->mem = 0;
    g->origin = -1;
    g->vfp_ok[0] = g->vfp_ok[1] = 0;
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
    unsigned n = (w >> 16) & 15, d = (w >> 12) & 15, op = (w >> 5) & 3, bytes = 4;
    int pre = !!(w & (1u << 24)), up = !!(w & (1u << 23));
    int wb = !pre || !!(w & (1u << 21)), load = !!(w & (1u << 20)), sign = 0, pair = 0;
    condition(g);
    SgD base = reg(g, n), offset;
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
    SgD adjusted = up ? sg_d_add(g, base, offset) : sg_d_sub(g, base, offset), addr = pre ? adjusted : base;
    /* LDRD and STRD, and a load of the PC, word aligned (llvm.c's: unaligned support on, as ARMv6's SCTLR.U) */
    sgx_a32_bounds(g, addr, pair ? 8 : bytes, pair || (load && d == 15) ? 4 : 1);
    if (load) {
        SgD value = sgx_a32_load(g, addr, (int)bytes, 0);
        if (sign)
            value = sg_d_sext(g, value, 32);
        else if (bytes < 4)
            value = sg_d_zext(g, value, 32);
        if (d == 15)
            write_pc(g, value);
        else
            sg_GPR_write(g, d, value);
        if (pair) sg_GPR_write(g, d + 1, sgx_a32_load(g, addr, 4, 4)); /* the range checked holds both: no wrap */
    } else {
        SgD value = reg(g, d); /* the Cortex-A57's PCStoreValue: the address plus 8 */
        sgx_a32_store(g, addr, (int)bytes, 0, bytes < 4 ? sg_d_slice(g, value, 8 * bytes - 1, 0) : value);
        if (pair) sgx_a32_store(g, addr, 4, 4, reg(g, d + 1));
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
    unsigned n = (w >> 16) & 15, count = 0, index = 0;
    int pre = !!(w & (1u << 24)), up = !!(w & (1u << 23)), load = !!(w & (1u << 20));
    SgD values[16];
    condition(g);
    for (unsigned i = 0; i < 16; ++i) count += (list >> i) & 1;
    SgD base = reg(g, n);
    SgD adjusted = up ? sg_d_add(g, base, sgx_const(32, count * 4)) : sg_d_sub(g, base, sgx_const(32, count * 4));
    SgD start = up ? sg_d_add(g, base, sgx_const(32, pre ? 4 : 0)) : sg_d_add(g, adjusted, sgx_const(32, pre ? 0 : 4));
    sgx_reg(sgx(g), &start);
    sgx_a32_bounds(g, start, count * 4, 4);
    /* every source read before any update, the base and the PC among them */
    for (unsigned i = 0; i < 16; ++i)
        if (list >> i & 1) {
            values[i] = load ? sgx_a32_load(g, start, 4, (int)(index * 4)) : reg(g, i);
            ++index;
        }
    if (load && (list >> 15 & 1)) write_pc(g, values[15]);
    index = 0;
    for (unsigned i = 0; i < 16; ++i)
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
    condition(g);
    SgD a = reg(g, w & 15), b = reg(g, (w >> 8) & 15);
    if (kind == SNX_A32_MULL) {
        int umaal = (w & 0x00e00000) == 0x00400000, sign = !umaal && (w & (1u << 22));
        a = sign ? sg_d_sext(g, a, 64) : sg_d_zext(g, a, 64);
        b = sign ? sg_d_sext(g, b, 64) : sg_d_zext(g, b, 64);
        SgD r = SGX_OP(x, mul, 64, a, b);
        if (umaal) /* Rn * Rm + RdHi + RdLo, unsigned: never past 64 bits */
            r = sg_d_add(g, r, sg_d_add(g, sg_d_zext(g, reg(g, d), 64), sg_d_zext(g, reg(g, n), 64)));
        else if (w & (1u << 21))
            r = sg_d_add(g, r, sg_d_concat(g, reg(g, n), reg(g, d)));
        sg_GPR_write(g, d, sg_d_slice(g, r, 31, 0));
        sg_GPR_write(g, n, sg_d_slice(g, r, 63, 32));
        if (!umaal && (w & (1u << 20))) nz(g, r, 64);
        return;
    }
    SgD r = SGX_OP(x, mul, 32, a, b);
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
    condition(g);
    /* 64 bits: the most negative over -1 needs no care there, and its quotient's low word is the Arm ARM's */
    SgD n = u ? sg_d_zext(g, reg(g, w & 15), 64) : sg_d_sext(g, reg(g, w & 15), 64);
    SgD m = u ? sg_d_zext(g, reg(g, (w >> 8) & 15), 64) : sg_d_sext(g, reg(g, (w >> 8) & 15), 64);
    SgB zero = sg_d_is_zero(g, m);
    m = sg_d_select(g, zero, sgx_const(64, 1), m);
    SgD q = u ? SGX_OP(x, udiv, 64, n, m) : SGX_OP(x, sdiv, 64, n, sg_d_asr(g, n, 63), m);
    sg_GPR_write(g, (w >> 16) & 15, sg_d_select(g, zero, sgx_const(32, 0), sg_d_slice(g, q, 31, 0)));
}

/* CLZ, REV, REV16, REVSH, RBIT, the extends, the bitfields, MRS and MSR (llvm.c's miscellaneous) */
static int miscellaneous(SgCtx* g, SnxA32Kind kind) {
    SgX* x = sgx(g);
    SnxU32 w = g->word;
    unsigned d = (w >> 12) & 15, n = (w >> 16) & 15;
    condition(g);
    SgD v = reg(g, w & 15), r;
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
            unsigned lsb = (w >> 7) & 31;
            if ((w & 0x0fe00070) == 0x07c00010) { /* BFI, BFC */
                SnxU32 mask = UINT32_MAX >> (32 - (((w >> 16) & 31) - lsb + 1));
                r = (w & 15) == 15 ? sgx_const(32, 0) : sg_d_lsl(g, sg_d_and(g, v, sgx_const(32, mask)), lsb);
                r = sg_d_or(g, sg_d_and(g, reg(g, d), sgx_const(32, ~(mask << lsb))), r);
            } else { /* SBFX, UBFX */
                r = sg_d_slice(g, v, lsb + ((w >> 16) & 31), lsb);
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
            if (w & (1u << 25)) {
                unsigned rot = ((w >> 8) & 15) * 2;
                SnxU32 imm = w & 255;
                if (rot) imm = (imm >> rot) | (imm << (32 - rot));
                v = sgx_const(32, imm);
            }
            sg_flags_flush(g);
            SgD c = sg_d_or(g, sg_d_and(g, sgx_a32_cpsr(g), sgx_const(32, ~mask)), sg_d_and(g, v, sgx_const(32, mask)));
            g->cpsr = sgx_a32_put(g, offsetof(SnxArmCPU, cpsr), 32, c);
            g->origin = -1;
            /* a block reads E only on entry: one that may have changed it ends here, the next one's entry checked */
            g->e_written = !!(w & (1u << 17));
            return g->e_written;
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
    condition(g);
    SgD addr = reg(g, n);
    sgx_reg(x, &addr);
    sgx_a32_bounds(g, addr, bytes, bytes);
    if (op & 1) {
        SgD value = sg_d_zext(g, sgx_a32_load(g, addr, (int)bytes, 0), 64);
        sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_value), 64, value);
        sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_address), 32, addr);
        sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_size), 32, sgx_const(32, bytes));
        sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_valid), 32, sgx_const(32, 1));
        sg_GPR_write(g, d, sg_d_slice(g, value, 31, 0));
        if (bytes == 8) sg_GPR_write(g, d + 1, sg_d_slice(g, value, 63, 32));
    } else {
        static const signed char fix[3] = {6, 2, 10}; /* RSI, RDX, R10 */
        SgD value = reg(g, t);
        if (bytes == 8) value = sg_d_concat(g, reg(g, t + 1), value);
        SgD in[3];
        int regs[3];
        in[0] = sgx_zext(x, addr, 64);
        in[1] = value;
        in[2] = sgx_val(64, sgx_a32_mem(g));
        for (int k = 0; k < 3; ++k) regs[k] = sgx_reg(x, &in[k]);
        SgXIns* i = sgx_a32_machine(g, SGX_A32_STREX, 1);
        for (int k = 0; k < 3; ++k) i->in[k] = regs[k], i->fixed[k] = fix[k];
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
    condition(g);
    SgD addr = reg(g, (w >> 16) & 15);
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

/* ---- VFP: llvm_vector.inc's scalar part (vector_memory, vector_transfer, vector_fp), its hot paths fp_call's ---- */

/* the D bank's byte offset off (S2n at Dn's, S2n+1 4 past it: the host's little-endian words) as a state field */
static int voff(unsigned off) { return (int)(offsetof(SnxArmCPU, d) + off); }
/* FPSCR, as a field the D bank's offsets reach */
#define FPSCR_OFF ((unsigned)(offsetof(SnxArmCPU, fpscr) - offsetof(SnxArmCPU, d)))
/* a VFP register's bits (32 or 64) in a general-purpose register */
static SgD vbits(SgCtx* g, unsigned off, unsigned width) {
    SgX* x = sgx(g);
    SgXIns* i = sgx_append(x, SGX_K_LOAD);
    i->w = (unsigned char)(width == 64 ? 64 : 32);
    i->disp = voff(off);
    i->def = sgx_vreg(x, x->n - 1, width == 64 ? 64 : 32);
    return sgx_val(width, i->def);
}
/* the VFP register = v (bits of 32 or 64, or an XMM value of them) */
static void vset(SgCtx* g, unsigned off, unsigned width, SgD v) {
    SgX* x = sgx(g);
    if (v.x) {
        int in[4] = {0, v.x, 0, 0};
        sgx_vins(x, width == 64 ? &sgx_v_movq_st : &sgx_v_movss_st, 0, 0, in, 0, 0, SGX_VM_STATE, 0, 0, voff(off), 0,
                 0);
        return;
    }
    {
        int r = sgx_reg(x, &v);
        SgXIns* i = sgx_append(x, SGX_K_STORE);
        i->w = (unsigned char)(width == 64 ? 64 : 32);
        i->in[0] = r;
        i->disp = voff(off);
    }
}
/* a VFP register as an XMM value (MOVSS or MOVQ from the state) */
static SgD vxmm(SgCtx* g, unsigned off, unsigned width) {
    return sgx_xval(width, sgx_vins(sgx(g), width == 64 ? &sgx_v_movq_ld : &sgx_v_movss_ld, 0, 0, NULL, 0, 1,
                                    SGX_VM_STATE, 0, 0, voff(off), 0, 0));
}
/* FPSCR lets the hot path run: round to nearest and no trap enabled, and for arithmetic (not a compare) IXC set already
   (the inline code raises no flag): fp_call's mask. Once a region, until a VMSR or a call */
static SgB fp_state(SgCtx* g, int arithmetic) {
    SgX* x = sgx(g);
    if (!g->vfp_ok[arithmetic]) {
        SnxU32 mask = 0x00c09f00u | (arithmetic ? 0x10u : 0);
        SgD f = sg_d_and(g, vbits(g, FPSCR_OFF, 32), sgx_const(32, mask));
        g->vfp_ok[arithmetic] = sg_d_eq(g, f, sgx_const(32, arithmetic ? 0x10u : 0));
    }
    (void)x;
    return g->vfp_ok[arithmetic];
}
/* fp_plain of bits (w 32 or 64): finite, and normal, or a zero where zero_ok (0: always). The bits doubled (the sign
   out) less the smallest normal's, below the span of the finite exponents, unsigned; or 0 */
static SgB fp_plain(SgCtx* g, SgD bits, unsigned w, SgB zero_ok) {
    SnxU64 low = w == 32 ? 1ull << 24 : 1ull << 53, span = w == 32 ? 0xFE000000ull : 0xFFC0000000000000ull;
    SgD t = sg_d_lsl(g, bits, 1);
    SgB normal = sgx_d_ult(g, sg_d_sub(g, t, sgx_const(w, low)), sgx_const(w, span));
    SgB zero = sg_d_is_zero(g, t);
    if (zero_ok) zero = sg_b_and(g, zero, zero_ok);
    return sg_b_or(g, normal, zero);
}
/* bits are a zero (either sign) */
static SgB fp_zero(SgCtx* g, SgD bits) { return sg_d_is_zero(g, sg_d_lsl(g, bits, 1)); }
/* snx_arm_fp's arguments after the operands, for snx_arm_fp_x64 */
static long long fp_desc(SnxArmVectorOp op, unsigned width, SnxU32 param) {
    return (long long)((SnxU64)op | (SnxU64)width << 8 | (SnxU64)param << 32);
}
/* fp_call's helper, always: the state's registers and CPSR read again after it */
static SgD fp_helper(SgCtx* g, SnxArmVectorOp op, unsigned width, SnxU32 param, SgD a, SgD b, SgD c) {
    SgX* x = sgx(g);
    SgD args[4];
    sgx_flat(x, &a);
    sgx_flat(x, &b);
    sgx_flat(x, &c);
    args[0] = sg_d_zext(g, a, 64);
    args[1] = sg_d_zext(g, b, 64);
    args[2] = sg_d_zext(g, c, 64);
    args[3] = sgx_const(64, (SnxU64)fp_desc(op, width, param));
    sgx_a32_call(g, "snx_arm_fp_x64", args, 4, 1);
    return sgx_val(64, x->ins[x->n - 1].def);
}
/* an x86 form of a and b (XMM values of width w) */
static SgD fp_op(SgCtx* g, const SgXVForm* f32, const SgXVForm* f64, SgD a, SgD b, unsigned w) {
    SgX* x = sgx(g);
    int ax = sgx_xreg(x, &a), bx = sgx_xreg(x, &b);
    return sgx_xval(w, sgx_v2(x, w == 64 ? f64 : f32, ax, bx, 0));
}
/* v with its sign flipped (an XMM value): XORPS of the sign's bit */
static SgD fp_neg(SgCtx* g, SgD v, unsigned w) {
    SgX* x = sgx(g);
    int vx = sgx_xreg(x, &v), k = sgx_xconst(x, w == 64 ? 1ull << 63 : 1ull << 31, 0);
    return sgx_xval(w, sgx_v2(x, &sgx_v_xorps, vx, k, 0));
}

/* fp_call for the arithmetic and the compares: the host's IEEE operation where fp_call's guards say it gives the
   console's result and flags (FPSCR's mode and IXC; operands finite and normal or zero; a result normal, or a zero that
   is exact), snx_arm_fp on a cold path where they don't; the other operations snx_arm_fp alone */
static SgD fp_call(SgCtx* g, SnxArmVectorOp op, unsigned width, SnxU32 param, SgD xv, SgD yv, SgD zv) {
    SgX* x = sgx(g);
    int compare = op == AV_FCMP || op == AV_FCMPE;
    int arithmetic = op == AV_FADD || op == AV_FSUB || op == AV_FMUL || op == AV_FDIV || op == AV_FMLA ||
                     op == AV_FMLS || op == AV_FNMLA || op == AV_FNMLS || op == AV_FNMUL;
    if ((width != 32 && width != 64) || (!compare && !arithmetic)) return fp_helper(g, op, width, param, xv, yv, zv);
    SgD xb = xv, yb = yv, zb = zv, r, args[3];
    sgx_flat(x, &xb);
    sgx_flat(x, &yb);
    sgx_flat(x, &zb);
    SgB ok = sg_b_and(g, fp_state(g, !compare), sg_b_and(g, fp_plain(g, xb, width, 0), fp_plain(g, yb, width, 0)));
    if (compare) {
        /* NZCV: less 8, equal 6, greater 2: UCOMISS's below, equal and not below (no NaN reaches it) */
        int in[4] = {0, 0, 0, 0};
        in[0] = sgx_xreg(x, &xv);
        in[1] = sgx_xreg(x, &yv);
        sgx_vins(x, width == 64 ? &sgx_v_ucomisd : &sgx_v_ucomiss, -1, 0, in, 0, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
        int ins = x->flags_ins;
        SgD n = sgx_bool_bits(x, sgx_bool(x, SGX_B_CC, ins, 2)), z = sgx_bool_bits(x, sgx_bool(x, SGX_B_CC, ins, 4));
        SgD c = sgx_bool_bits(x, sgx_bool(x, SGX_B_CC, ins, 3));
        r = sg_d_zext(g, sg_d_concat(g, sg_d_concat(g, n, z), sg_d_concat(g, c, sgx_const(1, 0))), 64);
    } else {
        SgB xz = fp_zero(g, xb), yz = fp_zero(g, yb);
        switch (op) {
            case AV_FADD: r = fp_op(g, &sgx_v_addss, &sgx_v_addsd, xv, yv, width); break;
            case AV_FSUB: r = fp_op(g, &sgx_v_subss, &sgx_v_subsd, xv, yv, width); break;
            case AV_FDIV:
                ok = sg_b_and(g, ok, sg_b_not(g, yz));
                r = fp_op(g, &sgx_v_divss, &sgx_v_divsd, xv, yv, width);
                break;
            default: r = fp_op(g, &sgx_v_mulss, &sgx_v_mulsd, xv, yv, width); break;
        }
        SgD rb = r;
        sgx_flat(x, &rb);
        SgB zero_ok = op == AV_FADD || op == AV_FSUB ? 0 : op == AV_FDIV ? xz : sg_b_or(g, xz, yz);
        ok = sg_b_and(g, ok, fp_plain(g, rb, width, zero_ok));
        if (op != AV_FADD && op != AV_FSUB && op != AV_FMUL && op != AV_FDIV) {
            /* the multiply-accumulates: the product, negated for FMLS, FNMLA and FNMUL, then added (two roundings) */
            if (op == AV_FMLS || op == AV_FNMLA || op == AV_FNMUL) r = fp_neg(g, r, width);
            if (op != AV_FNMUL) {
                SgD addend = zv;
                ok = sg_b_and(g, ok, fp_plain(g, zb, width, 0));
                if (op == AV_FNMLA || op == AV_FNMLS) addend = fp_neg(g, addend, width);
                r = fp_op(g, &sgx_v_addss, &sgx_v_addsd, addend, r, width);
                SgD sb = r;
                sgx_flat(x, &sb);
                ok = sg_b_and(g, ok, fp_plain(g, sb, width, 0));
            }
        }
    }
    args[0] = sg_d_zext(g, xb, 64);
    args[1] = sg_d_zext(g, yb, 64);
    args[2] = sg_d_zext(g, zb, 64);
    return sgx_cold_fp(x, sg_b_not(g, ok), r, "snx_arm_fp_x64", args, 3, -1, 3, fp_desc(op, width, param),
                       (int)offsetof(SnxArmCPU, fpscr), (int)offsetof(SnxArmCPU, fpscr), 0, 0);
}

/* VLDR, VSTR, VLDM, VSTM, VPUSH, VPOP (vector_memory): word aligned, the words in their order (a doubleword's two at
   once) */
static void vfp_memory(SgCtx* g, const SnxArmVector* v) {
    SgD base = reg(g, v->n), offset = sgx_const(32, (SnxU32)v->imm);
    int single = (v->flags & AV_PRE) && !(v->flags & AV_WB);
    SgD end = v->flags & AV_UP ? sg_d_add(g, base, offset) : sg_d_sub(g, base, offset);
    SgD addr = single || (v->flags & AV_PRE) ? end : base;
    sgx_reg(sgx(g), &addr);
    sgx_a32_bounds(g, addr, v->bytes, 4);
    for (unsigned i = 0, step; i < v->bytes; i += step) {
        step = !((v->d + i) & 7) && i + 8 <= v->bytes ? 8 : 4;
        if (v->flags & AV_LOAD)
            vset(g, v->d + i, step * 8, sgx_a32_load(g, addr, (int)step, (int)i));
        else
            sgx_a32_store(g, addr, (int)step, (int)i, vbits(g, v->d + i, step * 8));
    }
    if (!(v->flags & AV_LOAD)) sgx_a32_monitor(g, addr, v->bytes); /* one check for the words stored, as memory's */
    if (v->flags & AV_WB) sg_GPR_write(g, v->n, end);
}
/* VMRS, VMSR, VMOV between core registers and S registers or a D register's word (vector_transfer) */
static void vfp_transfer(SgCtx* g, const SnxArmVector* v) {
    if (v->op == AV_STATUS) {
        if (v->flags & AV_LOAD) {
            SgD f = vbits(g, FPSCR_OFF, 32);
            if (v->n == 15) { /* VMRS APSR_nzcv: CPSR's flags from FPSCR's */
                for (int k = 0; k < 4; ++k) sg_PSTATE_write(g, SG_PSTATE_N + k, 1, sg_d_slice(g, f, 31 - k, 31 - k));
            } else
                sg_GPR_write(g, v->n, f);
        } else {
            vset(g, FPSCR_OFF, 32, sg_d_and(g, reg(g, v->n), sgx_const(32, 0xffc0009fu)));
            g->vfp_ok[0] = g->vfp_ok[1] = 0;
        }
    } else if (v->op == AV_CORE_PAIR) {
        if (v->flags & AV_LOAD) {
            SgD a = vbits(g, v->d, 32), b = vbits(g, v->d + 4, 32);
            sg_GPR_write(g, v->n, a);
            sg_GPR_write(g, v->m, b);
        } else {
            SgD a = reg(g, v->n), b = reg(g, v->m);
            vset(g, v->d, 32, a);
            vset(g, v->d + 4, 32, b);
        }
    } else {
        if (v->width != 32) sg_fail("a VMOV of a D register's byte or halfword", v->width, 0);
        if (v->flags & AV_LOAD)
            sg_GPR_write(g, v->n, vbits(g, v->d, 32));
        else if (v->op == AV_DUP) {
            SgD r = reg(g, v->n);
            for (unsigned i = 0; i < v->bytes; i += 4) vset(g, v->d + i, 32, r);
        } else
            vset(g, v->d, 32, reg(g, v->n));
    }
}
/* the scalar data processing (vector_fp's, one lane) */
static void vfp_data(SgCtx* g, const SnxArmVector* v) {
    unsigned width = v->width, outwidth = width;
    SnxU32 param = 0;
    int unary = v->op == AV_MOV || (v->op >= AV_FABS && v->op <= AV_FSQRT) || v->op == AV_FCVT || v->op == AV_FTOI ||
                v->op == AV_ITOF || v->op == AV_FROUND || v->op == AV_FRECPE || v->op == AV_FRSQRTE ||
                v->op == AV_URECPE || v->op == AV_URSQRTE;
    /* the operations fp_call has a hot path for, whose operands are XMM values; the others' are bits */
    int hot = v->op == AV_FADD || v->op == AV_FSUB || v->op == AV_FMUL || v->op == AV_FDIV || v->op == AV_FMLA ||
              v->op == AV_FMLS || v->op == AV_FNMLA || v->op == AV_FNMLS || v->op == AV_FNMUL || v->op == AV_FCMP ||
              v->op == AV_FCMPE;
    if (v->op == AV_FCVT)
        outwidth = v->aux;
    else if (v->op == AV_FTOI)
        outwidth = v->bytes * 8;
    if (width == 16 || outwidth == 16) sg_fail("a half-precision conversion", width, outwidth);
    if (v->flags & AV_UNSIGNED) param |= 1u << 28;
    if (v->op == AV_FCVT) param |= v->aux | ((SnxU32)v->imm << 24);
    if (v->op == AV_FTOI || v->op == AV_ITOF)
        param |= v->aux << 8 | (((SnxU32)v->imm >> 8) & 255) << 16 | ((SnxU32)v->imm & 15) << 24;
    if (v->op == AV_FROUND) param |= (SnxU32)v->imm << 24 | v->aux;
    unsigned inwidth = v->op == AV_ITOF ? v->aux : width, xo = unary ? v->m : v->n;
    SgD xv = inwidth == 16 ? sg_d_slice(g, vbits(g, xo, 32), 15, 0)
             : hot         ? vxmm(g, xo, inwidth)
                           : vbits(g, xo, inwidth);
    SgD yv = (v->op == AV_FCMP || v->op == AV_FCMPE) && v->aux ? sgx_const(width, 0)
             : hot                                             ? vxmm(g, v->m, width)
                                                               : vbits(g, v->m, width);
    /* the destination, which the multiply-accumulates add to, and which fp_call hands the helper for every operation
       (the fused ones read it there) */
    SgD zv = v->op >= AV_FMLA && v->op <= AV_FNMLS ? vxmm(g, v->d, outwidth)
             : hot                                 ? sgx_const(outwidth, 0)
                                                   : vbits(g, v->d, outwidth);
    SgD r;
    switch (v->op) {
        case AV_MOV: r = xv; break;
        case AV_FABS: r = sg_d_and(g, xv, sgx_const(width, sg_ones(width - 1))); break;
        case AV_FNEG: r = sg_d_xor(g, xv, sgx_const(width, 1ull << (width - 1))); break;
        case AV_FSELECT: {
            static const unsigned conds[4] = {0, 6, 10, 12}; /* VSELEQ, VSELVS, VSELGE, VSELGT */
            r = sg_d_select(g, sgx_a32_condition(g, conds[v->aux & 3]), xv, yv);
            break;
        }
        default: r = fp_call(g, v->op, width, param, xv, yv, zv); break;
    }
    if (v->op == AV_FCMP || v->op == AV_FCMPE) {
        SgD f = vbits(g, FPSCR_OFF, 32);
        vset(g, FPSCR_OFF, 32,
             sg_d_or(g, sg_d_and(g, f, sgx_const(32, 0x0fffffffu)), sg_d_lsl(g, sg_d_slice(g, r, 31, 0), 28)));
        return;
    }
    if (v->op == AV_FTOI) {
        r = sg_d_slice(g, r, v->aux - 1, 0);
        r = v->flags & AV_UNSIGNED ? sg_d_zext(g, r, outwidth) : sg_d_sext(g, r, outwidth);
    } else if (!r.x && r.w != outwidth)
        r = sg_d_slice(g, r, outwidth - 1, 0);
    vset(g, v->d, outwidth, r);
}
/* a VFP word (llvm_vector.inc's vector_instruction on the Cortex-A57): the scalar ones; Advanced SIMD is LLVM's */
static void vfp(SgCtx* g) {
    SnxArmVector v = {0};
    condition(g);
    if (!snx_arm_vector_decode(g->word, &v)) {
        sg_undefined(g);
        return;
    }
    if ((v.flags & AV_SIMD) || v.op == AV_STRUCT || v.op == AV_CRYPTO) sg_fail("an Advanced SIMD instruction", v.op, 0);
    if (v.op == AV_MEM)
        vfp_memory(g, &v);
    else if (v.op == AV_CORE || v.op == AV_CORE_PAIR || v.op == AV_STATUS || v.op == AV_DUP)
        vfp_transfer(g, &v);
    else if (v.op == AV_IMM) {
        if (v.aux) sg_fail("an Advanced SIMD immediate", v.aux, 0);
        for (unsigned i = 0; i < v.bytes; i += v.width / 8) vset(g, v.d + i, v.width, sgx_const(v.width, v.imm));
    } else if (v.op == AV_MOV || (v.op >= AV_FADD && v.op <= AV_FSELECT))
        vfp_data(g, &v);
    else
        sg_fail("an Advanced SIMD instruction", v.op, 0);
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
            sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_valid), 32, sgx_const(32, 0));
            return 0;
        case SNX_A32_BARRIER: /* stronger than each valid DMB and DSB scope, never just a compiler barrier */
            condition(g);
            sgx_a32_machine(g, SGX_A32_FENCE, 0);
            return 0;
        case SNX_A32_HINT: return 0; /* hints may complete without waiting */
        case SNX_A32_VECTOR: vfp(g); return 0;
        case SNX_A32_COPROC: { /* the backend's (SnxArmRt.coproc): it reads or writes the registers, or stops here */
            SgD word = sgx_const(32, w);
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
    snprintf(out, size, "%c_%08x", key & 1 ? 'T' : 'A', (unsigned)(key & ~1u));
}

/* the frame's room, patched once the block is done (sgx_patch_frame's sizes): SUB or ADD RSP of an 8-bit immediate,
   a four-byte NOP where the block has no slot. A block whose frame passes 127 bytes is made again with 32-bit ones */
static void frame(SnxX64Arm* c, int add) {
    SgXBuf* b = &c->b;
    if (c->a.nframe >= 256) sg_fail("a block of too many epilogues", c->a.nframe, 256);
    sgx_bytes(b, c->big ? (add ? "48 81 C4" : "48 81 EC") : add ? "48 83 C4" : "48 83 EC");
    c->a.frame_at[c->a.nframe++] = b->n;
    sgx_imm(b, 0, c->big ? 4 : 1);
}
/* the frame's size in each SUB and ADD (sgx_patch_frame's rule: the slots, 16-aligned): 0, or -1 where an 8-bit
   immediate doesn't hold it */
static int patch_frame(SnxX64Arm* c) {
    int k, size = (c->a.slots_max * 8 + 15) & ~15;
    if (c->big) {
        sgx_patch_frame(&c->a);
        return 0;
    }
    if (size > 127) return -1;
    for (k = 0; k < c->a.nframe; ++k) {
        size_t at = c->a.frame_at[k];
        if (at + 1 > c->b.cap) continue;
        if (size)
            c->b.p[at] = (unsigned char)size;
        else /* nop dword [rax + 0]: the same four bytes */
            memcpy(c->b.p + at - 3, "\x0F\x1F\x40\x00", 4);
    }
    return 0;
}
/* the entry (block_begin's checks: R15 the block's address, CPSR.T and E clear; then exit and syndrome cleared), on
   RDI, before the frame; where a check fails, a jump to bad (entry_exit). Then the entry the block's own jumps from
   another of the x86-64 back end's take (X and the block's name), past them: RBX the state, the frame */
static void prologue(SnxX64Arm* c, SnxU32 pc, int* bad) {
    SgXBuf* b = &c->b;
    SgCtx* g = &c->g;
    *bad = sgx_a32_label(g);
    sgx_bytes(b, "81 7F"), sgx_byte(b, (unsigned char)(offsetof(SnxArmCPU, r) + 60)), sgx_imm(b, pc, 4); /* cmp */
    sgx_a32_jcc(g, b, 5, *bad);
    sgx_bytes(b, "F7 47"), sgx_byte(b, (unsigned char)offsetof(SnxArmCPU, cpsr)), sgx_imm(b, SNX_ARM_T | SNX_ARM_E, 4);
    sgx_a32_jcc(g, b, 5, *bad);
    sgx_bytes(b, "48 C7 47"), sgx_byte(b, (unsigned char)offsetof(SnxArmCPU, exit)),
        sgx_imm(b, 0, 4); /* and syndrome */
    c->own = b->n;
    sgx_bytes(b, "53 48 89 FB"); /* push rbx; mov rbx, rdi */
    frame(c, 0);
}
/* where a check of the entry failed: ESI the block's address, EDX its first word, and snx_x64_arm_entry_exit (the
   object's own, which sets the exit) */
static void entry_exit(SnxX64Arm* c, SnxU32 pc, SnxU32 first, int bad) {
    SgXBuf* b = &c->b;
    sgx_a32_bind(&c->g, b, bad);
    sgx_byte(b, 0xBE), sgx_imm(b, pc, 4);    /* mov esi, pc */
    sgx_byte(b, 0xBA), sgx_imm(b, first, 4); /* mov edx, first */
    sgx_byte(b, 0xE9);
    add_rel(c, b->n, "snx_x64_arm_entry_exit", R_X86_64_PLT32, -4);
    sgx_imm(b, 0, 4);
}
/* the frame given back and RBX the caller's */
static void epilogue(SnxX64Arm* c) {
    frame(c, 1);
    sgx_bytes(&c->b, "5B"); /* pop rbx */
}
/* where the block ends with R15 key's address in key's state: a jump to the block's own entry past its checks (X and
   its name: the code there reads no R15, and its every exit stores it) where key is one of the successors, after the
   poll for one at or before the block's start; else R15 stored and the return. public: the block's checked entry
   instead, R15 stored (after an MSR that may have set E, which that entry checks) */
static void go_on(SnxX64Arm* c, SnxU32 key, SnxU32 block, const SnxU32* next, size_t nnext, int public) {
    SgXBuf* b = &c->b;
    SgCtx* g = &c->g;
    size_t k = 0;
    while (k < nnext && next[k] != key) ++k;
    if (k == nnext || public) sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, key & ~1u);
    if (k == nnext) {
        sgx_a32_jmp(g, b, g->ret);
        return;
    }
    if ((key & ~1u) <= block) {
        /* mov rax, [rip + snx_arm_poll_offset]; cmp dword [rbx + rax], 0; jne: a thread of higher priority waits, the
           return with R15 stored */
        int wait = sgx_a32_label(g), go = sgx_a32_label(g);
        sgx_bytes(b, "48 8B 05");
        add_rel(c, b->n, "snx_arm_poll_offset", R_X86_64_PC32, -4);
        sgx_imm(b, 0, 4);
        sgx_bytes(b, "83 3C 03 00");
        sgx_a32_jcc(g, b, 4, go);
        sgx_a32_bind(g, b, wait);
        if (!public) sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, key & ~1u);
        sgx_a32_jmp(g, b, g->ret);
        sgx_a32_bind(g, b, go);
    }
    sgx_bytes(b, "48 89 DF"); /* mov rdi, rbx */
    epilogue(c);
    char sym[24];
    name_of(sym + (public ? 0 : 1), sizeof sym - 1, key);
    if (!public) sym[0] = 'X';
    sgx_byte(b, 0xE9);
    add_rel(c, b->n, sym, R_X86_64_PLT32, -4);
    sgx_imm(b, 0, 4);
}

/* the code's word i (little-endian) */
static SnxU32 word_at(const unsigned char* code, size_t i) {
    const unsigned char* p = code + 4 * i;
    return (SnxU32)p[0] | (SnxU32)p[1] << 8 | (SnxU32)p[2] << 16 | (SnxU32)p[3] << 24;
}

/* one block into c->b: its size, 0 where refused (t_x64_arm.why says why) */
static size_t block(SnxX64Arm* c, const unsigned char* code, size_t count, SnxU32 pc, size_t* consumed,
                    SnxX64ArmNext succ, void* user) {
    SgCtx* g = &c->g;
    SgX* x = &g->x;
    SgXBuf* b = &c->b;
    jmp_buf jb;
    jmp_buf* saved = t_x64_arm.fail;
    volatile size_t i = 0;
    int bad = 0;
    t_x64_arm.why = NULL;
    t_x64_arm.fail = &jb;
    if (setjmp(jb)) {
        t_x64_arm.fail = saved;
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
    g->e_written = 0;
    prologue(c, pc, &bad);
    for (i = 0; i < count; ++i) {
        SnxU32 w = word_at(code, i);
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
        /* the Sail's clauses' (their condition and their end their own), else the emitters here */
        int stop = sg_decode_a32(g) ? g->branched || (g->undefined && g->skip < 0) : instruction(g, snx_a32_decode(w));
        if (x->unmade) sg_fail("a primitive this host has no form of", 0, 0);
        sgx_a32_finish(g);
        if (stop || i + 1 == count) break;
        if (g->skip >= 0) {
            flush(c);
            sgx_a32_bind(g, b, g->skip);
        }
    }
    *consumed = i + 1;
    SnxU32 nexts[2];
    size_t nnext = succ ? succ(user, (SnxU32)g->pc, g->word, nexts) : 0;
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
        if (!tconst) sgx_a32_pc(g, t); /* a known target's R15 go_on's */
        flush(c);
        if (tconst)
            go_on(c, (SnxU32)t.k | (SnxU32)thumb, pc, nexts, nnext, 0);
        else
            sgx_a32_jmp(g, b, g->ret);
    } else if (!g->undefined) {
        /* on, both ways past the skip */
        flush(c);
        if (g->skip >= 0) sgx_a32_bind(g, b, g->skip), g->skip = -1;
        go_on(c, g->next, pc, nexts, nnext, g->e_written);
    } else
        flush(c); /* its own code left */
    if (g->skip >= 0) {
        sgx_a32_bind(g, b, g->skip);
        go_on(c, g->next, pc, nexts, nnext, 0);
    }
    sgx_a32_stubs(g, b);
    entry_exit(c, pc, word_at(code, 0), bad);
    sgx_a32_bind(g, b, g->ret);
    epilogue(c);
    sgx_byte(b, 0xC3); /* ret */
    sgx_cold_emit(&c->a);
    sgx_pool_emit(&c->a);
    if (patch_frame(c)) {
        t_x64_arm.fail = saved;
        c->big = 1; /* made again with 32-bit frame sizes */
        return (size_t)-1;
    }
    if (sgx_a32_patch(g, b)) sg_fail("a jump to a label never bound", 0, 0);
    t_x64_arm.fail = saved;
    return b->n;
}

/* a block's code (size bytes), relocations and record kept for the object: 1, or 0 out of memory */
static int keep(SnxX64Arm* c, const unsigned char* code, size_t size, const Rel* rel, size_t nrel, SnxU32 pc,
                const char* name, size_t own) {
    if (c->ntext + size > c->cap_text) {
        size_t cap = c->cap_text ? c->cap_text * 2 : (size_t)1 << 20;
        while (cap < c->ntext + size) cap *= 2;
        unsigned char* p = (unsigned char*)realloc(c->text, cap);
        if (!p) return 0;
        c->text = p, c->cap_text = cap;
    }
    if (c->nrels + nrel > c->cap_rels) {
        size_t cap = c->cap_rels ? c->cap_rels * 2 : 4096;
        while (cap < c->nrels + nrel) cap *= 2;
        Rel* p = (Rel*)realloc(c->rels, cap * sizeof *p);
        if (!p) return 0;
        c->rels = p, c->cap_rels = cap;
    }
    if (c->nmade == c->cap_made) {
        size_t cap = c->cap_made ? c->cap_made * 2 : 1024;
        Made* p = (Made*)realloc(c->made, cap * sizeof *p);
        if (!p) return 0;
        c->made = p, c->cap_made = cap;
    }
    Made* m = &c->made[c->nmade++];
    m->pc = pc;
    snprintf(m->name, sizeof m->name, "%s", name);
    m->at = c->ntext;
    m->size = size;
    m->rel0 = c->nrels;
    m->nrel = nrel;
    m->own = own;
    memcpy(c->text + c->ntext, code, size);
    c->ntext += size;
    if (nrel) memcpy(c->rels + c->nrels, rel, nrel * sizeof *rel);
    c->nrels += nrel;
    return 1;
}

int snx_x64_arm_block(SnxX64Arm* c, const char* name, const unsigned char* code, size_t count, SnxU32 pc,
                      size_t* consumed, SnxX64ArmNext next, void* user) {
    *consumed = 0;
    c->why = NULL;
    if (!count || count > 65536 || (pc & 3)) {
        c->why = "a block of no words, too many, or at a misaligned address";
        return 0;
    }
    c->big = 0;
    size_t size;
    for (;;) {
        size = block(c, code, count, pc, consumed, next, user);
        if (size == (size_t)-1) continue; /* its frame passes an 8-bit immediate: made again with 32-bit ones */
        if (!size) {
            c->why = t_x64_arm.why ? t_x64_arm.why : "unknown";
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
    char own[24];
    if (!name) name_of(own, sizeof own, pc), name = own;
    if (strlen(name) >= sizeof c->made[0].name) {
        c->why = "a block's name too long";
        return 0;
    }
    /* kept: its code, its relocations, its record */
    if (!keep(c, c->b.p, size, c->rel, (size_t)c->nrel, pc, name, c->own)) {
        c->why = "out of memory";
        return 0;
    }
    return 1;
}

/* snx_x64_arm_entry_exit(state RDI, the block's address ESI, its first word EDX), each object's own, where a
   block's entry check failed (llvm.c's block_begin, in its order): R15 not the address, BAD_ENTRY with the address;
   else CPSR.T set, BAD_ENTRY, else (E) ENDIAN_UNSUPPORTED, with the word; R15 left as it is */
static_assert(offsetof(SnxArmCPU, r) + 60 == 0x3C && offsetof(SnxArmCPU, cpsr) == 0x40 &&
                  offsetof(SnxArmCPU, exit) == 0x44 && offsetof(SnxArmCPU, syndrome) == 0x48,
              "entry_exit_fn's bytes reach R15, CPSR, the exit and the syndrome at their offsets");
static_assert(SNX_ARM_BAD_ENTRY == 8 && SNX_ARM_ENDIAN_UNSUPPORTED == 9 && SNX_ARM_T == 0x20,
              "entry_exit_fn's bytes write these exits and test this bit");
static void entry_exit_fn(SnxXElf* o) {
    SgXBuf b = {0};
    unsigned char code[64];
    b.p = code;
    b.cap = sizeof code;
    sgx_bytes(&b, "39 77 3C 75 1D");             /* cmp [rdi + r15], esi; jne pc */
    sgx_bytes(&b, "F7 47 40 20 00 00 00 75 09"); /* test dword [rdi + cpsr], T; jnz thumb */
    sgx_bytes(&b, "C7 47 44 09 00 00 00 EB 07"); /* mov dword [rdi + exit], ENDIAN_UNSUPPORTED; jmp word */
    sgx_bytes(&b, "C7 47 44 08 00 00 00");       /* thumb: mov dword [rdi + exit], BAD_ENTRY */
    sgx_bytes(&b, "89 57 48 C3");                /* word: mov [rdi + syndrome], edx; ret */
    sgx_bytes(&b, "C7 47 44 08 00 00 00");       /* pc: mov dword [rdi + exit], BAD_ENTRY */
    sgx_bytes(&b, "89 77 48 C3");                /* mov [rdi + syndrome], esi; ret */
    size_t at = snx_xelf_put(o, SNX_ELF_TEXT, code, b.n, 16);
    snx_xelf_symbol(o, "snx_x64_arm_entry_exit", SNX_ELF_TEXT, at, b.n, SNX_ELF_LOCAL | SNX_ELF_FUNC);
}
static int by_address(const void* a, const void* b) {
    const Made *x = (const Made*)a, *y = (const Made*)b;
    if (x->pc != y->pc) return x->pc < y->pc ? -1 : 1;
    return strcmp(x->name, y->name);
}
int snx_x64_arm_object(SnxX64Arm* c, unsigned char** object, size_t* size, size_t* blocks) {
    SnxXElf* o = snx_xelf_new();
    *object = NULL;
    *size = 0;
    if (!o) return -1;
    entry_exit_fn(o);
    qsort(c->made, c->nmade, sizeof *c->made, by_address);
    for (size_t i = 0; i < c->nmade; ++i) {
        const Made* m = &c->made[i];
        size_t at = snx_xelf_put(o, SNX_ELF_TEXT, c->text + m->at, m->size, 16);
        /* a block's name the descriptor and LLVM's blocks call (global); an entry the object's own (X_, local) */
        snx_xelf_symbol(o, m->name, SNX_ELF_TEXT, at, m->size,
                        m->own == (size_t)-1 && m->name[0] == 'X' ? SNX_ELF_LOCAL | SNX_ELF_FUNC
                                                                  : SNX_ELF_GLOBAL | SNX_ELF_HIDDEN | SNX_ELF_FUNC);
        if (m->own != (size_t)-1) {
            char own[sizeof m->name + 1];
            snprintf(own, sizeof own, "X%s", m->name);
            /* the object's own: only its blocks jump there */
            snx_xelf_symbol(o, own, SNX_ELF_TEXT, at + m->own, m->size - m->own, SNX_ELF_LOCAL | SNX_ELF_FUNC);
        }
        for (size_t k = m->rel0; k < m->rel0 + m->nrel; ++k)
            snx_xelf_reloc(o, SNX_ELF_TEXT, at + c->rels[k].at, c->rels[k].sym, c->rels[k].type, c->rels[k].addend);
    }
    if (blocks) *blocks = c->nmade;
    /* the blocks' code given back before the object is written: it has them */
    free(c->text), free(c->rels), free(c->made);
    c->text = NULL, c->rels = NULL, c->made = NULL;
    c->ntext = c->cap_text = c->nrels = c->cap_rels = c->nmade = c->cap_made = 0;
    int rc = snx_xelf_write(o, object, size);
    snx_xelf_free(o);
    return rc ? -1 : 0;
}
int snx_x64_arm_entry(SnxX64Arm* c, uint32_t key) {
    unsigned char code[12] = {0xC7, 0x47, (unsigned char)(offsetof(SnxArmCPU, r) + 60), 0, 0, 0, 0, 0xE9, 0, 0, 0, 0};
    SnxU32 address = key & ~1u;
    memcpy(code + 3, &address, 4); /* mov dword [rdi + r15], address; jmp the block */
    Rel rel = {.at = 8, .type = R_X86_64_PLT32, .addend = -4};
    name_of(rel.sym, sizeof rel.sym, key);
    char name[24];
    snprintf(name, sizeof name, "X%s", rel.sym);
    return keep(c, code, sizeof code, &rel, 1, address, name, (size_t)-1) ? 0 : -1;
}
int snx_x64_arm_absorb(SnxX64Arm* c, SnxX64Arm* from) {
    for (size_t i = 0; i < from->nmade; ++i) {
        const Made* m = &from->made[i];
        if (!keep(c, from->text + m->at, m->size, from->rels + m->rel0, m->nrel, m->pc, m->name, m->own)) return -1;
    }
    free(from->text), free(from->rels), free(from->made);
    from->text = NULL, from->rels = NULL, from->made = NULL;
    from->ntext = from->cap_text = from->nrels = from->cap_rels = from->nmade = from->cap_made = 0;
    return 0;
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
int snx_x64_arm_absorb(SnxX64Arm* c, SnxX64Arm* from) {
    (void)c, (void)from;
    return -1;
}
int snx_x64_arm_entry(SnxX64Arm* c, uint32_t key) {
    (void)c, (void)key;
    return -1;
}
#endif
