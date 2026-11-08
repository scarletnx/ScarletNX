/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (include/qemu/bitops.h), LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard */
/* The bit operations that the decoders tools/decodetree makes from .decode files call (QEMU's bitops.h): a field
   extracted, a field sign-extended from its top bit, and a field deposited into a value. The 32-bit forms serve
   decoders of 16-bit and 32-bit instructions, the 64-bit forms decoders made with -w 64. Each file that includes a
   decoder includes this header first */
#ifndef SNX_DECODETREE_H
#define SNX_DECODETREE_H

#include <stdbool.h>
#include <stdint.h>

static inline uint32_t extract32(uint32_t value, int start, int length) {
    return (value >> start) & (length == 32 ? 0xFFFFFFFFu : (1u << length) - 1);
}
static inline int32_t sextract32(uint32_t value, int start, int length) {
    uint32_t sign = 1u << (length - 1);
    return (int32_t)((extract32(value, start, length) ^ sign) - sign);
}
static inline uint32_t deposit32(uint32_t value, int start, int length, uint32_t field) {
    uint32_t mask = (length == 32 ? 0xFFFFFFFFu : (1u << length) - 1) << start;
    return (value & ~mask) | ((field << start) & mask);
}

static inline uint64_t extract64(uint64_t value, int start, int length) {
    return (value >> start) & (length == 64 ? ~0ull : (1ull << length) - 1);
}
static inline int64_t sextract64(uint64_t value, int start, int length) {
    uint64_t sign = 1ull << (length - 1);
    return (int64_t)((extract64(value, start, length) ^ sign) - sign);
}
static inline uint64_t deposit64(uint64_t value, int start, int length, uint64_t field) {
    uint64_t mask = (length == 64 ? ~0ull : (1ull << length) - 1) << start;
    return (value & ~mask) | ((field << start) & mask);
}

#endif
