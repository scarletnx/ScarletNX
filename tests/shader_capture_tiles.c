/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/capture_tiles_spirv.h"
#include <stdio.h>
#include <stdlib.h>

static uint32_t* capture_tile_module(int coordinates, size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 4096);
    spv_emit_shader_capability(&e);
    uint32_t u = spv_emit_type_u32(&e), f = spv_emit_type_f32(&e), b = spv_emit_type_bool(&e);
    uint32_t v4 = spv_emit_type_vec4(&e, f), uv3 = spv_emit_type_vec3(&e, u);
    uint32_t buffer = shader_capture_tiles_declare(&e, u, 0, coordinates ? 3 : 1);
    uint32_t invocation = spv_emit_builtin(&e, 1, uv3, coordinates ? 26 : 28);
    uint32_t local = coordinates ? spv_emit_builtin(&e, 1, uv3, 27) : 0;
    uint32_t function = spv_emit_function(&e, 0, spv_emit_type_void(&e), NULL, 0, NULL);
    uint32_t xyz = spv_emit_load(&e, uv3, invocation);
    uint32_t x = spv_emit_vec4_extract(&e, u, xyz, 0);
    if (coordinates)
        shader_capture_tile_coordinates(&e, u, buffer, xyz, spv_emit_load(&e, uv3, local), coordinates == 2);
    else {
        uint32_t y = spv_emit_vec4_extract(&e, u, xyz, 1), layer = spv_emit_vec4_extract(&e, u, xyz, 2);
        uint32_t zero = spv_emit_constant_f32(&e, f, 0), one = spv_emit_constant_f32(&e, f, 1);
        uint32_t position =
            spv_emit_vec4_construct(&e, v4,
                                    (const uint32_t[4]){spv_emit_unary(&e, SPV_OP_CONVERT_U_TO_F, f, x),
                                                        spv_emit_unary(&e, SPV_OP_CONVERT_U_TO_F, f, y), zero, one});
        shader_capture_tile_mark(&e, f, u, b, buffer, position, layer, spv_emit_const_bool(&e, 1));
    }
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    uint32_t interfaces[] = {buffer, invocation, local};
    spv_emit_compute_entry_str(&e, function, "main", interfaces, coordinates ? 3 : 2);
    spv_emit_local_size(&e, function, 8, 8, 1);
    uint32_t* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}
static int finalize_test(const uint32_t* code, size_t words) {
    const uint32_t shapes[][3] = {{1,1,1}, {64,64,1}, {13,7,3}, {32767,1,1}};
    for (uint32_t shape = 0; shape < sizeof shapes / sizeof *shapes; shape++) {
        uint32_t total = shapes[shape][0] * shapes[shape][1] * shapes[shape][2];
        const uint32_t counts[] = {0, 1, total - 1, total};
        for (uint32_t sample = 0; sample < 4; sample++) {
            uint32_t header[SH_CAPTURE_TILE_HEADER + 2];
            for (uint32_t i = 0; i < SH_CAPTURE_TILE_HEADER + 2; i++) header[i] = 0xdeadbeef;
            header[0] = counts[sample]; header[1] = header[2] = 1;
            header[3] = shapes[shape][0]; header[4] = shapes[shape][1]; header[7] = shapes[shape][2];
            uint32_t* values = calloc(code[3], sizeof *values);
            if (!values) return 0;
            int ok = 1;
            for (size_t at = 5; ok && at < words; at += code[at] >> 16) {
                const uint32_t* op = code + at;
                uint32_t length = *op >> 16;
                switch (*op & 65535) {
                    case SPV_OP_CONSTANT: values[op[2]] = op[3]; break;
                    case SPV_OP_ACCESS_CHAIN: values[op[2]] = values[op[length - 1]]; break;
                    case SPV_OP_LOAD:
                        if (values[op[3]] >= SH_CAPTURE_TILE_HEADER) ok = 0;
                        else values[op[2]] = header[values[op[3]]];
                        break;
                    case SPV_OP_I_MUL: values[op[2]] = values[op[3]] * values[op[4]]; break;
                    case SPV_OP_I_EQUAL: values[op[2]] = values[op[3]] == values[op[4]]; break;
                    case SPV_OP_SELECT: values[op[2]] = values[op[3]] ? values[op[4]] : values[op[5]]; break;
                    case SPV_OP_STORE:
                        if (values[op[1]] < 8 || values[op[1]] > 11) ok = 0;
                        else header[values[op[1]]] = values[op[2]];
                        break;
                    default: break;
                }
            }
            free(values);
            int full = counts[sample] == total;
            ok = ok && header[0] == counts[sample] && header[1] == 1 && header[2] == 1 &&
                 header[8] == (full ? shapes[shape][0] : counts[sample]) && header[9] == (full ? shapes[shape][1] : 1) &&
                 header[10] == (full ? shapes[shape][2] : 1) && header[11] == (uint32_t)full &&
                 header[12] == 0xdeadbeef && header[13] == 0xdeadbeef;
            if (!ok) return 0;
        }
    }
    return 1;
}
int main(int argc, char** argv) {
    for (int coordinates = 0; coordinates < 3; coordinates++) {
        size_t words = 0;
        uint32_t* code = capture_tile_module(coordinates, &words);
        if (!code || words < 5) return 1;
        uint32_t atomic_or = 0, atomic_add = 0, stores = 0, bindings = 0;
        for (size_t at = 5; at < words;) {
            uint32_t count = code[at] >> 16, op = code[at] & 65535;
            if (!count || count > words - at) return 1;
            if (op == 241) atomic_or++;
            if (op == 234) atomic_add++;
            if (op == 62) stores++;
            if (op == 71 && count == 4 && code[at + 2] == 33)
                bindings |= 1u << code[at + 3];
            if (op == 247 && (at + count >= words || (code[at + count] & 65535) != 250)) return 1;
            at += count;
        }
        if (atomic_or != (coordinates ? 0u : 1u) || atomic_add != (coordinates ? 0u : 1u) || stores != 1 ||
            bindings != (coordinates ? 8u : 2u))
            return 1;
        if (argc == 2) {
            char path[4096];
            if (snprintf(path, sizeof path, "%s/capture-tiles-%d.spv", argv[1], coordinates) >= (int)sizeof path) return 1;
            FILE* file = fopen(path, "wb");
            if (!file || fwrite(code, 4, words, file) != words || fclose(file)) return 1;
        }
        free(code);
    }
    size_t final_words;
    uint32_t* finalized = shader_capture_tiles_finalize_spirv(3, &final_words);
    if (!finalized || !finalize_test(finalized, final_words)) return 1;
    if (argc == 2) {
        char path[4096];
        if (snprintf(path, sizeof path, "%s/capture-tiles-finalize.spv", argv[1]) >= (int)sizeof path) return 1;
        FILE* file = fopen(path, "wb");
        if (!file || fwrite(finalized, 4, final_words, file) != final_words || fclose(file)) return 1;
    }
    free(finalized);
    puts("PASS capture tile emitter: unique append, regular/adaptive coordinates, isolated flag reset and GPU header semantics");
    return 0;
}
