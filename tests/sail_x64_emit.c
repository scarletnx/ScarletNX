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
