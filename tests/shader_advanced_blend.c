/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "support/advanced_blend_spirv.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    u32 modules = 0;
    for (u32 mode = 5; mode <= 50; mode++)
        for (u32 premultiplied = 0; premultiplied < 2; premultiplied++)
            for (u32 overlap = 0; overlap < 3; overlap++) {
                size_t words = 0;
                u32* code = nvn_blend_compute_spirv(mode, (int)premultiplied, overlap, &words);
                if (!code || words < 5 || code[0] != 0x07230203u) return 1;
                for (size_t at = 5; at < words;) {
                    u32 length = code[at] >> 16;
                    if (!length || length > words - at) return 1;
                    at += length;
                }
                if (argc == 2) {
                    char path[4096];
                    if (snprintf(path, sizeof path, "%s/blend-%u-%u-%u.spv", argv[1], mode, premultiplied, overlap) >=
                        (int)sizeof path)
                        return 1;
                    FILE* file = fopen(path, "wb");
                    if (!file || fwrite(code, 4, words, file) != words || fclose(file)) return 1;
                }
                free(code);
                modules++;
            }
    printf("PASS advanced blend emitter: %u mode/premultiplication/overlap modules\n", modules);
    return 0;
}
