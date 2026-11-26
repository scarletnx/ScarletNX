/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The A64 emitters sailgen generates with its x86-64 back end (a64_x64.inc: x64.h and x64_a64.h over the instruction
   selection derived from the Intel SDM, x64_isel.inc), in a translation unit of their own: sail_x64.c holds them to
   the LLVM back end's */
#include "sail_x64_emit.h"
#include "common/utilities/endian.h"

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
void snx_x64_set_xpool(int registers) { g_ctx.x.xpool = registers; }
int snx_x64_pool(void) { return g_ctx.x.pool; }
int snx_x64_xpool(void) { return g_ctx.x.xpool; }

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
u64 snx_x64_aes(u64 x, u32 op);
u64 snx_x64_estimate(u64 a, u32 sqrt);
u64 snx_x64_half(u64 a, u64 fpcr);
u64 snx_x64_bfdot(u64 sum, u64 a, u64 b);
u64 snx_x64_sm4_box(u64 a);
extern const u32 snx_x64_crc32_table[8 * 256], snx_x64_crc32c_table[256];
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
                 SNX_X64_FN(snx_x64_aes),
                 SNX_X64_FN(snx_x64_estimate),
                 SNX_X64_FN(snx_x64_half),
                 SNX_X64_FN(snx_x64_bfdot),
                 SNX_X64_FN(snx_x64_sm4_box),
                 {"snx_x64_crc32_table", (void*)snx_x64_crc32_table},
                 {"snx_x64_crc32c_table", (void*)snx_x64_crc32c_table}};
    for (size_t i = 0; i < sizeof table / sizeof *table; ++i)
        if (!strcmp(sym, table[i].name)) return (SnxU64)(uintptr_t)table[i].f;
    return 0;
}
[[gnu::constructor]] static void set_resolver(void) { g_ctx.resolve = resolve; }

const char* snx_x64_cold_pool(void) {
    unsigned char code[512];
    const SnxU64 lo = 0x0123456789ABCDEFull, hi = 0x0FEDCBA987654321ull;
    SgXBuf hot = {.p = code, .cap = sizeof code, .index = -1};
    SgXAsm a = {.b = &hot};
    const char* why = NULL;
    sgx_vpool(&a, &sgx_v_movdqu_ld, 1, 1, 2); /* the hot code's own, first in the pool */
    sgx_cold_room(&a, 64);
    a.cold.index = -1;
    a.b = &a.cold;
    sgx_byte(&a.cold, 0x90); /* the cold code not at its buffer's start */
    sgx_vpool(&a, &sgx_v_movdqu_ld, 2, lo, hi);
    size_t at = a.rips[1].at, end = a.rips[1].end, base = hot.n;
    a.b = &hot;
    sgx_cold_emit(&a);
    sgx_pool_emit(&a);
    if (hot.n > hot.cap)
        why = "the code past its buffer";
    else {
        SnxU64 got[2];
        uint32_t d = snx_ld_le32(code + base + at);
        long long to = (long long)(base + end) + (d >> 31 ? (long long)d - 0x100000000ll : (long long)d);
        if (to < 0 || (size_t)to + 16 > hot.n)
            why = "a displacement out of the code";
        else {
            memcpy(got, code + to, 16);
            if (got[0] != lo || got[1] != hi) why = "a displacement to other bytes than its constant's";
        }
    }
    free(a.cold.p);
    free(a.rips);
    free(a.pool);
    return why;
}
