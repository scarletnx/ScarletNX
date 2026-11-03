/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Typed syntax nodes into a SPIR-V 1.3 module for Vulkan 1.1. Each section of the module (capabilities, names,
   decorations, types and globals, functions) is a bounded word array, put together at the end, when the id bound is
   known; a function's local variables go to a section of their own while its code is written, since SPIR-V wants
   them in its first block. A value has one SPIR-V type per GLSL type. Memory laid out as std140 or std430 has types
   of its own, with offsets and strides, and bools become uints there (Vulkan has no bool in a block), so an array,
   struct or bool is converted member by member on its way into or out of a block. Control flow is structured as
   SPIR-V asks: every if, ?:, && and || has its merge block, every loop its merge and continue blocks. */
#include "glslc/internal.h"
#include <stdio.h>
#include <string.h>

/* SPIR-V's numbers */
enum {
    OP_UNDEF = 1,
    OP_NAME = 5,
    OP_MEMBER_NAME = 6,
    OP_EXT_INST_IMPORT = 11,
    OP_EXT_INST = 12,
    OP_MEMORY_MODEL = 14,
    OP_ENTRY_POINT = 15,
    OP_EXECUTION_MODE = 16,
    OP_CAPABILITY = 17,
    OP_TYPE_VOID = 19,
    OP_TYPE_BOOL = 20,
    OP_TYPE_INT = 21,
    OP_TYPE_FLOAT = 22,
    OP_TYPE_VECTOR = 23,
    OP_TYPE_MATRIX = 24,
    OP_TYPE_IMAGE = 25,
    OP_TYPE_SAMPLED_IMAGE = 27,
    OP_TYPE_ARRAY = 28,
    OP_TYPE_RUNTIME_ARRAY = 29,
    OP_TYPE_STRUCT = 30,
    OP_TYPE_POINTER = 32,
    OP_TYPE_FUNCTION = 33,
    OP_CONSTANT_TRUE = 41,
    OP_CONSTANT_FALSE = 42,
    OP_CONSTANT = 43,
    OP_CONSTANT_COMPOSITE = 44,
    OP_FUNCTION = 54,
    OP_FUNCTION_PARAMETER = 55,
    OP_FUNCTION_END = 56,
    OP_FUNCTION_CALL = 57,
    OP_VARIABLE = 59,
    OP_IMAGE_TEXEL_POINTER = 60,
    OP_LOAD = 61,
    OP_STORE = 62,
    OP_ACCESS_CHAIN = 65,
    OP_ARRAY_LENGTH = 68,
    OP_DECORATE = 71,
    OP_MEMBER_DECORATE = 72,
    OP_VECTOR_EXTRACT_DYNAMIC = 77,
    OP_VECTOR_SHUFFLE = 79,
    OP_COMPOSITE_CONSTRUCT = 80,
    OP_COMPOSITE_EXTRACT = 81,
    OP_TRANSPOSE = 84,
    OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87,
    OP_IMAGE_SAMPLE_EXPLICIT_LOD = 88,
    OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD = 89,
    OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD = 90,
    OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD = 91,
    OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD = 92,
    OP_IMAGE_SAMPLE_PROJ_DREF_IMPLICIT_LOD = 93,
    OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD = 94,
    OP_IMAGE_FETCH = 95,
    OP_IMAGE_GATHER = 96,
    OP_IMAGE_DREF_GATHER = 97,
    OP_IMAGE_READ = 98,
    OP_IMAGE_WRITE = 99,
    OP_IMAGE = 100,
    OP_IMAGE_QUERY_SIZE_LOD = 103,
    OP_IMAGE_QUERY_SIZE = 104,
    OP_CONVERT_F_TO_U = 109,
    OP_CONVERT_F_TO_S = 110,
    OP_CONVERT_S_TO_F = 111,
    OP_CONVERT_U_TO_F = 112,
    OP_BITCAST = 124,
    OP_S_NEGATE = 126,
    OP_F_NEGATE = 127,
    OP_I_ADD = 128,
    OP_F_ADD = 129,
    OP_I_SUB = 130,
    OP_F_SUB = 131,
    OP_I_MUL = 132,
    OP_F_MUL = 133,
    OP_U_DIV = 134,
    OP_S_DIV = 135,
    OP_F_DIV = 136,
    OP_U_MOD = 137,
    OP_S_MOD = 139,
    OP_VECTOR_TIMES_SCALAR = 142,
    OP_MATRIX_TIMES_SCALAR = 143,
    OP_VECTOR_TIMES_MATRIX = 144,
    OP_MATRIX_TIMES_VECTOR = 145,
    OP_MATRIX_TIMES_MATRIX = 146,
    OP_OUTER_PRODUCT = 147,
    OP_ANY = 154,
    OP_ALL = 155,
    OP_LOGICAL_EQUAL = 164,
    OP_LOGICAL_NOT_EQUAL = 165,
    OP_LOGICAL_OR = 166,
    OP_LOGICAL_AND = 167,
    OP_LOGICAL_NOT = 168,
    OP_SELECT = 169,
    OP_I_EQUAL = 170,
    OP_I_NOT_EQUAL = 171,
    OP_U_GREATER_THAN = 172,
    OP_S_GREATER_THAN = 173,
    OP_U_GREATER_THAN_EQUAL = 174,
    OP_S_GREATER_THAN_EQUAL = 175,
    OP_U_LESS_THAN = 176,
    OP_S_LESS_THAN = 177,
    OP_U_LESS_THAN_EQUAL = 178,
    OP_S_LESS_THAN_EQUAL = 179,
    OP_F_ORD_EQUAL = 180,
    OP_F_UNORD_NOT_EQUAL = 183,
    OP_F_ORD_LESS_THAN = 184,
    OP_F_ORD_GREATER_THAN = 186,
    OP_F_ORD_LESS_THAN_EQUAL = 188,
    OP_F_ORD_GREATER_THAN_EQUAL = 190,
    OP_SHIFT_RIGHT_LOGICAL = 194,
    OP_SHIFT_RIGHT_ARITHMETIC = 195,
    OP_SHIFT_LEFT_LOGICAL = 196,
    OP_BITWISE_OR = 197,
    OP_BITWISE_XOR = 198,
    OP_BITWISE_AND = 199,
    OP_NOT = 200,
    OP_CONTROL_BARRIER = 224,
    OP_MEMORY_BARRIER = 225,
    OP_ATOMIC_COMPARE_EXCHANGE = 230,
    OP_PHI = 245,
    OP_LOOP_MERGE = 246,
    OP_SELECTION_MERGE = 247,
    OP_LABEL = 248,
    OP_BRANCH = 249,
    OP_BRANCH_CONDITIONAL = 250,
    OP_SWITCH = 251,
    OP_KILL = 252,
    OP_RETURN = 253,
    OP_RETURN_VALUE = 254
};
enum {
    SC_UNIFORM_CONSTANT = 0,
    SC_INPUT = 1,
    SC_UNIFORM = 2,
    SC_OUTPUT = 3,
    SC_WORKGROUP = 4,
    SC_PRIVATE = 6,
    SC_FUNCTION = 7,
    SC_IMAGE = 11,
    SC_STORAGE_BUFFER = 12
};
enum {
    DEC_RELAXED_PRECISION = 0,
    DEC_BLOCK = 2,
    DEC_ROW_MAJOR = 4,
    DEC_COL_MAJOR = 5,
    DEC_ARRAY_STRIDE = 6,
    DEC_MATRIX_STRIDE = 7,
    DEC_BUILTIN = 11,
    DEC_FLAT = 14,
    DEC_CENTROID = 16,
    DEC_SAMPLE = 17,
    DEC_INVARIANT = 18,
    DEC_RESTRICT = 19,
    DEC_VOLATILE = 21,
    DEC_COHERENT = 23,
    DEC_NON_WRITABLE = 24,
    DEC_NON_READABLE = 25,
    DEC_LOCATION = 30,
    DEC_BINDING = 33,
    DEC_DESCRIPTOR_SET = 34,
    DEC_OFFSET = 35,
    DEC_NO_CONTRACTION = 42
};
enum {
    CAP_SHADER = 1,
    CAP_IMAGE_CUBE_ARRAY = 34,
    CAP_SAMPLE_RATE_SHADING = 35,
    CAP_SAMPLED_CUBE_ARRAY = 45,
    CAP_SAMPLED_BUFFER = 46,
    CAP_IMAGE_BUFFER = 47,
    CAP_IMAGE_QUERY = 50
};
enum { IMG_BIAS = 1, IMG_LOD = 2, IMG_GRAD = 4, IMG_CONST_OFFSET = 8, IMG_SAMPLE = 0x40 };
enum { K_PTR = 1, K_CONST, K_IMAGE, K_PAIR, K_SPLAT, K_UNDEF, K_ARRAY };
/* the image formats' SPIR-V numbers, in GLSLC_FMT_* order */
static const uint32_t spirv_formats[] = {0, 1, 2, 3, 4, 5, 21, 22, 23, 24, 30, 31, 32, 33};
/* GLSL.std.450 */
enum { X_MODF_STRUCT = 36, X_FREXP_STRUCT = 52 };

typedef struct {
    uint32_t base;    /* the variable, or the pointer the chain starts at */
    uint32_t storage; /* its SPIR-V storage class */
    uint32_t layout;  /* GLSLC_LAYOUT_* of the memory */
    uint32_t type;    /* the GLSL type the chain ends at */
    uint32_t img;     /* an image variable's SPIR-V image type */
    uint32_t vtype;   /* the vector a swizzle reads */
    uint32_t idx[16];
    uint32_t nidx;
    uint32_t swizzle, nswizzle; /* a swizzle of two or more components of the vector the chain ends at */
} Access;

static uint32_t rvalue(Glslc* g, uint32_t n);
static void statement(Glslc* g, uint32_t n);

static const GlslcType* ty(const Glslc* g, uint32_t t) { return &g->types[t]; }
static const GlslcNode* nd(const Glslc* g, uint32_t n) { return &g->nodes[n]; }
static uint32_t scalar_of(const Glslc* g, uint32_t t) { return glslc_vector_type(ty(g, t)->base, 1); }
static uint32_t rows_of(const Glslc* g, uint32_t t) { return ty(g, t)->kind == GLSLC_T_SCALAR ? 1u : ty(g, t)->rows; }
static uint32_t id(Glslc* g) { return g->e.next_id++; }
static uint32_t round_up(uint32_t v, uint32_t a) { return a ? (v + a - 1) / a * a : v; }

static void word(Glslc* g, GlslcWords* w, uint32_t v) {
    if (w->n >= w->cap)
        glslc_fail(g, g->e.site, "the SPIR-V module's %s need more than the %u words glslc gives them", w->what,
                   w->cap);
    w->w[w->n++] = v;
}

static void inst(Glslc* g, GlslcWords* w, uint32_t op, const uint32_t* ops, uint32_t n) {
    if (n + 1 > 0xffffu)
        glslc_fail(g, g->e.site, "a SPIR-V instruction would have %u words, more than the 65535 one can", n + 1);
    word(g, w, ((n + 1) << 16) | op);
    for (uint32_t i = 0; i < n; i++) word(g, w, ops[i]);
}

#define EMIT(g, w, op, ...)                                                                                            \
    inst((g), (w), (op), (const uint32_t[]){__VA_ARGS__},                                                              \
         (uint32_t)(sizeof((const uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t)))

/* the stack of ids an instruction with many operands gathers */
static uint32_t mark(const Glslc* g) { return g->e.nstack; }

static void push(Glslc* g, uint32_t v) {
    if (g->e.nstack >= GLSLC_STACK_WORDS)
        glslc_fail(g, g->e.site, "one SPIR-V instruction gathers more than the %u operands glslc takes",
                   GLSLC_STACK_WORDS);
    g->e.stack[g->e.nstack++] = v;
}

/* the instruction op with the ids pushed since at, which are popped */
static void inst_stack(Glslc* g, GlslcWords* w, uint32_t op, uint32_t at) {
    inst(g, w, op, &g->e.stack[at], g->e.nstack - at);
    g->e.nstack = at;
}

static void inst_string(Glslc* g, GlslcWords* w, uint32_t op, const uint32_t* pre, uint32_t npre, const char* text,
                        uint32_t len, const uint32_t* post, uint32_t npost) {
    uint32_t words = len / 4 + 1;
    word(g, w, ((npre + words + npost + 1) << 16) | op);
    for (uint32_t i = 0; i < npre; i++) word(g, w, pre[i]);
    for (uint32_t i = 0; i < words; i++) {
        uint32_t v = 0;
        for (uint32_t b = 0; b < 4; b++) {
            uint32_t k = i * 4 + b;
            if (k < len) v |= (uint32_t)(unsigned char)text[k] << (8 * b);
        }
        word(g, w, v);
    }
    for (uint32_t i = 0; i < npost; i++) word(g, w, post[i]);
}

static void name(Glslc* g, uint32_t target, GlslcView v) {
    if (v.len) inst_string(g, &g->e.names, OP_NAME, &target, 1, glslc_view_text(g, v), v.len, NULL, 0);
}

static void cap(Glslc* g, uint32_t c) {
    if ((g->e.cap_mask >> c) & 1u) return;
    g->e.cap_mask |= (uint64_t)1 << c;
    EMIT(g, &g->e.caps, OP_CAPABILITY, c);
}

static GlslcHashEntry* slot(Glslc* g, uint32_t k0, uint32_t k1, uint32_t k2, uint32_t k3) {
    uint32_t h = (k0 * 0x9e3779b1u) ^ (k1 * 0x85ebca77u) ^ (k2 * 0xc2b2ae3du) ^ (k3 * 0x27d4eb2fu);
    h ^= h >> 15;
    for (uint32_t i = 0; i < GLSLC_HASH_SIZE; i++) {
        GlslcHashEntry* e = &g->e.hash[(h + i) & (GLSLC_HASH_SIZE - 1u)];
        if (!e->id) {
            e->key[0] = k0;
            e->key[1] = k1;
            e->key[2] = k2;
            e->key[3] = k3;
            return e;
        }
        if (e->key[0] == k0 && e->key[1] == k1 && e->key[2] == k2 && e->key[3] == k3) return e;
    }
    glslc_fail(g, g->e.site, "the shader needs more than the %u types and constants glslc keeps", GLSLC_HASH_SIZE);
    return NULL;
}

static uint32_t element_of(const Glslc* g, uint32_t t) {
    while (ty(g, t)->kind == GLSLC_T_ARRAY) t = ty(g, t)->elem;
    return t;
}

/* A struct's samplers are no members of its SPIR-V struct, which Vulkan keeps to numbers: each is a variable of its
   own (sampler_variables), or a parameter of its own (emit_function), reached through the uniform or parameter that
   holds the struct. A struct holds samplers one deep, in one array dimension at most (parser.c) */
static int is_sampler_member(const Glslc* g, uint32_t t) { return ty(g, element_of(g, t))->kind == GLSLC_T_SAMPLER; }

/* t is a struct that holds samplers, or an array of them */
static int holds_samplers(const Glslc* g, uint32_t t) {
    const GlslcType* y = ty(g, element_of(g, t));
    return y->kind == GLSLC_T_STRUCT && (y->tflags & GLSLC_TF_OPAQUE);
}

/* member m of struct t as its SPIR-V struct numbers it, without the samplers before it */
static uint32_t spirv_member(const Glslc* g, uint32_t t, uint32_t m) {
    const GlslcType* y = ty(g, t);
    if (y->kind != GLSLC_T_STRUCT || !(y->tflags & GLSLC_TF_OPAQUE)) return m;
    uint32_t r = 0;
    for (uint32_t i = 0; i < m; i++) r += !is_sampler_member(g, g->members[y->first + i].type);
    return r;
}

/* the members of t's struct (t a struct, or an array of them) its SPIR-V struct has: none for samplers alone */
static uint32_t spirv_members(const Glslc* g, uint32_t t) {
    uint32_t e = element_of(g, t);
    return spirv_member(g, e, ty(g, e)->len);
}

/* layouts */

static int needs_layout(const Glslc* g, uint32_t t) {
    const GlslcType* y = ty(g, t);
    if (y->kind == GLSLC_T_ARRAY || y->kind == GLSLC_T_STRUCT) return 1;
    return (y->kind == GLSLC_T_SCALAR || y->kind == GLSLC_T_VECTOR) && y->base == SNX_GLSLC_BASE_BOOL;
}

/* an in or out block (GLSLC_PACK_NONE) is laid out by location: its struct has no offsets */
static uint32_t block_layout(uint8_t packing) {
    return packing == GLSLC_PACK_NONE     ? GLSLC_LAYOUT_NONE
           : packing == GLSLC_PACK_STD430 ? GLSLC_LAYOUT_STD430
                                          : GLSLC_LAYOUT_STD140;
}

static void lay(const Glslc* g, uint32_t t, uint32_t layout, int row_major, uint32_t* align, uint32_t* size);

static uint32_t matrix_stride(const Glslc* g, uint32_t t, uint32_t layout, int row_major) {
    uint32_t n = row_major ? ty(g, t)->cols : ty(g, t)->rows;
    uint32_t a = n == 2 ? 8u : 16u;
    if (layout == GLSLC_LAYOUT_STD140) a = 16;
    return round_up(4u * n, a);
}

static uint32_t array_stride(const Glslc* g, uint32_t t, uint32_t layout, int row_major) {
    uint32_t a, s;
    lay(g, ty(g, t)->elem, layout, row_major, &a, &s);
    if (layout == GLSLC_LAYOUT_STD140) a = round_up(a, 16);
    return round_up(s, a);
}

static void lay(const Glslc* g, uint32_t t, uint32_t layout, int row_major, uint32_t* align, uint32_t* size) {
    const GlslcType* y = ty(g, t);
    switch (y->kind) {
        case GLSLC_T_SCALAR: *align = *size = 4; return;
        case GLSLC_T_VECTOR:
            *align = y->rows == 2 ? 8u : 16u;
            *size = 4u * y->rows;
            return;
        case GLSLC_T_MATRIX: {
            uint32_t stride = matrix_stride(g, t, layout, row_major);
            *align = layout == GLSLC_LAYOUT_STD140 ? 16u : (row_major ? y->cols : y->rows) == 2 ? 8u : 16u;
            *size = stride * (row_major ? y->rows : y->cols);
            return;
        }
        case GLSLC_T_ARRAY: {
            uint32_t a, s;
            lay(g, y->elem, layout, row_major, &a, &s);
            if (layout == GLSLC_LAYOUT_STD140) a = round_up(a, 16);
            *align = a;
            *size = round_up(s, a) * y->len;
            return;
        }
        case GLSLC_T_STRUCT:
        case GLSLC_T_BLOCK: {
            uint32_t offset = 0, max = 4;
            for (uint32_t m = 0; m < y->len; m++) {
                const GlslcMember* mb = &g->members[y->first + m];
                if (is_sampler_member(g, mb->type)) continue;
                uint32_t a, s;
                lay(g, mb->type, layout, (mb->mflags & GLSLC_MEM_ROW_MAJOR) != 0, &a, &s);
                offset = round_up(offset, a) + s;
                if (a > max) max = a;
            }
            if (layout == GLSLC_LAYOUT_STD140) max = round_up(max, 16);
            *align = max;
            *size = round_up(offset, max);
            return;
        }
        default: *align = *size = 0;
    }
}

/* types */

static uint32_t type_id(Glslc* g, uint32_t t, uint32_t layout);

static uint32_t ptr_type(Glslc* g, uint32_t storage, uint32_t pointee) {
    GlslcHashEntry* e = slot(g, K_PTR, storage, pointee, 0);
    if (!e->id) {
        e->id = id(g);
        EMIT(g, &g->e.globals, OP_TYPE_POINTER, e->id, storage, pointee);
    }
    return e->id;
}

static uint32_t constant(Glslc* g, uint32_t t, uint32_t bits) {
    uint32_t tid = type_id(g, t, 0);
    GlslcHashEntry* e = slot(g, K_CONST, tid, bits, 0);
    if (!e->id) {
        e->id = id(g);
        if (t == GT_BOOL) {
            EMIT(g, &g->e.globals, bits ? OP_CONSTANT_TRUE : OP_CONSTANT_FALSE, tid, e->id);
        } else {
            EMIT(g, &g->e.globals, OP_CONSTANT, tid, e->id, bits);
        }
    }
    return e->id;
}

static uint32_t const_int(Glslc* g, int32_t v) { return constant(g, GT_INT, (uint32_t)v); }

/* t (a scalar or vector type) with every component bits */
static uint32_t const_splat(Glslc* g, uint32_t t, uint32_t bits) {
    uint32_t c = constant(g, scalar_of(g, t), bits);
    if (ty(g, t)->kind == GLSLC_T_SCALAR) return c;
    uint32_t tid = type_id(g, t, 0);
    GlslcHashEntry* e = slot(g, K_SPLAT, tid, bits, 0);
    if (!e->id) {
        uint32_t r = id(g), at = mark(g);
        push(g, tid);
        push(g, r);
        for (uint32_t i = 0; i < ty(g, t)->rows; i++) push(g, c);
        inst_stack(g, &g->e.globals, OP_CONSTANT_COMPOSITE, at);
        e = slot(g, K_SPLAT, tid, bits, 0);
        e->id = r;
    }
    return e->id;
}

/* one and zero in t's base type */
static uint32_t one_bits(const Glslc* g, uint32_t t) {
    return ty(g, t)->base == SNX_GLSLC_BASE_FLOAT ? 0x3f800000u : 1u;
}

static uint32_t image_type(Glslc* g, uint32_t t, uint32_t format) {
    const GlslcType* y = ty(g, t);
    uint32_t sampled = type_id(g, scalar_of(g, t), 0);
    GlslcHashEntry* e = slot(g, K_IMAGE, t, format, 0);
    if (e->id) return e->id;
    uint32_t r = id(g);
    static const uint32_t dims[] = {1, 2, 3, 5}; /* GLSLC_DIM_* as SPIR-V's Dim */
    int arrayed = (y->tflags & GLSLC_TF_ARRAYED) != 0;
    if (y->dim == GLSLC_DIM_CUBE && arrayed)
        cap(g, y->kind == GLSLC_T_IMAGE ? CAP_IMAGE_CUBE_ARRAY : CAP_SAMPLED_CUBE_ARRAY);
    if (y->dim == GLSLC_DIM_BUFFER) cap(g, y->kind == GLSLC_T_IMAGE ? CAP_IMAGE_BUFFER : CAP_SAMPLED_BUFFER);
    EMIT(g, &g->e.globals, OP_TYPE_IMAGE, r, sampled, dims[y->dim], (y->tflags & GLSLC_TF_SHADOW) ? 1u : 0u,
         arrayed ? 1u : 0u, (y->tflags & GLSLC_TF_MS) ? 1u : 0u, y->kind == GLSLC_T_IMAGE ? 2u : 1u,
         spirv_formats[format]);
    e = slot(g, K_IMAGE, t, format, 0);
    e->id = r;
    return r;
}

static void member_decorations(Glslc* g, uint32_t r, uint32_t t, uint32_t layout, int is_block) {
    const GlslcType* y = ty(g, t);
    uint32_t offset = 0;
    for (uint32_t i = 0, m = 0; i < y->len; i++) {
        const GlslcMember* mb = &g->members[y->first + i];
        if (is_sampler_member(g, mb->type)) continue;
        int row_major = is_block && (mb->mflags & GLSLC_MEM_ROW_MAJOR);
        uint32_t a, s;
        lay(g, mb->type, layout, row_major, &a, &s);
        offset = round_up(offset, a);
        EMIT(g, &g->e.annotations, OP_MEMBER_DECORATE, r, m, DEC_OFFSET, offset);
        uint32_t e = mb->type;
        while (ty(g, e)->kind == GLSLC_T_ARRAY) e = ty(g, e)->elem;
        if (ty(g, e)->kind == GLSLC_T_MATRIX) {
            EMIT(g, &g->e.annotations, OP_MEMBER_DECORATE, r, m, row_major ? DEC_ROW_MAJOR : DEC_COL_MAJOR);
            EMIT(g, &g->e.annotations, OP_MEMBER_DECORATE, r, m, DEC_MATRIX_STRIDE,
                 matrix_stride(g, e, layout, row_major));
        }
        if (is_block && (mb->mflags & GLSLC_MEM_READONLY))
            EMIT(g, &g->e.annotations, OP_MEMBER_DECORATE, r, m, DEC_NON_WRITABLE);
        if (is_block && (mb->mflags & GLSLC_MEM_WRITEONLY))
            EMIT(g, &g->e.annotations, OP_MEMBER_DECORATE, r, m, DEC_NON_READABLE);
        offset += s;
        m++;
    }
}

static uint32_t type_id(Glslc* g, uint32_t t, uint32_t layout) {
    const GlslcType* y = ty(g, t);
    if (!needs_layout(g, t)) layout = 0;
    if (y->kind == GLSLC_T_BLOCK) layout = block_layout(y->dim);
    uint32_t* cached = &g->e.type_ids[t * 3u + (y->kind == GLSLC_T_BLOCK ? 0u : layout)];
    if (*cached) return *cached;
    uint32_t r = 0;
    switch (y->kind) {
        case GLSLC_T_VOID:
            r = id(g);
            EMIT(g, &g->e.globals, OP_TYPE_VOID, r);
            break;
        case GLSLC_T_SCALAR:
            if (y->base == SNX_GLSLC_BASE_BOOL && layout) {
                r = type_id(g, GT_UINT, 0);
                break;
            }
            r = id(g);
            if (y->base == SNX_GLSLC_BASE_BOOL) {
                EMIT(g, &g->e.globals, OP_TYPE_BOOL, r);
            } else if (y->base == SNX_GLSLC_BASE_FLOAT) {
                EMIT(g, &g->e.globals, OP_TYPE_FLOAT, r, 32);
            } else {
                EMIT(g, &g->e.globals, OP_TYPE_INT, r, 32, y->base == SNX_GLSLC_BASE_INT ? 1u : 0u);
            }
            break;
        case GLSLC_T_VECTOR: {
            if (y->base == SNX_GLSLC_BASE_BOOL && layout) {
                r = type_id(g, glslc_vector_type(SNX_GLSLC_BASE_UINT, y->rows), 0);
                break;
            }
            uint32_t c = type_id(g, scalar_of(g, t), 0);
            r = id(g);
            EMIT(g, &g->e.globals, OP_TYPE_VECTOR, r, c, y->rows);
            break;
        }
        case GLSLC_T_MATRIX: {
            uint32_t c = type_id(g, glslc_vector_type(SNX_GLSLC_BASE_FLOAT, y->rows), 0);
            r = id(g);
            EMIT(g, &g->e.globals, OP_TYPE_MATRIX, r, c, y->cols);
            break;
        }
        case GLSLC_T_SAMPLER: {
            uint32_t img = image_type(g, t, 0);
            r = id(g);
            EMIT(g, &g->e.globals, OP_TYPE_SAMPLED_IMAGE, r, img);
            break;
        }
        case GLSLC_T_ARRAY: {
            uint32_t e = type_id(g, y->elem, layout);
            uint32_t len = y->len ? constant(g, GT_UINT, y->len) : 0;
            r = id(g);
            if (y->len) {
                EMIT(g, &g->e.globals, OP_TYPE_ARRAY, r, e, len);
            } else {
                EMIT(g, &g->e.globals, OP_TYPE_RUNTIME_ARRAY, r, e);
            }
            if (layout) EMIT(g, &g->e.annotations, OP_DECORATE, r, DEC_ARRAY_STRIDE, array_stride(g, t, layout, 0));
            break;
        }
        case GLSLC_T_STRUCT:
        case GLSLC_T_BLOCK: {
            uint32_t at = mark(g);
            push(g, 0);
            for (uint32_t m = 0; m < y->len; m++) {
                uint32_t mt = g->members[y->first + m].type;
                if (!is_sampler_member(g, mt)) push(g, type_id(g, mt, layout));
            }
            r = id(g);
            g->e.stack[at] = r;
            inst_stack(g, &g->e.globals, OP_TYPE_STRUCT, at);
            if (layout) member_decorations(g, r, t, layout, y->kind == GLSLC_T_BLOCK);
            name(g, r, y->name);
            for (uint32_t m = 0; m < y->len; m++) {
                GlslcView v = g->members[y->first + m].name;
                if (is_sampler_member(g, g->members[y->first + m].type)) continue;
                uint32_t pre[2] = {r, spirv_member(g, t, m)};
                inst_string(g, &g->e.names, OP_MEMBER_NAME, pre, 2, glslc_view_text(g, v), v.len, NULL, 0);
            }
            break;
        }
        default: glslc_fail(g, g->e.site, "glslc has no SPIR-V type for this kind of value");
    }
    g->e.type_ids[t * 3u + (y->kind == GLSLC_T_BLOCK ? 0u : layout)] = r;
    return r;
}

static uint32_t val_type(Glslc* g, uint32_t t) { return type_id(g, t, 0); }

/* the type of a struct's sampler variable or parameter: the member's, in the holder's array when it is one */
static uint32_t sampler_type(Glslc* g, uint32_t holder, uint32_t member) {
    uint32_t t = val_type(g, member);
    if (ty(g, holder)->kind != GLSLC_T_ARRAY) return t;
    uint32_t n = ty(g, holder)->len;
    GlslcHashEntry* e = slot(g, K_ARRAY, t, n, 0);
    if (e->id) return e->id;
    uint32_t r = id(g), len = constant(g, GT_UINT, n);
    EMIT(g, &g->e.globals, OP_TYPE_ARRAY, r, t, len);
    e = slot(g, K_ARRAY, t, n, 0); /* constant() may have taken the slot */
    e->id = r;
    return r;
}

/* a struct of two members, for the instructions that give two results (modf, uaddCarry...) */
static uint32_t pair_type(Glslc* g, uint32_t a, uint32_t b) {
    uint32_t ia = val_type(g, a), ib = val_type(g, b);
    GlslcHashEntry* e = slot(g, K_PAIR, ia, ib, 0);
    if (!e->id) {
        uint32_t r = id(g);
        EMIT(g, &g->e.globals, OP_TYPE_STRUCT, r, ia, ib);
        e = slot(g, K_PAIR, ia, ib, 0);
        e->id = r;
    }
    return e->id;
}

static uint32_t function_type(Glslc* g, uint32_t at) {
    /* SPIR-V takes one OpTypeFunction per signature: the one made before, if any */
    uint32_t n = g->e.nstack - at;
    GlslcWords* w = &g->e.globals;
    for (uint32_t i = 0; i < w->n;) {
        uint32_t len = w->w[i] >> 16;
        if ((w->w[i] & 0xffffu) == OP_TYPE_FUNCTION && len == n + 2 &&
            !memcmp(&w->w[i + 2], &g->e.stack[at], n * sizeof(uint32_t))) {
            g->e.nstack = at;
            return w->w[i + 1];
        }
        i += len ? len : 1;
    }
    uint32_t r = id(g);
    word(g, w, ((n + 2) << 16) | OP_TYPE_FUNCTION);
    word(g, w, r);
    for (uint32_t i = 0; i < n; i++) word(g, w, g->e.stack[at + i]);
    g->e.nstack = at;
    return r;
}

/* code */

static uint32_t op1(Glslc* g, uint32_t op, uint32_t type, uint32_t a) {
    uint32_t r = id(g);
    EMIT(g, &g->e.body, op, type, r, a);
    return r;
}

static uint32_t op2(Glslc* g, uint32_t op, uint32_t type, uint32_t a, uint32_t b) {
    uint32_t r = id(g);
    EMIT(g, &g->e.body, op, type, r, a, b);
    return r;
}

static uint32_t op3(Glslc* g, uint32_t op, uint32_t type, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = id(g);
    EMIT(g, &g->e.body, op, type, r, a, b, c);
    return r;
}

static uint32_t extract(Glslc* g, uint32_t type, uint32_t v, uint32_t index) {
    return op2(g, OP_COMPOSITE_EXTRACT, type, v, index);
}

static void label(Glslc* g, uint32_t l) {
    EMIT(g, &g->e.body, OP_LABEL, l);
    g->e.cur_block = l;
}

static void branch(Glslc* g, uint32_t target) {
    if (!g->e.cur_block) return;
    EMIT(g, &g->e.body, OP_BRANCH, target);
    g->e.cur_block = 0;
}

static uint32_t local_var(Glslc* g, uint32_t t) {
    uint32_t r = id(g);
    EMIT(g, &g->e.locals, OP_VARIABLE, ptr_type(g, SC_FUNCTION, val_type(g, t)), r, SC_FUNCTION);
    return r;
}

/* v (scalar t) as a vector of n components; a vector stays */
static uint32_t splat(Glslc* g, uint32_t v, uint32_t t, uint32_t n) {
    if (n <= 1 || ty(g, t)->kind != GLSLC_T_SCALAR) return v;
    uint32_t vt = glslc_vector_type(ty(g, t)->base, n), r = id(g), at = mark(g);
    push(g, val_type(g, vt));
    push(g, r);
    for (uint32_t i = 0; i < n; i++) push(g, v);
    inst_stack(g, &g->e.body, OP_COMPOSITE_CONSTRUCT, at);
    return r;
}

/* v of type from as type to, which has as many components: a GLSL constructor's conversion */
static uint32_t convert(Glslc* g, uint32_t v, uint32_t from, uint32_t to) {
    uint32_t fb = ty(g, from)->base, tb = ty(g, to)->base, tt = val_type(g, to);
    if (fb == tb) return v;
    if (fb == SNX_GLSLC_BASE_BOOL)
        return op3(g, OP_SELECT, tt, v, const_splat(g, to, one_bits(g, to)), const_splat(g, to, 0));
    if (tb == SNX_GLSLC_BASE_BOOL)
        return op2(g, fb == SNX_GLSLC_BASE_FLOAT ? OP_F_UNORD_NOT_EQUAL : OP_I_NOT_EQUAL, tt, v,
                   const_splat(g, from, 0));
    if (tb == SNX_GLSLC_BASE_FLOAT)
        return op1(g, fb == SNX_GLSLC_BASE_INT ? OP_CONVERT_S_TO_F : OP_CONVERT_U_TO_F, tt, v);
    if (fb == SNX_GLSLC_BASE_FLOAT)
        return op1(g, tb == SNX_GLSLC_BASE_INT ? OP_CONVERT_F_TO_S : OP_CONVERT_F_TO_U, tt, v);
    return op1(g, OP_BITCAST, tt, v);
}

/* a value of type t read from memory of layout into its own SPIR-V type, and back */
static uint32_t from_layout(Glslc* g, uint32_t v, uint32_t t, uint32_t layout, int into) {
    if (!layout || !needs_layout(g, t)) return v;
    const GlslcType* y = ty(g, t);
    if (y->kind == GLSLC_T_SCALAR || y->kind == GLSLC_T_VECTOR) {
        uint32_t ut = glslc_vector_type(SNX_GLSLC_BASE_UINT, rows_of(g, t));
        return into ? convert(g, v, ut, t) : convert(g, v, t, ut);
    }
    uint32_t n = y->len;
    if (y->kind == GLSLC_T_ARRAY && !y->len)
        glslc_fail(g, g->e.site, "an array with no size cannot be copied whole: read its elements");
    uint32_t r = id(g), at = mark(g);
    push(g, into ? val_type(g, t) : type_id(g, t, layout));
    push(g, r);
    for (uint32_t i = 0, k = 0; i < n; i++) {
        uint32_t et = y->kind == GLSLC_T_ARRAY ? y->elem : g->members[y->first + i].type;
        if (y->kind != GLSLC_T_ARRAY && is_sampler_member(g, et)) continue;
        uint32_t e = extract(g, into ? type_id(g, et, layout) : val_type(g, et), v, k++);
        push(g, from_layout(g, e, et, layout, into));
    }
    inst_stack(g, &g->e.body, OP_COMPOSITE_CONSTRUCT, at);
    return r;
}

/* the storage class a symbol's variable has */
static uint32_t storage_of(const Glslc* g, uint32_t s) {
    const GlslcSymbol* y = &g->syms[s];
    switch (y->storage) {
        case GLSLC_ST_GLOBAL:
        case GLSLC_ST_CONST:
            return y->kind == GLSLC_SK_VAR && g->nodes[y->node].kind == GLSLC_N_GLOBAL ? SC_PRIVATE : SC_FUNCTION;
        case GLSLC_ST_IN: return SC_INPUT;
        case GLSLC_ST_OUT: return SC_OUTPUT;
        case GLSLC_ST_UNIFORM: {
            uint32_t e = y->type;
            while (ty(g, e)->kind == GLSLC_T_ARRAY) e = ty(g, e)->elem;
            uint8_t k = ty(g, e)->kind;
            return k == GLSLC_T_SAMPLER || k == GLSLC_T_IMAGE ? SC_UNIFORM_CONSTANT : SC_UNIFORM;
        }
        case GLSLC_ST_BUFFER: return SC_STORAGE_BUFFER;
        case GLSLC_ST_SHARED: return SC_WORKGROUP;
        case GLSLC_ST_PARAM_IN: {
            uint32_t e = y->type;
            while (ty(g, e)->kind == GLSLC_T_ARRAY) e = ty(g, e)->elem;
            uint8_t k = ty(g, e)->kind;
            return k == GLSLC_T_SAMPLER || k == GLSLC_T_IMAGE ? SC_UNIFORM_CONSTANT : SC_FUNCTION;
        }
        default: return SC_FUNCTION;
    }
}

/* a constant's components, for SPIR-V constants: component i (column by column) of constant expression n, as n's
   base type */
static int const_flat(const Glslc* g, uint32_t n, uint32_t i, uint32_t* out) {
    for (uint32_t hops = 0; hops < 64; hops++) {
        const GlslcNode* x = nd(g, n);
        if (x->kind == GLSLC_N_CONST) {
            if (i) return 0;
            *out = x->a | ((uint32_t)x->b << 16);
            return 1;
        }
        if (x->kind == GLSLC_N_VAR) {
            const GlslcSymbol* s = &g->syms[x->a];
            if (s->kind == GLSLC_SK_BUILTIN && glslc_builtin_vars[s->aux].value == GLSLC_BUILTIN_VALUE_LOCAL_SIZE) {
                if (i > 2) return 0;
                *out = g->local_size[i];
                return 1;
            }
            if (s->storage != GLSLC_ST_CONST || !s->node || !nd(g, s->node)->b) return 0;
            n = nd(g, s->node)->b;
            continue;
        }
        if (x->kind != GLSLC_N_CONSTRUCT || !(x->flags & GLSLC_NF_CONST)) return 0;
        const GlslcType* t = ty(g, x->type);
        uint32_t first = x->a;
        uint32_t ft = nd(g, first)->type;
        if (!nd(g, first)->next && ty(g, ft)->kind == GLSLC_T_SCALAR) {
            uint32_t v;
            if (!const_flat(g, first, 0, &v)) return 0;
            if (t->kind == GLSLC_T_MATRIX && i / t->rows != i % t->rows) {
                *out = 0;
                return 1;
            }
            *out = glslc_fold_convert(g, v, ft, scalar_of(g, x->type));
            return 1;
        }
        if (!nd(g, first)->next && t->kind == GLSLC_T_MATRIX && ty(g, ft)->kind == GLSLC_T_MATRIX) {
            uint32_t c = i / t->rows, r = i % t->rows;
            const GlslcType* s = ty(g, ft);
            if (c < s->cols && r < s->rows) {
                i = c * s->rows + r;
                n = first;
                continue;
            }
            *out = c == r ? 0x3f800000u : 0;
            return 1;
        }
        for (uint32_t a = first; a; a = nd(g, a)->next) {
            uint32_t at = nd(g, a)->type, k = glslc_type_components(g, at);
            if (i < k) {
                uint32_t v;
                if (!const_flat(g, a, i, &v)) return 0;
                *out = glslc_fold_convert(g, v, scalar_of(g, at), scalar_of(g, x->type));
                return 1;
            }
            i -= k;
        }
        return 0;
    }
    return 0;
}

/* n as a SPIR-V constant, or 0 when it is not one glslc can write */
static uint32_t emit_constant(Glslc* g, uint32_t n, uint32_t depth) {
    const GlslcNode* x = nd(g, n);
    uint32_t t = x->type;
    if (depth > 32) return 0;
    if (x->kind == GLSLC_N_CONST) return constant(g, t, x->a | ((uint32_t)x->b << 16));
    if (x->kind == GLSLC_N_VAR) {
        const GlslcSymbol* s = &g->syms[x->a];
        int local_size =
            s->kind == GLSLC_SK_BUILTIN && glslc_builtin_vars[s->aux].value == GLSLC_BUILTIN_VALUE_LOCAL_SIZE;
        if (!local_size) {
            if (s->storage != GLSLC_ST_CONST || !s->node || !nd(g, s->node)->b) return 0;
            return emit_constant(g, nd(g, s->node)->b, depth + 1);
        }
    } else if (x->kind != GLSLC_N_CONSTRUCT || !(x->flags & GLSLC_NF_CONST)) {
        return 0;
    }
    const GlslcType* y = ty(g, t);
    uint32_t at = mark(g), r;
    if (y->kind == GLSLC_T_SCALAR || y->kind == GLSLC_T_VECTOR || y->kind == GLSLC_T_MATRIX) {
        uint32_t comps = glslc_type_components(g, t), v[16];
        for (uint32_t i = 0; i < comps; i++) {
            if (!const_flat(g, n, i, &v[i])) return 0;
        }
        if (y->kind == GLSLC_T_SCALAR) return constant(g, t, v[0]);
        uint32_t col = glslc_vector_type(y->base, y->rows), cols = y->kind == GLSLC_T_MATRIX ? y->cols : 1;
        uint32_t ids[4];
        for (uint32_t c = 0; c < cols; c++) {
            uint32_t q = id(g), m = mark(g);
            push(g, val_type(g, col));
            push(g, q);
            for (uint32_t k = 0; k < y->rows; k++) push(g, constant(g, scalar_of(g, t), v[c * y->rows + k]));
            inst_stack(g, &g->e.globals, OP_CONSTANT_COMPOSITE, m);
            ids[c] = q;
        }
        if (y->kind == GLSLC_T_VECTOR) return ids[0];
        r = id(g);
        push(g, val_type(g, t));
        push(g, r);
        for (uint32_t c = 0; c < cols; c++) push(g, ids[c]);
        inst_stack(g, &g->e.globals, OP_CONSTANT_COMPOSITE, at);
        return r;
    }
    if (x->kind != GLSLC_N_CONSTRUCT) return 0;
    push(g, val_type(g, t));
    push(g, 0);
    for (uint32_t a = x->a; a; a = nd(g, a)->next) {
        uint32_t c = emit_constant(g, a, depth + 1);
        if (!c) {
            g->e.nstack = at;
            return 0;
        }
        push(g, c);
    }
    r = id(g);
    g->e.stack[at + 1] = r;
    inst_stack(g, &g->e.globals, OP_CONSTANT_COMPOSITE, at);
    return r;
}

/* memory */

static void access_push(Glslc* g, Access* a, uint32_t index) {
    if (a->nidx == 16) { /* a long chain, in two instructions */
        uint32_t t = ty(g, a->type)->kind == GLSLC_T_IMAGE ? a->img : type_id(g, a->type, a->layout);
        uint32_t r = id(g), at = mark(g);
        push(g, ptr_type(g, a->storage, t));
        push(g, r);
        push(g, a->base);
        for (uint32_t i = 0; i < a->nidx; i++) push(g, a->idx[i]);
        inst_stack(g, &g->e.body, OP_ACCESS_CHAIN, at);
        a->base = r;
        a->nidx = 0;
    }
    a->idx[a->nidx++] = index;
}

/* how many of struct t's members before m are samplers: m's variable or parameter after the holder's first */
static uint32_t sampler_ordinal(const Glslc* g, uint32_t t, uint32_t m) { return m - spirv_member(g, t, m); }

/* sampler member m of the struct n names: VAR s, or INDEX(VAR s, i) of an array of them (parser.c's sampler_holder) */
static void sampler_access(Glslc* g, uint32_t n, uint32_t m, Access* a) {
    const GlslcNode* x = nd(g, n);
    uint32_t s = x->kind == GLSLC_N_INDEX ? nd(g, x->a)->a : x->a;
    uint32_t st = element_of(g, g->syms[s].type);
    memset(a, 0, sizeof *a);
    a->base = g->e.sampler_ids[s] + sampler_ordinal(g, st, m);
    a->storage = SC_UNIFORM_CONSTANT;
    a->type = g->members[ty(g, st)->first + m].type;
    if (x->kind == GLSLC_N_INDEX) a->idx[a->nidx++] = rvalue(g, x->b);
}

/* the memory n names, or 0 when n is a value */
static int access(Glslc* g, uint32_t n, Access* a) {
    const GlslcNode* x = nd(g, n);
    switch (x->kind) {
        case GLSLC_N_VAR: {
            uint32_t s = x->a;
            const GlslcSymbol* y = &g->syms[s];
            memset(a, 0, sizeof *a);
            a->type = y->type;
            if (y->kind == GLSLC_SK_BUILTIN && glslc_builtin_vars[y->aux].value == GLSLC_BUILTIN_VALUE_LOCAL_SIZE)
                return 0;
            if (y->kind == GLSLC_SK_BLOCK_MEMBER) {
                const GlslcSymbol* holder = &g->syms[y->aux];
                a->base = g->e.sym_ids[y->aux];
                a->storage = storage_of(g, y->aux);
                a->layout = block_layout(holder->packing);
                a->idx[a->nidx++] = const_int(g, y->aux2);
                return 1;
            }
            if (y->storage == GLSLC_ST_UNIFORM && g->e.sym_types[s] == 0xffffffffu) { /* a loose uniform */
                a->base = g->e.default_block;
                a->storage = SC_UNIFORM;
                a->layout = GLSLC_LAYOUT_STD140;
                a->idx[a->nidx++] = const_int(g, (int32_t)g->e.sym_ids[s]);
                return 1;
            }
            if (!g->e.sym_ids[s]) return 0;
            a->base = g->e.sym_ids[s];
            a->storage = storage_of(g, s);
            uint32_t e = y->type;
            while (ty(g, e)->kind == GLSLC_T_ARRAY) e = ty(g, e)->elem;
            if (ty(g, e)->kind == GLSLC_T_BLOCK) a->layout = block_layout(y->packing);
            if (ty(g, e)->kind == GLSLC_T_IMAGE) a->img = image_type(g, e, y->packing);
            return 1;
        }
        case GLSLC_N_INDEX:
            if (!access(g, x->a, a) || a->nswizzle) return 0;
            access_push(g, a, rvalue(g, x->b));
            a->type = x->type;
            return 1;
        case GLSLC_N_FIELD: {
            uint32_t st = nd(g, x->a)->type;
            if (ty(g, st)->kind == GLSLC_T_STRUCT && is_sampler_member(g, x->type)) {
                sampler_access(g, x->a, x->b, a);
                return 1;
            }
            if (!access(g, x->a, a)) return 0;
            access_push(g, a, const_int(g, (int32_t)spirv_member(g, st, x->b)));
            a->type = x->type;
            return 1;
        }
        case GLSLC_N_SWIZZLE:
            if (!access(g, x->a, a) || a->nswizzle) return 0;
            if (x->op == 1) {
                access_push(g, a, const_int(g, x->b & 3));
            } else {
                a->vtype = nd(g, x->a)->type;
                a->swizzle = x->b;
                a->nswizzle = x->op;
                a->type = x->type;
                return 1;
            }
            a->type = x->type;
            return 1;
        default: return 0;
    }
}

/* the pointer the access ends at, before its swizzle */
static uint32_t pointer_of(Glslc* g, const Access* a) {
    if (!a->nidx) return a->base;
    uint32_t t = a->nswizzle ? a->vtype : a->type;
    uint32_t pt = ty(g, t)->kind == GLSLC_T_IMAGE ? a->img : type_id(g, t, a->layout);
    uint32_t r = id(g), at = mark(g);
    push(g, ptr_type(g, a->storage, pt));
    push(g, r);
    push(g, a->base);
    for (uint32_t i = 0; i < a->nidx; i++) push(g, a->idx[i]);
    inst_stack(g, &g->e.body, OP_ACCESS_CHAIN, at);
    return r;
}

static uint32_t load_ptr(Glslc* g, uint32_t ptr, uint32_t t, uint32_t layout, uint32_t img) {
    uint32_t lt = ty(g, t)->kind == GLSLC_T_IMAGE ? img : type_id(g, t, layout);
    uint32_t v = op1(g, OP_LOAD, lt, ptr);
    return from_layout(g, v, t, layout, 1);
}

static uint32_t load(Glslc* g, const Access* a) {
    uint32_t ptr = pointer_of(g, a);
    if (!a->nswizzle) return load_ptr(g, ptr, a->type, a->layout, a->img);
    uint32_t v = load_ptr(g, ptr, a->vtype, a->layout, 0);
    uint32_t r = id(g), at = mark(g);
    push(g, val_type(g, a->type));
    push(g, r);
    push(g, v);
    push(g, v);
    for (uint32_t i = 0; i < a->nswizzle; i++) push(g, (a->swizzle >> (2 * i)) & 3u);
    inst_stack(g, &g->e.body, OP_VECTOR_SHUFFLE, at);
    return r;
}

/* v into the memory a names */
static void store(Glslc* g, const Access* a, uint32_t v) {
    uint32_t ptr = pointer_of(g, a);
    if (!a->nswizzle) {
        EMIT(g, &g->e.body, OP_STORE, ptr, from_layout(g, v, a->type, a->layout, 0));
        return;
    }
    uint32_t old = load_ptr(g, ptr, a->vtype, a->layout, 0);
    uint32_t size = ty(g, a->vtype)->rows, sel[4] = {0, 1, 2, 3};
    for (uint32_t i = 0; i < a->nswizzle; i++) sel[(a->swizzle >> (2 * i)) & 3u] = size + i;
    uint32_t r = id(g), at = mark(g);
    push(g, val_type(g, a->vtype));
    push(g, r);
    push(g, old);
    push(g, v);
    for (uint32_t i = 0; i < size; i++) push(g, sel[i]);
    inst_stack(g, &g->e.body, OP_VECTOR_SHUFFLE, at);
    EMIT(g, &g->e.body, OP_STORE, ptr, from_layout(g, r, a->vtype, a->layout, 0));
}

static void need_access(Glslc* g, uint32_t n, Access* a) {
    if (!access(g, n, a)) glslc_fail(g, g->e.site, "glslc found no memory behind a value it writes");
}

/* operators */

static uint32_t matrix_columns(Glslc* g, uint32_t op, uint32_t rt, uint32_t lt, uint32_t lv, uint32_t rtt,
                               uint32_t rv) {
    /* a matrix with a matrix or scalar, column by column */
    const GlslcType* m = ty(g, rt);
    uint32_t col = glslc_vector_type(SNX_GLSLC_BASE_FLOAT, m->rows), ct = val_type(g, col);
    uint32_t lsplat = ty(g, lt)->kind == GLSLC_T_SCALAR ? splat(g, lv, lt, m->rows) : 0;
    uint32_t rsplat = ty(g, rtt)->kind == GLSLC_T_SCALAR ? splat(g, rv, rtt, m->rows) : 0;
    uint32_t r = id(g), at = mark(g);
    push(g, val_type(g, rt));
    push(g, r);
    for (uint32_t c = 0; c < m->cols; c++) {
        uint32_t a = lsplat ? lsplat : extract(g, ct, lv, c);
        uint32_t b = rsplat ? rsplat : (rv ? extract(g, ct, rv, c) : 0);
        push(g, b ? op2(g, op, ct, a, b) : op1(g, op, ct, a));
    }
    inst_stack(g, &g->e.body, OP_COMPOSITE_CONSTRUCT, at);
    return r;
}

static uint32_t compare_all(Glslc* g, uint32_t t, uint32_t a, uint32_t b, int equal);

static uint32_t binary_value(Glslc* g, uint8_t op, uint32_t rt, uint32_t lt, uint32_t lv, uint32_t rtt, uint32_t rv) {
    const GlslcType *l = ty(g, lt), *r = ty(g, rtt);
    uint32_t base = l->base, tid = val_type(g, rt);
    int fl = base == SNX_GLSLC_BASE_FLOAT, si = base == SNX_GLSLC_BASE_INT;
    switch (op) {
        case GLSLC_OP_MUL:
            if (l->kind == GLSLC_T_MATRIX && r->kind == GLSLC_T_MATRIX)
                return op2(g, OP_MATRIX_TIMES_MATRIX, tid, lv, rv);
            if (l->kind == GLSLC_T_MATRIX && r->kind == GLSLC_T_VECTOR)
                return op2(g, OP_MATRIX_TIMES_VECTOR, tid, lv, rv);
            if (l->kind == GLSLC_T_VECTOR && r->kind == GLSLC_T_MATRIX)
                return op2(g, OP_VECTOR_TIMES_MATRIX, tid, lv, rv);
            if (l->kind == GLSLC_T_MATRIX) return op2(g, OP_MATRIX_TIMES_SCALAR, tid, lv, rv);
            if (r->kind == GLSLC_T_MATRIX) return op2(g, OP_MATRIX_TIMES_SCALAR, tid, rv, lv);
            if (fl && l->kind == GLSLC_T_VECTOR && r->kind == GLSLC_T_SCALAR)
                return op2(g, OP_VECTOR_TIMES_SCALAR, tid, lv, rv);
            if (fl && r->kind == GLSLC_T_VECTOR && l->kind == GLSLC_T_SCALAR)
                return op2(g, OP_VECTOR_TIMES_SCALAR, tid, rv, lv);
            /* fall through: the same shapes, or an int vector and scalar */
        case GLSLC_OP_ADD:
        case GLSLC_OP_SUB:
        case GLSLC_OP_DIV:
        case GLSLC_OP_MOD: {
            uint32_t code;
            switch (op) {
                case GLSLC_OP_ADD: code = fl ? OP_F_ADD : OP_I_ADD; break;
                case GLSLC_OP_SUB: code = fl ? OP_F_SUB : OP_I_SUB; break;
                case GLSLC_OP_MUL: code = fl ? OP_F_MUL : OP_I_MUL; break;
                case GLSLC_OP_DIV: code = fl ? OP_F_DIV : si ? OP_S_DIV : OP_U_DIV; break;
                default: code = si ? OP_S_MOD : OP_U_MOD;
            }
            if (ty(g, rt)->kind == GLSLC_T_MATRIX) return matrix_columns(g, code, rt, lt, lv, rtt, rv);
            uint32_t n = rows_of(g, rt);
            return op2(g, code, tid, splat(g, lv, lt, n), splat(g, rv, rtt, n));
        }
        case GLSLC_OP_AND:
        case GLSLC_OP_OR:
        case GLSLC_OP_XOR: {
            uint32_t n = rows_of(g, rt);
            uint32_t code = op == GLSLC_OP_AND ? OP_BITWISE_AND : op == GLSLC_OP_OR ? OP_BITWISE_OR : OP_BITWISE_XOR;
            return op2(g, code, tid, splat(g, lv, lt, n), splat(g, rv, rtt, n));
        }
        case GLSLC_OP_SHL:
        case GLSLC_OP_SHR: {
            uint32_t code = op == GLSLC_OP_SHL ? OP_SHIFT_LEFT_LOGICAL
                            : si               ? OP_SHIFT_RIGHT_ARITHMETIC
                                               : OP_SHIFT_RIGHT_LOGICAL;
            return op2(g, code, tid, lv, splat(g, rv, rtt, rows_of(g, lt)));
        }
        case GLSLC_OP_LT: return op2(g, fl ? OP_F_ORD_LESS_THAN : si ? OP_S_LESS_THAN : OP_U_LESS_THAN, tid, lv, rv);
        case GLSLC_OP_GT:
            return op2(g, fl ? OP_F_ORD_GREATER_THAN : si ? OP_S_GREATER_THAN : OP_U_GREATER_THAN, tid, lv, rv);
        case GLSLC_OP_LE:
            return op2(g,
                       fl   ? OP_F_ORD_LESS_THAN_EQUAL
                       : si ? OP_S_LESS_THAN_EQUAL
                            : OP_U_LESS_THAN_EQUAL,
                       tid, lv, rv);
        case GLSLC_OP_GE:
            return op2(g,
                       fl   ? OP_F_ORD_GREATER_THAN_EQUAL
                       : si ? OP_S_GREATER_THAN_EQUAL
                            : OP_U_GREATER_THAN_EQUAL,
                       tid, lv, rv);
        case GLSLC_OP_EQ: return compare_all(g, lt, lv, rv, 1);
        case GLSLC_OP_NE: return compare_all(g, lt, lv, rv, 0);
        case GLSLC_OP_LOGIC_XOR: return op2(g, OP_LOGICAL_NOT_EQUAL, tid, lv, rv);
        case GLSLC_OP_LOGIC_AND: return op2(g, OP_LOGICAL_AND, tid, lv, rv);
        case GLSLC_OP_LOGIC_OR: return op2(g, OP_LOGICAL_OR, tid, lv, rv);
        default: glslc_fail(g, g->e.site, "glslc writes no SPIR-V for operator %u", op); return 0;
    }
}

/* a == b (equal) or a != b of type t, as one bool */
static uint32_t compare_all(Glslc* g, uint32_t t, uint32_t a, uint32_t b, int equal) {
    const GlslcType* y = ty(g, t);
    uint32_t bt = val_type(g, GT_BOOL);
    if (y->kind == GLSLC_T_SCALAR || y->kind == GLSLC_T_VECTOR) {
        uint32_t code = y->base == SNX_GLSLC_BASE_FLOAT  ? (equal ? OP_F_ORD_EQUAL : OP_F_UNORD_NOT_EQUAL)
                        : y->base == SNX_GLSLC_BASE_BOOL ? (equal ? OP_LOGICAL_EQUAL : OP_LOGICAL_NOT_EQUAL)
                                                         : (equal ? OP_I_EQUAL : OP_I_NOT_EQUAL);
        if (y->kind == GLSLC_T_SCALAR) return op2(g, code, bt, a, b);
        uint32_t v = op2(g, code, val_type(g, glslc_vector_type(SNX_GLSLC_BASE_BOOL, y->rows)), a, b);
        return op1(g, equal ? OP_ALL : OP_ANY, bt, v);
    }
    uint32_t n = y->kind == GLSLC_T_MATRIX ? y->cols : y->len, result = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t et = y->kind == GLSLC_T_MATRIX  ? glslc_vector_type(SNX_GLSLC_BASE_FLOAT, y->rows)
                      : y->kind == GLSLC_T_ARRAY ? y->elem
                                                 : g->members[y->first + i].type;
        uint32_t c = compare_all(g, et, extract(g, val_type(g, et), a, i), extract(g, val_type(g, et), b, i), equal);
        result = result ? op2(g, equal ? OP_LOGICAL_AND : OP_LOGICAL_OR, bt, result, c) : c;
    }
    return result;
}

/* a value of type t whose every component is one (++ and --) */
static uint32_t ones(Glslc* g, uint32_t t) {
    const GlslcType* y = ty(g, t);
    if (y->kind != GLSLC_T_MATRIX) return const_splat(g, t, one_bits(g, t));
    uint32_t col = const_splat(g, glslc_vector_type(SNX_GLSLC_BASE_FLOAT, y->rows), 0x3f800000u);
    uint32_t r = id(g), at = mark(g);
    push(g, val_type(g, t));
    push(g, r);
    for (uint32_t c = 0; c < y->cols; c++) push(g, col);
    inst_stack(g, &g->e.globals, OP_CONSTANT_COMPOSITE, at);
    return r;
}

/* the components of value v (type t), each as a scalar of base, pushed on the stack: a constructor's arguments */
static void push_components(Glslc* g, uint32_t v, uint32_t t, uint32_t base, uint32_t* left) {
    const GlslcType* y = ty(g, t);
    uint32_t to = glslc_vector_type(base, 1);
    if (y->kind == GLSLC_T_SCALAR) {
        if (*left) {
            push(g, convert(g, v, t, to));
            --*left;
        }
        return;
    }
    if (y->kind == GLSLC_T_VECTOR) {
        uint32_t st = scalar_of(g, t);
        for (uint32_t i = 0; i < y->rows && *left; i++)
            push_components(g, extract(g, val_type(g, st), v, i), st, base, left);
        return;
    }
    uint32_t col = glslc_vector_type(SNX_GLSLC_BASE_FLOAT, y->rows);
    for (uint32_t c = 0; c < y->cols && *left; c++)
        push_components(g, extract(g, val_type(g, col), v, c), col, base, left);
}

static uint32_t construct(Glslc* g, uint32_t n) {
    const GlslcNode* x = nd(g, n);
    uint32_t t = x->type, c = emit_constant(g, n, 0);
    if (c) return c;
    const GlslcType* y = ty(g, t);
    uint32_t first = x->a, ft = nd(g, first)->type;
    if (y->kind == GLSLC_T_ARRAY || y->kind == GLSLC_T_STRUCT) {
        uint32_t at = mark(g), r = id(g);
        push(g, val_type(g, t));
        push(g, r);
        for (uint32_t a = first; a; a = nd(g, a)->next) push(g, rvalue(g, a));
        inst_stack(g, &g->e.body, OP_COMPOSITE_CONSTRUCT, at);
        return r;
    }
    int single = !nd(g, first)->next;
    if (y->kind == GLSLC_T_SCALAR) {
        uint32_t v = rvalue(g, first), left = 1, at = mark(g);
        push_components(g, v, ft, y->base, &left);
        uint32_t s = g->e.stack[at];
        g->e.nstack = at;
        return s;
    }
    if (single && ty(g, ft)->kind == GLSLC_T_SCALAR) {
        uint32_t v = convert(g, rvalue(g, first), ft, scalar_of(g, t));
        if (y->kind == GLSLC_T_VECTOR) return splat(g, v, scalar_of(g, t), y->rows);
        /* a matrix with v on its diagonal */
        uint32_t col = glslc_vector_type(SNX_GLSLC_BASE_FLOAT, y->rows), zero = constant(g, GT_FLOAT, 0);
        uint32_t r = id(g), at = mark(g);
        push(g, val_type(g, t));
        push(g, r);
        for (uint32_t cc = 0; cc < y->cols; cc++) {
            uint32_t q = id(g), m = mark(g);
            push(g, val_type(g, col));
            push(g, q);
            for (uint32_t k = 0; k < y->rows; k++) push(g, k == cc ? v : zero);
            inst_stack(g, &g->e.body, OP_COMPOSITE_CONSTRUCT, m);
            push(g, q);
        }
        inst_stack(g, &g->e.body, OP_COMPOSITE_CONSTRUCT, at);
        return r;
    }
    if (single && y->kind == GLSLC_T_MATRIX && ty(g, ft)->kind == GLSLC_T_MATRIX) {
        /* a matrix from a matrix: what fits, the identity around it */
        const GlslcType* s = ty(g, ft);
        uint32_t v = rvalue(g, first), col = glslc_vector_type(SNX_GLSLC_BASE_FLOAT, y->rows);
        uint32_t scol = glslc_vector_type(SNX_GLSLC_BASE_FLOAT, s->rows);
        uint32_t r = id(g), at = mark(g);
        push(g, val_type(g, t));
        push(g, r);
        for (uint32_t cc = 0; cc < y->cols; cc++) {
            uint32_t sc = cc < s->cols ? extract(g, val_type(g, scol), v, cc) : 0;
            uint32_t q = id(g), m = mark(g);
            push(g, val_type(g, col));
            push(g, q);
            for (uint32_t k = 0; k < y->rows; k++) {
                if (sc && k < s->rows) {
                    push(g, extract(g, val_type(g, GT_FLOAT), sc, k));
                } else {
                    push(g, constant(g, GT_FLOAT, k == cc ? 0x3f800000u : 0));
                }
            }
            inst_stack(g, &g->e.body, OP_COMPOSITE_CONSTRUCT, m);
            push(g, q);
        }
        inst_stack(g, &g->e.body, OP_COMPOSITE_CONSTRUCT, at);
        return r;
    }
    /* components, in order, until the type has them all */
    uint32_t need = glslc_type_components(g, t), left = need, comps = mark(g);
    for (uint32_t a = first; a && left; a = nd(g, a)->next)
        push_components(g, rvalue(g, a), nd(g, a)->type, y->base, &left);
    uint32_t r = id(g);
    if (y->kind == GLSLC_T_VECTOR) {
        uint32_t at = mark(g);
        push(g, val_type(g, t));
        push(g, r);
        for (uint32_t i = 0; i < need; i++) push(g, g->e.stack[comps + i]);
        inst_stack(g, &g->e.body, OP_COMPOSITE_CONSTRUCT, at);
        g->e.nstack = comps;
        return r;
    }
    uint32_t col = glslc_vector_type(SNX_GLSLC_BASE_FLOAT, y->rows), cols[4];
    for (uint32_t cc = 0; cc < y->cols; cc++) {
        uint32_t q = id(g), m = mark(g);
        push(g, val_type(g, col));
        push(g, q);
        for (uint32_t k = 0; k < y->rows; k++) push(g, g->e.stack[comps + cc * y->rows + k]);
        inst_stack(g, &g->e.body, OP_COMPOSITE_CONSTRUCT, m);
        cols[cc] = q;
    }
    uint32_t m = mark(g);
    push(g, val_type(g, t));
    push(g, r);
    for (uint32_t cc = 0; cc < y->cols; cc++) push(g, cols[cc]);
    inst_stack(g, &g->e.body, OP_COMPOSITE_CONSTRUCT, m);
    g->e.nstack = comps;
    return r;
}

/* built-in functions */

static uint32_t args_values(Glslc* g, uint32_t first, uint32_t* v, uint32_t* t, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t a = first; a && n < max; a = nd(g, a)->next, n++) {
        t[n] = nd(g, a)->type;
        v[n] = rvalue(g, a);
    }
    return n;
}

static uint32_t ext_inst(Glslc* g, uint32_t rt, uint32_t code, const uint32_t* v, uint32_t n) {
    uint32_t r = id(g), at = mark(g);
    push(g, val_type(g, rt));
    push(g, r);
    push(g, g->e.glsl_ext);
    push(g, code);
    for (uint32_t i = 0; i < n; i++) push(g, v[i]);
    inst_stack(g, &g->e.body, OP_EXT_INST, at);
    return r;
}

static void store_out(Glslc* g, uint32_t arg, uint32_t v) {
    Access a;
    need_access(g, arg, &a);
    store(g, &a, v);
}

static uint32_t texture_value(Glslc* g, const GlslcNode* x, const GlslcBuiltinFn* f) {
    uint32_t v[8], t[8];
    uint32_t n = args_values(g, x->a, v, t, 8), fl = f->code, rt = x->type, tid = val_type(g, rt);
    const GlslcType* s = ty(g, t[0]);
    int shadow = (s->tflags & GLSLC_TF_SHADOW) != 0, arrayed = (s->tflags & GLSLC_TF_ARRAYED) != 0;
    int ms = (s->tflags & GLSLC_TF_MS) != 0, cube = s->dim == GLSLC_DIM_CUBE;
    if (fl & (GLSLC_TX_IMAGE_LOAD | GLSLC_TX_IMAGE_STORE | GLSLC_TX_IMAGE_SIZE)) {
        if (fl & GLSLC_TX_IMAGE_SIZE) {
            cap(g, CAP_IMAGE_QUERY);
            return op1(g, OP_IMAGE_QUERY_SIZE, tid, v[0]);
        }
        if (fl & GLSLC_TX_IMAGE_LOAD) return op2(g, OP_IMAGE_READ, tid, v[0], v[1]);
        EMIT(g, &g->e.body, OP_IMAGE_WRITE, v[0], v[1], v[2]);
        return 0;
    }
    if (fl & GLSLC_TX_IMAGE_ATOMIC) {
        Access a;
        need_access(g, x->a, &a);
        uint32_t img_ptr = pointer_of(g, &a);
        uint32_t texel_ptr =
            op3(g, OP_IMAGE_TEXEL_POINTER, ptr_type(g, SC_IMAGE, tid), img_ptr, v[1], constant(g, GT_UINT, 0));
        uint32_t code = f->code2;
        if ((code == 236 || code == 238) && s->base == SNX_GLSLC_BASE_UINT) code++; /* UMin, UMax */
        uint32_t scope = constant(g, GT_UINT, 1), none = constant(g, GT_UINT, 0), r = id(g);
        if (code == OP_ATOMIC_COMPARE_EXCHANGE) {
            EMIT(g, &g->e.body, code, tid, r, texel_ptr, scope, none, none, v[3], v[2]);
        } else {
            EMIT(g, &g->e.body, code, tid, r, texel_ptr, scope, none, v[2]);
        }
        return r;
    }
    if (fl & GLSLC_TX_SIZE) {
        cap(g, CAP_IMAGE_QUERY);
        uint32_t img = op1(g, OP_IMAGE, image_type(g, t[0], 0), v[0]);
        if (ms || s->dim == GLSLC_DIM_BUFFER) return op1(g, OP_IMAGE_QUERY_SIZE, tid, img);
        return op2(g, OP_IMAGE_QUERY_SIZE_LOD, tid, img, v[1]);
    }
    if (fl & GLSLC_TX_FETCH) {
        uint32_t img = op1(g, OP_IMAGE, image_type(g, t[0], 0), v[0]), r = id(g);
        if (s->dim == GLSLC_DIM_BUFFER) {
            EMIT(g, &g->e.body, OP_IMAGE_FETCH, tid, r, img, v[1]);
        } else if (ms) {
            EMIT(g, &g->e.body, OP_IMAGE_FETCH, tid, r, img, v[1], IMG_SAMPLE, v[2]);
        } else if (fl & GLSLC_TX_OFFSET) {
            uint32_t off = emit_constant(g, nd(g, nd(g, nd(g, x->a)->next)->next)->next, 0);
            if (!off) glslc_fail(g, g->e.site, "glslc writes this offset as no SPIR-V constant: give it literals");
            EMIT(g, &g->e.body, OP_IMAGE_FETCH, tid, r, img, v[1], IMG_LOD | IMG_CONST_OFFSET, v[2], off);
        } else {
            EMIT(g, &g->e.body, OP_IMAGE_FETCH, tid, r, img, v[1], IMG_LOD, v[2]);
        }
        return r;
    }
    /* the arguments after the coordinates, by kind */
    uint32_t i = 2, dref = 0, lod = 0, dx = 0, dy = 0, offset = 0, bias = 0, comp = 0, coord = v[1];
    uint32_t arg = nd(g, nd(g, x->a)->next)->next; /* the node of argument i */
    if (fl & GLSLC_TX_GATHER) {
        if (shadow) {
            dref = v[i++];
            arg = nd(g, arg)->next;
        }
        if (fl & GLSLC_TX_OFFSET) {
            offset = emit_constant(g, arg, 0);
            if (!offset) glslc_fail(g, g->e.site, "glslc writes this offset as no SPIR-V constant: give it literals");
            i++;
            arg = nd(g, arg)->next;
        }
        uint32_t c = 0;
        if ((x->op & 2u) && arg) { const_flat(g, arg, 0, &c); }
        comp = constant(g, GT_INT, c);
        uint32_t r = id(g);
        if (shadow) {
            if (offset) {
                EMIT(g, &g->e.body, OP_IMAGE_DREF_GATHER, tid, r, v[0], coord, dref, IMG_CONST_OFFSET, offset);
            } else {
                EMIT(g, &g->e.body, OP_IMAGE_DREF_GATHER, tid, r, v[0], coord, dref);
            }
        } else if (offset) {
            EMIT(g, &g->e.body, OP_IMAGE_GATHER, tid, r, v[0], coord, comp, IMG_CONST_OFFSET, offset);
        } else {
            EMIT(g, &g->e.body, OP_IMAGE_GATHER, tid, r, v[0], coord, comp);
        }
        return r;
    }
    int proj = (fl & GLSLC_TX_PROJ) != 0;
    uint32_t vec3 = val_type(g, GT_VEC3), ft = val_type(g, GT_FLOAT);
    if (proj) {
        if (shadow) { /* (x, y, z = reference, w = q): the coordinate is (x, y, w) */
            dref = extract(g, ft, coord, 2);
            uint32_t r = id(g);
            EMIT(g, &g->e.body, OP_VECTOR_SHUFFLE, vec3, r, v[1], v[1], 0, 1, 3);
            coord = r;
        } else if (s->dim == GLSLC_DIM_2D && t[1] == GT_VEC4) {
            uint32_t r = id(g);
            EMIT(g, &g->e.body, OP_VECTOR_SHUFFLE, vec3, r, v[1], v[1], 0, 1, 3);
            coord = r;
        }
    } else if (shadow) {
        if (cube && arrayed) {
            dref = v[i++];
            arg = nd(g, arg)->next;
        } else {
            dref = extract(g, ft, coord, rows_of(g, t[1]) - 1);
        }
    }
    if (fl & GLSLC_TX_LOD) {
        lod = v[i++];
        arg = nd(g, arg)->next;
    }
    if (fl & GLSLC_TX_GRAD) {
        dx = v[i++];
        dy = v[i++];
        arg = nd(g, nd(g, arg)->next)->next;
    }
    if (fl & GLSLC_TX_OFFSET) {
        offset = emit_constant(g, arg, 0);
        if (!offset) glslc_fail(g, g->e.site, "glslc writes this offset as no SPIR-V constant: give it literals");
        i++;
        arg = nd(g, arg)->next;
    }
    if (x->op & 1u) bias = v[i++];
    (void)n;
    /* implicit derivatives only in a fragment shader: elsewhere the base level */
    int implicit = !lod && !dx && g->stage == SNX_GLSLC_FRAGMENT;
    if (!lod && !dx && !implicit) lod = constant(g, GT_FLOAT, 0);
    uint32_t mask = (bias ? IMG_BIAS : 0) | (lod ? IMG_LOD : 0) | (dx ? IMG_GRAD : 0) | (offset ? IMG_CONST_OFFSET : 0);
    uint32_t code = implicit
                        ? (proj ? (shadow ? OP_IMAGE_SAMPLE_PROJ_DREF_IMPLICIT_LOD : OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD)
                                : (shadow ? OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD : OP_IMAGE_SAMPLE_IMPLICIT_LOD))
                        : (proj ? (shadow ? OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD : OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD)
                                : (shadow ? OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD : OP_IMAGE_SAMPLE_EXPLICIT_LOD));
    uint32_t r = id(g), at = mark(g);
    push(g, tid);
    push(g, r);
    push(g, v[0]);
    push(g, coord);
    if (shadow) push(g, dref);
    if (mask) {
        push(g, mask);
        if (bias) push(g, bias);
        if (lod) push(g, lod);
        if (dx) {
            push(g, dx);
            push(g, dy);
        }
        if (offset) push(g, offset);
    }
    inst_stack(g, &g->e.body, code, at);
    return r;
}

static uint32_t builtin_value(Glslc* g, uint32_t n) {
    const GlslcNode* x = nd(g, n);
    const GlslcBuiltinFn* f = &glslc_builtin_fns[x->b];
    uint32_t rt = x->type, tid = val_type(g, rt);
    if (f->bk == GLSLC_BK_TEXTURE) return texture_value(g, x, f);
    if (f->bk == GLSLC_BK_SPECIAL) {
        switch (f->code) {
            case GLSLC_BS_BARRIER:
                EMIT(g, &g->e.body, OP_CONTROL_BARRIER, constant(g, GT_UINT, 2), constant(g, GT_UINT, 2),
                     constant(g, GT_UINT, 0x108));
                return 0;
            case GLSLC_BS_MEMORY_BARRIER:
                EMIT(g, &g->e.body, OP_MEMORY_BARRIER, constant(g, GT_UINT, 1), constant(g, GT_UINT, 0x948));
                return 0;
            case GLSLC_BS_MEMORY_BARRIER_SHARED:
                EMIT(g, &g->e.body, OP_MEMORY_BARRIER, constant(g, GT_UINT, 1), constant(g, GT_UINT, 0x108));
                return 0;
            case GLSLC_BS_MEMORY_BARRIER_BUFFER:
                EMIT(g, &g->e.body, OP_MEMORY_BARRIER, constant(g, GT_UINT, 1), constant(g, GT_UINT, 0x48));
                return 0;
            case GLSLC_BS_MEMORY_BARRIER_IMAGE:
                EMIT(g, &g->e.body, OP_MEMORY_BARRIER, constant(g, GT_UINT, 1), constant(g, GT_UINT, 0x808));
                return 0;
            case GLSLC_BS_GROUP_MEMORY_BARRIER:
                EMIT(g, &g->e.body, OP_MEMORY_BARRIER, constant(g, GT_UINT, 2), constant(g, GT_UINT, 0x948));
                return 0;
            case GLSLC_BS_ATOMIC:
            case GLSLC_BS_ATOMIC_COMP_SWAP: {
                Access a;
                need_access(g, x->a, &a);
                uint32_t ptr = pointer_of(g, &a);
                uint32_t b = nd(g, x->a)->next, v1 = rvalue(g, b);
                uint32_t scope = constant(g, GT_UINT, 1), none = constant(g, GT_UINT, 0), r = id(g);
                if (f->code == GLSLC_BS_ATOMIC_COMP_SWAP) {
                    uint32_t v2 = rvalue(g, nd(g, b)->next);
                    EMIT(g, &g->e.body, OP_ATOMIC_COMPARE_EXCHANGE, tid, r, ptr, scope, none, none, v2, v1);
                } else {
                    EMIT(g, &g->e.body, f->code2, tid, r, ptr, scope, none, v1);
                }
                return r;
            }
            default: break;
        }
    }
    /* the in arguments; an out argument is written, after the instruction, through its own access */
    uint32_t v[8], t[8], count = 0;
    const char* sig = f->sig + 2;
    for (uint32_t a = x->a; a && count < 8; a = nd(g, a)->next, count++) {
        int out = sig[0] && sig[1] == '&';
        t[count] = nd(g, a)->type;
        v[count] = out ? 0 : rvalue(g, a);
        if (*sig) sig += out ? 2 : 1;
    }
    switch (f->bk) {
        case GLSLC_BK_EXT_SPLAT:
        case GLSLC_BK_OP_SPLAT:
            for (uint32_t i = 0; i < count; i++) v[i] = splat(g, v[i], t[i], rows_of(g, rt));
            /* fall through */
        case GLSLC_BK_EXT:
        case GLSLC_BK_OP:
            if (f->bk == GLSLC_BK_EXT || f->bk == GLSLC_BK_EXT_SPLAT) return ext_inst(g, rt, f->code, v, count);
            if (count == 1) return op1(g, f->code, tid, v[0]);
            if (count == 2) return op2(g, f->code, tid, v[0], v[1]);
            if (count == 3) return op3(g, f->code, tid, v[0], v[1], v[2]);
            {
                uint32_t r = id(g);
                EMIT(g, &g->e.body, f->code, tid, r, v[0], v[1], v[2], v[3]);
                return r;
            }
        default: break;
    }
    switch (f->code) {
        case GLSLC_BS_MIX_BOOL: return op3(g, OP_SELECT, tid, v[2], v[1], v[0]);
        case GLSLC_BS_MATRIX_COMP_MULT: return matrix_columns(g, OP_F_MUL, rt, t[0], v[0], t[1], v[1]);
        case GLSLC_BS_OUTER_PRODUCT: return op2(g, OP_OUTER_PRODUCT, tid, v[0], v[1]);
        case GLSLC_BS_TRANSPOSE: return op1(g, OP_TRANSPOSE, tid, v[0]);
        case GLSLC_BS_BITCAST: return op1(g, OP_BITCAST, tid, v[0]);
        case GLSLC_BS_MODF:
        case GLSLC_BS_FREXP: {
            uint32_t second = f->code == GLSLC_BS_MODF ? rt : glslc_vector_type(SNX_GLSLC_BASE_INT, rows_of(g, rt));
            uint32_t pt = pair_type(g, rt, second), r = id(g);
            EMIT(g, &g->e.body, OP_EXT_INST, pt, r, g->e.glsl_ext,
                 f->code == GLSLC_BS_MODF ? X_MODF_STRUCT : X_FREXP_STRUCT, v[0]);
            store_out(g, nd(g, x->a)->next, extract(g, val_type(g, second), r, 1));
            return extract(g, tid, r, 0);
        }
        case GLSLC_BS_CARRY: {
            uint32_t pt = pair_type(g, rt, rt), r = op2(g, f->code2, pt, v[0], v[1]);
            store_out(g, nd(g, nd(g, x->a)->next)->next, extract(g, tid, r, 1));
            return extract(g, tid, r, 0);
        }
        case GLSLC_BS_EXTENDED: {
            uint32_t pt = pair_type(g, t[0], t[0]), r = op2(g, f->code2, pt, v[0], v[1]);
            uint32_t msb = nd(g, nd(g, x->a)->next)->next, lsb = nd(g, msb)->next;
            uint32_t et = val_type(g, t[0]);
            store_out(g, lsb, extract(g, et, r, 0));
            store_out(g, msb, extract(g, et, r, 1));
            return 0;
        }
        default: glslc_fail(g, g->e.site, "glslc writes no SPIR-V for %s", f->name); return 0;
    }
}

/* expressions */

static uint32_t call_value(Glslc* g, uint32_t n) {
    const GlslcNode* x = nd(g, n);
    const GlslcSymbol* f = &g->syms[x->b];
    Access outs[16];
    uint32_t temps[16], out_args[16], nouts = 0, i = 0, nvals = 0;
    uint32_t vals[64];
    for (uint32_t a = x->a; a; a = nd(g, a)->next, i++) {
        const GlslcParam* p = &g->params[f->aux + i];
        uint32_t v;
        if (holds_samplers(g, p->type)) { /* as sampler_parameters takes it */
            const GlslcType* st = ty(g, element_of(g, p->type));
            uint32_t plain = spirv_members(g, p->type);
            if (nvals + (plain ? 1u : 0u) + st->len - plain > 64)
                glslc_fail(g, g->e.site,
                           "a call passes more than the 64 SPIR-V arguments glslc takes: a struct's samplers are one "
                           "each");
            if (plain) vals[nvals++] = rvalue(g, a);
            for (uint32_t m = 0; m < st->len; m++) {
                if (!is_sampler_member(g, g->members[st->first + m].type)) continue;
                Access acc;
                sampler_access(g, a, m, &acc); /* the struct's, or the array's of them, as the parameter is */
                vals[nvals++] = pointer_of(g, &acc);
            }
            continue;
        }
        if (p->storage == GLSLC_ST_PARAM_IN) {
            uint32_t e = p->type;
            while (ty(g, e)->kind == GLSLC_T_ARRAY) e = ty(g, e)->elem;
            if (ty(g, e)->kind == GLSLC_T_SAMPLER || ty(g, e)->kind == GLSLC_T_IMAGE) {
                Access acc;
                need_access(g, a, &acc);
                v = pointer_of(g, &acc);
            } else {
                v = rvalue(g, a);
            }
        } else {
            if (nouts == 16)
                glslc_fail(g, g->e.site, "a call has more than the 16 out and inout arguments glslc takes");
            need_access(g, a, &outs[nouts]);
            temps[nouts] = local_var(g, p->type);
            out_args[nouts] = a;
            if (p->storage == GLSLC_ST_PARAM_INOUT) EMIT(g, &g->e.body, OP_STORE, temps[nouts], load(g, &outs[nouts]));
            v = temps[nouts++];
        }
        if (nvals == 64) glslc_fail(g, g->e.site, "a call has more than the 64 arguments glslc takes");
        vals[nvals++] = v;
    }
    uint32_t r = id(g), at = mark(g);
    push(g, val_type(g, f->type));
    push(g, r);
    push(g, g->e.sym_ids[x->b]);
    for (uint32_t k = 0; k < nvals; k++) push(g, vals[k]);
    inst_stack(g, &g->e.body, OP_FUNCTION_CALL, at);
    for (uint32_t k = 0; k < nouts; k++) {
        uint32_t pt = g->params[f->aux].type;
        uint32_t idx = 0;
        for (uint32_t a = x->a; a != out_args[k]; a = nd(g, a)->next) idx++;
        pt = g->params[f->aux + idx].type;
        store(g, &outs[k], op1(g, OP_LOAD, val_type(g, pt), temps[k]));
    }
    return r;
}

static uint32_t logic_value(Glslc* g, const GlslcNode* x) {
    uint32_t l = rvalue(g, x->a);
    const GlslcNode* rn = nd(g, x->b);
    uint32_t bt = val_type(g, GT_BOOL);
    if (rn->kind == GLSLC_N_CONST || rn->kind == GLSLC_N_VAR)
        return op2(g, x->op == GLSLC_OP_LOGIC_AND ? OP_LOGICAL_AND : OP_LOGICAL_OR, bt, l, rvalue(g, x->b));
    uint32_t from = g->e.cur_block, rhs = id(g), merge = id(g);
    EMIT(g, &g->e.body, OP_SELECTION_MERGE, merge, 0);
    if (x->op == GLSLC_OP_LOGIC_AND) {
        EMIT(g, &g->e.body, OP_BRANCH_CONDITIONAL, l, rhs, merge);
    } else {
        EMIT(g, &g->e.body, OP_BRANCH_CONDITIONAL, l, merge, rhs);
    }
    label(g, rhs);
    uint32_t r = rvalue(g, x->b), end = g->e.cur_block;
    branch(g, merge);
    label(g, merge);
    uint32_t p = id(g);
    EMIT(g, &g->e.body, OP_PHI, bt, p, l, from, r, end);
    return p;
}

static uint32_t rvalue(Glslc* g, uint32_t n) {
    const GlslcNode* x = nd(g, n);
    uint32_t t = x->type;
    g->e.site = g->toks[x->tok].site;
    switch (x->kind) {
        case GLSLC_N_CONST: return constant(g, t, x->a | ((uint32_t)x->b << 16));
        case GLSLC_N_VAR:
        case GLSLC_N_INDEX:
        case GLSLC_N_FIELD:
        case GLSLC_N_SWIZZLE: {
            uint32_t c = x->kind == GLSLC_N_VAR ? emit_constant(g, n, 0) : 0;
            if (c) return c;
            /* a struct of samplers alone, which is no SPIR-V value: what reads it, a statement or the left of a
               comma, drops it, and the rest read its members (parser.c) */
            if (holds_samplers(g, t) && !spirv_members(g, t)) return 0;
            Access a;
            if (access(g, n, &a)) return load(g, &a);
            uint32_t base = rvalue(g, x->a);
            uint32_t bt = nd(g, x->a)->type;
            if (x->kind == GLSLC_N_FIELD) return extract(g, val_type(g, t), base, spirv_member(g, bt, x->b));
            if (x->kind == GLSLC_N_SWIZZLE) {
                if (x->op == 1) return extract(g, val_type(g, t), base, x->b & 3u);
                uint32_t r = id(g), at = mark(g);
                push(g, val_type(g, t));
                push(g, r);
                push(g, base);
                push(g, base);
                for (uint32_t i = 0; i < x->op; i++) push(g, (x->b >> (2 * i)) & 3u);
                inst_stack(g, &g->e.body, OP_VECTOR_SHUFFLE, at);
                return r;
            }
            if (nd(g, x->b)->kind == GLSLC_N_CONST) return extract(g, val_type(g, t), base, nd(g, x->b)->a);
            if (ty(g, bt)->kind == GLSLC_T_VECTOR)
                return op2(g, OP_VECTOR_EXTRACT_DYNAMIC, val_type(g, t), base, rvalue(g, x->b));
            /* an array or matrix value indexed at run time: through a variable */
            uint32_t tmp = local_var(g, bt);
            EMIT(g, &g->e.body, OP_STORE, tmp, base);
            uint32_t ptr = op2(g, OP_ACCESS_CHAIN, ptr_type(g, SC_FUNCTION, val_type(g, t)), tmp, rvalue(g, x->b));
            return op1(g, OP_LOAD, val_type(g, t), ptr);
        }
        case GLSLC_N_UNARY: {
            uint32_t at = nd(g, x->a)->type;
            switch (x->op) {
                case GLSLC_OP_PLUS: return rvalue(g, x->a);
                case GLSLC_OP_NEG: {
                    uint32_t v = rvalue(g, x->a);
                    int fl = ty(g, t)->base == SNX_GLSLC_BASE_FLOAT;
                    if (ty(g, t)->kind == GLSLC_T_MATRIX) return matrix_columns(g, OP_F_NEGATE, t, t, v, t, 0);
                    return op1(g, fl ? OP_F_NEGATE : OP_S_NEGATE, val_type(g, t), v);
                }
                case GLSLC_OP_NOT: return op1(g, OP_LOGICAL_NOT, val_type(g, t), rvalue(g, x->a));
                case GLSLC_OP_BITNOT: return op1(g, OP_NOT, val_type(g, t), rvalue(g, x->a));
                default: {
                    Access a;
                    need_access(g, x->a, &a);
                    uint32_t old = load(g, &a);
                    int inc = x->op == GLSLC_OP_PRE_INC || x->op == GLSLC_OP_POST_INC;
                    uint32_t nv = binary_value(g, inc ? GLSLC_OP_ADD : GLSLC_OP_SUB, at, at, old, at, ones(g, at));
                    store(g, &a, nv);
                    return x->op == GLSLC_OP_PRE_INC || x->op == GLSLC_OP_PRE_DEC ? nv : old;
                }
            }
        }
        case GLSLC_N_BINARY:
            if (x->op == GLSLC_OP_LOGIC_AND || x->op == GLSLC_OP_LOGIC_OR) return logic_value(g, x);
            {
                uint32_t l = rvalue(g, x->a), r = rvalue(g, x->b);
                return binary_value(g, x->op, t, nd(g, x->a)->type, l, nd(g, x->b)->type, r);
            }
        case GLSLC_N_ASSIGN: {
            Access a;
            need_access(g, x->a, &a);
            uint32_t v;
            if (x->op == GLSLC_OP_ASSIGN) {
                v = rvalue(g, x->b);
            } else {
                uint32_t old = load(g, &a), r = rvalue(g, x->b);
                v = binary_value(g, x->op, t, t, old, nd(g, x->b)->type, r);
            }
            store(g, &a, v);
            return v;
        }
        case GLSLC_N_TERNARY: {
            uint32_t c = rvalue(g, x->a), a = id(g), b = id(g), merge = id(g);
            EMIT(g, &g->e.body, OP_SELECTION_MERGE, merge, 0);
            EMIT(g, &g->e.body, OP_BRANCH_CONDITIONAL, c, a, b);
            label(g, a);
            uint32_t va = rvalue(g, x->b), ea = g->e.cur_block;
            branch(g, merge);
            label(g, b);
            uint32_t vb = rvalue(g, x->c), eb = g->e.cur_block;
            branch(g, merge);
            label(g, merge);
            uint32_t r = id(g);
            EMIT(g, &g->e.body, OP_PHI, val_type(g, t), r, va, ea, vb, eb);
            return r;
        }
        case GLSLC_N_CALL: return call_value(g, n);
        case GLSLC_N_BUILTIN: return builtin_value(g, n);
        case GLSLC_N_CONSTRUCT: return construct(g, n);
        case GLSLC_N_LENGTH: {
            /* array.length() of a buffer block's last member */
            const GlslcNode* arr = nd(g, x->a);
            uint32_t block_ptr, member;
            if (arr->kind == GLSLC_N_FIELD) {
                Access a;
                need_access(g, arr->a, &a);
                block_ptr = pointer_of(g, &a);
                member = arr->b;
            } else if (arr->kind == GLSLC_N_VAR && g->syms[arr->a].kind == GLSLC_SK_BLOCK_MEMBER) {
                block_ptr = g->e.sym_ids[g->syms[arr->a].aux];
                member = g->syms[arr->a].aux2;
            } else {
                glslc_fail(g, g->e.site, "length() of an array with no size is for a buffer block's last member");
                return 0;
            }
            uint32_t len = op2(g, OP_ARRAY_LENGTH, val_type(g, GT_UINT), block_ptr, member);
            return op1(g, OP_BITCAST, val_type(g, GT_INT), len);
        }
        case GLSLC_N_COMMA: rvalue(g, x->a); return rvalue(g, x->b);
        default: glslc_fail(g, g->e.site, "glslc writes no SPIR-V for this expression"); return 0;
    }
}

/* statements */

static void relaxed(Glslc* g, uint32_t target, uint32_t t, uint8_t prec) {
    uint32_t e = element_of(g, t);
    uint8_t k = ty(g, e)->kind;
    if ((prec != GLSLC_PREC_LOW && prec != GLSLC_PREC_MEDIUM) ||
        !(k == GLSLC_T_SCALAR || k == GLSLC_T_VECTOR || k == GLSLC_T_MATRIX) || ty(g, e)->base == SNX_GLSLC_BASE_BOOL)
        return;
    EMIT(g, &g->e.annotations, OP_DECORATE, target, DEC_RELAXED_PRECISION);
}

static void local_declaration(Glslc* g, uint32_t n) {
    const GlslcNode* x = nd(g, n);
    uint32_t s = x->a;
    const GlslcSymbol* y = &g->syms[s];
    if (y->storage == GLSLC_ST_CONST && x->b && nd(g, x->b)->kind == GLSLC_N_CONST) return; /* every use was folded */
    uint32_t var = local_var(g, y->type);
    g->e.sym_ids[s] = var;
    name(g, var, y->name);
    relaxed(g, var, y->type, y->prec);
    if (x->b) EMIT(g, &g->e.body, OP_STORE, var, rvalue(g, x->b));
}

/* a loop's condition: an expression, or a declared bool */
static uint32_t condition(Glslc* g, uint32_t n) {
    if (nd(g, n)->kind != GLSLC_N_DECL) return rvalue(g, n);
    local_declaration(g, n);
    return op1(g, OP_LOAD, val_type(g, GT_BOOL), g->e.sym_ids[nd(g, n)->a]);
}

static void enter_construct(Glslc* g, uint32_t brk, uint32_t cont) {
    if (g->e.nest >= SNX_GLSLC_MAX_DEPTH)
        glslc_fail(g, g->e.site, "loops and switches nest more than the %u deep glslc takes", SNX_GLSLC_MAX_DEPTH);
    g->e.break_ids[g->e.nest] = brk;
    g->e.continue_ids[g->e.nest] = cont;
    g->e.nest++;
}

static void loop_head(Glslc* g, uint32_t header, uint32_t merge, uint32_t cont, uint32_t next) {
    branch(g, header);
    label(g, header);
    EMIT(g, &g->e.body, OP_LOOP_MERGE, merge, cont, 0);
    EMIT(g, &g->e.body, OP_BRANCH, next);
    g->e.cur_block = 0;
}

static void switch_statement(Glslc* g, const GlslcNode* x) {
    uint32_t sel = rvalue(g, x->a), merge = id(g), at = mark(g), dflt = merge, block = 0, prev_case = 0;
    for (uint32_t s = x->b; s; s = nd(g, s)->next) {
        if (nd(g, s)->kind != GLSLC_N_CASE) {
            prev_case = 0;
            continue;
        }
        if (!prev_case) block = id(g);
        prev_case = 1;
        push(g, s);
        push(g, block);
        if (!nd(g, s)->a) dflt = block;
    }
    uint32_t end = mark(g);
    EMIT(g, &g->e.body, OP_SELECTION_MERGE, merge, 0);
    push(g, sel);
    push(g, dflt);
    for (uint32_t k = at; k < end; k += 2) {
        uint32_t value = nd(g, g->e.stack[k])->a;
        if (!value) continue;
        push(g, nd(g, value)->a | ((uint32_t)nd(g, value)->b << 16));
        push(g, g->e.stack[k + 1]);
    }
    inst_stack(g, &g->e.body, OP_SWITCH, end);
    g->e.cur_block = 0;
    enter_construct(g, merge, 0);
    uint32_t k = at, labelled = 0;
    for (uint32_t s = x->b; s; s = nd(g, s)->next) {
        if (nd(g, s)->kind == GLSLC_N_CASE) {
            uint32_t b = g->e.stack[k + 1];
            k += 2;
            if (b != labelled) {
                branch(g, b); /* falling through from the case before */
                label(g, b);
                labelled = b;
            }
            continue;
        }
        statement(g, s);
    }
    branch(g, merge);
    g->e.nest--;
    g->e.nstack = at;
    label(g, merge);
}

static void statement(Glslc* g, uint32_t n) {
    if (!n || !g->e.cur_block) return; /* nothing reaches code after a return, break, continue or discard */
    const GlslcNode* x = nd(g, n);
    g->e.site = g->toks[x->tok].site;
    switch (x->kind) {
        case GLSLC_N_BLOCK:
            for (uint32_t s = x->a; s; s = nd(g, s)->next) statement(g, s);
            break;
        case GLSLC_N_EXPR: rvalue(g, x->a); break;
        case GLSLC_N_DECL: local_declaration(g, n); break;
        case GLSLC_N_IF: {
            uint32_t c = rvalue(g, x->a), then = id(g), merge = id(g), other = x->c ? id(g) : merge;
            EMIT(g, &g->e.body, OP_SELECTION_MERGE, merge, 0);
            EMIT(g, &g->e.body, OP_BRANCH_CONDITIONAL, c, then, other);
            label(g, then);
            statement(g, x->b);
            branch(g, merge);
            if (x->c) {
                label(g, other);
                statement(g, x->c);
                branch(g, merge);
            }
            label(g, merge);
            break;
        }
        case GLSLC_N_WHILE: {
            uint32_t header = id(g), cond = id(g), body = id(g), cont = id(g), merge = id(g);
            loop_head(g, header, merge, cont, cond);
            label(g, cond);
            uint32_t c = condition(g, x->a);
            EMIT(g, &g->e.body, OP_BRANCH_CONDITIONAL, c, body, merge);
            label(g, body);
            enter_construct(g, merge, cont);
            statement(g, x->b);
            g->e.nest--;
            branch(g, cont);
            label(g, cont);
            branch(g, header);
            label(g, merge);
            break;
        }
        case GLSLC_N_DO: {
            uint32_t header = id(g), body = id(g), cont = id(g), merge = id(g);
            loop_head(g, header, merge, cont, body);
            label(g, body);
            enter_construct(g, merge, cont);
            statement(g, x->a);
            g->e.nest--;
            branch(g, cont);
            label(g, cont);
            uint32_t c = rvalue(g, x->b);
            EMIT(g, &g->e.body, OP_BRANCH_CONDITIONAL, c, header, merge);
            label(g, merge);
            break;
        }
        case GLSLC_N_FOR: {
            const GlslcNode* rest = nd(g, x->c);
            statement(g, x->a);
            uint32_t header = id(g), cond = id(g), body = id(g), cont = id(g), merge = id(g);
            loop_head(g, header, merge, cont, cond);
            label(g, cond);
            if (x->b) {
                uint32_t c = condition(g, x->b);
                EMIT(g, &g->e.body, OP_BRANCH_CONDITIONAL, c, body, merge);
            } else {
                EMIT(g, &g->e.body, OP_BRANCH, body);
            }
            label(g, body);
            enter_construct(g, merge, cont);
            statement(g, rest->b);
            g->e.nest--;
            branch(g, cont);
            label(g, cont);
            if (rest->a) rvalue(g, rest->a);
            branch(g, header);
            label(g, merge);
            break;
        }
        case GLSLC_N_SWITCH: switch_statement(g, x); break;
        case GLSLC_N_BREAK: branch(g, g->e.break_ids[g->e.nest - 1]); break;
        case GLSLC_N_CONTINUE:
            for (uint32_t k = g->e.nest; k-- > 0;) {
                if (g->e.continue_ids[k]) {
                    branch(g, g->e.continue_ids[k]);
                    break;
                }
            }
            break;
        case GLSLC_N_DISCARD:
            inst(g, &g->e.body, OP_KILL, NULL, 0);
            g->e.cur_block = 0;
            break;
        case GLSLC_N_RETURN:
            if (x->a) {
                uint32_t v = rvalue(g, x->a);
                EMIT(g, &g->e.body, OP_RETURN_VALUE, v);
            } else {
                inst(g, &g->e.body, OP_RETURN, NULL, 0);
            }
            g->e.cur_block = 0;
            break;
        default: glslc_fail(g, g->e.site, "glslc writes no SPIR-V for this statement");
    }
}

/* functions */

static int is_opaque_type(const Glslc* g, uint32_t t) {
    uint8_t k = ty(g, element_of(g, t))->kind;
    return k == GLSLC_T_SAMPLER || k == GLSLC_T_IMAGE;
}

static uint32_t param_type(Glslc* g, const GlslcParam* p) {
    if (ty(g, element_of(g, p->type))->kind == GLSLC_T_IMAGE)
        glslc_fail(g, g->e.site, "glslc passes no image to a function: use the image's uniform in it");
    if (is_opaque_type(g, p->type)) return ptr_type(g, SC_UNIFORM_CONSTANT, val_type(g, p->type));
    if (p->storage == GLSLC_ST_PARAM_IN) return val_type(g, p->type);
    return ptr_type(g, SC_FUNCTION, val_type(g, p->type));
}

static uint32_t function_type_of(Glslc* g, uint32_t s) {
    const GlslcSymbol* f = &g->syms[s];
    uint32_t at = mark(g);
    push(g, val_type(g, f->type));
    for (uint32_t i = 0; i < f->aux2; i++) {
        const GlslcParam* p = &g->params[f->aux + i];
        if (!holds_samplers(g, p->type)) {
            push(g, param_type(g, p));
            continue;
        }
        if (spirv_members(g, p->type)) push(g, val_type(g, p->type)); /* as sampler_parameters gives them */
        const GlslcType* st = ty(g, element_of(g, p->type));
        for (uint32_t m = 0; m < st->len; m++) {
            uint32_t mt = g->members[st->first + m].type;
            if (is_sampler_member(g, mt)) push(g, ptr_type(g, SC_UNIFORM_CONSTANT, sampler_type(g, p->type, mt)));
        }
    }
    return function_type(g, at);
}

/* a parameter of a struct that holds samplers: the struct's other members as a value, then a pointer for each
   sampler, at the ids after the first (sampler_variables) */
static void sampler_parameters(Glslc* g, const GlslcParam* p) {
    if (spirv_members(g, p->type)) {
        uint32_t pid = id(g);
        EMIT(g, &g->e.functions, OP_FUNCTION_PARAMETER, val_type(g, p->type), pid);
        if (p->sym) { /* an in parameter is the function's to change */
            uint32_t var = local_var(g, p->type);
            EMIT(g, &g->e.body, OP_STORE, var, pid);
            g->e.sym_ids[p->sym] = var;
            name(g, var, p->name);
        }
    }
    const GlslcType* st = ty(g, element_of(g, p->type));
    uint32_t first = g->e.next_id;
    g->e.next_id += st->len - spirv_members(g, p->type);
    if (p->sym) g->e.sampler_ids[p->sym] = first;
    for (uint32_t m = 0, v = first; m < st->len; m++) {
        uint32_t mt = g->members[st->first + m].type;
        if (!is_sampler_member(g, mt)) continue;
        uint32_t pt = ptr_type(g, SC_UNIFORM_CONSTANT, sampler_type(g, p->type, mt));
        EMIT(g, &g->e.functions, OP_FUNCTION_PARAMETER, pt, v);
        v++;
    }
}

static void emit_function(Glslc* g, uint32_t n) {
    const GlslcNode* x = nd(g, n);
    uint32_t s = x->a;
    const GlslcSymbol* f = &g->syms[s];
    g->e.site = g->toks[x->tok].site;
    uint32_t ret = val_type(g, f->type), ft = function_type_of(g, s), fid = g->e.sym_ids[s];
    g->e.body.n = 0;
    g->e.locals.n = 0;
    EMIT(g, &g->e.functions, OP_FUNCTION, ret, fid, 0, ft);
    name(g, fid, f->name);
    for (uint32_t i = 0; i < f->aux2; i++) {
        const GlslcParam* p = &g->params[f->aux + i];
        if (holds_samplers(g, p->type)) {
            sampler_parameters(g, p);
            continue;
        }
        uint32_t pid = id(g);
        EMIT(g, &g->e.functions, OP_FUNCTION_PARAMETER, param_type(g, p), pid);
        if (!p->sym) continue;
        if (p->storage == GLSLC_ST_PARAM_IN && !is_opaque_type(g, p->type)) {
            uint32_t var = local_var(g, p->type); /* an in parameter is the function's to change */
            EMIT(g, &g->e.body, OP_STORE, var, pid);
            g->e.sym_ids[p->sym] = var;
            name(g, var, p->name);
            relaxed(g, var, p->type, p->prec);
        } else {
            g->e.sym_ids[p->sym] = pid;
            name(g, pid, p->name);
        }
    }
    uint32_t first = id(g);
    g->e.cur_block = first;
    if (fid == g->e.main_id) {
        for (uint32_t d = g->first_decl; d; d = nd(g, d)->next) {
            const GlslcNode* gd = nd(g, d);
            if (gd->kind == GLSLC_N_GLOBAL && (g->syms[gd->a].qual & GLSLC_Q_INIT))
                EMIT(g, &g->e.body, OP_STORE, g->e.sym_ids[gd->a], rvalue(g, gd->b));
        }
    }
    statement(g, x->b);
    if (g->e.cur_block) {
        if (f->type == GT_VOID) {
            inst(g, &g->e.body, OP_RETURN, NULL, 0);
        } else { /* GLSL leaves the value undefined when the end is reached */
            uint32_t u = id(g);
            EMIT(g, &g->e.body, OP_UNDEF, ret, u);
            EMIT(g, &g->e.body, OP_RETURN_VALUE, u);
        }
    }
    EMIT(g, &g->e.functions, OP_LABEL, first);
    for (uint32_t i = 0; i < g->e.locals.n; i++) word(g, &g->e.functions, g->e.locals.w[i]);
    for (uint32_t i = 0; i < g->e.body.n; i++) word(g, &g->e.functions, g->e.body.w[i]);
    inst(g, &g->e.functions, OP_FUNCTION_END, NULL, 0);
}

/* global variables */

static uint32_t location_count(const Glslc* g, uint32_t t) {
    const GlslcType* y = ty(g, t);
    if (y->kind == GLSLC_T_ARRAY) return y->len * location_count(g, y->elem);
    if (y->kind == GLSLC_T_MATRIX) return y->cols;
    return 1;
}

static void mark_location(Glslc* g, uint8_t* used, int32_t loc, uint32_t count, GlslcView name) {
    if (loc < 0 || (uint32_t)loc + count > GLSLC_MAX_LOCATION)
        glslc_fail(g, name.pos, "%.*s takes locations %d to %d: glslc takes 0 to %u", (int)name.len,
                   glslc_view_text(g, name), (int)loc, (int)(loc + (int32_t)count - 1), GLSLC_MAX_LOCATION - 1);
    for (uint32_t i = 0; i < count; i++) {
        if (used[loc + i])
            glslc_fail(g, name.pos, "location %d of %.*s is another variable's too", (int)(loc + (int32_t)i),
                       (int)name.len, glslc_view_text(g, name));
        used[loc + i] = 1;
    }
}

static int32_t free_location(Glslc* g, uint8_t* used, uint32_t count, GlslcView name) {
    for (uint32_t l = 0; l + count <= GLSLC_MAX_LOCATION; l++) {
        uint32_t i;
        for (i = 0; i < count && !used[l + i]; i++) {}
        if (i == count) {
            for (i = 0; i < count; i++) used[l + i] = 1;
            return (int32_t)l;
        }
    }
    glslc_fail(g, name.pos, "%.*s needs %u locations, and fewer than that are left", (int)name.len,
               glslc_view_text(g, name), count);
    return 0;
}

static int binding_taken(const Glslc* g, uint32_t set, uint32_t binding) {
    for (uint32_t i = 0; i < g->e.nresources; i++) {
        if (g->e.resources[i] == ((set << 16) | binding)) return 1;
    }
    return 0;
}

static void take_binding(Glslc* g, uint32_t set, uint32_t binding, GlslcView name) {
    if (g->e.nresources >= GLSLC_MAX_RESOURCES)
        glslc_fail(g, name.pos, "the shader has more than the %u samplers, images and blocks glslc takes",
                   GLSLC_MAX_RESOURCES);
    g->e.resources[g->e.nresources++] = (set << 16) | binding;
}

static uint32_t free_binding(Glslc* g, GlslcView name) {
    uint32_t b = 0;
    while (binding_taken(g, 0, b)) b++;
    take_binding(g, 0, b, name);
    return b;
}

static int is_resource(const Glslc* g, uint32_t s) {
    const GlslcSymbol* y = &g->syms[s];
    uint8_t k = ty(g, element_of(g, y->type))->kind;
    return (y->storage == GLSLC_ST_UNIFORM || y->storage == GLSLC_ST_BUFFER) &&
           (k == GLSLC_T_SAMPLER || k == GLSLC_T_IMAGE || k == GLSLC_T_BLOCK);
}

static void reflect(Glslc* g, uint8_t kind, GlslcView name, uint32_t t, uint32_t location, uint32_t set,
                    uint32_t offset) {
    GlslcResult* r = g->result;
    if (r->reflect_count >= SNX_GLSLC_MAX_REFLECT)
        glslc_fail(g, name.pos, "the shader's interface has more than the %u entries glslc describes",
                   SNX_GLSLC_MAX_REFLECT);
    GlslcReflect* e = &r->reflect[r->reflect_count++];
    memset(e, 0, sizeof *e);
    e->name = name;
    e->kind = kind;
    uint32_t el = element_of(g, t);
    const GlslcType* y = ty(g, el);
    e->array = ty(g, t)->kind == GLSLC_T_ARRAY ? ty(g, t)->len : 0;
    e->base = y->kind == GLSLC_T_SAMPLER || y->kind == GLSLC_T_IMAGE ? SNX_GLSLC_BASE_OPAQUE : y->base;
    e->rows = y->kind == GLSLC_T_VECTOR || y->kind == GLSLC_T_MATRIX ? y->rows : 1;
    e->cols = y->kind == GLSLC_T_MATRIX ? y->cols : 1;
    e->location = (uint16_t)location;
    e->set = (uint16_t)set;
    e->offset = offset;
}

static uint32_t variable(Glslc* g, uint32_t s, uint32_t storage, uint32_t pointee, uint32_t init) {
    uint32_t v = id(g);
    if (init) {
        EMIT(g, &g->e.globals, OP_VARIABLE, ptr_type(g, storage, pointee), v, storage, init);
    } else {
        EMIT(g, &g->e.globals, OP_VARIABLE, ptr_type(g, storage, pointee), v, storage);
    }
    g->e.sym_ids[s] = v;
    name(g, v, g->syms[s].name);
    if (storage == SC_INPUT || storage == SC_OUTPUT) {
        if (g->e.interface_count >= sizeof g->e.interface / sizeof g->e.interface[0])
            glslc_fail(g, g->syms[s].name.pos, "the shader has more than the %u inputs and outputs glslc takes",
                       (unsigned)(sizeof g->e.interface / sizeof g->e.interface[0]));
        g->e.interface[g->e.interface_count++] = v;
    }
    return v;
}

static void interface_variable(Glslc* g, uint32_t s) {
    GlslcSymbol* y = &g->syms[s];
    int in = y->storage == GLSLC_ST_IN;
    uint32_t v = variable(g, s, in ? SC_INPUT : SC_OUTPUT, val_type(g, y->type), 0);
    uint8_t* used = in ? g->e.in_locations : g->e.out_locations;
    int32_t loc = y->location >= 0 ? y->location : free_location(g, used, location_count(g, y->type), y->name);
    EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_LOCATION, (uint32_t)loc);
    int integer = ty(g, element_of(g, y->type))->base != SNX_GLSLC_BASE_FLOAT;
    if ((y->qual & GLSLC_Q_FLAT) || (integer && in && g->stage == SNX_GLSLC_FRAGMENT))
        EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_FLAT);
    if (y->qual & GLSLC_Q_CENTROID) EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_CENTROID);
    if (y->qual & GLSLC_Q_SAMPLE) {
        cap(g, CAP_SAMPLE_RATE_SHADING);
        EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_SAMPLE);
    }
    if (!in && (y->qual & GLSLC_Q_INVARIANT)) EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_INVARIANT);
    relaxed(g, v, y->type, y->prec);
    reflect(g, in ? SNX_GLSLC_REFLECT_INPUT : SNX_GLSLC_REFLECT_OUTPUT, y->name, y->type, (uint32_t)loc, 0, 0);
}

/* where the backend's map puts resource r: its answer, checked */
static void mapped_binding(Glslc* g, const GlslcResource* r, uint32_t* set, uint32_t* binding) {
    *set = r->set >= 0 ? (uint32_t)r->set : 0;
    *binding = r->binding >= 0 ? (uint32_t)r->binding : 0;
    int answer = g->bind(g->bind_user, r, set, binding);
    if (answer)
        glslc_fail(g, r->name.pos, "the binding map refuses %.*s (it returns %d)", (int)r->name.len,
                   glslc_view_text(g, r->name), answer);
    if (*set > 0xffffu || *binding > 0xffffu)
        glslc_fail(g, r->name.pos, "the binding map puts %.*s at set %u binding %u: glslc takes 0 to 65535 for each",
                   (int)r->name.len, glslc_view_text(g, r->name), *set, *binding);
    if (binding_taken(g, *set, *binding))
        glslc_fail(g, r->name.pos, "the binding map puts %.*s at set %u binding %u, which another resource has",
                   (int)r->name.len, glslc_view_text(g, r->name), *set, *binding);
    take_binding(g, *set, *binding, r->name);
}

/* the samplers of uniform s, a struct or an array of structs: a variable each, at the ids after the first, in the
   order the struct declares them, placed as the shader's other samplers are */
static void sampler_variables(Glslc* g, uint32_t s) {
    const GlslcSymbol* y = &g->syms[s];
    const GlslcType* st = ty(g, element_of(g, y->type));
    uint32_t holder_len = ty(g, y->type)->kind == GLSLC_T_ARRAY ? ty(g, y->type)->len : 0;
    uint32_t first = g->e.next_id;
    g->e.next_id += st->len - spirv_members(g, y->type);
    g->e.sampler_ids[s] = first;
    for (uint32_t m = 0, v = first; m < st->len; m++) {
        const GlslcMember* mb = &g->members[st->first + m];
        if (!is_sampler_member(g, mb->type)) continue;
        uint32_t pointee = sampler_type(g, y->type, mb->type), set, binding;
        EMIT(g, &g->e.globals, OP_VARIABLE, ptr_type(g, SC_UNIFORM_CONSTANT, pointee), v, SC_UNIFORM_CONSTANT);
        char text[256]; /* a debug name, cut at 255 bytes */
        int len = snprintf(text, sizeof text, "%.*s.%.*s", (int)y->name.len, glslc_view_text(g, y->name),
                           (int)mb->name.len, glslc_view_text(g, mb->name));
        inst_string(g, &g->e.names, OP_NAME, &v, 1, text, len < (int)sizeof text ? (uint32_t)len : sizeof text - 1,
                    NULL, 0);
        uint32_t array = holder_len ? holder_len : ty(g, mb->type)->kind == GLSLC_T_ARRAY ? ty(g, mb->type)->len : 0;
        if (g->bind) {
            GlslcResource r = {0};
            r.name = y->name;
            r.member = mb->name;
            r.kind = SNX_GLSLC_REFLECT_SAMPLER;
            r.texel_buffer = ty(g, element_of(g, mb->type))->dim == GLSLC_DIM_BUFFER;
            r.array = (uint16_t)array;
            r.set = r.binding = -1;
            mapped_binding(g, &r, &set, &binding);
        } else {
            set = 0;
            binding = free_binding(g, y->name);
        }
        EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_DESCRIPTOR_SET, set);
        EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_BINDING, binding);
        reflect(g, SNX_GLSLC_REFLECT_SAMPLER, y->name, mb->type, binding, set, 0);
        GlslcReflect* e = &g->result->reflect[g->result->reflect_count - 1];
        e->member = mb->name;
        e->array = (uint16_t)array;
        v++;
    }
}

/* an in or out block: the block's location, or the members' own, and each member after the one before it */
static void io_block_variable(Glslc* g, uint32_t s) {
    GlslcSymbol* y = &g->syms[s];
    int array = ty(g, y->type)->kind == GLSLC_T_ARRAY;
    uint32_t elements = array ? ty(g, y->type)->len : 1;
    const GlslcType* bt = ty(g, array ? ty(g, y->type)->elem : y->type);
    int in = y->storage == GLSLC_ST_IN;
    uint32_t st = type_id(g, array ? ty(g, y->type)->elem : y->type, 0);
    uint32_t v = variable(g, s, in ? SC_INPUT : SC_OUTPUT, type_id(g, y->type, 0), 0);
    EMIT(g, &g->e.annotations, OP_DECORATE, st, DEC_BLOCK);
    uint8_t* used = in ? g->e.in_locations : g->e.out_locations;
    uint32_t total = 0, given = 0;
    for (uint32_t m = 0; m < bt->len; m++) {
        total += location_count(g, g->members[bt->first + m].type);
        given += g->members[bt->first + m].location >= 0;
    }
    int32_t at = y->location;
    int whole = at < 0 && !given; /* the first free run of the block's size; given ones are taken already */
    if (whole) at = free_location(g, used, total * elements, bt->name);
    if (at >= 0) EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_LOCATION, (uint32_t)at);
    for (uint32_t i = 0; i < bt->len * elements; i++) { /* an array's elements one after the other */
        uint32_t m = i % bt->len;
        const GlslcMember* mb = &g->members[bt->first + m];
        uint32_t count = location_count(g, mb->type);
        if (mb->location >= 0 && i < bt->len) { /* no array of blocks has member locations (parser.c) */
            at = mb->location;
            EMIT(g, &g->e.annotations, OP_MEMBER_DECORATE, st, m, DEC_LOCATION, (uint32_t)at);
        }
        if (i >= bt->len) { /* the decorations are the struct's, for every element */
            reflect(g, in ? SNX_GLSLC_REFLECT_INPUT : SNX_GLSLC_REFLECT_OUTPUT, mb->name, mb->type, (uint32_t)at, 0, 0);
            at += (int32_t)count;
            continue;
        }
        int integer = ty(g, element_of(g, mb->type))->base != SNX_GLSLC_BASE_FLOAT;
        if ((mb->mflags & GLSLC_MEM_FLAT) || (integer && in && g->stage == SNX_GLSLC_FRAGMENT))
            EMIT(g, &g->e.annotations, OP_MEMBER_DECORATE, st, m, DEC_FLAT);
        if (mb->mflags & GLSLC_MEM_CENTROID) EMIT(g, &g->e.annotations, OP_MEMBER_DECORATE, st, m, DEC_CENTROID);
        if (mb->mflags & GLSLC_MEM_SAMPLE) {
            cap(g, CAP_SAMPLE_RATE_SHADING);
            EMIT(g, &g->e.annotations, OP_MEMBER_DECORATE, st, m, DEC_SAMPLE);
        }
        reflect(g, in ? SNX_GLSLC_REFLECT_INPUT : SNX_GLSLC_REFLECT_OUTPUT, mb->name, mb->type, (uint32_t)at, 0, 0);
        at += (int32_t)count;
    }
}

static void resource_variable(Glslc* g, uint32_t s) {
    GlslcSymbol* y = &g->syms[s];
    uint32_t e = element_of(g, y->type), set, binding;
    const GlslcType* et = ty(g, e);
    uint8_t kind = et->kind == GLSLC_T_SAMPLER     ? SNX_GLSLC_REFLECT_SAMPLER
                   : et->kind == GLSLC_T_IMAGE     ? SNX_GLSLC_REFLECT_IMAGE
                   : y->storage == GLSLC_ST_BUFFER ? SNX_GLSLC_REFLECT_BUFFER_BLOCK
                                                   : SNX_GLSLC_REFLECT_UNIFORM_BLOCK;
    uint32_t size = 0, align;
    if (et->kind == GLSLC_T_BLOCK) lay(g, e, block_layout(y->packing), 0, &align, &size);
    if (g->bind) {
        GlslcResource r = {0};
        r.name = et->kind == GLSLC_T_BLOCK ? et->name : y->name;
        r.kind = kind;
        r.texel_buffer = et->kind != GLSLC_T_BLOCK && et->dim == GLSLC_DIM_BUFFER;
        r.array = ty(g, y->type)->kind == GLSLC_T_ARRAY ? ty(g, y->type)->len : 0;
        r.set = y->set;
        r.binding = y->binding;
        r.size = size;
        mapped_binding(g, &r, &set, &binding);
    } else {
        set = y->set >= 0 ? (uint32_t)y->set : 0;
        binding = y->binding >= 0 ? (uint32_t)y->binding : free_binding(g, y->name);
    }
    uint32_t pointee, v;
    if (et->kind == GLSLC_T_IMAGE) {
        pointee = image_type(g, e, y->packing);
        if (ty(g, y->type)->kind == GLSLC_T_ARRAY) { /* of one dimension (parser.c) */
            uint32_t a = id(g), len = constant(g, GT_UINT, ty(g, y->type)->len);
            EMIT(g, &g->e.globals, OP_TYPE_ARRAY, a, pointee, len);
            pointee = a;
        }
    } else {
        pointee = val_type(g, y->type);
    }
    v = variable(g, s, storage_of(g, s), pointee, 0);
    if (et->kind == GLSLC_T_BLOCK) EMIT(g, &g->e.annotations, OP_DECORATE, type_id(g, e, 0), DEC_BLOCK);
    EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_DESCRIPTOR_SET, set);
    EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_BINDING, binding);
    if (y->qual & GLSLC_Q_READONLY) EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_NON_WRITABLE);
    if (y->qual & GLSLC_Q_WRITEONLY) EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_NON_READABLE);
    if (y->qual & GLSLC_Q_COHERENT) EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_COHERENT);
    if (y->qual & GLSLC_Q_VOLATILE) EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_VOLATILE);
    if (y->qual & GLSLC_Q_RESTRICT) EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_RESTRICT);
    reflect(g, kind, et->kind == GLSLC_T_BLOCK ? et->name : y->name, y->type, binding, set, size);
}

static void builtin_variables(Glslc* g) {
    for (uint32_t r = 0; r < glslc_builtin_var_count && r < GLSLC_MAX_BUILTIN_VARS; r++) {
        uint32_t s = g->builtin_syms[r];
        const GlslcBuiltinVar* b = &glslc_builtin_vars[r];
        if (!s || b->value == GLSLC_BUILTIN_VALUE_LOCAL_SIZE) continue;
        GlslcSymbol* y = &g->syms[s];
        int in = b->storage == GLSLC_ST_IN;
        uint32_t v = variable(g, s, in ? SC_INPUT : SC_OUTPUT, val_type(g, y->type), 0);
        if (b->builtin != GLSLC_NOT_BUILTIN) {
            EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_BUILTIN, b->builtin);
        } else { /* gl_FragColor, gl_FragData: the first outputs */
            mark_location(g, g->e.out_locations, 0, location_count(g, y->type), y->name);
            EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_LOCATION, 0);
            reflect(g, SNX_GLSLC_REFLECT_OUTPUT, y->name, y->type, 0, 0, 0);
        }
        if (in && g->stage == SNX_GLSLC_FRAGMENT && ty(g, element_of(g, y->type))->base == SNX_GLSLC_BASE_INT)
            EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_FLAT);
        if (b->builtin == 18 || b->builtin == 19) cap(g, CAP_SAMPLE_RATE_SHADING); /* SampleId, SamplePosition */
        if (b->builtin == 22) g->e.depth_replacing = 1;                            /* FragDepth */
        if (y->qual & GLSLC_Q_INVARIANT) EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_INVARIANT);
    }
}

static void default_uniform_block(Glslc* g) {
    uint32_t count = g->e.default_members;
    if (!count) return;
    GlslcView none = {0, 0};
    uint32_t at = mark(g), r = id(g);
    push(g, r);
    for (uint32_t s = 1; s < g->nsyms; s++) {
        if (g->syms[s].storage == GLSLC_ST_UNIFORM && g->e.sym_types[s] == 0xffffffffu)
            push(g, type_id(g, g->syms[s].type, GLSLC_LAYOUT_STD140));
    }
    inst_stack(g, &g->e.globals, OP_TYPE_STRUCT, at);
    static const char block_name[] = "gl_DefaultUniformBlock";
    inst_string(g, &g->e.names, OP_NAME, &r, 1, block_name, (uint32_t)sizeof block_name - 1, NULL, 0);
    uint32_t offset = 0, max = 16;
    for (uint32_t s = 1; s < g->nsyms; s++) {
        const GlslcSymbol* y = &g->syms[s];
        if (y->storage != GLSLC_ST_UNIFORM || g->e.sym_types[s] != 0xffffffffu) continue;
        uint32_t m = g->e.sym_ids[s], a, size;
        lay(g, y->type, GLSLC_LAYOUT_STD140, 0, &a, &size);
        offset = round_up(offset, a);
        if (a > max) max = a;
        EMIT(g, &g->e.annotations, OP_MEMBER_DECORATE, r, m, DEC_OFFSET, offset);
        uint32_t e = element_of(g, y->type);
        if (ty(g, e)->kind == GLSLC_T_MATRIX) {
            EMIT(g, &g->e.annotations, OP_MEMBER_DECORATE, r, m, DEC_COL_MAJOR);
            EMIT(g, &g->e.annotations, OP_MEMBER_DECORATE, r, m, DEC_MATRIX_STRIDE,
                 matrix_stride(g, e, GLSLC_LAYOUT_STD140, 0));
        }
        uint32_t pre[2] = {r, m};
        inst_string(g, &g->e.names, OP_MEMBER_NAME, pre, 2, glslc_view_text(g, y->name), y->name.len, NULL, 0);
        if (y->prec == GLSLC_PREC_LOW || y->prec == GLSLC_PREC_MEDIUM) {
            uint32_t k = ty(g, e)->kind;
            if ((k == GLSLC_T_SCALAR || k == GLSLC_T_VECTOR || k == GLSLC_T_MATRIX) &&
                ty(g, e)->base != SNX_GLSLC_BASE_BOOL)
                EMIT(g, &g->e.annotations, OP_MEMBER_DECORATE, r, m, DEC_RELAXED_PRECISION);
        }
        reflect(g, SNX_GLSLC_REFLECT_UNIFORM, y->name, y->type, 0, 0, offset);
        offset += size;
    }
    EMIT(g, &g->e.annotations, OP_DECORATE, r, DEC_BLOCK);
    uint32_t v = id(g);
    EMIT(g, &g->e.globals, OP_VARIABLE, ptr_type(g, SC_UNIFORM, r), v, SC_UNIFORM);
    inst_string(g, &g->e.names, OP_NAME, &v, 1, "", 0, NULL, 0);
    uint32_t binding = free_binding(g, none);
    EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_DESCRIPTOR_SET, 0);
    EMIT(g, &g->e.annotations, OP_DECORATE, v, DEC_BINDING, binding);
    g->e.default_block = v;
    g->e.default_type = r;
    g->e.default_size = round_up(offset, max);
    g->e.default_binding = (uint16_t)binding;
    GlslcResult* res = g->result;
    if (res->reflect_count >= SNX_GLSLC_MAX_REFLECT)
        glslc_fail(g, 0, "the shader's interface has more than the %u entries glslc describes", SNX_GLSLC_MAX_REFLECT);
    GlslcReflect* e = &res->reflect[res->reflect_count++];
    memset(e, 0, sizeof *e);
    e->kind = SNX_GLSLC_REFLECT_UNIFORM_BLOCK;
    e->base = SNX_GLSLC_BASE_STRUCT;
    e->rows = e->cols = 1;
    e->location = (uint16_t)binding;
    e->offset = g->e.default_size;
}

static void global_variables(Glslc* g) {
    /* the locations and bindings given, so that the others go around them */
    for (uint32_t d = g->first_decl; d; d = nd(g, d)->next) {
        if (nd(g, d)->kind != GLSLC_N_GLOBAL) continue;
        const GlslcSymbol* y = &g->syms[nd(g, d)->a];
        int io = y->storage == GLSLC_ST_IN || y->storage == GLSLC_ST_OUT;
        uint8_t* used = y->storage == GLSLC_ST_IN ? g->e.in_locations : g->e.out_locations;
        uint32_t block = io && ty(g, y->type)->kind == GLSLC_T_ARRAY ? ty(g, y->type)->elem : y->type;
        if (io && ty(g, block)->kind == GLSLC_T_BLOCK) { /* each member after the one before, from the given */
            const GlslcType* bt = ty(g, block);
            uint32_t elements = block != y->type ? ty(g, y->type)->len : 1;
            int32_t at = y->location;
            for (uint32_t i = 0; i < bt->len * elements; i++) {
                const GlslcMember* mb = &g->members[bt->first + i % bt->len];
                if (mb->location >= 0) at = mb->location;
                if (at < 0) break; /* none given: the block takes a free run later */
                mark_location(g, used, at, location_count(g, mb->type), mb->name);
                at += (int32_t)location_count(g, mb->type);
            }
        } else if (io && y->location >= 0) {
            mark_location(g, used, y->location, location_count(g, y->type), y->name);
        }
        if (is_resource(g, nd(g, d)->a) && y->binding >= 0 && !g->bind) { /* a map places every one itself */
            uint32_t set = y->set >= 0 ? (uint32_t)y->set : 0;
            if (binding_taken(g, set, (uint32_t)y->binding))
                glslc_fail(g, y->name.pos, "set %u binding %d is another resource's too", set, (int)y->binding);
            take_binding(g, set, (uint32_t)y->binding, y->name);
        }
    }
    builtin_variables(g);
    for (uint32_t d = g->first_decl; d; d = nd(g, d)->next) {
        const GlslcNode* x = nd(g, d);
        if (x->kind != GLSLC_N_GLOBAL) continue;
        uint32_t s = x->a;
        GlslcSymbol* y = &g->syms[s];
        g->e.site = y->name.pos;
        switch (y->storage) {
            case GLSLC_ST_IN:
            case GLSLC_ST_OUT:
                if (ty(g, element_of(g, y->type))->kind == GLSLC_T_BLOCK) {
                    io_block_variable(g, s);
                } else {
                    interface_variable(g, s);
                }
                break;
            case GLSLC_ST_UNIFORM:
            case GLSLC_ST_BUFFER:
                if (is_resource(g, s)) {
                    resource_variable(g, s);
                    break;
                }
                if (holds_samplers(g, y->type)) sampler_variables(g, s);
                if (!holds_samplers(g, y->type) || spirv_members(g, y->type)) {
                    /* a loose uniform, or a struct's members that are no samplers: the default uniform block's */
                    g->e.sym_types[s] = 0xffffffffu;
                    g->e.sym_ids[s] = g->e.default_members++;
                }
                break;
            case GLSLC_ST_SHARED: {
                uint32_t v = variable(g, s, SC_WORKGROUP, val_type(g, y->type), 0);
                relaxed(g, v, y->type, y->prec);
                break;
            }
            default: {
                if (y->storage == GLSLC_ST_CONST && x->b && nd(g, x->b)->kind == GLSLC_N_CONST) break; /* folded */
                uint32_t init = x->b ? emit_constant(g, x->b, 0) : 0;
                if (x->b && !init) y->qual |= GLSLC_Q_INIT;
                uint32_t v = variable(g, s, SC_PRIVATE, val_type(g, y->type), init);
                relaxed(g, v, y->type, y->prec);
            }
        }
    }
    default_uniform_block(g);
}

static void fail_size(Glslc* g, const char* message) {
    snprintf(g->result->error, sizeof g->result->error, "%s", message);
    g->result->line = g->result->column = 0;
    longjmp(g->fail, 1);
}

void glslc_emit(Glslc* g, uint32_t* words, size_t capacity) {
    GlslcEmitter* e = &g->e;
    e->next_id = 1;
    e->site = 0;
    cap(g, CAP_SHADER);
    e->glsl_ext = id(g);
    static const char glsl[] = "GLSL.std.450";
    inst_string(g, &e->imports, OP_EXT_INST_IMPORT, &e->glsl_ext, 1, glsl, (uint32_t)sizeof glsl - 1, NULL, 0);
    EMIT(g, &e->names, 3, 1, g->version); /* OpSource ESSL version */
    global_variables(g);
    for (uint32_t d = g->first_decl; d; d = nd(g, d)->next) {
        if (nd(g, d)->kind != GLSLC_N_FUNCTION) continue;
        uint32_t s = nd(g, d)->a;
        e->sym_ids[s] = id(g);
        if (glslc_view_is(g, g->syms[s].name, "main")) e->main_id = e->sym_ids[s];
    }
    for (uint32_t d = g->first_decl; d; d = nd(g, d)->next) {
        if (nd(g, d)->kind == GLSLC_N_FUNCTION) emit_function(g, d);
    }
    static const uint32_t models[] = {0, 4, 5}; /* Vertex, Fragment, GLCompute */
    static const char main_name[] = "main";
    uint32_t pre[2] = {models[g->stage], e->main_id};
    inst_string(g, &e->entry, OP_ENTRY_POINT, pre, 2, main_name, 4, e->interface, e->interface_count);
    if (g->stage == SNX_GLSLC_FRAGMENT) {
        EMIT(g, &e->modes, OP_EXECUTION_MODE, e->main_id, 7); /* OriginUpperLeft, which Vulkan takes only */
        if (g->early_fragment_tests) EMIT(g, &e->modes, OP_EXECUTION_MODE, e->main_id, 9);
        if (e->depth_replacing) EMIT(g, &e->modes, OP_EXECUTION_MODE, e->main_id, 12);
    } else if (g->stage == SNX_GLSLC_COMPUTE) {
        EMIT(g, &e->modes, OP_EXECUTION_MODE, e->main_id, 17, g->local_size[0], g->local_size[1], g->local_size[2]);
    }
    const GlslcWords* sections[] = {&e->caps,  &e->imports,     &e->entry,   &e->modes,
                                    &e->names, &e->annotations, &e->globals, &e->functions};
    uint64_t total = 5 + 3;
    for (uint32_t i = 0; i < sizeof sections / sizeof sections[0]; i++) total += sections[i]->n;
    g->result->words = (uint32_t)total;
    char message[160];
    if (total > SNX_GLSLC_MAX_WORDS) {
        snprintf(message, sizeof message, "the SPIR-V module has %llu words, more than the %u glslc writes",
                 (unsigned long long)total, SNX_GLSLC_MAX_WORDS);
        fail_size(g, message);
    }
    if (!words || total > capacity) {
        snprintf(message, sizeof message, "the SPIR-V module has %llu words, and the buffer holds %llu",
                 (unsigned long long)total, (unsigned long long)(words ? capacity : 0));
        fail_size(g, message);
    }
    uint32_t w = 0;
    words[w++] = 0x07230203u; /* magic */
    words[w++] = 0x00010300u; /* SPIR-V 1.3: Vulkan 1.1's, with the StorageBuffer storage class */
    words[w++] = 0;           /* no registered generator */
    words[w++] = e->next_id;
    words[w++] = 0;
    for (uint32_t i = 0; i < sizeof sections / sizeof sections[0]; i++) {
        memcpy(&words[w], sections[i]->w, sections[i]->n * sizeof(uint32_t));
        w += sections[i]->n;
        if (sections[i] == &e->imports) { /* OpMemoryModel Logical GLSL450, after the imports */
            words[w++] = (3u << 16) | OP_MEMORY_MODEL;
            words[w++] = 0;
            words[w++] = 1;
        }
    }
}
