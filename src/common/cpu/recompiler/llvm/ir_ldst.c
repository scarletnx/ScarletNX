/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/translate-a64.c: the trans_* functions of these instructions, do_STG,
   ext_and_shift_reg and the op_addr_* address helpers), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The loads and stores as LLVM IR, keyed on the generated decoder's ops and arguments. The general-purpose
   registers' (every addressing mode, the literals, the pairs, the exclusives, load-acquire and store-release, the LSE
   and RCpc atomics) are generated from their Sail (ir_sail.c, nx.sail's guest memory); here are the FP/SIMD registers'
   forms of every addressing mode, the pairs and the literals, the AdvSIMD structure loads and stores, and the
   memory-tagging loads and stores as FEAT_MTE has them (instructions only: no allocation tags are stored, so a tag
   reads as 0). Guest memory uses ir_state.c's accessors (c->mem plus the guest address, unaligned and volatile only
   with volatile_memory enabled), so a page the GPU owns (memory/own.c) traps on the host access as it does for every
   other one. */
#include "common/cpu/recompiler/llvm/ir_ldst.h"
#include "common/cpu/recompiler/llvm/ir_sail.h"
#include "common/cpu/aarch64/cpu.h"

typedef LLVMValueRef V;
#define B ir->b

/* ---- operands ---- */

static LLVMTypeRef bytes_type(SnxIr* ir, unsigned n) { return LLVMVectorType(ir->t_i8, n); }

/* the whole of Vn as 16 bytes, in the guest's order on any host (the CPU structure aligns V128 to 8) */
static V vreg(SnxIr* ir, unsigned n) {
    V load = LLVMBuildLoad2(B, bytes_type(ir, 16), ir_cpu(ir, ir_off_v(n)), "");
    LLVMSetAlignment(load, 8);
    return ir_tag_cpu(ir, load);
}
/* Vn = a 16-byte vector of any lane type */
static void set_vreg(SnxIr* ir, unsigned n, V value) {
    ir_vectors_invalidate(ir);
    V store = LLVMBuildStore(B, value, ir_cpu(ir, ir_off_v(n)));
    LLVMSetAlignment(store, 8);
    ir_tag_cpu(ir, store);
}

/* shufflevector(a, b, mask) with a mask of n indices */
static V shuffle(SnxIr* ir, V a, V b, const unsigned* mask, unsigned n) {
    V m[64];
    for (unsigned i = 0; i < n; ++i) m[i] = ir_i32(ir, mask[i]);
    return LLVMBuildShuffleVector(B, a, b, LLVMConstVector(m, n), "");
}

/* <16 x i8> of the count bytes of v (<total x i8>) from first, the rest zero (do_fp_ld's clear_vec_high) */
static V widen_bytes(SnxIr* ir, V v, unsigned total, unsigned first, unsigned count) {
    unsigned mask[16];
    if (total == 16 && first == 0 && count == 16) return v;
    for (unsigned i = 0; i < 16; ++i) mask[i] = i < count ? first + i : total;
    return shuffle(ir, v, LLVMConstNull(bytes_type(ir, total)), mask, 16);
}

/* ---- immediate, register and literal offsets ---- */

/* the same for the FP/SIMD registers: the low 1 << sz bytes of Vt, a load clearing the rest (do_fp_ld, do_fp_st) */
static void fp_access(SnxIr* ir, V address, unsigned rt, unsigned sz, int store) {
    unsigned n = 1u << sz;
    if (store && sz < 4)
        /* A scalar store needs only its lane, not a whole V register and a byte shuffle. Keep the access width,
           unaligned guest access and endian conversion of ir_wr; this also avoids vector temporaries at O0. */
        ir_wr(ir, address, 8u << sz, ir_ve(ir, rt, sz, 0));
    else if (store)
        ir_wr_bytes(ir, address, vreg(ir, rt));
    else
        set_vreg(ir, rt, widen_bytes(ir, ir_rd_bytes(ir, address, n), n, 0, n));
}

/* the immediate forms: the address is Xn + imm unless post-indexed (p), and the writeback (w) puts Xn + imm in Xn
   after the access (op_addr_ldst_imm_pre/post) */
static void ldst_imm(SnxIr* ir, const SnxA64Args* a, int store) {
    unsigned rn = (unsigned)a->rn;
    SnxU64 offset = (SnxU64)(int64_t)a->imm;
    V base = ir_x(ir, rn), address = a->p ? base : LLVMBuildAdd(B, base, ir_i64(ir, offset), "");
    fp_access(ir, address, (unsigned)a->rt, (unsigned)a->sz, store);
    if (a->w) ir_set_x(ir, rn, a->p ? LLVMBuildAdd(B, base, ir_i64(ir, offset), "") : address);
}

/* the register offset: Xn + Rm extended by option (UXTW, LSL, SXTW, SXTX) and shifted by the size when s
   (op_addr_ldst_pre, ext_and_shift_reg) */
static void ldst_reg(SnxIr* ir, const SnxA64Args* a, int store) {
    static const unsigned widths[4] = {8, 16, 32, 64};
    unsigned option = (unsigned)a->opt, sz = (unsigned)a->sz;
    V m = ir_rx(ir, (unsigned)a->rm);
    if ((option & 3) != 3)
        m = ir_cast(ir, ir_cast(ir, m, ir_int_type(ir, widths[option & 3]), 0), ir->t_i64, option & 4);
    if (a->s) m = LLVMBuildShl(B, m, ir_i64(ir, sz), "");
    V address = LLVMBuildAdd(B, ir_x(ir, (unsigned)a->rn), m, "");
    fp_access(ir, address, (unsigned)a->rt, sz, store);
}

/* ---- pairs ---- */

/* STP, LDP and the non-temporal STNP and LDNP of the FP/SIMD registers, which take the same rows: the offset is imm
   scaled by the size, and the addressing and writeback are the immediate forms'. One access of both registers' low
   bytes (trans_LDP_v, trans_STP_v make two adjacent ones) */
static void ldst_pair(SnxIr* ir, const SnxA64Args* a, int store) {
    unsigned rn = (unsigned)a->rn, rt = (unsigned)a->rt, rt2 = (unsigned)a->rt2, sz = (unsigned)a->sz;
    SnxU64 offset = (SnxU64)(int64_t)a->imm << sz;
    V base = ir_x(ir, rn), address = a->p ? base : LLVMBuildAdd(B, base, ir_i64(ir, offset), "");
    unsigned n = 1u << sz, mask[32];
    if (store) {
        for (unsigned i = 0; i < 2 * n; ++i) mask[i] = i < n ? i : 16 + i - n;
        ir_wr_bytes(ir, address, shuffle(ir, vreg(ir, rt), vreg(ir, rt2), mask, 2 * n));
    } else {
        V both = ir_rd_bytes(ir, address, 2 * n);
        set_vreg(ir, rt, widen_bytes(ir, both, 2 * n, 0, n));
        set_vreg(ir, rt2, widen_bytes(ir, both, 2 * n, n, n));
    }
    if (a->w) ir_set_x(ir, rn, a->p ? LLVMBuildAdd(B, base, ir_i64(ir, offset), "") : address);
}

/* ---- the AdvSIMD structures ---- */

/* the post-index writeback of the structure forms: Xn += Xm, or the bytes transferred for Rm 31 */
static void structure_writeback(SnxIr* ir, const SnxA64Args* a, V base, unsigned total) {
    unsigned rm = (unsigned)a->rm;
    if (!a->p) return;
    ir_set_x(ir, (unsigned)a->rn, LLVMBuildAdd(B, base, rm == 31 ? ir_i64(ir, total) : ir_x(ir, rm), ""));
}

/* LD1-LD4 and ST1-ST4 (multiple structures): rpt registers of selem-element structures, interleaved in memory
   (element e of register r + x at ((r * elements + e) * selem + x) << sz), the register list wrapping at V31. One
   access of all the bytes, the interleaving done as a byte shuffle; a 64-bit load clears its registers' high halves
   (trans_LD_mult, trans_ST_mult) */
static void ldst_mult(SnxIr* ir, const SnxA64Args* a, int store) {
    unsigned size = (unsigned)a->sz, esize = 1u << size, vbytes = a->q ? 16 : 8, elements = vbytes >> size;
    unsigned selem = (unsigned)a->selem, rpt = (unsigned)a->rpt, regs = rpt * selem, total = regs * vbytes;
    unsigned rt = (unsigned)a->rt, mask[64];
    V base = ir_x(ir, (unsigned)a->rn);
    /* where byte o of the memory comes from, as register j's byte */
    for (unsigned i = 0; i < total; ++i) {
        unsigned k = i >> size, x = k % selem, e = (k / selem) % elements, r = k / (selem * elements);
        mask[i] = (r + x) * 16 + e * esize + (i & (esize - 1));
    }
    if (store) {
        V v[4], wide;
        for (unsigned j = 0; j < regs; ++j) v[j] = vreg(ir, (rt + j) % 32);
        if (regs == 1)
            wide = v[0];
        else {
            unsigned concat[64];
            for (unsigned i = 0; i < 64; ++i) concat[i] = i;
            wide = shuffle(ir, v[0], v[1], concat, 32);
            if (regs > 2)
                wide = shuffle(ir, wide, shuffle(ir, v[2], regs > 3 ? v[3] : LLVMConstNull(bytes_type(ir, 16)), concat, 32),
                               concat, 64);
        }
        ir_wr_bytes(ir, base, shuffle(ir, wide, wide, mask, total));
    } else {
        V memory = ir_rd_bytes(ir, base, total), zero = LLVMConstNull(bytes_type(ir, total));
        for (unsigned j = 0; j < regs; ++j) {
            unsigned lanes[16];
            for (unsigned i = 0; i < 16; ++i) lanes[i] = total; /* zero */
            for (unsigned i = 0; i < total; ++i)
                if (mask[i] / 16 == j) lanes[mask[i] % 16] = i;
            set_vreg(ir, (rt + j) % 32, shuffle(ir, memory, zero, lanes, 16));
        }
    }
    structure_writeback(ir, a, base, total);
}

/* LD1-LD4 and ST1-ST4 (single structure): element index of selem consecutive registers, the other elements kept
   (trans_LD_single, trans_ST_single) */
static void ldst_single(SnxIr* ir, const SnxA64Args* a, int store) {
    unsigned scale = (unsigned)a->scale, esize = 1u << scale, selem = (unsigned)a->selem;
    V base = ir_x(ir, (unsigned)a->rn);
    for (unsigned x = 0; x < selem; ++x) {
        unsigned t = ((unsigned)a->rt + x) % 32;
        V address = x ? LLVMBuildAdd(B, base, ir_i64(ir, x * esize), "") : base;
        if (store)
            ir_wr(ir, address, 8 * esize, ir_ve(ir, t, scale, (unsigned)a->index));
        else
            ir_set_ve(ir, t, scale, (unsigned)a->index, ir_rd(ir, address, 8 * esize));
    }
    structure_writeback(ir, a, base, selem * esize);
}

/* LD1R-LD4R: one element to every element of selem consecutive registers, the high half cleared for a 64-bit
   arrangement (trans_LD_single_repl) */
static void ld_single_repl(SnxIr* ir, const SnxA64Args* a) {
    unsigned scale = (unsigned)a->scale, esize = 1u << scale, selem = (unsigned)a->selem, lanes = 16 >> scale;
    LLVMTypeRef type = ir_int_type(ir, 8 * esize), vector = LLVMVectorType(type, lanes);
    V base = ir_x(ir, (unsigned)a->rn);
    for (unsigned x = 0; x < selem; ++x) {
        unsigned mask[16];
        V address = x ? LLVMBuildAdd(B, base, ir_i64(ir, x * esize), "") : base;
        V value = ir_rd(ir, address, 8 * esize);
        V splat = LLVMBuildInsertElement(B, LLVMConstNull(vector), value, ir_i32(ir, 0), "");
        for (unsigned i = 0; i < lanes; ++i) mask[i] = a->q || i < lanes / 2 ? 0 : lanes;
        splat = shuffle(ir, splat, LLVMConstNull(vector), mask, lanes);
        set_vreg(ir, ((unsigned)a->rt + x) % 32, ir_guest_order(ir, splat));
    }
    structure_writeback(ir, a, base, selem * esize);
}

/* ---- memory tagging (FEAT_MTE, instructions only) ----
   No allocation tags are stored: STG and ST2G only address their granules, the zeroing forms clear them, LDG gives
   Rt a 0 tag, and STGP is the store of its pair, as QEMU has them with tag access off (do_STG, trans_LDG,
   trans_STGP). The granule is 16 bytes, and an immediate counts granules (the decoder scales it) */

/* STG, STZG (zero), ST2G, STZ2G (pair: two granules) */
static void store_tag(SnxIr* ir, const SnxA64Args* a, int zero, int pair) {
    unsigned rn = (unsigned)a->rn;
    SnxU64 offset = (SnxU64)(int64_t)a->imm;
    V base = ir_x(ir, rn), address = a->p ? base : LLVMBuildAdd(B, base, ir_i64(ir, offset), "");
    if (zero) ir_wr_bytes(ir, address, LLVMConstNull(bytes_type(ir, pair ? 32 : 16)));
    if (a->w) ir_set_x(ir, rn, a->p ? LLVMBuildAdd(B, base, ir_i64(ir, offset), "") : address);
}

/* LDG: Rt with its tag bits 59:56 cleared, the tag of a granule holding none */
static void load_tag(SnxIr* ir, const SnxA64Args* a) {
    unsigned rt = (unsigned)a->rt;
    ir_setx(ir, rt, LLVMBuildAnd(B, ir_rx(ir, rt), ir_i64(ir, ~(0xFull << 56)), ""));
}

/* STGP: Xt and Xt2 to the 16 bytes at the address, the offset scaled by the granule */
static void store_tag_pair(SnxIr* ir, const SnxA64Args* a) {
    unsigned rn = (unsigned)a->rn;
    SnxU64 offset = (SnxU64)(int64_t)a->imm << 4;
    V base = ir_x(ir, rn), address = a->p ? base : LLVMBuildAdd(B, base, ir_i64(ir, offset), "");
    V first = ir_rx(ir, (unsigned)a->rt), second = ir_rx(ir, (unsigned)a->rt2);
    ir_wr(ir, address, 64, first);
    ir_wr(ir, LLVMBuildAdd(B, address, ir_i64(ir, 8), ""), 64, second);
    if (a->w) ir_set_x(ir, rn, a->p ? LLVMBuildAdd(B, base, ir_i64(ir, offset), "") : address);
}

/* ---- the entry points ---- */

/* the general-purpose registers' loads and stores, whose emitters sailgen generates from the Sail (a64.roots) */
static int generated(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_STR_i:
        case SNX_A64_OP_LDR_i:
        case SNX_A64_OP_STR:
        case SNX_A64_OP_LDR:
        case SNX_A64_OP_LD_lit:
        case SNX_A64_OP_STP:
        case SNX_A64_OP_LDP:
        case SNX_A64_OP_STXR:
        case SNX_A64_OP_LDXR:
        case SNX_A64_OP_STXP:
        case SNX_A64_OP_LDXP:
        case SNX_A64_OP_STLR:
        case SNX_A64_OP_LDAR:
        case SNX_A64_OP_LDAPR:
        case SNX_A64_OP_LDAPR_i:
        case SNX_A64_OP_STLR_i:
        case SNX_A64_OP_CAS:
        case SNX_A64_OP_CASP:
        case SNX_A64_OP_LDADD:
        case SNX_A64_OP_LDCLR:
        case SNX_A64_OP_LDEOR:
        case SNX_A64_OP_LDSET:
        case SNX_A64_OP_LDSMAX:
        case SNX_A64_OP_LDSMIN:
        case SNX_A64_OP_LDUMAX:
        case SNX_A64_OP_LDUMIN:
        case SNX_A64_OP_SWP: return 1;
        default: return 0;
    }
}

int snx_ir_ldst_handles(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_STR_i:
        case SNX_A64_OP_LDR_i:
        case SNX_A64_OP_STR_v_i:
        case SNX_A64_OP_LDR_v_i:
        case SNX_A64_OP_STR:
        case SNX_A64_OP_LDR:
        case SNX_A64_OP_STR_v:
        case SNX_A64_OP_LDR_v:
        case SNX_A64_OP_LD_lit:
        case SNX_A64_OP_LD_lit_v:
        case SNX_A64_OP_STP:
        case SNX_A64_OP_LDP:
        case SNX_A64_OP_STP_v:
        case SNX_A64_OP_LDP_v:
        case SNX_A64_OP_STXR:
        case SNX_A64_OP_LDXR:
        case SNX_A64_OP_STXP:
        case SNX_A64_OP_LDXP:
        case SNX_A64_OP_STLR:
        case SNX_A64_OP_LDAR:
        case SNX_A64_OP_LDAPR:
        case SNX_A64_OP_LDAPR_i:
        case SNX_A64_OP_STLR_i:
        case SNX_A64_OP_CAS:
        case SNX_A64_OP_CASP:
        case SNX_A64_OP_LDADD:
        case SNX_A64_OP_LDCLR:
        case SNX_A64_OP_LDEOR:
        case SNX_A64_OP_LDSET:
        case SNX_A64_OP_LDSMAX:
        case SNX_A64_OP_LDSMIN:
        case SNX_A64_OP_LDUMAX:
        case SNX_A64_OP_LDUMIN:
        case SNX_A64_OP_SWP:
        case SNX_A64_OP_ST_mult:
        case SNX_A64_OP_LD_mult:
        case SNX_A64_OP_ST_single:
        case SNX_A64_OP_LD_single:
        case SNX_A64_OP_LD_single_repl:
        case SNX_A64_OP_STGP:
        case SNX_A64_OP_STG:
        case SNX_A64_OP_STZG:
        case SNX_A64_OP_ST2G:
        case SNX_A64_OP_STZ2G:
        case SNX_A64_OP_LDG: return 1;
        default: return 0;
    }
}

/* the word as the Sail's decode takes it: the fields an exclusive leaves unused (STXR's Rt2, LDXR's Rs and Rt2, LDXP's
   Rs) are ones in its decode clauses, which the Arm ARM asks of them, while QEMU's Cortex-A57, which qemu_ref_selftest
   holds the decoder to, runs the word whatever they hold */
static SnxU32 sail_word(SnxA64Op op, SnxU32 word) {
    switch (op) {
        case SNX_A64_OP_STXR: return word | 0x1fu << 10;
        case SNX_A64_OP_LDXR: return word | 0x1fu << 16 | 0x1fu << 10;
        case SNX_A64_OP_LDXP: return word | 0x1fu << 16;
        default: return word;
    }
}

void snx_ir_ldst(SnxIr* ir, const SnxA64Insn* insn, SnxU32 word, SnxU64 pc) {
    const SnxA64Args* a = &insn->args;
    if (generated(insn->op)) {
        if (ir->ldst_reference)
            ir->ldst_reference(ir, insn, word, pc);
        else if (!snx_ir_sail(ir, sail_word(insn->op, word), pc) && !ir->fallbacks++) {
            /* the decoder's op and the Sail's decode clauses disagree on the word: the module is refused, naming it */
            ir->fallback_word = word;
            ir->fallback_pc = pc;
        }
        return;
    }
    switch (insn->op) {
        case SNX_A64_OP_STR_v_i: ldst_imm(ir, a, 1); break;
        case SNX_A64_OP_LDR_v_i: ldst_imm(ir, a, 0); break;
        case SNX_A64_OP_STR_v: ldst_reg(ir, a, 1); break;
        case SNX_A64_OP_LDR_v: ldst_reg(ir, a, 0); break;
        case SNX_A64_OP_LD_lit_v:
            fp_access(ir, ir_i64(ir, pc + (SnxU64)(int64_t)a->imm), (unsigned)a->rt, (unsigned)a->sz, 0);
            break;
        case SNX_A64_OP_STP_v: ldst_pair(ir, a, 1); break;
        case SNX_A64_OP_LDP_v: ldst_pair(ir, a, 0); break;
        case SNX_A64_OP_ST_mult: ldst_mult(ir, a, 1); break;
        case SNX_A64_OP_LD_mult: ldst_mult(ir, a, 0); break;
        case SNX_A64_OP_ST_single: ldst_single(ir, a, 1); break;
        case SNX_A64_OP_LD_single: ldst_single(ir, a, 0); break;
        case SNX_A64_OP_LD_single_repl: ld_single_repl(ir, a); break;
        case SNX_A64_OP_STGP: store_tag_pair(ir, a); break;
        case SNX_A64_OP_STG: store_tag(ir, a, 0, 0); break;
        case SNX_A64_OP_STZG: store_tag(ir, a, 1, 0); break;
        case SNX_A64_OP_ST2G: store_tag(ir, a, 0, 1); break;
        case SNX_A64_OP_STZ2G: store_tag(ir, a, 1, 1); break;
        case SNX_A64_OP_LDG: load_tag(ir, a); break;
        default: break;
    }
}
