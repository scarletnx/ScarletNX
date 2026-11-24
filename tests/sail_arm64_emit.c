/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The A64 emitters sailgen generates with its arm64 back end (a64_arm64.inc: arm64.h and arm64_a64.h over the
   encoders of the A64 Sail, a64_enc.inc), in a translation unit of their own: sail_arm64.c runs what they write
   (under Unicorn on another host) beside the LLVM back end's. They build on any host: the code is arm64's */
#include "sail_arm64_emit.h"

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "a64_arm64.inc"

static SgCtx g_ctx; /* large: the block's SSA */

size_t snx_arm64_step(uint32_t word, uint64_t pc, uint64_t features, unsigned host_features, unsigned char* code,
                      size_t cap) {
    return sg_x64_step(&g_ctx, word, pc, features, host_features, code, cap);
}

size_t snx_arm64_block(const uint32_t* words, size_t* count, uint64_t pc, uint64_t features, unsigned host_features,
                       unsigned char* code, size_t cap) {
    return sg_x64_block(&g_ctx, words, count, pc, features, host_features, code, cap);
}

void snx_arm64_set_pool(int registers) { g_ctx.x.pool = registers; }

void snx_arm64_set_resolve(uint64_t (*resolve)(const char* sym)) { g_ctx.resolve = resolve; }
