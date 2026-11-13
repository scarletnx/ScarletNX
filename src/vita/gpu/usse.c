/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Vita3K's shader recompiler (GPL-2.0-or-later, Copyright (C) 2026 Vita3K team) at Vita3K/Vita3K f68b6ee:
   shader/src/usse_translator_entry.cpp (the instructions' bit patterns, which usse.decode holds),
   usse_decode_helpers.cpp (operand banks, register numbers, swizzle tables, write masks), usse_utilities.cpp (operand
   loads and stores, packing),
   translator/data.cpp (VMOV, VPCK, VLDST, LIMM), translator/alu.cpp (VMAD, VMAD2, VDP, VNMAD, VCOMP, SOP2, SOP2M),
   translator/ialu.cpp (VBW, I16MAD, I32MAD, I32MAD2), translator/branch_cond.cpp (VTST, VTSTMSK),
   translator/special.cpp (KILL, DEPTHF, SMBO), translator/texture.cpp (texture reads, SMP), usse_program_analyzer.cpp
   (predicates, branches), spirv_recompiler.cpp and gxp_parser.cpp (the register banks, the program's inputs, uniform
   buffers and the buffers read from memory, literals, dependent samplers, varyings and outputs) and gxm/src/gxp.cpp
   (the vertex outputs).

   Where this differs from Vita3K beyond the registers: branches are a loop over the program's basic blocks with a
   switch on the next one (Vita3K matches its programs' if, else and loop shapes); VBW and the tests rotate left, which
   Vita3K refuses; SOP2M writes its masked components where they are, not packed.

   The registers differ from Vita3K's in how they are held: each bank is an array of 32-bit words, one a register,
   read and written a component at a time (Vita3K keeps vec4 floats), so values here are up to four scalars and every
   instruction is emitted componentwise. What is read and written, from which register, in which type, is Vita3K's. */
#include "vita/gpu/usse.h"
#include "vita/gpu/gxp.h"
#include "common/utilities/spirv_emit.h"
#include "common/utilities/common.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USSE_NAN     NAN
#define USSE_NEG_NAN (-NAN)
#include "vita/gpu/usse_constants.inc"
#include "common/utilities/bits.h"

/* ---- the operand model (usse_types.h) ---- */
typedef enum {
    B_TEMP, B_PA, B_OUT, B_SA, B_INTERNAL, B_SPECIAL, B_GLOBAL, B_CONST, B_IMM, B_INDEX, B_INDEXED1, B_INDEXED2,
    B_PRED, B_INVALID
} Bank;
typedef enum { T_I8, T_I16, T_I32, T_C10, T_F16, T_F32, T_U8, T_U16, T_U32, T_O8, T_UNK } Type;
enum { SW_X, SW_Y, SW_Z, SW_W, SW_0, SW_1, SW_2, SW_H, SW_U };
enum { F_ABS = 1, F_NEG = 2, F_GPI = 4 };
typedef struct {
    unsigned num;
    Bank bank;
    unsigned flags;
    uint8_t swizzle[4];
    Type type;
} Operand;
/* A value: up to four scalars, all of one kind */
enum { K_FLOAT, K_UINT, K_INT };
typedef struct {
    uint32_t c[4];
    unsigned n;
    int kind;
} Value;

static unsigned type_size(Type t) {
    switch (t) {
        case T_I8: case T_U8: case T_O8: case T_C10: return 1;
        case T_I16: case T_U16: case T_F16: return 2;
        default: return 4;
    }
}
static bool is_float_type(Type t) { return t == T_C10 || t == T_F16 || t == T_F32; }
static bool is_signed_type(Type t) { return t == T_I8 || t == T_I16 || t == T_I32; }
static bool is_unsigned_type(Type t) { return t == T_U8 || t == T_U16 || t == T_U32; }
/* components a 32-bit register holds */
static unsigned packed_count(Type t) { return t == T_C10 ? 3 : 4 / type_size(t); }

/* ---- the translator ---- */
#define BANK_WORDS 256u
#define GLOBALS    96u
typedef struct {
    SpvEmitter e;
    uint32_t t_void, t_bool, t_u32, t_i32, t_f32, t_v2f, t_v3f, t_v4f, t_v4u, t_v4i, glsl;
    uint32_t bank[B_INVALID];
    uint32_t globals[GLOBALS];
    unsigned global_count;
    const unsigned char* gxp;
    size_t size;
    bool fragment, secondary;
    uint64_t instr; /* the instruction being translated */
    unsigned pc;
    int repeat_increase[4][17], repeat_multiplier[4];
    const UsseHints* hints;
    UsseShader* out;
    char* error;
    size_t error_size;
    bool failed;
    uint32_t push; /* the push constant block (vertex) */
    uint32_t samplers[USSE_TEXTURES];
    uint8_t sampler_dimensions[USSE_TEXTURES];
    uint32_t buffer;    /* the uniform buffers' storage buffer, 0 for none */
    uint32_t terminated; /* the instruction just translated ended its block (KILL) */
    uint32_t frag_depth; /* gl_FragDepth, for a program that writes depth (DEPTHF) */
    uint32_t frag_coord; /* gl_FragCoord, made at its first use */
    /* the SA registers holding a dependent sampler: its texture's index plus 1, 0 for none */
    uint8_t dependent[BANK_WORDS];
    /* the literal buffer's base register (an SA offset, -1 for none) and the bytes it reads, the program's */
    int literal_register;
    uint32_t literal_base;
    uint32_t literals, literal_words; /* the literal buffer as a Private array, for reads at a register's offset */
    /* the decode of the instruction: its pattern's name, whether it is translated, and the end of its predicate's if
       (0 for none). A survey (usse_survey) only names the instructions */
    const char* name;
    bool known, survey;
    uint32_t merge;
} T;

/* the decoder that tools/decodetree makes from usse.decode: decode_usse(t, word) calls the trans_ function of the
   word's pattern with its fields (arg_<pattern>), and answers false for a word that matches no pattern */
typedef T DisasContext;
#include "common/utilities/decodetree.h"
/* the generated extractors take a context that they leave unused, as QEMU's do */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "decode-usse.inc"
#pragma GCC diagnostic pop

static bool fail(T* t, const char* format, ...) {
    if (!t->failed) {
        va_list list;
        va_start(list, format);
        vsnprintf(t->error, t->error_size, format, list);
        va_end(list);
    }
    t->failed = true;
    return false;
}
static void global(T* t, uint32_t id) {
    if (t->global_count < GLOBALS) t->globals[t->global_count++] = id;
}
/* a module-scope variable; a register bank (Private) starts as zeros, so a read of a register the program didn't
   write is the same on every device */
static uint32_t var(T* t, uint32_t storage, uint32_t type) {
    uint32_t id = storage == SPV_STORAGE_PRIVATE ? spv_emit_variable_zeroed(&t->e, storage, type)
                                                 : spv_emit_variable(&t->e, storage, type);
    global(t, id);
    return id;
}
static uint32_t cf(T* t, float f) { return spv_emit_const_f32(&t->e, f); }
static uint32_t cu(T* t, uint32_t u) { return spv_emit_const_u32(&t->e, u); }
static uint32_t ci(T* t, int32_t i) { return spv_emit_const_i32(&t->e, i); }
static uint32_t kind_type(T* t, int kind) { return kind == K_FLOAT ? t->t_f32 : kind == K_UINT ? t->t_u32 : t->t_i32; }
static uint32_t op1(T* t, uint32_t opcode, uint32_t type, uint32_t a) { return SPV(&t->e, opcode, type, a); }
static uint32_t op2(T* t, uint32_t opcode, uint32_t type, uint32_t a, uint32_t b) { return SPV(&t->e, opcode, type, a, b); }
static uint32_t ext1(T* t, uint32_t type, uint32_t inst, uint32_t a) { return SPV_EXT(&t->e, type, inst, a); }
static uint32_t ext2(T* t, uint32_t type, uint32_t inst, uint32_t a, uint32_t b) { return SPV_EXT(&t->e, type, inst, a, b); }
static uint32_t ext3(T* t, uint32_t type, uint32_t inst, uint32_t a, uint32_t b, uint32_t c) {
    return SPV_EXT(&t->e, type, inst, a, b, c);
}
static uint32_t bitcast(T* t, uint32_t type, uint32_t v) { return op1(t, SPV_OP_BITCAST, type, v); }

/* a word of a bank, by a constant register number */
static uint32_t word_pointer(T* t, Bank bank, unsigned index) {
    uint32_t pointer = spv_emit_type_pointer(&t->e, SPV_STORAGE_PRIVATE, t->t_u32);
    return SPV(&t->e, SPV_OP_ACCESS_CHAIN, pointer, t->bank[bank], cu(t, index));
}
static uint32_t load_word(T* t, Bank bank, unsigned index) {
    if (index >= BANK_WORDS) return fail(t, "register %u past the bank", index), cu(t, 0);
    return SPV(&t->e, SPV_OP_LOAD, t->t_u32, word_pointer(t, bank, index));
}
static void store_word(T* t, Bank bank, unsigned index, uint32_t value) {
    if (index >= BANK_WORDS) {
        fail(t, "register %u past the bank", index);
        return;
    }
    SPV_VOID(&t->e, SPV_OP_STORE, word_pointer(t, bank, index), value);
}

/* ---- conversions (usse_utilities.cpp: convert_to_float, convert_to_int) ---- */
static float normalize_range(Type type) {
    switch (type) {
        case T_U8: return 255.0f;
        case T_I8: return 127.0f;
        case T_C10: return 255.0f;
        case T_U16: return 65535.0f;
        case T_I16: return 32767.0f;
        case T_U32: return 4294967295.0f;
        default: return 2147483647.0f;
    }
}
static Value to_float(T* t, Value v, Type type, bool normal) {
    Value r = {{0}, v.n, K_FLOAT};
    for (unsigned i = 0; i < v.n; ++i) {
        uint32_t x = op1(t, v.kind == K_INT ? SPV_OP_CONVERT_S_TO_F : SPV_OP_CONVERT_U_TO_F, t->t_f32, v.c[i]);
        if (normal) {
            x = op2(t, SPV_OP_F_DIV, t->t_f32, x, cf(t, normalize_range(type)));
            if (v.kind == K_INT) x = ext2(t, t->t_f32, SPV_GLSL_F_MAX, x, cf(t, type == T_C10 ? -2.0f : -1.0f));
        }
        r.c[i] = x;
    }
    return r;
}
static Value to_int(T* t, Value v, Type type, bool normal) {
    bool is_uint = is_unsigned_type(type);
    Value r = {{0}, v.n, is_uint ? K_UINT : K_INT};
    for (unsigned i = 0; i < v.n; ++i) {
        uint32_t x = v.c[i];
        if (normal) {
            x = ext3(t, t->t_f32, SPV_GLSL_F_CLAMP, x, cf(t, is_uint ? 0.0f : type == T_C10 ? -2.0f : -1.0f),
                     cf(t, type == T_C10 ? 2.0f : 1.0f));
            x = op2(t, SPV_OP_F_MUL, t->t_f32, x, cf(t, normalize_range(type)));
            x = ext1(t, t->t_f32, SPV_GLSL_ROUND, x);
        }
        r.c[i] = op1(t, is_uint ? SPV_OP_CONVERT_F_TO_U : SPV_OP_CONVERT_F_TO_S, is_uint ? t->t_u32 : t->t_i32, x);
    }
    return r;
}

/* ---- operand decoding (usse_decode_helpers.cpp) ---- */
static void check_internal(Operand* r, bool doubled, unsigned reg_bits) {
    unsigned temps = doubled ? 8 : 4, limit = (1u << reg_bits) - temps;
    if (r->bank == B_TEMP && r->num >= limit) {
        r->num -= limit;
        if (doubled) r->num >>= 1;
        r->bank = B_INTERNAL;
    }
}
static void finalize_register(T* t, Operand* r, bool doubled, unsigned reg_bits) {
    check_internal(r, doubled, reg_bits);
    if (r->bank == B_SPECIAL) {
        if (r->num & 0x40) {
            r->num &= ~0x40u;
            r->bank = B_GLOBAL;
        } else
            r->bank = B_CONST;
    }
    if (r->bank != B_INTERNAL && r->bank != B_CONST && r->bank != B_IMM && t->secondary) r->bank = B_SA;
}
static void double_reg(Operand* r) {
    if (r->bank != B_SPECIAL && r->bank != B_IMM) r->num = (r->num << 1) & 0xff;
}
static const uint8_t default_swizzle[4] = {SW_X, SW_Y, SW_Z, SW_W};
static Operand decode_dest(T* t, unsigned n, unsigned bank, bool ext, bool doubled, unsigned reg_bits) {
    static const Bank normal[4] = {B_TEMP, B_OUT, B_PA, B_INDEXED1}, extended[4] = {B_SA, B_SPECIAL, B_INDEX, B_INDEXED2};
    Operand r = {n, ext ? extended[bank & 3] : normal[bank & 3], 0, {SW_X, SW_Y, SW_Z, SW_W}, T_F32};
    if (doubled) double_reg(&r);
    finalize_register(t, &r, doubled, reg_bits);
    return r;
}
static Operand decode_src0(T* t, unsigned n, unsigned bank, bool ext, bool doubled, unsigned reg_bits) {
    static const Bank normal[2] = {B_TEMP, B_PA}, extended[2] = {B_OUT, B_SA};
    Operand r = {n, ext ? extended[bank & 1] : normal[bank & 1], 0, {SW_X, SW_Y, SW_Z, SW_W}, T_F32};
    if (doubled) double_reg(&r);
    finalize_register(t, &r, doubled, reg_bits);
    return r;
}
static Operand decode_src12(T* t, unsigned n, unsigned bank, bool ext, bool doubled, unsigned reg_bits) {
    static const Bank normal[4] = {B_TEMP, B_OUT, B_PA, B_SA}, extended[4] = {B_INDEXED1, B_SPECIAL, B_IMM, B_INDEXED2};
    Operand r = {n, ext ? extended[bank & 3] : normal[bank & 3], 0, {SW_X, SW_Y, SW_Z, SW_W}, T_F32};
    if (doubled) double_reg(&r);
    finalize_register(t, &r, doubled, reg_bits);
    return r;
}
static unsigned decode_write_mask(Bank dest, unsigned mask, bool f16) {
    bool f16_normal = dest != B_PA && dest != B_SA && dest != B_OUT && dest != B_INTERNAL && dest != B_TEMP;
    if (dest == B_INTERNAL || (f16 && f16_normal)) return mask;
    unsigned r = 0;
    if (f16) {
        if (mask & 1) r |= 3;
        if (mask & 4) r |= 12;
    } else
        r = mask & 3;
    return r;
}
static unsigned decode_modifier(unsigned mod) {
    return mod == 1 ? F_NEG : mod == 2 ? F_ABS : mod == 3 ? F_NEG | F_ABS : 0;
}
static void swizzle4(uint8_t out[4], unsigned encoded) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)((encoded >> (i * 3)) & 7);
}
#define S4(a, b, c, d) {SW_##a, SW_##b, SW_##c, SW_##d}
#define S3(a, b, c) {SW_##a, SW_##b, SW_##c, SW_X}
#define SU {SW_U, SW_U, SW_U, SW_U}
/* decode_vec34_swizzle: type 0 scalar, 1 vec3, 2 vec4 */
static void vec34_swizzle(uint8_t out[4], unsigned swizz, bool extended, int type) {
    static const uint8_t v4_std[16][4] = {S4(X, X, X, X), S4(Y, Y, Y, Y), S4(Z, Z, Z, Z), S4(W, W, W, W), S4(X, Y, Z, W),
                                          S4(Y, Z, W, W), S4(X, Y, Z, Z), S4(X, X, Y, Z), S4(X, Y, X, Y), S4(X, Y, W, Z),
                                          S4(Z, X, Y, W), S4(Z, W, Z, W), S4(Y, Z, X, Z), S4(X, X, Y, Y), S4(X, Z, W, W),
                                          S4(X, Y, Z, 1)};
    static const uint8_t v4_ext[16][4] = {S4(Y, Z, X, W), S4(Z, W, X, Y), S4(X, Z, W, Y), S4(Y, Y, W, W), S4(W, Y, Z, W),
                                          S4(W, Z, W, Z), S4(X, Y, Z, X), S4(Z, Z, W, W), S4(X, W, Z, X), S4(Y, Y, Y, X),
                                          S4(Y, Y, Y, Z), S4(X, Z, Y, W), S4(X, X, X, Y), S4(Z, Y, X, W), S4(Y, Y, Z, Z),
                                          S4(Z, Z, Z, Y)};
    static const uint8_t v3_std[16][4] = {S3(X, X, X), S3(Y, Y, Y), S3(Z, Z, Z), S3(W, W, W), S3(X, Y, Z), S3(Y, Z, W),
                                          S3(X, X, Y), S3(X, Y, X), S3(Y, Y, X), S3(Y, Y, Z), S3(Z, X, Y), S3(X, Z, Y),
                                          S3(Y, Z, X), S3(Z, Y, X), S3(Z, Z, Y), S3(X, Y, 1)};
    static const uint8_t v3_ext[16][4] = {S3(X, Y, Y), S3(Y, X, Y), S3(X, X, Z), S3(Y, X, X), S3(X, Y, 0), S3(X, 1, 0),
                                          S3(0, 0, 0), S3(1, 1, 1), S3(H, H, H), S3(2, 2, 2), S3(X, 0, 0), SU, SU, SU,
                                          SU, SU};
    static const uint8_t scalar[16][4] = {S4(X, X, X, X), S4(Y, Y, Y, Y), S4(Z, Z, Z, Z), S4(W, W, W, W), S4(0, 0, 0, 0),
                                          S4(1, 1, 1, 1), S4(2, 2, 2, 2), S4(H, H, H, H), SU, SU, SU, SU, SU, SU, SU, SU};
    const uint8_t* s = type == 0 ? (extended ? (const uint8_t[4])SU : scalar[swizz & 15])
                       : type == 1 ? (extended ? v3_ext[swizz & 15] : v3_std[swizz & 15])
                                   : (extended ? v4_ext[swizz & 15] : v4_std[swizz & 15]);
    memcpy(out, s, 4);
}

/* ---- loads and stores (usse_utilities.cpp: load, store, unpack, pack) ---- */
static uint32_t swizzle_constant(T* t, unsigned channel, int kind) {
    float f = channel == SW_0 ? 0.0f : channel == SW_1 ? 1.0f : channel == SW_2 ? 2.0f : 0.5f;
    return kind == K_FLOAT ? cf(t, f) : kind == K_UINT ? cu(t, (uint32_t)f) : ci(t, (int32_t)f);
}
static Value apply_modifiers(T* t, Value v, unsigned flags) {
    for (unsigned i = 0; i < v.n; ++i) {
        if ((flags & F_ABS) && v.kind == K_FLOAT) v.c[i] = ext1(t, t->t_f32, SPV_GLSL_F_ABS, v.c[i]);
        if ((flags & F_ABS) && v.kind == K_INT) v.c[i] = ext1(t, t->t_i32, SPV_GLSL_S_ABS, v.c[i]);
        if (flags & F_NEG)
            v.c[i] = v.kind == K_FLOAT ? op1(t, SPV_OP_F_NEGATE, t->t_f32, v.c[i])
                                       : op1(t, SPV_OP_S_NEGATE, kind_type(t, v.kind), v.c[i]);
    }
    return v;
}
static int type_kind(Type type) { return is_float_type(type) ? K_FLOAT : is_signed_type(type) ? K_INT : K_UINT; }
/* component k of a register range from base in type: its word and its place there */
static uint32_t component(T* t, Bank bank, unsigned base, unsigned k, Type type) {
    unsigned per = packed_count(type), word = base + k / per, at = k % per;
    uint32_t w = load_word(t, bank, word);
    switch (type) {
        case T_F32: return bitcast(t, t->t_f32, w);
        case T_I32: return bitcast(t, t->t_i32, w);
        case T_U32: return w;
        case T_F16: {
            uint32_t pair = ext1(t, t->t_v2f, SPV_GLSL_UNPACK_HALF2X16, w);
            return SPV(&t->e, SPV_OP_COMPOSITE_EXTRACT, t->t_f32, pair, at);
        }
        case T_U8:
        case T_U16:
            return SPV(&t->e, SPV_OP_BIT_FIELD_U_EXTRACT, t->t_u32, w, cu(t, at * type_size(type) * 8),
                       cu(t, type_size(type) * 8));
        case T_I8:
        case T_I16:
            return SPV(&t->e, SPV_OP_BIT_FIELD_S_EXTRACT, t->t_i32, bitcast(t, t->t_i32, w), cu(t, at * type_size(type) * 8),
                       cu(t, type_size(type) * 8));
        case T_C10: { /* three signed 10-bit fixed point values, 255 a unit */
            uint32_t x = SPV(&t->e, SPV_OP_BIT_FIELD_S_EXTRACT, t->t_i32, bitcast(t, t->t_i32, w), cu(t, at * 10), cu(t, 10));
            Value v = {{x}, 1, K_INT};
            return to_float(t, v, T_C10, true).c[0];
        }
        default: return fail(t, "registers of type %d", (int)type), cf(t, 0);
    }
}
/* The operand's components in the mask (in order), of its type. FPINTERNAL registers hold four floats each */
static Value load(T* t, Operand op, unsigned mask, int shift) {
    Value v = {{0}, 0, type_kind(op.type)};
    if (op.bank == B_CONST) {
        for (unsigned i = 0; i < 4; ++i) {
            if (!(mask & (1u << i))) continue;
            unsigned ch = op.swizzle[i];
            if (ch >= SW_0 && ch <= SW_H) {
                v.c[v.n++] = swizzle_constant(t, ch, v.kind);
                continue;
            }
            if (ch > SW_W || op.num >= 64) return fail(t, "constant register %u", op.num), v;
            if (type_size(op.type) == 4) {
                uint32_t bits = ch == SW_Y ? usse_f32_bank1[op.num] : usse_f32_bank0[op.num];
                float f;
                memcpy(&f, &bits, 4);
                v.c[v.n++] = v.kind == K_FLOAT ? cf(t, f) : v.kind == K_UINT ? cu(t, bits) : ci(t, (int32_t)bits);
            } else {
                static const float* const banks[4] = {usse_f16_bank0, usse_f16_bank1, usse_f16_bank2, usse_f16_bank3};
                float f = banks[ch][op.num];
                v.c[v.n++] = v.kind == K_FLOAT ? cf(t, f) : v.kind == K_UINT ? cu(t, (uint32_t)f) : ci(t, (int32_t)f);
            }
        }
        return apply_modifiers(t, v, op.flags);
    }
    if (op.bank == B_IMM) {
        for (unsigned i = 0; i < 4; ++i) {
            if (!(mask & (1u << i))) continue;
            unsigned ch = op.swizzle[i];
            v.c[v.n++] = ch >= SW_0 && ch <= SW_H ? swizzle_constant(t, ch, v.kind)
                         : v.kind == K_FLOAT       ? cf(t, (float)op.num)
                         : v.kind == K_UINT        ? cu(t, op.num)
                                                   : ci(t, (int32_t)op.num);
        }
        return apply_modifiers(t, v, op.flags);
    }
    if (op.bank == B_INTERNAL) {
        op.type = op.type == T_F16 ? T_F32 : op.type == T_I16 || op.type == T_I8 ? T_I32
                  : op.type == T_U16 || op.type == T_U8                         ? T_U32
                                                                                : op.type;
        v.kind = type_kind(op.type);
        op.num <<= 2;
        shift <<= 2;
    }
    if (op.bank == B_PRED || op.bank == B_INDEX || op.bank == B_INDEXED1 || op.bank == B_INDEXED2 ||
        op.bank == B_GLOBAL || op.bank == B_SPECIAL || op.bank == B_INVALID)
        return fail(t, "operands in bank %d", (int)op.bank), v;
    for (unsigned i = 0; i < 4; ++i) {
        if (!(mask & (1u << i))) continue;
        unsigned ch = op.swizzle[i];
        if (ch >= SW_0 && ch <= SW_H)
            v.c[v.n++] = swizzle_constant(t, ch, v.kind);
        else if (ch <= SW_W)
            v.c[v.n++] = component(t, op.bank, op.num + (unsigned)shift, ch, op.type);
        else
            return fail(t, "an undefined swizzle"), v;
    }
    return apply_modifiers(t, v, op.flags);
}
/* The value's components into the masked components of dest (in order: a component the mask leaves out keeps its
   bits), of dest's type. Packed types are read, changed and written back a register at a time */
static void store(T* t, Operand dest, Value v, unsigned mask, int shift) {
    if (!v.n) return;
    if (dest.bank == B_INDEX || dest.bank == B_PRED || dest.bank == B_CONST || dest.bank == B_IMM ||
        dest.bank == B_INDEXED1 || dest.bank == B_INDEXED2 || dest.bank == B_SPECIAL || dest.bank == B_GLOBAL) {
        fail(t, "writes to bank %d", (int)dest.bank);
        return;
    }
    if (is_float_type(dest.type) != (v.kind == K_FLOAT)) {
        fail(t, "a %s value stored to a %s register", v.kind == K_FLOAT ? "float" : "integer",
             is_float_type(dest.type) ? "float" : "integer");
        return;
    }
    if (dest.bank == B_INTERNAL) { /* 32 bits a component there */
        dest.num <<= 2;
        shift <<= 2;
        if (dest.type == T_F16 || dest.type == T_C10) dest.type = T_F32;
        if (!is_float_type(dest.type)) dest.type = v.kind == K_INT ? T_I32 : T_U32;
    }
    unsigned base = dest.num + (unsigned)shift, per = packed_count(dest.type), taken = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!(mask & (1u << i))) continue;
        uint32_t x = v.c[taken < v.n ? taken : v.n - 1];
        ++taken;
        unsigned word = base + i / per, at = i % per;
        switch (dest.type) {
            case T_F32:
            case T_I32:
            case T_U32: store_word(t, dest.bank, word, v.kind == K_UINT ? x : bitcast(t, t->t_u32, x)); break;
            case T_F16: {
                uint32_t half = ext1(t, t->t_u32, SPV_GLSL_PACK_HALF2X16,
                                     SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v2f, x, cf(t, 0)));
                uint32_t old = load_word(t, dest.bank, word);
                store_word(t, dest.bank, word,
                           SPV(&t->e, SPV_OP_BIT_FIELD_INSERT, t->t_u32, old, half, cu(t, at * 16), cu(t, 16)));
                break;
            }
            case T_U8:
            case T_I8:
            case T_U16:
            case T_I16: {
                uint32_t bits = v.kind == K_UINT ? x : bitcast(t, t->t_u32, x), old = load_word(t, dest.bank, word);
                store_word(t, dest.bank, word,
                           SPV(&t->e, SPV_OP_BIT_FIELD_INSERT, t->t_u32, old, bits, cu(t, at * type_size(dest.type) * 8),
                               cu(t, type_size(dest.type) * 8)));
                break;
            }
            case T_C10: {
                Value one = {{x}, 1, K_FLOAT};
                one.c[0] = ext3(t, t->t_f32, SPV_GLSL_F_CLAMP, x, cf(t, -2.0f), cf(t, 2.0f));
                uint32_t bits = bitcast(t, t->t_u32, to_int(t, one, T_C10, true).c[0]), old = load_word(t, dest.bank, word);
                store_word(t, dest.bank, word,
                           SPV(&t->e, SPV_OP_BIT_FIELD_INSERT, t->t_u32, old, bits, cu(t, at * 10), cu(t, 10)));
                break;
            }
            default: fail(t, "stores of type %d", (int)dest.type); return;
        }
    }
}

/* ---- repeats (usse_translator.h: get_repeat_offset) ---- */
enum { R_EXTERNAL, R_INTERNAL, R_BOTH, R_SLMSI };
static int repeat_offset(T* t, const Operand* op, unsigned index, unsigned which, int mode) {
    if ((mode == R_INTERNAL || mode == R_BOTH) && op->bank == B_INTERNAL) return (int)index;
    if (op->flags & F_GPI) return 0;
    if (mode == R_BOTH) return op->bank == B_INTERNAL ? (int)index : (int)index * 4;
    if (mode == R_EXTERNAL && op->bank != B_INTERNAL) return (int)index * 4;
    if (mode == R_SLMSI) {
        int inc = t->repeat_increase[which][index];
        if ((op->bank >= B_TEMP && op->bank <= B_SA) || op->bank == B_PRED || op->bank == B_INDEXED1 ||
            op->bank == B_INDEXED2)
            inc *= t->repeat_multiplier[which];
        return inc;
    }
    return 0;
}
static void set_multiplier(T* t, int a, int b, int c, int d) {
    t->repeat_multiplier[0] = a, t->repeat_multiplier[1] = b, t->repeat_multiplier[2] = c, t->repeat_multiplier[3] = d;
}
static void reset_repeat_multiplier(T* t) { set_multiplier(t, 2, 2, 2, 2); }
/* SMLSI increments persist across instructions; reset them only at program/phase boundaries. */
static void reset_repeats(T* t) {
    reset_repeat_multiplier(t);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 17; ++j) t->repeat_increase[i][j] = j;
}
/* operands' indices into repeat_increase: src0, src1, src2, dest */
enum { O_SRC0, O_SRC1, O_SRC2, O_DEST };

/* ---- the instructions: each a handler of its pattern's fields (usse.decode, arg_<pattern>) ---- */
static unsigned popcount4(unsigned m) { return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1); }

/* VMOV (data.cpp) */
static bool vmov(T* t, const arg_VMOV* arg) {
    unsigned mask = arg->mask;
    Type type = (Type)arg->data_type;
    if (arg->move_type == 3) return fail(t, "VMOV move type 3");
    bool doubled = type == T_C10 || type == T_F16 || type == T_F32, conditional = arg->move_type != 0;
    unsigned bits = doubled ? 7 : 6;
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, doubled, bits);
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, doubled, bits);
    if (type == T_F16 || type == T_F32)
        mask = decode_write_mask(dest.bank, mask, type == T_F16);
    else if (!doubled)
        mask = 1;
    if (doubled)
        vec34_swizzle(src1.swizzle, arg->src0_swiz, false, 2);
    else
        memcpy(src1.swizzle, default_swizzle, 4);
    src1.type = dest.type = type;
    Operand src0 = {0}, src2 = {0};
    Type test_type = arg->move_type == 2 ? T_U8 : type;
    unsigned compare = (arg->test2 << 1) | arg->test1; /* 0 == 0, 1 != 0, 2 < 0, 3 <= 0 */
    if (conditional) {
        src0 = decode_src0(t, arg->src0_n, arg->src0_bank, arg->src0_ext, doubled, bits);
        src0.type = test_type;
        src2 = decode_src12(t, arg->src2_n, arg->src2_bank, arg->src2_ext, doubled, bits);
        src2.type = type;
        if (arg->comp_sel) memcpy(src0.swizzle, src1.swizzle, 4);
        memcpy(src2.swizzle, src1.swizzle, 4);
    }
    if (doubled && (type == T_F16 || type == T_F32))
        set_multiplier(t, 2, 2, 2, 2);
    else
        set_multiplier(t, 1, 1, 1, 1);
    for (int r = 0; r <= arg->repeat_count && !t->failed; ++r) {
        Value a = load(t, src1, mask, repeat_offset(t, &src1, r, O_SRC1, R_SLMSI)), result = a;
        if (conditional) {
            Value test = load(t, src0, mask, repeat_offset(t, &src0, r, O_SRC0, R_SLMSI));
            Value b = load(t, src2, mask, repeat_offset(t, &src2, r, O_SRC2, R_SLMSI));
            static const uint32_t float_ops[4] = {SPV_OP_F_ORD_EQUAL, SPV_OP_F_UNORD_NOT_EQUAL, SPV_OP_F_ORD_LESS_THAN,
                                                  SPV_OP_F_ORD_LESS_THAN_EQUAL};
            for (unsigned i = 0; i < a.n && i < test.n; ++i) {
                uint32_t zero = test.kind == K_FLOAT ? cf(t, 0) : test.kind == K_UINT ? cu(t, 0) : ci(t, 0), cond;
                if (test.kind == K_FLOAT)
                    cond =
                        op2(t, compare == 1 ? SPV_OP_F_ORD_NOT_EQUAL : float_ops[compare], t->t_bool, test.c[i], zero);
                else if (compare < 2)
                    cond = op2(t, compare ? SPV_OP_I_NOT_EQUAL : SPV_OP_I_EQUAL, t->t_bool, test.c[i], zero);
                else if (test.kind == K_UINT)
                    cond = op2(t, compare == 2 ? SPV_OP_U_LESS_THAN : SPV_OP_U_LESS_THAN_EQUAL, t->t_bool, test.c[i], zero);
                else
                    cond = op2(t, compare == 2 ? SPV_OP_S_LESS_THAN : SPV_OP_S_LESS_THAN_EQUAL, t->t_bool, test.c[i], zero);
                result.c[i] = SPV(&t->e, SPV_OP_SELECT, kind_type(t, a.kind), cond, a.c[i], b.c[i]);
            }
        }
        store(t, dest, result, mask, repeat_offset(t, &dest, r, O_DEST, R_SLMSI));
    }
    reset_repeat_multiplier(t);
    return !t->failed;
}

/* VMAD (vec3/vec4 multiply-add with the GPI registers) */
static bool vmad(T* t, const arg_VMAD* arg) {
    int type = arg->opcode2 ? 2 : 1;
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, true, 7);
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, true, 7);
    Operand src0 = {arg->gpi0_n, B_INTERNAL, F_GPI, {0}, T_F32}, src2 = {arg->gpi1_n, B_INTERNAL, F_GPI, {0}, T_F32};
    vec34_swizzle(src1.swizzle, arg->src1_swiz, arg->src1_swiz_ext, type);
    vec34_swizzle(src0.swizzle, arg->gpi0_swiz, arg->gpi0_swiz_ext, type);
    vec34_swizzle(src2.swizzle, arg->gpi1_swiz, arg->gpi1_swiz_ext, type);
    src1.flags |= (arg->src1_abs ? F_ABS : 0) | (arg->src1_neg ? F_NEG : 0);
    src0.flags |= (arg->gpi0_abs ? F_ABS : 0) | (arg->gpi0_neg ? F_NEG : 0);
    src2.flags |= (arg->gpi1_abs ? F_ABS : 0) | (arg->gpi1_neg ? F_NEG : 0);
    set_multiplier(t, 2, 2, 2, 4);
    for (int r = 0; r <= arg->repeat_count && !t->failed; ++r) {
        Value a = load(t, src0, arg->mask, repeat_offset(t, &src0, r, O_SRC0, (int)arg->repeat_mode));
        Value b = load(t, src1, arg->mask, repeat_offset(t, &src1, r, O_SRC1, (int)arg->repeat_mode));
        Value c = load(t, src2, arg->mask, repeat_offset(t, &src2, r, O_SRC2, (int)arg->repeat_mode));
        Value result = {{0}, a.n, K_FLOAT};
        for (unsigned i = 0; i < a.n; ++i) result.c[i] = ext3(t, t->t_f32, SPV_GLSL_FMA, a.c[i], b.c[i], c.c[i]);
        store(t, dest, result, arg->mask, repeat_offset(t, &dest, r, O_DEST, (int)arg->repeat_mode));
    }
    reset_repeat_multiplier(t);
    return !t->failed;
}

/* V32NMAD and V16NMAD (alu.cpp: v32nmad, do_alu_op) */
static bool vnmad(T* t, const arg_nmad* arg, bool f32) {
    Type type = f32 ? T_F32 : T_F16;
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, true, 7);
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, true, 7);
    Operand src2 = decode_src12(t, arg->src2_n, arg->src2_bank, arg->src2_ext, true, 7);
    src1.flags = decode_modifier(arg->src1_mod);
    if (arg->src2_mod == 1) src2.flags = F_ABS;
    dest.type = src1.type = src2.type = type;
    swizzle4(src1.swizzle, arg->src1_swiz);
    vec34_swizzle(src2.swizzle, arg->src2_swiz, false, 2);
    unsigned source_mask = arg->alu == 7 ? 15 : arg->mask; /* a dot product reads all four */
    Value a = load(t, src1, source_mask, 0), b = load(t, src2, source_mask, 0);
    if (t->failed) return false;
    Value r = {{0}, a.n, K_FLOAT};
    for (unsigned i = 0; i < a.n; ++i) {
        switch (arg->alu) {
            case 0: r.c[i] = op2(t, SPV_OP_F_MUL, t->t_f32, a.c[i], b.c[i]); break;
            case 1: r.c[i] = op2(t, SPV_OP_F_ADD, t->t_f32, a.c[i], b.c[i]); break;
            case 2: /* FRC: a - floor(b) */
                r.c[i] = op2(t, SPV_OP_F_SUB, t->t_f32, a.c[i], ext1(t, t->t_f32, SPV_GLSL_FLOOR, b.c[i]));
                break;
            case 3: r.c[i] = op1(t, SPV_OP_DPDX, t->t_f32, a.c[i]); break;
            case 4: r.c[i] = op1(t, SPV_OP_DPDY, t->t_f32, a.c[i]); break;
            case 5: r.c[i] = ext2(t, t->t_f32, SPV_GLSL_F_MIN, a.c[i], b.c[i]); break;
            case 6: r.c[i] = ext2(t, t->t_f32, SPV_GLSL_F_MAX, a.c[i], b.c[i]); break;
            default: break;
        }
    }
    if (arg->alu == 7) { /* DP: the sum of the products, in each masked component */
        uint32_t sum = op2(t, SPV_OP_F_MUL, t->t_f32, a.c[0], b.c[0]);
        for (unsigned i = 1; i < a.n; ++i)
            sum = ext3(t, t->t_f32, SPV_GLSL_FMA, a.c[i], b.c[i], sum);
        r.n = popcount4(arg->mask);
        for (unsigned i = 0; i < r.n; ++i) r.c[i] = sum;
    }
    store(t, dest, r, arg->mask, 0);
    return !t->failed;
}

/* VPCK (data.cpp) */
static bool vpck(T* t, const arg_VPCK* arg) {
    unsigned src1_n = arg->src1_n;
    static const Type types[8] = {T_U8, T_I8, T_O8, T_U16, T_I16, T_F16, T_F32, T_C10};
    Type dest_type = types[arg->dest_fmt], src_type = types[arg->src_fmt];
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, false, 7);
    dest.type = dest_type;
    bool src1_doubled = true;
    if (!is_float_type(src_type)) {
        src1_n = arg->sel0_bit1 | (src1_n << 1);
        src1_doubled = false;
    }
    Operand src1 = decode_src12(t, src1_n, arg->src1_bank, arg->src1_ext, src1_doubled, 7);
    Operand src2 = decode_src12(t, arg->src2_n, arg->src2_bank, arg->src2_ext, true, 7);
    src1.type = src_type;
    unsigned sel0 = arg->sel0_bit0 | ((src_type == T_F32 ? arg->sel0_bit1 & 1 : arg->src2_n & 1) << 1);
    uint8_t swizzle[4] = {(uint8_t)sel0, (uint8_t)arg->sel1, (uint8_t)arg->sel2, (uint8_t)arg->sel3};
    memcpy(src1.swizzle, swizzle, 4);
    bool no_cycle = (is_float_type(src_type) && is_float_type(dest_type)) ||
                    (arg->scale && (src_type == T_U8 || dest_type == T_U8));
    bool use_src2 = false;
    if (src_type == T_F32 && src2.bank != B_IMM) {
        bool same = src1.num == src2.num && src1.bank == src2.bank;
        bool contiguous = arg->mask == 1 || arg->mask == 3 || arg->mask == 7 || arg->mask == 15, defaulted = true;
        unsigned n = popcount4(arg->mask);
        for (unsigned i = 0; i < n; ++i) defaulted = defaulted && src1.swizzle[i] == i;
        use_src2 = !defaulted || !same || !contiguous;
    }
    if (!no_cycle) {
        uint8_t taken[4];
        memcpy(taken, src1.swizzle, 4);
        bool two = src_type == T_U8 || dest_type == T_U8;
        unsigned next = 0;
        for (unsigned i = 0; i < 4; ++i)
            if (arg->mask & (1u << i)) src1.swizzle[i] = two && arg->mask == 15 ? taken[i % 2] : taken[next++];
    }
    set_multiplier(t, 1, is_float_type(src_type) ? 2 : 1, is_float_type(src_type) ? 2 : 1, 1);
    for (int r = 0; r <= arg->repeat_count && !t->failed; ++r) {
        int o1 = repeat_offset(t, &src1, r, O_SRC1, R_SLMSI), o2 = repeat_offset(t, &src2, r, O_SRC2, R_SLMSI);
        Value v;
        if (use_src2) {
            /* the two sources as one range of four: src1's x and y, then src2's */
            Operand a = src1, b = src2;
            memcpy(a.swizzle, default_swizzle, 4), memcpy(b.swizzle, default_swizzle, 4);
            b.type = src_type;
            Value lo = load(t, a, 3, o1), hi = load(t, b, 3, o2);
            uint32_t all[4] = {lo.c[0], lo.c[1], hi.c[0], hi.c[1]};
            v = (Value){{0}, 0, K_FLOAT};
            for (unsigned i = 0; i < 4; ++i)
                if (arg->mask & (1u << i)) {
                    unsigned ch = src1.swizzle[i];
                    v.c[v.n++] = ch <= SW_W ? all[ch] : swizzle_constant(t, ch, K_FLOAT);
                }
        } else
            v = load(t, src1, arg->mask, o1);
        if (is_float_type(dest_type) && !is_float_type(src_type)) v = to_float(t, v, src_type, arg->scale);
        if (!is_float_type(dest_type) && is_float_type(src_type)) v = to_int(t, v, dest_type, arg->scale);
        if (!is_float_type(dest_type) && !is_float_type(src_type) && is_signed_type(dest_type) != is_signed_type(src_type))
            for (unsigned i = 0; i < v.n; ++i)
                v.c[i] = bitcast(t, is_signed_type(dest_type) ? t->t_i32 : t->t_u32, v.c[i]),
                v.kind = is_signed_type(dest_type) ? K_INT : K_UINT;
        store(t, dest, v, arg->mask, repeat_offset(t, &dest, r, O_DEST, R_SLMSI));
    }
    reset_repeat_multiplier(t);
    return !t->failed;
}
/* SMLSI: the repeat increments (special.cpp) */
static bool smlsi(T* t, const arg_SMLSI* arg) {
    unsigned modes[4] = {arg->src0_mode, arg->src1_mode, arg->src2_mode, arg->dest_mode};
    unsigned values[4] = {arg->src0_inc, arg->src1_inc, arg->src2_inc, arg->dest_inc};
    for (unsigned k = 0; k < 4; ++k) {
        if (modes[k])
            for (int i = 0; i < 4; ++i) t->repeat_increase[k][i] = (int)((values[k] >> (2 * i)) & 3);
        else
            for (int i = 0; i < 17; ++i) t->repeat_increase[k][i] = i * (int8_t)values[k];
    }
    return true;
}

/* ---- the instructions ported second: the ALU (alu.cpp, ialu.cpp), tests (branch_cond.cpp), loads (data.cpp),
   dependent texture reads (texture.cpp) and the special ones (special.cpp) ---- */
static Type texture_component_type(uint32_t format);
static unsigned gxp_sampler_dimension(const T* t, unsigned resource);
static uint32_t sampler_of(T* t, unsigned resource, unsigned dimension);
static unsigned texture_components(uint32_t format);
static Value vector_value(T* t, uint32_t vec, unsigned n, int kind);
static void store_input(T* t, Bank bank, unsigned offset, Type type, Value v);
static uint32_t zero_of(T* t, int kind) { return kind == K_FLOAT ? cf(t, 0) : kind == K_UINT ? cu(t, 0) : ci(t, 0); }
static void store_predicate(T* t, unsigned n, uint32_t cond) {
    uint32_t pointer = spv_emit_type_pointer(&t->e, SPV_STORAGE_PRIVATE, t->t_bool);
    SPV_VOID(&t->e, SPV_OP_STORE, SPV(&t->e, SPV_OP_ACCESS_CHAIN, pointer, t->bank[B_PRED], cu(t, n & 3)), cond);
}
static Value one_value(uint32_t c, int kind) { return (Value){{c}, 1, kind}; }

static bool vmad2(T* t, const arg_VMAD2* arg) {
    unsigned mask = arg->mask;
    static const uint8_t swizzles[3][8][4] = {
        {S4(X, X, X, X), S4(Y, Y, Y, Y), S4(Z, Z, Z, Z), S4(W, W, W, W), S4(X, Y, Z, W), S4(Y, Z, X, W), S4(X, Y, W, W),
         S4(Z, W, X, Y)},
        {S4(X, X, X, X), S4(Y, Y, Y, Y), S4(Z, Z, Z, Z), S4(W, W, W, W), S4(X, Y, Z, W), S4(X, Y, Y, Z), S4(Y, Y, W, W),
         S4(W, Y, Z, W)},
        {S4(X, X, X, X), S4(Y, Y, Y, Y), S4(Z, Z, Z, Z), S4(W, W, W, W), S4(X, Y, Z, W), S4(X, Z, W, W), S4(X, X, Y, Z),
         S4(X, Y, Z, Z)}};
    Type type = arg->dat_fmt ? T_F16 : T_F32;
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, false, true, 7);
    Operand src0 = decode_src0(t, arg->src0_n, arg->src0_bank, false, true, 7);
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, true, 7);
    Operand src2 = decode_src12(t, arg->src2_n, arg->src2_bank, arg->src2_ext, true, 7);
    mask = decode_write_mask(dest.bank, mask, arg->dat_fmt);
    dest.type = src0.type = src1.type = src2.type = type;
    memcpy(src0.swizzle, swizzles[0][arg->src0_swiz01 | arg->src0_swiz2 << 2], 4);
    memcpy(src1.swizzle, swizzles[1][arg->src1_swiz01 | arg->src1_swiz2 << 2], 4);
    memcpy(src2.swizzle, swizzles[2][arg->src2_swiz], 4);
    if (arg->src0_abs) src0.flags |= F_ABS;
    src1.flags = decode_modifier(arg->src1_mod), src2.flags = decode_modifier(arg->src2_mod);
    Value a = load(t, src0, mask, 0), b = load(t, src1, mask, 0), c = load(t, src2, mask, 0);
    if (t->failed) return false;
    Value r = {{0}, a.n, K_FLOAT};
    for (unsigned i = 0; i < a.n; ++i) r.c[i] = ext3(t, t->t_f32, SPV_GLSL_FMA, a.c[i], b.c[i], c.c[i]);
    store(t, dest, r, mask, 0);
    return !t->failed;
}

/* VDP (a dot product with a GPI register). The destination stays, the write mask turns one component a repeat */
static bool vdp(T* t, const arg_VDP* arg) {
    unsigned mask = arg->mask;
    static const uint8_t channels[8] = {SW_X, SW_Y, SW_Z, SW_W, SW_0, SW_1, SW_2, SW_H};
    int type = arg->opcode2 ? 2 : 1;
    unsigned source_mask = arg->opcode2 ? 15 : 7;
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, true, 7);
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, true, 7);
    Operand src2 = {arg->gpi0_n, B_INTERNAL, F_GPI, {0}, T_F32};
    vec34_swizzle(src2.swizzle, arg->gpi0_swiz, false, type);
    const int swizzle[4] = {arg->src1_swiz_x, arg->src1_swiz_y, arg->src1_swiz_z, arg->src1_swiz_w};
    for (unsigned i = 0; i < 4; ++i) src1.swizzle[i] = channels[swizzle[i]];
    src1.flags |= (arg->src1_neg ? F_NEG : 0) | (arg->src1_abs ? F_ABS : 0);
    if (arg->gpi0_abs) src2.flags |= F_ABS;
    for (int r = 0; r <= arg->repeat_count && !t->failed; ++r) {
        Value a = load(t, src1, source_mask, repeat_offset(t, &src1, r, O_SRC1, (int)arg->repeat_mode));
        Value b = load(t, src2, source_mask, repeat_offset(t, &src2, r, O_SRC2, (int)arg->repeat_mode));
        if (t->failed) break;
        uint32_t sum = op2(t, SPV_OP_F_MUL, t->t_f32, a.c[0], b.c[0]);
        for (unsigned i = 1; i < a.n; ++i) sum = ext3(t, t->t_f32, SPV_GLSL_FMA, a.c[i], b.c[i], sum);
        store(t, dest, one_value(sum, K_FLOAT), mask, 0);
        mask = ((mask << 1) | (mask >> 3)) & 15;
    }
    return !t->failed;
}

static void dual_swizzle(uint8_t out[4], unsigned swizzle, bool extended, bool vec4) {
    static const uint8_t v4[2][16][4] = {
        {S4(X, X, X, X), S4(Y, Y, Y, Y), S4(Z, Z, Z, Z), S4(W, W, W, W), S4(X, Y, Z, W), S4(Y, Z, W, W),
         S4(X, Y, Z, Z), S4(X, X, Y, Z), S4(X, Y, X, Y), S4(X, Y, W, Z), S4(Z, X, Y, W), S4(Z, W, Z, W),
         S4(0, 0, 0, 0), S4(H, H, H, H), S4(1, 1, 1, 1), S4(2, 2, 2, 2)},
        {S4(Y, Z, X, W), S4(Z, W, X, Y), S4(X, Z, W, Y), S4(Y, Y, W, W), S4(W, Y, Z, W), S4(W, Z, W, Z),
         S4(X, Y, Z, X), S4(Z, Z, W, W), S4(X, W, Z, X), S4(Y, Y, Y, X), S4(Y, Y, Y, Z), S4(Z, W, Z, W),
         S4(Y, Z, X, Z), S4(X, X, Y, Y), S4(X, Z, W, W), S4(X, Y, Z, 1)}};
    static const uint8_t v3[2][16][4] = {
        {S3(X, X, X), S3(Y, Y, Y), S3(Z, Z, Z), S3(W, W, W), S3(X, Y, Z), S3(Y, Z, W), S3(X, X, Y), S3(X, Y, X),
         S3(Y, Y, X), S3(Y, Y, Z), S3(Z, X, Y), S3(X, Z, Y), S3(0, 0, 0), S3(H, H, H), S3(1, 1, 1), S3(2, 2, 2)},
        {S3(X, Y, Y), S3(Y, X, Y), S3(X, X, Z), S3(Y, X, X), S3(X, Y, 0), S3(X, 1, 0), S3(X, Z, Y), S3(Y, Z, X),
         S3(Z, Y, X), S3(Z, Z, Y), S3(X, Y, 1), SU, SU, SU, SU, SU}};
    memcpy(out, vec4 ? v4[extended][swizzle & 15] : v3[extended][swizzle & 15], 4);
}

enum { DUAL_INTERNAL0, DUAL_INTERNAL1, DUAL_INTERNAL2, DUAL_UNIFIED, DUAL_NONE };
typedef struct {
    unsigned sources;
    bool vector_load, vector_store;
} DualInfo;
static const DualInfo dual_info[14] = {
    {3, true, true},  {2, true, false}, {1, true, false}, {2, true, true},  {2, true, true},
    {1, true, true},  {1, false, false}, {1, false, false}, {3, false, false}, {2, false, false},
    {2, false, false}, {2, false, false}, {1, false, false}, {1, false, false}};

static bool dual_op1_layout(unsigned count, unsigned config, uint8_t out[3]) {
    static const uint8_t layouts[3][4][3] = {
        {{DUAL_UNIFIED, DUAL_NONE, DUAL_NONE}, {DUAL_INTERNAL0, DUAL_NONE, DUAL_NONE},
         {DUAL_INTERNAL1, DUAL_NONE, DUAL_NONE}, {DUAL_INTERNAL2, DUAL_NONE, DUAL_NONE}},
        {{DUAL_UNIFIED, DUAL_INTERNAL1, DUAL_NONE}, {DUAL_INTERNAL0, DUAL_UNIFIED, DUAL_NONE},
         {DUAL_INTERNAL0, DUAL_INTERNAL1, DUAL_NONE}, {DUAL_NONE, DUAL_NONE, DUAL_NONE}},
        {{DUAL_UNIFIED, DUAL_INTERNAL1, DUAL_INTERNAL2}, {DUAL_INTERNAL0, DUAL_UNIFIED, DUAL_INTERNAL2},
         {DUAL_INTERNAL0, DUAL_INTERNAL1, DUAL_UNIFIED}, {DUAL_INTERNAL0, DUAL_INTERNAL1, DUAL_INTERNAL2}}};
    if (!count || count > 3 || config > 3) return false;
    memcpy(out, layouts[count - 1][config], 3);
    return out[0] != DUAL_NONE;
}

static bool dual_op2_layout(unsigned first_count, unsigned count, unsigned config, uint8_t out[3]) {
    if (config > 3) return false;
    out[1] = out[2] = DUAL_NONE;
    if (first_count == 1) {
        if (count == 1) {
            out[0] = config == 0 ? DUAL_INTERNAL0 : DUAL_UNIFIED;
            return true;
        }
        if (count == 2) {
            static const uint8_t layouts[4][3] = {
                {DUAL_INTERNAL0, DUAL_INTERNAL1, DUAL_NONE}, {DUAL_UNIFIED, DUAL_INTERNAL1, DUAL_NONE},
                {DUAL_INTERNAL0, DUAL_UNIFIED, DUAL_NONE}, {DUAL_INTERNAL1, DUAL_UNIFIED, DUAL_NONE}};
            memcpy(out, layouts[config], 3);
            return true;
        }
        if (count == 3) {
            static const uint8_t layouts[4][3] = {
                {DUAL_INTERNAL0, DUAL_INTERNAL1, DUAL_INTERNAL2}, {DUAL_UNIFIED, DUAL_INTERNAL1, DUAL_INTERNAL2},
                {DUAL_INTERNAL0, DUAL_UNIFIED, DUAL_INTERNAL2}, {DUAL_INTERNAL0, DUAL_UNIFIED, DUAL_INTERNAL1}};
            memcpy(out, layouts[config], 3);
            return true;
        }
    } else if (first_count == 2) {
        if (count == 1 && config < 3) {
            out[0] = config == 0 ? DUAL_INTERNAL0 : config == 1 ? DUAL_INTERNAL1 : DUAL_UNIFIED;
            return true;
        }
        if (count == 2 && config < 3) {
            static const uint8_t layouts[3][3] = {
                {DUAL_INTERNAL0, DUAL_INTERNAL2, DUAL_NONE}, {DUAL_INTERNAL1, DUAL_INTERNAL2, DUAL_NONE},
                {DUAL_INTERNAL2, DUAL_UNIFIED, DUAL_NONE}};
            memcpy(out, layouts[config], 3);
            return true;
        }
    } else if (first_count == 3 && count == 1) {
        out[0] = config == 0 ? DUAL_INTERNAL0 : config == 1 ? DUAL_INTERNAL1 : config == 2 ? DUAL_INTERNAL2 : DUAL_UNIFIED;
        return true;
    }
    return false;
}

static Operand dual_source(T* t, unsigned id, unsigned first_count, const arg_VDUAL* arg,
                           Type type, bool vector_load) {
    unsigned swizzle, number;
    Operand op = {0, B_INTERNAL, 0, {SW_X, SW_Y, SW_Z, SW_W}, type};
    switch (id) {
        case DUAL_UNIFIED:
            op = decode_src12(t, arg->unified_store_slot_num, arg->unified_store_slot_bank, false, vector_load,
                              vector_load ? 8 : 7);
            dual_swizzle(op.swizzle, arg->unified_store_swiz, first_count < 2 && arg->gpi2_slot_num_bit_1,
                         arg->comp_count_type);
            if (first_count < 2 && arg->gpi2_slot_num_bit_0_or_unified_store_abs) op.flags |= F_ABS;
            if (arg->unified_store_neg) op.flags |= F_NEG;
            break;
        case DUAL_INTERNAL0:
            number = arg->gpi0_slot_num, swizzle = arg->gpi0_swiz;
            op.num = number;
            dual_swizzle(op.swizzle, swizzle, false, arg->comp_count_type);
            break;
        case DUAL_INTERNAL1:
            number = arg->gpi1_slot_num, swizzle = arg->gpi1_swiz;
            op.num = number;
            dual_swizzle(op.swizzle, swizzle, arg->gpi1_swiz_ext, arg->comp_count_type);
            if (arg->gpi1_neg) op.flags |= F_NEG;
            break;
        case DUAL_INTERNAL2:
            op.num = first_count >= 2
                         ? ((arg->gpi2_slot_num_bit_1 << 1) | arg->gpi2_slot_num_bit_0_or_unified_store_abs)
                         : 2;
            break;
        default:
            fail(t, "VDUAL source layout");
            break;
    }
    op.type = type;
    return op;
}

static Value dual_alu(T* t, unsigned code, Operand src[3], unsigned source_count, unsigned write_mask,
                      unsigned comp_count_type) {
    bool dot = code == 1 || code == 2;
    unsigned source_mask = dual_info[code].vector_load ? (dot ? (comp_count_type ? 15 : 7) : write_mask) : 1;
    Value a = load(t, src[0], source_mask, 0);
    Value b = source_count > 1 ? load(t, src[1], source_mask, 0) : (Value){{0}, 0, K_FLOAT};
    Value c = source_count > 2 ? load(t, src[2], source_mask, 0) : (Value){{0}, 0, K_FLOAT};
    Value result = {{0}, a.n, K_FLOAT};
    if (t->failed) return result;
    switch (code) {
        case 0:
        case 8:
            for (unsigned i = 0; i < a.n; ++i) {
                uint32_t product = op2(t, SPV_OP_F_MUL, t->t_f32, a.c[i], b.c[i]);
                result.c[i] = op2(t, SPV_OP_F_ADD, t->t_f32, product, c.c[i]);
            }
            break;
        case 1:
        case 2:
            result = one_value(op2(t, SPV_OP_F_MUL, t->t_f32, a.c[0], code == 1 ? b.c[0] : a.c[0]), K_FLOAT);
            for (unsigned i = 1; i < a.n; ++i)
                result.c[0] = ext3(t, t->t_f32, SPV_GLSL_FMA, a.c[i], code == 1 ? b.c[i] : a.c[i], result.c[0]);
            break;
        case 3:
        case 10:
        case 4:
        case 9:
        case 11:
            for (unsigned i = 0; i < a.n; ++i) {
                if (code == 3 || code == 10) result.c[i] = op2(t, SPV_OP_F_MUL, t->t_f32, a.c[i], b.c[i]);
                else if (code == 4 || code == 9) result.c[i] = op2(t, SPV_OP_F_ADD, t->t_f32, a.c[i], b.c[i]);
                else result.c[i] = op2(t, SPV_OP_F_SUB, t->t_f32, a.c[i], ext1(t, t->t_f32, SPV_GLSL_FLOOR, b.c[i]));
            }
            break;
        case 5: result = a; break;
        case 6:
        case 7:
        case 12:
        case 13:
            for (unsigned i = 0; i < a.n; ++i) {
                if (code == 6) result.c[i] = ext1(t, t->t_f32, SPV_GLSL_INVERSE_SQRT, a.c[i]);
                else if (code == 7) result.c[i] = op2(t, SPV_OP_F_DIV, t->t_f32, cf(t, 1), a.c[i]);
                else if (code == 12) {
                    uint32_t exp = ext1(t, t->t_f32, SPV_GLSL_EXP2, a.c[i]);
                    result.c[i] = SPV(&t->e, SPV_OP_SELECT, t->t_f32, op1(t, SPV_OP_IS_NAN, t->t_bool, exp),
                                      cf(t, 1), exp);
                } else
                    result.c[i] = ext1(t, t->t_f32, SPV_GLSL_LOG2, a.c[i]);
            }
            break;
        default: fail(t, "VDUAL opcode %u", code); break;
    }
    return result;
}

/* The paired vector/scalar operations share their register reads and commit both destinations after evaluating them. */
static bool vdual(T* t, const arg_VDUAL* arg) {
    unsigned op1_code = ((!arg->comp_count_type && arg->op1_ext_vec3_or_has_w_vec4) << 3) | arg->op1;
    unsigned op2_code = (arg->op2_ext << 3) | arg->op2;
    if (op1_code >= 14 || !op2_code || op2_code >= 14) return fail(t, "VDUAL opcode");
    const DualInfo op1_info = dual_info[op1_code], op2_info = dual_info[op2_code];
    unsigned first_count = op1_info.sources;
    bool unified_vector = (arg->prim_ustore && op1_info.vector_store) || (!arg->prim_ustore && op2_info.vector_store);
    Type type = arg->type_f16 ? T_F16 : T_F32;

    Operand internal_dest = {arg->prim_dest_num_gpi, B_INTERNAL, 0, {SW_X, SW_Y, SW_Z, SW_W}, type};
    Operand unified_dest =
        decode_dest(t, arg->prim_dest_num, arg->prim_dest_bank, false, unified_vector, unified_vector ? 8 : 7);
    unified_dest.type = type;
    Operand op1_dest = arg->prim_ustore ? unified_dest : internal_dest;
    Operand op2_dest = arg->prim_ustore ? internal_dest : unified_dest;

    uint8_t layout1[3], layout2[3];
    if (!dual_op1_layout(op1_info.sources, arg->src_config, layout1) ||
        !dual_op2_layout(op1_info.sources, op2_info.sources, arg->src_config, layout2))
        return fail(t, "VDUAL source layout");

    unsigned fixed_mask = arg->write_mask_non_gpi | ((arg->comp_count_type && arg->op1_ext_vec3_or_has_w_vec4) << 3);
    unsigned op1_mask = arg->prim_ustore ? (op1_dest.bank == B_INTERNAL ? (arg->comp_count_type ? 15 : 7)
                                                              : op1_info.vector_store ? (arg->type_f16 ? 15 : 3)
                                                                                      : (arg->type_f16 ? 3 : 1))
                              : fixed_mask;
    unsigned op2_mask = arg->prim_ustore ? fixed_mask
                              : (op2_dest.bank == B_INTERNAL ? (arg->comp_count_type ? 15 : 7)
                                                             : op2_info.vector_store ? (arg->type_f16 ? 15 : 3)
                                                                                     : (arg->type_f16 ? 3 : 1));
    Operand src1[3], src2[3];
    for (unsigned i = 0; i < op1_info.sources; ++i)
        src1[i] = dual_source(t, layout1[i], first_count, arg, type, op1_info.vector_load);
    for (unsigned i = 0; i < op2_info.sources; ++i)
        src2[i] = dual_source(t, layout2[i], first_count, arg, type, op2_info.vector_load);
    if (t->failed) return false;

    Value result1 = dual_alu(t, op1_code, src1, op1_info.sources, op1_mask, arg->comp_count_type);
    Value result2 = dual_alu(t, op2_code, src2, op2_info.sources, op2_mask, arg->comp_count_type);
    if (t->failed) return false;
    store(t, op1_dest, result1, op1_mask, 0);
    store(t, op2_dest, result2, op2_mask, 0);
    return !t->failed;
}

/* VCOMP (the complex unit: one component's reciprocal, reciprocal square root, base 2 logarithm or exponential,
   in each masked one): p s d y e n r a o b c m f t k g h w */
static bool vcomp(T* t, const arg_VCOMP* arg) {
    static const Type types[4] = {T_F32, T_F16, T_C10, T_UNK};
    if (types[arg->dest_type] == T_UNK || types[arg->src_type] == T_UNK) return fail(t, "VCOMP with type 3");
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, true, 8);
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, true, 8);
    src1.flags = decode_modifier(arg->src1_mod);
    src1.type = types[arg->src_type], dest.type = types[arg->dest_type];
    for (int r = 0; r <= arg->repeat_count && !t->failed; ++r) {
        Value v = load(t, src1, 1u << arg->src_comp, repeat_offset(t, &src1, r, O_SRC1, R_SLMSI));
        if (t->failed) break;
        uint32_t x = v.c[0];
        switch (arg->op) {
            case 0: x = op2(t, SPV_OP_F_DIV, t->t_f32, cf(t, 1), x); break;
            case 1: x = ext1(t, t->t_f32, SPV_GLSL_INVERSE_SQRT, x); break;
            case 2: x = ext1(t, t->t_f32, SPV_GLSL_LOG2, x); break;
            default: /* exp2(NaN) is 1, as a game needs */
                x = SPV(&t->e, SPV_OP_SELECT, t->t_f32, op1(t, SPV_OP_IS_NAN, t->t_bool, x), cf(t, 1),
                        ext1(t, t->t_f32, SPV_GLSL_EXP2, x));
                break;
        }
        store(t, dest, one_value(x, K_FLOAT), arg->mask, repeat_offset(t, &dest, r, O_DEST, R_SLMSI));
    }
    return !t->failed;
}

/* a rotate left of a 32-bit (or 16-bit) value */
static uint32_t rotate_left(T* t, uint32_t x, uint32_t n, unsigned bits) {
    n = op2(t, SPV_OP_BITWISE_AND, t->t_u32, n, cu(t, bits - 1));
    uint32_t left = op2(t, SPV_OP_SHIFT_LEFT_LOGICAL, t->t_u32, x, n);
    uint32_t right = op2(t, SPV_OP_SHIFT_RIGHT_LOGICAL, t->t_u32, x, op2(t, SPV_OP_I_SUB, t->t_u32, cu(t, bits), n));
    uint32_t r = SPV(&t->e, SPV_OP_SELECT, t->t_u32, op2(t, SPV_OP_I_EQUAL, t->t_bool, n, cu(t, 0)), x,
                     op2(t, SPV_OP_BITWISE_OR, t->t_u32, left, right));
    return bits == 16 ? op2(t, SPV_OP_BITWISE_AND, t->t_u32, r, cu(t, 0xffff)) : r;
}
/* VBW (bitwise: and, or, xor, shifts, rotate) */
static bool vbw(T* t, const arg_VBW* arg) {
    enum { AND, OR, XOR, SHL, ROL, SHR, ASR } op;
    switch (arg->op_1) {
        case 2: op = arg->op_2 ? OR : AND; break;
        case 3: op = XOR; break;
        case 4: op = arg->op_2 ? ROL : SHL; break;
        case 5: op = arg->op_2 ? ASR : SHR; break;
        default: return fail(t, "VBW operation %u", arg->op_1);
    }
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, false, 7);
    Operand src2 = decode_src12(t, arg->src2_n, arg->src2_bank, arg->src2_ext, false, 7);
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, false, 7);
    unsigned bits = arg->partial ? 16 : 32;
    src1.type = src2.type = dest.type = arg->partial ? T_U16 : T_U32;
    set_multiplier(t, 1, 1, 1, 1);
    for (int r = 0; r <= arg->repeat_count && !t->failed; ++r) {
        uint32_t b;
        if (arg->src2_ext && src2.bank == B_IMM) { /* the immediate, rotated and inverted as encoded */
            uint32_t value = arg->src2_n | arg->src2_sel << 7 | arg->exth << 14, n = arg->rot & (bits - 1);
            if (n) value = bits == 16 ? ((value << n) | ((value & 0xffff) >> (16 - n))) & 0xffff
                                      : (value << n) | (value >> (32 - n));
            if (arg->invert) value = ~value;
            if (bits == 16) value &= 0xffff;
            b = cu(t, value);
        } else {
            b = load(t, src2, 1, repeat_offset(t, &src2, r, O_SRC2, R_SLMSI)).c[0];
            if (arg->rot) b = rotate_left(t, b, cu(t, arg->rot), bits);
            if (arg->invert) {
                b = op1(t, SPV_OP_NOT, t->t_u32, b);
                if (bits == 16) b = op2(t, SPV_OP_BITWISE_AND, t->t_u32, b, cu(t, 0xffff));
            }
        }
        uint32_t a = load(t, src1, 1, repeat_offset(t, &src1, r, O_SRC1, R_SLMSI)).c[0], x;
        if (t->failed) break;
        uint32_t shift = op2(t, SPV_OP_BITWISE_AND, t->t_u32, b, cu(t, 31));
        switch (op) {
            case AND: x = op2(t, SPV_OP_BITWISE_AND, t->t_u32, a, b); break;
            case OR: x = op2(t, SPV_OP_BITWISE_OR, t->t_u32, a, b); break;
            case XOR: x = op2(t, SPV_OP_BITWISE_XOR, t->t_u32, a, b); break;
            case SHL: x = op2(t, SPV_OP_SHIFT_LEFT_LOGICAL, t->t_u32, a, shift); break;
            case SHR: x = op2(t, SPV_OP_SHIFT_RIGHT_LOGICAL, t->t_u32, a, shift); break;
            case ASR: x = op2(t, SPV_OP_SHIFT_RIGHT_ARITHMETIC, t->t_u32, a, shift); break;
            default: x = rotate_left(t, a, b, bits); break;
        }
        store(t, dest, one_value(x, K_UINT), 1, repeat_offset(t, &dest, r, O_DEST, R_SLMSI));
    }
    reset_repeat_multiplier(t);
    return !t->failed;
}

/* ---- tests (branch_cond.cpp): the ALU operation, then its result compared with zero, or two sources compared ---- */
typedef enum {
    TO_INVALID, TO_ADD, TO_SUB, TO_FRC, TO_RCP, TO_RSQ, TO_LOG, TO_EXP, TO_DP, TO_MIN, TO_MAX, TO_DSX, TO_DSY, TO_MUL,
    TO_IADD, TO_ISUB, TO_IMUL, TO_AND, TO_OR, TO_XOR, TO_SHL, TO_SHR, TO_ROL, TO_ASR
} TestOp;
/* decode_test_inst: the operation and the type its sources are read in */
static TestOp test_op(unsigned alu_sel, unsigned alu_op, unsigned prec, Type* type) {
    static const TestOp vector[16] = {TO_INVALID, TO_INVALID, TO_ADD, TO_FRC, TO_RCP, TO_RSQ, TO_LOG, TO_EXP,
                                      TO_DP,      TO_MIN,     TO_MAX, TO_DSX, TO_DSY, TO_MUL, TO_SUB, TO_INVALID};
    static const TestOp ints[16] = {TO_INVALID, TO_INVALID, TO_INVALID, TO_INVALID, TO_INVALID, TO_INVALID,
                                    TO_IADD,    TO_ISUB,    TO_IMUL,    TO_IADD,    TO_ISUB,    TO_IMUL,
                                    TO_IADD,    TO_IADD,    TO_ISUB,    TO_ISUB};
    static const Type int_types[16] = {T_UNK, T_UNK, T_UNK, T_UNK, T_UNK, T_UNK, T_I16, T_I16,
                                       T_I16, T_U16, T_U16, T_U16, T_I32, T_U32, T_I32, T_U32};
    static const TestOp bytes[16] = {TO_IADD, TO_ISUB, TO_IADD, TO_ISUB, TO_IMUL, TO_INVALID, TO_IMUL, TO_INVALID,
                                     TO_ISUB, TO_INVALID, TO_INVALID, TO_INVALID, TO_INVALID, TO_INVALID, TO_INVALID,
                                     TO_INVALID};
    static const TestOp bitwise[16] = {TO_AND, TO_OR, TO_XOR, TO_SHL, TO_SHR, TO_ROL, TO_INVALID, TO_ASR,
                                       TO_INVALID, TO_INVALID, TO_INVALID, TO_INVALID, TO_INVALID, TO_INVALID,
                                       TO_INVALID, TO_INVALID};
    switch (alu_sel) {
        case 0: *type = prec ? T_F32 : T_F16; return vector[alu_op];
        case 1: *type = int_types[alu_op]; return ints[alu_op];
        case 2: *type = T_U8; return bytes[alu_op]; /* FPMUL8 and FPADD8 not yet */
        default: *type = T_U32; return bitwise[alu_op];
    }
}
/* The test of n components (mask) of src1 and src2 for each, as booleans: a subtraction compares the sources, any
   other operation compares its result with zero. zero_test 1 includes equality; sign_test 0 is (in)equality, 1 less
   than, 2 greater than (vtst_impl's table) */
static unsigned test(T* t, TestOp op, Operand src1, Operand src2, unsigned zero_test, unsigned sign_test,
                     unsigned mask, uint32_t out[4]) {
    if (sign_test == 3) return fail(t, "a test of sign 3"), 0;
    Value a = load(t, src1, mask, 0), b = load(t, src2, mask, 0), r = a;
    if (t->failed) return 0;
    int kind = a.kind;
    uint32_t type = kind_type(t, kind);
    if (op != TO_SUB && op != TO_ISUB) {
        for (unsigned i = 0; i < a.n; ++i) {
            uint32_t x = a.c[i], y = b.c[i];
            switch (op) {
                case TO_ADD: r.c[i] = op2(t, SPV_OP_F_ADD, type, x, y); break;
                case TO_MUL: r.c[i] = op2(t, SPV_OP_F_MUL, type, x, y); break;
                case TO_MIN: r.c[i] = ext2(t, type, SPV_GLSL_F_MIN, x, y); break;
                case TO_MAX: r.c[i] = ext2(t, type, SPV_GLSL_F_MAX, x, y); break;
                case TO_FRC: r.c[i] = op2(t, SPV_OP_F_SUB, type, x, ext1(t, type, SPV_GLSL_FLOOR, y)); break;
                case TO_DSX: r.c[i] = op1(t, SPV_OP_DPDX, type, x); break;
                case TO_DSY: r.c[i] = op1(t, SPV_OP_DPDY, type, x); break;
                case TO_RCP: r.c[i] = op2(t, SPV_OP_F_DIV, type, cf(t, 1), x); break;
                case TO_RSQ: r.c[i] = ext1(t, type, SPV_GLSL_INVERSE_SQRT, x); break;
                case TO_LOG: r.c[i] = ext1(t, type, SPV_GLSL_LOG2, x); break;
                case TO_EXP: r.c[i] = ext1(t, type, SPV_GLSL_EXP2, x); break;
                case TO_IADD: r.c[i] = op2(t, SPV_OP_I_ADD, type, x, y); break;
                case TO_IMUL: r.c[i] = op2(t, SPV_OP_I_MUL, type, x, y); break;
                case TO_AND: r.c[i] = op2(t, SPV_OP_BITWISE_AND, type, x, y); break;
                case TO_OR: r.c[i] = op2(t, SPV_OP_BITWISE_OR, type, x, y); break;
                case TO_XOR: r.c[i] = op2(t, SPV_OP_BITWISE_XOR, type, x, y); break;
                case TO_SHL: r.c[i] = op2(t, SPV_OP_SHIFT_LEFT_LOGICAL, type, x, y); break;
                case TO_SHR: r.c[i] = op2(t, SPV_OP_SHIFT_RIGHT_LOGICAL, type, x, y); break;
                case TO_ASR: r.c[i] = op2(t, SPV_OP_SHIFT_RIGHT_ARITHMETIC, type, x, y); break;
                case TO_ROL: r.c[i] = rotate_left(t, x, y, 32); break;
                default: break;
            }
        }
        if (op == TO_DP) { /* the sum of the products, compared once */
            uint32_t sum = op2(t, SPV_OP_F_MUL, type, a.c[0], b.c[0]);
            for (unsigned i = 1; i < a.n; ++i) sum = ext3(t, type, SPV_GLSL_FMA, a.c[i], b.c[i], sum);
            r.c[0] = sum, r.n = 1;
        }
        for (unsigned i = 0; i < r.n; ++i) b.c[i] = zero_of(t, kind);
    }
    static const uint32_t ops[3][2][3] = {
        {{SPV_OP_F_ORD_NOT_EQUAL, SPV_OP_F_ORD_LESS_THAN, SPV_OP_F_ORD_GREATER_THAN},
         {SPV_OP_F_ORD_EQUAL, SPV_OP_F_ORD_LESS_THAN_EQUAL, SPV_OP_F_ORD_GREATER_THAN_EQUAL}},
        {{SPV_OP_I_NOT_EQUAL, SPV_OP_S_LESS_THAN, SPV_OP_S_GREATER_THAN},
         {SPV_OP_I_EQUAL, SPV_OP_S_LESS_THAN_EQUAL, SPV_OP_S_GREATER_THAN_EQUAL}},
        {{SPV_OP_I_NOT_EQUAL, SPV_OP_U_LESS_THAN, SPV_OP_U_GREATER_THAN},
         {SPV_OP_I_EQUAL, SPV_OP_U_LESS_THAN_EQUAL, SPV_OP_U_GREATER_THAN_EQUAL}}};
    uint32_t compare = ops[kind == K_FLOAT ? 0 : kind == K_INT ? 1 : 2][zero_test == 1][sign_test];
    for (unsigned i = 0; i < r.n; ++i) out[i] = op2(t, compare, t->t_bool, r.c[i], b.c[i]);
    return r.n;
}
/* the sources of a test: 64-bit registers for the vector ALU, src2 one component for all when src2_vscomp */
static void test_sources(T* t, unsigned alu_sel, Type type, bool vscomp, Operand* src1, Operand* src2,
                         unsigned src1_n, unsigned src1_bank, unsigned src1_ext, unsigned src2_n, unsigned src2_bank,
                         unsigned src2_ext) {
    bool doubled = alu_sel == 0;
    *src1 = decode_src12(t, src1_n, src1_bank, src1_ext, doubled, doubled ? 8 : 7);
    *src2 = decode_src12(t, src2_n, src2_bank, src2_ext, doubled, doubled ? 8 : 7);
    src1->type = src2->type = type;
    if (vscomp && doubled) memset(src2->swizzle, SW_X, 4);
}
/* VTST (a test into a predicate register) */
static bool vtst(T* t, const arg_VTST* arg) {
    Type type;
    TestOp op = test_op(arg->alu_sel, arg->alu_op, arg->prec, &type);
    if (op == TO_INVALID || type == T_UNK) return fail(t, "VTST of ALU %u operation %u", arg->alu_sel, arg->alu_op);
    if (arg->chan >= 4) return fail(t, "VTST of channel %u", arg->chan);
    Operand src1, src2;
    test_sources(t, arg->alu_sel, type, arg->vscomp, &src1, &src2, arg->src1_n, arg->src1_bank, arg->src1_ext,
                 arg->src2_n, arg->src2_bank, arg->src2_ext);
    if (arg->src1_neg) src1.flags |= F_NEG;
    uint32_t result[4];
    if (!test(t, op, src1, src2, arg->zero_test, arg->sign_test, 1u << arg->chan, result)) return false;
    store_predicate(t, arg->pdst_n, result[0]);
    return true;
}
/* VTSTMSK (a test's result as a mask into a register: all ones or zero) */
static bool vtstmsk(T* t, const arg_VTSTMSK* arg) {
    Type type;
    TestOp op = test_op(arg->alu_sel, arg->alu_op, arg->prec, &type);
    if (op == TO_INVALID || type == T_UNK) return fail(t, "VTSTMSK of ALU %u operation %u", arg->alu_sel, arg->alu_op);
    if (arg->mask_type == 3) return fail(t, "VTSTMSK of mask type 3");
    if (!arg->wben) return fail(t, "VTSTMSK without its write");
    Operand src1, src2;
    test_sources(t, arg->alu_sel, type, arg->vscomp, &src1, &src2, arg->src1_n, arg->src1_bank, arg->src1_ext,
                 arg->src2_n, arg->src2_bank, arg->src2_ext);
    if (arg->src1_neg) src1.flags |= F_NEG;
    bool doubled = arg->alu_sel == 0, four = doubled && arg->mask_type == 2;
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, doubled, doubled ? 8 : 7);
    dest.type = type;
    uint32_t result[4];
    unsigned n = test(t, op, src1, src2, arg->zero_test, arg->sign_test, op == TO_DP || four ? 15 : 1, result);
    if (!n) return false;
    int kind = type_kind(type);
    uint32_t ones, zero = zero_of(t, kind);
    switch (type) {
        case T_F16: case T_F32: ones = cf(t, 1); break;
        case T_I8: ones = ci(t, 0x7f); break;
        case T_I16: ones = ci(t, 0x7fff); break;
        case T_I32: ones = ci(t, 0x7fffffff); break;
        case T_U8: ones = cu(t, 0xff); break;
        case T_U16: ones = cu(t, 0xffff); break;
        default: ones = cu(t, 0xffffffffu); break;
    }
    Value v = {{0}, n, kind};
    for (unsigned i = 0; i < n; ++i) v.c[i] = SPV(&t->e, SPV_OP_SELECT, kind_type(t, kind), result[i], ones, zero);
    store(t, dest, v, 15, 0); /* a single result in each component, as Vita3K stores it */
    return !t->failed;
}

/* ---- the scalar (8-bit color) operations ---- */
static uint32_t sop_apply(T* t, unsigned op, uint32_t a, uint32_t b) {
    switch (op) {
        case 0: return op2(t, SPV_OP_F_ADD, t->t_f32, a, b);
        case 1: return op2(t, SPV_OP_F_SUB, t->t_f32, a, b);
        case 2: return ext2(t, t->t_f32, SPV_GLSL_F_MIN, a, b);
        default: return ext2(t, t->t_f32, SPV_GLSL_F_MAX, a, b);
    }
}
static uint32_t complement(T* t, uint32_t x, unsigned enable) {
    return enable ? op2(t, SPV_OP_F_SUB, t->t_f32, cf(t, 1), x) : x;
}
/* SOP2 (alu.cpp). Each source's color (xyz) and alpha (w) as unsigned bytes, scaled by a factor each selects, then
   added, subtracted or the min or max */
static bool sop2(T* t, const arg_SOP2* arg) {
    if (arg->csel1 > 4 || arg->csel2 > 4 || arg->asel1 > 2 || arg->asel2 > 2)
        return fail(t, "SOP2 selectors %u %u %u %u", arg->csel1, arg->csel2, arg->asel1, arg->asel2);
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, false, 7);
    Operand src2 = decode_src12(t, arg->src2_n, arg->src2_bank, arg->src2_ext, false, 7);
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, false, 7);
    src1.type = src2.type = dest.type = T_U8;
    for (int r = 0; r <= arg->count && !t->failed; ++r) {
        int o1 = repeat_offset(t, &src1, r, O_SRC1, R_SLMSI), o2 = repeat_offset(t, &src2, r, O_SRC2, R_SLMSI);
        Value c1 = to_float(t, load(t, src1, 7, o1), T_U8, true), c2 = to_float(t, load(t, src2, 7, o2), T_U8, true);
        Value a1 = to_float(t, load(t, src1, 8, o1), T_U8, true), a2 = to_float(t, load(t, src2, 8, o2), T_U8, true);
        if (t->failed) break;
        for (unsigned i = 0; i < 3; ++i) c1.c[i] = complement(t, c1.c[i], arg->src1_mod);
        a1.c[0] = complement(t, a1.c[0], arg->src1_mod);
        Value color = {{0}, 3, K_FLOAT};
        for (unsigned i = 0; i < 3; ++i) {
            uint32_t pick[5] = {cf(t, 0), c1.c[i], c2.c[i], a1.c[0], a2.c[0]};
            uint32_t lhs = op2(t, SPV_OP_F_MUL, t->t_f32, complement(t, pick[arg->csel1], arg->cmod1), c1.c[i]);
            uint32_t rhs = op2(t, SPV_OP_F_MUL, t->t_f32, complement(t, pick[arg->csel2], arg->cmod2), c2.c[i]);
            color.c[i] = sop_apply(t, arg->cop, lhs, rhs);
        }
        uint32_t apick[3] = {cf(t, 0), a1.c[0], a2.c[0]};
        uint32_t lhs = op2(t, SPV_OP_F_MUL, t->t_f32, complement(t, apick[arg->asel1], arg->amod1), a1.c[0]);
        uint32_t rhs = op2(t, SPV_OP_F_MUL, t->t_f32, complement(t, apick[arg->asel2], arg->amod2), a2.c[0]);
        Value alpha = one_value(sop_apply(t, arg->aop, lhs, rhs), K_FLOAT);
        int od = repeat_offset(t, &dest, r, O_DEST, R_SLMSI);
        store(t, dest, to_int(t, color, T_U8, true), 7, od);
        store(t, dest, to_int(t, alpha, T_U8, true), 8, od);
    }
    return !t->failed;
}
/* SOP2M (with a write mask, alpha its first bit) */
static bool sop2m(T* t, const arg_SOP2M* arg) {
    unsigned wmask = arg->wmask;
    /* zero, -, src1, src1's alpha, -, -, src2, src2's alpha */
    if (arg->sel1 == 1 || arg->sel1 == 4 || arg->sel1 == 5 || arg->sel2 == 1 || arg->sel2 == 4 || arg->sel2 == 5)
        return fail(t, "SOP2M selectors %u %u", arg->sel1, arg->sel2);
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, false, 7);
    Operand src2 = decode_src12(t, arg->src2_n, arg->src2_bank, arg->src2_ext, false, 7);
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, false, 7);
    src1.type = src2.type = dest.type = T_U8;
    Value a = to_float(t, load(t, src1, 15, 0), T_U8, true), b = to_float(t, load(t, src2, 15, 0), T_U8, true);
    if (t->failed) return false;
    wmask = ((wmask & 14) >> 1) | ((wmask & 1) << 3);
    Value r = {{0}, 0, K_FLOAT};
    for (unsigned i = 0; i < 4; ++i) {
        if (!(wmask & (1u << i))) continue;
        uint32_t s1[8] = {cf(t, 0), 0, a.c[i], a.c[3], 0, 0, b.c[i], b.c[3]};
        uint32_t x = op2(t, SPV_OP_F_MUL, t->t_f32, complement(t, s1[arg->sel1], arg->mod1), a.c[i]);
        uint32_t y = op2(t, SPV_OP_F_MUL, t->t_f32, complement(t, s1[arg->sel2], arg->mod2), b.c[i]);
        r.c[r.n++] = sop_apply(t, i == 3 ? arg->aop : arg->cop, x, y);
    }
    store(t, dest, to_int(t, r, T_U8, true), wmask, 0);
    return !t->failed;
}

/* ---- integer multiply-adds (ialu.cpp) ---- */
static Value imad(T* t, Value a, Value b, Value c) {
    uint32_t type = kind_type(t, a.kind);
    if (b.kind != a.kind) b.c[0] = bitcast(t, type, b.c[0]);
    if (c.kind != a.kind) c.c[0] = bitcast(t, type, c.c[0]);
    return one_value(op2(t, SPV_OP_I_ADD, type, op2(t, SPV_OP_I_MUL, type, a.c[0], b.c[0]), c.c[0]), a.kind);
}
/* I32MAD (16-bit sources, a 32-bit result) */
static bool i32mad(T* t, const arg_I32MAD* arg) {
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, false, 7);
    Operand src0 = decode_src0(t, arg->src0_n, arg->src0_bank, false, false, 7);
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, false, 7);
    Operand src2 = decode_src12(t, arg->src2_n, arg->src2_bank, arg->src2_ext, false, 7);
    Type wide = arg->is_signed ? T_I32 : T_U32, narrow = arg->is_signed ? T_I16 : T_U16;
    dest.type = wide, src0.type = src1.type = narrow, src2.type = arg->src2_type == 2 ? wide : narrow;
    unsigned m0 = arg->src0_high ? 2 : 1, m1 = arg->src1_high ? 2 : 1;
    unsigned m2 = arg->src2_type != 2 && arg->src2_high ? 2 : 1;
    set_multiplier(t, 1, 1, 1, 1);
    for (int r = 0; r <= arg->repeat_count && !t->failed; ++r) {
        Value a = load(t, src0, m0, repeat_offset(t, &src0, r, O_SRC0, R_SLMSI));
        Value b = load(t, src1, m1, repeat_offset(t, &src1, r, O_SRC1, R_SLMSI));
        Value c = load(t, src2, m2, repeat_offset(t, &src2, r, O_SRC2, R_SLMSI));
        if (t->failed) break;
        store(t, dest, imad(t, a, b, c), 1, repeat_offset(t, &dest, r, O_DEST, R_SLMSI));
    }
    reset_repeat_multiplier(t);
    return !t->failed;
}
static bool i16mad(T* t, const arg_I16MAD* arg) {
    bool is_signed = arg->mode >= 2;
    Type type = is_signed ? T_I16 : T_U16, bytes = is_signed ? T_I8 : T_U8;
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, false, 7);
    Operand src0 = decode_src0(t, arg->src0_n, arg->src0_bank, false, false, 7);
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, false, 7);
    Operand src2 = decode_src12(t, arg->src2_n, arg->src2_bank, arg->src2_ext, false, 7);
    dest.type = src0.type = src1.type = src2.type = type;
    unsigned m1 = 1, m2 = 1;
    if (arg->src1_format > 1 || arg->src2_format > 1) return fail(t, "I16MAD of sign-extended bytes");
    if (arg->src1_format) src1.type = bytes, m1 = arg->sel1h ? 2 : 1;
    if (arg->src2_format) src2.type = bytes, m2 = arg->sel2h ? 2 : 1;
    for (int r = 0; r <= arg->repeat_count && !t->failed; ++r) {
        Value a = load(t, src0, 1, repeat_offset(t, &src0, r, O_SRC0, R_SLMSI));
        Value b = load(t, src1, m1, repeat_offset(t, &src1, r, O_SRC1, R_SLMSI));
        Value c = load(t, src2, m2, repeat_offset(t, &src2, r, O_SRC2, R_SLMSI));
        if (t->failed) break;
        store(t, dest, imad(t, a, b, c), 1, repeat_offset(t, &dest, r, O_DEST, R_SLMSI));
    }
    return !t->failed;
}
/* I32MAD2 (32-bit sources) */
static bool i32mad2(T* t, const arg_I32MAD2* arg) {
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, false, 7);
    Operand src0 = decode_src0(t, arg->src0_n, arg->src0_bank, arg->src0_ext, false, 7);
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, false, 7);
    Operand src2 = decode_src12(t, arg->src2_n, arg->src2_bank, arg->src2_ext, false, 7);
    dest.type = src0.type = src1.type = src2.type = arg->is_signed ? T_I32 : T_U32;
    if (arg->neg1) src1.flags |= F_NEG;
    if (arg->neg2) src2.flags |= F_NEG;
    set_multiplier(t, 1, 1, 1, 1);
    for (int r = 0; r <= arg->count && !t->failed; ++r) {
        Value a = load(t, src0, 1, repeat_offset(t, &src0, r, O_SRC0, R_SLMSI));
        Value b = load(t, src1, 1, repeat_offset(t, &src1, r, O_SRC1, R_SLMSI));
        Value c = load(t, src2, 1, repeat_offset(t, &src2, r, O_SRC2, R_SLMSI));
        if (t->failed) break;
        /* arg->sn 1 passes src2 through (Vita3K's hardware tests) */
        store(t, dest, arg->sn ? c : imad(t, a, b, c), 1, repeat_offset(t, &dest, r, O_DEST, R_SLMSI));
    }
    reset_repeat_multiplier(t);
    return !t->failed;
}

/* ---- loads (data.cpp) ---- */
/* VLDST: a load of 32-bit words from a uniform buffer read from memory, at src0 + src1 (+ 4 without moe expansion) +
   src2 bytes, src0 a register the program's uniform buffer table filled; one from the literal buffer, constants from
   the program */
static bool vldst(T* t, const arg_VLDST* arg) {
    if (arg->op != 1) return fail(t, arg->op == 2 ? "VLDST stores" : "VLDST operation %u", arg->op);
    if (arg->data_type > 2) return fail(t, "VLDST of data type %u", arg->data_type);
    unsigned size = arg->data_type == 0 ? 4 : arg->data_type == 1 ? 2 : 1, bytes = size * (arg->mask_count + 1);
    Operand src0 = decode_src0(t, arg->src0_n, arg->src0_bank, arg->src0_ext, false, 7);
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, false, 7);
    Operand src2 = decode_src12(t, arg->src2_n, arg->src2_bank, arg->src2_ext, false, 7);
    src0.type = src1.type = src2.type = T_I32;
    Bank bank = t->secondary ? B_SA : arg->primattr ? B_PA : B_TEMP;
    if (src1.bank == B_IMM) src1.num *= size;
    if (src0.bank == B_SA && (int)src0.num == t->literal_register) {
        if (arg->mask_count || src1.bank != B_IMM || src2.bank != B_IMM) { /* at a register's offset: from the array */
            if (!t->literals) return fail(t, "a literal buffer read past the program");
            uint32_t b1 = load(t, src1, 1, 0).c[0], b2 = load(t, src2, 1, 0).c[0];
            if (t->failed) return false;
            if (src1.bank != B_IMM) b1 = op2(t, SPV_OP_I_SUB, t->t_i32, b1, ci(t, 0x10000));
            uint32_t offset = op2(t, SPV_OP_I_ADD, t->t_i32, ci(t, (int32_t)t->literal_base), op2(t, SPV_OP_I_ADD, t->t_i32, b1, b2));
            uint32_t first = op2(t, SPV_OP_SHIFT_RIGHT_LOGICAL, t->t_u32, bitcast(t, t->t_u32, offset), cu(t, 2));
            uint32_t pointer = spv_emit_type_pointer(&t->e, SPV_STORAGE_PRIVATE, t->t_u32);
            for (unsigned i = 0; i < bytes / 4; ++i) {
                uint32_t at = ext2(t, t->t_u32, SPV_GLSL_U_MIN, op2(t, SPV_OP_I_ADD, t->t_u32, first, cu(t, i)),
                                   cu(t, t->literal_words - 1));
                store_word(t, bank, arg->dest_n + i, SPV(&t->e, SPV_OP_LOAD, t->t_u32,
                                                    SPV(&t->e, SPV_OP_ACCESS_CHAIN, pointer, t->literals, at)));
            }
            return !t->failed;
        }
        uint32_t offset = t->literal_base + src1.num + src2.num;
        const unsigned char* data = gxp_at(t->gxp, GXP_LITERAL_DATA);
        if ((size_t)(data - t->gxp) + offset + 4 > t->size) return fail(t, "a literal past the program");
        store_word(t, bank, arg->dest_n, cu(t, snx_ld_le32(data + offset)));
        return !t->failed;
    }
    if (!t->buffer) return fail(t, "a VLDST with no uniform buffer");
    uint32_t a0 = load(t, src0, 1, 0).c[0], a1 = load(t, src1, 1, 0).c[0], a2 = load(t, src2, 1, 0).c[0];
    if (t->failed) return false;
    if (src1.bank != B_IMM) a1 = op2(t, SPV_OP_I_SUB, t->t_i32, a1, ci(t, 0x10000));
    if (!arg->moe_expand) a1 = op2(t, SPV_OP_I_ADD, t->t_i32, a1, ci(t, 4));
    uint32_t address = bitcast(t, t->t_u32, op2(t, SPV_OP_I_ADD, t->t_i32, a0, op2(t, SPV_OP_I_ADD, t->t_i32, a1, a2)));
    uint32_t index = op2(t, SPV_OP_SHIFT_RIGHT_LOGICAL, t->t_u32, address, cu(t, 2));
    uint32_t pointer = spv_emit_type_pointer(&t->e, SPV_STORAGE_STORAGE_BUFFER, t->t_u32);
    for (unsigned i = 0; i < bytes / 4; ++i) {
        uint32_t at = op2(t, SPV_OP_I_ADD, t->t_u32, index, cu(t, i));
        uint32_t w = SPV(&t->e, SPV_OP_LOAD, t->t_u32, SPV(&t->e, SPV_OP_ACCESS_CHAIN, pointer, t->buffer, cu(t, 0), at));
        store_word(t, bank, arg->dest_n + i, w);
    }
    return !t->failed;
}
/* LIMM (a 32-bit immediate into a register) */
static bool limm(T* t, const arg_LIMM* arg) {
    Operand dest = decode_dest(t, arg->dest_n, arg->dest_bank, arg->dest_ext, false, 7);
    dest.type = T_U32;
    store(t, dest, one_value(cu(t, arg->imm), K_UINT), 1, 0);
    return !t->failed;
}

/* ---- dependent texture reads (texture.cpp) ---- */
/* SMP's gather4: the four texels' components pixel by pixel, four a register group; with uv, then the four bilinear
   weights as halves, in the order the SDK's shaders read them ((1-u)(1-v), u(1-v), uv, (1-u)v) */
static bool gather(T* t, uint32_t sampler, uint32_t coord, uint32_t format, Type type, Bank bank, unsigned dest,
                   bool uv) {
    unsigned n = texture_components(format);
    uint32_t texels[16];
    for (unsigned c = 0; c < n; ++c) {
        uint32_t g = SPV(&t->e, SPV_OP_IMAGE_GATHER, t->t_v4f, sampler, coord, ci(t, (int32_t)c));
        for (unsigned pixel = 0; pixel < 4; ++pixel)
            texels[pixel * n + c] = SPV(&t->e, SPV_OP_COMPOSITE_EXTRACT, t->t_f32, g, pixel);
    }
    unsigned step = type_size(type); /* registers four components take */
    for (unsigned at = 0; at < 4 * n; at += 4, dest += step) {
        Value v = {{texels[at], texels[at + 1], texels[at + 2], texels[at + 3]}, 4, K_FLOAT};
        store_input(t, bank, dest, type, v);
    }
    if (!uv) return !t->failed;
    spv_emit_image_query_capability(&t->e);
    uint32_t v2i = spv_emit_type_vector(&t->e, t->t_i32, 2);
    uint32_t lod = t->fragment ? SPV(&t->e, SPV_OP_COMPOSITE_EXTRACT, t->t_f32,
                                     SPV(&t->e, SPV_OP_IMAGE_QUERY_LOD, t->t_v2f, sampler, coord), 0)
                               : cf(t, 0);
    uint32_t image_type = spv_emit_type_image(&t->e, t->t_f32, SPV_DIM_2D, 0, 0, 0, 1, SPV_IMAGE_FORMAT_UNKNOWN);
    uint32_t image = op1(t, SPV_OP_IMAGE, image_type, sampler);
    uint32_t size = SPV(&t->e, SPV_OP_IMAGE_QUERY_SIZE_LOD, v2i, image, op1(t, SPV_OP_CONVERT_F_TO_S, t->t_i32, lod));
    uint32_t texel = op2(t, SPV_OP_F_MUL, t->t_v2f, coord, op1(t, SPV_OP_CONVERT_S_TO_F, t->t_v2f, size));
    uint32_t half = SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v2f, cf(t, 0.5f), cf(t, 0.5f));
    uint32_t f = ext1(t, t->t_v2f, SPV_GLSL_FRACT, op2(t, SPV_OP_F_SUB, t->t_v2f, texel, half));
    uint32_t u = SPV(&t->e, SPV_OP_COMPOSITE_EXTRACT, t->t_f32, f, 0), w = SPV(&t->e, SPV_OP_COMPOSITE_EXTRACT, t->t_f32, f, 1);
    uint32_t nu = op2(t, SPV_OP_F_SUB, t->t_f32, cf(t, 1), u), nw = op2(t, SPV_OP_F_SUB, t->t_f32, cf(t, 1), w);
    Value weights = {{op2(t, SPV_OP_F_MUL, t->t_f32, nu, nw), op2(t, SPV_OP_F_MUL, t->t_f32, u, nw),
                      op2(t, SPV_OP_F_MUL, t->t_f32, u, w), op2(t, SPV_OP_F_MUL, t->t_f32, nu, w)},
                     4, K_FLOAT};
    store_input(t, bank, dest, T_F16, weights);
    return !t->failed;
}
/* SMP: the texture is the dependent sampler src1 names, read at src0 (one, two, or three components), its LOD from
   src2 when lod_mode says */
static bool smp(T* t, const arg_SMP* arg) {
    unsigned dim = arg->dim + 1;
    Operand src0 = decode_src0(t, arg->src0_n, arg->src0_bank, arg->src0_ext, true, 8);
    Operand src1 = decode_src12(t, arg->src1_n, arg->src1_bank, arg->src1_ext, true, 8);
    src0.type = arg->src0_type == 0 ? T_F32 : arg->src0_type == 1 ? T_F16 : T_C10;
    if (src1.bank != B_SA || src1.num >= BANK_WORDS || !t->dependent[src1.num])
        return fail(t, "an SMP whose sampler isn't a dependent one");
    if (arg->sb_mode == 2) return fail(t, "SMP queries");
    if (dim > 3) return fail(t, "SMP dimension %u", dim);
    unsigned resource = t->dependent[src1.num] - 1u;
    if (resource >= USSE_TEXTURES) return fail(t, "SMP sampler %u", resource);
    uint32_t format = t->hints->texture_formats[resource];
    Type type = arg->fconv == 2 ? T_F16 : arg->fconv == 3 ? T_F32 : texture_component_type(format);
    Value c = load(t, src0, dim == 1 ? 1 : dim == 2 ? 3 : 7, 0);
    if (t->failed) return false;
    if (c.kind != K_FLOAT) c = to_float(t, c, src0.type, true);
    uint32_t coord = dim == 3 ? SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v3f, c.c[0], c.c[1], c.c[2])
                              : SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v2f, c.c[0], dim == 1 ? cf(t, 0) : c.c[1]);
    unsigned sampler_dim = dim == 3 ? SPV_DIM_CUBE : SPV_DIM_2D;
    uint32_t sampler = sampler_of(t, resource, sampler_dim), sampled;
    Bank bank = arg->use_pa ? (t->secondary ? B_SA : B_PA) : B_TEMP;
    if (arg->sb_mode == 3 && dim == 3) return fail(t, "SMP cube gather with UV coefficients");
    if (arg->sb_mode) /* the four texels' components: each component of the four, then the next component's */
        return gather(t, sampler, coord, format, type, bank, arg->dest_n, arg->sb_mode == 3);
    Operand src2 = decode_src12(t, arg->src2_n, arg->src2_bank, arg->src2_ext, true, 8);
    src2.type = src0.type;
    bool implicit = t->fragment && (arg->lod_mode == 0 || arg->lod_mode == 1);
    if (arg->lod_mode == 3) { /* gradients */
        Value dx = dim == 3 ? load(t, src2, 7, 0) : load(t, src2, 3, 0);
        Value dy = dim == 3 ? load(t, src2, 7, 1) : load(t, src2, 12, 0);
        if (t->failed) return false;
        uint32_t gx = dim == 3 ? SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v3f, dx.c[0], dx.c[1], dx.c[2])
                               : SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v2f, dx.c[0], dx.c[1]);
        uint32_t gy = dim == 3 ? SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v3f, dy.c[0], dy.c[1], dy.c[2])
                               : SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v2f, dy.c[0], dy.c[1]);
        sampled =
            SPV(&t->e, SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD, t->t_v4f, sampler, coord, SPV_IMAGE_OPERANDS_GRAD, gx, gy);
    } else if (arg->lod_mode) {
        uint32_t lod = load(t, src2, 1, 0).c[0];
        if (t->failed) return false;
        sampled = implicit ? SPV(&t->e, SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, t->t_v4f, sampler, coord,
                                 SPV_IMAGE_OPERANDS_BIAS, lod)
                           : SPV(&t->e, SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD, t->t_v4f, sampler, coord,
                                 SPV_IMAGE_OPERANDS_LOD, arg->lod_mode == 1 ? cf(t, 0) : lod);
    } else
        sampled = implicit ? SPV(&t->e, SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, t->t_v4f, sampler, coord)
                           : SPV(&t->e, SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD, t->t_v4f, sampler, coord,
                                 SPV_IMAGE_OPERANDS_LOD, cf(t, 0));
    store_input(t, bank, arg->dest_n, type, vector_value(t, sampled, texture_components(format), K_FLOAT));
    return !t->failed;
}

/* ---- the special instructions (special.cpp) ---- */
/* KILL: the fragment is discarded (predicated, the program's if around it) */
static bool kill(T* t) {
    if (!t->fragment) return fail(t, "KILL in a vertex program");
    spv_emit_void_op(&t->e, SPV_OP_KILL, NULL, 0);
    t->terminated = 1;
    return true;
}
/* DEPTHF: src0 is the fragment's depth */
static bool depthf(T* t, const arg_DEPTHF* arg) {
    Operand src0 = decode_src0(t, arg->src0_n, arg->src0_bank, arg->src0_ext, true, 7);
    src0.type = T_F32;
    if (!t->frag_depth) return fail(t, "DEPTHF in a program that doesn't say it writes depth");
    uint32_t depth = load(t, src0, 1, 0).c[0];
    if (!t->failed) SPV_VOID(&t->e, SPV_OP_STORE, t->frag_depth, depth);
    return !t->failed;
}
/* SMBO: the repeats' base offsets */
static bool smbo(T* t, const arg_SMBO* arg) {
    unsigned offsets[4] = {arg->src0_offset, arg->src1_offset, arg->src2_offset, arg->dest_offset};
    for (unsigned k = 0; k < 4; ++k)
        for (int i = 0; i < 17; ++i) t->repeat_increase[k][i] = i + (int)offsets[k];
    return true;
}

/* the predicate of an instruction (usse_program_analyzer.cpp: get_predicate): 0 none, else ExtPredicate's
   P0 to P3 (1 to 4), NEGP0 and NEGP1 (5, 6), PN (7); 8 for one this translator doesn't know */
static unsigned predicate(uint64_t inst) {
    unsigned op = (unsigned)(inst >> 59), high = (unsigned)(inst >> 32);
    static const unsigned short_to_ext[4] = {0, 1, 2, 5}; /* NONE, P0, P1, NEGP0 */
    switch (op) {
        case 0: return (high >> 24) & 3; /* VMAD2 */
        case 1: case 2: case 3: { /* V32NMAD, V16NMAD, VMAD: ExtVecPredicate */
            static const unsigned map[8] = {0, 1, 2, 3, 5, 6, 8, 0};
            return map[(high >> 24) & 7];
        }
        case 4: case 5: { /* VDUAL: a short vector predicate */
            static const unsigned map[4] = {0, 1, 5, 7};
            return map[(high >> 24) & 3];
        }
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: /* SOP2, SOP2M, SOP3, I8/I16/I32MAD */
            return short_to_ext[(high >> 25) & 3];
        case 0x1f: {
            unsigned category = (high >> 20) & 3, extra = (high >> 22) & 1, op2 = (high >> 24) & 7;
            if (category == 0) return extra ? 0 : (high >> 24) & 7; /* PHAS, or BR */
            if (category == 3 && op2 == 1) { /* KILL: the short predicate's opposite */
                static const unsigned map[4] = {0, 5, 6, 1};
                return map[(high >> 9) & 3];
            }
            if (category == 2 && op2 == 4) return (high >> 9) & 7;                /* LIMM */
            if (category == 3 && op2 == 3) return short_to_ext[(high >> 9) & 3]; /* DEPTHF */
            return 0;
        }
        default: return (high >> 24) & 7;
    }
}
static uint32_t predicate_value(T* t, unsigned pred) {
    bool negate = pred >= 5;
    unsigned n = negate ? pred - 5 : pred - 1;
    uint32_t pointer = spv_emit_type_pointer(&t->e, SPV_STORAGE_PRIVATE, t->t_bool);
    uint32_t v = SPV(&t->e, SPV_OP_LOAD, t->t_bool, SPV(&t->e, SPV_OP_ACCESS_CHAIN, pointer, t->bank[B_PRED], cu(t, n)));
    return negate ? op1(t, SPV_OP_LOGICAL_NOT, t->t_bool, v) : v;
}
/* one instruction's predicate, before its handler: an if around what the handler emits when the instruction is
   predicated. False when there is nothing to translate (a survey only names the instruction) or the predicate can't
   be translated */
static bool instruction_begin(T* t, const char* name, bool predicated) {
    t->name = name, t->known = true, t->merge = 0;
    if (t->survey) return false;
    unsigned pred = predicated ? predicate(t->instr) : 0;
    if (pred == 7) return fail(t, "the PN predicate (%s at %u)", name, t->pc);
    if (pred == 8) return fail(t, "the NEGP2 predicate (%s at %u)", name, t->pc);
    if (!pred) return true;
    uint32_t yes = spv_emit_id(&t->e), merge = spv_emit_id(&t->e);
    uint32_t cond = predicate_value(t, pred);
    SPV_VOID(&t->e, SPV_OP_SELECTION_MERGE, merge, SPV_SELECTION_CONTROL_NONE);
    SPV_VOID(&t->e, SPV_OP_BRANCH_CONDITIONAL, cond, yes, merge);
    spv_emit_label(&t->e, yes);
    t->merge = merge;
    return true;
}
/* after the handler: the predicate's if closed, or a new block after an instruction that ended its own */
static void instruction_end(T* t) {
    if (t->merge) {
        if (!t->terminated) SPV_VOID(&t->e, SPV_OP_BRANCH, t->merge);
        spv_emit_label(&t->e, t->merge);
    } else if (t->terminated)
        spv_emit_label(&t->e, spv_emit_id(&t->e)); /* what follows is unreachable */
}
/* an instruction this translator doesn't have: the program fails */
static bool untranslated(T* t, const char* name) {
    t->name = name, t->known = false;
    if (!t->survey) fail(t, "%s (%016llx at %u) is not translated yet", name, (unsigned long long)t->instr, t->pc);
    return true;
}

/* the trans_ function of each pattern of usse.decode, which decode_usse calls: the handler of the instruction, inside
   its predicate (PREDICATED: whether the instruction has one). Each answers true, so a { } group of usse.decode never
   tries a later pattern for a word that an earlier one matched, as Vita3K's first match */
#define TRANS(NAME, PREDICATED, CALL)                                                                                  \
    static bool trans_##NAME(T* t, arg_##NAME* arg) {                                                                  \
        (void)arg;                                                                                                     \
        if (instruction_begin(t, #NAME, PREDICATED)) {                                                                 \
            CALL;                                                                                                      \
            instruction_end(t);                                                                                        \
        }                                                                                                              \
        return true;                                                                                                   \
    }
#define UNTRANSLATED(NAME)                                                                                             \
    static bool trans_##NAME(T* t, arg_##NAME* arg) { return (void)arg, untranslated(t, #NAME); }
TRANS(VMAD2, true, vmad2(t, arg))
TRANS(V32NMAD, true, vnmad(t, arg, true))
TRANS(V16NMAD, true, vnmad(t, arg, false))
TRANS(VMAD, true, vmad(t, arg))
TRANS(VDP, true, vdp(t, arg))
TRANS(VDUAL, true, vdual(t, arg))
TRANS(VCOMP, true, vcomp(t, arg))
TRANS(VMOV, true, vmov(t, arg))
TRANS(VPCK, true, vpck(t, arg))
TRANS(VTST, true, vtst(t, arg))
TRANS(VTSTMSK, true, vtstmsk(t, arg))
TRANS(VBW, true, vbw(t, arg))
TRANS(SOP2, true, sop2(t, arg))
TRANS(SOP2M, true, sop2m(t, arg))
UNTRANSLATED(SOP3)
UNTRANSLATED(I8MAD)
TRANS(I16MAD, true, i16mad(t, arg))
TRANS(I32MAD, true, i32mad(t, arg))
UNTRANSLATED(ILLEGAL22)
UNTRANSLATED(ILLEGAL23)
UNTRANSLATED(ILLEGAL24)
UNTRANSLATED(I8MAD2)
TRANS(I32MAD2, true, i32mad2(t, arg))
UNTRANSLATED(ILLEGAL27)
TRANS(SMP, true, smp(t, arg))
TRANS(PHAS, false, (void)0)
TRANS(NOP, false, (void)0)
TRANS(BR, false, (void)0) /* translate_code makes the branches of the program's blocks */
TRANS(SMLSI, false, smlsi(t, arg))
TRANS(SMBO, true, smbo(t, arg))
TRANS(KILL, true, kill(t))
TRANS(LIMM, true, limm(t, arg))
TRANS(DEPTHF, true, depthf(t, arg))
TRANS(SPEC, false, (void)0)
TRANS(VLDST, true, vldst(t, arg))

/* one instruction: its handler, in an if when it is predicated. False when the program can't be translated */
static bool translate_one(T* t, uint64_t w, unsigned pc) {
    t->instr = w, t->pc = pc, t->terminated = 0;
    if (!decode_usse(t, w)) return fail(t, "instruction %016llx (at %u) matches no pattern", (unsigned long long)w, pc);
    return !t->failed;
}
static uint64_t instruction(const unsigned char* code, uint32_t pc) {
    return snx_ld_le64(code + pc * 8);
}
/* a BR (usse_program_analyzer.cpp: is_branch): its target, relative to it */
static bool branch(uint64_t w, int32_t* offset) {
    uint32_t high = (uint32_t)(w >> 32);
    if ((high >> 27) != 0x1f || ((high >> 20) & 3) || (high & 0x00400000u) || ((high >> 6) & 7) > 1) return false;
    int32_t off = (int32_t)(w & 0xfffffu);
    if ((w & (1ull << 38)) && (off & (1 << 19))) off |= (int32_t)0xfff00000u;
    *offset = off;
    return true;
}
static int by_value(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}
/* One program (the primary or the secondary): its instructions in order. One with branches is a loop over its
   basic blocks, a switch on the next block's first instruction (pc, a Function variable) taking one block a turn,
   which SPIR-V's structured control flow holds whatever the branches do */
static void translate_code(T* t, const unsigned char* code, uint32_t count) {
    uint32_t* leaders = malloc((2 * (size_t)count + 1) * sizeof *leaders);
    size_t n = 0;
    if (!leaders) {
        fail(t, "out of memory");
        return;
    }
    leaders[n++] = 0;
    for (uint32_t pc = 0; pc < count; ++pc) {
        int32_t off;
        if (!branch(instruction(code, pc), &off)) continue;
        if ((instruction(code, pc) >> 41) & 1) {
            free(leaders);
            fail(t, "a BR that calls (at %u)", pc);
            return;
        }
        int64_t target = (int64_t)pc + off;
        if (target < 0 || target > count) {
            free(leaders);
            fail(t, "a BR to %lld (at %u)", (long long)target, pc);
            return;
        }
        leaders[n++] = (uint32_t)target;
        leaders[n++] = pc + 1;
    }
    if (n == 1) { /* straight-line code */
        free(leaders);
        for (uint32_t pc = 0; pc < count && translate_one(t, instruction(code, pc), pc); ++pc) continue;
        return;
    }
    qsort(leaders, n, sizeof *leaders, by_value);
    size_t unique = 0;
    for (size_t i = 0; i < n; ++i)
        if (leaders[i] < count && (!unique || leaders[unique - 1] != leaders[i])) leaders[unique++] = leaders[i];
    SpvEmitter* e = &t->e;
    uint32_t pcv = spv_emit_local(e, t->t_u32);
    SPV_VOID(e, SPV_OP_STORE, pcv, cu(t, 0));
    uint32_t header = spv_emit_id(e), body = spv_emit_id(e), cont = spv_emit_id(e), merge = spv_emit_id(e);
    uint32_t cases_merge = spv_emit_id(e);
    SPV_VOID(e, SPV_OP_BRANCH, header);
    spv_emit_label(e, header);
    SPV_VOID(e, SPV_OP_LOOP_MERGE, merge, cont, SPV_LOOP_CONTROL_NONE);
    SPV_VOID(e, SPV_OP_BRANCH, body);
    spv_emit_label(e, body);
    uint32_t pc_now = SPV(e, SPV_OP_LOAD, t->t_u32, pcv);
    uint32_t* labels = malloc(unique * sizeof *labels);
    uint32_t* operands = malloc((2 + 2 * unique) * sizeof *operands);
    if (!labels || !operands) {
        free(labels), free(operands), free(leaders);
        fail(t, "out of memory");
        return;
    }
    operands[0] = pc_now, operands[1] = cases_merge;
    for (size_t i = 0; i < unique; ++i) {
        labels[i] = spv_emit_id(e);
        operands[2 + 2 * i] = leaders[i], operands[3 + 2 * i] = labels[i];
    }
    SPV_VOID(e, SPV_OP_SELECTION_MERGE, cases_merge, SPV_SELECTION_CONTROL_NONE);
    spv_emit_void_op(e, SPV_OP_SWITCH, operands, 2 + 2 * unique);
    for (size_t i = 0; i < unique && !t->failed; ++i) {
        uint32_t first = leaders[i], end = i + 1 < unique ? leaders[i + 1] : count, next = end, last = end - 1;
        spv_emit_label(e, labels[i]);
        int32_t off;
        bool ends_in_branch = branch(instruction(code, last), &off);
        for (uint32_t pc = first; pc < end - (ends_in_branch ? 1 : 0); ++pc)
            if (!translate_one(t, instruction(code, pc), pc)) break;
        uint32_t to = cu(t, next);
        if (ends_in_branch) {
            unsigned pred = predicate(instruction(code, last));
            if (pred >= 7) {
                fail(t, "a BR's predicate %u (at %u)", pred, last);
                break;
            }
            uint32_t target = cu(t, (uint32_t)((int64_t)last + off));
            to = pred ? SPV(e, SPV_OP_SELECT, t->t_u32, predicate_value(t, pred), target, to) : target;
        }
        SPV_VOID(e, SPV_OP_STORE, pcv, to);
        SPV_VOID(e, SPV_OP_BRANCH, cases_merge);
    }
    free(labels), free(operands), free(leaders);
    spv_emit_label(e, cases_merge);
    SPV_VOID(e, SPV_OP_BRANCH, cont);
    spv_emit_label(e, cont);
    uint32_t done = op2(t, SPV_OP_U_GREATER_THAN_EQUAL, t->t_bool, SPV(e, SPV_OP_LOAD, t->t_u32, pcv), cu(t, count));
    SPV_VOID(e, SPV_OP_BRANCH_CONDITIONAL, done, merge, header);
    spv_emit_label(e, merge);
}

/* ---- the program's interface (spirv_recompiler.cpp, gxp_parser.cpp) ---- */
static Type parameter_store_type(unsigned type) {
    switch (type) {
        case GXP_TYPE_F32: return T_F32;
        case GXP_TYPE_F16: return T_F16;
        case GXP_TYPE_C10: return T_C10;
        case GXP_TYPE_U32: return T_U32;
        case GXP_TYPE_S32: return T_I32;
        case GXP_TYPE_U16: return T_U16;
        case GXP_TYPE_S16: return T_I16;
        case GXP_TYPE_U8: return T_U8;
        case GXP_TYPE_S8: return T_I8;
        default: return T_F32;
    }
}
/* the container (8 bytes: index, unknown, base SA offset, size in 32-bit words) of an index, NULL for none */
static const unsigned char* container(const unsigned char* gxp, unsigned index) {
    uint32_t count = snx_ld_le32(gxp + GXP_CONTAINER_COUNT);
    const unsigned char* c = gxp_at(gxp, GXP_CONTAINERS);
    for (uint32_t i = 0; i < count; ++i)
        if (snx_ld_le16(c + i * 8) == index) return c + i * 8;
    return NULL;
}
/* a value of n components, each stored into PA or SA from register offset as type, from an input variable's vec4 */
static Value vector_value(T* t, uint32_t vec, unsigned n, int kind) {
    Value v = {{0}, n, kind};
    for (unsigned i = 0; i < n; ++i) v.c[i] = SPV(&t->e, SPV_OP_COMPOSITE_EXTRACT, kind_type(t, kind), vec, i);
    return v;
}
static void store_input(T* t, Bank bank, unsigned offset, Type type, Value v) {
    if (!is_float_type(type) && v.kind == K_FLOAT) v = to_int(t, v, type, true);
    if (is_float_type(type) && v.kind != K_FLOAT) v = to_float(t, v, type, false);
    if (!is_float_type(type) && v.kind != (is_signed_type(type) ? K_INT : K_UINT))
        for (unsigned i = 0; i < v.n; ++i) v.c[i] = bitcast(t, is_signed_type(type) ? t->t_i32 : t->t_u32, v.c[i]);
    if (!is_float_type(type)) v.kind = is_signed_type(type) ? K_INT : K_UINT;
    Operand dest = {offset, bank, 0, {SW_X, SW_Y, SW_Z, SW_W}, type};
    for (unsigned at = 0; at < v.n; at += 4) {
        Value part = {{0}, v.n - at < 4 ? v.n - at : 4, v.kind};
        memcpy(part.c, v.c + at, part.n * 4);
        unsigned per = packed_count(type);
        store(t, dest, part, (1u << part.n) - 1, (int)(at / per));
    }
}
/* SceGxmAttributeFormat: whether the host gives the shader integers (the non-normalized integer formats). UNTYPED's
   words reach the registers as they are (vertex_inputs) */
#define GXM_ATTRIBUTE_UNTYPED 10u
static int attribute_kind(unsigned format) {
    switch (format) {
        case 0: case 2: return K_UINT; /* U8, U16 */
        case 1: case 3: return K_INT;  /* S8, S16 */
        default: return K_FLOAT;
    }
}
static void vertex_inputs(T* t) {
    const unsigned char* gxp = t->gxp;
    uint32_t count = snx_ld_le32(gxp + GXP_PARAMETER_COUNT), slot = 0;
    for (unsigned i = 0; i < USSE_ATTRIBUTES; ++i) t->out->attribute_location[i] = -1;
    for (uint32_t i = 0; i < count && !t->failed; ++i) {
        const unsigned char* q = gxp_parameter(gxp, i);
        if (gxp_parameter_category(q) != GXP_CATEGORY_ATTRIBUTE) continue;
        unsigned resource = gxp_parameter_resource(q), components = gxp_parameter_components(q);
        unsigned total = components * gxp_parameter_array_size(q), slots = (total + 3) / 4;
        Type type = parameter_store_type(gxp_parameter_type(q));
        int kind = K_FLOAT, hint = -1;
        for (unsigned a = 0; a < t->hints->attribute_count; ++a)
            if (t->hints->attributes[a].reg == resource) hint = (int)a;
        if (hint >= 0) {
            kind = attribute_kind(t->hints->attributes[hint].format);
            t->out->attribute_location[hint] = (int)slot;
        }
        if (hint >= 0 && t->hints->attributes[hint].format == GXM_ATTRIBUTE_UNTYPED) {
            /* its 32-bit words as they are, one a register from the resource's, whatever the parameter's type */
            unsigned words = t->hints->attributes[hint].components;
            for (unsigned s = 0; s < (words + 3) / 4; ++s) {
                uint32_t input = spv_emit_location(&t->e, SPV_STORAGE_INPUT, t->t_v4u, slot + s);
                global(t, input);
                uint32_t vec = SPV(&t->e, SPV_OP_LOAD, t->t_v4u, input);
                for (unsigned i = 0; i < 4 && s * 4 + i < words; ++i)
                    store_word(t, B_PA, resource + s * 4 + i, SPV(&t->e, SPV_OP_COMPOSITE_EXTRACT, t->t_u32, vec, i));
            }
            /* a three-component float parameter's fourth is 1, as a typed one's (a position's w) */
            if (words == 3 && components == 3 && type == T_F32) store_word(t, B_PA, resource + 3, cu(t, 0x3f800000u));
            slot += (words + 3) / 4;
            continue;
        }
        for (unsigned s = 0; s < slots; ++s) {
            uint32_t type4 = kind == K_FLOAT ? t->t_v4f : kind == K_UINT ? t->t_v4u : t->t_v4i;
            uint32_t input = spv_emit_location(&t->e, SPV_STORAGE_INPUT, type4, slot + s);
            global(t, input);
            uint32_t vec = SPV(&t->e, SPV_OP_LOAD, type4, input);
            unsigned n = total - s * 4 < 4 ? total - s * 4 : 4;
            Value v = vector_value(t, vec, n, kind);
            /* a 3-component attribute's fourth is 1, as the shader expects (Vita3K's is_4th_component_1) */
            store_input(t, B_PA, resource + s * 4 / packed_count(type), type, v);
        }
        slot += slots;
    }
}
/* a texture format's component type for a read whose type the program leaves to it (Vita3K's
   get_texture_component_type), by SceGxmTextureBaseFormat (the format's bits 31 and 24 to 28) */
static Type texture_component_type(uint32_t format) {
    switch (format & 0x9f000000u) {
        case 0x01000000u: case 0x08000000u: case 0x0d000000u: case 0x14000000u: case 0x99000000u: return T_I8;
        case 0x09000000u: case 0x0e000000u: case 0x0f000000u: case 0x1c000000u: return T_U16;
        case 0x0a000000u: case 0x10000000u: case 0x1d000000u: return T_I16;
        case 0x0b000000u: case 0x11000000u: case 0x19000000u: case 0x1a000000u: case 0x1b000000u: case 0x9a000000u:
            return T_F16;
        case 0x12000000u: case 0x13000000u: case 0x15000000u: case 0x17000000u: case 0x18000000u: case 0x1e000000u:
        case 0x1f000000u:
            return T_F32;
        default: return T_U8; /* the 8-bit unsigned ones, palettes, the compressed and YUV formats */
    }
}
static unsigned gxp_sampler_dimension(const T* t, unsigned resource) {
    uint32_t count = snx_ld_le32(t->gxp + GXP_PARAMETER_COUNT);
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char* q = gxp_parameter(t->gxp, i);
        if (gxp_parameter_category(q) == GXP_CATEGORY_SAMPLER && gxp_parameter_resource(q) == resource)
            return (gxp_parameter_semantic_index(q) & 0x10) ? SPV_DIM_CUBE : SPV_DIM_2D;
    }
    return 0;
}
/* texture resource's combined image sampler, loaded (its variable made at its first use) */
static uint32_t sampler_of(T* t, unsigned resource, unsigned dimension) {
    if (resource >= USSE_TEXTURES) return fail(t, "sampler %u", resource), cu(t, 0);
    unsigned declared_dimension = gxp_sampler_dimension(t, resource);
    if (declared_dimension && declared_dimension != dimension)
        return fail(t, "sampler %u dimension %u used as %u", resource, declared_dimension, dimension), cu(t, 0);
    if (t->sampler_dimensions[resource] && t->sampler_dimensions[resource] != dimension)
        return fail(t, "sampler %u used with multiple dimensions", resource), cu(t, 0);
    uint32_t image = spv_emit_type_image(&t->e, t->t_f32, dimension, 0, 0, 0, 1, SPV_IMAGE_FORMAT_UNKNOWN);
    uint32_t sampled_type = spv_emit_type_sampled_image(&t->e, image);
    if (!t->samplers[resource]) {
        t->samplers[resource] =
            spv_emit_resource(&t->e, SPV_STORAGE_UNIFORM_CONSTANT, sampled_type, 0, USSE_TEXTURE_BINDING + resource);
        global(t, t->samplers[resource]);
        t->sampler_dimensions[resource] = (uint8_t)dimension;
    }
    t->out->sampler_mask |= 1u << resource;
    return SPV(&t->e, SPV_OP_LOAD, sampled_type, t->samplers[resource]);
}
/* the components a texture read gives (get_texture_component_count): one or two for the formats whose swizzle keeps
   them so (R, GR), four for the others */
static unsigned texture_components(uint32_t format) {
    uint32_t base = format & 0x9f000000u, swizzle = format & 0x7000u;
    switch (base) {
        case 0x00000000u: case 0x01000000u: case 0x09000000u: case 0x0a000000u: case 0x0b000000u: case 0x12000000u:
        case 0x13000000u: case 0x17000000u: case 0x18000000u: /* U8 S8 U16 S16 F16 F32 F32M U32 S32 */
            return swizzle ? 4 : 1;
        case 0x07000000u: case 0x08000000u: case 0x0f000000u: case 0x10000000u: case 0x11000000u: case 0x1e000000u:
        case 0x1f000000u: /* U8U8 S8S8 U16U16 S16S16 F16F16 F32F32 U32U32 */
            return swizzle ? 4 : 2;
        case 0x15000000u: return 1; /* X8U24 */
        default: return 4;
    }
}
static uint32_t frag_coord(T* t) {
    if (!t->frag_coord) {
        t->frag_coord = spv_emit_builtin(&t->e, SPV_STORAGE_INPUT, t->t_v4f, SPV_BUILT_IN_FRAG_COORD);
        global(t, t->frag_coord);
    }
    return t->frag_coord;
}
/* the components to store for an input of n components into regs registers of type: as many as the registers hold,
   up to four, and at least n */
static unsigned input_components(unsigned n, unsigned regs, Type type) {
    unsigned fit = regs * packed_count(type);
    fit = fit > 4 ? 4 : fit;
    return fit > n ? fit : n;
}
static void fragment_inputs(T* t) {
    const unsigned char* gxp = t->gxp;
    const unsigned char* varyings = gxp_at(gxp, GXP_VARYINGS);
    const unsigned char* d = gxp_at(varyings, 16); /* the attribute descriptors (16 bytes each) */
    unsigned count = snx_ld_le16(varyings + 12), pa = 0;
    uint32_t coords[11] = {0}, others[14] = {0};
    for (unsigned i = 0; i < count && !t->failed; ++i, d += 16) {
        uint32_t info = snx_ld_le32(d), resource = snx_ld_le32(d + 4), size = snx_ld_le32(d + 8), components = snx_ld_le32(d + 12);
        if ((info & 0x4000f000u) != 0xf000u) { /* a varying, into PA */
            uint32_t id = info & 0x4000f000u, location = 0;
            if (id & 0x40000000u) {
                fail(t, "sprite coordinates");
                return;
            }
            static const uint32_t ids[14] = {0xd000, 0xc000, 0xa000, 0xb000, 0x0,    0x1000, 0x2000,
                                             0x3000, 0x4000, 0x5000, 0x6000, 0x7000, 0x8000, 0x9000};
            static const uint32_t locations[14] = {0, 3, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
            for (unsigned k = 0; k < 14; ++k)
                if (ids[k] == id) location = locations[k];
            Type type = T_U8;
            uint32_t input_type = info & 0x30100000u;
            if (input_type == 0x20000000u)
                type = T_F16;
            else if (input_type == 0x10000000u) {
                fail(t, "fixed point varyings");
                return;
            } else if (input_type == 0x100000u) {
                if (id == 0xa000 || id == 0xb000) type = T_F32;
            } else if (id != 0xa000 && id != 0xb000)
                type = T_F32;
            unsigned n = ((info >> 22) & 3) + 1;
            uint32_t vec;
            if (id == 0xd000) {
                vec = SPV(&t->e, SPV_OP_LOAD, t->t_v4f, frag_coord(t));
            } else {
                /* one variable a location: a texture read may have made a texture coordinate's already */
                uint32_t* input = id <= 0x9000 ? &coords[id / 0x1000] : &others[location];
                if (!*input) {
                    *input = spv_emit_location(&t->e, SPV_STORAGE_INPUT, t->t_v4f, location);
                    global(t, *input);
                }
                vec = SPV(&t->e, SPV_OP_LOAD, t->t_v4f, *input);
            }
            /* the registers the varying takes, whole: the iterator writes them all, and a program reads past its
               components (a three-component one's fourth register), which the vertex program's outputs give */
            unsigned regs = ((size >> 4) & 3) + 1;
            store_input(t, B_PA, pa, type, vector_value(t, vec, input_components(n, regs, type), K_FLOAT));
            pa += regs;
        }
        uint32_t coord_index = info & 0x40f;
        if (coord_index != 0xf) { /* a non-dependent texture read, into PA (texture.cpp: do_texture_queries) */
            if (coord_index == 0x400) {
                fail(t, "point sprite texture reads");
                return;
            }
            if (resource >= USSE_TEXTURES) {
                fail(t, "sampler %u", (unsigned)resource);
                return;
            }
            unsigned swizzle = info & 0x300;
            int project = swizzle == 0x300 ? 2 : swizzle == 0x200 ? 3 : -1;
            unsigned component_type = (components >> 4) & 3;
            Type type = component_type == 3 ? T_F32 : component_type == 2 ? T_F16 : T_UNK;
            uint32_t format = t->hints->texture_formats[resource];
            if (type == T_UNK) type = texture_component_type(format);
            if (!coords[coord_index]) {
                coords[coord_index] = spv_emit_location(&t->e, SPV_STORAGE_INPUT, t->t_v4f, 4 + coord_index);
                global(t, coords[coord_index]);
            }
            uint32_t coord = SPV(&t->e, SPV_OP_LOAD, t->t_v4f, coords[coord_index]), sampled;
            unsigned dimension = gxp_sampler_dimension(t, resource);
            if (!dimension) dimension = SPV_DIM_2D;
            uint32_t sampler = sampler_of(t, resource, dimension);
            if (project >= 0) {
                if (dimension == SPV_DIM_CUBE) {
                    fail(t, "projected cube texture inputs");
                    return;
                }
                uint32_t xyp = SPV(&t->e, SPV_OP_VECTOR_SHUFFLE, t->t_v3f, coord, coord, 0, 1, (uint32_t)project);
                sampled = SPV(&t->e, SPV_OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD, t->t_v4f, sampler, xyp);
            } else if (dimension == SPV_DIM_CUBE) {
                uint32_t xyz = SPV(&t->e, SPV_OP_VECTOR_SHUFFLE, t->t_v3f, coord, coord, 0, 1, 2);
                sampled = SPV(&t->e, SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, t->t_v4f, sampler, xyz);
            } else {
                uint32_t xy = SPV(&t->e, SPV_OP_VECTOR_SHUFFLE, t->t_v2f, coord, coord, 0, 1);
                sampled = SPV(&t->e, SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, t->t_v4f, sampler, xy);
            }
            unsigned regs = ((size >> 6) & 3) + 1; /* its registers, whole, as a varying's */
            store_input(t, B_PA, pa, type,
                        vector_value(t, sampled, input_components(texture_components(format), regs, type), K_FLOAT));
            pa += regs;
        }
    }
}
/* the uniform buffers into SA (copy_uniform_block_to_register), and the literals */
static void uniforms(T* t) {
    const unsigned char* gxp = t->gxp;
    uint32_t count = snx_ld_le32(gxp + GXP_PARAMETER_COUNT);
    struct {
        bool used;
        uint32_t size, reg_start, reg_size;
    } buffers[15] = {{0}};
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char* q = gxp_parameter(gxp, i);
        unsigned category = gxp_parameter_category(q), index = gxp_parameter_container(q);
        if (category == GXP_CATEGORY_UNIFORM_BUFFER) { /* a buffer the program reads from memory (VLDST) */
            uint32_t resource = gxp_parameter_resource(q), words = (gxp_parameter_array_size(q) + 3) / 4;
            if (resource >= 15) continue;
            buffers[resource].used = true;
            if (words > buffers[resource].size) buffers[resource].size = words;
            continue;
        }
        if (category != GXP_CATEGORY_UNIFORM || index >= 15) continue;
        Type type = parameter_store_type(gxp_parameter_type(q));
        uint32_t components = gxp_parameter_components(q), array = gxp_parameter_array_size(q);
        uint32_t vector = components * type_size(type);
        if (array != 1) vector = is_float_type(type) && components != 1 ? (vector + 7) & ~7u : (vector + 3) & ~3u;
        uint32_t words = (array * vector + 3) / 4, end = gxp_parameter_resource(q) + words;
        const unsigned char* c = container(gxp, index);
        if (!buffers[index].used) {
            buffers[index].used = true;
            buffers[index].reg_size = c ? snx_ld_le16(c + 6) : 0;
            buffers[index].reg_start = c ? snx_ld_le16(c + 4) : 0;
        }
        if (end > buffers[index].size) buffers[index].size = end;
    }
    /* a buffer that may be larger than a GXP's sizes say (the most an array of them holds) is read to the most a
       buffer holds here, as Vita3K does */
    for (unsigned i = 0; i < 15; ++i)
        if (buffers[i].size == 1024) buffers[i].size = 2048;
    uint32_t defaults = snx_ld_le32(gxp + GXP_DEFAULT_UNIFORM_COUNT);
    if (!buffers[GXP_DEFAULT_CONTAINER].used && defaults) {
        buffers[GXP_DEFAULT_CONTAINER].used = true;
        buffers[GXP_DEFAULT_CONTAINER].reg_size = defaults;
        buffers[GXP_DEFAULT_CONTAINER].size = defaults;
    }
    /* the storage buffer: each buffer's words in turn */
    UsseShader* out = t->out;
    for (unsigned i = 0; i < 15; ++i)
        if (buffers[i].used && buffers[i].size) {
            out->buffers[out->buffer_count].container = i;
            out->buffers[out->buffer_count].offset = out->buffer_words;
            out->buffers[out->buffer_count].words = buffers[i].size;
            out->buffer_words += buffers[i].size;
            ++out->buffer_count;
        }
    if (out->buffer_words) {
        uint32_t runtime = spv_emit_type_runtime_array(&t->e, t->t_u32, 4), zero = 0;
        uint32_t block = spv_emit_type_struct(&t->e, &runtime, &zero, 1);
        spv_emit_decorate(&t->e, block, SPV_DECORATION_BLOCK, NULL, 0);
        /* only read: a vertex stage may write no storage buffer without vertexPipelineStoresAndAtomics */
        spv_emit_member_decorate(&t->e, block, 0, SPV_DECORATION_NON_WRITABLE, NULL, 0);
        uint32_t buffer = spv_emit_resource(&t->e, SPV_STORAGE_STORAGE_BUFFER, block, 0, t->fragment ? 1 : 0);
        global(t, buffer);
        t->buffer = buffer;
        uint32_t pointer = spv_emit_type_pointer(&t->e, SPV_STORAGE_STORAGE_BUFFER, t->t_u32);
        for (unsigned b = 0; b < out->buffer_count; ++b) {
            unsigned index = out->buffers[b].container;
            uint32_t n = buffers[index].reg_size < out->buffers[b].words ? buffers[index].reg_size : out->buffers[b].words;
            for (uint32_t k = 0; k < n && buffers[index].reg_start + k < BANK_WORDS; ++k) {
                uint32_t w = SPV(&t->e, SPV_OP_LOAD, t->t_u32,
                                 SPV(&t->e, SPV_OP_ACCESS_CHAIN, pointer, buffer, cu(t, 0), cu(t, out->buffers[b].offset + k)));
                store_word(t, B_SA, buffers[index].reg_start + k, w);
            }
        }
    }
    /* the buffers read from memory (gxp_parser.cpp: the program's uniform buffer table, 8 bytes each: the buffer, the
       register from container 19's base holding where its reads start, that start in bytes): the register holds the
       buffer's bytes' place in the storage buffer and the start. The literal buffer's reads are the program's own
       bytes, read as it is translated (VLDST) */
    const unsigned char* sa_base = container(gxp, 19);
    uint32_t base = sa_base ? snx_ld_le16(sa_base + 4) : 0, table = snx_ld_le32(gxp + GXP_UNIFORM_BUFFER_COUNT);
    const unsigned char* entry = gxp_at(gxp, GXP_UNIFORM_BUFFERS);
    for (uint32_t i = 0; i < table && !t->failed; ++i, entry += 8) {
        uint32_t reside = snx_ld_le16(entry), reg = base + snx_ld_le16(entry + 2), start = snx_ld_le32(entry + 4);
        if (reside == 16) {
            t->literal_register = (int)reg, t->literal_base = start;
            /* its words, as the program holds them */
            const unsigned char* data = gxp_at(gxp, GXP_LITERAL_DATA);
            size_t at = (size_t)(data - gxp), words = at < t->size ? (t->size - at) / 4 : 0;
            uint32_t count = (snx_ld_le32(gxp + 88) + 3) / 4; /* literal_buffer_count, in bytes */
            if (words > count) words = count;
            if (words && !t->literals) {
                t->literal_words = (uint32_t)words;
                t->literals = var(t, SPV_STORAGE_PRIVATE, spv_emit_type_array(&t->e, t->t_u32, (uint32_t)words));
                uint32_t pointer = spv_emit_type_pointer(&t->e, SPV_STORAGE_PRIVATE, t->t_u32);
                for (uint32_t k = 0; k < words; ++k)
                    SPV_VOID(&t->e, SPV_OP_STORE, SPV(&t->e, SPV_OP_ACCESS_CHAIN, pointer, t->literals, cu(t, k)),
                             cu(t, snx_ld_le32(data + k * 4)));
            }
            continue;
        }
        if (reside >= 15) {
            fail(t, "a %s buffer", reside == 15 ? "texture" : reside == 18 ? "thread" : "program");
            return;
        }
        for (unsigned b = 0; b < out->buffer_count; ++b)
            if (out->buffers[b].container == reside) store_word(t, B_SA, reg, cu(t, out->buffers[b].offset * 4 + start));
    }
    /* the dependent samplers (4 bytes each: the texture's index times 4, the register from container 19's base) */
    uint32_t dependents = snx_ld_le32(gxp + GXP_DEPENDENT_SAMPLER_COUNT);
    const unsigned char* d = gxp_at(gxp, GXP_DEPENDENT_SAMPLERS);
    for (uint32_t i = 0; i < dependents && sa_base; ++i) {
        uint32_t texture = snx_ld_le16(d + i * 4) / 4, reg = base + snx_ld_le16(d + i * 4 + 2);
        if (texture < USSE_TEXTURES && reg < BANK_WORDS) t->dependent[reg] = (uint8_t)(texture + 1);
    }
    /* the literals: (register, word) pairs, the register from container 19's base as the dependent samplers' (at 0 the
       program's uniforms would be overwritten, and the registers it reads left as they were) */
    uint32_t literals = snx_ld_le32(gxp + GXP_LITERALS_COUNT);
    const unsigned char* l = gxp_at(gxp, GXP_LITERALS);
    for (uint32_t i = 0; i < literals; ++i)
        store_word(t, B_SA, base + snx_ld_le32(l + i * 8), cu(t, snx_ld_le32(l + i * 8 + 4)));
}
/* the vertex outputs (gxp.cpp: get_vertex_outputs) from O0 on, in order, and the position's viewport transform
   (make_vert_finalize_function) */
static void vertex_outputs(T* t) {
    const unsigned char* varyings = gxp_at(t->gxp, GXP_VARYINGS);
    uint32_t vo1 = snx_ld_le32(varyings + 16), vo2 = snx_ld_le32(varyings + 20);
    unsigned o = 0;
    Operand out = {0, B_OUT, 0, {SW_X, SW_Y, SW_Z, SW_W}, T_F32};
    /* the position */
    out.num = o;
    Value pos = load(t, out, 15, 0);
    o += 4;
    {
        uint32_t pointer = spv_emit_type_pointer(&t->e, SPV_STORAGE_PUSH_CONSTANT, t->t_f32);
        uint32_t field[5];
        for (unsigned i = 0; i < 5; ++i)
            field[i] = SPV(&t->e, SPV_OP_LOAD, t->t_f32, SPV(&t->e, SPV_OP_ACCESS_CHAIN, pointer, t->push, cu(t, i)));
        /* enabled: z' = z_scale * z + z_offset * w, whose z' / w is the viewport's z_offset + z_scale * z / w, linear in
           clip space for the clipper to interpolate (the depth clamp, where the device has it, keeps what lies past
           [0, 1] as the console does); disabled: x, y from window coordinates, z = w */
        uint32_t enabled = op2(t, SPV_OP_F_ORD_GREATER_THAN, t->t_bool, field[0], cf(t, 0.5f));
        uint32_t z = ext3(t, t->t_f32, SPV_GLSL_FMA, pos.c[2], field[4], op2(t, SPV_OP_F_MUL, t->t_f32, pos.c[3], field[3]));
        uint32_t sx = op2(t, SPV_OP_F_SUB, t->t_f32, op2(t, SPV_OP_F_MUL, t->t_f32, pos.c[0],
                                                          op2(t, SPV_OP_F_DIV, t->t_f32, cf(t, 2), field[1])), cf(t, 1));
        uint32_t sy = op2(t, SPV_OP_F_SUB, t->t_f32, op2(t, SPV_OP_F_MUL, t->t_f32, pos.c[1],
                                                          op2(t, SPV_OP_F_DIV, t->t_f32, cf(t, 2), field[2])), cf(t, 1));
        uint32_t x = SPV(&t->e, SPV_OP_SELECT, t->t_f32, enabled, pos.c[0], sx);
        uint32_t y = SPV(&t->e, SPV_OP_SELECT, t->t_f32, enabled, pos.c[1], sy);
        z = SPV(&t->e, SPV_OP_SELECT, t->t_f32, enabled, z, pos.c[3]);
        uint32_t position = spv_emit_builtin(&t->e, SPV_STORAGE_OUTPUT, t->t_v4f, SPV_BUILT_IN_POSITION);
        global(t, position);
        SPV_VOID(&t->e, SPV_OP_STORE, position, SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v4f, x, y, z, pos.c[3]));
    }
    struct {
        bool present;
        unsigned location, components;
    } outputs[13] = {{vo1 & 0x800, 1, 4}, {vo1 & 0x400, 2, 4}, {vo1 & 0x200, 3, 2}};
    for (unsigned i = 0; i < 10; ++i) {
        unsigned info = (vo2 >> (i * 3)) & 7;
        outputs[3 + i].present = info != 0;
        outputs[3 + i].location = 4 + i;
        outputs[3 + i].components = (info & 1 ? 2 : 0) + (info & 2 ? 1 : 0) + (info & 4 ? 1 : 0);
    }
    for (unsigned i = 0; i < 13; ++i) {
        if (!outputs[i].present) continue;
        out.num = o;
        Value v = load(t, out, 15, 0);
        uint32_t variable = spv_emit_location(&t->e, SPV_STORAGE_OUTPUT, t->t_v4f, outputs[i].location);
        global(t, variable);
        SPV_VOID(&t->e, SPV_OP_STORE, variable, SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v4f, v.c[0], v.c[1], v.c[2], v.c[3]));
        o += outputs[i].components;
    }
    /* the varyings it doesn't write as zero, as its output registers start: a fragment program may read one (a game
       pairs programs so), which a missing output would leave undefined */
    for (unsigned i = 0; i < 13; ++i) {
        if (outputs[i].present) continue;
        uint32_t variable = spv_emit_location(&t->e, SPV_STORAGE_OUTPUT, t->t_v4f, outputs[i].location);
        global(t, variable);
        SPV_VOID(&t->e, SPV_OP_STORE, variable, SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v4f, cf(t, 0), cf(t, 0), cf(t, 0), cf(t, 0)));
    }
    if (vo1 & 0x100) {
        out.num = o;
        Value v = load(t, out, 1, 0);
        uint32_t size = spv_emit_builtin(&t->e, SPV_STORAGE_OUTPUT, t->t_f32, SPV_BUILT_IN_POINT_SIZE);
        global(t, size);
        SPV_VOID(&t->e, SPV_OP_STORE, size, v.c[0]);
    }
}
/* the color from PA (or O for a native color program), of the output type, to location 0 */
static void fragment_output(T* t) {
    const unsigned char* gxp = t->gxp;
    const unsigned char* varyings = gxp_at(gxp, GXP_VARYINGS);
    bool native = snx_ld_le32(gxp + GXP_FLAGS) & GXP_FLAG_NATIVECOLOR_USED;
    Type type = parameter_store_type(varyings[10]);
    if (type == T_I32 || type == T_U32) type = T_F32;
    unsigned offset = !native && varyings[10] == 1 ? varyings[8] : 0;
    Operand color = {0, native ? B_OUT : B_PA, 0, {SW_X, SW_Y, SW_Z, SW_W}, type};
    Value v = load(t, color, 15, (int)offset);
    if (!is_float_type(type)) v = to_float(t, v, type, true);
    uint32_t out = spv_emit_location(&t->e, SPV_STORAGE_OUTPUT, t->t_v4f, 0);
    global(t, out);
    SPV_VOID(&t->e, SPV_OP_STORE, out, SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v4f, v.c[0], v.c[1], v.c[2], v.c[3]));
}

bool usse_translate(const unsigned char* gxp, size_t size, const UsseHints* hints, bool preserve_infnan,
                    UsseShader* out, char* error, size_t error_size) {
    memset(out, 0, sizeof *out);
    if (size < 160 || snx_ld_le32(gxp) != GXP_MAGIC) {
        snprintf(error, error_size, "not a GXP program");
        return false;
    }
    T* t = calloc(1, sizeof *t);
    if (!t) {
        snprintf(error, error_size, "out of memory");
        return false;
    }
    t->gxp = gxp, t->size = size, t->hints = hints, t->out = out, t->error = error, t->error_size = error_size;
    t->literal_register = -1;
    t->fragment = gxp_fragment(gxp);
    out->fragment = t->fragment;
    reset_repeats(t);
    SpvEmitter* e = &t->e;
    spv_emit_init(e, 4096);
    spv_emit_capability(e, SPV_CAPABILITY_SHADER);
    t->glsl = spv_emit_glsl450_import(e);
    t->t_void = spv_emit_type_void(e), t->t_bool = spv_emit_type_bool(e);
    t->t_u32 = spv_emit_type_u32(e), t->t_i32 = spv_emit_type_i32(e), t->t_f32 = spv_emit_type_f32(e);
    t->t_v2f = spv_emit_type_vector(e, t->t_f32, 2), t->t_v3f = spv_emit_type_vector(e, t->t_f32, 3);
    t->t_v4f = spv_emit_type_vector(e, t->t_f32, 4), t->t_v4u = spv_emit_type_vector(e, t->t_u32, 4);
    t->t_v4i = spv_emit_type_vector(e, t->t_i32, 4);
    /* the banks: PA, SA, temporaries, outputs, the internal registers (four words each), predicates */
    uint32_t words = spv_emit_type_array(e, t->t_u32, BANK_WORDS);
    Bank word_banks[5] = {B_PA, B_SA, B_TEMP, B_OUT, B_INTERNAL};
    for (unsigned i = 0; i < 5; ++i) t->bank[word_banks[i]] = var(t, SPV_STORAGE_PRIVATE, words);
    t->bank[B_PRED] = var(t, SPV_STORAGE_PRIVATE, spv_emit_type_array(e, t->t_bool, 4));
    if (!t->fragment) {
        uint32_t members[5] = {t->t_f32, t->t_f32, t->t_f32, t->t_f32, t->t_f32}, offsets[5] = {0, 4, 8, 12, 16};
        uint32_t block = spv_emit_type_struct(e, members, offsets, 5);
        spv_emit_decorate(e, block, SPV_DECORATION_BLOCK, NULL, 0);
        t->push = var(t, SPV_STORAGE_PUSH_CONSTANT, block);
    }
    uint32_t main = spv_emit_function(e, 0, t->t_void, NULL, 0, NULL);
    /* zeroed outputs, as Vita3K's vertex programs start with */
    for (unsigned i = 0; i < 80 && !t->fragment; ++i) store_word(t, B_OUT, i, cu(t, 0));
    uniforms(t);
    if (t->fragment)
        fragment_inputs(t);
    else
        vertex_inputs(t);
    /* a program that blends itself (FRAGCOLOR_USED) starts with its pixel's color in O0, of its output's type
       (spirv_recompiler.cpp: the input attachment read, its last_frag_data) */
    if (t->fragment && (snx_ld_le32(gxp + GXP_FLAGS) & GXP_FLAG_FRAGCOLOR_USED)) {
        spv_emit_capability(e, SPV_CAPABILITY_INPUT_ATTACHMENT);
        uint32_t image = spv_emit_type_image(e, t->t_f32, SPV_DIM_SUBPASS_DATA, 0, 0, 0, 2, SPV_IMAGE_FORMAT_UNKNOWN);
        uint32_t color = spv_emit_resource(e, SPV_STORAGE_UNIFORM_CONSTANT, image, 0, USSE_COLOR_BINDING);
        const uint32_t index = 0;
        spv_emit_decorate(e, color, SPV_DECORATION_INPUT_ATTACHMENT_INDEX, &index, 1);
        global(t, color);
        uint32_t v2i = spv_emit_type_vector(e, t->t_i32, 2);
        uint32_t texel = SPV(e, SPV_OP_IMAGE_READ, t->t_v4f, SPV(e, SPV_OP_LOAD, image, color),
                             spv_emit_constant_vec2(e, v2i, ci(t, 0), ci(t, 0)));
        const unsigned char* varyings = gxp_at(gxp, GXP_VARYINGS);
        Type type = parameter_store_type(varyings[10]);
        if (type == T_I32 || type == T_U32) type = T_F32;
        Value v = vector_value(t, texel, 4, K_FLOAT);
        if (!is_float_type(type)) v = to_int(t, v, type, true);
        Operand o0 = {0, B_OUT, 0, {SW_X, SW_Y, SW_Z, SW_W}, type};
        store(t, o0, v, 15, 0);
        out->reads_color = true;
    }
    /* the depth a program that writes it (DEPTHF) leaves alone where it doesn't: the fragment's */
    if (t->fragment && (snx_ld_le32(gxp + GXP_FLAGS) & GXP_FLAG_DEPTH_USED)) {
        t->frag_depth = spv_emit_builtin(e, SPV_STORAGE_OUTPUT, t->t_f32, SPV_BUILT_IN_FRAG_DEPTH);
        global(t, t->frag_depth);
        uint32_t z = SPV(e, SPV_OP_COMPOSITE_EXTRACT, t->t_f32, SPV(e, SPV_OP_LOAD, t->t_v4f, frag_coord(t)), 2);
        SPV_VOID(e, SPV_OP_STORE, t->frag_depth, z);
    }
    /* the secondary program (once per draw on the console, its results in SA), then the primary */
    uint32_t secondary = snx_ld_le32(gxp + GXP_SECONDARY_COUNT);
    const unsigned char* start = gxp_at(gxp, GXP_SECONDARY);
    const unsigned char* end = gxp_at(gxp, GXP_SECONDARY_END);
    if (end > start && (size_t)(end - gxp) <= size) secondary = (uint32_t)((end - start) / 8);
    if (secondary && !t->failed) {
        t->secondary = true;
        translate_code(t, start, secondary);
        t->secondary = false;
        reset_repeats(t);
    }
    uint32_t primary = snx_ld_le32(gxp + GXP_PRIMARY_COUNT);
    const unsigned char* code = gxp_at(gxp, GXP_PRIMARY);
    if ((size_t)(code - gxp) + primary * 8ull > size) fail(t, "the program's code is past its end");
    if (!t->failed) translate_code(t, code, primary);
    if (!t->failed) {
        if (t->fragment)
            fragment_output(t);
        else
            vertex_outputs(t);
    }
    spv_emit_return(e);
    spv_emit_function_end(e);
    bool ok = !t->failed;
    if (ok) {
        spv_emit_entry_str(e, t->fragment ? SPV_STAGE_FRAGMENT : SPV_STAGE_VERTEX, main, "main", t->globals,
                           t->global_count);
        if (t->fragment) spv_emit_fragment_origin_upper_left(e, main);
        if (t->frag_depth) spv_emit_fragment_depth_replacing(e, main);
        spv_emit_float_controls(e, main, 0, preserve_infnan, 0);
        out->words = spv_emit_finish(e, &out->count);
        ok = out->words != NULL;
    }
    spv_emit_destroy(e);
    free(t);
    return ok;
}
void usse_shader_free(UsseShader* shader) {
    free(shader->words);
    memset(shader, 0, sizeof *shader);
}

/* ---- the shader corpus ---- */
static const char corpus_magic[8] = "SNXGXP1";
bool usse_corpus_write(const char* folder, const unsigned char* program, size_t size, const UsseHints* hints) {
    uint64_t h = snx_fnv1a(snx_fnv1a(SNX_FNV_BASIS, hints, sizeof *hints), program, size);
    char path[1200];
    snprintf(path, sizeof path, "%s/%016llx.gxp", folder, (unsigned long long)h);
    snx_path_native(path);
    FILE* f = fopen(path, "rb");
    if (f) { /* there already */
        fclose(f);
        return true;
    }
    if (!(f = fopen(path, "wb"))) return false;
    bool ok = fwrite(corpus_magic, 1, 8, f) == 8 && fwrite(hints, sizeof *hints, 1, f) == 1 &&
              fwrite(program, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}
bool usse_corpus_read(const char* path, unsigned char** program, size_t* size, UsseHints* hints) {
    FILE* f = fopen(path, "rb");
    char magic[8];
    *program = NULL;
    if (!f) return false;
    bool ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, corpus_magic, 8) && fread(hints, sizeof *hints, 1, f) == 1;
    long start = ok ? ftell(f) : 0;
    ok = ok && !fseek(f, 0, SEEK_END);
    long end = ok ? ftell(f) : 0;
    ok = ok && end > start && !fseek(f, start, SEEK_SET) && (*program = malloc((size_t)(end - start)));
    ok = ok && fread(*program, 1, (size_t)(end - start), f) == (size_t)(end - start);
    fclose(f);
    if (!ok) {
        free(*program);
        *program = NULL;
        return false;
    }
    *size = (size_t)(end - start);
    return true;
}
static void survey_code(const unsigned char* code, uint32_t count, void (*each)(void*, const char*, bool), void* c) {
    T t = {0};
    t.survey = true;
    for (uint32_t pc = 0; pc < count; ++pc) {
        t.name = NULL, t.known = false;
        decode_usse(&t, (uint64_t)snx_ld_le32(code + pc * 8) | (uint64_t)snx_ld_le32(code + pc * 8 + 4) << 32);
        each(c, t.name, t.known);
    }
}
void usse_survey(const unsigned char* gxp, size_t size, void (*each)(void* context, const char* name, bool known),
                 void* context) {
    if (size < 160 || snx_ld_le32(gxp) != GXP_MAGIC) return;
    const unsigned char *start = gxp_at(gxp, GXP_SECONDARY), *end = gxp_at(gxp, GXP_SECONDARY_END);
    if (end > start && (size_t)(end - gxp) <= size) survey_code(start, (uint32_t)((end - start) / 8), each, context);
    uint32_t primary = snx_ld_le32(gxp + GXP_PRIMARY_COUNT);
    const unsigned char* code = gxp_at(gxp, GXP_PRIMARY);
    if ((size_t)(code - gxp) + primary * 8ull <= size) survey_code(code, primary, each, context);
}
