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
u64 snx_x64_ldx(CPU* c, u64 a, u32 sz);
u32 snx_x64_stx(CPU* c, u64 a, u32 sz, u64 v);
u64 snx_x64_ldxp(CPU* c, u64 a, u32 sz);
u32 snx_x64_stxp(CPU* c, u64 a, u32 sz, u64 v1, u64 v2);
u64 snx_x64_rmw(CPU* c, u64 a, u64 v, u32 op);
u64 snx_x64_cas(CPU* c, u64 a, u64 expected, u64 desired, u32 sz);
u64 snx_x64_cas128(CPU* c, u64 a, u64 e_lo, u64 e_hi, u64 d_lo, u64 d_hi);
u32 snx_x64_crc32(u32 acc, u64 value, u32 bytes, u32 castagnoli);
static SnxU64 resolve(const char* sym) {
    static const struct {
        const char* name;
        void* f;
    } table[] = {{"snx_x64_ldx", (void*)snx_x64_ldx},       {"snx_x64_stx", (void*)snx_x64_stx},
                 {"snx_x64_ldxp", (void*)snx_x64_ldxp},     {"snx_x64_stxp", (void*)snx_x64_stxp},
                 {"snx_x64_rmw", (void*)snx_x64_rmw},       {"snx_x64_cas", (void*)snx_x64_cas},
                 {"snx_x64_cas128", (void*)snx_x64_cas128}, {"snx_x64_crc32", (void*)snx_x64_crc32}};
    for (size_t i = 0; i < sizeof table / sizeof *table; ++i)
        if (!strcmp(sym, table[i].name)) return (SnxU64)(uintptr_t)table[i].f;
    return 0;
}
__attribute__((constructor)) static void set_resolver(void) { g_ctx.resolve = resolve; }
