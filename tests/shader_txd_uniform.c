/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>

static struct {
    u32 value, calls, wrong;
    int available;
} g_test;

static int read_offset(u32 bank, u32 offset, u32* value) {
    g_test.calls++;
    if (bank != 3 || offset != 0x200) {
        g_test.wrong++;
        return 0;
    }
    *value = g_test.value;
    return g_test.available;
}
static int constant(const u32* code, size_t words, u32 id, u32 value) {
    for (size_t i = 5; i < words; i += code[i] >> 16)
        if ((code[i] & 0xffff) == 43 && (code[i] >> 16) == 4 && code[i + 2] == id && code[i + 3] == value) return 1;
    return 0;
}
static int has_offset(const u32* code, size_t words, u32 packed, int known) {
    u32 samples = 0, queries = 0, divisions = 0;
    for (size_t i = 5; i < words;) {
        u32 length = code[i] >> 16, op = code[i] & 0xffff;
        if (!length || length > words - i) return 0;
        if (op == 103) queries++;
        if (op == 136) divisions++;
        if (op == 88) {
            if (length < 8 || code[i + 5] != (known ? 12u : 4u)) return 0;
            if (known) {
                if (length != 9) return 0;
                u32 vector = code[i + 8], found = 0;
                for (size_t at = 5; at < words; at += code[at] >> 16)
                    if ((code[at] & 0xffff) == 44 && (code[at] >> 16) == 5 && code[at + 2] == vector) {
                        u32 x = (u32)((i32)(packed << 12) >> 28), y = (u32)((i32)(packed << 8) >> 28);
                        found = constant(code, words, code[at + 3], x) && constant(code, words, code[at + 4], y);
                    }
                if (!found) return 0;
            }
            samples++;
        }
        i += length;
    }
    return samples == 1 && (known ? !queries && !divisions : queries == 1 && divisions >= 2);
}
int main(int argc, char** argv) {
    const u64 mov_c = 0x4c98078000070006ull | 3ull << 34 | 0x80ull << 20;
    const u64 ldc = 0xef9400000007ff06ull | 3ull << 36 | 0x200ull << 20;
    const u64 sample = 0xde00000000000000ull | 7ull << 51 | 1ull << 35 | 74ull << 36 | 1ull << 31 | 2ull << 28 |
                       8ull << 20 | 7ull << 16 | 4ull << 8;
    const u64 cases[][3] = {
        {mov_c, 0, 0},
        {mov_c - 1, 0x5c98078000570006ull, 0}, /* MOV_C R5, then MOV R6,R5 */
        {ldc, 0, 0},
        {0x0100000010071007ull, (ldc & ~(0xffull << 8 | 0xffffull << 20)) | 7ull << 8 | 0x100ull << 20, 0},
        {0x0100001008071007ull,
         (ldc & ~(0xffull << 8 | 31ull << 36 | 0xffffull << 20)) | 7ull << 8 | 2ull << 36 | 0x180ull << 20 | 1ull << 44,
         0},
        {(ldc & ~(0xffull | 0xffffull << 20)) | 5ull | 0x1fcull << 20 | 1ull << 48, 0, 0},
        {(ldc & ~(0xffull << 8)) | 7ull << 8, 0, 0},                     /* unknown address */
        {0x0100000000171007ull, (ldc & ~(0xffull << 8)) | 7ull << 8, 0}, /* unaligned address */
        {mov_c, 0, 0},
        {(ldc & ~(0xffull | 0xffffull << 20)) | 5ull | 0x1fcull << 20 | 1ull << 48, 0x5c98078000770006ull,
         0}}; /* the high word is overwritten by an unknown register */
    g_shader_cb_read = read_offset;
    for (u32 test = 0; test < sizeof cases / sizeof *cases; test++) {
        for (u32 value = 0; value < 3; value++) {
            g_test.value = value == 0 ? 0x10000 : value == 1 ? 0xf20000 : 0x1f0000;
            g_test.calls = g_test.wrong = 0;
            g_test.available = test != 8;
            u64 program[24] = {0}, ops[5];
            u32 count = 0;
            for (u32 i = 0; i < 3; i++)
                if (cases[test][i]) ops[count++] = cases[test][i];
            ops[count++] = sample;
            ops[count++] = 0xe30000000007000full;
            u32 at = 10;
            for (u32 i = 0; i < count; i++) {
                if (!(i % 3)) program[at++] = 0x001f8000fc0007e0ull;
                program[at++] = ops[i];
            }
            u8 bytes[sizeof program];
            for (u32 i = 0; i < 24; i++) snx_st_le64(bytes + i * 8, program[i]);
            ShaderInfo info = {0};
            size_t words = 0;
            u32* code = shader_to_spirv(bytes, sizeof bytes, ST_FS, &info, &words, 0, 1, 1, 1);
            int known = test < 6;
            if (!code || info.unsupported || !has_offset(code, words, g_test.value, known) || g_test.wrong ||
                g_test.calls != (test < 6 || test == 8 ? 1u : 0u)) {
                fprintf(stderr, "TXD uniform test%u value%u: known%d calls%u wrong%u %s\n", test, value, known,
                        g_test.calls, g_test.wrong, info.first_unsupported);
                free(code);
                return 1;
            }
            if (argc == 2) {
                char path[4096];
                int n = snprintf(path, sizeof path, "%s/txd-uniform-%u-%u.spv", argv[1], test, value);
                if (n < 0 || n >= (int)sizeof path) return 1;
                FILE* file = fopen(path, "wb");
                if (!file || fwrite(code, 4, words, file) != words || fclose(file)) return 1;
            }
            free(code);
        }
    }
    g_shader_cb_read = NULL;
    puts("PASS TXD uniform offsets: MOV_C, copies, fixed LDC, wide words and dynamic-address refusal exact=1");
    return 0;
}
