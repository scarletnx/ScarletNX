/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The A64 emitters sailgen generates with a native back end other than the host's (SAIL_MACH_INC: a64_arm64.inc,
   a64_rv64.inc...), in a translation unit of their own: sail_mach.c runs what they write under Unicorn against
   QEMU and the recorded ends. They build on any host: the code is the machine's */
#include "sail_mach_emit.h"

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include SAIL_MACH_INC

static SgCtx g_ctx; /* large: the block's SSA */

size_t snx_mach_step(uint32_t word, uint64_t pc, uint64_t features, unsigned host_features, unsigned char* code,
                     size_t cap) {
    return sg_x64_step(&g_ctx, word, pc, features, host_features, code, cap);
}

size_t snx_mach_block(const uint32_t* words, size_t* count, uint64_t pc, uint64_t features, unsigned host_features,
                      unsigned char* code, size_t cap) {
    return sg_x64_block(&g_ctx, words, count, pc, features, host_features, code, cap);
}

void snx_mach_set_pool(int registers) { g_ctx.x.pool = registers; }
void snx_mach_set_xpool(int registers) { g_ctx.x.xpool = registers; }

void snx_mach_set_resolve(uint64_t (*resolve)(const char* sym)) { g_ctx.resolve = resolve; }
