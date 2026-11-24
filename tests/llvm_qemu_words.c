/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* llvm_qemu_differential's words from the generated decoder (llvm_qemu_words.h) */
#include "llvm_qemu_words.h"

#define SNX_A64_DECODE_FIELDS
#include "a64_decode.h"
#include "a64_decode.inc"

#include <stdlib.h>

#define ROWS     (sizeof(snx_a64_rows) / sizeof(snx_a64_rows[0]))
#define ATTEMPTS 64

/* value's low bits spread over the set bits of mask, lowest first */
static uint32_t deposit(uint32_t value, uint32_t mask) {
    uint32_t out = 0, bit;
    for (bit = 1; mask; bit <<= 1) {
        uint32_t low = mask & (0u - mask);
        if (value & bit) out |= low;
        mask &= mask - 1;
    }
    return out;
}

static unsigned popcount(uint32_t x) {
    unsigned n = 0;
    for (; x; x &= x - 1) ++n;
    return n;
}

static int push(uint32_t** out, size_t* count, size_t* capacity, uint32_t word) {
    if (*count == *capacity) {
        size_t cap = *capacity ? *capacity * 2 : 4096;
        uint32_t* p = realloc(*out, cap * sizeof(**out));
        if (!p) return -1;
        *out = p;
        *capacity = cap;
    }
    (*out)[(*count)++] = word;
    return 0;
}

static size_t row_words(unsigned samples, LqdRandom random, uint64_t without, uint32_t** out) {
    size_t count = 0, capacity = 0, i;
    *out = NULL;
    for (i = 0; i < ROWS; ++i) {
        const SnxA64Row* row = &snx_a64_rows[i];
        uint32_t bases[2], first = row->value, second = row->value;
        unsigned s, k, b;
        if (row->feature & without) continue;
        for (s = 0; s < samples; ++s) {
            uint32_t word = row->value;
            unsigned attempt;
            for (attempt = 0; attempt < ATTEMPTS; ++attempt) {
                SnxA64Insn insn;
                uint32_t free_bits = s == 0 && !attempt ? 0 : s == 1 && !attempt ? ~0u : (uint32_t)random();
                word = row->value | (free_bits & ~row->mask);
                if (snx_a64_decode(word, SNX_A64_TARGET_MAX, &insn) == (SnxA64Op)row->op) break;
            }
            if (!s) first = word;
            if (s == 1) second = word;
            if (push(out, &count, &capacity, word)) return count;
        }
        /* every value of each small field, from the sample with no free bit set and from the one with all of them
           (when it decodes): an AdvSIMD row's Q and size fields then meet in both halves' lane sizes */
        bases[0] = first;
        bases[1] = second;
        for (b = 0; b < (samples > 1 && second != first ? 2u : 1u); ++b)
            for (k = 0; k < snx_a64_row_fields[i].count; ++k) {
                const uint32_t mask = snx_a64_field_masks[snx_a64_row_fields[i].first + k];
                uint32_t v;
                if (popcount(mask) > 3) continue;
                for (v = 0; v < 1u << popcount(mask); ++v)
                    if (push(out, &count, &capacity, (bases[b] & ~mask) | deposit(v, mask))) return count;
            }
    }
    return count;
}
size_t lqd_row_words(unsigned samples, LqdRandom random, uint32_t** out) {
    return row_words(samples, random, SNX_A64_FEAT_SME | SNX_A64_FEAT_MOPS, out);
}
size_t lqd_all_row_words(unsigned samples, LqdRandom random, uint32_t** out) {
    return row_words(samples, random, 0, out);
}

const char* lqd_op_name(uint32_t word, int* unrunnable) {
    SnxA64Insn insn;
    const uint32_t op0 = (word >> 25) & 0xF;
    *unrunnable = 0;
    if (snx_a64_decode(word, SNX_A64_TARGET_MAX, &insn) != SNX_A64_OP_UNDEF) {
        *unrunnable = (insn.feature & (SNX_A64_FEAT_SME | SNX_A64_FEAT_MOPS)) != 0;
        return snx_a64_op_name(insn.op);
    }
    /* SVE's encodings have op0 (bits 28-25) 0010, SME's bit 31 set and op0 0000 */
    return op0 == 2 ? "(SVE space)" : (word >> 31) && !op0 ? "(SME space)" : "(unallocated)";
}

const char* lqd_switch_op_name(uint32_t word) {
    SnxA64Insn insn;
    return snx_a64_decode(word, SNX_A64_TARGET_SWITCH, &insn) != SNX_A64_OP_UNDEF ? snx_a64_op_name(insn.op) : NULL;
}

size_t lqd_rows(void) { return ROWS; }
