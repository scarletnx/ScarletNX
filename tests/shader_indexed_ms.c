/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>

/* Validate descriptor arrays' MS operand and each fetch's Sample operand for float, unsigned and signed views,
   both 2D and 2D-array instructions. The translated programs use an actual dynamic LDC handle table. */
int main(int argc, char** argv) {
    g_shader_texture_arrays = 1;
    for (u32 array = 0; array < 2; array++)
        for (u32 number = 0; number < 3; number++) {
            const u64 ops[] = {0x3848000000470005ull, /* SHL R5, R0, 4 */
                               0xef94002004070504ull, /* LDC R4, c[2][R5 + 0x40] */
                               0xdd380007a0470000ull | 1ull << 50 | (u64)array << 28, 0xe30000000007000full};
            u64 program[16] = {0};
            program[9] = 15;
            u32 at = 10;
            for (u32 i = 0; i < sizeof ops / sizeof *ops; i++) {
                if (!(i % 3)) program[at++] = 0x001f8000fc0007e0ull;
                program[at++] = ops[i];
            }
            u8 bytes[sizeof program];
            for (u32 i = 0; i < 16; i++) snx_st_le64(bytes + i * 8, program[i]);
            ShaderInfo info = {.tex_int_mask = number != 0, .tex_sint_mask = number == 2};
            size_t words = 0;
            u32* code = shader_to_spirv(bytes, sizeof bytes, ST_FS, &info, &words, 0, 1, 1, 1);
            if (!code || info.unsupported || info.ntex != 1 || (info.tex[0].flags & 6) != 6 ||
                info.tex[0].array != array) {
                fprintf(stderr, "indexed multisample array%u number%u: %s\n", array, number, info.first_unsupported);
                free(code);
                return 1;
            }
            u32 image = 0, fetch = 0;
            for (size_t i = 5; i < words;) {
                u32 width = code[i] >> 16, op = code[i] & 0xffff;
                if (!width || width > words - i) {
                    free(code);
                    return 1;
                }
                if (op == 25 && width == 9 && code[i + 3] == 1 && code[i + 5] == array && code[i + 6] == 1) image++;
                if (op == 95 && width == 7 && code[i + 5] == 64) fetch++;
                i += width;
            }
            if (image != 1 || fetch != 1) {
                fprintf(stderr, "indexed multisample array%u number%u: image%u fetch%u\n", array, number, image, fetch);
                free(code);
                return 1;
            }
            if (argc == 2) {
                char path[4096];
                int n = snprintf(path, sizeof path, "%s/indexed-ms-%u-%u.spv", argv[1], array, number);
                if (n < 0 || n >= (int)sizeof path) {
                    free(code);
                    return 1;
                }
                FILE* file = fopen(path, "wb");
                if (!file || fwrite(code, 4, words, file) != words || fclose(file)) {
                    free(code);
                    return 1;
                }
            }
            free(code);
        }
    puts("PASS indexed multisample: six actual Maxwell modules retain the image and Sample operand types");
    return 0;
}
