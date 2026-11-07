/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>

/* Layer and viewport outputs of a control program cannot select the raster target. The evaluation stage follows
   it. Reads need a separate representation and must never become loads from Position. */
static u32* translate(const u64* instructions, u32 count, int stage, ShaderInfo* info, size_t* words) {
    u8 bytes[256] = {0};
    if (stage == ST_TCS) snx_st_le32(bytes + 8, 3u << 24);
    if (stage == ST_GS) {
        snx_st_le32(bytes + 8, 1u << 24);
        snx_st_le32(bytes + 12, 7u << 24);
        snx_st_le32(bytes + 16, 3);
    }
    u32 at = 10;
    for (u32 i = 0; i < count; i++) {
        if (!(i % 3)) snx_st_le64(bytes + at++ * 8, 0x001f8000fc0007e0ull);
        snx_st_le64(bytes + at++ * 8, instructions[i]);
    }
    *info = (ShaderInfo){.clip_enable = 255u | (stage == ST_TCS ? SNX_SH_NOT_LAST : 0),
                         .rt_int_mask = stage == ST_GS ? 3 : 0};
    return shader_to_spirv(bytes, at * 8, stage, info, words, 1, 1, 1, 1);
}
static int save(const char* directory, const u32* code, size_t words, const char* name, int index) {
    char path[4096];
    int length = snprintf(path, sizeof path, "%s/%s-%d.spv", directory, name, index);
    if (length < 0 || length >= (int)sizeof path) return 1;
    FILE* file = fopen(path, "wb");
    if (!file) return 1;
    int failed = fwrite(code, 4, words, file) != words;
    if (fclose(file)) failed = 1;
    return failed;
}
/* Follow the actual variable, pointer, array and length IDs. The outer per-vertex array is a separate interface
   dimension, so its size must not be mistaken for the number of clip distances. */
static int check_clip_arrays(const u32* code, size_t words, int stage, u32 expected) {
    typedef struct { u32 kind, a, b; } Type;
    if (words < 5 || code[3] > 65536) return 1;
    Type* type = calloc(code[3], sizeof *type);
    if (!type) return 1;
    for (size_t at = 5; at < words;) {
        u32 count = code[at] >> 16, opcode = code[at] & 0xffff;
        if (!count || count > words - at) { free(type); return 1; }
        if ((opcode == 28 || opcode == 32) && count == 4 && code[at + 1] < code[3])
            type[code[at + 1]] = (Type){opcode, code[at + 2], code[at + 3]};
        if (opcode == 43 && count == 4 && code[at + 2] < code[3])
            type[code[at + 2]] = (Type){opcode, code[at + 3], 0};
        if (opcode == 59 && count >= 4 && code[at + 2] < code[3])
            type[code[at + 2]] = (Type){opcode, code[at + 1], code[at + 3]};
        at += count;
    }
    u32 found = 0;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        if ((code[at] & 0xffff) != 71 || code[at] >> 16 != 4 || code[at + 2] != 11 || code[at + 3] != 3)
            continue;
        u32 variable = code[at + 1];
        if (variable >= code[3] || type[variable].kind != 59 || type[variable].a >= code[3]) goto fail;
        Type pointer = type[type[variable].a];
        if (pointer.kind != 32 || pointer.b >= code[3]) goto fail;
        u32 array = pointer.b;
        if (stage == ST_TCS || (pointer.a == 1 && (stage == ST_TES || stage == ST_GS))) {
            if (type[array].kind != 28 || type[array].a >= code[3]) goto fail;
            array = type[array].a;
        }
        if (type[array].kind != 28 || type[array].b >= code[3] ||
            type[type[array].b].kind != 43 || type[type[array].b].a != 8) goto fail;
        found++;
    }
    free(type);
    return found != expected;
fail:
    free(type);
    return 1;
}
static int test_clip_arrays(const char* directory) {
    const struct { int stage; u32 input, output; } cases[] = {
        {ST_VS, 0, 1}, {ST_VS, 0, 128}, {ST_TCS, 1, 128}, {ST_TCS, 128, 1},
        {ST_TES, 1, 128}, {ST_TES, 128, 1}, {ST_GS, 1, 128}, {ST_GS, 128, 1}};
    for (u32 i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        u64 ops[4] = {0};
        u32 count = 0;
        if (cases[i].input)
            ops[count++] = 0xefd800000007ff00ull | (0xffull << 39) |
                            (cases[i].input == 1 ? 0x2c0ull : 0x2dcull) << 20;
        if (cases[i].output)
            ops[count++] = 0xeff07f800007ff00ull | (cases[i].output == 1 ? 0x2c0ull : 0x2dcull) << 20;
        ops[count++] = 0xe30000000007000full;
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(ops, count, cases[i].stage, &info, &words);
        if (!code || info.unsupported || info.clip_input_mask != cases[i].input ||
            (u32)info.uses_clip_mask != cases[i].output ||
            check_clip_arrays(code, words, cases[i].stage, (cases[i].input != 0) + (cases[i].output != 0))) {
            fprintf(stderr, "clip interface case%u stage%d input%x output%x: %s\n", i, cases[i].stage,
                    cases[i].input, cases[i].output, info.first_unsupported);
            free(code);
            return 1;
        }
        if (directory && save(directory, code, words, "clip-interface", (int)i)) { free(code); return 1; }
        free(code);
    }
    return 0;
}
int main(int argc, char** argv) {
    const u64 outputs[] = {0x01000003f807f000ull, /* MOV32I R0, 1.0 */
                            0xeff07f800647ff00ull, /* AST a[0x64], R0 */
                            0xeff07f800687ff00ull, /* AST a[0x68], R0 */
                            0xe30000000007000full};
    for (int features = 0; features < 2; features++) {
        g_shader_output_layer = features;
        g_shader_viewport_index = features ? 3 : 0;
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(outputs, 4, ST_TCS, &info, &words);
        if (!code || info.unsupported || info.writes_layer || info.writes_viewport) {
            fprintf(stderr, "control output omission with features%d: %s\n", features, info.first_unsupported);
            free(code);
            return 1;
        }
        for (size_t at = 5; at < words;) {
            u32 count = code[at] >> 16, opcode = code[at] & 0xffff;
            if (!count || count > words - at) {
                free(code);
                return 1;
            }
            if (opcode == 71 && count == 4 && code[at + 2] == 11 &&
                (code[at + 3] == 0 || code[at + 3] == 9 || code[at + 3] == 10)) {
                fputs("dead control outputs must not alias Position or declare Layer/ViewportIndex\n", stderr);
                free(code);
                return 1;
            }
            at += count;
        }
        if (argc == 2 && save(argv[1], code, words, "tcs-dead-outputs", features)) {
            free(code);
            return 1;
        }
        free(code);
    }
    const int stages[] = {ST_TCS, ST_TES, ST_GS};
    for (u32 s = 0; s < sizeof stages / sizeof stages[0]; s++)
        for (u32 location = 0x64; location <= 0x68; location += 4)
            for (u32 output = 0; output < (stages[s] == ST_TCS ? 2u : 1u); output++) {
                const u64 read[] = {0xefd800000007ff00ull | (0xffull << 39) | (u64)location << 20 |
                                       (u64)output << 32,
                                   0xe30000000007000full};
                ShaderInfo info;
                size_t words = 0;
                u32* code = translate(read, 2, stages[s], &info, &words);
                if (code || !info.unsupported) {
                    fprintf(stderr, "unexpected builtin read support at stage%d address%x output%u: %s\n",
                            stages[s], location, output, info.first_unsupported);
                    free(code);
                    return 1;
                }
                free(code);
            }
    if (test_clip_arrays(argc == 2 ? argv[1] : NULL)) return 1;
    puts("PASS control outputs: two feature states, eight unsupported builtin reads and eight clip interfaces");
    return 0;
}
