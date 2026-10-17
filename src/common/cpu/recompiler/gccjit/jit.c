/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/gccjit/jit.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- context ---- */

/* -march=native is resolved by GCC's driver, which libgccjit's compiler proper doesn't run through: the CPU by name
   (build == host == target), from libgccjit itself where it can tell (GCC 15), else from a GCC on PATH, with what it
   supports of the instructions the FP fast paths need. Those are used only with the name passed: without it the code
   is the target's generic one, which calls libm for them, and a module links no libm. Generic without either */
typedef struct {
    char march[96];
    int fma, round;
} HostTarget;

/* "  -mfma   [enabled]": an option of --help=target's listing, enabled */
static int enabled(const char* listing, const char* option) {
    size_t n = strlen(option);
    const char* p;
    for (p = listing; (p = strstr(p, option)) != NULL; p += n)
        if ((p == listing || p[-1] == ' ' || p[-1] == '\t') && (p[n] == ' ' || p[n] == '\t')) {
            const char* end = strchr(p, '\n');
            const char* on = strstr(p, "[enabled]");
            return on && (!end || on < end);
        }
    return 0;
}

static const HostTarget* host_target(void) {
    static HostTarget host;
    static int done;
    if (!done) {
        const char* args[5] = {"gcc", "-march=native", "-Q", "--help=target", NULL};
        char *gcc = NULL, *output = NULL, *p;
        done = 1;
#if defined(LIBGCCJIT_HAVE_TARGET_INFO_API) && (defined(__x86_64__) || defined(__i386__))
        {
            gcc_jit_context* ctx = gcc_jit_context_acquire();
            gcc_jit_target_info* info = gcc_jit_context_get_target_info(ctx);
            const char* arch = gcc_jit_target_info_arch(info);
            if (arch && *arch && strlen(arch) < sizeof(host.march) - 8) sprintf(host.march, "-march=%s", arch);
            gcc_jit_target_info_release(info);
            gcc_jit_context_release(ctx);
            if (host.march[0]) { /* what the CPU supports shows only in a context told its name */
                ctx = gcc_jit_context_acquire();
                gcc_jit_context_add_command_line_option(ctx, host.march);
                info = gcc_jit_context_get_target_info(ctx);
                host.fma = gcc_jit_target_info_cpu_supports(info, "fma");
                host.round = gcc_jit_target_info_cpu_supports(info, "sse4.1");
                gcc_jit_target_info_release(info);
                gcc_jit_context_release(ctx);
            }
        }
#endif
        if (!host.march[0]) gcc = snx_find_tool("gcc");
        if (gcc) {
            args[0] = gcc;
            if (!snx_run(args, NULL, NULL, &output) && output && (p = strstr(output, "-march=")) != NULL) {
                char name[64];
                p += 7;
                while (*p == ' ' || *p == '\t') ++p;
                /* AArch64's GCC lists it empty: what follows is the next option */
                if (*p != '\n' && *p != '\r' && sscanf(p, "%63s", name) == 1 && strcmp(name, "native")) {
                    sprintf(host.march, "-march=%s", name);
                    host.fma = enabled(output, "-mfma");
                    host.round = enabled(output, "-msse4.1");
                }
            }
            free(output);
            free(gcc);
        }
#if defined(__aarch64__)
        host.fma = host.round = 1; /* FMADD and FRINT* are baseline */
#endif
    }
    return &host;
}

void snx_jit_host(void) { host_target(); }

SnxJit* snx_jit_new(char opt) {
    SnxJit* jit = (SnxJit*)calloc(1, sizeof(*jit));
    const HostTarget* host = host_target();
    gcc_jit_type* params[2];
    if (!jit) return NULL;
    jit->ctx = gcc_jit_context_acquire();
    if (opt == 's' || opt == 'z') {
        gcc_jit_context_set_int_option(jit->ctx, GCC_JIT_INT_OPTION_OPTIMIZATION_LEVEL, 2);
        gcc_jit_context_add_command_line_option(jit->ctx, "-Os");
    } else
        gcc_jit_context_set_int_option(jit->ctx, GCC_JIT_INT_OPTION_OPTIMIZATION_LEVEL, opt - '0');
    /* a shared object for this machine (build == host == target), with the flags generated C had */
#ifdef _WIN32
    /* no stack probes: they call libgcc's ___chkstk_ms, which the module doesn't link (nor the LLVM backend's) */
    gcc_jit_context_add_command_line_option(jit->ctx, "-mno-stack-arg-probe");
#else
    gcc_jit_context_add_command_line_option(jit->ctx, "-fPIC");
#endif
    /* linked from its own objects only, as the LLVM backend's linker does (link.h): the start files, C library and
       libgcc GCC's driver adds are those of the GCC libgccjit was built with, at its paths, which a system with
       another GCC or none lacks. The runtime library defines what the code calls (rtlib.c) */
    gcc_jit_context_add_driver_option(jit->ctx, "-nostdlib");
#if defined(__aarch64__)
    /* atomics inline: GCC would call libgcc's, which the module doesn't link (rtlib.c is built the same way) */
    gcc_jit_context_add_command_line_option(jit->ctx, "-mno-outline-atomics");
#endif
    if (host->march[0]) gcc_jit_context_add_command_line_option(jit->ctx, host->march);
    gcc_jit_context_add_command_line_option(jit->ctx, "-fno-strict-aliasing");
    gcc_jit_context_add_command_line_option(jit->ctx, "-fwrapv");
    gcc_jit_context_add_command_line_option(jit->ctx, "-foptimize-sibling-calls");
    /* the FP fast paths: each operation rounds as written (GCC fuses a*b+c by default, which would break TwoSum),
       and sqrt is the instruction, without an errno call for negative operands into a libm the module doesn't link */
    gcc_jit_context_add_command_line_option(jit->ctx, "-ffp-contract=off");
    gcc_jit_context_add_command_line_option(jit->ctx, "-fno-math-errno");
    /* GCC's SLP vectorizer packs the scalar code's neighbouring CPU fields (NZCV, adjacent X registers) into vector
       inserts and extracts: without it the integer benchmark (llvm_bench) runs 3x faster, the SIMD one, whose lanes
       are written one by one for it, 30% slower. SIMD in GCC's generic vector types would have both */
    gcc_jit_context_add_command_line_option(jit->ctx, "-fno-tree-slp-vectorize");
    jit->fma = host->fma;
    jit->round = host->round;
    jit->t_void = gcc_jit_context_get_type(jit->ctx, GCC_JIT_TYPE_VOID);
    jit->t_ptr = gcc_jit_context_get_type(jit->ctx, GCC_JIT_TYPE_VOID_PTR);
    jit->t_char_ptr = gcc_jit_type_get_pointer(gcc_jit_context_get_type(jit->ctx, GCC_JIT_TYPE_CHAR));
    jit->t_bool = gcc_jit_context_get_type(jit->ctx, GCC_JIT_TYPE_BOOL);
    jit->t_int = gcc_jit_context_get_type(jit->ctx, GCC_JIT_TYPE_INT);
    jit->t_u8 = gcc_jit_context_get_int_type(jit->ctx, 1, 0);
    jit->t_u16 = gcc_jit_context_get_int_type(jit->ctx, 2, 0);
    jit->t_u32 = gcc_jit_context_get_int_type(jit->ctx, 4, 0);
    jit->t_u64 = gcc_jit_context_get_int_type(jit->ctx, 8, 0);
    jit->t_u128 = gcc_jit_context_get_int_type(jit->ctx, 16, 0);
    jit->t_s8 = gcc_jit_context_get_int_type(jit->ctx, 1, 1);
    jit->t_s16 = gcc_jit_context_get_int_type(jit->ctx, 2, 1);
    jit->t_s32 = gcc_jit_context_get_int_type(jit->ctx, 4, 1);
    jit->t_s64 = gcc_jit_context_get_int_type(jit->ctx, 8, 1);
    jit->t_s128 = gcc_jit_context_get_int_type(jit->ctx, 16, 1);
    jit->t_f32 = gcc_jit_context_get_type(jit->ctx, GCC_JIT_TYPE_FLOAT);
    jit->t_f64 = gcc_jit_context_get_type(jit->ctx, GCC_JIT_TYPE_DOUBLE);
    params[0] = jit->t_ptr;
    params[1] = jit->t_u64;
    jit->t_guest_ptr = gcc_jit_context_new_function_ptr_type(jit->ctx, NULL, jit->t_void, 2, params, 0);
    return jit;
}

void snx_jit_free(SnxJit* jit) {
    if (!jit) return;
    gcc_jit_context_release(jit->ctx);
    free(jit->addresses);
    free(jit->functions);
    free(jit);
}

const char* snx_jit_error(SnxJit* jit) {
    size_t index;
    const SnxA64Pattern* pattern;
    if (!jit->fallbacks) return gcc_jit_context_get_first_error(jit->ctx);
    pattern = snx_a64_case_pattern(snx_a64_decode(jit->fallback_word), &index);
    sprintf(jit->fallback_error, "no libgccjit emitter for %.40s (%08lx at %llx), %lu instruction%s in all",
            pattern ? pattern->name : "?", (unsigned long)jit->fallback_word, (unsigned long long)jit->fallback_pc,
            jit->fallbacks, jit->fallbacks == 1 ? "" : "s");
    return jit->fallback_error;
}

/* ---- values ---- */

/* v fitted to the type: truncated, sign-extended for signed types; 128-bit types take v zero-extended (a long would
   sign-extend from bit 63) */
/* libgccjit takes constants as a C long, 32 bits on Windows: a wider one is built from its halves, which GCC folds */
static gcc_jit_rvalue* constant(SnxJit* jit, gcc_jit_type* type, SnxU64 v) {
    gcc_jit_rvalue *high, *low;
    if ((SnxU64)(long)v == v) return gcc_jit_context_new_rvalue_from_long(jit->ctx, type, (long)v);
    high = gcc_jit_context_new_cast(jit->ctx, NULL,
                                    gcc_jit_context_new_rvalue_from_long(jit->ctx, jit->t_u32, (long)(SnxU32)(v >> 32)),
                                    jit->t_u64);
    low = gcc_jit_context_new_cast(
        jit->ctx, NULL, gcc_jit_context_new_rvalue_from_long(jit->ctx, jit->t_u32, (long)(SnxU32)v), jit->t_u64);
    high = gcc_jit_context_new_binary_op(jit->ctx, NULL, GCC_JIT_BINARY_OP_LSHIFT, jit->t_u64, high,
                                         gcc_jit_context_new_rvalue_from_int(jit->ctx, jit->t_u64, 32));
    return gcc_jit_context_new_cast(
        jit->ctx, NULL,
        gcc_jit_context_new_binary_op(jit->ctx, NULL, GCC_JIT_BINARY_OP_BITWISE_OR, jit->t_u64, high, low), type);
}

gcc_jit_rvalue* jit_k(SnxJit* jit, gcc_jit_type* type, SnxU64 v) {
    unsigned bits = 64;
    int is_signed = 0;
    if (type == jit->t_u128 || type == jit->t_s128)
        return gcc_jit_context_new_cast(jit->ctx, NULL, constant(jit, jit->t_u64, v), type);
    if (type == jit->t_u8 || type == jit->t_s8)
        bits = 8;
    else if (type == jit->t_u16 || type == jit->t_s16)
        bits = 16;
    else if (type == jit->t_u32 || type == jit->t_s32)
        bits = 32;
    is_signed = type == jit->t_s8 || type == jit->t_s16 || type == jit->t_s32;
    if (bits < 64) {
        SnxU64 mask = ((SnxU64)1 << bits) - 1;
        v &= mask;
        if (is_signed && (v >> (bits - 1))) v |= ~mask;
    }
    return constant(jit, type, v);
}
gcc_jit_rvalue* jit_u64(SnxJit* jit, SnxU64 v) { return jit_k(jit, jit->t_u64, v); }
gcc_jit_rvalue* jit_u32(SnxJit* jit, SnxU32 v) { return jit_k(jit, jit->t_u32, v); }
gcc_jit_rvalue* jit_int(SnxJit* jit, int v) { return gcc_jit_context_new_rvalue_from_int(jit->ctx, jit->t_int, v); }
gcc_jit_rvalue* jit_true(SnxJit* jit) { return gcc_jit_context_new_rvalue_from_int(jit->ctx, jit->t_bool, 1); }
gcc_jit_rvalue* jit_false(SnxJit* jit) { return gcc_jit_context_new_rvalue_from_int(jit->ctx, jit->t_bool, 0); }

gcc_jit_type* jit_uint(SnxJit* jit, unsigned bits) {
    return bits == 8    ? jit->t_u8
           : bits == 16 ? jit->t_u16
           : bits == 32 ? jit->t_u32
           : bits == 64 ? jit->t_u64
                        : jit->t_u128;
}
gcc_jit_type* jit_sint(SnxJit* jit, unsigned bits) {
    return bits == 8    ? jit->t_s8
           : bits == 16 ? jit->t_s16
           : bits == 32 ? jit->t_s32
           : bits == 64 ? jit->t_s64
                        : jit->t_s128;
}

static gcc_jit_type* type_of(gcc_jit_rvalue* v) { return gcc_jit_rvalue_get_type(v); }

gcc_jit_rvalue* jit_cast(SnxJit* jit, gcc_jit_rvalue* v, gcc_jit_type* type) {
    if (type_of(v) == type) return v;
    return gcc_jit_context_new_cast(jit->ctx, NULL, v, type);
}
gcc_jit_rvalue* jit_bitcast(SnxJit* jit, gcc_jit_rvalue* v, gcc_jit_type* type) {
    return gcc_jit_context_new_bitcast(jit->ctx, NULL, v, type);
}

gcc_jit_lvalue* jit_local(SnxJit* jit, gcc_jit_type* type) {
    char name[32];
    sprintf(name, "t%lu", jit->names++);
    return gcc_jit_function_new_local(jit->fn, NULL, type, name);
}
void jit_assign(SnxJit* jit, gcc_jit_lvalue* lvalue, gcc_jit_rvalue* value) {
    gcc_jit_block_add_assignment(jit->block, NULL, lvalue,
                                 jit_cast(jit, value, type_of(gcc_jit_lvalue_as_rvalue(lvalue))));
}
gcc_jit_rvalue* jit_val(SnxJit* jit, gcc_jit_rvalue* v) {
    gcc_jit_lvalue* local = jit_local(jit, type_of(v));
    gcc_jit_block_add_assignment(jit->block, NULL, local, v);
    return gcc_jit_lvalue_as_rvalue(local);
}

gcc_jit_rvalue* jit_op(SnxJit* jit, enum gcc_jit_binary_op op, gcc_jit_rvalue* a, gcc_jit_rvalue* b) {
    gcc_jit_type* t = type_of(a);
    return gcc_jit_context_new_binary_op(jit->ctx, NULL, op, t, a, jit_cast(jit, b, t));
}
gcc_jit_rvalue* jit_unary(SnxJit* jit, enum gcc_jit_unary_op op, gcc_jit_rvalue* a) {
    return gcc_jit_context_new_unary_op(jit->ctx, NULL, op, type_of(a), a);
}
gcc_jit_rvalue* jit_cmp(SnxJit* jit, enum gcc_jit_comparison op, gcc_jit_rvalue* a, gcc_jit_rvalue* b) {
    return gcc_jit_context_new_comparison(jit->ctx, NULL, op, a, jit_cast(jit, b, type_of(a)));
}
gcc_jit_rvalue* jit_land(SnxJit* jit, gcc_jit_rvalue* a, gcc_jit_rvalue* b) {
    return gcc_jit_context_new_binary_op(jit->ctx, NULL, GCC_JIT_BINARY_OP_LOGICAL_AND, jit->t_bool, a, b);
}
gcc_jit_rvalue* jit_lor(SnxJit* jit, gcc_jit_rvalue* a, gcc_jit_rvalue* b) {
    return gcc_jit_context_new_binary_op(jit->ctx, NULL, GCC_JIT_BINARY_OP_LOGICAL_OR, jit->t_bool, a, b);
}
gcc_jit_rvalue* jit_lnot(SnxJit* jit, gcc_jit_rvalue* a) {
    return gcc_jit_context_new_unary_op(jit->ctx, NULL, GCC_JIT_UNARY_OP_LOGICAL_NEGATE, jit->t_bool, a);
}
gcc_jit_rvalue* jit_sel(SnxJit* jit, gcc_jit_rvalue* cond, gcc_jit_rvalue* a, gcc_jit_rvalue* b) {
    gcc_jit_type* t = type_of(a);
    gcc_jit_rvalue* mask = jit_op(jit, GCC_JIT_BINARY_OP_MINUS, jit_k(jit, t, 0), jit_cast(jit, cond, t));
    b = jit_cast(jit, b, t);
    return jit_op(jit, GCC_JIT_BINARY_OP_BITWISE_XOR, b,
                  jit_op(jit, GCC_JIT_BINARY_OP_BITWISE_AND, jit_op(jit, GCC_JIT_BINARY_OP_BITWISE_XOR, a, b), mask));
}
gcc_jit_rvalue* jit_expect(SnxJit* jit, gcc_jit_rvalue* cond, int likely) {
    gcc_jit_rvalue* args[2];
    args[0] = cond;
    args[1] = jit_int(jit, likely);
    return jit_cmp(jit, GCC_JIT_COMPARISON_NE, jit_call(jit, jit_builtin(jit, "__builtin_expect"), 2, args),
                   jit_int(jit, 0));
}

/* ---- the CPU ---- */

gcc_jit_lvalue* jit_at(SnxJit* jit, gcc_jit_rvalue* base, size_t offset, gcc_jit_type* type) {
    gcc_jit_rvalue* bytes = gcc_jit_context_new_cast(jit->ctx, NULL, base, jit->t_char_ptr);
    gcc_jit_lvalue* at = gcc_jit_context_new_array_access(
        jit->ctx, NULL, bytes, gcc_jit_context_new_rvalue_from_long(jit->ctx, jit->t_int, (long)offset));
    gcc_jit_rvalue* pointer =
        gcc_jit_context_new_cast(jit->ctx, NULL, gcc_jit_lvalue_get_address(at, NULL), gcc_jit_type_get_pointer(type));
    return gcc_jit_rvalue_dereference(pointer, NULL);
}
gcc_jit_lvalue* jit_cpu(SnxJit* jit, size_t offset, gcc_jit_type* type) { return jit_at(jit, jit->c, offset, type); }
gcc_jit_rvalue* jit_ld(SnxJit* jit, size_t offset, gcc_jit_type* type) {
    return jit_val(jit, gcc_jit_lvalue_as_rvalue(jit_cpu(jit, offset, type)));
}
void jit_st(SnxJit* jit, size_t offset, gcc_jit_type* type, gcc_jit_rvalue* value) {
    gcc_jit_block_add_assignment(jit->block, NULL, jit_cpu(jit, offset, type), jit_cast(jit, value, type));
}
size_t jit_off_x(unsigned n) { return offsetof(CPU, x) + 8 * (size_t)n; }
size_t jit_off_v(unsigned n) { return offsetof(CPU, v) + 16 * (size_t)n; }
gcc_jit_rvalue* jit_x(SnxJit* jit, unsigned n) { return jit_ld(jit, jit_off_x(n), jit->t_u64); }
gcc_jit_rvalue* jit_rx(SnxJit* jit, unsigned n) { return n == 31 ? jit_u64(jit, 0) : jit_x(jit, n); }
void jit_set_x(SnxJit* jit, unsigned n, gcc_jit_rvalue* value) { jit_st(jit, jit_off_x(n), jit->t_u64, value); }
void jit_setx(SnxJit* jit, unsigned n, gcc_jit_rvalue* value) {
    /* whatever computing the value does (flags, memory) is already in the block: values that do are jit_val'd */
    if (n != 31) jit_set_x(jit, n, value);
}
gcc_jit_rvalue* jit_vd(SnxJit* jit, unsigned n, unsigned lane) {
    return jit_ld(jit, jit_off_v(n) + 8 * (size_t)lane, jit->t_u64);
}
void jit_set_vd(SnxJit* jit, unsigned n, unsigned lane, gcc_jit_rvalue* value) {
    jit_st(jit, jit_off_v(n) + 8 * (size_t)lane, jit->t_u64, value);
}
gcc_jit_rvalue* jit_ve(SnxJit* jit, unsigned n, unsigned size, unsigned index) {
    return jit_ld(jit, jit_off_v(n) + ((size_t)index << size), jit_uint(jit, 8u << size));
}
void jit_set_ve(SnxJit* jit, unsigned n, unsigned size, unsigned index, gcc_jit_rvalue* value) {
    jit_st(jit, jit_off_v(n) + ((size_t)index << size), jit_uint(jit, 8u << size), value);
}

/* ---- guest memory ---- */

gcc_jit_rvalue* jit_mp(SnxJit* jit, gcc_jit_rvalue* address) {
    gcc_jit_lvalue* at = gcc_jit_context_new_array_access(jit->ctx, NULL, jit->M, jit_cast(jit, address, jit->t_u64));
    return gcc_jit_context_new_cast(jit->ctx, NULL, gcc_jit_lvalue_get_address(at, NULL), jit->t_ptr);
}
/* __atomic_load_n/__atomic_store_n/__atomic_compare_exchange_n on T* (cpu.h), seq_cst */
static gcc_jit_rvalue* atomic_load(SnxJit* jit, gcc_jit_rvalue* address, unsigned bits, int order) {
    char name[32];
    gcc_jit_rvalue* args[2];
    sprintf(name, "__atomic_load_%u", bits / 8);
    args[0] = jit_mp(jit, address);
    args[1] = jit_int(jit, order);
    return jit_cast(jit, jit_call(jit, jit_builtin(jit, name), 2, args), jit_uint(jit, bits));
}
static void atomic_store(SnxJit* jit, gcc_jit_rvalue* address, unsigned bits, gcc_jit_rvalue* value, int order) {
    char name[32];
    gcc_jit_rvalue* args[3];
    sprintf(name, "__atomic_store_%u", bits / 8);
    args[0] = jit_mp(jit, address);
    args[1] = jit_cast(jit, value, jit_uint(jit, bits));
    args[2] = jit_int(jit, order);
    jit_call(jit, jit_builtin(jit, name), 3, args);
}
/* Every guest access happens exactly as the guest wrote it (ir.c, ir_rd, says why): GCC must neither forward a store
   to a later load nor merge, sink or hoist accesses (a loop may poll memory another thread writes). libgccjit 14 never
   marks a dereference volatile (TREE_THIS_VOLATILE), whatever the pointer's type, so the accesses are relaxed
   atomics, which GCC keeps as written: plain moves on x86-64, unaligned ones included. Like any call or asm, they
   may clobber the CPU's fields for GCC (an empty asm volatile "+m" on the location measured the same) */
gcc_jit_rvalue* jit_rd(SnxJit* jit, gcc_jit_rvalue* address, unsigned bits) {
    return atomic_load(jit, address, bits, 0 /* __ATOMIC_RELAXED */);
}
void jit_wr(SnxJit* jit, gcc_jit_rvalue* address, unsigned bits, gcc_jit_rvalue* value) {
    atomic_store(jit, address, bits, value, 0);
}
gcc_jit_rvalue* jit_ldacq(SnxJit* jit, gcc_jit_rvalue* address, unsigned bits) {
    return atomic_load(jit, address, bits, 5 /* __ATOMIC_SEQ_CST */);
}
void jit_strel(SnxJit* jit, gcc_jit_rvalue* address, unsigned bits, gcc_jit_rvalue* value) {
    atomic_store(jit, address, bits, value, 5);
}
gcc_jit_rvalue* jit_cas(SnxJit* jit, gcc_jit_rvalue* address, unsigned bits, gcc_jit_rvalue* expected,
                        gcc_jit_rvalue* value) {
    char name[40];
    gcc_jit_lvalue* old = jit_local(jit, jit_uint(jit, bits));
    gcc_jit_rvalue* args[6];
    jit_assign(jit, old, expected);
    sprintf(name, "__atomic_compare_exchange_%u", bits / 8);
    args[0] = jit_mp(jit, address);
    args[1] = gcc_jit_lvalue_get_address(old, NULL);
    args[2] = jit_cast(jit, value, jit_uint(jit, bits));
    args[3] = jit_false(jit);
    args[4] = jit_int(jit, 5);
    args[5] = jit_int(jit, 5);
    return jit_cast(jit, jit_call(jit, jit_builtin(jit, name), 6, args), jit->t_u32);
}

/* ---- calls ---- */

gcc_jit_function* jit_extern(SnxJit* jit, const char* name, gcc_jit_type* ret, int count, gcc_jit_type** params) {
    gcc_jit_param* p[8];
    gcc_jit_function* f;
    size_t i;
    int k;
    for (i = 0; i < jit->extern_count; ++i)
        if (!strcmp(jit->externs[i].name, name)) return jit->externs[i].fn;
    for (k = 0; k < count; ++k) {
        char pname[8];
        sprintf(pname, "p%d", k);
        p[k] = gcc_jit_context_new_param(jit->ctx, NULL, params[k], pname);
    }
    f = gcc_jit_context_new_function(jit->ctx, NULL, GCC_JIT_FUNCTION_IMPORTED, ret, name, count, p, 0);
    /* everything the module links is local to it (the runtime library is built -fvisibility=hidden) */
    gcc_jit_function_add_string_attribute(f, GCC_JIT_FN_ATTRIBUTE_VISIBILITY, "hidden");
    if (jit->extern_count < sizeof(jit->externs) / sizeof(jit->externs[0])) {
        jit->externs[jit->extern_count].name = name;
        jit->externs[jit->extern_count++].fn = f;
    }
    return f;
}
gcc_jit_function* jit_builtin(SnxJit* jit, const char* name) {
    return gcc_jit_context_get_builtin_function(jit->ctx, name);
}
gcc_jit_rvalue* jit_call(SnxJit* jit, gcc_jit_function* function, int count, gcc_jit_rvalue** args) {
    gcc_jit_rvalue *cast[8], *call;
    int i;
    for (i = 0; i < count; ++i)
        cast[i] = jit_cast(jit, args[i], type_of(gcc_jit_param_as_rvalue(gcc_jit_function_get_param(function, i))));
    call = gcc_jit_context_new_call(jit->ctx, NULL, function, count, cast);
    if (type_of(call) == jit->t_void) {
        gcc_jit_block_add_eval(jit->block, NULL, call);
        return NULL;
    }
    return jit_val(jit, call);
}
gcc_jit_rvalue* jit_rt_call(SnxJit* jit, size_t offset, gcc_jit_type* ret, int count, gcc_jit_type** params,
                            gcc_jit_rvalue** args) {
    gcc_jit_type* fn_type = gcc_jit_context_new_function_ptr_type(jit->ctx, NULL, ret, count, params, 0);
    gcc_jit_rvalue *rt = jit_ld(jit, offsetof(CPU, rt), jit->t_ptr), *callee, *cast[8], *call;
    int i;
    callee = jit_val(jit, gcc_jit_lvalue_as_rvalue(jit_at(jit, rt, offset, fn_type)));
    for (i = 0; i < count; ++i) cast[i] = jit_cast(jit, args[i], params[i]);
    call = gcc_jit_context_new_call_through_ptr(jit->ctx, NULL, callee, count, cast);
    if (ret == jit->t_void) {
        gcc_jit_block_add_eval(jit->block, NULL, call);
        return NULL;
    }
    return jit_val(jit, call);
}
size_t jit_rt_sev(void) { return offsetof(RtAPI, sev); }
size_t jit_rt_wfe(void) { return offsetof(RtAPI, wfe); }

void jit_undef(SnxJit* jit, SnxU64 pc, SnxU32 word) {
    gcc_jit_type* params[3];
    gcc_jit_rvalue* args[3];
    params[0] = jit->t_ptr;
    params[1] = jit->t_u64;
    params[2] = jit->t_u32;
    args[0] = jit->c;
    args[1] = jit_u64(jit, pc);
    args[2] = jit_u32(jit, word);
    jit_rt_call(jit, offsetof(RtAPI, undef), jit->t_void, 3, params, args);
}
void jit_fence(SnxJit* jit) {
    gcc_jit_rvalue* order = jit_int(jit, 5);
    jit_call(jit, jit_builtin(jit, "__atomic_thread_fence"), 1, &order);
}
void jit_barrier(SnxJit* jit) {
    gcc_jit_extended_asm* barrier = gcc_jit_block_add_extended_asm(jit->block, NULL, "");
    gcc_jit_extended_asm_set_volatile_flag(barrier, 1);
    gcc_jit_extended_asm_add_clobber(barrier, "memory");
}

/* ---- blocks ---- */

gcc_jit_block* jit_block(SnxJit* jit) {
    char name[32];
    sprintf(name, "b%lu", jit->names++);
    return gcc_jit_function_new_block(jit->fn, name);
}
void jit_goto(SnxJit* jit, gcc_jit_block* target) {
    gcc_jit_block_end_with_jump(jit->block, NULL, target);
    jit->block = NULL;
}
void jit_branch(SnxJit* jit, gcc_jit_rvalue* cond, gcc_jit_block* yes, gcc_jit_block* no) {
    gcc_jit_block_end_with_conditional(jit->block, NULL, cond, yes, no);
    jit->block = NULL;
}
void jit_enter(SnxJit* jit, gcc_jit_block* block) { jit->block = block; }

static gcc_jit_rvalue* load(gcc_jit_lvalue* lvalue) { return gcc_jit_lvalue_as_rvalue(lvalue); }
static void ret(SnxJit* jit) {
    gcc_jit_block_end_with_void_return(jit->block, NULL);
    jit->block = NULL;
}
static gcc_jit_rvalue* compare(SnxJit* jit, enum gcc_jit_comparison op, gcc_jit_rvalue* a, gcc_jit_rvalue* b) {
    return gcc_jit_context_new_comparison(jit->ctx, NULL, op, a, b);
}

/* ---- functions ---- */

/* CPU* RESTRICT c, as the generated C declared it */
static gcc_jit_type* cpu_param_type(SnxJit* jit) { return gcc_jit_type_get_restrict(jit->t_ptr); }

gcc_jit_function* snx_jit_guest_function(SnxJit* jit, SnxU64 address, int define) {
    char name[32];
    size_t i;
    gcc_jit_param* params[2];
    gcc_jit_function* f;
    for (i = 0; i < jit->count; ++i)
        if (jit->addresses[i] == address) return jit->functions[i];
    if (address >> 32)
        sprintf(name, "F_%lX%08lX", (unsigned long)(address >> 32), (unsigned long)(address & 0xFFFFFFFFUL));
    else
        sprintf(name, "F_%lX", (unsigned long)address);
    params[0] = gcc_jit_context_new_param(jit->ctx, NULL, cpu_param_type(jit), "c");
    params[1] = gcc_jit_context_new_param(jit->ctx, NULL, jit->t_u64, "e");
    f = gcc_jit_context_new_function(jit->ctx, NULL, define ? GCC_JIT_FUNCTION_EXPORTED : GCC_JIT_FUNCTION_IMPORTED,
                                     jit->t_void, name, 2, params, 0);
    /* -fvisibility=hidden: calls between units bind inside the module, without the PLT */
    gcc_jit_function_add_string_attribute(f, GCC_JIT_FN_ATTRIBUTE_VISIBILITY, "hidden");
    if (jit->count == jit->capacity) {
        jit->capacity = jit->capacity ? jit->capacity * 2 : 256;
        jit->addresses = (SnxU64*)realloc(jit->addresses, jit->capacity * sizeof(*jit->addresses));
        jit->functions = (gcc_jit_function**)realloc(jit->functions, jit->capacity * sizeof(*jit->functions));
    }
    jit->addresses[jit->count] = address;
    jit->functions[jit->count++] = f;
    return f;
}

/* snx_lookup (cpu.h): the guest function at pc, or the runtime's RtAPI.miss */
static gcc_jit_function* lookup_function(SnxJit* jit) {
    gcc_jit_param* params[2];
    gcc_jit_function* f;
    gcc_jit_block *entry, *table, *found, *none;
    gcc_jit_rvalue *c, *pc, *lo, *o, *inside, *args[2];
    gcc_jit_function* atomic_load8;
    gcc_jit_lvalue* fn;
    if (jit->lookup) return jit->lookup;
    params[0] = gcc_jit_context_new_param(jit->ctx, NULL, jit->t_ptr, "c");
    params[1] = gcc_jit_context_new_param(jit->ctx, NULL, jit->t_u64, "pc");
    f = gcc_jit_context_new_function(jit->ctx, NULL, GCC_JIT_FUNCTION_ALWAYS_INLINE, jit->t_guest_ptr, "snx_lookup", 2,
                                     params, 0);
    c = gcc_jit_param_as_rvalue(params[0]);
    pc = gcc_jit_param_as_rvalue(params[1]);
    entry = gcc_jit_function_new_block(f, "entry");
    table = gcc_jit_function_new_block(f, "table");
    found = gcc_jit_function_new_block(f, "found");
    none = gcc_jit_function_new_block(f, "none");
    lo = load(jit_at(jit, c, offsetof(CPU, code_lo), jit->t_u64));
    o = gcc_jit_context_new_binary_op(jit->ctx, NULL, GCC_JIT_BINARY_OP_MINUS, jit->t_u64, pc, lo);
    inside = gcc_jit_context_new_binary_op(
        jit->ctx, NULL, GCC_JIT_BINARY_OP_LOGICAL_AND, jit->t_bool,
        compare(jit, GCC_JIT_COMPARISON_LT, o,
                gcc_jit_context_new_binary_op(jit->ctx, NULL, GCC_JIT_BINARY_OP_MINUS, jit->t_u64,
                                              load(jit_at(jit, c, offsetof(CPU, code_hi), jit->t_u64)), lo)),
        compare(jit, GCC_JIT_COMPARISON_EQ,
                gcc_jit_context_new_binary_op(jit->ctx, NULL, GCC_JIT_BINARY_OP_BITWISE_AND, jit->t_u64, pc,
                                              jit_u64(jit, 3)),
                jit_u64(jit, 0)));
    gcc_jit_block_end_with_conditional(entry, NULL, inside, table, none);
    fn = gcc_jit_function_new_local(f, NULL, jit->t_guest_ptr, "fn");
    /* __atomic_load_n(&c->disp[o>>2],__ATOMIC_RELAXED): the runtime JIT fills slots while this runs (cpu.h). The
       builtin takes a const volatile void* and returns an integer as wide as the pointer */
    atomic_load8 = jit_builtin(jit, "__atomic_load_8");
    args[0] = gcc_jit_context_new_cast(
        jit->ctx, NULL,
        gcc_jit_lvalue_get_address(
            gcc_jit_context_new_array_access(
                jit->ctx, NULL,
                gcc_jit_context_new_cast(jit->ctx, NULL, load(jit_at(jit, c, offsetof(CPU, disp), jit->t_ptr)),
                                         gcc_jit_type_get_pointer(jit->t_guest_ptr)),
                gcc_jit_context_new_binary_op(jit->ctx, NULL, GCC_JIT_BINARY_OP_RSHIFT, jit->t_u64, o,
                                              jit_u64(jit, 2))),
            NULL),
        gcc_jit_rvalue_get_type(gcc_jit_param_as_rvalue(gcc_jit_function_get_param(atomic_load8, 0))));
    args[1] = gcc_jit_context_new_cast(
        jit->ctx, NULL, jit_int(jit, 0 /* __ATOMIC_RELAXED */),
        gcc_jit_rvalue_get_type(gcc_jit_param_as_rvalue(gcc_jit_function_get_param(atomic_load8, 1))));
    gcc_jit_block_add_assignment(
        table, NULL, fn,
        jit_bitcast(jit, gcc_jit_context_new_call(jit->ctx, NULL, atomic_load8, 2, args), jit->t_guest_ptr));
    gcc_jit_block_end_with_conditional(
        table, NULL, compare(jit, GCC_JIT_COMPARISON_NE, load(fn), gcc_jit_context_null(jit->ctx, jit->t_guest_ptr)),
        found, none);
    gcc_jit_block_end_with_return(found, NULL, load(fn));
    /* c->rt->miss */
    gcc_jit_block_end_with_return(none, NULL,
                                  load(jit_at(jit, load(jit_at(jit, c, offsetof(CPU, rt), jit->t_ptr)),
                                              offsetof(RtAPI, miss), jit->t_guest_ptr)));
    return jit->lookup = f;
}

/* CALLDYN(t): snx_lookup(c,t)(c,t) */
static gcc_jit_rvalue* call_dynamic(SnxJit* jit, gcc_jit_rvalue* target) {
    gcc_jit_rvalue *args[2], *fn;
    args[0] = jit->c;
    args[1] = target;
    fn = gcc_jit_context_new_call(jit->ctx, NULL, lookup_function(jit), 2, args);
    return gcc_jit_context_new_call_through_ptr(jit->ctx, NULL, fn, 2, args);
}
/* a call the C follows with return: required to become a jump, so guest tail calls never grow the host stack */
static void tail(SnxJit* jit, gcc_jit_rvalue* call) {
    gcc_jit_rvalue_set_bool_require_tail_call(call, 1);
    gcc_jit_block_add_eval(jit->block, NULL, call);
    ret(jit);
}

static void call(SnxJit* jit, const SnxCodegen* codegen, SnxU64 target, int is_tail) {
    gcc_jit_rvalue *args[2], *site;
    args[0] = jit->c;
    args[1] = jit_u64(jit, target);
    /* --hook-direct-calls: through CPU.disp even to a known function, where runtime wrappers can see the call */
    if (snx_codegen_function(codegen, target) && !codegen->hook_direct_calls)
        site = gcc_jit_context_new_call(jit->ctx, NULL, snx_jit_guest_function(jit, target, 0), 2, args);
    else
        site = call_dynamic(jit, args[1]);
    if (is_tail)
        tail(jit, site);
    else
        gcc_jit_block_add_eval(jit->block, NULL, site);
}

static size_t index_of(const SnxFunction* function, SnxU64 address) {
    size_t lo = 0, hi = function->count, middle;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
        if (snx_function_address(function, middle) < address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo;
}

/* if(UNLIKELY(__atomic_load_n(&c->poll,__ATOMIC_RELAXED))){ [c->pc=resume;] c->rt->poll(c); } (codegen.c): a loop
   that makes no SVC can still be taken off its core */
static void poll(SnxJit* jit, const SnxU64* resume) {
    gcc_jit_block *yes = jit_block(jit), *no = jit_block(jit);
    gcc_jit_rvalue *args[2], *flag;
    gcc_jit_type* param = jit->t_ptr;
    args[0] = gcc_jit_context_new_cast(
        jit->ctx, NULL, gcc_jit_lvalue_get_address(jit_cpu(jit, offsetof(CPU, poll), jit->t_u32), NULL), jit->t_ptr);
    args[1] = jit_int(jit, 0);
    flag = jit_call(jit, jit_builtin(jit, "__atomic_load_4"), 2, args);
    jit_branch(jit, jit_expect(jit, jit_cmp(jit, GCC_JIT_COMPARISON_NE, flag, jit_k(jit, type_of(flag), 0)), 0), yes,
               no);
    jit_enter(jit, yes);
    if (resume) jit_st(jit, offsetof(CPU, pc), jit->t_u64, jit_u64(jit, *resume));
    args[0] = jit->c;
    jit_rt_call(jit, offsetof(RtAPI, poll), jit->t_void, 1, &param, args);
    jit_goto(jit, no);
    jit_enter(jit, no);
}

/* goto L_target inside the function (polling first when it goes backward), a tail call outside it */
static void branch(SnxJit* jit, const SnxCodegen* codegen, const SnxFunction* function, gcc_jit_block** labels,
                   SnxU64 address, SnxU64 target) {
    if (snx_function_contains(function, target)) {
        if (target <= address) poll(jit, &target);
        jit_goto(jit, labels[index_of(function, target)]);
    } else
        call(jit, codegen, target, 1);
}

/* if(UNLIKELY(c->pc!=pc+4)) return; */
static void check_return(SnxJit* jit, SnxU64 next) {
    gcc_jit_block *leave = jit_block(jit), *stay = jit_block(jit);
    jit_branch(
        jit,
        jit_expect(jit,
                   jit_cmp(jit, GCC_JIT_COMPARISON_NE, jit_ld(jit, offsetof(CPU, pc), jit->t_u64), jit_u64(jit, next)),
                   0),
        leave, stay);
    gcc_jit_block_end_with_void_return(leave, NULL);
    jit_enter(jit, stay);
}

static void svc(SnxJit* jit, SnxU64 pc, gcc_jit_rvalue* imm) {
    gcc_jit_type* params[2];
    gcc_jit_rvalue* args[2];
    jit_st(jit, offsetof(CPU, pc), jit->t_u64, jit_u64(jit, pc + 4));
    params[0] = jit->t_ptr;
    params[1] = jit->t_u32;
    args[0] = jit->c;
    args[1] = imm;
    jit_rt_call(jit, offsetof(RtAPI, svc), jit->t_void, 2, params, args);
}

static void brk(SnxJit* jit, SnxU64 pc, gcc_jit_rvalue* imm) {
    gcc_jit_type* params[3];
    gcc_jit_rvalue* args[3];
    params[0] = jit->t_ptr;
    params[1] = jit->t_u64;
    params[2] = jit->t_u32;
    args[0] = jit->c;
    args[1] = jit_u64(jit, pc);
    args[2] = imm;
    jit_rt_call(jit, offsetof(RtAPI, brk), jit->t_void, 3, params, args);
}

/* The body of a PLAIN instruction. One without an emitter is noted, and snx_jit_error fails the unit */
static void plain(SnxJit* jit, SnxU32 word, SnxU64 pc, const SnxA64Case* instruction) {
    if (snx_jit_plain(jit, word, pc, instruction)) return;
    if (!jit->fallbacks++) {
        jit->fallback_word = word;
        jit->fallback_pc = pc;
    }
}

static void begin(SnxJit* jit, gcc_jit_function* function) {
    jit->fn = function;
    jit->c = gcc_jit_param_as_rvalue(gcc_jit_function_get_param(function, 0));
    jit->block = gcc_jit_function_new_block(function, "entry");
    jit->M = jit_val(jit, gcc_jit_context_new_cast(jit->ctx, NULL, jit_ld(jit, offsetof(CPU, mem), jit->t_ptr),
                                                   gcc_jit_type_get_pointer(jit->t_u8)));
}

void snx_jit_emit(SnxJit* jit, SnxCodegen* codegen, const SnxFunction* function) {
    gcc_jit_function* fn = snx_jit_guest_function(jit, function->start, 1);
    gcc_jit_block **labels, *dispatch_block = NULL, *mismatch, *body;
    gcc_jit_rvalue *e, *t;
    size_t i;
    SnxU64 address, next, target;
    SnxU32 word;
    const SnxA64Case* c;
    int dispatch, terminal;
    labels = (gcc_jit_block**)calloc(function->count ? function->count : 1, sizeof(*labels));
    begin(jit, fn);
    e = gcc_jit_param_as_rvalue(gcc_jit_function_get_param(fn, 1));
    for (i = 0; i < function->count; ++i)
        if (function->leaders[i]) labels[i] = jit_block(jit);
    dispatch = function->leader_count > 1;
    if (dispatch) dispatch_block = jit_block(jit);
    /* if(UNLIKELY(e!=start)){ c->pc=e; goto D_; } or { CALLDYN(e); return; } */
    mismatch = jit_block(jit);
    body = jit_block(jit);
    jit_branch(jit, jit_expect(jit, jit_cmp(jit, GCC_JIT_COMPARISON_NE, e, jit_u64(jit, function->start)), 0), mismatch,
               body);
    jit_enter(jit, mismatch);
    if (dispatch) {
        jit_st(jit, offsetof(CPU, pc), jit->t_u64, e);
        jit_goto(jit, dispatch_block);
    } else
        tail(jit, call_dynamic(jit, e));
    jit_enter(jit, body);
    if (function->count && snx_function_address(function, 0) != function->start)
        jit_goto(jit, labels[index_of(function, function->start)]);
    for (i = 0; i < function->count; ++i) {
        address = snx_function_address(function, i);
        next = address + 4;
        if (function->leaders[i]) {
            if (jit->block) jit_goto(jit, labels[i]);
            jit_enter(jit, labels[i]);
        }
        if (!jit->block) continue; /* unreachable after a terminator until the next label, as in the C */
        word = 0;
        if (!snx_codegen_word(codegen, address, &word))
            c = snx_a64_decode(0);
        else
            c = snx_a64_decode(word);
        if (c->kind == SNX_FLOW_UNDEF) {
            jit_st(jit, offsetof(CPU, pc), jit->t_u64, jit_u64(jit, address));
            jit_undef(jit, address, word);
            ret(jit);
            continue;
        }
        terminal = 0;
        switch (c->kind) {
            case SNX_FLOW_PLAIN: plain(jit, word, address, c); break;
            case SNX_FLOW_B:
                branch(jit, codegen, function, labels, address, snx_a64_target(word, address));
                terminal = 1;
                break;
            case SNX_FLOW_BC: {
                gcc_jit_block *taken = jit_block(jit), *fall = jit_block(jit);
                jit_branch(jit, snx_jit_flow_operand(jit, word, address, c), taken, fall);
                jit_enter(jit, taken);
                branch(jit, codegen, function, labels, address, snx_a64_target(word, address));
                jit_enter(jit, fall);
                break;
            }
            case SNX_FLOW_BL:
                target = snx_a64_target(word, address);
                jit_set_x(jit, 30, jit_u64(jit, next));
                call(jit, codegen, target, 0);
                check_return(jit, next);
                break;
            case SNX_FLOW_BLR:
                t = snx_jit_flow_operand(jit, word, address, c);
                jit_set_x(jit, 30, jit_u64(jit, next));
                gcc_jit_block_add_eval(jit->block, NULL, call_dynamic(jit, t));
                check_return(jit, next);
                break;
            case SNX_FLOW_BR:
                t = snx_jit_flow_operand(jit, word, address, c);
                if (dispatch) {
                    jit_st(jit, offsetof(CPU, pc), jit->t_u64, t);
                    jit_goto(jit, dispatch_block);
                } else
                    tail(jit, call_dynamic(jit, t));
                terminal = 1;
                break;
            case SNX_FLOW_RET:
                jit_st(jit, offsetof(CPU, pc), jit->t_u64, snx_jit_flow_operand(jit, word, address, c));
                ret(jit);
                terminal = 1;
                break;
            case SNX_FLOW_SVC: svc(jit, address, snx_jit_flow_operand(jit, word, address, c)); break;
            case SNX_FLOW_BRK:
                jit_st(jit, offsetof(CPU, pc), jit->t_u64, jit_u64(jit, address));
                brk(jit, address, snx_jit_flow_operand(jit, word, address, c));
                ret(jit);
                terminal = 1;
                break;
            case SNX_FLOW_UNDEF: break;
        }
        if (!terminal && jit->block &&
            (!snx_function_contains(function, next) ||
             (i + 1 < function->count && snx_function_address(function, i + 1) != next)))
            call(jit, codegen, next, 1);
    }
    if (jit->block) ret(jit);
    if (dispatch) {
        /* D_: if(UNLIKELY(poll)) c->rt->poll(c); switch(c->pc){ case <leader>: goto L_<leader>; ...
           default: CALLDYN(c->pc); return; } */
        gcc_jit_case** cases = (gcc_jit_case**)calloc(function->leader_count, sizeof(*cases));
        gcc_jit_block* other = jit_block(jit);
        size_t n = 0;
        jit_enter(jit, dispatch_block);
        poll(jit, NULL);
        for (i = 0; i < function->count; ++i)
            if (function->leaders[i]) {
                gcc_jit_rvalue* value = jit_u64(jit, snx_function_address(function, i));
                cases[n++] = gcc_jit_context_new_case(jit->ctx, value, value, labels[i]);
            }
        gcc_jit_block_end_with_switch(jit->block, NULL, jit_ld(jit, offsetof(CPU, pc), jit->t_u64), other, (int)n,
                                      cases);
        free(cases);
        jit_enter(jit, other);
        tail(jit, call_dynamic(jit, jit_ld(jit, offsetof(CPU, pc), jit->t_u64)));
    }
    free(labels);
}

gcc_jit_function* snx_jit_static_step(SnxJit* jit, SnxU32 word, SnxU64 pc, const char* name) {
    gcc_jit_param* param = gcc_jit_context_new_param(jit->ctx, NULL, cpu_param_type(jit), "c");
    gcc_jit_function* fn =
        gcc_jit_context_new_function(jit->ctx, NULL, GCC_JIT_FUNCTION_EXPORTED, jit->t_u64, name, 1, &param, 0);
    const SnxA64Case* c = snx_a64_decode(word);
    gcc_jit_rvalue *operand, *result = NULL;
    begin(jit, fn);
    switch (c->kind) {
        case SNX_FLOW_PLAIN:
            plain(jit, word, pc, c);
            result = jit_u64(jit, pc + 4);
            break;
        case SNX_FLOW_UNDEF:
            jit_undef(jit, pc, word);
            result = jit_u64(jit, pc);
            break;
        case SNX_FLOW_B: result = jit_u64(jit, snx_a64_target(word, pc)); break;
        case SNX_FLOW_BR:
        case SNX_FLOW_RET: result = snx_jit_flow_operand(jit, word, pc, c); break;
        case SNX_FLOW_BC:
            result = jit_sel(jit, snx_jit_flow_operand(jit, word, pc, c), jit_u64(jit, snx_a64_target(word, pc)),
                             jit_u64(jit, pc + 4));
            break;
        case SNX_FLOW_BL:
        case SNX_FLOW_BLR:
            /* { u64 t_=<target>; c->x[30]=pc+4; return t_; } */
            operand = c->kind == SNX_FLOW_BL ? jit_u64(jit, snx_a64_target(word, pc))
                                             : jit_val(jit, snx_jit_flow_operand(jit, word, pc, c));
            jit_set_x(jit, 30, jit_u64(jit, pc + 4));
            result = operand;
            break;
        case SNX_FLOW_SVC:
            svc(jit, pc, snx_jit_flow_operand(jit, word, pc, c));
            result = jit_u64(jit, pc + 4);
            break;
        case SNX_FLOW_BRK:
            brk(jit, pc, snx_jit_flow_operand(jit, word, pc, c));
            result = jit_u64(jit, pc);
            break;
    }
    gcc_jit_block_end_with_return(jit->block, NULL, result);
    jit->block = NULL;
    return fn;
}
