/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The incoming SPH declares mask/depth outputs before ShaderInfo has learned that header. Trace output values
   back through the emitted scalar instructions and register stores, including the first translation of a shader.
   An optional output directory receives modules for external spirv-val checks. This test uses no Vulkan device. */
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/shader/advanced_blend_spirv.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const u32* code;
    size_t words;
    const u32** definitions;
    u32 bound;
} MaskModule;
static struct { u32 checks, failures; } g_test;
static void check(int pass, const char* message) {
    g_test.checks++;
    if (!pass && g_test.failures++ < 12) fprintf(stderr, "FAIL: %s\n", message);
}
static int valid_id(const MaskModule* module, u32 id) { return id && id < module->bound; }
static int scalar_result(u32 op) {
    return op == SPV_OP_CONSTANT || op == SPV_OP_VARIABLE || op == SPV_OP_LOAD || op == SPV_OP_ACCESS_CHAIN ||
        op == SPV_OP_COMPOSITE_CONSTRUCT ||
        op == SPV_OP_BITCAST || op == SPV_OP_F_MUL || op == SPV_OP_CONVERT_F_TO_U ||
        op == SPV_OP_SHIFT_LEFT_LOGICAL || op == SPV_OP_I_SUB || op == SPV_OP_BITWISE_AND ||
        op == SPV_OP_F_ORD_GREATER_THAN || op == SPV_OP_F_ORD_LESS_THAN || op == SPV_OP_SELECT;
}
static MaskModule module_index(const u32* code, size_t words) {
    MaskModule module = {code, words, NULL, words >= 5 ? code[3] : 0};
    if (!module.bound) return module;
    module.definitions = calloc(module.bound, sizeof *module.definitions);
    if (!module.definitions) return module;
    for (size_t at = 5; at < words;) {
        u32 width = code[at] >> 16, op = code[at] & 0xffff;
        if (!width || width > words - at) { free(module.definitions); module.definitions = NULL; return module; }
        if (scalar_result(op)) {
            if (width < 3 || !valid_id(&module, code[at + 2])) {
                free(module.definitions); module.definitions = NULL; return module;
            }
            module.definitions[code[at + 2]] = code + at;
        }
        at += width;
    }
    return module;
}
static u32 pointer_root(const MaskModule* module, u32 id, u32 depth) {
    if (depth > 16 || !valid_id(module, id)) return 0;
    const u32* definition = module->definitions[id];
    if (!definition) return id;
    if ((definition[0] & 0xffff) != SPV_OP_ACCESS_CHAIN) return id;
    if ((definition[0] >> 16) < 4) return 0;
    return pointer_root(module, definition[3], depth + 1);
}
static float as_float(u32 value) { float result; memcpy(&result, &value, sizeof result); return result; }
static u32 as_word(float value) { u32 result; memcpy(&result, &value, sizeof result); return result; }
static int scalar(const MaskModule* module, u32 id, u32* result, u32 depth) {
    if (depth > 64 || !valid_id(module, id) || !module->definitions[id]) return 0;
    const u32* instruction = module->definitions[id];
    u32 op = instruction[0] & 0xffff, width = instruction[0] >> 16, a, b;
    if (op == SPV_OP_CONSTANT && width == 4) { *result = instruction[3]; return 1; }
    if (op == SPV_OP_BITCAST && width == 4) return scalar(module, instruction[3], result, depth + 1);
    if (op == SPV_OP_LOAD && width >= 4) {
        u32 stored = 0;
        for (size_t at = 5; module->code + at < instruction; at += module->code[at] >> 16)
            if ((module->code[at] & 0xffff) == SPV_OP_STORE && module->code[at + 1] == instruction[3])
                stored = module->code[at + 2];
        return scalar(module, stored, result, depth + 1);
    }
    if (op == SPV_OP_CONVERT_F_TO_U && width == 4 && scalar(module, instruction[3], &a, depth + 1)) {
        float value = as_float(a);
        if (!(value >= 0 && value <= 16)) return 0;
        *result = (u32)value;
        return 1;
    }
    if (op == SPV_OP_SELECT && width == 6 && scalar(module, instruction[3], &a, depth + 1))
        return scalar(module, instruction[a ? 4 : 5], result, depth + 1);
    if (width != 5 || !scalar(module, instruction[3], &a, depth + 1) ||
        !scalar(module, instruction[4], &b, depth + 1)) return 0;
    switch (op) {
        case SPV_OP_BITWISE_AND: *result = a & b; return 1;
        case SPV_OP_I_SUB: *result = a - b; return 1;
        case SPV_OP_SHIFT_LEFT_LOGICAL: if (b < 32) { *result = a << b; return 1; } return 0;
        case SPV_OP_F_MUL: *result = as_word(as_float(a) * as_float(b)); return 1;
        case SPV_OP_F_ORD_GREATER_THAN: *result = as_float(a) > as_float(b); return 1;
        case SPV_OP_F_ORD_LESS_THAN: *result = as_float(a) < as_float(b); return 1;
        default: return 0;
    }
}
static int save(const char* directory, const u32* code, size_t words, u32 coverage, u32 depth, u32 reused) {
    if (!directory) return 1;
    char path[4096];
    int length = snprintf(path, sizeof path, "%s/fragment-mask-%u-%u-%u.spv", directory, coverage, depth, reused);
    if (length < 0 || length >= (int)sizeof path) return 0;
    FILE* file = fopen(path, "wb");
    if (!file) return 0;
    int wrote = fwrite(code, sizeof *code, words, file) == words;
    return fclose(file) == 0 && wrote;
}
static void test(u32 coverage, u32 depth, const char* directory) {
    u8 source[256] = {0};
    snx_st_le32(source + 18 * 4, 15);
    snx_st_le32(source + 19 * 4, 1u | depth << 1);
    const u32 registers[6] = {0, 0x3f800000, 0, 0x3f000000, 10, 0x3e800000};
    u32 at = 10;
    for (u32 k = 0; k < 7; k++) {
        if (!(k % 3)) snx_st_le64(source + 8 * at++, 0x001f8000fc0007e0ull);
        u64 operation = k < 6 ? 0x0100000000071000ull | (u64)registers[k] << 20 | k : 0xe30000000007000full;
        snx_st_le64(source + 8 * at++, operation);
    }
    ShaderInfo info = {0};
    info.advanced_blend_mask = coverage ? SH_ALPHA_COVERAGE | 4u << 8 : 0;
    for (u32 reused = 0; reused < 2; reused++) {
        size_t words = 0;
        u32* code = shader_to_spirv(source, sizeof source, ST_FS, &info, &words, 0, 1, 1, 1);
        check(code && !info.unsupported, "plain and coverage fragment programs translate on first and repeated use");
        if (!code || info.unsupported) { free(code); return; }
        MaskModule module = module_index(code, words);
        check(module.definitions != NULL, "SPIR-V instruction sizes and result IDs are valid");
        if (!module.definitions) { free(code); return; }
        u32 mask_output = 0, depth_output = 0, mask_stores = 0, depth_stores = 0, intersections = 0;
        for (size_t cursor = 5; cursor < words; cursor += code[cursor] >> 16)
            if ((code[cursor] & 0xffff) == SPV_OP_DECORATE && code[cursor] >> 16 == 4 && code[cursor + 2] == 11) {
                if (code[cursor + 3] == 20) mask_output = code[cursor + 1];
                if (code[cursor + 3] == SPV_BUILT_IN_FRAG_DEPTH) depth_output = code[cursor + 1];
            }
        check(valid_id(&module, mask_output), "actual SPH19 declares a SampleMask output");
        check(!!depth_output == !!depth, "depth interface follows actual SPH19");
        for (size_t cursor = 5; cursor < words; cursor += code[cursor] >> 16) {
            const u32* instruction = code + cursor;
            u32 op = instruction[0] & 0xffff, width = instruction[0] >> 16;
            if (op == SPV_OP_STORE) {
                int ids = width >= 3 && valid_id(&module, instruction[1]) && valid_id(&module, instruction[2]);
                check(ids, "every OpStore pointer and object is a nonzero ID");
                if (!ids) continue;
                u32 root = pointer_root(&module, instruction[1], 0), value = 0;
                if (root == mask_output) {
                    mask_stores++;
                    check(scalar(&module, instruction[2], &value, 0) && value == (coverage ? 2u : 10u),
                          "mask store derives from R4=10, intersecting alpha0.5 prefix3 when enabled");
                }
                if (depth_output && root == depth_output) {
                    depth_stores++;
                    check(scalar(&module, instruction[2], &value, 0) && value == registers[5],
                          "depth following guest mask derives from R5, not mask R4");
                }
            }
            if (op == SPV_OP_ACCESS_CHAIN || op == SPV_OP_BITCAST || op == SPV_OP_LOAD)
                for (u32 k = 1; k < width; k++) check(valid_id(&module, instruction[k]), "scalar path contains no zero IDs");
            if (op == SPV_OP_BITWISE_AND && width == 5) {
                u32 a, b;
                if (scalar(&module, instruction[3], &a, 0) && scalar(&module, instruction[4], &b, 0) &&
                    ((a == 10 && b == 3) || (a == 3 && b == 10))) intersections++;
            }
        }
        check(mask_stores == 1 && depth_stores == depth, "one mask output and optional following depth output are stored");
        check(intersections == coverage, "undithered coverage explicitly intersects the actual R4 guest mask");
        check(save(directory, code, words, coverage, depth, reused), "save module for external validation when requested");
        free(module.definitions);
        free(code);
    }
}
/* Use distinct alphas for float RT0/RT2 and raw integer RT1. The prefix and all six alpha-test comparisons must
   retain RT0's original0.25 while only the floating outputs receive1. Integer RT0 disables the alpha operations. */
static void alpha_to_one(u32 primary_integer, u32 enabled, const char* directory) {
    u8 source[512] = {0};
    snx_st_le32(source + 18 * 4, 0xfff);
    snx_st_le32(source + 19 * 4, 1);
    const u32 registers[] = {0x3e000000, 0x3e800000, 0x3f000000, 0x3e800000,
                              11, 13, 17, 19,
                              0x3f000000, 0x3e800000, 0x3e000000, 0x3f400000, 15};
    u32 at = 10;
    for (u32 k = 0; k <= sizeof registers / sizeof *registers; k++) {
        if (!(k % 3)) snx_st_le64(source + 8 * at++, 0x001f8000fc0007e0ull);
        u64 operation = k < sizeof registers / sizeof *registers
                            ? 0x0100000000071000ull | (u64)registers[k] << 20 | k
                            : 0xe30000000007000full;
        snx_st_le64(source + 8 * at++, operation);
    }
    ShaderInfo info = {.rt_int_mask = 2u | primary_integer,
        .advanced_blend_mask = (primary_integer ? 0 : SH_ALPHA_COVERAGE | 4u << 8) |
                              (enabled ? SH_ALPHA_TO_ONE : 0)};
    for (u32 reused = 0; reused < 2; reused++) {
        size_t words = 0;
        u32* code = shader_to_spirv(source, sizeof source, ST_FS, &info, &words, 0, 1, 1, 1);
        check(code && !info.unsupported, "alpha-to-one mixed float/integer outputs translate and reuse metadata");
        if (!code || info.unsupported) { free(code); return; }
        MaskModule module = module_index(code, words);
        check(module.definitions != NULL, "index alpha-to-one output definitions");
        if (!module.definitions) { free(code); return; }
        u32 outputs[3] = {0}, mask_output = 0, colors = 0, masks = 0, alpha_tests = 0;
        for (size_t cursor = 5; cursor < words; cursor += code[cursor] >> 16) {
            const u32* instruction = code + cursor;
            if ((instruction[0] & 0xffff) != SPV_OP_DECORATE || instruction[0] >> 16 != 4) continue;
            if (instruction[2] == 30 && instruction[3] < 3) outputs[instruction[3]] = instruction[1];
            if (instruction[2] == 11 && instruction[3] == 20) mask_output = instruction[1];
        }
        for (size_t cursor = 5; cursor < words; cursor += code[cursor] >> 16) {
            const u32* instruction = code + cursor;
            u32 op = instruction[0] & 0xffff, width = instruction[0] >> 16;
            if (op == SPV_OP_STORE && width == 3) {
                u32 root = pointer_root(&module, instruction[1], 0), value;
                if (root == mask_output) {
                    masks++;
                    check(scalar(&module, instruction[2], &value, 0) && value == (primary_integer ? 15u : 1u),
                          "coverage uses original alpha0.25 before alpha-to-one and integer primary skips coverage");
                }
                for (u32 rt = 0; rt < 3; rt++) {
                    if (root != outputs[rt]) continue;
                    const u32* color = module.definitions[instruction[2]];
                    int vector = color && color[0] >> 16 == 7 && (color[0] & 0xffff) == SPV_OP_COMPOSITE_CONSTRUCT;
                    check(vector, "each color stores its own four-component output");
                    if (!vector) continue;
                    colors++;
                    for (u32 component = 0; component < 4; component++) {
                        u32 expected = component == 3 && enabled && !primary_integer && rt != 1
                                           ? 0x3f800000u : registers[4 * rt + component];
                        check(scalar(&module, color[3 + component], &value, 0) && value == expected,
                              "alpha-to-one changes every float alpha only, preserving RGB and integer output bits");
                    }
                }
            }
            if (op >= 180 && op <= 191 && width == 5) {
                u32 original, reference;
                if (scalar(&module, instruction[3], &original, 0) && original == registers[3] &&
                    !scalar(&module, instruction[4], &reference, 0)) alpha_tests++;
            }
        }
        check(colors == 3 && masks == 1, "all MRT outputs and the guest coverage mask are written");
        check(alpha_tests == 6, "all six alpha-test comparisons retain the original primary alpha");
        check(save(directory, code, words, 2 + enabled, primary_integer, reused), "save alpha-to-one module");
        free(module.definitions); free(code);
    }
}
int main(int argc, char** argv) {
    if (argc > 2) return 2;
    for (u32 coverage = 0; coverage < 2; coverage++)
        for (u32 depth = 0; depth < 2; depth++) test(coverage, depth, argc == 2 ? argv[1] : NULL);
    for (u32 integer = 0; integer < 2; integer++)
        for (u32 enabled = 0; enabled < 2; enabled++) alpha_to_one(integer, enabled, argc == 2 ? argv[1] : NULL);
    printf("fragment guest mask translation: %u checks, %u failures\n", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
