/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Execute the emitted scalar SSA against a separate state-machine oracle. No Vulkan device is required.
   Optional argv1 receives complete compute/fragment modules for external SPIR-V validation. */
#include "nx/gpu/shader/depth_stencil_spirv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { INPUT_DEPTH = 16, OLD_DEPTH, OLD_STENCIL, FRONT, COVERED, RESULT_PASS, RESULT_DEPTH, RESULT_STENCIL,
       TEST_WORDS };
typedef struct {
    uint32_t* word;
    uint32_t words, mutable_begin, mutable_end, reads, writes;
} TestMemory;
static struct { unsigned checks, failures; uint32_t random; } g_test;
static void check(int pass, const char* text) {
    g_test.checks++;
    if (!pass && g_test.failures++ < 12) fprintf(stderr, "FAIL: %s\n", text);
}
static uint32_t word(float value) { uint32_t result; memcpy(&result, &value, sizeof result); return result; }
static float real(uint32_t value) { float result; memcpy(&result, &value, sizeof result); return result; }
static uint32_t random_word(void) {
    uint32_t value = g_test.random;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    g_test.random = value;
    return value;
}
static int comparison(uint32_t op, float reference, float stored) {
    switch (op) {
        case 0: return 0;
        case 1: return reference < stored;
        case 2: return reference == stored;
        case 3: return reference <= stored;
        case 4: return reference > stored;
        case 5: return reference != stored;
        case 6: return reference >= stored;
        case 7: return 1;
        default: return 0;
    }
}
static uint32_t stencil_op(uint32_t operation, uint32_t old, uint32_t reference) {
    switch (operation) {
        case 0: return old;
        case 1: return 0;
        case 2: return reference;
        case 3: return old < 255 ? old + 1 : 255;
        case 4: return old ? old - 1 : 0;
        case 5: return old ^ 255;
        case 6: return (old + 1) % 256;
        case 7: return (old + 255) % 256;
        default: return old;
    }
}
static void reference(const uint32_t input[TEST_WORDS], uint32_t output[3]) {
    uint32_t flags = input[SH_DS_FLAGS];
    float incoming = real(input[INPUT_DEPTH]), old_depth = real(input[OLD_DEPTH]);
    uint32_t old_stencil = input[OLD_STENCIL];
    output[0] = 0;
    output[1] = input[OLD_DEPTH];
    output[2] = old_stencil;
    if (!input[COVERED] || (flags & SH_DS_FIXED_MASK_OFF)) return;
    if ((flags & SH_DS_DEPTH_BOUNDS) &&
        !(old_depth >= real(input[SH_DS_BOUNDS_MIN]) && old_depth <= real(input[SH_DS_BOUNDS_MAX]))) return;
    if (flags & SH_DS_DEPTH_CLAMP) {
        if (incoming < real(input[SH_DS_CLAMP_MIN])) incoming = real(input[SH_DS_CLAMP_MIN]);
        if (incoming > real(input[SH_DS_CLAMP_MAX])) incoming = real(input[SH_DS_CLAMP_MAX]);
    }
    unsigned face = input[FRONT] ? SH_DS_FRONT_CONTROL : SH_DS_BACK_CONTROL;
    uint32_t control = input[face], values = input[face + 1];
    uint32_t ref = values & 255, cmask = values >> 8 & 255, wmask = values >> 16 & 255;
    int stencil_pass = !(flags & SH_DS_STENCIL_TEST) || comparison(control & 7, (float)(ref & cmask),
                                                                                  (float)(old_stencil & cmask));
    int depth_pass = !(flags & SH_DS_DEPTH_TEST) || comparison(flags >> SH_DS_COMPARE_SHIFT & 7, incoming, old_depth);
    if (flags & SH_DS_STENCIL_TEST) {
        uint32_t operation = !stencil_pass ? control >> 3 & 7 : !depth_pass ? control >> 9 & 7 : control >> 6 & 7;
        uint32_t value = stencil_op(operation, old_stencil, ref);
        output[2] = (old_stencil & (255 - wmask)) | (value & wmask);
    }
    if (!stencil_pass || !depth_pass) return;
    output[0] = 1;
    if ((flags & (SH_DS_DEPTH_TEST | SH_DS_DEPTH_WRITE)) == (SH_DS_DEPTH_TEST | SH_DS_DEPTH_WRITE))
        output[1] = incoming >= 0 && incoming <= 1 ? word(incoming) : 0;
}
static uint32_t buffer_word(SpvEmitter* e, uint32_t u, uint32_t buffer, uint32_t slot) {
    return SPV(e, SPV_OP_ACCESS_CHAIN, spv_emit_type_storage_pointer(e, u), buffer,
        spv_emit_constant_u32(e, u, 0), spv_emit_constant_u32(e, u, slot));
}
static uint32_t* make_scalar_module(size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 4096);
    spv_emit_shader_capability(&e);
    uint32_t u = spv_emit_type_u32(&e), f = spv_emit_type_f32(&e), b = spv_emit_type_bool(&e);
    uint32_t buffer = shader_depth_stencil_declare(&e, u, 0, 0);
    uint32_t function = spv_emit_function(&e, 0, spv_emit_type_void(&e), NULL, 0, NULL);
    uint32_t input[RESULT_PASS];
    for (uint32_t i = 0; i < RESULT_PASS; i++) input[i] = spv_emit_load(&e, u, buffer_word(&e, u, buffer, i));
    uint32_t zero = spv_emit_constant_u32(&e, u, 0);
    ShaderDepthStencilResult result = shader_depth_stencil_evaluate(&e, f, u, b, input,
        spv_emit_bitcast(&e, f, input[INPUT_DEPTH]), input[OLD_DEPTH], input[OLD_STENCIL],
        spv_emit_i_not_equal(&e, b, input[FRONT], zero), spv_emit_i_not_equal(&e, b, input[COVERED], zero));
    spv_emit_store(&e, buffer_word(&e, u, buffer, RESULT_PASS), spv_emit_select(&e, u, result.pass,
        spv_emit_constant_u32(&e, u, 1), zero));
    spv_emit_store(&e, buffer_word(&e, u, buffer, RESULT_DEPTH), result.depth);
    spv_emit_store(&e, buffer_word(&e, u, buffer, RESULT_STENCIL), result.stencil);
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    spv_emit_compute_entry_str(&e, function, "main", &buffer, 1);
    spv_emit_local_size(&e, function, 1, 1, 1);
    uint32_t* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}
/* A small literal executor, independent of the emitter's source. It rejects unrecognized executable opcodes.
   Pointer values are checked array indices, not C pointer casts. Arithmetic values retain their raw uint bits. */
static size_t label_offset(const uint32_t* code, size_t words, uint32_t label) {
    for (size_t at = 5; at < words;) {
        uint32_t count = code[at] >> 16;
        if (!count || count > words - at) return words;
        if ((code[at] & 0xffff) == SPV_OP_LABEL && count == 2 && code[at + 1] == label) return at;
        at += count;
    }
    return words;
}
static int execute(const uint32_t* code, size_t words, TestMemory* memory, uint32_t* value, uint8_t* pointer) {
    memset(value, 0, (size_t)code[3] * sizeof *value);
    memset(pointer, 0, code[3]);
    int function = 0;
    uint32_t block = 0, previous = 0;
    size_t instructions = 0;
    for (size_t at = 5; at < words;) {
        if (++instructions > words * 2) return 0;
        const uint32_t* p = code + at;
        uint32_t width = p[0] >> 16, op = p[0] & 0xffff;
        if (!width || width > words - at) return 0;
        at += width;
        if (op == SPV_OP_FUNCTION) { function = 1; continue; }
        if (op == SPV_OP_FUNCTION_END) { function = 0; continue; }
        if (op == SPV_OP_CONSTANT && width == 4) { value[p[2]] = p[3]; continue; }
        if (op == SPV_OP_CONSTANT_TRUE || op == SPV_OP_CONSTANT_FALSE) {
            if (width != 3) return 0;
            value[p[2]] = op == SPV_OP_CONSTANT_TRUE;
            continue;
        }
        if (!function || op == SPV_OP_RETURN) continue;
        if (op == SPV_OP_LABEL && width == 2) { previous = block; block = p[1]; continue; }
        if (op == SPV_OP_SELECTION_MERGE && width == 3) continue;
        if (op == SPV_OP_BRANCH && width == 2) {
            at = label_offset(code, words, p[1]);
            if (at == words) return 0;
            continue;
        }
        if (op == SPV_OP_BRANCH_CONDITIONAL && width == 4) {
            at = label_offset(code, words, p[value[p[1]] ? 2 : 3]);
            if (at == words) return 0;
            continue;
        }
        if (op == SPV_OP_PHI && width >= 5 && (width & 1)) {
            uint32_t found = 0;
            for (uint32_t i = 3; i + 1 < width; i += 2)
                if (p[i + 1] == previous) { value[p[2]] = value[p[i]]; found++; }
            if (found != 1) return 0;
            continue;
        }
        if (op == SPV_OP_ACCESS_CHAIN) {
            if (width != 6 || value[p[4]] || value[p[5]] >= memory->words) return 0;
            pointer[p[2]] = 1;
            value[p[2]] = value[p[5]];
            continue;
        }
        if (op == SPV_OP_LOAD) {
            if (width != 4 || !pointer[p[3]] || value[p[3]] >= memory->words) return 0;
            uint32_t index = value[p[3]];
            value[p[2]] = memory->word[index];
            if (index >= memory->mutable_begin && index < memory->mutable_end) memory->reads++;
            continue;
        }
        if (op == SPV_OP_STORE) {
            if (width != 3 || !pointer[p[1]] || value[p[1]] >= memory->words) return 0;
            uint32_t index = value[p[1]];
            memory->word[index] = value[p[2]];
            if (index >= memory->mutable_begin && index < memory->mutable_end) memory->writes++;
            continue;
        }
        if ((op == SPV_OP_ATOMIC_LOAD && width == 6) || (op == SPV_OP_ATOMIC_XOR && width == 7)) {
            if (!pointer[p[3]] || value[p[3]] >= memory->words || value[p[4]] != 1 || value[p[5]]) return 0;
            uint32_t index = value[p[3]];
            value[p[2]] = memory->word[index];
            if (index >= memory->mutable_begin && index < memory->mutable_end) memory->reads++;
            if (op == SPV_OP_ATOMIC_XOR) {
                memory->word[index] ^= value[p[6]];
                if (index >= memory->mutable_begin && index < memory->mutable_end) memory->writes++;
            }
            continue;
        }
        if (width == 4) {
            uint32_t a = value[p[3]];
            if (op == SPV_OP_BITCAST) value[p[2]] = a;
            else if (op == SPV_OP_LOGICAL_NOT) value[p[2]] = !a;
            else return 0;
            continue;
        }
        if (op == SPV_OP_SELECT && width == 6) {
            value[p[2]] = value[value[p[3]] ? p[4] : p[5]];
            continue;
        }
        if (width != 5) return 0;
        uint32_t a = value[p[3]], b = value[p[4]], result;
        switch (op) {
            case SPV_OP_I_ADD: result = a + b; break;
            case SPV_OP_I_SUB: result = a - b; break;
            case SPV_OP_I_MUL: result = a * b; break;
            case SPV_OP_SHIFT_LEFT_LOGICAL: if (b > 31) return 0; result = a << b; break;
            case SPV_OP_SHIFT_RIGHT_LOGICAL: if (b > 31) return 0; result = a >> b; break;
            case SPV_OP_BITWISE_AND: result = a & b; break;
            case SPV_OP_BITWISE_OR: result = a | b; break;
            case SPV_OP_BITWISE_XOR: result = a ^ b; break;
            case SPV_OP_I_EQUAL: result = a == b; break;
            case SPV_OP_I_NOT_EQUAL: result = a != b; break;
            case SPV_OP_U_LESS_THAN: result = a < b; break;
            case SPV_OP_U_LESS_THAN_EQUAL: result = a <= b; break;
            case SPV_OP_U_GREATER_THAN: result = a > b; break;
            case SPV_OP_U_GREATER_THAN_EQUAL: result = a >= b; break;
            case SPV_OP_F_ORD_LESS_THAN: result = real(a) < real(b); break;
            case SPV_OP_F_ORD_LESS_THAN_EQUAL: result = real(a) <= real(b); break;
            case SPV_OP_F_ORD_GREATER_THAN: result = real(a) > real(b); break;
            case SPV_OP_F_ORD_GREATER_THAN_EQUAL: result = real(a) >= real(b); break;
            case SPV_OP_F_ORD_EQUAL: result = real(a) == real(b); break;
            case SPV_OP_F_UNORD_NOT_EQUAL: result = real(a) != real(b); break;
            case SPV_OP_LOGICAL_AND: result = a && b; break;
            case SPV_OP_LOGICAL_OR: result = a || b; break;
            default: return 0;
        }
        value[p[2]] = result;
    }
    return !function;
}
static int save(const char* directory, const char* name, const uint32_t* code, size_t words) {
    if (!directory) return 1;
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/%s.spv", directory, name);
    if (n < 0 || n >= (int)sizeof path) return 0;
    FILE* file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fwrite(code, sizeof *code, words, file) == words;
    return !fclose(file) && ok;
}
static void test_case(const uint32_t* code, size_t words, uint32_t memory[TEST_WORDS], uint32_t* value,
                        uint8_t* pointer) {
    uint32_t expected[3];
    reference(memory, expected);
    TestMemory storage = {.word = memory, .words = TEST_WORDS};
    check(execute(code, words, &storage, value, pointer), "emitted scalar SSA executes using only supported instructions");
    check(memory[RESULT_PASS] == expected[0], "late color pass matches independent ordered depth/stencil state machine");
    check(memory[RESULT_DEPTH] == expected[1], "raw D32F write or retention is bit exact");
    check(memory[RESULT_STENCIL] == expected[2], "front/back fail/depth-fail/pass and write masks are exact");
}
static void scalar_cases(const uint32_t* code, size_t words) {
    uint32_t* value = calloc(code[3], sizeof *value);
    uint8_t* pointer = calloc(code[3], 1);
    check(value && pointer, "scalar executor storage allocation");
    if (!value || !pointer) { free(value); free(pointer); return; }
    uint32_t memory[TEST_WORDS] = {0};
    memory[SH_DS_BOUNDS_MIN] = word(0.25f); memory[SH_DS_BOUNDS_MAX] = word(0.75f);
    memory[SH_DS_CLAMP_MIN] = word(0.125f); memory[SH_DS_CLAMP_MAX] = word(0.875f);
    /* Each byte value, each stencil operation, each fail stage and both faces, including different face state. */
    for (uint32_t old = 0; old < 256; old++)
        for (uint32_t operation = 0; operation < 8; operation++)
            for (uint32_t stage = 0; stage < 3; stage++)
                for (uint32_t front = 0; front < 2; front++) {
                    memory[FRONT] = front; memory[COVERED] = 1;
                    memory[OLD_DEPTH] = word(0.5f); memory[INPUT_DEPTH] = word(stage == 1 ? 0.75f : 0.25f);
                    memory[OLD_STENCIL] = old;
                    memory[SH_DS_FLAGS] = SH_DS_DEPTH_TEST | SH_DS_DEPTH_WRITE | SH_DS_STENCIL_TEST |
                                          1u << SH_DS_COMPARE_SHIFT;
                    uint32_t control = (stage == 0 ? 0u : 7u) | operation << 3 | operation << 6 | operation << 9;
                    memory[front ? SH_DS_FRONT_CONTROL : SH_DS_BACK_CONTROL] = control;
                    memory[front ? SH_DS_BACK_CONTROL : SH_DS_FRONT_CONTROL] = 0;
                    memory[front ? SH_DS_FRONT_VALUES : SH_DS_BACK_VALUES] = 0xff0000u | 0xff00u | (255 - old);
                    memory[front ? SH_DS_BACK_VALUES : SH_DS_FRONT_VALUES] = 0;
                    test_case(code, words, memory, value, pointer);
                }
    static const uint32_t depths[] = {0, 0x80000000u, 1, 0x3dffffffu, 0x3e000000u, 0x3e800000u,
        0x3f000000u, 0x3f400000u, 0x3f600000u, 0x3f800000u, 0x3f800001u, 0xbf000000u,
        0x7f800000u, 0xff800000u, 0x7fc00001u};
    for (uint32_t compare_op = 0; compare_op < 8; compare_op++)
        for (uint32_t incoming = 0; incoming < sizeof depths / sizeof *depths; incoming++)
            for (uint32_t stored = 0; stored < sizeof depths / sizeof *depths; stored++)
                for (uint32_t flags = 0; flags < 64; flags++) {
                    memory[INPUT_DEPTH] = depths[incoming]; memory[OLD_DEPTH] = depths[stored];
                    memory[OLD_STENCIL] = 0x7f; memory[FRONT] = flags & 1; memory[COVERED] = flags & 2;
                    memory[SH_DS_FLAGS] = flags | compare_op << SH_DS_COMPARE_SHIFT;
                    memory[SH_DS_FRONT_CONTROL] = 7 | 3 << 3 | 4 << 6 | 5 << 9;
                    memory[SH_DS_BACK_CONTROL] = 1 | 6 << 3 | 7 << 6 | 2 << 9;
                    memory[SH_DS_FRONT_VALUES] = 0xa5f08cu; memory[SH_DS_BACK_VALUES] = 0x5a0f3cu;
                    test_case(code, words, memory, value, pointer);
                }
    for (uint32_t i = 0; i < 20000; i++) {
        memory[SH_DS_FLAGS] = random_word() & 63u; memory[SH_DS_FLAGS] |= (random_word() & 7u) << SH_DS_COMPARE_SHIFT;
        memory[SH_DS_FRONT_CONTROL] = random_word() & 4095; memory[SH_DS_BACK_CONTROL] = random_word() & 4095;
        memory[SH_DS_FRONT_VALUES] = random_word() & 0xffffffu;
        memory[SH_DS_BACK_VALUES] = random_word() & 0xffffffu;
        memory[INPUT_DEPTH] = word((float)(random_word() & 1023) / 1024.0f);
        memory[OLD_DEPTH] = word((float)(random_word() & 1023) / 1024.0f);
        memory[OLD_STENCIL] = random_word() & 255; memory[FRONT] = random_word() & 1;
        memory[COVERED] = random_word() & 1;
        test_case(code, words, memory, value, pointer);
    }
    free(value); free(pointer);
}
static void layout_cases(void) {
    uint32_t depth = 99, stencil = 99;
    check(shader_depth_stencil_layout(1, 1, 1, &depth, &stencil) == 72 && depth == 16 && stencil == 17,
          "one pixel allocates a full trailing packed stencil word");
    check(shader_depth_stencil_layout(3, 5, 1, &depth, &stencil) == 140 && depth == 16 && stencil == 31,
          "odd extents retain tightly packed S8 byte-plane layout");
    check(shader_depth_stencil_layout(3, 5, 0, &depth, &stencil) == 124 && depth == 16 && !stencil,
          "depth-only shape needs no stencil plane");
    check(!shader_depth_stencil_layout(UINT32_MAX, UINT32_MAX, 1, &depth, &stencil) && !depth && !stencil,
          "large dimensions reject before uint64 size overflow");
    check(!shader_depth_stencil_layout(0, 1, 1, &depth, &stencil) && !depth && !stencil,
          "empty shape clears both output offsets");
    /* Model all24 schedules of independent neighboring-byte updates. Whole-word stale stores would lose bytes. */
    for (uint32_t a = 0; a < 4; a++) for (uint32_t b = 0; b < 4; b++)
        for (uint32_t c = 0; c < 4; c++) for (uint32_t d = 0; d < 4; d++) {
            if (a == b || a == c || a == d || b == c || b == d || c == d) continue;
            uint32_t indices[4] = {a,b,c,d}, initial = 0xa57f00ffu, expected = 0x5a80ff00u, packed = initial;
            for (uint32_t i = 0; i < 4; i++) {
                uint32_t shift = indices[i] * 8;
                packed ^= ((initial >> shift ^ expected >> shift) & 255u) << shift;
            }
            check(packed == expected, "neighbor stencil-byte atomic XOR updates commute in every schedule");
        }
}
enum { GUARDED_INDEX = 24, GUARDED_DEPTH, GUARDED_FRONT, GUARDED_COVERED, GUARDED_RESULT, GUARDED_WORDS = 32 };
static uint32_t* make_guarded_module(size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 4096);
    spv_emit_shader_capability(&e);
    uint32_t u = spv_emit_type_u32(&e), f = spv_emit_type_f32(&e), b = spv_emit_type_bool(&e);
    uint32_t buffer = shader_depth_stencil_declare(&e, u, 0, 0);
    uint32_t function = spv_emit_function(&e, 0, spv_emit_type_void(&e), NULL, 0, NULL);
    uint32_t input[4];
    for (uint32_t i = 0; i < 4; i++) input[i] = spv_emit_load(&e, u, buffer_word(&e, u, buffer, GUARDED_INDEX + i));
    uint32_t zero = spv_emit_constant_u32(&e, u, 0);
    uint32_t result = shader_depth_stencil_test(&e, f, u, b, buffer, input[0],
        spv_emit_bitcast(&e, f, input[1]), spv_emit_i_not_equal(&e, b, input[2], zero),
        spv_emit_i_not_equal(&e, b, input[3], zero));
    spv_emit_store(&e, buffer_word(&e, u, buffer, GUARDED_RESULT), spv_emit_select(&e, u, result,
        spv_emit_constant_u32(&e, u, 1), zero));
    spv_emit_return(&e); spv_emit_function_end(&e);
    spv_emit_compute_entry_str(&e, function, "main", &buffer, 1);
    spv_emit_local_size(&e, function, 1, 1, 1);
    uint32_t* code = spv_emit_finish(&e, words); spv_emit_destroy(&e);
    return code;
}
static void guard_cases(const uint32_t* code, size_t words) {
    uint32_t* values = calloc(code[3], sizeof *values);
    uint8_t* pointers = calloc(code[3], 1);
    check(values && pointers, "guarded shadow executor storage allocation");
    if (!values || !pointers) { free(values); free(pointers); return; }
    for (uint32_t covered = 0; covered < 2; covered++)
        for (uint32_t fixed_off = 0; fixed_off < 2; fixed_off++)
            for (uint32_t stencil = 0; stencil < 2; stencil++)
                for (uint32_t invalid = 0; invalid < 2; invalid++) {
                    if (covered && !fixed_off && invalid) continue; /* caller bounds proof is required for live pixels */
                    uint32_t memory[GUARDED_WORDS] = {0};
                    memory[SH_DS_WIDTH] = 4; memory[SH_DS_HEIGHT] = 1;
                    memory[SH_DS_DEPTH_OFFSET] = 16; memory[SH_DS_STENCIL_OFFSET] = stencil ? 20 : 0;
                    memory[SH_DS_FLAGS] = SH_DS_DEPTH_TEST | SH_DS_DEPTH_WRITE |
                        (stencil ? SH_DS_STENCIL_TEST : 0) | (fixed_off ? SH_DS_FIXED_MASK_OFF : 0) |
                        7u << SH_DS_COMPARE_SHIFT;
                    memory[SH_DS_BOUNDS_MAX] = word(1); memory[SH_DS_CLAMP_MAX] = word(1);
                    memory[SH_DS_FRONT_CONTROL] = 7 | 2u << 6;
                    memory[SH_DS_FRONT_VALUES] = 0xffff12u;
                    memory[16] = word(0.75f); memory[20] = 0x334455aau;
                    memory[GUARDED_INDEX] = invalid ? UINT32_MAX - 32u : 0;
                    memory[GUARDED_DEPTH] = word(0.25f); memory[GUARDED_FRONT] = 1;
                    memory[GUARDED_COVERED] = covered;
                    TestMemory data = {.word = memory, .words = GUARDED_WORDS, .mutable_begin = 16, .mutable_end = 21};
                    check(execute(code, words, &data, values, pointers),
                          "guarded SSA branches and Phi predecessor labels execute exactly");
                    if (!covered || fixed_off) {
                        check(!data.reads && !data.writes,
                              "inactive/outside/fixed-mask-off lanes never access mutable depth or stencil planes");
                        check(!memory[GUARDED_RESULT] && memory[16] == word(0.75f) && memory[20] == 0x334455aau,
                              "inactive lane returns false and retains all raw aspect bits");
                    } else {
                        check(memory[GUARDED_RESULT] == 1 && memory[16] == word(0.25f) &&
                                  memory[20] == (stencil ? 0x33445512u : 0x334455aau),
                              "active lane still performs exact depth and packed-stencil updates");
                        check(data.writes == (stencil ? 2u : 1u) && data.reads == (stencil ? 3u : 1u),
                              "active path performs only its depth access and atomic stencil accesses");
                    }
                }
    uint32_t merges = 0, phis = 0, atomic_loads = 0, atomic_xors = 0;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        uint32_t op = code[at] & 0xffff, width = code[at] >> 16;
        if (op == SPV_OP_SELECTION_MERGE) {
            merges++;
            check((code[at + width] & 0xffff) == SPV_OP_BRANCH_CONDITIONAL,
                  "each selection merge immediately precedes its conditional branch");
        }
        phis += op == SPV_OP_PHI;
        atomic_loads += op == SPV_OP_ATOMIC_LOAD;
        atomic_xors += op == SPV_OP_ATOMIC_XOR;
    }
    check(merges == 3 && phis == 1 && atomic_loads == 1 && atomic_xors == 1,
          "guard branch surrounds nested aspect writes, one result Phi and atomic packed-stencil operations");
    free(values); free(pointers);
}
static uint32_t* make_fragment_module(size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 4096);
    spv_emit_shader_capability(&e);
    spv_emit_extension(&e, "SPV_EXT_fragment_shader_interlock");
    spv_emit_capability(&e, 5378);
    uint32_t u = spv_emit_type_u32(&e), f = spv_emit_type_f32(&e), b = spv_emit_type_bool(&e);
    uint32_t v4 = spv_emit_type_vec4(&e, f), buffer = shader_depth_stencil_declare(&e, u, 0, 0);
    uint32_t position = spv_emit_variable(&e, 1, v4), front = spv_emit_variable(&e, 1, b);
    uint32_t coordinate = 15, facing = 17;
    spv_emit_decorate(&e, position, 11, &coordinate, 1); spv_emit_decorate(&e, front, 11, &facing, 1);
    uint32_t function = spv_emit_function(&e, 0, spv_emit_type_void(&e), NULL, 0, NULL);
    uint32_t p = spv_emit_load(&e, v4, position), inside;
    uint32_t index = shader_depth_stencil_coordinates(&e, f, u, b, buffer, p, &inside);
    SPV_VOID(&e, 5364);
    shader_depth_stencil_test(&e, f, u, b, buffer, index, spv_emit_vec4_extract(&e, f, p, 2),
                                spv_emit_load(&e, b, front), inside);
    SPV_VOID(&e, 5365);
    spv_emit_return(&e); spv_emit_function_end(&e);
    uint32_t interface[3] = {buffer, position, front};
    spv_emit_entry_str(&e, SPV_STAGE_FRAGMENT, function, "main", interface, 3);
    spv_emit_fragment_origin_upper_left(&e, function);
    spv_emit_execution_mode(&e, function, 5366, 0, 0);
    uint32_t* code = spv_emit_finish(&e, words); spv_emit_destroy(&e);
    return code;
}
int main(int argc, char** argv) {
    g_test.random = 0x7ea1d329u;
    layout_cases();
    size_t words = 0;
    uint32_t* code = make_scalar_module(&words);
    check(code && words >= 5, "scalar helper produces a complete compute module");
    if (code) {
        scalar_cases(code, words);
        check(save(argc > 1 ? argv[1] : NULL, "depth-stencil-scalar", code, words), "save scalar module");
    }
    free(code);
    code = make_guarded_module(&words);
    check(code && words >= 5, "guarded memory helper produces a complete compute module");
    if (code) {
        guard_cases(code, words);
        check(save(argc > 1 ? argv[1] : NULL, "depth-stencil-guard", code, words), "save guarded memory module");
    }
    free(code);
    code = make_fragment_module(&words);
    check(code && words >= 5, "shadow memory helper produces a complete interlocked fragment module");
    if (code) check(save(argc > 1 ? argv[1] : NULL, "depth-stencil-fragment", code, words), "save fragment module");
    free(code);
    printf("shader depth stencil: %u checks, %u failures\n", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
