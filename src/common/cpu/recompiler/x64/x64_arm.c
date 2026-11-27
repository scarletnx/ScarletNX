/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's AArch32 blocks: an A32 or Thumb block of any profile and memory mode. Each word is sailgen's
   emitter of the AArch32 Sail where its clauses take it (a32_x64.inc: the data processing and branches), else one
   written here over the same SSA region (the loads and stores, the multiplies, the exclusives...). The region is
   assembled where a condition's skip ends it, where it grows long, and at the block's end; the guest's registers are in
   the state at each region's end, as each write stores them.

   The function is a block's: void name(SnxArmCPU*), System V x86-64, the state in RDI kept in RBX (pushed), RSP
   16-aligned after the prologue (the push and a frame of spill slots, its size patched in at the end). Windows' (the
   features' SGX_F_WIN64) has the state in RCX and no other register of the callee's: a block's values are in the
   registers a call may change (x64.h's pool), so it saves RBX alone there too. Its entry checks clear the exit and its
   syndrome and check R15 the block's address and CPSR.T and E clear, and it ends after an instruction that branches,
   one that leaves for the backend, or the last word.
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

#if defined(__x86_64__) && defined(SNX_HAVE_X64)
#define MG_ARM_INC "a32_x64.inc"
#define MG_ARM SnxX64Arm
#define MG_ARM_NEXT SnxX64ArmNext
#define MG_ARM_API(name) snx_x64_arm_##name
#define MG_ARM_AVAILABLE() snx_x64_available()
#define MG_ARM_MAKES() snx_x64_available()
#define MG_ARM_FEATURES() snx_x64_features()
#define MG_ARM_EM EM_X86_64
#define MG_ARM_CALL_RELOC R_X86_64_PLT32
#define MG_ARM_LOAD_FENCE 0 /* an x86 load is an acquire one */
#define MG_ARM_FP_HOT 1
#define MG_ARM_ABI SGX_F_WIN64
/* Windows' unwinder takes a jump out of a function's entry in its table for an epilogue's: the code of a frame is one
   range, with the one exit that has none past it */
#define MG_ARM_EXIT_LAST(c) win64(c)
#define MG_ARM_COFF snx_x64_win64()
#if defined(__APPLE__) || defined(_WIN32)
#include "common/cpu/recompiler/object/elf_defs.h"
#else
#include <elf.h>
#endif

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

static int win64(const SnxX64Arm* c);
/* the helpers that compute with the host's floating point (x64_codegen.h's): a block that calls one checks MXCSR */
#define MG_HOST_FP_HELPER snx_x64_host_fp_helper
#include "common/cpu/recompiler/mach/mach_arm.inc"

/* ---- the x86-64 glue ---- */

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
                       (int)offsetof(SnxArmCPU, fpscr), (int)offsetof(SnxArmCPU, fpscr), 0, 0, NULL, 0);
}

/* Windows' calling convention: the code's, and the object's format with it */
static int win64(const MG_ARM* c) { return (c->features & SGX_F_WIN64) != 0; }
/* ModRM of [rdi + disp8] (Windows' [rcx + disp8]) with the register or opcode extension reg: the state, before RBX has it */
static unsigned char state8(const MG_ARM* c, unsigned reg) {
    return (unsigned char)((win64(c) ? 0x41 : 0x47) | reg << 3);
}

/* the frame's room, patched once the block is done (sgx_patch_frame's sizes): SUB or ADD RSP of an 8-bit immediate,
   a four-byte NOP where the block has no slot. A block whose frame passes 127 bytes is made again with 32-bit ones, and
   on Windows one of a page or more again, with a probe of it (sgx_stack_probe) */
static void frame(MG_ARM* c, int add) {
    SgXBuf* b = &c->b;
    if (c->a.nframe + (c->big > 1) >= 256) sg_fail("a block of too many epilogues", c->a.nframe, 256);
    if (c->big > 1 && !add) {
        sgx_stack_probe(b);
        c->a.frame_at[c->a.nframe++] = b->n - SGX_STACK_PROBE_SIZE + 1;
    }
    sgx_bytes(b, c->big ? (add ? "48 81 C4" : "48 81 EC") : add ? "48 83 C4" : "48 83 EC");
    c->a.frame_at[c->a.nframe++] = b->n;
    sgx_imm(b, 0, c->big ? 4 : 1);
}
/* the frame's size in each SUB and ADD (sgx_patch_frame's rule: the slots, 16-aligned): 0, or -1 where an 8-bit
   immediate doesn't hold it */
static int mg_patch_frame(MG_ARM* c) {
    int k, size = (c->a.slots_max * 8 + 15) & ~15;
    if (c->big == 1 && win64(c) && size >= 4096) return -1;
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
/* block_begin's checks of the entry's state, on RDI (RCX) */
static void entry_checks(MG_ARM* c, SnxU32 pc, int bad) {
    SgXBuf* b = &c->b;
    SgCtx* g = &c->g;
    sgx_byte(b, 0x81), sgx_byte(b, state8(c, 7));
    sgx_byte(b, (unsigned char)(offsetof(SnxArmCPU, r) + 60)), sgx_imm(b, pc, 4); /* cmp */
    sgx_a32_jcc(g, b, 5, bad);
    if (g->thumb) { /* mov eax, [rdi + cpsr]; and eax, T, E and ITSTATE's bits; cmp eax, T */
        sgx_byte(b, 0x8B), sgx_byte(b, state8(c, 0)), sgx_byte(b, (unsigned char)offsetof(SnxArmCPU, cpsr));
        sgx_byte(b, 0x25), sgx_imm(b, (long long)(SNX_ARM_T | SNX_ARM_E | SG_IT_BITS), 4);
        sgx_bytes(b, "83 F8"), sgx_byte(b, SNX_ARM_T);
    } else {
        sgx_byte(b, 0xF7), sgx_byte(b, state8(c, 0)), sgx_byte(b, (unsigned char)offsetof(SnxArmCPU, cpsr));
        sgx_imm(b, SNX_ARM_T | SNX_ARM_E, 4);
    }
    sgx_a32_jcc(g, b, 5, bad);
    sgx_bytes(b, "48 C7"), sgx_byte(b, state8(c, 0)), sgx_byte(b, (unsigned char)offsetof(SnxArmCPU, exit)),
        sgx_imm(b, 0, 4); /* and syndrome */
}
/* the entry (block_begin's checks: R15 the block's address, CPSR.T and E clear, a Thumb block's T set and its ITSTATE
   0; then exit and syndrome cleared), on RDI (RCX), before the frame; where a check fails, a jump to bad (entry_exit). Then
   the entry the block's own jumps from another of the x86-64 back end's take (X and the block's name), past them: RBX
   the state, the frame. A hosted block's entry is that one alone (codegen.h) */
static void mg_prologue(MG_ARM* c, SnxU32 pc, int* bad) {
    SgXBuf* b = &c->b;
    SgCtx* g = &c->g;
    *bad = sgx_a32_label(g);
    if (!sgx_a32_hosted(g)) entry_checks(c, pc, *bad);
    c->own = b->n;
    sgx_bytes(b, win64(c) ? "53 48 89 CB" : "53 48 89 FB"); /* push rbx; mov rbx, rdi (rcx) */
    frame(c, 0);
    if (c->fp_entry) { /* a block with floating point of the host's (mach_arm.inc's block) */
        if (c->a.slots_max < 1) c->a.slots_max = 1; /* the frame's first slot, SNX_X64_MXCSR_ENTRY's */
        sgx_bytes(b, SNX_X64_MXCSR_ENTRY);
    }
}
/* where a check of the entry failed: ESI the block's address, EDX its first word (a Thumb block's halfword; Windows' EDX
   and R8D), and snx_x64_arm_entry_exit or snx_x64_thumb_entry (the object's own, which set the exit) */
static void mg_entry_exit(MG_ARM* c, SnxU32 pc, SnxU32 first, int bad) {
    SgXBuf* b = &c->b;
    sgx_a32_bind(&c->g, b, bad);
    sgx_byte(b, win64(c) ? 0xBA : 0xBE), sgx_imm(b, pc, 4);                   /* mov esi (edx), pc */
    sgx_bytes(b, win64(c) ? "41 B8" : "BA"), sgx_imm(b, (long long)first, 4); /* mov edx (r8d), first */
    sgx_byte(b, 0xE9);
    add_rel(c, b->n, c->g.thumb ? "snx_x64_thumb_entry" : "snx_x64_arm_entry_exit", R_X86_64_PLT32, -4);
    sgx_imm(b, 0, 4);
}
/* the frame given back and RBX the caller's */
static void epilogue(MG_ARM* c) {
    frame(c, 1);
    sgx_bytes(&c->b, "5B"); /* pop rbx */
}
/* where the block ends with R15 key's address in key's state: a jump to the block's own entry past its checks (X and
   its name: the code there reads no R15, and its every exit stores it) where key is one of the successors, after the
   poll where polled (key at or before the block's start); else R15 stored and the return. public: the block's checked
   entry instead, R15 stored (after an MSR that may have set E, which that entry checks) */
static void mg_go_on(MG_ARM* c, SnxU32 key, int polled, const SnxU32* next, size_t nnext, int public) {
    SgXBuf* b = &c->b;
    SgCtx* g = &c->g;
    size_t k = 0;
    while (k < nnext && next[k] != key) ++k;
    if (k == nnext || public) sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, key & ~1u);
    if (k == nnext) {
        sgx_a32_jmp(g, b, g->ret);
        return;
    }
    if (polled) {
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
    sgx_state_arg(&c->a, b);
    epilogue(c);
    char sym[24];
    name_of(sym + (public ? 0 : 1), sizeof sym - 1, key);
    if (!public) sym[0] = 'X';
    sgx_byte(b, 0xE9);
    add_rel(c, b->n, sym, R_X86_64_PLT32, -4);
    sgx_imm(b, 0, 4);
}

/* the block's end in a tail call of the block at the address in reg: the state its argument, as it is this one's */
static void mg_tail(MG_ARM* c, int reg) {
    SgXBuf* b = &c->b;
    if (reg != SGX_RET) sgx_mov_rr(&c->a, SGX_RET, reg);
    sgx_state_arg(&c->a, b);
    epilogue(c);
    sgx_bytes(b, "48 FF E0"); /* jmp rax, whose REX.W says to Windows' unwinder that it ends the epilogue */
}

/* the block's return: the epilogue and RET */
static void mg_return(MG_ARM* c) {
    epilogue(c);
    sgx_byte(&c->b, 0xC3); /* ret */
}
/* snx_x64_arm_entry_exit(state RDI, the block's address ESI, its first word EDX; Windows' RCX, EDX, R8D), each object's
   own, where a block's entry check failed, in this order: R15 not the address, BAD_ENTRY with the
   address; else CPSR.T set, BAD_ENTRY, else (E) ENDIAN_UNSUPPORTED, with the word; R15 left as it is */
static_assert(offsetof(SnxArmCPU, r) + 60 == 0x3C && offsetof(SnxArmCPU, cpsr) == 0x40 &&
                  offsetof(SnxArmCPU, exit) == 0x44 && offsetof(SnxArmCPU, syndrome) == 0x48,
              "entry_exit_fn's bytes reach R15, CPSR, the exit and the syndrome at their offsets");
static_assert(SNX_ARM_BAD_ENTRY == 8 && SNX_ARM_ENDIAN_UNSUPPORTED == 9 && SNX_ARM_IT_ENTRY == 13 && SNX_ARM_T == 0x20,
              "entry_exit_fn's and thumb_entry_exit_fn's bytes write these exits and test this bit");
static void entry_exit_fn(SnxXElf* o) {
    SgXBuf b = {0};
    unsigned char code[64];
    const int w = snx_x64_win64();
    size_t pc, thumb, word;
    b.p = code;
    b.cap = sizeof code;
    sgx_bytes(&b, w ? "39 51 3C 75 00" : "39 77 3C 75 00"), pc = b.n - 1; /* cmp [rdi + r15], esi; jne pc */
    sgx_bytes(&b, w ? "F7 41 40 20 00 00 00 75 00" : "F7 47 40 20 00 00 00 75 00");
    thumb = b.n - 1; /* test dword [rdi + cpsr], T; jnz thumb */
    sgx_bytes(&b, w ? "C7 41 44 09 00 00 00 EB 00" : "C7 47 44 09 00 00 00 EB 00");
    word = b.n - 1; /* mov dword [rdi + exit], ENDIAN_UNSUPPORTED; jmp word */
    sgx_a32_patch8(&b, thumb);
    sgx_bytes(&b, w ? "C7 41 44 08 00 00 00" : "C7 47 44 08 00 00 00"); /* thumb: mov dword [rdi + exit], BAD_ENTRY */
    sgx_a32_patch8(&b, word);
    sgx_bytes(&b, w ? "44 89 41 48 C3" : "89 57 48 C3"); /* word: mov [rdi + syndrome], edx (r8d); ret */
    sgx_a32_patch8(&b, pc);
    sgx_bytes(&b, w ? "C7 41 44 08 00 00 00" : "C7 47 44 08 00 00 00"); /* pc: mov dword [rdi + exit], BAD_ENTRY */
    sgx_bytes(&b, w ? "89 51 48 C3" : "89 77 48 C3");                   /* mov [rdi + syndrome], esi (edx); ret */
    size_t at = snx_xelf_put(o, SNX_ELF_TEXT, code, b.n, 16);
    snx_xelf_symbol(o, "snx_x64_arm_entry_exit", SNX_ELF_TEXT, at, b.n, SNX_ELF_LOCAL | SNX_ELF_FUNC);
}
/* snx_x64_thumb_entry(state RDI, the block's address ESI, its first halfword EDX; Windows' RCX, EDX, R8D), where a Thumb
   block's entry check failed (thumb_block's, in its order): R15 not the address, BAD_ENTRY with the address; CPSR.T
   clear, BAD_ENTRY with the halfword; ITSTATE in CPSR (its [1:0] in bits 26:25, [7:2] in 15:10), IT_ENTRY with it; else
   (E) ENDIAN_UNSUPPORTED with the halfword */
static void thumb_entry_exit_fn(SnxXElf* o) {
    SgXBuf b = {0};
    unsigned char code[96];
    const int w = snx_x64_win64();
    b.p = code;
    b.cap = sizeof code;
    sgx_bytes(&b, w ? "39 51 3C 75 00" : "39 77 3C 75 00"); /* cmp [rdi + r15], esi; jne pc */
    size_t pc = b.n - 1;
    sgx_bytes(&b,
              w ? "8B 41 40 A8 20 74 00" : "8B 47 40 A8 20 74 00"); /* mov eax, [rdi + cpsr]; test al, T; jz clear */
    size_t clear = b.n - 1;
    /* mov ecx (r9d), eax; shr ecx, 25; and ecx, 3 */
    sgx_bytes(&b, w ? "41 89 C1 41 C1 E9 19 41 83 E1 03" : "89 C1 C1 E9 19 83 E1 03");
    sgx_bytes(&b, "C1 E8 08 25 FC 00 00 00");            /* shr eax, 8; and eax, 0xFC */
    sgx_bytes(&b, w ? "41 09 C1 75 00" : "09 C1 75 00"); /* or ecx, eax; jnz it */
    size_t it = b.n - 1;
    /* mov dword [rdi + exit], ENDIAN_UNSUPPORTED; jmp word */
    sgx_bytes(&b, w ? "C7 41 44 09 00 00 00 EB 00" : "C7 47 44 09 00 00 00 EB 00");
    size_t word = b.n - 1;
    sgx_a32_patch8(&b, clear);
    sgx_bytes(&b, w ? "C7 41 44 08 00 00 00" : "C7 47 44 08 00 00 00"); /* clear: mov dword [rdi + exit], BAD_ENTRY */
    sgx_a32_patch8(&b, word);
    sgx_bytes(&b, w ? "44 89 41 48 C3" : "89 57 48 C3"); /* word: mov [rdi + syndrome], edx; ret */
    sgx_a32_patch8(&b, it);
    /* it: mov dword [rdi + exit], IT_ENTRY; mov [rdi + syndrome], ecx; ret */
    sgx_bytes(&b, w ? "C7 41 44 0D 00 00 00 44 89 49 48 C3" : "C7 47 44 0D 00 00 00 89 4F 48 C3");
    sgx_a32_patch8(&b, pc);
    /* pc: BAD_ENTRY; mov [rdi + syndrome], esi; ret */
    sgx_bytes(&b, w ? "C7 41 44 08 00 00 00 89 51 48 C3" : "C7 47 44 08 00 00 00 89 77 48 C3");
    if (b.n > b.cap) sg_fail("the Thumb entry's exit past its room", (long long)b.n, (long long)b.cap);
    size_t at = snx_xelf_put(o, SNX_ELF_TEXT, code, b.n, 16);
    snx_xelf_symbol(o, "snx_x64_thumb_entry", SNX_ELF_TEXT, at, b.n, SNX_ELF_LOCAL | SNX_ELF_FUNC);
}
static void mg_entry_exit_fn(SnxXElf* o) {
    entry_exit_fn(o);
    thumb_entry_exit_fn(o);
}
int snx_x64_arm_entry(MG_ARM* c, uint32_t key) {
    unsigned char code[12] = {0xC7, state8(c, 0), (unsigned char)(offsetof(SnxArmCPU, r) + 60), 0, 0, 0, 0, 0xE9};
    SnxU32 address = key & ~1u;
    memcpy(code + 3, &address, 4); /* mov dword [rdi + r15], address; jmp the block */
    Rel rel = {.at = 8, .type = R_X86_64_PLT32, .addend = -4};
    name_of(rel.sym, sizeof rel.sym, key);
    char name[24];
    snprintf(name, sizeof name, "X%s", rel.sym);
    return keep(c, code, sizeof code, &rel, 1, address, name, (size_t)-1, 0) ? 0 : -1;
}
#else
int snx_x64_arm_available(void) { return 0; }
SnxX64Arm* snx_x64_arm_new(void) { return NULL; }
void snx_x64_arm_free(SnxX64Arm* c) { (void)c; }
int snx_x64_arm_options(SnxX64Arm* c, const SnxArmCodegenOptions* options) {
    (void)c, (void)options;
    return -1;
}
int snx_x64_arm_block(SnxX64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                      size_t* consumed, SnxX64ArmNext next, void* user) {
    (void)c, (void)name, (void)code, (void)count, (void)pc, (void)next, (void)user;
    *consumed = 0;
    return 0;
}
int snx_x64_arm_thumb_block(SnxX64Arm* c, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                            size_t* consumed, SnxX64ArmNext next, void* user) {
    return snx_x64_arm_block(c, name, code, count, pc, consumed, next, user);
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
