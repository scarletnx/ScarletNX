/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include "support/texture_half_instructions.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    for (u32 backend = 0; backend < 3; backend++)
        for (u32 kind = 0; kind < 4; kind++) {
            g_shader_half_rounding = backend == 2 ? 3 : (int)backend;
            u64 program[16] = {0};
            program[9] = 1;
            program[10] = 0x001f8000fc0007e0ull;
            program[11] = 0x0100000000071004ull | 0x3f803000ull << 20;
            program[12] = texture_half_instruction(kind);
            program[13] = 0xe30000000007000full;
            u8 bytes[sizeof program];
            for (u32 i = 0; i < 16; i++) snx_st_le64(bytes + i * 8, program[i]);
            ShaderInfo info = {.rt_int_mask = 1};
            size_t words = 0;
            u32* code = shader_to_spirv(bytes, sizeof bytes, ST_FS, &info, &words, 0, 1, 1, 1);
            if (!code || info.unsupported) {
                fprintf(stderr, "half packing backend%u kind%u: %s\n", backend, kind, info.first_unsupported);
                free(code);
                return 1;
            }
            u32 fp16 = 0, pack = 0, rte = 0;
            for (size_t at = 5; at < words;) {
                u32 width = code[at] >> 16, op = code[at] & 0xffff;
                if (!width || width > words - at) return 1;
                if (op == 17 && width == 2 && code[at + 1] == 9) fp16++;
                if (op == 12 && width >= 5 && code[at + 4] == 58) pack++;
                if (op == 16 && width == 4 && code[at + 2] == 4462 && code[at + 3] == 16) rte++;
                at += width;
            }
            if (fp16 != !!backend || rte != !!backend || (backend ? pack != 0 : pack != (kind == 2 ? 2u : 1u))) {
                fprintf(stderr, "half packing backend%u kind%u: Float16%u RTE%u PackHalf%u\n", backend, kind, fp16, rte,
                        pack);
                free(code);
                return 1;
            }
            if (argc == 2) {
                char path[4096];
                int n = snprintf(path, sizeof path, "%s/half-packing-%u-%u.spv", argv[1], backend, kind);
                if (n < 0 || n >= (int)sizeof path) return 1;
                FILE* file = fopen(path, "wb");
                if (!file || fwrite(code, 4, words, file) != words || fclose(file)) return 1;
            }
            free(code);
        }
    puts("PASS half packing: twelve texture/arithmetic modules, controlled native RTE and32-bit fallback");
    return 0;
}
