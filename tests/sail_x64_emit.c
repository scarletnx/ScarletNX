/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The A64 emitters sailgen generates with its x86-64 back end (a64_x64.inc: x64.h and x64_a64.h over the instruction
   selection derived from the Intel SDM, x64_isel.inc), in a translation unit of their own: sail_x64.c holds them to
   the LLVM back end's */
#include "sail_x64_emit.h"

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "a64_x64.inc"

static SgCtx g_ctx; /* large: the block's SSA */

size_t snx_x64_step(uint32_t word, uint64_t pc, uint64_t features, unsigned host_features, unsigned char* code,
                    size_t cap) {
    return sg_x64_step(&g_ctx, word, pc, features, host_features, code, cap);
}

unsigned snx_x64_instructions(void) { return (unsigned)g_ctx.x.n; }

size_t snx_x64_block(const uint32_t* words, size_t* count, uint64_t pc, uint64_t features, unsigned host_features,
                     unsigned char* code, size_t cap) {
    return sg_x64_block(&g_ctx, words, count, pc, features, host_features, code, cap);
}

void snx_x64_set_pool(int registers) { g_ctx.x.pool = registers; }

/* the helpers the code calls (x64_helpers.c, linked in), by name */
u64 snx_x64_fp_binary(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_fma(CPU* c, u64 add, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_unary(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_minmax(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_compare(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_convert(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_round(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_round_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_to_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_from_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_jcvt(CPU* c, u64 a, u64 fpcr);
u64 snx_x64_fp_bfcvt(CPU* c, u64 a, u64 fpcr);
extern const u32 snx_x64_crc32_table[256];
/* the floating-point helpers counted: a hot path's operation calls none */
static unsigned long g_fp_calls;
unsigned long snx_x64_fp_calls(void) { return g_fp_calls; }
static u64 counted_binary(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc) {
    ++g_fp_calls;
    return snx_x64_fp_binary(c, a, b, fpcr, desc);
}
static u64 counted_fma(CPU* c, u64 add, u64 a, u64 b, u64 fpcr, u32 desc) {
    ++g_fp_calls;
    return snx_x64_fp_fma(c, add, a, b, fpcr, desc);
}
static u64 counted_unary(CPU* c, u64 a, u64 fpcr, u32 desc) {
    ++g_fp_calls;
    return snx_x64_fp_unary(c, a, fpcr, desc);
}
static u64 counted_minmax(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc) {
    ++g_fp_calls;
    return snx_x64_fp_minmax(c, a, b, fpcr, desc);
}
static u64 counted_compare(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc) {
    ++g_fp_calls;
    return snx_x64_fp_compare(c, a, b, fpcr, desc);
}
static u64 counted_convert(CPU* c, u64 a, u64 fpcr, u32 desc) {
    ++g_fp_calls;
    return snx_x64_fp_convert(c, a, fpcr, desc);
}
static u64 counted_round(CPU* c, u64 a, u64 fpcr, u32 desc) {
    ++g_fp_calls;
    return snx_x64_fp_round(c, a, fpcr, desc);
}
static u64 counted_to_int(CPU* c, u64 a, u64 fpcr, u32 desc) {
    ++g_fp_calls;
    return snx_x64_fp_to_int(c, a, fpcr, desc);
}
#define SNX_X64_FN(name) {#name, (void*)name}
#define SNX_X64_COUNTED(name, f) {#name, (void*)f}
static SnxU64 resolve(const char* sym) {
    static const struct {
        const char* name;
        void* f;
    } table[] = {SNX_X64_COUNTED(snx_x64_fp_binary, counted_binary),
                 SNX_X64_COUNTED(snx_x64_fp_fma, counted_fma),
                 SNX_X64_COUNTED(snx_x64_fp_unary, counted_unary),
                 SNX_X64_COUNTED(snx_x64_fp_minmax, counted_minmax),
                 SNX_X64_COUNTED(snx_x64_fp_compare, counted_compare),
                 SNX_X64_COUNTED(snx_x64_fp_convert, counted_convert),
                 SNX_X64_COUNTED(snx_x64_fp_round, counted_round),
                 SNX_X64_FN(snx_x64_fp_round_int),
                 SNX_X64_COUNTED(snx_x64_fp_to_int, counted_to_int),
                 SNX_X64_FN(snx_x64_fp_from_int),
                 SNX_X64_FN(snx_x64_fp_jcvt),
                 SNX_X64_FN(snx_x64_fp_bfcvt),
                 {"snx_x64_crc32_table", (void*)snx_x64_crc32_table}};
    for (size_t i = 0; i < sizeof table / sizeof *table; ++i)
        if (!strcmp(sym, table[i].name)) return (SnxU64)(uintptr_t)table[i].f;
    return 0;
}
[[gnu::constructor]] static void set_resolver(void) { g_ctx.resolve = resolve; }
