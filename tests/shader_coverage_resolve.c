/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Check the coverage resolver's binding ABI, integer type, full-mask-before-write ordering and explicit samples.
   The optional output directory receives every image-shape module for validation by spirv-val. */
#include "nx/gpu/shader/coverage_spirv.h"
#include "nx/gpu/shader/advanced_blend_spirv.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, failures;
static void check(int pass, const char* what) {
    checks++;
    if (!pass && failures++ < 12) fprintf(stderr, "FAIL: %s\n", what);
}
static int inspect(const uint32_t* code, size_t words, uint32_t samples, int array, int signed_target) {
    if (words < 5 || code[0] != 0x07230203u || !code[3]) return 0;
    uint32_t* constants = calloc(code[3], sizeof *constants);
    uint32_t* binding = calloc(code[3], sizeof *binding);
    uint32_t* origin = calloc(code[3], sizeof *origin);
    if (!constants || !binding || !origin) exit(1);
    uint32_t float_type = 0, integer_type = 0, float4_type = 0;
    uint32_t valid_type = 0, target_type = 0, valid_shape = 0, target_shape = 0;
    uint32_t reads = 0, writes = 0, sample_reads = 0, sample_writes = 0, first_write = 0, last_read = 0;
    uint32_t written_value = 0, local_size = 0, integer_bitcast = 0, bounds = 0, branches = 0;
    int okay = 1;
    for (size_t at = 5; at < words;) {
        const uint32_t* op = code + at;
        uint32_t count = op[0] >> 16, kind = op[0] & 0xffff;
        if (!count || count > words - at) { okay = 0; break; }
        if (kind == SPV_OP_TYPE_FLOAT && count == 3 && op[2] == 32) float_type = op[1];
        if (kind == SPV_OP_TYPE_INT && count == 4 && op[2] == 32 && op[3] == (uint32_t)signed_target)
            integer_type = op[1];
        if (kind == SPV_OP_TYPE_VECTOR && count == 4 && op[2] == float_type && op[3] == 4) float4_type = op[1];
        if (kind == SPV_OP_TYPE_IMAGE && count == 9) {
            int shape = op[3] == SPV_DIM_2D && op[5] == (uint32_t)array && op[6] == (samples > 1) &&
                        op[7] == 2 && op[8] == 0;
            if (op[2] == float_type) valid_type = op[1], valid_shape = (uint32_t)shape;
            if (op[2] == integer_type) target_type = op[1], target_shape = (uint32_t)shape;
        }
        if (kind == SPV_OP_CONSTANT && count == 4) constants[op[2]] = op[3];
        if (kind == SPV_OP_DECORATE && count == 4 && op[2] == SPV_DECORATION_BINDING) binding[op[1]] = op[3];
        if (kind == SPV_OP_LOAD && count == 4) origin[op[2]] = binding[op[3]];
        if (kind == SPV_OP_EXECUTION_MODE && count == 6 && op[2] == 17 && op[3] == 8 && op[4] == 8 && op[5] == 1)
            local_size++;
        if (kind == SPV_OP_U_LESS_THAN) bounds++;
        if (kind == SPV_OP_BRANCH_CONDITIONAL) branches++;
        if (kind == SPV_OP_BITCAST && count == 4 && op[1] == integer_type) integer_bitcast++;
        if (kind == SPV_OP_IMAGE_READ) {
            if (op[1] != float4_type || origin[op[3]] != 1 || count != (samples > 1 ? 7u : 5u)) okay = 0;
            if (samples > 1) {
                if (op[5] != 64 || constants[op[6]] >= samples) okay = 0;
                else sample_reads |= 1u << constants[op[6]];
            }
            reads++;
            last_read = (uint32_t)at;
        }
        if (kind == SPV_OP_IMAGE_WRITE) {
            if (origin[op[1]] != 2 || count != (samples > 1 ? 6u : 4u)) okay = 0;
            if (!first_write) first_write = (uint32_t)at, written_value = op[3];
            if (written_value != op[3]) okay = 0;
            if (samples > 1) {
                if (op[4] != 64 || constants[op[5]] >= samples) okay = 0;
                else sample_writes |= 1u << constants[op[5]];
            }
            writes++;
        }
        at += count;
    }
    uint32_t all = samples == 32 ? UINT32_MAX : (1u << samples) - 1;
    okay = okay && valid_type && target_type && valid_shape && target_shape && reads == samples && writes == samples &&
           last_read < first_write && local_size == 1 && integer_bitcast == (uint32_t)signed_target &&
           bounds == (array ? 3u : 2u) && branches == samples + 1 &&
           (samples == 1 || (sample_reads == all && sample_writes == all));
    free(constants);
    free(binding);
    free(origin);
    return okay;
}
int main(int argc, char** argv) {
    for (uint32_t samples = 1; samples <= 32; samples *= 2)
        for (int array = 0; array < 2; array++)
            for (int signed_target = 0; signed_target < 2; signed_target++) {
                uint32_t flags = array ? SH_BLEND_ARRAY : 0;
                if (samples > 1) flags |= SH_BLEND_MULTISAMPLE | samples << 8;
                size_t words = 0;
                uint32_t* code = shader_coverage_resolve_spirv(flags, signed_target, &words);
                check(code && inspect(code, words, samples, array, signed_target),
                      "coverage resolve image types, bindings, bounds and per-sample union writes");
                if (code && argc == 2) {
                    char path[4096];
                    int n = snprintf(path, sizeof path, "%s/coverage-resolve-%u-%d-%d.spv", argv[1], samples,
                                     array, signed_target);
                    FILE* file = n < 0 || n >= (int)sizeof path ? NULL : fopen(path, "wb");
                    check(file && fwrite(code, 4, words, file) == words, "write structural-validation module");
                    if (file) check(!fclose(file), "close structural-validation module");
                }
                free(code);
            }
    const uint32_t invalid[] = {0, 3, 6, 17, 33};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        size_t words = 99;
        uint32_t* code = shader_coverage_resolve_spirv(SH_BLEND_MULTISAMPLE | invalid[i] << 8, 0, &words);
        check(!code && !words, "invalid raster counts have no resolver");
        free(code);
    }
    printf("coverage resolve SPIR-V: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
