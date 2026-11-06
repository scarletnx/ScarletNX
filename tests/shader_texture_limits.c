/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Independently seeded registers prove the LC word/coordinate order and fixed-point scale. Every constant-offset
   vector in the varying TXD switch is checked, including signed extrema and the 1D zero-y coordinate. */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const u32** defs;
    u32 bound, input_id, input_value;
} Definitions;
static u64 mov(u32 reg, u32 value) { return 0x0100000000071000ull | (u64)value << 20 | reg; }
static u32 bits(float value) { u32 result; memcpy(&result, &value, 4); return result; }
static Definitions definitions(const u32* code, size_t words) {
    Definitions d = {.defs = calloc(code[3], sizeof *d.defs), .bound = code[3]};
    if (!d.defs) return d;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        u32 op = code[at] & 0xffff, n = code[at] >> 16;
        if (!n || n > words - at) { free(d.defs); d.defs = NULL; return d; }
        if (op == 43 || op == 44 || op == 61 || op == 65 || op == 80 || op == 112 || op == 124 ||
            op == 133 || op == 194 || op == 199 || op == 202 || op == 12)
            if (n > 2 && code[at + 2] < d.bound) d.defs[code[at + 2]] = code + at;
    }
    return d;
}
static int scalar(const Definitions* d, u32 id, u32* value, u32 depth) {
    if (id && id == d->input_id) { *value = d->input_value; return 1; }
    if (depth > 32 || id >= d->bound || !d->defs[id]) return 0;
    const u32* op = d->defs[id];
    u32 a, b;
    if ((*op & 0xffff) == 43) { *value = op[3]; return 1; }
    if ((*op & 0xffff) == 124) return scalar(d, op[3], value, depth + 1);
    if ((*op & 0xffff) == 112 && scalar(d, op[3], &a, depth + 1)) { *value = bits((float)a); return 1; }
    if ((*op & 0xffff) == 194 && scalar(d, op[3], &a, depth + 1) && scalar(d, op[4], &b, depth + 1)) {
        *value = a >> (b & 31); return 1;
    }
    /* Array layers use the packed word's low16 bits, independently of its minimum-LOD high half. */
    if ((*op & 0xffff) == 199 && scalar(d, op[3], &a, depth + 1) && scalar(d, op[4], &b, depth + 1)) {
        *value = a & b; return 1;
    }
    if ((*op & 0xffff) == 202 && scalar(d, op[3], &a, depth + 1) && scalar(d, op[4], &b, depth + 1)) {
        u32 width;
        if (!scalar(d, op[5], &width, depth + 1) || !width || b >= 32 || width > 32 - b) return 0;
        u32 mask = width == 32 ? UINT32_MAX : (1u << width) - 1, field = a >> b & mask;
        *value = field & (1u << (width - 1)) ? field | ~mask : field;
        return 1;
    }
    if (((*op & 0xffff) == 133 || ((*op & 0xffff) == 12 && op[4] == 40)) &&
        scalar(d, op[(*op & 0xffff) == 133 ? 3 : 5], &a, depth + 1) &&
        scalar(d, op[(*op & 0xffff) == 133 ? 4 : 6], &b, depth + 1)) {
        float x, y;
        memcpy(&x, &a, 4); memcpy(&y, &b, 4);
        *value = bits((*op & 0xffff) == 133 ? x * y : x > y ? x : y);
        return 1;
    }
    return 0;
}
static int vector(const Definitions* d, u32 id, const u32* expected, u32 count) {
    if (id >= d->bound || !d->defs[id]) return 0;
    const u32* v = d->defs[id];
    if (((*v & 0xffff) != 80 && (*v & 0xffff) != 44) || *v >> 16 != count + 3) return 0;
    for (u32 k = 0; k < count; k++) {
        u32 value;
        if (!scalar(d, v[3 + k], &value, 0) || value != expected[k]) return 0;
    }
    return 1;
}
/* Independent operand checks run on every generated CS/FS module. These catch null sampled resources and
   undeclared extended-math imports even when the constant-value oracle can evaluate the intended expression. */
static int module_ids(const u32* code, size_t words) {
    if (words < 5 || !code[3]) return 0;
    u8* imports = calloc(code[3], 1);
    if (!imports) return 0;
    int ok = 1;
    for (size_t at = 5; ok && at < words;) {
        u32 width = code[at] >> 16, op = code[at] & 0xffff;
        if (!width || width > words - at) { ok = 0; break; }
        if (op == 11) {
            if (width < 3 || !code[at + 1] || code[at + 1] >= code[3]) { ok = 0; break; }
            imports[code[at + 1]] = 1;
        }
        if (op == 12 && (width < 5 || !code[at + 3] || code[at + 3] >= code[3] || !imports[code[at + 3]])) ok = 0;
        if (op == 55 || op == 57 || op == 61 || op == 62 || op == 65 || op == 245)
            for (u32 k = 1; k < width; k++)
                if (!code[at + k] || code[at + k] >= code[3]) ok = 0;
        if (op == 87 || op == 88) {
            if (width < 5 || (op == 88 && width < 7)) ok = 0;
            for (u32 k = 1; k < width; k++)
                if (k != 5 && (!code[at + k] || code[at + k] >= code[3])) ok = 0;
        }
        if (!ok) fprintf(stderr, "texture fixture invalid resource/math ID at word%zu opcode%u\n", at, op);
        at += width;
    }
    free(imports);
    return ok;
}
static u32* translate(const u64* ops, u32 count, int stage, ShaderInfo* info, size_t* words) {
    u8 bytes[1024] = {0};
    /* Both compute translators consume the same80-byte synthetic SPH used by vk/compute.c. */
    u32 at = 10;
    for (u32 k = 0; k < count; k++) {
        if (!(k % 3)) snx_st_le64(bytes + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(bytes + 8 * at++, ops[k]);
    }
    if (stage == ST_CS) info->cs_local[0] = info->cs_local[1] = info->cs_local[2] = 1;
    u32* code = shader_to_spirv(bytes, sizeof bytes, stage, info, words, 0, 1, 1, 1);
    if (code && !module_ids(code, *words)) { free(code); return NULL; }
    return code;
}
static int save(const char* dir, const char* name, u32 index, const u32* code, size_t words) {
    if (!dir) return 1;
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/%s-%u.spv", dir, name, index);
    if (n < 0 || n >= (int)sizeof path) return 0;
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(code, 4, words, f) == words;
    return fclose(f) == 0 && ok;
}
static int clamp_case(u32 ty, u32 mode, u32 bindless, int stage, const char* dir, u32 test) {
    u64 ops[16];
    u32 count = 0;
    ops[count++] = mov(4, 0x02800001); /* Minimum LOD2.5, layer1. */
    ops[count++] = mov(5, 0x3e800000); /* Coordinates0.25,0.625,0.875. */
    ops[count++] = mov(6, 0x3f200000);
    ops[count++] = mov(7, 0x3f600000);
    if (bindless) ops[count++] = 0x4c98078000070010ull | 2ull << 34 | 74ull << 20;
    ops[count++] = mov(16 + bindless, 0x3fa00000); /* Bias1.25. */
    ops[count++] = (bindless ? 0xdeb8000000070008ull : 0xc038000000070008ull | 74ull << 36) |
                   (u64)mode << (bindless ? 37 : 55) | 1ull << (bindless ? 40 : 58) | 1ull << 31 |
                   (u64)ty << 28 | 16ull << 20 | 4ull << 8;
    ops[count++] = 0xe30000000007000full;
    ShaderInfo info = {0};
    size_t words = 0;
    u32* code = translate(ops, count, stage, &info, &words);
    if (!code || info.unsupported) { fprintf(stderr, "LC test%u: %s\n", test, info.first_unsupported); free(code); return 0; }
    if (!save(dir, "tex-min-lod", test, code, words)) { free(code); return 0; }
    Definitions d = definitions(code, words);
    int ok = d.defs != NULL;
    u32 samples = 0, min_cap = 0;
    u32 expected[4] = {bits(0.25f), bits(ty < 2 ? 0.5f : 0.625f), bits(0.875f), bits(1.0f)};
    u32 components = ty >= 6 ? 3 : ty == 4 ? 3 : 2;
    if (ty & 1) expected[components++] = bits(1.0f);
    for (size_t at = 5; ok && at < words; at += code[at] >> 16) {
        u32 op = code[at] & 0xffff;
        if (op == 17 && code[at + 1] == 42) min_cap++;
        if (op == 87 || op == 88) {
            u32 length = code[at] >> 16, value;
            u32 mask = stage == ST_FS ? 128u | (mode ? 1u : 0u) : 2u;
            ok = code[at + 5] == mask && vector(&d, code[at + 4], expected, components) &&
                 scalar(&d, code[at + length - 1], &value, 0) && value == bits(2.5f);
            samples++;
        }
    }
    ok = ok && samples == 1 && min_cap == (stage == ST_FS ? 1u : 0u);
    if (!ok) fprintf(stderr, "LC test%u ty%u mode%u bindless%u stage%d failed operand proof\n", test, ty, mode, bindless, stage);
    free(d.defs); free(code);
    return ok;
}
static int clamp_register_case(u32 type, u32 bindless, int stage, const char* directory, u32 test) {
    u64 operations[4];
    u32 count = 0;
    operations[count++] = mov(4, 0x02800001);
    if (bindless) operations[count++] = 0x4c98078000070010ull | 2ull << 34 | 74ull << 20;
    operations[count++] = (bindless ? 0xdeb8000000070008ull : 0xc038000000070008ull | 74ull << 36) |
        1ull << (bindless ? 40 : 58) | 1ull << 31 | (u64)type << 28 | 16ull << 20 | 4ull << 8;
    operations[count++] = 0xe30000000007000full;
    ShaderInfo info = {0};
    size_t words = 0;
    /* Coordinates have no defining MOV. Their Function registers must be declared by the LC read's liveness
       analysis, including the additional packed-word prefix on non-array textures. */
    u32* code = translate(operations, count, stage, &info, &words);
    int ok = code && !info.unsupported && save(directory, "tex-min-lod-registers", test, code, words);
    if (!ok) fprintf(stderr, "LC register liveness type%u bindless%u stage%d: %s\n",
                     type, bindless, stage, info.first_unsupported);
    free(code);
    return ok;
}
/* Follow the actual emitted branches for every possible packed spatial offset. Check the native sample's
   constant offset and unchanged coordinate/gradient operands, then follow every Phi through its real predecessor.
   This catches incorrect nested-merge labels as well as selector/offset bit-order mistakes. */
static int dispatch_routes(const u32* code, size_t words, const Definitions* d, u32 count,
                            u32 unknown, u32 fixed) {
    size_t* labels = calloc(d->bound, sizeof *labels);
    if (!labels) return 0;
    size_t first = 0;
    u32 current = 0, root = 0, merge = 0, raw = 0, operands[5] = {0};
    int ok = 1;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        u32 op = code[at] & 0xffff;
        if (op == 248 && code[at + 1] < d->bound) labels[current = code[at + 1]] = at + 2;
        if (op == 247 && !first) merge = code[at + 1];
        if (op == 251 && !first) first = at, root = current;
    }
    if (!first || !merge) ok = 0;
    u32 cases = 1u << (4 * count);
    for (u32 bits = 0; ok && bits < cases; bits++) {
        if ((bits & ~unknown) != (fixed & (cases - 1) & ~unknown)) continue;
        size_t at = first;
        u32 block = root, predecessor = 0, result = 0, samples = 0, done = 0, steps = 0;
        while (ok && !done && ++steps < 64) {
            if (at >= words) { ok = 0; break; }
            const u32* instruction = code + at;
            u32 width = instruction[0] >> 16, op = instruction[0] & 0xffff;
            if (op == 251) {
                u32 selector = instruction[1], mask = 0;
                const u32* definition = selector < d->bound ? d->defs[selector] : NULL;
                if (!definition || (definition[0] & 0xffff) != 199 ||
                    !scalar(d, definition[4], &mask, 0)) { ok = 0; break; }
                if (!raw) raw = definition[3];
                if (raw != definition[3] || raw >= d->bound || !d->defs[raw] ||
                    (d->defs[raw][0] & 0xffff) != 194) { ok = 0; break; }
                u32 shift;
                if (!scalar(d, d->defs[raw][4], &shift, 0) || shift != 16) { ok = 0; break; }
                u32 label = instruction[2];
                for (u32 k = 3; k < width; k += 2)
                    if (instruction[k] == (bits & mask)) label = instruction[k + 1];
                if (label >= d->bound || !labels[label]) { ok = 0; break; }
                predecessor = block;
                block = label;
                at = labels[label];
                continue;
            }
            if (op == 88) {
                u32 expected[3] = {0};
                for (u32 axis = 0; axis < count; axis++) {
                    u32 nibble = bits >> (4 * axis) & 15;
                    expected[axis] = nibble >= 8 ? nibble - 16 : nibble;
                }
                if (width != 9 || instruction[5] != 12 ||
                    !vector(d, instruction[8], expected, count == 3 ? 3 : 2)) { ok = 0; break; }
                /* Each leaf loads its own sampled-image SSA ID. Identity is the unchanged descriptor pointer,
                   not that new load's result ID. Coordinates and explicit gradients are shared SSA values. */
                const u32* image = instruction[3] < d->bound ? d->defs[instruction[3]] : NULL;
                if (!image || (image[0] & 0xffff) != 61 || image[0] >> 16 != 4) { ok = 0; break; }
                const u32 sample_operands[5] = {instruction[1], image[3], instruction[4], instruction[6], instruction[7]};
                if (!operands[0]) memcpy(operands, sample_operands, sizeof operands);
                if (memcmp(operands, sample_operands, sizeof operands)) { ok = 0; break; }
                samples++;
                result = instruction[2];
            }
            if (op == 245) {
                u32 incoming = 0;
                for (u32 k = 3; k < width; k += 2)
                    if (instruction[k + 1] == predecessor) incoming = instruction[k];
                if (!result || incoming != result) { ok = 0; break; }
                result = instruction[2];
                if (block == merge) done = 1;
            }
            if (op == 249) {
                u32 label = instruction[1];
                if (label >= d->bound || !labels[label]) { ok = 0; break; }
                predecessor = block;
                block = label;
                at = labels[label];
                continue;
            }
            at += width;
        }
        if (!done || samples != 1) ok = 0;
        if (!ok) fprintf(stderr, "TXD dispatch path count%u packed%#x failed\n", count, bits);
    }
    free(labels);
    return ok;
}
static int varying_case(u32 ty, u32 bindless, const char* dir, u32 test) {
    u32 count = ty < 2 ? 1 : ty < 4 ? 2 : 3, a = bindless ? 12 : 4;
    u64 ops[12];
    u32 n = 0;
    if (bindless) ops[n++] = 0x4c9807800007000cull | 2ull << 34 | 74ull << 20;
    /* TID.X is a real varying source. The packed low half remains the array layer. */
    ops[n++] = 0xf0c8000002170000ull | (u64)(a + bindless + count);
    ops[n++] = (bindless ? 0xde40000000070008ull : 0xde00000000070008ull | 74ull << 36) |
               7ull << 51 | 1ull << 35 | 1ull << 31 | (u64)ty << 28 | 20ull << 20 | (u64)a << 8;
    ops[n++] = 0xe30000000007000full;
    ShaderInfo info = {0};
    size_t words = 0;
    u32* code = translate(ops, n, ST_CS, &info, &words);
    if (!code || info.unsupported) { fprintf(stderr, "TXD varying test%u: %s\n", test, info.first_unsupported); free(code); return 0; }
    Definitions d = definitions(code, words);
    u32 cases = 1u << (count * 4), samples = 0, switches = 0, phis = 0, queries = 0, divisions = 0;
    u32 functions = 0, calls = 0, dynamic_capability = 0;
    int ok = d.defs != NULL;
    for (size_t at = 5; ok && at < words; at += code[at] >> 16) {
        u32 op = code[at] & 0xffff;
        if (op == 54) functions++;
        if (op == 57) calls++;
        if (op == 17 && code[at + 1] == 25) dynamic_capability++;
        if (op == 251) { switches++; ok = (code[at] >> 16) <= 33; }
        if (op == 245) { phis++; ok = (code[at] >> 16) <= 35; }
        if (op == 247) ok = (code[at] >> 16) == 3 && code[at + 2] == 2; /* DontFlatten hint */
        if (op == 103) queries++;
        if (op == 136) divisions++;
        if (op == 88) {
            u32 expected[3] = {0};
            for (u32 axis = 0; axis < count; axis++) {
                u32 nibble = samples >> (4 * axis) & 15;
                expected[axis] = nibble >= 8 ? nibble - 16 : nibble;
            }
            ok = (code[at] >> 16) == 9 && code[at + 5] == 12 &&
                 vector(&d, code[at + 8], expected, count == 3 ? 3 : 2);
            samples++;
        }
    }
    u32 nodes = count == 3 ? 273 : count == 2 ? 17 : 1;
    ok = ok && switches == nodes && phis == nodes && samples == cases && !queries && !divisions &&
         functions == 1 && !calls && !dynamic_capability &&
         words < 40u * cases + 4096 && dispatch_routes(code, words, &d, count, cases - 1, 0) &&
         save(dir, "txd-varying-offset", test, code, words);
    if (!ok) fprintf(stderr, "TXD varying test%u ty%u bindless%u samples%u/%u switches%u\n", test, ty, bindless,
                     samples, cases, switches);
    printf("TXD offset domain%u bindless%u samples%u bytes%zu\n", count, bindless, samples, words * 4);
    free(d.defs); free(code);
    return ok;
}
static int partial_case(u32 test, const char* dir) {
    static const struct { u32 type, source_mask, shift, fixed, cases; } cases[] = {
        {1, 0xffff, 0, 0x00080000, 1},  /* Varying1D layer, constant offset-8. */
        {3, 0xffff, 0, 0x00f10000, 1},  /* Varying2D layer, constant offsets(1,-1). */
        {4, 15, 16, 0x02f00000, 16},   /* Only the3D X nibble varies. */
        {4, 9, 16, 0x02140000, 4},     /* Sparse X bits0/3, fixed Y/Z. */
        {2, 0x11, 16, 0x00a60000, 4},  /* Sparse bits in both2D coordinates. */
        {4, 0x1ff, 16, 0x02000000, 512}, /* Sparse3D domains retain every compatible offset. */
        {4, 0x3ff, 16, 0x04000000, 1024},
        {4, 0x7ff, 16, 0x08000000, 2048},
    };
    u32 type = cases[test].type, components = type == 4 ? 3 : 2, packed_reg = type == 4 ? 7 : type < 2 ? 5 : 6;
    u64 ops[8]; u32 n = 0;
    ops[n++] = 0xf0c8000002170000ull | packed_reg;
    ops[n++] = 0x0400000000070000ull | (u64)cases[test].source_mask << 20 | (u64)packed_reg << 8 | packed_reg;
    if (cases[test].shift) ops[n++] = 0x3848000000070000ull | (u64)cases[test].shift << 20 | (u64)packed_reg << 8 | packed_reg;
    ops[n++] = 0x0420000000070000ull | (u64)cases[test].fixed << 20 | (u64)packed_reg << 8 | packed_reg;
    ops[n++] = 0xde00000000070008ull | 74ull << 36 | 7ull << 51 | 1ull << 35 | 1ull << 31 |
               (u64)type << 28 | 20ull << 20 | 4ull << 8;
    ops[n++] = 0xe30000000007000full;
    ShaderInfo info = {0}; size_t words = 0;
    u32* code = translate(ops, n, ST_CS, &info, &words);
    if (!code || info.unsupported) { fprintf(stderr, "TXD partial test%u: %s\n", test, info.first_unsupported); free(code); return 0; }
    Definitions d = definitions(code, words);
    u32 samples = 0, switches = 0, phis = 0, unknown = (cases[test].source_mask << cases[test].shift >> 16) & ((1u << (4 * (type == 4 ? 3 : type < 2 ? 1 : 2))) - 1);
    int ok = d.defs != NULL;
    for (size_t at = 5; ok && at < words; at += code[at] >> 16) {
        u32 op = code[at] & 0xffff;
        if (op == 251) { switches++; ok = (code[at] >> 16) <= 33; }
        if (op == 245) { phis++; ok = (code[at] >> 16) <= 35; }
        if (op == 88) {
            u32 number = samples, value = cases[test].fixed >> 16, expected[3] = {0};
            for (u32 bit = 0; bit < 12; bit++)
                if (unknown & (1u << bit)) { value |= (number & 1) << bit; number >>= 1; }
            for (u32 axis = 0; axis < components; axis++) {
                u32 nibble = value >> (4 * axis) & 15;
                expected[axis] = nibble >= 8 ? nibble - 16 : nibble;
            }
            ok = code[at + 5] == 12 && vector(&d, code[at + 8], expected, components);
            samples++;
        }
    }
    u32 unknown_bits = 0, levels = 0, nodes = 0, prefix = 1;
    for (u32 bits = unknown; bits; bits >>= 1) unknown_bits += bits & 1;
    levels = (unknown_bits + 3) / 4;
    for (u32 level = 0; level < levels; level++) {
        nodes += prefix;
        prefix <<= level ? 4 : (unknown_bits - 1) % 4 + 1;
    }
    ok = ok && samples == cases[test].cases && switches == nodes && phis == nodes &&
         (samples == 1 || dispatch_routes(code, words, &d, type == 4 ? 3 : type < 2 ? 1 : 2,
                                         unknown, cases[test].fixed >> 16)) &&
         save(dir, "txd-partial-offset", test, code, words);
    if (!ok) fprintf(stderr, "TXD partial test%u samples%u expected%u switches%u\n", test, samples, cases[test].cases, switches);
    printf("TXD partial test%u samples%u bytes%zu\n", test, samples, words * 4);
    free(d.defs); free(code);
    return ok;
}
static struct { u32 reads; } g_native;
static int native_offset_read(u32 bank, u32 offset, u32* value) {
    (void)bank; (void)offset;
    g_native.reads++;
    *value = 0x00f10001;
    return 1;
}
static int native_case(u32 type, u32 bindless, int stage, u32 uniform, const char* directory, u32 test) {
    const float coordinates[3] = {.25f, .625f, .875f};
    const float gradient_x[3] = {.125f, .375f, .625f}, gradient_y[3] = {.25f, .5f, .75f};
    u32 count = type < 2 ? 1 : type < 4 ? 2 : 3, dims = count == 1 ? 2 : count;
    u32 array = type & 1, a = bindless ? 12 : 4, coordinate_reg = a + bindless, packed_reg = coordinate_reg + count;
    u64 operations[16];
    u32 n = 0;
    if (bindless) operations[n++] = 0x4c98078000070000ull | 2ull << 34 | 74ull << 20 | a;
    for (u32 axis = 0; axis < count; axis++) operations[n++] = mov(coordinate_reg + axis, bits(coordinates[axis]));
    for (u32 axis = 0; axis < count; axis++) {
        operations[n++] = mov(20 + 2 * axis, bits(gradient_x[axis]));
        operations[n++] = mov(21 + 2 * axis, bits(gradient_y[axis]));
    }
    /* TID.X is compute-only. Fragment programs use a real GPU constant-buffer load whose contents remain
       unknown during translation; the oracle injects all packed values at that load's SSA result. */
    operations[n++] = uniform || stage == ST_FS
        ? 0x4c98078000070000ull | 3ull << 34 | 0x80ull << 20 | packed_reg
        : 0xf0c8000002170000ull | packed_reg;
    operations[n++] = (bindless ? 0xde40000000070008ull : 0xde00000000070008ull | 74ull << 36) |
        7ull << 51 | 1ull << 35 | 1ull << 31 | (u64)type << 28 | 20ull << 20 | (u64)a << 8;
    operations[n++] = 0xe30000000007000full;
    ShaderInfo info = {0};
    size_t words = 0;
    g_native.reads = 0;
    g_shader_cb_read = uniform ? native_offset_read : NULL;
    u32* code = translate(operations, n, stage, &info, &words);
    g_shader_cb_read = NULL;
    if (!code || info.unsupported) { fprintf(stderr, "native TXD test%u: %s\n", test, info.first_unsupported); free(code); return 0; }
    Definitions d = definitions(code, words);
    int ok = d.defs && !g_native.reads;
    const u32* sample = NULL;
    u32 samples = 0, switches = 0, queries = 0, divisions = 0, functions = 0, calls = 0, dynamic_capability = 0;
    for (size_t at = 5; ok && at < words; at += code[at] >> 16) {
        u32 op = code[at] & 0xffff;
        if (op == 88) { samples++; sample = code + at; }
        if (op == 251) switches++;
        if (op == 103) queries++;
        if (op == 136) divisions++;
        if (op == 54) functions++;
        if (op == 57) calls++;
        if (op == 17 && code[at + 1] == 25) dynamic_capability++;
    }
    ok = ok && samples == 1 && !switches && !queries && !divisions && functions == 1 && !calls && dynamic_capability == 1 &&
        sample && sample[0] >> 16 == 9 && sample[5] == 20; /* Grad | Offset, never ConstOffset. */
    if (ok) {
        const u32* offset = sample[8] < d.bound ? d.defs[sample[8]] : NULL;
        const u32* x = offset && offset[3] < d.bound ? d.defs[offset[3]] : NULL;
        const u32* base = x && x[3] < d.bound ? d.defs[x[3]] : NULL;
        ok = offset && (offset[0] & 0xffff) == 80 && x && (x[0] & 0xffff) == 202 &&
            base && (base[0] & 0xffff) == 124;
        if (ok) d.input_id = base[3];
    }
    u32 cases = 1u << (4 * count);
    for (u32 packed = 0; ok && packed < cases; packed++)
        for (u32 layer_test = 0; ok && layer_test < 3; layer_test++) {
            u32 layer = layer_test == 2 ? 65535 : layer_test;
            d.input_value = packed << 16 | layer;
            u32 expected_offset[3] = {0}, expected_coordinates[4] = {0}, expected_x[3] = {0}, expected_y[3] = {0};
            for (u32 axis = 0; axis < count; axis++) {
                u32 nibble = packed >> (4 * axis) & 15;
                expected_offset[axis] = nibble >= 8 ? nibble - 16 : nibble;
                expected_coordinates[axis] = bits(coordinates[axis]);
                expected_x[axis] = bits(gradient_x[axis]);
                expected_y[axis] = bits(gradient_y[axis]);
            }
            if (count == 1) expected_coordinates[1] = bits(.5f);
            if (array) expected_coordinates[dims] = bits((float)layer);
            ok = vector(&d, sample[8], expected_offset, dims) &&
                vector(&d, sample[4], expected_coordinates, dims + array) &&
                vector(&d, sample[6], expected_x, dims) && vector(&d, sample[7], expected_y, dims);
        }
    ok = ok && save(directory, "txd-native-offset", test, code, words);
    if (!ok) fprintf(stderr, "native TXD test%u type%u bindless%u stage%d uniform%u samples%u callback%u failed\n",
                     test, type, bindless, stage, uniform, samples, g_native.reads);
    free(d.defs);
    free(code);
    return ok;
}
int main(int argc, char** argv) {
    g_shader_dynamic_texture_offsets = 0;
    const char* dir = argc == 2 ? argv[1] : NULL;
    int saved_min_lod = g_shader_resource_min_lod;
    g_shader_resource_min_lod = 0;
    for (u32 bindless = 0; bindless < 2; bindless++) {
        u64 ops[] = {0x4c98078000070010ull | 2ull << 34 | 74ull << 20,
                     (bindless ? 0xdeb8000000070008ull : 0xc038000000070008ull | 74ull << 36) |
                         1ull << (bindless ? 40 : 58) | 1ull << 31 | 2ull << 28 | 16ull << 20 | 4ull << 8,
                     0xe30000000007000full};
        ShaderInfo info = {0}; size_t words = 0;
        const u64* selected = ops + !bindless;
        u32 count = bindless ? 3 : 2;
        u32* code = translate(selected, count, ST_FS, &info, &words);
        int rejected = !code && info.unsupported && strstr(info.first_unsupported, "shaderResourceMinLod");
        free(code);
        if (!rejected) { fprintf(stderr, "missing MinLod feature guard bindless%u: %s\n", bindless, info.first_unsupported); return 1; }
        info = (ShaderInfo){0};
        code = translate(selected, count, ST_CS, &info, &words);
        int explicit_ok = code && !info.unsupported;
        free(code);
        if (!explicit_ok) { fprintf(stderr, "explicit LOD needs no MinLod feature bindless%u: %s\n", bindless, info.first_unsupported); return 1; }
    }
    g_shader_resource_min_lod = saved_min_lod;
    const u32 types[] = {0, 1, 2, 3, 4, 6, 7}, modes[] = {0, 2, 6};
    u32 tests = 0;
    for (u32 b = 0; b < 2; b++)
        for (u32 t = 0; t < sizeof types / sizeof *types; t++)
            for (u32 m = 0; m < sizeof modes / sizeof *modes; m++)
                for (u32 s = 0; s < 2; s++)
                    if (!clamp_case(types[t], modes[m], b, s ? ST_CS : ST_FS, dir, tests++)) return 1;
    u32 register_tests = 0;
    for (u32 b = 0; b < 2; b++)
        for (u32 t = 0; t < sizeof types / sizeof *types; t++)
            for (u32 s = 0; s < 2; s++)
                if (!clamp_register_case(types[t], b, s ? ST_CS : ST_FS, dir, register_tests++)) return 1;
    for (u32 b = 0; b < 2; b++)
        for (u32 ty = 0; ty <= 4; ty++)
            if (!varying_case(ty, b, dir, ty + 5 * b)) return 1;
    for (u32 test = 0; test < 8; test++)
        if (!partial_case(test, dir)) return 1;
    for (u32 mode = 1; mode < 8; mode++) {
        if (mode == 2 || mode == 6) continue;
        u64 ops[] = {0xc438000000070008ull | (u64)mode << 55 | 2ull << 28 | 1ull << 31,
                     0xe30000000007000full};
        ShaderInfo info = {0}; size_t words = 0;
        u32* code = translate(ops, 2, ST_FS, &info, &words);
        if (code || !info.unsupported) { free(code); return 1; }
    }
    u32 native_tests = 0;
    g_shader_dynamic_texture_offsets = 1;
    for (u32 b = 0; b < 2; b++)
        for (u32 type = 0; type <= 4; type++)
            for (u32 stage = 0; stage < 2; stage++)
                if (!native_case(type, b, stage ? ST_CS : ST_FS, 0, dir, native_tests++)) return 1;
    for (u32 stage = 0; stage < 2; stage++)
        if (!native_case(2, 0, stage ? ST_CS : ST_FS, 1, dir, native_tests++)) return 1;
    g_shader_dynamic_texture_offsets = 0;
    printf("PASS texture operand limits: %u LC modules,10 full/8 reduced fallback domains,%u native offset modules exact=1\n",
           tests, native_tests);
    return 0;
}
