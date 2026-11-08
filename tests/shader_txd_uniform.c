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
/* Follow the texture tree independently for all256 spatial selectors. LDC.IS has an unrelated bank switch;
   start at the proven high-nibble texture selector and require its merged result to be the one selected sample. */
static int routes_offset(const u32* code, size_t words, const u32* const* definitions,
                          const size_t* labels, size_t root, u32 root_label, u32 merge, u32 raw) {
    for (u32 packed = 0; packed < 256; packed++) {
        size_t at = root;
        u32 block = root_label, predecessor = 0, result = 0, samples = 0, done = 0;
        for (u32 steps = 0; !done && steps < 32; steps++) {
            if (at >= words) return 0;
            const u32* instruction = code + at;
            u32 length = instruction[0] >> 16, op = instruction[0] & 0xffff;
            if (op == 251) {
                const u32* selector = instruction[1] < code[3] ? definitions[instruction[1]] : NULL;
                if (!selector || (selector[0] & 0xffff) != 199 || selector[3] != raw || selector[4] >= code[3]) return 0;
                const u32* mask = definitions[selector[4]];
                if (!mask || (mask[0] & 0xffff) != 43) return 0;
                u32 target = instruction[2];
                for (u32 k = 3; k < length; k += 2)
                    if (instruction[k] == (packed & mask[3])) target = instruction[k + 1];
                if (target >= code[3] || !labels[target]) return 0;
                predecessor = block; block = target; at = labels[target];
                continue;
            }
            if (op == 88) {
                const u32* vector = definitions[instruction[8]];
                for (u32 axis = 0; axis < 2; axis++) {
                    u32 nibble = packed >> (4 * axis) & 15;
                    if (definitions[vector[3 + axis]][3] != (nibble >= 8 ? nibble - 16 : nibble)) return 0;
                }
                result = instruction[2]; samples++;
            }
            if (op == 245) {
                u32 incoming = 0;
                for (u32 k = 3; k < length; k += 2)
                    if (instruction[k + 1] == predecessor) incoming = instruction[k];
                if (!result || incoming != result) return 0;
                result = instruction[2];
                if (block == merge) done = 1;
            }
            if (op == 249) {
                u32 target = instruction[1];
                if (target >= code[3] || !labels[target]) return 0;
                predecessor = block; block = target; at = labels[target];
                continue;
            }
            at += length;
        }
        if (!done || samples != 1) return 0;
    }
    return 1;
}
static int has_offset(const u32* code, size_t words, u32 packed, int known) {
    const u32** definitions = calloc(code[3], sizeof *definitions);
    size_t* labels = calloc(code[3], sizeof *labels);
    if (!definitions || !labels) { free(definitions); free(labels); return 0; }
    int ok = 1;
    for (size_t i = 5; i < words;) {
        u32 length = code[i] >> 16, op = code[i] & 0xffff;
        if (!length || length > words - i) { ok = 0; break; }
        if ((op == 43 || op == 44 || op == 194 || op == 199) && length > 2 && code[i + 2] < code[3])
            definitions[code[i + 2]] = code + i;
        if (op == 248 && length == 2 && code[i + 1] < code[3]) labels[code[i + 1]] = i + 2;
        i += length;
    }
    u32 samples = 0, queries = 0, divisions = 0, switches = 0, phis = 0;
    u32 high = 0, low = 0, block = 0, selection = 0, root_label = 0, merge = 0, raw = 0;
    size_t root = 0;
    for (size_t i = 5; ok && i < words; i += code[i] >> 16) {
        u32 length = code[i] >> 16, op = code[i] & 0xffff;
        if (op == 103) queries++;
        if (op == 136) divisions++;
        if (op == 248) block = code[i + 1];
        if (op == 247) selection = code[i + 1];
        /* LDC.IS has its own32-way bank switch. TXD uses one high-nibble and16 low-nibble selections. */
        if (op == 251 && length == 33) {
            switches++;
            const u32* selector = code[i + 1] < code[3] ? definitions[code[i + 1]] : NULL;
            const u32* mask = selector && (selector[0] & 0xffff) == 199 && selector[4] < code[3]
                                  ? definitions[selector[4]] : NULL;
            if (!mask || (mask[0] & 0xffff) != 43 || (mask[3] != 15 && mask[3] != 240)) { ok = 0; break; }
            if (!raw) raw = selector[3];
            if (raw != selector[3] || raw >= code[3] || !definitions[raw] ||
                (definitions[raw][0] & 0xffff) != 194 || definitions[raw][4] >= code[3] ||
                !definitions[definitions[raw][4]] || definitions[definitions[raw][4]][3] != 16) { ok = 0; break; }
            if (mask[3] == 240) { high++; root = i; root_label = block; merge = selection; }
            else low++;
            for (u32 k = 0; k < 15; k++)
                if (code[i + 3 + 2 * k] != (k + 1) * (mask[3] == 240 ? 16u : 1u)) ok = 0;
        }
        if (op == 245 && length == 35) phis++;
        if (op == 88) {
            if (length != 9 || code[i + 5] != 12 || code[i + 8] >= code[3]) { ok = 0; break; }
            const u32* vector = definitions[code[i + 8]];
            u32 expected = known ? packed >> 16 : samples;
            u32 x = expected & 15, y = expected >> 4 & 15;
            x = x >= 8 ? x - 16 : x;
            y = y >= 8 ? y - 16 : y;
            if (!vector || (*vector & 0xffff) != 44 || *vector >> 16 != 5 || vector[3] >= code[3] || vector[4] >= code[3]) {
                ok = 0; break;
            }
            const u32* cx = definitions[vector[3]], *cy = definitions[vector[4]];
            ok = cx && cy && (*cx & 0xffff) == 43 && (*cy & 0xffff) == 43 && cx[3] == x && cy[3] == y;
            samples++;
        }
    }
    ok = ok && samples == (known ? 1u : 256u) && switches == (known ? 0u : 17u) && phis == (known ? 0u : 17u) &&
         !queries && !divisions && (known || (high == 1 && low == 16 && root && merge &&
         routes_offset(code, words, definitions, labels, root, root_label, merge, raw)));
    free(definitions); free(labels);
    return ok;
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
