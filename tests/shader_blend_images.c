/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include "support/blend_image_program.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    u64 program[32] = {0};
    program[9] = 15;
    u32 at = 10;
    for (u32 i = 0; i < sizeof g_blend_image_instructions / sizeof *g_blend_image_instructions; i++) {
        if (!(i % 3)) program[at++] = 0x001f8000fc0007e0ull;
        program[at++] = g_blend_image_instructions[i];
    }
    u8 bytes[sizeof program];
    for (u32 i = 0; i < 32; i++) snx_st_le64(bytes + i * 8, program[i]);
    /* Overlay23, normalized destination and premultiplied source, full RGBA mask. */
    ShaderInfo info = {.advanced_blend = 23ull << 37 | 1ull << 43 | 1ull << 46, .advanced_blend_mask = 15};
    size_t words = 0;
    u32* code = shader_to_spirv(bytes, sizeof bytes, ST_FS, &info, &words, 0, 1, 1, 1);
    if (!code || info.unsupported || info.nimg != 1 || info.img[0].type != 3 || info.ntex != 1) {
        fprintf(stderr, "advanced blend with guest images: %s\n", info.first_unsupported);
        free(code);
        return 1;
    }
    u32 guest = 0, blend = 0, writes = 0, reads = 0, fetches = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || width > words - i) {
            free(code);
            return 1;
        }
        if (op == 71 && width == 4 && code[i + 2] == 33) {
            if (code[i + 3] == SNX_VKB_IMG) guest = code[i + 1];
            if (code[i + 3] == SNX_VKB_BLEND + (u32)info.nimg) blend = code[i + 1];
        }
        if (op == 99) writes++;
        if (op == 98) reads++;
        if (op == 95) fetches++;
        i += width;
    }
    if (!guest || !blend || guest == blend || writes != 2 || reads != 1 || fetches != 1) {
        fprintf(stderr, "advanced blend image bindings guest%u blend%u writes%u reads%u fetches%u\n", guest, blend,
                writes, reads, fetches);
        free(code);
        return 1;
    }
    if (argc == 2) {
        FILE* file = fopen(argv[1], "wb");
        if (!file || fwrite(code, 4, words, file) != words || fclose(file)) {
            free(code);
            return 1;
        }
    }
    free(code);
    puts("PASS advanced blend with guest image: distinct image bindings and independent guest/attachment writes");
    return 0;
}
