/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The libgccjit backend (--backend=gcc), for hosts LLVM doesn't generate code for (DEC Alpha EV67); elsewhere the
   LLVM backend is the one to use. It mirrors the LLVM backend: the control flow of ir_codegen.c (labels, the D_
   dispatch, required tail calls, CALLDYN, BL's return check, polls on backward branches) and instruction bodies that
   build the C of a64_templates.inc for a constant word (jit_a64.c, jit_fp.c, jit_simd.c). Every pattern has an
   emitter: an instruction without one fails its unit (snx_jit_error).
   Nothing here is specific to a host: SIMD is written lane by lane, as the templates' C is, for GCC's vectorizer, and
   host instructions only come from GCC's code generator with -march=native (build == host == target). */
#ifndef SNX_RECOMPILER_GCCJIT_JIT_H
#define SNX_RECOMPILER_GCCJIT_JIT_H

#include "common/cpu/recompiler/codegen.h"
#include <libgccjit.h>

typedef struct {
    const char* name;
    gcc_jit_function* fn;
} SnxJitExtern;

typedef struct SnxJit {
    gcc_jit_context* ctx;
    gcc_jit_type *t_void, *t_ptr, *t_char_ptr, *t_bool, *t_int, *t_guest, *t_guest_ptr;
    gcc_jit_type *t_u8, *t_u16, *t_u32, *t_u64, *t_u128, *t_s8, *t_s16, *t_s32, *t_s64, *t_s128, *t_f32, *t_f64;
    gcc_jit_function* lookup; /* snx_lookup */
    /* out-of-line functions of the runtime library and GCC builtins, declared on first use */
    SnxJitExtern externs[64];
    size_t extern_count;
    /* F_<address> declared in this context: defined here (exported) or elsewhere (imported) */
    SnxU64* addresses;
    gcc_jit_function** functions;
    size_t count, capacity;
    /* the host's fused multiply-add and round-to-integral instructions, as -march=native resolves them: the FP fast
       paths need them to be instructions, never calls into a C library the module doesn't link */
    int fma, round;
    /* the function being emitted: CPU* c (restrict) and the guest memory base M=c->mem */
    gcc_jit_function* fn;
    gcc_jit_rvalue *c, *M;
    gcc_jit_block* block; /* NULL after a terminator, until the next label */
    unsigned long names;
    /* PLAIN instructions no emitter covers, the first of them, and the error that names it: snx_jit_error reports
       it, so the unit fails (every pattern has an emitter, and the interpreter's step function is gone) */
    unsigned long fallbacks;
    SnxU32 fallback_word;
    SnxU64 fallback_pc;
    char fallback_error[160];
} SnxJit;

/* opt: the -O letter */
SnxJit* snx_jit_new(char opt);
void snx_jit_free(SnxJit* jit);
/* NULL, or the first instruction without an emitter, or the context's first error */
const char* snx_jit_error(SnxJit* jit);

/* F_<address>: exported (defined in this context) when define, imported otherwise; declare defined ones first */
gcc_jit_function* snx_jit_guest_function(SnxJit* jit, SnxU64 address, int define);
void snx_jit_emit(SnxJit* jit, SnxCodegen* codegen, const SnxFunction* function);
/* u64 name(CPU* c): one instruction, as snx_a64_static_step returns (tests) */
gcc_jit_function* snx_jit_static_step(SnxJit* jit, SnxU32 word, SnxU64 pc, const char* name);
/* asks GCC's driver what -march=native is here, once: snx_jit_new does on first use */
void snx_jit_host(void);

/* ---- code units (jit_units.c) ----
   Unit i is the functions [bounds[i], bounds[i + 1]) in a context of its own, compiled to the object file objects[i].
   libgccjit compiles one context at a time per process (GCC's state is global, behind one lock), so units compile
   jobs at a time in processes of their own; with one job, or on Windows, one after another in this one */
typedef struct SnxJitUnits {
    SnxCodegen* codegen;
    const size_t* bounds;
    char* const* objects;
    size_t count;
    char opt;            /* -O letter */
    unsigned jobs;       /* 0: one per CPU */
    const char* keep_ir; /* when set, each unit's libgccjit dump is saved there as code_NNN.c */
    /* after each unit, as they finish; finished counts them */
    void (*done)(const struct SnxJitUnits* units, size_t unit, size_t finished, double seconds);
    void* user;
    unsigned long fallbacks; /* every unit's */
    char error[1024];
} SnxJitUnits;
/* 0, or -1 with units->error naming the first unit that failed, once the others running have ended */
int snx_jit_compile_units(SnxJitUnits* units);
/* gcc_jit_context_compile_to_file and gcc_jit_context_compile under the lock units' processes are forked under: a
   process forked while another thread is inside libgccjit would start with GCC's lock held. NULL or the first error */
const char* snx_jit_compile_to_file(SnxJit* jit, enum gcc_jit_output_kind kind, const char* path);
gcc_jit_result* snx_jit_compile(SnxJit* jit);

/* ---- instructions (jit_a64.c, jit_fp.c, jit_simd.c), as ir.h's ---- */
/* The body of a PLAIN instruction; 0 when no emitter covers the pattern: the unit then fails (SnxJit.fallbacks) */
int snx_jit_plain(SnxJit* jit, SnxU32 word, SnxU64 pc, const SnxA64Case* instruction);
/* the bool condition of BC, the u64 target of BR/BLR/RET, the u32 immediate of SVC/BRK */
gcc_jit_rvalue* snx_jit_flow_operand(SnxJit* jit, SnxU32 word, SnxU64 pc, const SnxA64Case* instruction);
int snx_jit_fp_plain(SnxJit* jit, SnxU32 word, SnxU64 pc, const SnxA64Pattern* pattern, size_t index);
int snx_jit_simd_plain(SnxJit* jit, SnxU32 word, SnxU64 pc, const SnxA64Pattern* pattern, size_t index);
int snx_jit_fast_simd(SnxJit* jit, SnxU32 word);
int snx_jit_more_simd(SnxJit* jit, SnxU32 word);

/* ---- floating point fast paths (jit_fp.c): ir_fp_fast's, a lane at a time ---- */
enum { SNX_JIT_FP_ADD, SNX_JIT_FP_SUB, SNX_JIT_FP_MUL, SNX_JIT_FP_DIV, SNX_JIT_FP_SQRT, SNX_JIT_FP_FMA };
typedef struct {
    gcc_jit_rvalue* value[4]; /* result bits per lane */
    gcc_jit_rvalue* ok;       /* bool: the fast path is exact in every lane */
    gcc_jit_rvalue* inexact;  /* bool: IXC to set; NULL: never */
} SnxJitFpFast;
/* a, b, add: lane bits (uN); FMA computes add + a*b, SQRT reads a */
void jit_fp_fast(SnxJit* jit, unsigned op, unsigned width, unsigned lanes, gcc_jit_rvalue* const* a,
                 gcc_jit_rvalue* const* b, gcc_jit_rvalue* const* add, SnxJitFpFast* out);
/* if (ok) { fast } else { slow }: returns the join block, enters the fast one; jit_fp_else enters the slow block
   (setting IXC from inexact on the way out of the fast one), jit_fp_join the join */
gcc_jit_block* jit_fp_branch(SnxJit* jit, gcc_jit_rvalue* ok, gcc_jit_block** slow);
void jit_fp_else(SnxJit* jit, gcc_jit_block* join, gcc_jit_block* slow, gcc_jit_rvalue* inexact);
void jit_fp_join(SnxJit* jit, gcc_jit_block* join);
/* snx_fp_binary/snx_fp_fma(c, ...) */
gcc_jit_rvalue* jit_fp_binary(SnxJit* jit, gcc_jit_rvalue* a, gcc_jit_rvalue* b, unsigned width, unsigned op);
gcc_jit_rvalue* jit_fp_fma(SnxJit* jit, gcc_jit_rvalue* add, gcc_jit_rvalue* a, gcc_jit_rvalue* b, unsigned width);
/* ir_fp_minmax_fast's FMAX/FMIN without a NaN (or a denormal under FZ), a lane at a time; inexact is NULL */
void jit_fp_minmax_fast(SnxJit* jit, unsigned width, unsigned lanes, gcc_jit_rvalue* const* a, gcc_jit_rvalue* const* b,
                        unsigned minimum, SnxJitFpFast* out);
/* snx_fp_minmax(c, a, b, width, minimum, numeric) */
gcc_jit_rvalue* jit_fp_minmax(SnxJit* jit, gcc_jit_rvalue* a, gcc_jit_rvalue* b, unsigned width, unsigned minimum,
                              unsigned numeric);
/* the AdvSIMD compares' lane masks (simd.c's fcmpmask): a == b, a >= b, a > b, a <= b, a < b */
enum { SNX_JIT_CMP_EQ, SNX_JIT_CMP_GE, SNX_JIT_CMP_GT, SNX_JIT_CMP_LE, SNX_JIT_CMP_LT };
/* ir_fp_compare_fast's masks under the same guard; inexact is NULL */
void jit_fp_compare_fast(SnxJit* jit, unsigned cmp, unsigned width, unsigned lanes, gcc_jit_rvalue* const* a,
                         gcc_jit_rvalue* const* b, SnxJitFpFast* out);
/* one lane through snx_fp_compare, signalling for all but EQ: all ones (uN) or zero */
gcc_jit_rvalue* jit_fp_compare_mask(SnxJit* jit, unsigned cmp, gcc_jit_rvalue* a, gcc_jit_rvalue* b, unsigned width);
/* ir_fp_from_int_fast's SCVTF/UCVTF of integers (the low integer bits of each v) scaled by 2^-fbits; inexact is
   NULL */
void jit_fp_from_int_fast(SnxJit* jit, unsigned width, unsigned lanes, gcc_jit_rvalue* const* v, unsigned integer,
                          unsigned uns, unsigned fbits, SnxJitFpFast* out);
/* ir_fp_to_int_fast's FCVT[NPMZA][SU], results uN of the integer's width; 0 when the host can't round inline */
int jit_fp_to_int_fast(SnxJit* jit, unsigned width, unsigned lanes, gcc_jit_rvalue* const* bits, unsigned integer,
                       unsigned uns, unsigned rm, unsigned fbits, SnxJitFpFast* out);
/* ir_fp_estimate_fast's FRECPE (rsqrt 0) or FRSQRTE (1) estimate; inexact is NULL */
void jit_fp_estimate_fast(SnxJit* jit, unsigned rsqrt, unsigned width, unsigned lanes, gcc_jit_rvalue* const* bits,
                          SnxJitFpFast* out);
/* ir_fp_step_fast's FRECPS (rsqrt 0) or FRSQRTS (1) */
void jit_fp_step_fast(SnxJit* jit, unsigned rsqrt, unsigned width, unsigned lanes, gcc_jit_rvalue* const* a,
                      gcc_jit_rvalue* const* b, SnxJitFpFast* out);
/* ir_fp_narrow_fast's FCVTN (odd 0) or FCVTXN (odd 1) of float lanes of from bits to half that width (values
   u(from/2)) */
void jit_fp_narrow_fast(SnxJit* jit, unsigned from, unsigned lanes, gcc_jit_rvalue* const* bits, unsigned odd,
                        SnxJitFpFast* out);
/* one lane through snx_fp_convert from from bits to half that width, rounded per FPCR or (odd) to odd (result u64) */
gcc_jit_rvalue* jit_fp_narrow(SnxJit* jit, gcc_jit_rvalue* bits, unsigned from, unsigned odd);
/* one lane through snx_fp_unary, snx_fp_to_int and snx_fp_from_int (result u64) */
gcc_jit_rvalue* jit_fp_unary(SnxJit* jit, gcc_jit_rvalue* bits, unsigned width, unsigned op);
gcc_jit_rvalue* jit_fp_to_int(SnxJit* jit, gcc_jit_rvalue* bits, unsigned fp, unsigned integer, unsigned uns,
                              unsigned fbits, unsigned rm);
gcc_jit_rvalue* jit_fp_from_int(SnxJit* jit, gcc_jit_rvalue* bits, unsigned integer, unsigned fp, unsigned uns,
                                unsigned fbits);

/* ---- building blocks (jit.c) ----
   Values follow the C's evaluation order: anything that reads memory or calls is assigned to a fresh local where it
   is built (jit_val), so later stores can't change it; arithmetic on such values stays an expression. Operands of
   an operation take the first one's type */
gcc_jit_rvalue* jit_k(SnxJit* jit, gcc_jit_type* type, SnxU64 v);
gcc_jit_rvalue* jit_u64(SnxJit* jit, SnxU64 v);
gcc_jit_rvalue* jit_u32(SnxJit* jit, SnxU32 v);
gcc_jit_rvalue* jit_int(SnxJit* jit, int v);
gcc_jit_rvalue* jit_true(SnxJit* jit);
gcc_jit_rvalue* jit_false(SnxJit* jit);
gcc_jit_type* jit_uint(SnxJit* jit, unsigned bits); /* u8..u128 */
gcc_jit_type* jit_sint(SnxJit* jit, unsigned bits); /* i8..i128 */
gcc_jit_rvalue* jit_cast(SnxJit* jit, gcc_jit_rvalue* v, gcc_jit_type* type);
gcc_jit_rvalue* jit_bitcast(SnxJit* jit, gcc_jit_rvalue* v, gcc_jit_type* type);
gcc_jit_rvalue* jit_val(SnxJit* jit, gcc_jit_rvalue* v);
gcc_jit_lvalue* jit_local(SnxJit* jit, gcc_jit_type* type);
void jit_assign(SnxJit* jit, gcc_jit_lvalue* lvalue, gcc_jit_rvalue* value); /* casts to the lvalue's type */
gcc_jit_rvalue* jit_op(SnxJit* jit, enum gcc_jit_binary_op op, gcc_jit_rvalue* a, gcc_jit_rvalue* b);
gcc_jit_rvalue* jit_unary(SnxJit* jit, enum gcc_jit_unary_op op, gcc_jit_rvalue* a);
gcc_jit_rvalue* jit_cmp(SnxJit* jit, enum gcc_jit_comparison op, gcc_jit_rvalue* a, gcc_jit_rvalue* b);
gcc_jit_rvalue* jit_land(SnxJit* jit, gcc_jit_rvalue* a, gcc_jit_rvalue* b);
gcc_jit_rvalue* jit_lor(SnxJit* jit, gcc_jit_rvalue* a, gcc_jit_rvalue* b);
gcc_jit_rvalue* jit_lnot(SnxJit* jit, gcc_jit_rvalue* a);
/* cond ? a : b for integers, without a branch: b ^ ((a ^ b) & -(T)cond) */
gcc_jit_rvalue* jit_sel(SnxJit* jit, gcc_jit_rvalue* cond, gcc_jit_rvalue* a, gcc_jit_rvalue* b);
/* __builtin_expect(cond, likely): the branch weights of LIKELY/UNLIKELY */
gcc_jit_rvalue* jit_expect(SnxJit* jit, gcc_jit_rvalue* cond, int likely);

/* the CPU: *(type*)((char*)c + offset) */
gcc_jit_lvalue* jit_at(SnxJit* jit, gcc_jit_rvalue* base, size_t offset, gcc_jit_type* type);
gcc_jit_lvalue* jit_cpu(SnxJit* jit, size_t offset, gcc_jit_type* type);
gcc_jit_rvalue* jit_ld(SnxJit* jit, size_t offset, gcc_jit_type* type);
void jit_st(SnxJit* jit, size_t offset, gcc_jit_type* type, gcc_jit_rvalue* value);
size_t jit_off_x(unsigned n);
size_t jit_off_v(unsigned n);
gcc_jit_rvalue* jit_x(SnxJit* jit, unsigned n);                 /* c->x[n] */
gcc_jit_rvalue* jit_rx(SnxJit* jit, unsigned n);                /* RX(n) */
void jit_set_x(SnxJit* jit, unsigned n, gcc_jit_rvalue* value); /* c->x[n]=(u64)value */
void jit_setx(SnxJit* jit, unsigned n, gcc_jit_rvalue* value);  /* SETX(n,value) */
gcc_jit_rvalue* jit_vd(SnxJit* jit, unsigned n, unsigned lane); /* c->v[n].d[lane] */
void jit_set_vd(SnxJit* jit, unsigned n, unsigned lane, gcc_jit_rvalue* value);
/* c->v[n].<lane>[index], elements of 1<<size bytes, unsigned */
gcc_jit_rvalue* jit_ve(SnxJit* jit, unsigned n, unsigned size, unsigned index);
void jit_set_ve(SnxJit* jit, unsigned n, unsigned size, unsigned index, gcc_jit_rvalue* value);

/* guest memory: RDn(a) as uN, WRn(a,v) storing (uN)v, exactly as written (jit.c says how); LDACQ/STREL */
gcc_jit_rvalue* jit_rd(SnxJit* jit, gcc_jit_rvalue* address, unsigned bits);
void jit_wr(SnxJit* jit, gcc_jit_rvalue* address, unsigned bits, gcc_jit_rvalue* value);
gcc_jit_rvalue* jit_ldacq(SnxJit* jit, gcc_jit_rvalue* address, unsigned bits);
void jit_strel(SnxJit* jit, gcc_jit_rvalue* address, unsigned bits, gcc_jit_rvalue* value);
/* snx_cas<bits>(MP(a),o,n): __atomic_compare_exchange_n, strong, seq_cst; a u32 1 on success */
gcc_jit_rvalue* jit_cas(SnxJit* jit, gcc_jit_rvalue* address, unsigned bits, gcc_jit_rvalue* expected,
                        gcc_jit_rvalue* value);
gcc_jit_rvalue* jit_mp(SnxJit* jit, gcc_jit_rvalue* address); /* MP(a) as void* */

/* calls: arguments are cast to the parameters' types; a non-void result is a value (jit_val) */
gcc_jit_function* jit_extern(SnxJit* jit, const char* name, gcc_jit_type* ret, int count, gcc_jit_type** params);
gcc_jit_function* jit_builtin(SnxJit* jit, const char* name);
gcc_jit_rvalue* jit_call(SnxJit* jit, gcc_jit_function* function, int count, gcc_jit_rvalue** args);
/* c->rt-><field at offset>(args...) */
gcc_jit_rvalue* jit_rt_call(SnxJit* jit, size_t offset, gcc_jit_type* ret, int count, gcc_jit_type** params,
                            gcc_jit_rvalue** args);
size_t jit_rt_sev(void);
size_t jit_rt_wfe(void);
void jit_undef(SnxJit* jit, SnxU64 pc, SnxU32 word); /* c->rt->undef(c,pc,w) */
void jit_fence(SnxJit* jit);                         /* SNX_FENCE() */
void jit_barrier(SnxJit* jit);                       /* CBAR(): asm volatile("" ::: "memory") */

/* blocks: jit_block makes one, jit_goto/jit_branch end the current one, jit_enter continues in another */
gcc_jit_block* jit_block(SnxJit* jit);
void jit_goto(SnxJit* jit, gcc_jit_block* target);
void jit_branch(SnxJit* jit, gcc_jit_rvalue* cond, gcc_jit_block* yes, gcc_jit_block* no);
void jit_enter(SnxJit* jit, gcc_jit_block* block);

#endif
