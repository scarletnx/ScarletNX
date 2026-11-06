/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Interpret the emitted scalar instructions against independently stated threshold comparisons. This checks
   conversion/shift safety for infinities and NaNs, every16-bit alpha fraction, boundary neighbors, and guest masks.
   The optional output directory receives valid fragment modules for spirv-val. No GPU is required. */
#include "nx/gpu/shader/coverage_spirv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
static void check(int pass, const char* what) {
    checks++;
    if (!pass && failures++ < 12) fprintf(stderr, "FAIL: %s\n", what);
}
static float as_float(uint32_t value) {
    float result;
    memcpy(&result, &value, sizeof result);
    return result;
}
static uint32_t as_word(float value) {
    uint32_t result;
    memcpy(&result, &value, sizeof result);
    return result;
}
/* Each bit has its own threshold. No float-to-integer conversion, multiplication or variable shift. */
static uint32_t oracle(float alpha, uint32_t samples, uint32_t guest) {
    uint32_t mask = 0;
    for (uint32_t sample = 0; sample < samples; sample++)
        if (alpha >= (float)(sample + 1) / (float)samples) mask |= 1u << sample;
    return mask & guest;
}
typedef struct {
    SpvEmitter emit;
    size_t begin, end;
    uint32_t alpha, guest, result;
    uint32_t* values;
} Module;
static int evaluate(Module* m, float alpha, uint32_t guest, uint32_t* result) {
    uint32_t* v = m->values;
    v[m->alpha] = as_word(alpha);
    if (m->guest) v[m->guest] = guest;
    for (size_t at = m->begin; at < m->end;) {
        const uint32_t* op = m->emit.functions.words + at;
        uint32_t width = op[0] >> 16, opcode = op[0] & 0xffff, value;
        if (width < 4 || width > m->end - at) return 0;
        switch (opcode) {
            case SPV_OP_F_ORD_GREATER_THAN: value = as_float(v[op[3]]) > as_float(v[op[4]]); break;
            case SPV_OP_F_ORD_LESS_THAN: value = as_float(v[op[3]]) < as_float(v[op[4]]); break;
            case SPV_OP_SELECT: value = v[op[3]] ? v[op[4]] : v[op[5]]; break;
            case SPV_OP_F_MUL: value = as_word(as_float(v[op[3]]) * as_float(v[op[4]])); break;
            case SPV_OP_CONVERT_F_TO_U: {
                float x = as_float(v[op[3]]);
                if (!isfinite(x) || x < 0 || x > 16) return 0;
                value = (uint32_t)x;
                break;
            }
            case SPV_OP_SHIFT_LEFT_LOGICAL:
                if (v[op[4]] >= 32) return 0;
                value = v[op[3]] << v[op[4]];
                break;
            case SPV_OP_I_SUB: value = v[op[3]] - v[op[4]]; break;
            case SPV_OP_BITWISE_AND: value = v[op[3]] & v[op[4]]; break;
            default: return 0;
        }
        v[op[2]] = value;
        at += width;
    }
    *result = v[m->result];
    return 1;
}
static void sample(Module* m, float alpha, uint32_t samples, uint32_t guest) {
    uint32_t result = 0;
    check(evaluate(m, alpha, guest, &result), "conversion and shifts remain defined");
    check(result == oracle(alpha, samples, m->guest ? guest : UINT32_MAX),
          "emitted mask equals independent thresholds and guest intersection");
}
static Module module(uint32_t samples, int guest) {
    Module m = {0};
    spv_emit_init(&m.emit, 128);
    SpvEmitter* e = &m.emit;
    spv_emit_shader_capability(e);
    uint32_t f32 = spv_emit_type_float(e, 32), u32 = spv_emit_type_u32(e), boolean = spv_emit_type_bool(e);
    uint32_t input = spv_emit_location(e, SPV_STORAGE_INPUT, f32, 0);
    uint32_t guest_input = guest ? spv_emit_location(e, SPV_STORAGE_INPUT, u32, 1) : 0;
    if (guest_input) spv_emit_decorate(e, guest_input, SPV_DECORATION_FLAT, NULL, 0);
    uint32_t output = spv_emit_location(e, SPV_STORAGE_OUTPUT, u32, 0);
    uint32_t function = spv_emit_function(e, 0, spv_emit_type_void(e), NULL, 0, NULL);
    m.alpha = spv_emit_load(e, f32, input);
    m.guest = guest ? spv_emit_load(e, u32, guest_input) : 0;
    m.begin = e->functions.count;
    m.result = shader_alpha_coverage_spirv(e, f32, u32, boolean, m.alpha, samples, m.guest);
    m.end = e->functions.count;
    spv_emit_store(e, output, m.result);
    spv_emit_return(e);
    spv_emit_function_end(e);
    spv_emit_entry_str(e, SPV_STAGE_FRAGMENT, function, "main", e->interface, e->interface_count);
    spv_emit_fragment_origin_upper_left(e, function);
    m.values = calloc(e->next_id, sizeof *m.values);
    if (!m.values) exit(1);
    for (size_t at = 0; at < e->types.count;) {
        uint32_t* op = e->types.words + at;
        uint32_t width = op[0] >> 16;
        if (!width || width > e->types.count - at) exit(1);
        if ((op[0] & 0xffff) == SPV_OP_CONSTANT && width == 4) m.values[op[2]] = op[3];
        at += width;
    }
    unsigned instructions = 0, convert = 0, shift = 0, selects = 0, intersections = 0;
    for (size_t at = m.begin; at < m.end;) {
        uint32_t width = e->functions.words[at] >> 16, op = e->functions.words[at] & 0xffff;
        if (!width || width > m.end - at) exit(1);
        instructions++;
        convert += op == SPV_OP_CONVERT_F_TO_U;
        shift += op == SPV_OP_SHIFT_LEFT_LOGICAL;
        selects += op == SPV_OP_SELECT;
        intersections += op == SPV_OP_BITWISE_AND;
        at += width;
    }
    check(instructions <= 9 && convert == 1 && shift == 1 && selects == 2 && intersections == (unsigned)guest,
          "mask construction is bounded and has no coordinate, table or control-flow dependence");
    return m;
}
int main(int argc, char** argv) {
    for (uint32_t samples = 1; samples <= 16; samples *= 2)
        for (int guest = 0; guest < 2; guest++) {
            Module m = module(samples, guest);
            for (uint32_t fraction = 0; fraction <= 65536; fraction++)
                sample(&m, (float)fraction / 65536, samples, 0x9365a6bdu);
            for (uint32_t step = 0; step <= samples; step++) {
                float threshold = (float)step / (float)samples;
                sample(&m, nextafterf(threshold, -INFINITY), samples, UINT32_MAX);
                sample(&m, threshold, samples, UINT32_MAX);
                sample(&m, nextafterf(threshold, INFINITY), samples, UINT32_MAX);
                if (guest && samples == 16)
                    for (uint32_t mask = 0; mask <= UINT16_MAX; mask++) sample(&m, threshold, samples, mask);
            }
            const uint32_t special[] = {0, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00001u, 0xffc00001u,
                                         1, 0x80000001u, 0x7f7fffffu, 0xff7fffffu, 0x3fffffffu, 0xbf800000u};
            for (size_t i = 0; i < sizeof special / sizeof *special; i++)
                sample(&m, as_float(special[i]), samples, UINT32_MAX);
            if (argc == 2) {
                size_t words;
                uint32_t* code = spv_emit_finish(&m.emit, &words);
                char path[4096];
                int n = snprintf(path, sizeof path, "%s/alpha-coverage-%u-%d.spv", argv[1], samples, guest);
                FILE* file = n < 0 || n >= (int)sizeof path ? NULL : fopen(path, "wb");
                check(file && fwrite(code, 4, words, file) == words, "write structural-validation module");
                if (file) check(!fclose(file), "close structural-validation module");
                free(code);
            }
            free(m.values);
            spv_emit_destroy(&m.emit);
        }
    SpvEmitter e;
    spv_emit_init(&e, 16);
    uint32_t f32 = spv_emit_type_float(&e, 32), u32 = spv_emit_type_u32(&e), boolean = spv_emit_type_bool(&e);
    const uint32_t invalid[] = {0, 3, 6, 17, 32};
    size_t before = e.functions.count;
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
        check(!shader_alpha_coverage_spirv(&e, f32, u32, boolean, 1, invalid[i], 0) && e.functions.count == before,
              "unsupported sample counts emit nothing");
    spv_emit_destroy(&e);
    printf("alpha coverage SPIR-V: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
