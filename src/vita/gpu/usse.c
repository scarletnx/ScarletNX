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
    uint32_t push; /* the push constant block: the vertex stage's UssePush, the fragment stage's scale */
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

/* ---- the instructions: what each computes is in Sail (below, usse_sail.inc); the texture reads' back end
   (gpu/sail/usse_mem_spirv.h) takes the program's textures and the registers they fill from the interface's code
   further down ---- */
static Type texture_component_type(uint32_t format);
static unsigned gxp_sampler_dimension(const T* t, unsigned resource);
static uint32_t sampler_of(T* t, unsigned resource, unsigned dimension);
static unsigned texture_components(uint32_t format);
static Value vector_value(T* t, uint32_t vec, unsigned n, int kind);
static void store_input(T* t, Bank bank, unsigned offset, Type type, Value v);

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

/* ---- the instructions in Sail: what each computes is gpu/sail/usse.sail's and its families' (sailgen --gpu), their back
   ends gpu/sail/usse_spirv.h and usse_<family>_spirv.h over the operand model above, the generated usse_sail.inc's
   sgu_emit running the root of a pattern ---- */
#define USSE_OPS(X)                                                                                                    \
    X(VMAD2) X(V32NMAD) X(V16NMAD) X(VMAD) X(VDP) X(VDUAL) X(VCOMP) X(VMOV) X(VPCK) X(VTST) X(VTSTMSK) X(VBW) X(SOP2)  \
    X(SOP3) X(SOP2M) X(I8MAD) X(I16MAD) X(I32MAD) X(ILLEGAL22) X(ILLEGAL23) X(ILLEGAL24) X(I8MAD2) X(I32MAD2)         \
    X(ILLEGAL27) X(SMP) X(PHAS) X(NOP) X(BR) X(SMLSI) X(SMBO) X(KILL) X(LIMM) X(DEPTHF) X(SPEC) X(VLDST)
#define USSE_OP(NAME) U_##NAME,
enum { USSE_OPS(USSE_OP) };
#undef USSE_OP
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "usse_sail.inc"
#pragma GCC diagnostic pop
/* the instruction being translated, its root run */
static void sail(T* t, int op, const void* arg) {
    SgG g;
    sgu_begin(&g, t, t->instr, arg);
    sgu_emit(&g, op);
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
TRANS(VMAD2, true, sail(t, U_VMAD2, arg))
TRANS(V32NMAD, true, sail(t, U_V32NMAD, arg))
TRANS(V16NMAD, true, sail(t, U_V16NMAD, arg))
TRANS(VMAD, true, sail(t, U_VMAD, arg))
TRANS(VDP, true, sail(t, U_VDP, arg))
TRANS(VDUAL, true, sail(t, U_VDUAL, arg))
TRANS(VCOMP, true, sail(t, U_VCOMP, arg))
TRANS(VMOV, true, sail(t, U_VMOV, arg))
TRANS(VPCK, true, sail(t, U_VPCK, arg))
TRANS(VTST, true, sail(t, U_VTST, arg))
TRANS(VTSTMSK, true, sail(t, U_VTSTMSK, arg))
TRANS(VBW, true, sail(t, U_VBW, arg))
TRANS(SOP2, true, sail(t, U_SOP2, arg))
TRANS(SOP2M, true, sail(t, U_SOP2M, arg))
UNTRANSLATED(SOP3)
UNTRANSLATED(I8MAD)
TRANS(I16MAD, true, sail(t, U_I16MAD, arg))
TRANS(I32MAD, true, sail(t, U_I32MAD, arg))
UNTRANSLATED(ILLEGAL22)
UNTRANSLATED(ILLEGAL23)
UNTRANSLATED(ILLEGAL24)
UNTRANSLATED(I8MAD2)
TRANS(I32MAD2, true, sail(t, U_I32MAD2, arg))
UNTRANSLATED(ILLEGAL27)
TRANS(SMP, true, sail(t, U_SMP, arg))
TRANS(PHAS, false, sail(t, U_PHAS, arg))
TRANS(NOP, false, sail(t, U_NOP, arg))
TRANS(BR, false, sail(t, U_BR, arg)) /* translate_code makes the branches of the program's blocks */
TRANS(SMLSI, false, sail(t, U_SMLSI, arg))
TRANS(SMBO, true, sail(t, U_SMBO, arg))
TRANS(KILL, true, sail(t, U_KILL, arg))
TRANS(LIMM, true, sail(t, U_LIMM, arg))
TRANS(DEPTHF, true, sail(t, U_DEPTHF, arg))
TRANS(SPEC, false, sail(t, U_SPEC, arg))
TRANS(VLDST, true, sail(t, U_VLDST, arg))

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
                /* the pixel's position as the console's surface has it: the scene's, drawn larger by the resolution
                   setting, divided by its scale */
                uint32_t pointer = spv_emit_type_pointer(&t->e, SPV_STORAGE_PUSH_CONSTANT, t->t_f32);
                uint32_t scale =
                    SPV(&t->e, SPV_OP_LOAD, t->t_f32, SPV(&t->e, SPV_OP_ACCESS_CHAIN, pointer, t->push, cu(t, 0)));
                uint32_t at = SPV(&t->e, SPV_OP_LOAD, t->t_v4f, frag_coord(t)), c[4];
                for (unsigned k = 0; k < 4; ++k) c[k] = SPV(&t->e, SPV_OP_COMPOSITE_EXTRACT, t->t_f32, at, k);
                for (unsigned k = 0; k < 2; ++k) c[k] = op2(t, SPV_OP_F_DIV, t->t_f32, c[k], scale);
                vec = SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v4f, c[0], c[1], c[2], c[3]);
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
            if (coord_index >= sizeof coords / sizeof *coords) {
                fail(t, "texture coordinate %u", (unsigned)coord_index);
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

/* whether a table lies in the program's bytes: count records of bytes each, from where the offset field at field
   points (an empty one is nowhere). The counts and the offsets are the game's words, and the translator walks each
   table by its count */
static bool table_fits(const unsigned char* gxp, size_t size, unsigned field, unsigned count_field, unsigned bytes) {
    uint64_t at = (uint64_t)field + snx_ld_le32(gxp + field), count = snx_ld_le32(gxp + count_field);
    return !count || (at <= size && count * bytes <= size - at);
}
bool usse_translate(const unsigned char* gxp, size_t size, const UsseHints* hints, bool preserve_infnan,
                    UsseShader* out, char* error, size_t error_size) {
    static const struct {
        const char* name;
        unsigned field, count_field, bytes;
    } tables[] = {{"parameters", GXP_PARAMETERS, GXP_PARAMETER_COUNT, 16},
                  {"containers", GXP_CONTAINERS, GXP_CONTAINER_COUNT, 8},
                  {"uniform buffers", GXP_UNIFORM_BUFFERS, GXP_UNIFORM_BUFFER_COUNT, 8},
                  {"dependent samplers", GXP_DEPENDENT_SAMPLERS, GXP_DEPENDENT_SAMPLER_COUNT, 4},
                  {"literals", GXP_LITERALS, GXP_LITERALS_COUNT, 8}};
    memset(out, 0, sizeof *out);
    if (size < 160 || snx_ld_le32(gxp) != GXP_MAGIC) {
        snprintf(error, error_size, "not a GXP program");
        return false;
    }
    for (unsigned i = 0; i < sizeof tables / sizeof *tables; ++i)
        if (!table_fits(gxp, size, tables[i].field, tables[i].count_field, tables[i].bytes)) {
            snprintf(error, error_size, "the GXP program's %u %s, from byte %llu, end past its %zu bytes",
                     (unsigned)snx_ld_le32(gxp + tables[i].count_field), tables[i].name,
                     (unsigned long long)tables[i].field + snx_ld_le32(gxp + tables[i].field), size);
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
    } else {
        uint32_t member = t->t_f32, offset = USSE_FRAGMENT_PUSH;
        uint32_t block = spv_emit_type_struct(e, &member, &offset, 1);
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
