/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The LLVM backend: A64 to LLVM IR through the LLVM-C API, mirroring the instructions' C.
   Each emitter builds the IR of the C a64.c writes for the same instruction (a64_templates.inc, snx_a64_fast_simd,
   codegen.c's snx_codegen_emit, as the interpreter and the static steps have it), with the instruction word and pc
   constant. cpu.h's inline helpers
   become internal always-inline IR functions (ir.c); its out-of-line helpers are called in the runtime library
   (rtlib.c), which every module links. */
#ifndef SNX_RECOMPILER_LLVM_IR_H
#define SNX_RECOMPILER_LLVM_IR_H

#include "common/cpu/recompiler/codegen.h"
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

/* cpu.h's inline functions and macro bodies, built on first use in each module */
enum {
    SNX_IR_ADDS64,
    SNX_IR_ADDS32,
    SNX_IR_SUBS64,
    SNX_IR_SUBS32,
    SNX_IR_LOGIC64,
    SNX_IR_LOGIC32,
    SNX_IR_COND_OK,
    SNX_IR_SET_NZCV,
    SNX_IR_ROR64,
    SNX_IR_ROR32,
    SNX_IR_SHF64,
    SNX_IR_SHF32,
    SNX_IR_EXT64,
    SNX_IR_CLZ64,
    SNX_IR_CLZ32,
    SNX_IR_CLS64,
    SNX_IR_CLS32,
    SNX_IR_RBIT64,
    SNX_IR_RBIT32,
    SNX_IR_REV16_64,
    SNX_IR_REV32_64,
    SNX_IR_BSWAP32,
    SNX_IR_BSWAP64,
    SNX_IR_UDIV64,
    SNX_IR_UDIV32,
    SNX_IR_SDIV64,
    SNX_IR_SDIV32,
    SNX_IR_UMULH64,
    SNX_IR_SMULH64,
    SNX_IR_BMASK,
    SNX_IR_UBFM,
    SNX_IR_SBFM,
    SNX_IR_BFM,
    SNX_IR_LDX,
    SNX_IR_STX,
    SNX_IR_LDXP,
    SNX_IR_STXP,
    SNX_IR_LOOKUP,
    SNX_IR_DC_ZVA,
    SNX_IR_VFP_IMM32,
    SNX_IR_VFP_IMM64,
    SNX_IR_HELPER_COUNT
};

/* Instruction set extensions the generated code may use. The host's are read once per process (snx_ir_init, from
   LLVM's CPUID-based host detection) into snx_ir_host; an SnxIr for another triple (tests) gets none of them. The
   x86 fields are only ever set when the target is x86-64, so an emitter gated on them is gated on the architecture */
typedef struct {
    int x86;                                  /* x86-64 target: x86 intrinsics are available at all */
    int sse42, crc32, pclmul, aes, gfni, fma; /* x86 extensions */
    int avx2, avx512bw, avx512vl, avx512bitalg;
} SnxIrFeatures;
extern SnxIrFeatures snx_ir_host;

typedef struct SnxIr {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef b;
    LLVMTargetMachineRef tm;
    int windows;
    const SnxIrFeatures* features;
    LLVMTypeRef t_void, t_i1, t_i8, t_i16, t_i32, t_i64, t_i128, t_ptr, t_guest;
    /* the function being emitted: its CPU* and the guest memory base (u8* const M=c->mem) */
    LLVMValueRef fn, c, M;
    LLVMValueRef helpers[SNX_IR_HELPER_COUNT];
    /* alias scopes: guest memory (through M) and the CPU structure never overlap */
    unsigned kind_scope, kind_noalias;
    LLVMValueRef scope_cpu, scope_guest;
    /* PLAIN instructions no emitter covers, and the first of them: snx_ir_object refuses a module with any (every
       pattern has an emitter, and the interpreter's step function is gone from the runtime library) */
    unsigned long fallbacks;
    SnxU32 fallback_word;
    SnxU64 fallback_pc;
    /* the runtime JIT's: nonzero when a target outside the codegen has an AOT module's function, called directly as
       snx_aot_<target> (the JIT's loader resolves it), 0 for CALLDYN. NULL in the AOT build */
    SnxU64 (*direct)(void* user, SnxU64 target);
    void* direct_user;
} SnxIr;

/* Once per process, before any other call; threads may then each own SnxIrs. 0, or -1 with the reason */
int snx_ir_init(char* error, size_t error_size);
/* opt is the -O letter: 0,1,2,3,s,z */
SnxIr* snx_ir_new(const char* name, char opt, char* error, size_t error_size);
/* the same for another target triple (NULL: the host), for tests of the linkers; CPU and features stay generic */
SnxIr* snx_ir_new_target(const char* name, char opt, const char* triple, char* error, size_t error_size);
void snx_ir_free(SnxIr* ir);
/* Optimizes the module as Clang does at the -O level and returns the host object file (malloc'd) */
int snx_ir_object(SnxIr* ir, char opt, unsigned char** data, size_t* size, char* error, size_t error_size);
/* The runtime library (rtlib.c) as a host object file (malloc'd): its bitcode, embedded when the recompiler was
   built, compiled for this machine like the generated code */
int snx_ir_rtlib_object(char opt, unsigned char** data, size_t* size, char* error, size_t error_size);
/* The same for a 32-bit module's library (armlib.c: the AArch32 blocks' helpers) */
int snx_ir_armlib_object(char opt, unsigned char** data, size_t* size, char* error, size_t error_size);
/* The module's IR as text (LLVMDisposeMessage) */
char* snx_ir_text(SnxIr* ir);

/* ---- functions (ir_codegen.c) ---- */
/* F_<address>, declared on first use, defined by snx_ir_emit */
LLVMValueRef snx_ir_guest_function(SnxIr* ir, SnxU64 address);
void snx_ir_emit(SnxIr* ir, SnxCodegen* codegen, const SnxFunction* function);
/* u64 name(CPU* c): one instruction at pc, as snx_a64_static_step writes it (tests and audit tools) */
LLVMValueRef snx_ir_static_step(SnxIr* ir, SnxU32 word, SnxU64 pc, const char* name);
/* Starts a function body: entry block, c and M */
void snx_ir_begin(SnxIr* ir, LLVMValueRef function);

/* ---- instructions (ir_a64.c, ir_simd.c, ir_fp.c) ---- */
/* The body of a PLAIN instruction (snx_a64_body with "c->rt->undef(c,pc,w)" for UNDEF, after snx_a64_fast_simd).
   0 when no emitter covers the pattern: the module then fails (SnxIr.fallbacks) */
int snx_ir_plain(SnxIr* ir, SnxU32 word, SnxU64 pc, const SnxA64Case* instruction);
/* The case's first operand of a flow instruction: the i1 condition of BC, the i64 target of BR/BLR/RET, the i32
   immediate of SVC/BRK */
LLVMValueRef snx_ir_flow_operand(SnxIr* ir, SnxU32 word, SnxU64 pc, const SnxA64Case* instruction);
int snx_ir_simd_plain(SnxIr* ir, SnxU32 word, SnxU64 pc, const SnxA64Pattern* pattern, size_t index);
int snx_ir_fp_plain(SnxIr* ir, SnxU32 word, SnxU64 pc, const SnxA64Pattern* pattern, size_t index);
/* snx_a64_fast_simd's shortcuts: 1 when it emitted the instruction */
int snx_ir_fast_simd(SnxIr* ir, SnxU32 word);
/* encodings of the SIMD patterns snx_simd handles that the backend also emits inline, with snx_simd's semantics
   (loads/stores, permutes, byte ops, immediates; AES and PMULL where CPUID shows the x86 extensions) */
int snx_ir_more_simd(SnxIr* ir, SnxU32 word);

/* ---- floating point fast paths (ir_fp.c) ----
   The host's IEEE arithmetic gives the soft-float helpers' exact result and flags when the rounding mode is
   to-nearest, the operands are normal or zero and the result is well inside the normal range or an exact zero: no
   denormal, overflow, underflow or NaN can occur, FZ/DN/AHP don't apply, and the only flag is IXC. A zero result is
   exact when a product's operand or a dividend is zero, or for any sum (floats only cancel exactly); nonzero
   operands that round to zero underflowed and take the helper. Zeros keep IEEE's signs, which are the
   architecture's in round to nearest. IXC is computed exactly (TwoSum for add/sub, an FMA residual for mul/div/sqrt
   where the host has FMA), or not needed: it is sticky, and once set an inexact result changes nothing. Everything
   else takes the helper call. All of this assumes the host thread never flushes denormals (x86 MXCSR.FTZ/DAZ, or
   FPCR.FZ on an AArch64 host): a sum of normals that is a denormal would read as an exact zero */
enum { SNX_IR_FP_ADD, SNX_IR_FP_SUB, SNX_IR_FP_MUL, SNX_IR_FP_DIV, SNX_IR_FP_SQRT, SNX_IR_FP_FMA };
typedef struct {
    LLVMValueRef value;   /* result bits: iN, or <lanes x iN> */
    LLVMValueRef ok;      /* i1: the fast path is exact */
    LLVMValueRef inexact; /* i1: IXC to set */
} SnxFpFast;
/* a, b, add: bits as iN (lanes 1) or <lanes x iN>; FMA computes add + a*b, SQRT reads a */
void ir_fp_fast(SnxIr* ir, unsigned op, unsigned width, unsigned lanes, LLVMValueRef a, LLVMValueRef b,
                LLVMValueRef add, SnxFpFast* out);
/* if (ok) { fast } else { slow }: returns the join block, positions at the fast block; ir_fp_else moves to the slow
   block (setting IXC from inexact on the way out of the fast one), ir_fp_join to the join */
LLVMBasicBlockRef ir_fp_branch(SnxIr* ir, LLVMValueRef ok, LLVMBasicBlockRef* slow);
void ir_fp_else(SnxIr* ir, LLVMBasicBlockRef join, LLVMBasicBlockRef slow, LLVMValueRef inexact);
void ir_fp_join(SnxIr* ir, LLVMBasicBlockRef join);
/* FMAX/FMIN (and the NM forms, which differ only for NaNs) with no NaN operand in any lane, and no denormal one
   when FPCR.FZ is set (flushing it sets IDC): FPMax/FPMin then return the larger or smaller operand unchanged, and
   for equal operands their bitwise AND (max: +0 beats -0) or OR (min: -0 beats +0), raising nothing. inexact is
   false */
void ir_fp_minmax_fast(SnxIr* ir, unsigned width, unsigned lanes, LLVMValueRef a, LLVMValueRef b, unsigned minimum,
                       SnxFpFast* out);
/* snx_fp_minmax(c, a, b, width, minimum, numeric) */
LLVMValueRef ir_fp_minmax(SnxIr* ir, LLVMValueRef a, LLVMValueRef b, unsigned width, unsigned minimum,
                          unsigned numeric);
/* the AdvSIMD compares' lane masks (simd.c's fcmpmask): a == b, a >= b, a > b, a <= b, a < b */
enum { SNX_IR_CMP_EQ, SNX_IR_CMP_GE, SNX_IR_CMP_GT, SNX_IR_CMP_LE, SNX_IR_CMP_LT };
/* all ones where the comparison holds, under the minmax guard: without a NaN no FPSR bit changes (only NaNs raise
   IOC, quiet ones for every predicate but EQ), and the host's ordered comparison is FPCompare's */
void ir_fp_compare_fast(SnxIr* ir, unsigned cmp, unsigned width, unsigned lanes, LLVMValueRef a, LLVMValueRef b,
                        SnxFpFast* out);
/* one lane through snx_fp_compare, signalling for all but EQ: all ones (iN) or zero */
LLVMValueRef ir_fp_compare_mask(SnxIr* ir, unsigned cmp, LLVMValueRef a, LLVMValueRef b, unsigned width);
/* SCVTF/UCVTF of integer lanes v (one iN, or lanes as wide as the float) scaled by 2^-fbits: exact when every magnitude fits the
   precision, or once IXC is set, in round to nearest (the host rounds the same way, and no conversion can overflow
   or be tiny). inexact is false */
void ir_fp_from_int_fast(SnxIr* ir, unsigned width, unsigned lanes, LLVMValueRef v, unsigned uns, unsigned fbits,
                         SnxFpFast* out);
/* FCVT[NPMZA][SU] (rm 0-4 as snx_fp_to_int's) of float lanes scaled by 2^fbits, to integer lanes of the integer's
   width: fast for finite values whose rounded value is in range (no IOC), denormals only without FZ (flushing sets
   IDC), inexact when rounding changed the value. 0 when the host can't round inline (no fast path: out unset) */
int ir_fp_to_int_fast(SnxIr* ir, unsigned width, unsigned lanes, LLVMValueRef bits, unsigned integer, unsigned uns,
                      unsigned rm, unsigned fbits, SnxFpFast* out);
/* FRECPE (rsqrt 0) or FRSQRTE (1): snx_fp_unary's 8-bit estimate computed inline for the lanes whose result is a
   normal number and raises nothing (a normal operand, positive for FRSQRTE, and for FRECPE an exponent below
   emax-2: from there the reciprocal is denormal, or flushed under FZ). inexact is false */
void ir_fp_estimate_fast(SnxIr* ir, unsigned rsqrt, unsigned width, unsigned lanes, LLVMValueRef bits, SnxFpFast* out);
/* FRECPS (rsqrt 0: 2 - a*b) or FRSQRTS (1: (3 - a*b) / 2), fused with one rounding, under ir_fp_fast's FMA guard
   (which also keeps out the infinity-times-zero lanes) */
void ir_fp_step_fast(SnxIr* ir, unsigned rsqrt, unsigned width, unsigned lanes, LLVMValueRef a, LLVMValueRef b,
                     SnxFpFast* out);
/* FCVTN (odd 0: FPCR's rounding, fast only in round to nearest) or FCVTXN (odd 1: round to odd, whatever the mode)
   of float lanes of from bits (64 or 32) to half that width, in integer arithmetic: fast for normal operands whose
   result is normal before rounding and finite after it (no flush, underflow, overflow, NaN or infinity, so FZ, DN
   and AHP don't apply). value: <lanes x i(from/2)> or i(from/2); inexact when bits were dropped */
void ir_fp_narrow_fast(SnxIr* ir, unsigned from, unsigned lanes, LLVMValueRef bits, unsigned odd, SnxFpFast* out);
/* one lane through snx_fp_convert from from bits to half that width, rounded per FPCR or (odd) to odd (result i64) */
LLVMValueRef ir_fp_narrow(SnxIr* ir, LLVMValueRef bits, unsigned from, unsigned odd);
/* one lane through the helpers: snx_fp_binary, snx_fp_unary, snx_fp_to_int and snx_fp_from_int (result i64) */
LLVMValueRef ir_fp_binary(SnxIr* ir, LLVMValueRef a, LLVMValueRef b, unsigned width, unsigned op);
LLVMValueRef ir_fp_unary(SnxIr* ir, LLVMValueRef bits, unsigned width, unsigned op);
LLVMValueRef ir_fp_to_int(SnxIr* ir, LLVMValueRef bits, unsigned fp, unsigned integer, unsigned uns, unsigned fbits,
                          unsigned rm);
LLVMValueRef ir_fp_from_int(SnxIr* ir, LLVMValueRef bits, unsigned integer, unsigned fp, unsigned uns, unsigned fbits);

/* ---- building blocks (ir.c) ---- */
LLVMValueRef ir_i8(SnxIr* ir, unsigned v);
LLVMValueRef ir_i16(SnxIr* ir, unsigned v);
LLVMValueRef ir_i32(SnxIr* ir, SnxU32 v);
LLVMValueRef ir_i64(SnxIr* ir, SnxU64 v);
/* integer conversions as C casts do them: to a narrower type truncates, to a wider one extends by sign or zero */
LLVMValueRef ir_cast(SnxIr* ir, LLVMValueRef v, LLVMTypeRef to, int is_signed);
LLVMValueRef ir_u32(SnxIr* ir, LLVMValueRef v);            /* (u32) */
LLVMValueRef ir_u64(SnxIr* ir, LLVMValueRef v);            /* (u64) from an unsigned value */
LLVMValueRef ir_s64(SnxIr* ir, LLVMValueRef v);            /* (u64)(i64) from a signed value */
LLVMValueRef ir_bool32(SnxIr* ir, LLVMValueRef condition); /* a C comparison's int */

/* pointer to the CPU field at byte offset */
LLVMValueRef ir_cpu(SnxIr* ir, size_t offset);
LLVMValueRef ir_load(SnxIr* ir, LLVMTypeRef type, LLVMValueRef pointer);
void ir_store(SnxIr* ir, LLVMValueRef value, LLVMValueRef pointer);
/* tags a load, store or atomic as an access to the CPU structure or to guest memory */
LLVMValueRef ir_tag_cpu(SnxIr* ir, LLVMValueRef access);
LLVMValueRef ir_tag_guest(SnxIr* ir, LLVMValueRef access);
LLVMValueRef ir_cpu_load(SnxIr* ir, LLVMTypeRef type, size_t offset);
void ir_cpu_store(SnxIr* ir, LLVMValueRef value, size_t offset);
size_t ir_off_x(unsigned n);
size_t ir_off_v(unsigned n);
size_t ir_off_pc(void);
size_t ir_off_cf(void);
size_t ir_off_excl_on(void);
size_t ir_off_fpcr(void);
size_t ir_off_mem(void);
LLVMValueRef ir_x(SnxIr* ir, unsigned n);                 /* c->x[n] */
void ir_set_x(SnxIr* ir, unsigned n, LLVMValueRef value); /* c->x[n]=(u64)value */
LLVMValueRef ir_rx(SnxIr* ir, unsigned n);                /* RX(n) */
void ir_setx(SnxIr* ir, unsigned n, LLVMValueRef value);  /* SETX(n,value) */
LLVMValueRef ir_vd(SnxIr* ir, unsigned n, unsigned lane); /* c->v[n].d[lane] */
void ir_set_vd(SnxIr* ir, unsigned n, unsigned lane, LLVMValueRef value);
/* c->v[n].<lane type>[index], element of 1<<size bytes */
LLVMValueRef ir_ve(SnxIr* ir, unsigned n, unsigned size, unsigned index);
void ir_set_ve(SnxIr* ir, unsigned n, unsigned size, unsigned index, LLVMValueRef value);
LLVMValueRef ir_pc(SnxIr* ir);
void ir_set_pc(SnxIr* ir, LLVMValueRef value);

/* guest memory: MP(a), RDn(a) as iN, WRn(a,v) storing (uN)v, RDQ/WRQ as 16-byte copies */
LLVMValueRef ir_mp(SnxIr* ir, LLVMValueRef address);
LLVMValueRef ir_rd(SnxIr* ir, LLVMValueRef address, unsigned bits);
void ir_wr(SnxIr* ir, LLVMValueRef address, unsigned bits, LLVMValueRef value);
/* n bytes as <n x i8> */
LLVMValueRef ir_rd_bytes(SnxIr* ir, LLVMValueRef address, unsigned bytes);
void ir_wr_bytes(SnxIr* ir, LLVMValueRef address, LLVMValueRef value);
void ir_rdq(SnxIr* ir, LLVMValueRef destination, LLVMValueRef address); /* RDQ(*destination, address) */
void ir_wrq(SnxIr* ir, LLVMValueRef address, LLVMValueRef source);      /* WRQ(address, *source) */
LLVMValueRef ir_ldacq(SnxIr* ir, LLVMValueRef address, unsigned bits);  /* LDACQ(uN,a) */
void ir_strel(SnxIr* ir, LLVMValueRef address, unsigned bits, LLVMValueRef value);

LLVMValueRef ir_helper(SnxIr* ir, int id);
LLVMValueRef ir_call(SnxIr* ir, LLVMValueRef function, LLVMValueRef* args, unsigned count);
LLVMValueRef ir_call_helper(SnxIr* ir, int id, LLVMValueRef* args, unsigned count);
/* an out-of-line function of the runtime library (cpu_helpers.c, rtlib.c), declared on first use */
LLVMValueRef ir_extern(SnxIr* ir, const char* name, LLVMTypeRef ret, LLVMTypeRef* params, unsigned count);
/* c->rt-><field>(args...) */
LLVMValueRef ir_rt_call(SnxIr* ir, size_t offset, LLVMTypeRef ret, LLVMTypeRef* params, LLVMValueRef* args,
                        unsigned count);
size_t ir_rt_svc(void);
size_t ir_rt_undef(void);
size_t ir_rt_note_hint(void);
size_t ir_rt_brk(void);
size_t ir_rt_sev(void);
size_t ir_rt_wfe(void);
size_t ir_rt_poll(void);
size_t ir_rt_miss(void);
size_t ir_off_poll(void);                         /* CPU.poll */
void ir_undef(SnxIr* ir, SnxU64 pc, SnxU32 word); /* c->rt->undef(c,pc,w) */
void ir_fence(SnxIr* ir);                         /* SNX_FENCE() */
void ir_yield(SnxIr* ir);                         /* YIELD() */
/* branch weights: the condition is expected false (UNLIKELY) */
void ir_unlikely(SnxIr* ir, LLVMValueRef branch);
/* a call to an LLVM intrinsic, overloaded on type (NULL: not overloaded) */
LLVMValueRef ir_intrinsic(SnxIr* ir, const char* name, LLVMTypeRef type, LLVMValueRef* args, unsigned count);
/* memory effects of a declared function: none (pure), or only memory reached through its pointer arguments */
void ir_pure(SnxIr* ir, LLVMValueRef function);
void ir_argmem(SnxIr* ir, LLVMValueRef function);
/* if (condition) { then() } else { otherwise() }: blocks positioned by the caller */
LLVMBasicBlockRef ir_block(SnxIr* ir, const char* name);
/* a stack slot in the entry block, as a C local whose address is taken */
LLVMValueRef ir_local(SnxIr* ir, LLVMTypeRef type);
LLVMTypeRef ir_v128(SnxIr* ir); /* [16 x i8] with V128's alignment */

#endif
