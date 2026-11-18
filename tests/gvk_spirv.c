/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* src/nx/gpu/gvk/gvk_spirv.c: the SPIR-V that the console's Vulkan driver takes and Vulkan doesn't, mended for the
   host. An ArrayStride on a structure's member moves to the member's array type when the type has none, and goes when
   it has one. A matrix member with a MatrixStride and no RowMajor or ColMajor gets ColMajor, also through an array of
   matrices; one with RowMajor keeps it. A module that needs nothing, or that gvk can't read, is NULL */
#include "nx/gpu/gvk/gvk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* gvk.c's scratch: here, memory the test never frees */
void* gvk_scratch(size_t n) {
    void* p = calloc(1, n ? n : 1);
    assert(p);
    return p;
}

enum {
    OP_TYPE_INT = 21,
    OP_TYPE_FLOAT = 22,
    OP_TYPE_VECTOR = 23,
    OP_TYPE_MATRIX = 24,
    OP_TYPE_ARRAY = 28,
    OP_TYPE_STRUCT = 30,
    OP_CONSTANT = 43,
    OP_DECORATE = 71,
    OP_MEMBER_DECORATE = 72,
    ROW_MAJOR = 4,
    COL_MAJOR = 5,
    ARRAY_STRIDE = 6,
    MATRIX_STRIDE = 7,
};
/* the ids of the module */
enum { INT = 1, FLOAT, VEC4, MAT4, EIGHT, INTS, MATS, BLOCK, BOUND };

typedef struct {
    u32 w[128];
    u32 n;
} Module;

static void put(Module* m, u32 op, u32 count, const u32* words) {
    m->w[m->n++] = (count + 1) << 16 | op;
    for (u32 i = 0; i < count; i++) m->w[m->n++] = words[i];
}
#define PUT(m, op, ...) put((m), (op), sizeof((u32[]){__VA_ARGS__}) / 4, (u32[]){__VA_ARGS__})

static void header(Module* m) {
    static const u32 h[5] = {0x07230203u, 0x00010300u, 0, BOUND, 0};
    memcpy(m->w, h, sizeof h);
    m->n = 5;
}
/* the types after the decorations: int, float, vec4, mat4, int[8], mat4[8], and the block {int[8], mat4, mat4[8]} */
static void types(Module* m, int strided_ints) {
    if (strided_ints) PUT(m, OP_DECORATE, INTS, ARRAY_STRIDE, 16);
    PUT(m, OP_TYPE_INT, INT, 32, 1);
    PUT(m, OP_TYPE_FLOAT, FLOAT, 32);
    PUT(m, OP_TYPE_VECTOR, VEC4, FLOAT, 4);
    PUT(m, OP_TYPE_MATRIX, MAT4, VEC4, 4);
    PUT(m, OP_CONSTANT, INT, EIGHT, 8);
    PUT(m, OP_TYPE_ARRAY, INTS, INT, EIGHT);
    PUT(m, OP_TYPE_ARRAY, MATS, MAT4, EIGHT);
    PUT(m, OP_TYPE_STRUCT, BLOCK, INTS, MAT4, MATS);
}

/* how many instructions of op the module holds with exactly these operands */
static int count(const u32* w, size_t bytes, u32 op, u32 nops, const u32* ops) {
    int found = 0;
    for (u32 i = 5; i < bytes / 4; i += w[i] >> 16) {
        assert(w[i] >> 16);
        if ((w[i] & 0xffff) == op && (w[i] >> 16) == nops + 1 && !memcmp(w + i + 1, ops, nops * 4)) found++;
    }
    return found;
}
#define COUNT(w, bytes, op, ...) count((w), (bytes), (op), sizeof((u32[]){__VA_ARGS__}) / 4, (u32[]){__VA_ARGS__})

int main(void) {
    Module m;
    size_t bytes = 0;

    /* a member's ArrayStride goes on its array type, and matrices with no order get ColMajor */
    header(&m);
    PUT(&m, OP_MEMBER_DECORATE, BLOCK, 0, ARRAY_STRIDE, 4);
    PUT(&m, OP_MEMBER_DECORATE, BLOCK, 1, MATRIX_STRIDE, 16);
    PUT(&m, OP_MEMBER_DECORATE, BLOCK, 2, MATRIX_STRIDE, 16);
    types(&m, 0);
    const u32* out = gvk_spirv_mend(m.w, m.n * 4, &bytes);
    assert(out && bytes == (m.n + 4 + 4 + 4 - 5) * 4);
    assert(!memcmp(out, m.w, 5 * 4));
    assert(COUNT(out, bytes, OP_MEMBER_DECORATE, BLOCK, 0, ARRAY_STRIDE, 4) == 0);
    assert(COUNT(out, bytes, OP_DECORATE, INTS, ARRAY_STRIDE, 4) == 1);
    assert(COUNT(out, bytes, OP_MEMBER_DECORATE, BLOCK, 1, COL_MAJOR) == 1);
    assert(COUNT(out, bytes, OP_MEMBER_DECORATE, BLOCK, 2, COL_MAJOR) == 1); /* an array of matrices */
    assert(COUNT(out, bytes, OP_MEMBER_DECORATE, BLOCK, 1, MATRIX_STRIDE, 16) == 1);

    /* an array type with its own stride keeps it: the member's goes */
    header(&m);
    PUT(&m, OP_MEMBER_DECORATE, BLOCK, 0, ARRAY_STRIDE, 4);
    types(&m, 1);
    out = gvk_spirv_mend(m.w, m.n * 4, &bytes);
    assert(out && bytes == (m.n - 5) * 4);
    assert(COUNT(out, bytes, OP_DECORATE, INTS, ARRAY_STRIDE, 16) == 1);
    assert(COUNT(out, bytes, OP_DECORATE, INTS, ARRAY_STRIDE, 4) == 0);
    assert(COUNT(out, bytes, OP_MEMBER_DECORATE, BLOCK, 0, ARRAY_STRIDE, 4) == 0);

    /* a matrix with its order, given before or after its stride, is left alone: nothing to mend */
    header(&m);
    PUT(&m, OP_MEMBER_DECORATE, BLOCK, 1, MATRIX_STRIDE, 16);
    PUT(&m, OP_MEMBER_DECORATE, BLOCK, 1, ROW_MAJOR);
    PUT(&m, OP_MEMBER_DECORATE, BLOCK, 2, COL_MAJOR);
    PUT(&m, OP_MEMBER_DECORATE, BLOCK, 2, MATRIX_STRIDE, 16);
    types(&m, 0);
    assert(!gvk_spirv_mend(m.w, m.n * 4, &bytes));

    /* a MatrixStride on a member that is no matrix isn't given an order */
    header(&m);
    PUT(&m, OP_MEMBER_DECORATE, BLOCK, 0, MATRIX_STRIDE, 16);
    types(&m, 0);
    assert(!gvk_spirv_mend(m.w, m.n * 4, &bytes));

    /* not SPIR-V, a size that isn't whole words, an instruction past the end, or an instruction of no words */
    header(&m);
    PUT(&m, OP_MEMBER_DECORATE, BLOCK, 0, ARRAY_STRIDE, 4);
    types(&m, 0);
    u32 magic = m.w[0];
    m.w[0] = 0x03022307u;
    assert(!gvk_spirv_mend(m.w, m.n * 4, &bytes));
    m.w[0] = magic;
    assert(!gvk_spirv_mend(m.w, m.n * 4 - 2, &bytes));
    assert(!gvk_spirv_mend(m.w, (m.n - 1) * 4, &bytes));
    u32 first = m.w[5];
    m.w[5] = OP_MEMBER_DECORATE;
    assert(!gvk_spirv_mend(m.w, m.n * 4, &bytes));
    m.w[5] = first;
    assert(gvk_spirv_mend(m.w, m.n * 4, &bytes));
    return 0;
}
