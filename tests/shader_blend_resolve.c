/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/advanced_blend_spirv.h"
#include <stdio.h>
#include <stdlib.h>

static int check_module(u64 packed, u32 flags, const char* directory, u32 index) {
    size_t words = 0;
    u32* code = shader_blend_resolve_spirv(packed, flags, &words);
    if (!code || words < 5 || code[0] != 0x07230203u) return 0;
    u32 bindings = 0, writes = 0, queries = 0, local_size = 0;
    for (size_t at = 5; at < words;) {
        u32 count = code[at] >> 16, op = code[at] & 65535;
        if (!count || count > words - at) return 0;
        if (op == 71 && count == 4 && code[at + 2] == 33) {
            if (code[at + 3] > 3 || (code[at + 3] == 3 && !(flags & SH_CAPTURE_TILED))) return 0;
            bindings |= 1u << code[at + 3];
        }
        if (op == 99) writes++;
        if (op == 104) queries++;
        if (op == 16 && count == 6 && code[at + 2] == 17)
            local_size = code[at + 3] == 8 && code[at + 4] == 8 && code[at + 5] == 1;
        /* The resolver never executes guest code, discard, atomics, or a fragment interlock. */
        if (op == 252 || (op >= 227 && op <= 242) || op == 5364 || op == 5365) return 0;
        if (op == 247 && (at + count >= words || (code[at + count] & 65535) != 250)) return 0;
        at += count;
    }
    u32 samples = flags & SH_BLEND_MULTISAMPLE ? flags >> 8 & 63 : 1;
    if (bindings != (flags & SH_CAPTURE_TILED ? 15u : 7u) || writes != samples || queries != 1 || !local_size) return 0;
    if (directory) {
        char path[4096];
        if (snprintf(path, sizeof path, "%s/blend-resolve-%u.spv", directory, index) >= (int)sizeof path) return 0;
        FILE* file = fopen(path, "wb");
        if (!file || fwrite(code, 4, words, file) != words || fclose(file)) return 0;
    }
    free(code);
    return 1;
}

int main(int argc, char** argv) {
    u32 modules = 0;
    const char* directory = argc == 2 ? argv[1] : NULL;
    for (u32 mode = 5; mode <= 50; mode++)
        for (u32 pm = 0; pm < 2; pm++)
            for (u32 overlap = 0; overlap < 3; overlap++) {
                u64 packed = (u64)mode << 37 | (u64)pm << 46 | (u64)overlap << 44;
                if (!check_module(packed, 15, directory, modules++)) return 1;
            }
    for (u32 shape = 0; shape < 16; shape++)
        for (u32 mask = 0; mask < 3; mask++) {
            u32 flags = mask == 0 ? 0 : mask == 1 ? 5 : 15;
            if (shape & 1) flags |= SH_BLEND_ARRAY;
            if (shape & 2) flags |= SH_BLEND_SRGB;
            if (shape & 4) flags |= SH_BLEND_MULTISAMPLE | 4u << 8;
            if (shape & 8) flags |= SH_CAPTURE_TILED;
            if (!check_module(23ull << 37 | 1ull << 46, flags, directory, modules++)) return 1;
        }
    const u64 invalid[] = {0, 4ull << 37, 51ull << 37, 23ull << 37 | 3ull << 44};
    for (u32 i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        size_t words = 1;
        u32* code = shader_blend_resolve_spirv(invalid[i], 15, &words);
        if (code || words) return 1;
    }
    const u32 invalid_samples[] = {0, 3, 33};
    for (u32 i = 0; i < sizeof invalid_samples / sizeof invalid_samples[0]; i++) {
        size_t words = 1;
        u32* code = shader_blend_resolve_spirv(23ull << 37, 15 | SH_BLEND_MULTISAMPLE | invalid_samples[i] << 8, &words);
        if (code || words) return 1;
    }
    printf("PASS late blend resolver: %u equation/interface modules\n", modules);
    return 0;
}
