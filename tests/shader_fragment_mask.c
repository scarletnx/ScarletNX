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
#include <math.h>

typedef struct {
    const u32* code;
    size_t words;
    const u32** definitions;
    u32 bound, fragcoord;
    u32 position[2];
} MaskModule;
static struct { u32 checks, failures; } g_test;
static void check(int pass, const char* message) {
    g_test.checks++;
    if (!pass && g_test.failures++ < 12) fprintf(stderr, "FAIL: %s\n", message);
}
static int valid_id(const MaskModule* module, u32 id) { return id && id < module->bound; }
static int scalar_result(u32 op) {
    return op == SPV_OP_CONSTANT || op == SPV_OP_VARIABLE || op == SPV_OP_LOAD || op == SPV_OP_ACCESS_CHAIN ||
        op == SPV_OP_COMPOSITE_CONSTRUCT || op == SPV_OP_COMPOSITE_EXTRACT ||
        op == SPV_OP_BITCAST || op == SPV_OP_F_MUL || op == SPV_OP_CONVERT_F_TO_U ||
        op == SPV_OP_CONVERT_U_TO_F || op == SPV_OP_F_SUB || op == SPV_OP_F_MOD ||
        op == SPV_OP_SHIFT_LEFT_LOGICAL || op == SPV_OP_SHIFT_RIGHT_LOGICAL || op == SPV_OP_I_SUB ||
        op == SPV_OP_I_ADD || op == SPV_OP_BITWISE_AND || op == SPV_OP_BITWISE_OR || op == SPV_OP_BITWISE_XOR ||
        op == SPV_OP_F_ORD_GREATER_THAN || op == SPV_OP_F_ORD_LESS_THAN ||
        op == SPV_OP_F_ORD_GREATER_THAN_EQUAL || op == SPV_OP_SELECT;
}
static MaskModule module_index(const u32* code, size_t words) {
    MaskModule module = {.code = code, .words = words, .bound = words >= 5 ? code[3] : 0,
                         .position = {0x3f000000, 0x3f000000}};
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
        if (op == SPV_OP_DECORATE && width == 4 && code[at + 2] == 11 && code[at + 3] == 15)
            module.fragcoord = code[at + 1];
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
    if (op == SPV_OP_COMPOSITE_EXTRACT && width == 5 && instruction[4] < 2) {
        const u32* vector = valid_id(module, instruction[3]) ? module->definitions[instruction[3]] : NULL;
        if (vector && (vector[0] & 0xffff) == SPV_OP_LOAD && vector[3] == module->fragcoord) {
            *result = module->position[instruction[4]];
            return 1;
        }
    }
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
    if (op == SPV_OP_CONVERT_U_TO_F && width == 4 && scalar(module, instruction[3], &a, depth + 1)) {
        *result = as_word((float)a);
        return 1;
    }
    if (op == SPV_OP_SELECT && width == 6 && scalar(module, instruction[3], &a, depth + 1))
        return scalar(module, instruction[a ? 4 : 5], result, depth + 1);
    if (width != 5 || !scalar(module, instruction[3], &a, depth + 1) ||
        !scalar(module, instruction[4], &b, depth + 1)) return 0;
    switch (op) {
        case SPV_OP_BITWISE_AND: *result = a & b; return 1;
        case SPV_OP_BITWISE_OR: *result = a | b; return 1;
        case SPV_OP_BITWISE_XOR: *result = a ^ b; return 1;
        case SPV_OP_I_SUB: *result = a - b; return 1;
        case SPV_OP_I_ADD: *result = a + b; return 1;
        case SPV_OP_SHIFT_LEFT_LOGICAL: if (b < 32) { *result = a << b; return 1; } return 0;
        case SPV_OP_SHIFT_RIGHT_LOGICAL: if (b < 32) { *result = a >> b; return 1; } return 0;
        case SPV_OP_F_MUL: *result = as_word(as_float(a) * as_float(b)); return 1;
        case SPV_OP_F_SUB: *result = as_word(as_float(a) - as_float(b)); return 1;
        case SPV_OP_F_MOD: *result = as_word(as_float(a) - as_float(b) * floorf(as_float(a) / as_float(b))); return 1;
        case SPV_OP_F_ORD_GREATER_THAN: *result = as_float(a) > as_float(b); return 1;
        case SPV_OP_F_ORD_LESS_THAN: *result = as_float(a) < as_float(b); return 1;
        case SPV_OP_F_ORD_GREATER_THAN_EQUAL: *result = as_float(a) >= as_float(b); return 1;
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
    info.advanced_blend_mask = coverage ? SH_ALPHA_COVERAGE | 4u << 8 |
        (coverage == 2 ? SH_ALPHA_COVERAGE_DITHER : 0) : 0;
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
        check(!!module.fragcoord == (coverage == 2), "only dither synthesizes the raw framebuffer coordinate interface");
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
                    if (coverage == 2) {
                        module.position[0] = as_word(2.5f);
                        check(scalar(&module, instruction[2], &value, 0) && value == 8,
                              "dither rotates original-alpha coverage at another pixel before guest intersection");
                        module.position[0] = as_word(0.5f);
                    }
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
        check(intersections == !!coverage, "both alpha coverage modes explicitly intersect the actual R4 guest mask");
        check(save(directory, code, words, coverage, depth, reused), "save module for external validation when requested");
        free(module.definitions);
        free(code);
    }
}
/* Use distinct alphas for float RT0/RT2 and raw integer RT1. The prefix and all six alpha-test comparisons must
   retain RT0's original0.25 while only the floating outputs receive1. Integer RT0 disables the alpha operations. */
static void alpha_to_one(u32 primary_integer, u32 enabled, u32 coverage, u32 mode, u32 factor16, int infinity,
                          const char* directory) {
    u8 source[512] = {0};
    snx_st_le32(source + 18 * 4, 0xfff);
    snx_st_le32(source + 19 * 4, 1);
    u32 registers[] = {0x3e000000, 0x3e800000, 0x3f000000, 0x3e800000,
                              11, 0x80000001, 17, 19,
                              0x3f000000, 0x3e800000, 0x3e000000, 0x3f400000, 15};
    if (infinity) registers[0] = 0x7f800000;
    u32 at = 10;
    for (u32 k = 0; k <= sizeof registers / sizeof *registers; k++) {
        if (!(k % 3)) snx_st_le64(source + 8 * at++, 0x001f8000fc0007e0ull);
        u64 operation = k < sizeof registers / sizeof *registers
                            ? 0x0100000000071000ull | (u64)registers[k] << 20 | k
                            : 0xe30000000007000full;
        snx_st_le64(source + 8 * at++, operation);
    }
    u32 scale = shader_coverage_r1_flags(mode, factor16);
    u32 alpha_flags = coverage && !primary_integer
        ? SH_ALPHA_COVERAGE | 4u << 8 | (coverage == 2 ? SH_ALPHA_COVERAGE_DITHER : 0) : 0;
    ShaderInfo info = {.rt_int_mask = 2u | primary_integer, .rt_sint_mask = 2,
        .advanced_blend_mask = alpha_flags | (enabled ? SH_ALPHA_TO_ONE : 0) | scale};
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
                    check(scalar(&module, instruction[2], &value, 0) && value == (coverage && !primary_integer ? 1u : 15u),
                          "coverage keeps original alpha0.25 before alpha-to-one/modulation and integer primary skips coverage");
                }
                for (u32 rt = 0; rt < 3; rt++) {
                    if (root != outputs[rt]) continue;
                    const u32* color = module.definitions[instruction[2]];
                    int vector = color && color[0] >> 16 == 7 && (color[0] & 0xffff) == SPV_OP_COMPOSITE_CONSTRUCT;
                    check(vector, "each color stores its own four-component output");
                    if (!vector) continue;
                    colors++;
                    for (u32 component = 0; component < 4; component++) {
                        int integer = rt == 1 || (rt == 0 && primary_integer);
                        u32 expected = registers[4 * rt + component];
                        if (component == 3 && enabled && !primary_integer && !integer) expected = 0x3f800000u;
                        int modulated = scale && !integer && (component == 3 ? mode & 2 : mode & 1);
                        if (modulated) expected = as_word(as_float(expected) * ((float)factor16 / 16.0f));
                        int resolved = scalar(&module, color[3 + component], &value, 0);
                        check(resolved && (isnan(as_float(expected)) ? isnan(as_float(value)) : value == expected),
                              "ratio-one scales float MRT components after alpha-to-one and preserves integer bits");
                        if (modulated) {
                            const u32* operation = module.definitions[color[3 + component]];
                            check(operation && (operation[0] & 0xffff) == SPV_OP_F_MUL,
                                  "every active factor, including zero, remains an FP multiply");
                        }
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
        if (infinity) {
            int capability = 0, preserve = 0;
            for (size_t cursor = 5; cursor < words; cursor += code[cursor] >> 16) {
                const u32* instruction = code + cursor;
                if ((instruction[0] & 0xffff) == SPV_OP_CAPABILITY && instruction[1] == SPV_CAPABILITY_SIGNED_ZERO_INF_NAN_PRESERVE) capability++;
                if ((instruction[0] & 0xffff) == SPV_OP_EXECUTION_MODE && instruction[2] == SPV_MODE_SIGNED_ZERO_INF_NAN_PRESERVE && instruction[3] == 32) preserve++;
            }
            check(capability && preserve, "zero times infinity retains its required float-control mode and NaN result");
        }
        check(info.advanced_blend_mask == (alpha_flags | (enabled ? SH_ALPHA_TO_ONE : 0) | scale),
              "all alpha/dither/ratio-one key bits survive first and repeated translation");
        u32 variant = mode ? 16 + mode + factor16 * 4 + primary_integer * 128 + enabled * 256 + coverage * 512 + (u32)infinity * 2048
                           : 2 + enabled;
        check(save(directory, code, words, variant, primary_integer, reused), "save alpha/coverage ordering module");
        free(module.definitions); free(code);
    }
}
static void ratio_one_keys(void) {
    for (u32 mode = 0; mode <= 4; mode++)
        for (u32 factor = 0; factor <= 17; factor++) {
            u32 flags = shader_coverage_r1_flags(mode, factor);
            int active = mode > 0 && mode < 4 && factor < 16;
            check(!!flags == active && shader_coverage_r1_valid(flags), "ratio-one key canonicalizes NONE/unity and excludes invalid state");
            if (active)
                check((flags & SH_COVERAGE_R1) && (flags >> SH_COVERAGE_R1_MODE_SHIFT & 3) == mode &&
                          (flags >> SH_COVERAGE_R1_FACTOR_SHIFT & 31) == factor, "every packed mode and factor is exact");
        }
    u8 source[160] = {0};
    snx_st_le32(source + 18 * 4, 15);
    snx_st_le64(source + 10 * 8, 0x001f8000fc0007e0ull);
    snx_st_le64(source + 11 * 8, 0x0100000000071000ull);
    snx_st_le64(source + 12 * 8, 0xe30000000007000full);
    const u32 malformed[] = {1u << SH_COVERAGE_R1_MODE_SHIFT, 1u << SH_COVERAGE_R1_FACTOR_SHIFT,
        SH_COVERAGE_R1, SH_COVERAGE_R1 | 1u << SH_COVERAGE_R1_MODE_SHIFT | 17u << SH_COVERAGE_R1_FACTOR_SHIFT};
    for (u32 i = 0; i < sizeof malformed / sizeof *malformed; i++) {
        ShaderInfo info = {.advanced_blend_mask = malformed[i]};
        size_t words = 0;
        u32* code = shader_to_spirv(source, sizeof source, ST_FS, &info, &words, 0, 1, 1, 1);
        check(!shader_coverage_r1_valid(malformed[i]) && !code && info.unsupported,
              "malformed or orphaned ratio-one key bits reject the translation");
        free(code);
    }
    for (u32 nonfragment = 0; nonfragment < 2; nonfragment++) {
        ShaderInfo info = {.advanced_blend_mask = shader_coverage_r1_flags(3, 0)};
        size_t words = 0;
        u32* code = shader_to_spirv(source, sizeof source, nonfragment ? ST_VS : ST_FS, &info, &words,
                                     0, nonfragment ? 1 : 0, 1, 1);
        check(!code && info.unsupported, "ratio-one rejects nonfragment use or missing Inf/NaN preservation");
        free(code);
    }
}
static void alpha_keys(void) {
    u8 source[160] = {0};
    snx_st_le32(source + 18 * 4, 15);
    snx_st_le64(source + 10 * 8, 0x001f8000fc0007e0ull);
    snx_st_le64(source + 11 * 8, 0x0100000000071000ull);
    snx_st_le64(source + 12 * 8, 0xe30000000007000full);
    const u32 flags[] = {SH_ALPHA_COVERAGE_DITHER, SH_ALPHA_COVERAGE,
        SH_ALPHA_COVERAGE | 3u << 8, SH_ALPHA_COVERAGE | 32u << 8,
        SH_ALPHA_COVERAGE | SH_ALPHA_COVERAGE_DITHER | 4u << 8, SH_ALPHA_TO_ONE};
    for (u32 i = 0; i < sizeof flags / sizeof *flags; i++) {
        ShaderInfo info = {.advanced_blend_mask = flags[i]};
        size_t words = 0;
        int stage = i >= 4 ? ST_VS : ST_FS;
        u32* code = shader_to_spirv(source, sizeof source, stage, &info, &words, 0, 1, 1, 1);
        check(!code && info.unsupported, "alpha keys reject orphan dither, invalid counts and nonfragment use");
        free(code);
    }
}
/* A storage write precedes the color-register setup, so private/native color bookkeeping cannot consume or move
   that side effect into the ordered late-test region. Every color has distinct raw words. */
static void epilogue_program(u8* source, size_t bytes, u32 colors, u32 outputs, int kill, int storage) {
    for (size_t i = 0; i < bytes; i++) source[i] = 0;
    snx_st_le32(source + 18 * 4, colors == 8 ? UINT32_MAX : (1u << (4 * colors)) - 1);
    snx_st_le32(source + 19 * 4, outputs);
    u64 operations[48];
    u32 count = 0;
    if (storage) {
        operations[count++] = 0x0100000000071000ull;
        operations[count++] = 0x0100000000071001ull;
        operations[count++] = 0x0100000000171004ull;
        operations[count++] = 0x4c98078804a70002ull;
        operations[count++] = 0xeb20010600f70004ull;
    }
    if (kill) operations[count++] = 0xe33000000007000full;
    for (u32 reg = 0; reg < colors * 4 + 2; reg++) {
        u32 value = reg == colors * 4 ? 1 : reg == colors * 4 + 1 ? 0x3f000000u : 0x3e000000u + reg;
        operations[count++] = 0x0100000000071000ull | (u64)value << 20 | reg;
    }
    operations[count++] = 0xe30000000007000full;
    u32 at = 10;
    for (u32 i = 0; i < count; i++) {
        if (!(i % 3)) snx_st_le64(source + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(source + 8 * at++, operations[i]);
    }
    check((size_t)at * 8 <= bytes, "epilogue fixture program fits its source");
}
static void capture_interfaces(const char* directory) {
    for (u32 count = 1; count <= 7; count++) {
        u8 source[1024];
        epilogue_program(source, sizeof source, count, 1, 0, 0);
        u32 config = count == 1 ? 0 : count;
        ShaderInfo info = {.advanced_blend_mask = SH_CAPTURE_SOURCE, .capture_outputs = config,
                           .rt_int_mask = count > 1 ? 2u : 0, .rt_sint_mask = count > 1 ? 2u : 0};
        for (u32 reused = 0; reused < 2; reused++) {
            size_t words = 0;
            u32* code = shader_to_spirv(source, sizeof source, ST_FS, &info, &words, 0, 1, 1, 1);
            check(code && !info.unsupported && info.capture_outputs == config,
                  "all capture counts translate and retain their keyed validity location");
            if (!code || info.unsupported) { free(code); continue; }
            MaskModule module = module_index(code, words);
            check(module.definitions != NULL, "index all captured guest output values");
            if (!module.definitions) { free(code); continue; }
            u32 locations[8] = {0}, mask = 0;
            for (size_t at = 5; at < words; at += code[at] >> 16) {
                const u32* op = code + at;
                if ((op[0] & 0xffff) != SPV_OP_DECORATE || op[0] >> 16 != 4) continue;
                if (op[2] == 30 && op[3] < 8) locations[op[3]] = op[1];
                if (op[2] == 11 && op[3] == 20) mask = op[1];
            }
            check(locations[count] && mask, "capture has a separate validity location and guest SampleMask");
            u32 seen = 0, masks = 0;
            for (size_t at = 5; at < words; at += code[at] >> 16) {
                const u32* op = code + at;
                if ((op[0] & 0xffff) != SPV_OP_STORE || op[0] >> 16 != 3) continue;
                u32 root = pointer_root(&module, op[1], 0), value;
                if (root == mask) {
                    masks++;
                    check(scalar(&module, op[2], &value, 0) && value == 1,
                          "validity export does not consume any guest color or mask registers");
                }
                for (u32 rt = 0; rt < count; rt++) {
                    if (root != locations[rt]) continue;
                    const u32* color = module.definitions[op[2]];
                    int vector = color && (color[0] & 0xffff) == SPV_OP_COMPOSITE_CONSTRUCT && color[0] >> 16 == 7;
                    check(vector, "captured guest output remains a four-component vector");
                    if (!vector) continue;
                    seen |= 1u << rt;
                    for (u32 k = 0; k < 4; k++)
                        check(scalar(&module, color[3 + k], &value, 0) && value == 0x3e000000u + 4 * rt + k,
                              "all guest MRT component words and signed integer outputs retain their values");
                }
            }
            check(seen == (1u << count) - 1 && masks == 1, "every guest target is written independently once");
            check(save(directory, code, words, 8192 + count, 0, reused), "save MRT capture module");
            free(module.definitions); free(code);
        }
    }
    u8 source[1024];
    epilogue_program(source, sizeof source, 2, 0, 0, 0);
    const ShaderInfo invalid[] = {
        {.capture_outputs = 2}, {.capture_outputs = 1, .advanced_blend_mask = SH_CAPTURE_SOURCE},
        {.capture_outputs = 8, .advanced_blend_mask = SH_CAPTURE_SOURCE},
        {.advanced_blend_mask = SH_CAPTURE_SOURCE},
    };
    for (u32 i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        ShaderInfo info = invalid[i];
        size_t words = 0;
        u32* code = shader_to_spirv(source, sizeof source, ST_FS, &info, &words, 0, 1, 1, 1);
        check(!code && info.unsupported, "capture rejects orphan/invalid configuration and guest-validity collisions");
        free(code);
    }
}
static void late_depth_interfaces(const char* directory) {
    for (u32 path = 0; path < 3; path++)
        for (u32 mask = 0; mask < 2; mask++)
            for (u32 kill = 0; kill < 2; kill++) {
                u8 source[1024];
                epilogue_program(source, sizeof source, 1, mask, (int)kill, 1);
                ShaderInfo info = {.late_depth = 1, .advanced_blend = path ? 0 : 5ull << 37,
                    .advanced_blend_mask = path ? SH_COVERAGE_TO_COLOR : SH_BLEND_MASK,
                    .coverage_to_color = path ? 1 : 0, .rt_int_mask = path ? 1 : 0, .rt_sint_mask = path == 2 ? 1 : 0};
                for (u32 reused = 0; reused < 2; reused++) {
                    size_t words = 0;
                    u32* code = shader_to_spirv(source, sizeof source, ST_FS, &info, &words, 0, 1, 1, 1);
                    check(code && !info.unsupported && info.late_depth == 1,
                          "late advanced/unsigned/signed coverage variants translate and preserve metadata");
                    if (!code || info.unsupported) { free(code); continue; }
                    MaskModule module = module_index(code, words);
                    check(module.definitions != NULL, "index ordered late-depth module");
                    if (!module.definitions) { free(code); continue; }
                    size_t begin = 0, end = 0, first_image_write = 0, last_image_write = 0, first_kill = 0;
                    u32 begins = 0, ends = 0, early = 0, post = 0, ordered = 0, writes = 0, shadow = 0;
                    for (size_t at = 5; at < words; at += code[at] >> 16) {
                        const u32* op = code + at;
                        u32 opcode = op[0] & 0xffff, width = op[0] >> 16;
                        if (opcode == 5364) { begin = at; begins++; }
                        if (opcode == 5365) { end = at; ends++; }
                        if (opcode == SPV_OP_IMAGE_WRITE) { if (!first_image_write) first_image_write = at; last_image_write = at; writes++; }
                        if (opcode == SPV_OP_KILL && !first_kill) first_kill = at;
                        if (opcode == SPV_OP_EXECUTION_MODE) {
                            early += op[2] == 9; post += op[2] == 4446; ordered += op[2] == 5366;
                        }
                        if (opcode == SPV_OP_DECORATE && width == 4 && op[2] == 33 && op[3] == SNX_VKB_CAPTURE) shadow = op[1];
                        if (opcode == SPV_OP_SELECTION_MERGE)
                            check(at + width < words && ((code[at + width] & 0xffff) == SPV_OP_BRANCH_CONDITIONAL ||
                                (code[at + width] & 0xffff) == SPV_OP_SWITCH), "selection merge immediately precedes its branch");
                    }
                    check(begins == 1 && ends == 1 && ordered == 1 && begin < end && first_kill > end,
                          "all paths reach one ordered region and kill only after its end");
                    check(!early && !post && shadow, "late tests declare the shadow without native early/post-depth modes");
                    check(first_image_write && first_image_write < begin && writes == (path ? 1u : 2u) &&
                        (path || (last_image_write > begin && last_image_write < end)),
                          "guest storage side effect precedes interlock; only advanced color writes inside it");
                    u32 depth_writes = 0;
                    for (size_t at = 5; at < words; at += code[at] >> 16) {
                        const u32* op = code + at;
                        if ((op[0] & 0xffff) == SPV_OP_STORE && pointer_root(&module, op[1], 0) == shadow) {
                            depth_writes++;
                            check(at > begin && at < end, "depth-shadow stores occur only inside the ordered region");
                        }
                    }
                    check(depth_writes > 0, "late test helper can update depth before guarded color writes");
                    check(save(directory, code, words, 8256 + path * 4 + mask * 2 + kill, 0, reused),
                          "save ordered late-depth integration module");
                    free(module.definitions); free(code);
                }
            }
    u8 source[1024];
    epilogue_program(source, sizeof source, 1, 0, 0, 0);
    const ShaderInfo invalid[] = {
        {.late_depth = 2}, {.late_depth = 1},
        {.late_depth = 1, .coverage_to_color = 1, .rt_int_mask = 1, .early_fragment_tests = 1},
        {.late_depth = 1, .coverage_to_color = 1, .rt_int_mask = 1, .advanced_blend_mask = SH_BLEND_MULTISAMPLE},
        {.late_depth = 1, .coverage_to_color = 1, .rt_int_mask = 1, .advanced_blend_mask = SH_CAPTURE_TILED},
    };
    for (u32 i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        ShaderInfo info = invalid[i];
        size_t words = 0;
        u32* code = shader_to_spirv(source, sizeof source, ST_FS, &info, &words, 0, 1, 1, 1);
        check(!code && info.unsupported, "late-depth variants reject unproved interfaces and binding conflicts");
        free(code);
    }
}
static void single_sample_alpha(const char* directory) {
    static const u8 ranks[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (u32 dither = 0; dither < 2; dither++)
        for (u32 one = 0; one < 2; one++) {
            u8 source[1024];
            epilogue_program(source, sizeof source, 1, 1, 0, 0);
            ShaderInfo info = {.advanced_blend_mask = SH_ALPHA_COVERAGE | 1u << 8 |
                (dither ? SH_ALPHA_COVERAGE_DITHER : 0) | (one ? SH_ALPHA_TO_ONE : 0)};
            for (u32 reused = 0; reused < 2; reused++) {
                size_t words = 0;
                u32* code = shader_to_spirv(source, sizeof source, ST_FS, &info, &words, 0, 1, 1, 1);
                check(code && !info.unsupported, "C1 coverage and independent alpha-to-one translate and reuse");
                if (!code || info.unsupported) { free(code); continue; }
                MaskModule module = module_index(code, words);
                check(module.definitions != NULL, "index C1 fragment outputs");
                if (!module.definitions) { free(code); continue; }
                u32 mask = 0, color = 0;
                for (size_t at = 5; at < words; at += code[at] >> 16) {
                    const u32* op = code + at;
                    if ((op[0] & 0xffff) != SPV_OP_DECORATE || op[0] >> 16 != 4) continue;
                    if (op[2] == 11 && op[3] == 20) mask = op[1];
                    if (op[2] == 30 && !op[3]) color = op[1];
                }
                u32 masks = 0, colors = 0;
                for (size_t at = 5; at < words; at += code[at] >> 16) {
                    const u32* op = code + at;
                    if ((op[0] & 0xffff) != SPV_OP_STORE || op[0] >> 16 != 3) continue;
                    u32 root = pointer_root(&module, op[1], 0), value;
                    if (root == mask) {
                        masks++;
                        for (u32 y = 0; y < 4; y++)
                            for (u32 x = 0; x < 4; x++) {
                                module.position[0] = as_word((float)x + .5f);
                                module.position[1] = as_word((float)y + .5f);
                                u32 expected = dither && as_float(0x3e000003u) >= (float)(ranks[y][x] + 1) / 16.0f;
                                check(scalar(&module, op[2], &value, 0) && value == expected,
                                      "C1 mask uses original alpha before alpha-to-one at every dither pixel");
                            }
                    }
                    if (root == color) {
                        const u32* vector = module.definitions[op[2]];
                        check(vector && vector[0] >> 16 == 7 && scalar(&module, vector[6], &value, 0) &&
                            value == (one ? 0x3f800000u : 0x3e000003u), "C1 stored alpha changes independently of coverage");
                        colors++;
                    }
                }
                check(masks == 1 && colors == 1, "C1 declares one guest color and one sample-mask output");
                check(save(directory, code, words, 8320 + 2 * one + dither, 0, reused), "save C1 alpha ordering module");
                free(module.definitions); free(code);
            }
        }
}
int main(int argc, char** argv) {
    if (argc > 2) return 2;
    ratio_one_keys();
    alpha_keys();
    capture_interfaces(argc == 2 ? argv[1] : NULL);
    late_depth_interfaces(argc == 2 ? argv[1] : NULL);
    single_sample_alpha(argc == 2 ? argv[1] : NULL);
    for (u32 coverage = 0; coverage < 3; coverage++)
        for (u32 depth = 0; depth < 2; depth++) test(coverage, depth, argc == 2 ? argv[1] : NULL);
    for (u32 integer = 0; integer < 2; integer++)
        for (u32 enabled = 0; enabled < 2; enabled++) alpha_to_one(integer, enabled, 1, 0, 16, 0, argc == 2 ? argv[1] : NULL);
    for (u32 mode = 0; mode < 4; mode++)
        for (u32 factor = 0; factor <= 16; factor++)
            for (u32 integer = 0; integer < 2; integer++)
                for (u32 enabled = 0; enabled < 2; enabled++)
                    for (u32 coverage = 0; coverage < 3; coverage++)
                        alpha_to_one(integer, enabled, coverage, mode, factor, 0,
                            argc == 2 && mode && (factor == 0 || factor == 8) ? argv[1] : NULL);
    alpha_to_one(0, 0, 0, 1, 0, 1, argc == 2 ? argv[1] : NULL);
    printf("fragment guest mask translation: %u checks, %u failures\n", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
