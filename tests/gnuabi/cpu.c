/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
#include "guest.h"

extern u64 boundary_probe(u64);
typedef u32 vec4 [[gnu::vector_size(16)]];
static volatile u64 input[] = {0xfedcba9876543210UL, 0x123456789abcdef1UL, 63, 17};
static volatile double fp_input[] = {1.5, -2.25, 4.0};
static volatile vec4 simd_input[] = {{1, 0x80000000U, 7, 0xffffffffU}, {3, 11, 19, 23}};
static u64 atomic_word;

int guest_main(u64* stack) {
    u64 a = input[0], b = input[1], result = a + b;
    result ^= (a * b) + (a / (b | 1));
    result ^= (a << (input[2] & 63)) | (b >> (input[3] & 63));
    result ^= (u64)((__uint128_t)a * b >> 64);
    result ^= (u64)((s64)a >> 13);
    emit_hex(result);

    for (u64 i = 0; i != 129; ++i) {
        if ((i & 3) == 1)
            result += i * 13;
        else if (i & 8)
            result ^= (result >> 7) + i;
        else
            result = (result << 1) | (result >> 63);
    }
    emit_hex(result);

    u8 memory[259];
    for (u64 i = 0; i < sizeof(memory); ++i) memory[i] = (u8)(i * 37 + 9);
    result = 0;
    for (u64 i = 1; i + 8 < sizeof(memory); i += 7) {
        u64 word;
        __asm__ volatile("ldr %0, [%1]" : "=r"(word) : "r"(memory + i) : "memory");
        result = (result ^ word) + i;
    }
    emit_hex(result);

    double fp = fp_input[0] * fp_input[1] + fp_input[2];
    union {
        double d;
        u64 u;
    } bits = {fp};
    emit_hex(bits.u);
    emit_hex((u64)(s64)(fp * 1024.0));

    vec4 lanes = (simd_input[0] + simd_input[1]) ^ (simd_input[0] >> 3);
    emit_hex(((u64)lanes[0] << 32) | lanes[1]);
    emit_hex(((u64)lanes[2] << 32) | lanes[3]);

    u64 old, updated;
    u32 failed;
    do {
        __asm__ volatile("ldaxr %0, [%3]\nadd %1, %0, #23\nstlxr %w2, %1, [%3]"
                         : "=&r"(old), "=&r"(updated), "=&r"(failed)
                         : "r"(&atomic_word)
                         : "memory");
    } while (failed);
    __asm__ volatile("dmb ish" ::: "memory");
    emit_hex(atomic_word);
    emit_hex(boundary_probe(result));
    /* Initial stack must be aligned and argc must include the executable. */
    emit_hex(((u64)stack & 15) | (stack[0] == 1 ? 0x100UL : 0));
    return 0;
}
