/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's x86-64 back end, the part every machine's shares (README.md, "The x86-64 back end"), in three files
   copied at the top of the file sailgen writes after bits.h and before the guest's part (x64_a64.h, x64_a32.h),
   which defines SgCtx with an SgX in it (sgx(g)) and keeps the guest's registers:

     machdef.h  the code buffer and the forms' types every machine's back end shares (SgXBuf, SgXSel, SgXVForm);
     x64.h      x86-64's encodings, the instruction selection sailgen derived from the Intel SDM (x64_isel.inc) and
                what mach.h asks of a machine (its registers, the vector forms its front makes values with);
     mach.h     what any machine's back end does: the SSA of the block's instructions, the folds as it is appended,
                the flags, the dead stores and the register allocation;
     x64_asm.h  x86-64's assembly of what mach.h leaves to the machine (vector forms, cold paths, the frame).

   No LLVM: the generated emitters write x86-64 instructions themselves, chosen by the instruction selection sailgen
   derived from the Intel SDM (sdm/x64_isel.inc: an encoder per selected form, and each primitive's candidates). At run
   time an emitter only appends instructions to a short SSA of them (the assembly itself, virtual registers for
   operands: SgXIns), folds what is constant as it appends, and once the block is done drops what nothing reads, gives
   the virtual registers host ones and writes the bytes. */
#include "common/utilities/bits.h"

/* the r/m operand that is the constant pool's (RIP-relative: mod 00, r/m 101), which the assembler patches */
#define SGX_RIP 32

/* REX.X: the index register of a memory r/m operand is R8 to R15 */
static inline unsigned sgx_rex_x(const SgXBuf* b, int rm) { return rm >= 16 && b->index >= 8 ? 2u : 0u; }

/* ModRM: a register (0 to 15, mod 11), or the memory at a register (16 + it) plus b->index (when not -1) plus
   b->disp: mod 00 without a displacement (but for RBP and R13, which mean RIP or a displacement there), 01 of 8 bits,
   10 of 32; a SIB for an index, or for RSP and R12 as the base (Vol. 2A, 2.1.5, tables 2-2 and 2-3) */
static inline void sgx_modrm(SgXBuf* b, int reg, int rm) {
    int base, mod, sib;
    if (rm == SGX_RIP) {
        sgx_byte(b, (unsigned char)((reg & 7) << 3 | 5));
        b->rip_at = b->n;
        sgx_imm(b, 0, 4);
        return;
    }
    if (rm < 16) {
        sgx_byte(b, (unsigned char)(0xC0 | (reg & 7) << 3 | (rm & 7)));
        return;
    }
    base = rm - 16;
    mod = b->disp == 0 && (base & 7) != 5 ? 0 : b->disp >= -128 && b->disp < 128 ? 1 : 2;
    sib = b->index >= 0 || (base & 7) == 4;
    sgx_byte(b, (unsigned char)(mod << 6 | (reg & 7) << 3 | (sib ? 4 : (base & 7))));
    if (sib) sgx_byte(b, (unsigned char)((b->index >= 0 ? (b->index & 7) : 4) << 3 | (base & 7)));
    if (mod == 1) sgx_byte(b, (unsigned char)b->disp);
    else if (mod == 2)
        sgx_imm(b, b->disp, 4);
}

/* the host's features a candidate may need (CPUID's), SgX.features */
enum {
    SGX_F_BMI1 = 1, SGX_F_BMI2 = 2, SGX_F_LZCNT = 4, SGX_F_SSE3 = 8, SGX_F_SSSE3 = 16, SGX_F_SSE41 = 32,
    SGX_F_SSE42 = 64, SGX_F_PCLMUL = 128, SGX_F_F16C = 256, SGX_F_POPCNT = 512, SGX_F_AVX = 1024, SGX_F_FMA = 2048,
    SGX_F_AES = 4096, SGX_F_SHA = 8192,
    /* not CPUID's: the code's calling convention is Microsoft's x64 one (a Windows host's), System V's otherwise */
    SGX_F_WIN64 = 16384,
    SGX_F_AVX2 = 32768, SGX_F_GFNI = 65536, SGX_F_SM4 = 131072
};

#include "x64_isel.inc"

/* LEA for the additions nothing reads the flags of (add, add_i): three operands, no flags written, so an address's
   arithmetic leaves the flags a branch reads where they are. The SDM's LEA computes EffectiveAddress(SRC), which its
   Operation section doesn't spell out, so these forms are written here rather than selected (the recompiler's tests
   check them as they check the derived ones): operand 0 the result, 1 the base, 2 the index; a 32-bit one is the
   64-bit sum's low half, zero extended (no address-size prefix) */
static inline void sgx_enc_lea(SgXBuf* b, const unsigned char* r, long long imm, int w64, int with_index) {
    int d = r[0], base = r[1], index = with_index ? r[2] : -1;
    if (index == 4) { /* RSP can't be an index */
        index = base;
        base = 4;
    }
    sgx_byte(b, (unsigned char)(0x40 | (w64 ? 8 : 0) | (d >> 3) << 2 | (index > 0 ? (index >> 3) << 1 : 0) | (base >> 3)));
    sgx_byte(b, 0x8D);
    if (index >= 0) {
        int mod = imm < -128 || imm > 127 ? 2 : imm || (base & 7) == 5 ? 1 : 0;
        sgx_byte(b, (unsigned char)(mod << 6 | (d & 7) << 3 | 4));
        sgx_byte(b, (unsigned char)((index & 7) << 3 | (base & 7)));
        if (mod) sgx_imm(b, imm, mod == 1 ? 1 : 4);
    } else {
        sgx_byte(b, (unsigned char)(0x80 | (d & 7) << 3 | (base & 7)));
        if ((base & 7) == 4) sgx_byte(b, 0x24);
        sgx_imm(b, imm, 4);
    }
}
static void sgx_enc_lea_rr64(SgXBuf* b, const unsigned char* r, long long imm, int cc) { (void)imm, (void)cc, sgx_enc_lea(b, r, 0, 1, 1); }
static void sgx_enc_lea_rr32(SgXBuf* b, const unsigned char* r, long long imm, int cc) { (void)imm, (void)cc, sgx_enc_lea(b, r, 0, 0, 1); }
static void sgx_enc_lea_ri64(SgXBuf* b, const unsigned char* r, long long imm, int cc) { (void)cc, sgx_enc_lea(b, r, imm, 1, 0); }
static void sgx_enc_lea_ri32(SgXBuf* b, const unsigned char* r, long long imm, int cc) { (void)cc, sgx_enc_lea(b, r, imm, 0, 0); }
#define SGX_LEA_SEL(e, im) {.enc = e, .rm = -1, .in = {1, (im) ? -1 : 2, -1, -1}, .imm = (im) ? 2 : 0, .fit_bits = 0, \
                            .fit_kind = '-', .out = 0, .clean_out = 1, .flag_of = {-1, -1, -1, -1},                   \
                            .cc_of = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}}
static const SgXSel sgx_sel_lea_64[1] = {SGX_LEA_SEL(sgx_enc_lea_rr64, 0)};
static const SgXSel sgx_sel_lea_32[1] = {SGX_LEA_SEL(sgx_enc_lea_rr32, 0)};
static const SgXSel sgx_sel_leai_64[1] = {SGX_LEA_SEL(sgx_enc_lea_ri64, 1)};
static const SgXSel sgx_sel_leai_32[1] = {SGX_LEA_SEL(sgx_enc_lea_ri32, 1)};
#define sgx_sel_lea_64_n 1u
#define sgx_sel_lea_32_n 1u
#define sgx_sel_leai_64_n 1u
#define sgx_sel_leai_32_n 1u

/* ---- the calling convention ----
   System V's, or where the features say so Microsoft's x64 (SGX_F_WIN64): its arguments in RCX, RDX, R8 and R9, then
   on the stack past 32 bytes the caller leaves its callee (the frame's lowest slots: SGX_SLOT0), RSI, RDI and XMM6 to
   XMM15 the callee's. Values are in the registers a call may change, so a function saves no more there than here: RSI
   and RDI hold pins alone (x64_codegen.c), and XMM6 to XMM15 what the function saved (SgXAsm.xsaved) */
#define SGX_WIN64(x) (((x)->features & SGX_F_WIN64) != 0)

/* the host registers values may be in: those a call may change (RAX RCX RDX RSI RDI R8-R11, without RSI and RDI on
   Windows); RBX holds the state's address. The machine's part writes what an ABI needs (its prologue, the runtime's
   calls, the flow) */
static const signed char sgx_pool[] = {6, 7, 8, 9, 10, 11, 0, 2, 1}; /* the fixed-purpose ones (RAX, RDX, RCX: MUL, CL) last */
static const signed char sgx_pool_win64[] = {8, 9, 1, 10, 11, 0, 2}; /* RCX, no atomic sequence's input, among the first */
#define SGX_POOL(a) (SGX_WIN64((a)->x) ? sgx_pool_win64 : sgx_pool)
#define SGX_POOL_N(a) (SGX_WIN64((a)->x) ? sizeof sgx_pool_win64 : sizeof sgx_pool)
#define SGX_CALLER_SAVED (SGX_WIN64(a->x) ? 0x0F07u : 0x0FC7u) /* RAX RCX RDX (RSI RDI) R8-R11 */
/* the frame's first slot for values: below it what a Windows callee is left, four arguments' shadow and two more, then
   the XMM registers the function saved. And the XMM registers of the callee's no value may be in there */
#define SGX_WIN64_ARGS 6
#define SGX_SLOT0(a) (SGX_WIN64((a)->x) ? SGX_WIN64_ARGS + 2 * (int)snx_popcount64((a)->xsaved) : 0)
#define SGX_XCALLEE(a) (SGX_WIN64((a)->x) ? 0xFFC0u & ~(a)->xsaved : 0u)
#define SGX_RSP 4
#define SGX_FRAME SGX_RSP

/* ---- what mach.h asks of the machine ----
   its registers (16 general-purpose, 16 XMM); a selected form's place of a fixed register (SGX_FIX + it) and a
   memory operand's register (SGX_MEMR, what sgx_modrm reads as [base]); a pinned vector register's number in
   SGX_K_PGET's and SGX_K_PSET's imm (SGX_XPIN + it); the frame's register; a call's results (RAX, RDX); the
   features a general-purpose candidate alone may need (a vector form never asks for them) */
#define SGX_NREG 16
#define SGX_NXREG 16
#define SGX_FIX 16
#define SGX_MEMR(r) (16 + (r))
#define SGX_POOLR SGX_RIP /* the constant pool's operand */
#define SGX_XPIN 16
#define SGX_RET 0
#define SGX_RET2 2
#define SGX_F_GPR (SGX_F_BMI1 | SGX_F_BMI2 | SGX_F_LZCNT)
/* the flag a carry in is (ADC's CF: flag_of's index), and whether the division is Arm's (0: DIV and IDIV fault where
   Arm's give 0 and the dividend, and mach.h keeps them from those divisors) */
/* the state's address (RBX, the callee's), and a call's arguments after the state's (RDI) */
#define SGX_BASE 3
static const signed char sgx_arg_regs[6] = {7, 6, 2, 1, 8, 9};
/* Windows' (RCX, RDX, R8, R9, then the stack's fifth and sixth: the frame's slots 4 and 5, SGX_NREG + a slot) */
static const signed char sgx_arg_regs_win64[6] = {1, 2, 8, 9, 20, 21};
#define SGX_ARG_REG(x, n) (SGX_WIN64(x) ? sgx_arg_regs_win64[n] : sgx_arg_regs[n])
/* AArch32's calls' arguments after the state's (RSI, RDX, RCX, R8), and its exclusive store's address, value and the
   memory's base (RSI, RDX, R10: mach_a32.h's SGX_A32_STREX) */
static const signed char sgx_a32_args[4] = {6, 2, 1, 8};
static const signed char sgx_a32_strex[3] = {6, 2, 10};
/* Windows' (RDX, R8, R9, the stack's fifth; R9, RDX, R10) */
static const signed char sgx_a32_strex_win64[3] = {9, 2, 10};
#define SGX_A32_ARG(x, n) (SGX_WIN64(x) ? sgx_arg_regs_win64[1 + (n)] : sgx_a32_args[n])
#define SGX_A32_STREX_REGS(x) (SGX_WIN64(x) ? sgx_a32_strex_win64 : sgx_a32_strex)
#define SGX_A32_BUS_MEM(x) (SGX_WIN64(x) ? 10 : sgx_a32_args[3]) /* the fourth argument's, the stack's on Windows */
#define SGX_A32_STREX_CLOBBER (1u << 1) /* RCX, its compare's */
#define SGX_CARRY 0
#define SGX_DIV_ARM 0
/* x86's implicit <XMM0> operand (PBLENDVB's): a vector form's operand no field encodes is in XMM0 */
#define SGX_IMPLICIT_X0 1
/* the vector forms mach.h's front makes values with: a 16-byte load (of the pool) and store (of the state), a vector
   register made 0, a vector value's low 32 or 64 bits to a general-purpose register and back, its high doubleword
   to one (imm 1), a doubleword into one (imm its index) */
#define SGX_VF_LD16 (&sgx_v_movdqu_ld)
#define SGX_VF_ST16 (&sgx_v_movdqu_st)
#define SGX_VF_ZERO (&sgx_v_pxor)
#define SGX_VF_X2G32 (&sgx_v_movd_rx)
#define SGX_VF_X2G64 (&sgx_v_movq_rx)
#define SGX_VF_G2X32 (&sgx_v_movd_xr)
#define SGX_VF_G2X64 (&sgx_v_movq_xr)
#define SGX_VF_XHI2G (&sgx_v_pextrq)
#define SGX_VF_INS64 (&sgx_v_pinsrq)
/* a SIMD&FP register's 8 or 4 bytes to or from memory, its rest zeroed by the load (MOVQ, MOVD) */
#define SGX_VF_LD8 (&sgx_v_movq_ld)
#define SGX_VF_ST8 (&sgx_v_movq_st)
#define SGX_VF_LD4 (&sgx_v_movd_xr)
#define SGX_VF_ST4 (&sgx_v_movd_rx)

