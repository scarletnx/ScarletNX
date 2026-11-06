/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>

/* Native RNE half conversion has explicit float controls, including the linked16/64 case. Offline validation
   covers both device-property combinations; the GPU narrowing fixtures check the same source instructions. */
int main(int argc, char** argv) {
    for (u32 linked = 0; linked < 2; linked++)
        for (u32 source = 0; source < 3; source++) {
            g_shader_half_rounding = linked ? 3 : 1;
            u64 code[16] = {0};
            code[9] = 1;
            code[10] = 0x001f8000fc0007e0ull;
            code[11] = 0x0100000000071004ull | 2051ull << 20;
            code[12] = 0x0100000000071005ull | 0x3ff00200ull << 20;
            code[13] = source == 2 ? 0x5cb8000000470900ull /* I2F.F16.U32 */
                                   : 0x5ca8000000470100ull | (u64)(source + 2) << 10;
            code[14] = 0x001f8000fc0007e0ull;
            code[15] = 0xe30000000007000full;
            u8 bytes[sizeof code];
            for (u32 i = 0; i < 16; i++) snx_st_le64(bytes + i * 8, code[i]);
            ShaderInfo info = {.rt_int_mask = 1};
            size_t words = 0;
            u32* module = shader_to_spirv(bytes, sizeof bytes, ST_FS, &info, &words, 0, 1, 1, 1);
            if (!module || info.unsupported) {
                fprintf(stderr, "native half linked%u source%u: %s\n", linked, source, info.first_unsupported);
                free(module);
                return 1;
            }
            u32 modes[2][3] = {{0}}, f16 = 0, converts = 0, int16 = 0;
            for (size_t at = 5; at < words;) {
                u32 width = module[at] >> 16, op = module[at] & 0xffff;
                if (!width || width > words - at) return 1;
                if (op == 22 && width == 3 && module[at + 2] == 16) f16 = module[at + 1];
                if (op == 115 && width == 4 && module[at + 1] == f16) converts++;
                if (op == 17 && width == 2 && module[at + 1] == 22) int16++;
                if (op == 16 && width == 4 && (module[at + 3] == 16 || module[at + 3] == 64)) {
                    u32 scalar = module[at + 3] == 64;
                    u32 mode = module[at + 2];
                    if (mode == 4459) modes[scalar][0]++;
                    if (mode == 4461) modes[scalar][1]++;
                    if (mode == 4462) modes[scalar][2]++;
                }
                at += width;
            }
            if (!f16 || converts != 1 || int16 || modes[0][0] != 1 || modes[0][1] != 1 || modes[0][2] != 1 ||
                (linked && (modes[1][0] != 1 || modes[1][1] != 1 || modes[1][2] != 1))) {
                fprintf(stderr, "native half controls linked%u source%u: f16%u converts%u int16%u\n", linked, source,
                        f16, converts, int16);
                free(module);
                return 1;
            }
            if (argc == 2) {
                char path[4096];
                int n = snprintf(path, sizeof path, "%s/native-half-%u-%u.spv", argv[1], linked, source);
                if (n < 0 || n >= (int)sizeof path) return 1;
                FILE* file = fopen(path, "wb");
                if (!file || fwrite(module, 4, words, file) != words || fclose(file)) return 1;
            }
            free(module);
        }
    puts("PASS native half: six RNE modules, explicit16/64 controls and no Int16 requirement");
    return 0;
}
