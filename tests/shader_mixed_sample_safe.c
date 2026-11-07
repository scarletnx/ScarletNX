/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Only a source proven independent of invocation/sample inputs may be reduced from its first surviving sample.
   Exercise actual translation: ordinary color inputs, coverage, sample IDs, clocks and quad-local operations must
   clear a stale proof, even when they happen to return the same value in one synthetic invocation. */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>

static u64 immediate(u32 destination, u32 value) {
    return 0x0100000000071000ull | (u64)value << 20 | destination;
}
static int proof(const char* name, u64 extra, u32 output, int stage, int expected, int split) {
    u64 program[32] = {0};
    program[9] = stage == ST_FS ? 15ull | (u64)output << 32 : 0;
    for (u32 i = 10; i < 32; i += 4) program[i] = 0x001f8000fc0007e0ull;
    u32 at = 11;
    for (u32 i = 0; i < 4; i++) {
        if (at % 4 == 2) at++;
        program[at++] = immediate(i, i == 3 ? 0x3f800000u : 0x3f000000u);
    }
    if (extra) {
        if (at % 4 == 2) at++;
        program[at++] = extra;
    }
    if (at % 4 == 2) at++;
    program[at] = 0xe30000000007000full;
    u8 bytes[sizeof program];
    for (u32 i = 0; i < 32; i++) snx_st_le64(bytes + 8 * i, program[i]);
    ShaderInfo info = {.mixed_sample_invariant = 1, .primitive_split_safe = 1};
    size_t words = 0;
    u32* module = shader_to_spirv(bytes, sizeof bytes, stage, &info, &words, 0, 1, 1, 1);
    int good = module && !info.unsupported && info.mixed_sample_invariant == expected && info.primitive_split_safe == split;
    if (!good)
        fprintf(stderr, "%s: module=%d invariant=%d/%d split=%d/%d: %s\n", name, module != NULL,
                info.mixed_sample_invariant, expected, info.primitive_split_safe, split, info.first_unsupported);
    free(module);
    return good;
}
int main(void) {
    g_shader_clock_scopes = 3; /* Include both optional clock forms in the rejected-invariance cases. */
    const struct { const char* name; u64 instruction; int safe; } cases[] = {
        {"literal color", 0, 1},
        {"uniform constant load", 0xef9400000007ff00ull, 1},
        {"uniform arithmetic", 0x5c58000000170000ull, 1},
        {"varying color", 0xe000000000070000ull | 0x20ull << 30, 0},
        {"lane identifier", 0xf0c8000000070000ull, 0},
        {"clock", 0x50c8000005070000ull, 0},
        {"coverage mask", 0xefe8e0008007ff03ull, 0},
        {"sample identifier", 0xefe8e0028007ff02ull, 0},
        {"quad arithmetic", 0x50f8000000070000ull | 0x99ull << 28 | 2ull << 20 | 1ull << 8, 0},
        {"shuffle", 0xef17007cf0370207ull, 0},
    };
    int good = 1;
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++)
        good &= proof(cases[i].name, cases[i].instruction, 0, ST_FS, cases[i].safe, 0);
    good &= proof("guest sample-mask output", 0, 1, ST_FS, 0, 0);
    good &= proof("guest depth output", 0, 2, ST_FS, 0, 0);
    good &= proof("independent vertex stage", 0, 0, ST_VS, 0, 1);
    good &= proof("vertex lane groups", 0xf0c8000000070000ull, 0, ST_VS, 0, 0);
    good &= proof("vertex clock", 0x50c8000005070000ull, 0, ST_VS, 0, 0);
    good &= proof("vertex shuffle", 0xef17007cf0370207ull, 0, ST_VS, 0, 0);
    good &= proof("vertex vote", 0x50d8000000070000ull | 7ull << 39, 0, ST_VS, 0, 0);
    ShaderInfo rejected = {.mixed_sample_invariant = 1, .primitive_split_safe = 1};
    size_t words = 0;
    u32* invalid = shader_to_spirv(NULL, 0, ST_FS, &rejected, &words, 0, 1, 1, 1);
    good &= !invalid && !rejected.mixed_sample_invariant && !rejected.primitive_split_safe;
    free(invalid);
    u8 short_program[80] = {0};
    rejected.mixed_sample_invariant = rejected.primitive_split_safe = 1;
    invalid = shader_to_spirv(short_program, sizeof short_program, ST_FS, &rejected, &words, 0, 1, 1, 1);
    good &= !invalid && !rejected.mixed_sample_invariant && !rejected.primitive_split_safe;
    free(invalid);
    if (good) puts("PASS shader mixed-sample source invariance: uniform sources and invocation hazards checked");
    return !good;
}
