/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Soundness of instruction-derived bit domains against actual integer values, not another domain transfer.
   Inputs vary independently; partial domains must contain every concrete result. No shader or GPU is executed. */
#include "nx/gpu/shader/translate.h"
#include <stdio.h>

static u32 checks, failures, state = 0x9365a6bd;
static void check(int pass, const char* what) {
    checks++;
    if (!pass && failures++ < 12) fprintf(stderr, "FAIL: %s\n", what);
}
static u32 random_word(void) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
static u64 move(u32 destination, u32 value) {
    return 0x0100000000071000ull | (u64)value << 20 | destination;
}
static u64 logic(u32 destination, u32 source, u32 value, u32 mode) {
    return 0x0400000000070000ull | (u64)value << 20 | (u64)mode << 53 | (u64)source << 8 | destination;
}
static u32 right_value(u64 word, int kind, const u32 registers[256]) {
    if (!kind) return registers[B(word, 20, 8)];
    if (kind == 1) return registers[254]; /* arbitrary constant-buffer word, independent of the register source */
    return B(word, 20, 19) | (B(word, 56, 1) ? 0xfff80000u : 0);
}
static int execute(u64 word, u32 registers[256]) {
    const Op* decoded = decode(word);
    if (!decoded) return 0;
    int op = decoded->id;
    u32 destination = B(word, 0, 8), source = registers[B(word, 8, 8)], value;
    if (op == O_MOV32I) value = B(word, 20, 32);
    else if (op == O_MOV_R) value = registers[B(word, 20, 8)];
    else if (op == O_LOP32I || op == O_LOP_R || op == O_LOP_C || op == O_LOP_I) {
        u32 right = op == O_LOP32I ? B(word, 20, 32) : right_value(word, op - O_LOP_R, registers);
        if (B(word, op == O_LOP32I ? 55 : 39, 1)) source = ~source;
        if (B(word, op == O_LOP32I ? 56 : 40, 1)) right = ~right;
        switch (B(word, op == O_LOP32I ? 53 : 41, 2)) {
            case 0: value = source & right; break;
            case 1: value = source | right; break;
            case 2: value = source ^ right; break;
            default: value = right; break;
        }
    } else if (op == O_SHL_R || op == O_SHL_C || op == O_SHL_I || op == O_SHR_R || op == O_SHR_C || op == O_SHR_I) {
        int right = op == O_SHR_R || op == O_SHR_C || op == O_SHR_I;
        u32 shift = right_value(word, op - (right ? O_SHR_R : O_SHL_R), registers);
        int wrap = B(word, 39, 1), sign = right && B(word, 48, 1);
        if (wrap) shift %= 32;
        if (shift >= 32) value = sign && (source >> 31) ? UINT32_MAX : 0;
        else {
            value = right ? source >> shift : source << shift;
            if (sign && shift && source >> 31) value |= UINT32_MAX << (32 - shift);
        }
    } else if (op == O_BFE_R || op == O_BFE_C || op == O_BFE_I) {
        u32 control = right_value(word, op - O_BFE_R, registers), offset = control & 255, width = control >> 8 & 255;
        int sign = B(word, 48, 1);
        if ((!sign && offset >= 32) || !width) value = 0;
        else {
            if (offset > 31) offset = 31;
            if (width > 32 - offset) width = 32 - offset;
            value = source >> offset;
            if (width < 32) {
                u32 mask = (1u << width) - 1;
                value &= mask;
                if (sign && (value >> (width - 1))) value |= ~mask;
            }
        }
    } else if (op == O_BFI_R || op == O_BFI_I) {
        u32 control = right_value(word, op == O_BFI_R ? 0 : 2, registers);
        u32 offset = control & 255, width = control >> 8 & 255;
        value = registers[B(word, 39, 8)];
        if (offset < 32)
            for (u32 bit = 0; bit < width && bit + offset < 32; bit++) {
                u32 target = 1u << (bit + offset);
                value = (value & ~target) | ((source >> bit & 1) ? target : 0);
            }
    } else return 0;
    if (destination < 255) registers[destination] = value;
    return 1;
}
static void test(const u64* operations, u32 count, u32 minimum_known, u32 expected_value) {
    u64 program[64] = {0};
    u32 index = 10;
    for (u32 i = 0; i < count; i++) {
        if ((index - 10) % 4 == 0) program[index++] = 0x001f8000fc0007e0ull;
        program[index++] = operations[i];
    }
    if ((index - 10) % 4 == 0) program[index++] = 0x001f8000fc0007e0ull;
    program[index] = 0xe30000000007000full;
    u32 known = UINT32_MAX, value = UINT32_MAX;
    int found = shader_direct_track_known_bits(program, index + 1, index, 6, &known, &value, 0);
    check(found == (known != 0) && !(value & ~known), "domain is canonical and reports unknown explicitly");
    check((known & minimum_known) == minimum_known && (value & minimum_known) == expected_value,
          "instruction domain retains its forced bits");
    for (u32 trial = 0; trial < 64; trial++) {
        u32 registers[256];
        for (u32 reg = 0; reg < 255; reg++) registers[reg] = random_word();
        registers[255] = 0;
        registers[40] = trial == 0 ? 0 : trial == 1 ? UINT32_MAX : trial < 34 ? 1u << (trial - 2) : random_word();
        int valid = 1;
        for (u32 at = 11; at < index; at++) if (IDX_OK(at)) valid &= execute(program[at], registers);
        check(valid && (registers[6] & known) == value, "every concrete integer result lies in the inferred domain");
    }
}
int main(void) {
    for (u32 mode = 0; mode < 4; mode++)
        for (u32 inverse = 0; inverse < 4; inverse++)
            for (u32 iteration = 0; iteration < 16; iteration++) {
                u32 immediate = random_word();
                u64 operation = logic(6, 40, immediate, mode) | (u64)(inverse & 1) << 55 | (u64)(inverse >> 1) << 56;
                test(&operation, 1, 0, 0);
                u64 basic[] = {move(12, immediate), 0x5c47000000070000ull | 12ull << 20 | 40ull << 8 | 6 |
                                                         (u64)mode << 41 | (u64)inverse << 39};
                test(basic, 2, 0, 0);
                u64 immediate19 = 0x3847000000070000ull | (u64)(immediate & 0x7ffff) << 20 | 40ull << 8 | 6 |
                                   (u64)mode << 41 | (u64)inverse << 39 | (u64)(iteration & 1) << 56;
                test(&immediate19, 1, 0, 0);
            }
    const u64 layer[] = {logic(6, 40, 0xffff, 0), logic(6, 6, 0x001f0000, 1)};
    test(layer, 2, 0xffff0000u, 0x001f0000u);
    const u64 x_only[] = {logic(6, 40, 0x000f0000, 0), logic(6, 6, 0x03200000, 1)};
    test(x_only, 2, 0xfff0ffffu, 0x03200000u);
    for (u32 shift = 0; shift < 64; shift++)
        for (u32 mode = 0; mode < 4; mode++) {
            u64 prefix[] = {logic(5, 40, 0x00ff00ffu, 0), logic(5, 5, 0x90010000u, 1),
                            0x3848000000070000ull | (u64)shift << 20 | 5ull << 8 | 6 | (u64)(mode & 1) << 39};
            test(prefix, 3, 0, 0);
            prefix[2] = 0x3828000000070000ull | (u64)shift << 20 | 5ull << 8 | 6 |
                        (u64)(mode & 1) << 39 | (u64)(mode >> 1) << 48;
            test(prefix, 3, 0, 0);
        }
    for (u32 offset = 0; offset <= 40; offset++)
        for (u32 width = 0; width <= 40; width++) {
            u32 control = offset | width << 8;
            for (u32 sign = 0; sign < 2; sign++) {
                u64 extract[] = {logic(5, 40, 0x00ff00ffu, 0), logic(5, 5, 0x90010000u, 1),
                                  0x3800000000070000ull | (u64)control << 20 | 5ull << 8 | 6 | (u64)sign << 48};
                test(extract, 3, 0, 0);
            }
            const u64 insert[] = {move(12, 0x03210000),
                                    0x36f0000000070000ull | (u64)control << 20 | 12ull << 39 | 40ull << 8 | 6};
            test(insert, 2, 0, 0);
        }
    const u64 copy[] = {move(5, 0x12345678), 0x5c98078000070000ull | 5ull << 20 | 6};
    test(copy, 2, UINT32_MAX, 0x12345678);
    const u64 zero_copy = 0x5c98078000070000ull | 255ull << 20 | 6;
    test(&zero_copy, 1, UINT32_MAX, 0);
    u64 uncertain[16] = {0};
    uncertain[10] = 0x001f8000fc0007e0ull;
    uncertain[11] = move(6, 7) & ~(15ull << 16); /* predicated P0: old R6 remains possible */
    uncertain[12] = 0xe30000000007000full;
    u32 known, value;
    check(!shader_direct_track_known_bits(uncertain, 13, 12, 6, &known, &value, 0) && !known && !value,
          "predicated definition remains unknown");
    uncertain[11] = logic(6, 40, 0xff, 0);
    check(!shader_direct_track_known_bits(uncertain, 13, 12, 6, &known, &value, 7) && !known && !value,
          "recursion limit remains unknown");
    uncertain[11] = 0xf0c8000002070006ull;
    check(!shader_direct_track_known_bits(uncertain, 13, 12, 6, &known, &value, 0) && !known && !value,
          "unrestricted system input remains unknown");
    printf("shader known bits: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
