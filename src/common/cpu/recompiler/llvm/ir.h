/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The LLVM backend: A64 to LLVM IR through the LLVM-C API. ir_codegen.c walks a function's instructions as the
   generated decoder (a64_decoder.h) decodes them, and the emitters ported from QEMU's translate-a64.c (ir_int.c,
   ir_ldst.c, ir_fpu.c, ir_vec*.c) build each one's IR with the instruction word and pc constant. cpu.h's inline
   helpers become internal always-inline IR functions (ir.c); its out-of-line helpers are called in the runtime
   library (rtlib.c), which every module links. */
#ifndef SNX_RECOMPILER_LLVM_IR_H
#define SNX_RECOMPILER_LLVM_IR_H

#include "common/cpu/recompiler/codegen.h"
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

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
   x86 fields are only ever set when the target is x86-64 and the arm ones when it is AArch64, so an emitter gated
   on them is gated on the architecture */
typedef struct {
    unsigned x86 : 1; /* x86-64 target: x86 intrinsics are available at all */
    unsigned sse42 : 1, crc32 : 1, pclmul : 1, aes : 1, sha_ni : 1, gfni : 1, fma : 1, f16c : 1; /* x86 extensions */
    unsigned avx2 : 1, avx512bw : 1, avx512vl : 1, avx512bitalg : 1;
    unsigned aarch64 : 1;                /* AArch64 target: AArch64 intrinsics are available at all */
    unsigned arm_aes : 1, arm_sha2 : 1; /* AArch64 crypto extensions (PMULL comes with AES) */
    unsigned arm_crc : 1;                /* AArch64 CRC32 extension */
} SnxIrFeatures;
extern SnxIrFeatures snx_ir_host;

/* The cache belongs to the IR builder, not the guest CPU. One bit per GPR;
   changing blocks or observing an unknown write drops the whole snapshot. */
typedef struct {
    LLVMValueRef x[32];
    LLVMBasicBlockRef block;
    uint32_t valid;
} SnxIrRegisterCache;

/* Low scalar lanes in guest numeric order. A write to any vector register, an unknown call or a new block drops
   the snapshots. No architectural store is removed, and this does not cache guest memory. */
typedef struct {
    LLVMValueRef low[32][4];
    LLVMBasicBlockRef block;
    uint32_t valid[4];
} SnxIrVectorCache;

/* SSA flags are a per-block forwarding table, not a second CPU state.
   Every write still commits to the architectural structure immediately. When all four come from a compare (SUBS,
   ADDS of a constant, ANDS), origin keeps its operands: a condition on them is that compare (ir_flags_cond), which
   the host's own flags give, not the NZCV values */
enum { SNX_IR_FLAGS_SOME, SNX_IR_FLAGS_SUB, SNX_IR_FLAGS_ADD, SNX_IR_FLAGS_LOGIC };
typedef struct {
    LLVMValueRef value[4];
    LLVMBasicBlockRef block;
    unsigned valid;
    unsigned origin;          /* SNX_IR_FLAGS_*: what all four came from, in this block (SOME: anything) */
    LLVMValueRef left, right; /* SUB: left - right; ADD: left + right; LOGIC: left, the result */
} SnxIrFlags;

/* Block-local LLVM SSA snapshots, never guest architectural state. */
typedef struct {
    SnxIrRegisterCache registers;
    SnxIrVectorCache vectors;
    SnxIrFlags flags;
} SnxIrBlockState;

/* the AOT recompile's LLVM settings (nx/runtime/settings_llvm.h): the pipeline run in place of default<O?>, NULL for
   that one, and what is set on the pass builder's options and on the target machine, NULL for nothing. Without one
   (the runtime JIT's, the recompiler library's) LLVM keeps its own */
typedef struct SnxIrTuning {
    const char* pipeline;
    void (*builder)(LLVMPassBuilderOptionsRef options);
    void (*machine)(LLVMTargetMachineRef machine);
    /* every value the three above are made from, as text: what the unit cache keys objects with (unit_cache.c) */
    const char* text;
} SnxIrTuning;

/* Compile-time IR statistics. Counts describe emitted IR before LLVM's machine optimizations, not dynamic work. */
typedef struct {
    unsigned enabled : 1; /* the setting ir_stats (snx_ir_diagnostics) */
    SnxU64 functions, promoted_functions, call_sites, fp_calls, vector_calls, guest_calls, indirect_calls;
    SnxU64 sync_stores, sync_reloads, call_stores, store_bytes, reload_bytes;
    SnxU64 lookups, polls, stale_checks, return_checks;
} SnxIrStats;

typedef struct SnxIr {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef b;
    LLVMTargetMachineRef tm;
    int windows;
    int promote; /* ir_promote_registers at this level (O1 and above): at O0 SROA and mem2reg leave its slots on
                    the stack */
    const SnxIrFeatures* features;
    SnxIrStats stats;
    SnxU64 dump_pc, dump_instruction_pc;
    unsigned dump_function : 1, dump_active : 1; /* the setting ir_dump_pc's host diagnostics, never generated code */
    LLVMTypeRef t_void, t_i1, t_i8, t_i16, t_i32, t_i64, t_i128, t_ptr, t_guest;
    /* the function being emitted: its CPU* and the guest memory base (u8* const M=c->mem) */
    LLVMValueRef fn, c, M;
    LLVMValueRef helpers[SNX_IR_HELPER_COUNT];
    SnxIrBlockState state;
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
    /* the AOT build's (snx_ir_stale_checks): snx_stale, the module's byte per function, which each function reads
       first and, once the runtime has set it, runs its entry through dispatch (SNX_ABI 14). NULL elsewhere */
    LLVMValueRef stale;
    size_t stale_index; /* the function being emitted's byte in it */
    const SnxIrTuning* tuning; /* snx_ir_tune's, NULL for LLVM's own */
    /* the target stores values big-endian (ppc64): guest memory and the vector registers hold the guest's
       little-endian bytes, so the accesses that read them as numbers swap (ir_rd, ir_vd and their siblings) */
    int big_endian;
    /* PowerPC64's ABI is ELFv2, else ELFv1 (new_ir): what a function pointer is, where the TOC is saved */
    int elfv2;
    /* the guest's features (snx_a64_features) for the function being emitted: what its system registers keep */
    SnxU64 guest_features;
} SnxIr;

void ir_state_invalidate(SnxIr* ir);
void ir_vectors_invalidate(SnxIr* ir);
void ir_registers_invalidate(SnxIr* ir);
void ir_flags_invalidate(SnxIr* ir);
LLVMValueRef ir_flag_read(SnxIr* ir, unsigned index);
LLVMValueRef ir_flags_adds(SnxIr* ir, unsigned bits, LLVMValueRef a, LLVMValueRef b, LLVMValueRef cin);
LLVMValueRef ir_flags_subs(SnxIr* ir, unsigned bits, LLVMValueRef a, LLVMValueRef b);
void ir_flags_logic(SnxIr* ir, LLVMValueRef value, unsigned bits);
void ir_flags_set_nzcv(SnxIr* ir, LLVMValueRef nzcv);
LLVMValueRef ir_flags_cond(SnxIr* ir, unsigned cond);

/* Once per process, before any other call; threads may then each own SnxIrs. 0, or -1 with the reason */
int snx_ir_init(char* error, size_t error_size);
/* LLVM's SLP vectorizer on or off for every module this process makes (the setting slp_vectorizer): an option of
   LLVM's command line, so once per process, from one thread, before any module is made */
void snx_ir_slp_vectorizer(int on);
/* whether it runs: LLVM's own, on, until snx_ir_slp_vectorizer. The caches' keys carry it */
int snx_ir_slp(void);
/* Whether the generated code keeps FPSR's cumulative flags (IOC, DZC, OFC, UFC, IXC and QC) as the console raises
   them (the setting fpsr_flags, on by default): off, the FP fast paths give the console's results and nothing else,
   the host's IEEE arithmetic wherever the result is not a NaN, the flags only where the helpers run, and saturating
   instructions leave QC alone. Once per process, before
   any module is made; the caches' keys carry it */
void snx_ir_fpsr_flags(int exact);
int snx_ir_fpsr_exact(void);
/* the settings ir_stats (each code unit's static IR counts in a recompile's log) and ir_dump_pc (hex: the function
   there printed, its IR before and after the promotion), for the SnxIr made after */
void snx_ir_diagnostics(int stats, const char* dump_pc);
/* Whether the guest's loads and stores are volatile (the setting volatile_memory, off by default): on, LLVM keeps
   each where the guest has it (ir_state.c's ir_rd). Once per process, before any module is made; the caches' keys
   carry it */
void snx_ir_volatile_memory(int on);
int snx_ir_volatile(void);
/* opt is the -O letter: 0,1,2,3,s,z */
SnxIr* snx_ir_new(const char* name, char opt, char* error, size_t error_size);
/* the same for code the JIT loader loads into this process (code_loader.h): the host's target, CPU and calling
   convention, its objects in the format the loader reads, ELF on Darwin, whose own is Mach-O */
SnxIr* snx_ir_new_jit(const char* name, char opt, char* error, size_t error_size);
/* the same for another target triple (NULL: the host), for tests of the linkers; CPU and features stay generic */
SnxIr* snx_ir_new_target(const char* name, char opt, const char* triple, char* error, size_t error_size);
void snx_ir_free(SnxIr* ir);
/* the settings the SnxIr's objects are made with, kept (NULL: LLVM's own): the target machine's set now, the pipeline
   and the pass builder's at each snx_ir_object */
void snx_ir_tune(SnxIr* ir, const SnxIrTuning* tuning);
/* the AOT module's functions emitted from here check their stale byte in snx_stale, which the module descriptor
   defines (SNX_IR_STALE) */
#define SNX_IR_STALE "snx_stale"
void snx_ir_stale_checks(SnxIr* ir);
/* Optimizes the module as Clang does at the -O level and returns the host object file (malloc'd) */
int snx_ir_object(SnxIr* ir, char opt, unsigned char** data, size_t* size, char* error, size_t error_size);
/* What makes an object of a module besides its IR, LLVM, the target machine's triple, CPU and features, the -O letter
   and the tuning: new_ir's other target machine settings and what snx_ir_module_object does around LLVM's passes. The
   unit cache's key holds it (unit_cache.c), so a change there that changes the object of the same IR bumps it */
#define SNX_IR_OBJECT_VERSION 3
/* The same for another module of the SnxIr's context, made with its target and data layout (the A32 executor's
   blocks, common/cpu/arm/exec.c): verified, optimized and compiled with its target machine. The module stays the
   caller's */
int snx_ir_module_object(SnxIr* ir, LLVMModuleRef module, char opt, unsigned char** data, size_t* size, char* error,
                         size_t error_size);
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
/* u64 name(CPU* c): one instruction at pc, for a guest with these A64 features (0: the Switch's; tests and audit
   tools) */
LLVMValueRef snx_ir_static_step(SnxIr* ir, SnxU32 word, SnxU64 pc, SnxU64 features, const char* name);
/* the emitted function's accesses to the registers in the CPU structure onto locals, which the CPU structure gets
   back around every call and return (ir_promote.c). snx_ir_emit runs it at O1 and above */
void ir_promote_registers(SnxIr* ir, LLVMValueRef fn);
/* Accumulates one emitted function's call counts when stats.enabled is set. */
void ir_stats_function(SnxIr* ir, LLVMValueRef fn);
/* Starts a function body: entry block, c and M */
void snx_ir_begin(SnxIr* ir, LLVMValueRef function);

/* ---- building blocks (ir.c) ---- */
LLVMValueRef ir_i8(SnxIr* ir, unsigned v);
LLVMValueRef ir_i32(SnxIr* ir, SnxU32 v);
LLVMValueRef ir_i64(SnxIr* ir, SnxU64 v);
/* integer conversions as C casts do them: to a narrower type truncates, to a wider one extends by sign or zero */
LLVMValueRef ir_cast(SnxIr* ir, LLVMValueRef v, LLVMTypeRef to, int is_signed);
LLVMValueRef ir_u32(SnxIr* ir, LLVMValueRef v);            /* (u32) */
LLVMValueRef ir_u64(SnxIr* ir, LLVMValueRef v);            /* (u64) from an unsigned value */
LLVMValueRef ir_bool32(SnxIr* ir, LLVMValueRef condition); /* a C comparison's int */

/* pointer to the CPU field at byte offset */
LLVMValueRef ir_cpu(SnxIr* ir, size_t offset);
LLVMValueRef ir_load(SnxIr* ir, LLVMTypeRef type, LLVMValueRef pointer);
void ir_store(SnxIr* ir, LLVMValueRef value, LLVMValueRef pointer);
/* tags a load, store or atomic as an access to the CPU structure or to guest memory */
LLVMValueRef ir_tag_cpu(SnxIr* ir, LLVMValueRef access);
LLVMValueRef ir_tag_guest(SnxIr* ir, LLVMValueRef access);
LLVMValueRef ir_cpu_load(SnxIr* ir, LLVMTypeRef type, size_t offset);
/* iN */
static inline LLVMTypeRef ir_int_type(SnxIr* ir, unsigned bits) { return LLVMIntTypeInContext(ir->ctx, bits); }
void ir_cpu_store(SnxIr* ir, LLVMValueRef value, size_t offset);
size_t ir_off_x(unsigned n);
size_t ir_off_v(unsigned n);
size_t ir_off_excl_on(void);
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

/* A number (or a vector of them, integer or floating) read from or written to the guest's little-endian bytes, in
   guest memory or a vector register: byte-swapped on a big-endian target, as it is; the same both ways */
LLVMValueRef ir_guest_order(SnxIr* ir, LLVMValueRef value);
/* guest memory: MP(a), an N-bit read as iN, an N-bit write storing (uN)v */
LLVMValueRef ir_mp(SnxIr* ir, LLVMValueRef address);
LLVMValueRef ir_rd(SnxIr* ir, LLVMValueRef address, unsigned bits);
void ir_wr(SnxIr* ir, LLVMValueRef address, unsigned bits, LLVMValueRef value);
/* n bytes as <n x i8> */
LLVMValueRef ir_rd_bytes(SnxIr* ir, LLVMValueRef address, unsigned bytes);
void ir_wr_bytes(SnxIr* ir, LLVMValueRef address, LLVMValueRef value);
LLVMValueRef ir_ldacq(SnxIr* ir, LLVMValueRef address, unsigned bits);  /* LDACQ(uN,a) */
void ir_strel(SnxIr* ir, LLVMValueRef address, unsigned bits, LLVMValueRef value);

LLVMValueRef ir_helper(SnxIr* ir, int id);
LLVMValueRef ir_call(SnxIr* ir, LLVMValueRef function, LLVMValueRef* args, unsigned count);
LLVMValueRef ir_call_helper(SnxIr* ir, int id, LLVMValueRef* args, unsigned count);
/* an out-of-line function of the runtime library (cpu_helpers.c, rtlib.c), declared on first use */
LLVMValueRef ir_extern(SnxIr* ir, const char* name, LLVMTypeRef ret, LLVMTypeRef* params, unsigned count);
/* the enum attribute name (no value) at index of function */
void ir_attribute(SnxIr* ir, LLVMValueRef function, LLVMAttributeIndex index, const char* name);
/* c->rt-><field>(args...) */
LLVMValueRef ir_rt_call(SnxIr* ir, size_t offset, LLVMTypeRef ret, LLVMTypeRef* params, LLVMValueRef* args,
                        unsigned count);
/* c->rt-><table field>[entry](args...) */
LLVMValueRef ir_rt_table_call(SnxIr* ir, size_t offset, unsigned entry, LLVMTypeRef ret, LLVMTypeRef* params,
                              LLVMValueRef* args, unsigned count);
size_t ir_rt_svc(void);
size_t ir_rt_svcs(void);
size_t ir_rt_undef(void);
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
/* Optional offline branch profile: entries are keyed by guest PC and exact word.
   No profile means the existing LLVM branch prediction stays unchanged. */
void ir_profile_branch(SnxIr* ir, LLVMValueRef branch, SnxU64 pc, SnxU32 word);
void ir_unlikely(SnxIr* ir, LLVMValueRef branch);
void ir_likely(SnxIr* ir, LLVMValueRef branch);
/* a call to an LLVM intrinsic, overloaded on type (NULL: not overloaded) */
LLVMValueRef ir_intrinsic(SnxIr* ir, const char* name, LLVMTypeRef type, LLVMValueRef* args, unsigned count);
/* memory effects of a declared function: none (pure), or only memory reached through its pointer arguments */
void ir_pure(SnxIr* ir, LLVMValueRef function);
void ir_argmem(SnxIr* ir, LLVMValueRef function);
/* a helper that takes c but touches none of its registers (X, SP, NZCV, V), only FPCR, FPSR or the exclusive
   monitor: ir_promote_registers needs the CPU structure to hold none of them around its calls */
void ir_noregs(SnxIr* ir, LLVMValueRef function);
/* if (condition) { then() } else { otherwise() }: blocks positioned by the caller */
LLVMBasicBlockRef ir_block(SnxIr* ir, const char* name);
/* a stack slot in the entry block, as a C local whose address is taken */
LLVMValueRef ir_local(SnxIr* ir, LLVMTypeRef type);

#endif
