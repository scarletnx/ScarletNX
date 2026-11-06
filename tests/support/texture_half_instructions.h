/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_TEXTURE_HALF_INSTRUCTIONS_H
#define SNX_TEST_TEXTURE_HALF_INSTRUCTIONS_H
#include "common/cpu/aarch64/cpu.h"

static inline u64 texture_half_instruction(u32 kind) {
    if (kind < 2)
        return 0xd000000000000000ull | (kind ? 1ull << 57 : 0) | 2ull << 53 | 74ull << 36 | 255ull << 28 | 5ull << 20 |
               7ull << 16 | 4ull << 8 | 8; /* TEXS.LZ or TLDS, R only, packed half */
    if (kind == 2)
        return 0xdf00000000000000ull | 1ull << 55 | 74ull << 36 | 9ull << 28 | 5ull << 20 | 7ull << 16 | 4ull << 8 |
               8; /* TLD4S: packed pairs in R8 andR9 */
    return 0x5d10000000070008ull | 1ull << 47 | 1ull << 28 | 255ull << 20 | 4ull << 8; /* HADD2: F32+0 */
}
#endif
