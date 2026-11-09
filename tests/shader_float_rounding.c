/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>

/* Generate the actual narrowing instructions for independent spirv-val checks. */
int main(int argc, char** argv) {
    for (u32 pair = 0; pair < 3; pair++)
        for (u32 mode = 0; mode < 4; mode++) {
            u32 source = pair ? 3 : 2, destination = pair == 2 ? 2 : 1;
            u64 program[16] = {0};
            program[9] = 1; /* one fragment color component */
            program[10] = 0x001f8000fc0007e0ull;
            program[11] = 0x0100000000071004ull | 0x00000001ull << 20;
            program[12] = 0x0100000000071005ull | 0x3ff00200ull << 20;
            program[13] = 0x5ca8000000470000ull | (u64)source << 10 | (u64)destination << 8 | (u64)mode << 39;
            program[14] = 0x001f8000fc0007e0ull;
            program[15] = 0xe30000000007000full;
            u8 bytes[sizeof program];
            for (u32 i = 0; i < 16; i++) snx_st_le64(bytes + i * 8, program[i]);
            ShaderInfo info = {.rt_int_mask = 1};
            size_t words = 0;
            u32* code = shader_to_spirv(bytes, sizeof bytes, ST_FS, &info, &words, 0, 1, 1, 1);
            if (!code || info.unsupported) {
                fprintf(stderr, "F2F %u→%u mode%u: %s\n", source, destination, mode, info.first_unsupported);
                free(code);
                return 1;
            }
            int f64 = 0;
            for (size_t at = 5; at < words;) {
                u32 length = code[at] >> 16;
                if (!length || length > words - at) {
                    free(code);
                    return 1;
                }
                if ((code[at] & 0xffff) == 17 && length == 2 && code[at + 1] == 11) {
                    fputs("Float narrowing must not add an Int64 requirement\n", stderr);
                    free(code);
                    return 1;
                }
                if ((code[at] & 0xffff) == 17 && length == 2 && code[at + 1] == 10) f64 = 1;
                at += length;
            }
            /* A conversion with a 64-bit side is the same requirement on either console: this Maxwell program declares
               Float64, which a device without shaderFloat64 (Metal) refuses whole, as it refused one Ounce game's
               fragment programs (docs/platforms/nx/ounce.md) */
            if (f64 != (source == 3)) {
                fprintf(stderr, "F2F %u\u2192%u mode%u: shaderFloat64 %s, and a 64-bit source %s it\n", source,
                        destination, mode, f64 ? "declared" : "not declared", f64 ? "did not want" : "wanted");
                free(code);
                return 1;
            }
            if (argc == 2) {
                char path[4096];
                int n = snprintf(path, sizeof path, "%s/narrow-%u-%u-%u.spv", argv[1], source, destination, mode);
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
    puts("PASS float narrowing:12 actual Maxwell modules, no Int64 requirement, Float64 exactly on the 64-bit ones");
    return 0;
}
