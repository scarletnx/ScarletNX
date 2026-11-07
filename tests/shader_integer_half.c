/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>

/* Generate actual I2F.F16 instructions for every source width, signedness, rounding mode and abs/neg combination. */
int main(int argc, char** argv) {
    for (u32 size = 0; size < 4; size++)
        for (u32 is_signed = 0; is_signed < 2; is_signed++)
            for (u32 modifiers = 0; modifiers < 4; modifiers++)
                for (u32 mode = 0; mode < 4; mode++) {
                    u64 program[16] = {0};
                    program[9] = 1;
                    program[10] = 0x001f8000fc0007e0ull;
                    program[11] = 0x0100000000071004ull | 2051ull << 20;
                    program[12] = 0x0100000000071005ull;
                    program[13] = 0x5cb8000000470100ull | (u64)size << 10 | (u64)is_signed << 13 | (u64)mode << 39 |
                                  (u64)(modifiers & 1) << 49 | (u64)(modifiers >> 1) << 45;
                    program[14] = 0x001f8000fc0007e0ull;
                    program[15] = 0xe30000000007000full;
                    u8 bytes[sizeof program];
                    for (u32 i = 0; i < 16; i++) snx_st_le64(bytes + i * 8, program[i]);
                    ShaderInfo info = {.rt_int_mask = 1};
                    size_t words = 0;
                    u32* code = shader_to_spirv(bytes, sizeof bytes, ST_FS, &info, &words, 0, 1, 1, 1);
                    if (!code || info.unsupported) {
                        fprintf(stderr, "I2F.F16 size%u signed%u modifiers%u mode%u: %s\n", size, is_signed, modifiers,
                                mode, info.first_unsupported);
                        free(code);
                        return 1;
                    }
                    if (argc == 2) {
                        char path[4096];
                        int n = snprintf(path, sizeof path, "%s/integer-half-%u-%u-%u-%u.spv", argv[1], size, is_signed,
                                         modifiers, mode);
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
    puts("PASS integer-to-half: 128 actual Maxwell conversion modules");
    return 0;
}
