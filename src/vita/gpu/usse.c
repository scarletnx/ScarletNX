/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Vita3K's shader recompiler (GPL-2.0-or-later, Copyright (C) 2026 Vita3K team) at Vita3K/Vita3K f68b6ee:
   shader/src/usse_translator_entry.cpp (the instructions' bit patterns), usse_decode_helpers.cpp (operand banks,
   register numbers, swizzle tables, write masks), usse_utilities.cpp (operand loads and stores, packing),
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
} T;

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

/* ---- the instructions (fields in order of their letters in the bit patterns) ---- */
typedef bool (*Handler)(T* t, const uint32_t* f);
static unsigned popcount4(unsigned m) { return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1); }

/* VMOV: p s t r y d e c b m a n o i w k l f g h j q u v (data.cpp) */
static bool vmov(T* t, const uint32_t* f) {
    unsigned test2 = f[2], comp_sel = f[3], dest_ext = f[5], src0_ext = f[6], src1_ext = f[7], src2_ext = f[8];
    unsigned move_type = f[9], repeat_count = f[10], test1 = f[13], src0_swiz = f[14], src0_bank = f[15];
    unsigned dest_bank = f[16], src1_bank = f[17], src2_bank = f[18], mask = f[19], dest_n = f[20], src0_n = f[21];
    unsigned src1_n = f[22], src2_n = f[23];
    Type type = (Type)f[12];
    if (move_type == 3) return fail(t, "VMOV move type 3");
    bool doubled = type == T_C10 || type == T_F16 || type == T_F32, conditional = move_type != 0;
    unsigned bits = doubled ? 7 : 6;
    Operand dest = decode_dest(t, dest_n, dest_bank, dest_ext, doubled, bits);
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, doubled, bits);
    if (type == T_F16 || type == T_F32)
        mask = decode_write_mask(dest.bank, mask, type == T_F16);
    else if (!doubled)
        mask = 1;
    if (doubled)
        vec34_swizzle(src1.swizzle, src0_swiz, false, 2);
    else
        memcpy(src1.swizzle, default_swizzle, 4);
    src1.type = dest.type = type;
    Operand src0 = {0}, src2 = {0};
    Type test_type = move_type == 2 ? T_U8 : type;
    unsigned compare = (test2 << 1) | test1; /* 0 == 0, 1 != 0, 2 < 0, 3 <= 0 */
    if (conditional) {
        src0 = decode_src0(t, src0_n, src0_bank, src0_ext, doubled, bits);
        src0.type = test_type;
        src2 = decode_src12(t, src2_n, src2_bank, src2_ext, doubled, bits);
        src2.type = type;
        if (comp_sel) memcpy(src0.swizzle, src1.swizzle, 4);
        memcpy(src2.swizzle, src1.swizzle, 4);
    }
    if (doubled && (type == T_F16 || type == T_F32))
        set_multiplier(t, 2, 2, 2, 2);
    else
        set_multiplier(t, 1, 1, 1, 1);
    for (unsigned r = 0; r <= repeat_count && !t->failed; ++r) {
        Value a = load(t, src1, mask, repeat_offset(t, &src1, r, O_SRC1, R_SLMSI)), result = a;
        if (conditional) {
            Value test = load(t, src0, mask, repeat_offset(t, &src0, r, O_SRC0, R_SLMSI));
            Value b = load(t, src2, mask, repeat_offset(t, &src2, r, O_SRC2, R_SLMSI));
            static const uint32_t float_ops[4] = {SPV_OP_F_ORD_EQUAL, SPV_OP_F_UNORD_NOT_EQUAL, SPV_OP_F_ORD_LESS_THAN,
                                                  SPV_OP_F_ORD_LESS_THAN_EQUAL};
            for (unsigned i = 0; i < a.n && i < test.n; ++i) {
                uint32_t zero = test.kind == K_FLOAT ? cf(t, 0) : test.kind == K_UINT ? cu(t, 0) : ci(t, 0), cond;
                if (test.kind == K_FLOAT)
                    cond = op2(t, compare == 1 ? 182u /* OpFOrdNotEqual */ : float_ops[compare], t->t_bool, test.c[i], zero);
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

/* VMAD (vec3/vec4 multiply-add with the GPI registers): p s g o d e r a i t n w c b f h z k j l m q u v x y A B */
static bool vmad(T* t, const uint32_t* f) {
    unsigned gpi1_swiz_ext = f[2], opcode2 = f[3], dest_ext = f[4], src1_ext = f[6], repeat_mode = f[7];
    unsigned gpi0_abs = f[8], repeat_count = f[9], mask = f[11], src1_neg = f[12], src1_abs = f[13];
    unsigned gpi1_neg = f[14], gpi1_abs = f[15], gpi0_swiz_ext = f[16], dest_bank = f[17], src1_bank = f[18];
    unsigned gpi0_n = f[19], dest_n = f[20], gpi0_swiz = f[21], gpi1_swiz = f[22], gpi1_n = f[23], gpi0_neg = f[24];
    unsigned src1_swiz_ext = f[25], src1_swiz = f[26], src1_n = f[27];
    int type = opcode2 ? 2 : 1;
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, true, 7);
    Operand dest = decode_dest(t, dest_n, dest_bank, dest_ext, true, 7);
    Operand src0 = {gpi0_n, B_INTERNAL, F_GPI, {0}, T_F32}, src2 = {gpi1_n, B_INTERNAL, F_GPI, {0}, T_F32};
    vec34_swizzle(src1.swizzle, src1_swiz, src1_swiz_ext, type);
    vec34_swizzle(src0.swizzle, gpi0_swiz, gpi0_swiz_ext, type);
    vec34_swizzle(src2.swizzle, gpi1_swiz, gpi1_swiz_ext, type);
    src1.flags |= (src1_abs ? F_ABS : 0) | (src1_neg ? F_NEG : 0);
    src0.flags |= (gpi0_abs ? F_ABS : 0) | (gpi0_neg ? F_NEG : 0);
    src2.flags |= (gpi1_abs ? F_ABS : 0) | (gpi1_neg ? F_NEG : 0);
    set_multiplier(t, 2, 2, 2, 4);
    for (unsigned r = 0; r <= repeat_count && !t->failed; ++r) {
        Value a = load(t, src0, mask, repeat_offset(t, &src0, r, O_SRC0, (int)repeat_mode));
        Value b = load(t, src1, mask, repeat_offset(t, &src1, r, O_SRC1, (int)repeat_mode));
        Value c = load(t, src2, mask, repeat_offset(t, &src2, r, O_SRC2, (int)repeat_mode));
        Value result = {{0}, a.n, K_FLOAT};
        for (unsigned i = 0; i < a.n; ++i) result.c[i] = ext3(t, t->t_f32, SPV_GLSL_FMA, a.c[i], b.c[i], c.c[i]);
        store(t, dest, result, mask, repeat_offset(t, &dest, r, O_DEST, (int)repeat_mode));
    }
    reset_repeat_multiplier(t);
    return !t->failed;
}

/* V32NMAD and V16NMAD: p s r y d c b a w n e m o i t k l f z g h j (alu.cpp: v32nmad, do_alu_op) */
static bool vnmad(T* t, const uint32_t* f) {
    unsigned swiz_10_11 = f[2], dest_ext = f[4], swiz_9 = f[5], src1_ext = f[6], src2_ext = f[7], src2_swiz = f[8];
    unsigned mask = f[10], src1_mod = f[11], src2_mod = f[12], swiz_7_8 = f[13], dest_bank = f[14];
    unsigned src1_bank = f[15], src2_bank = f[16], dest_n = f[17], swiz_0_6 = f[18], alu = f[19], src1_n = f[20];
    unsigned src2_n = f[21];
    bool f32 = (t->instr >> 59) & 1;
    Type type = f32 ? T_F32 : T_F16;
    Operand dest = decode_dest(t, dest_n, dest_bank, dest_ext, true, 7);
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, true, 7);
    Operand src2 = decode_src12(t, src2_n, src2_bank, src2_ext, true, 7);
    src1.flags = decode_modifier(src1_mod);
    if (src2_mod == 1) src2.flags = F_ABS;
    dest.type = src1.type = src2.type = type;
    swizzle4(src1.swizzle, swiz_0_6 | swiz_7_8 << 7 | swiz_9 << 9 | swiz_10_11 << 10);
    vec34_swizzle(src2.swizzle, src2_swiz, false, 2);
    unsigned source_mask = alu == 7 ? 15 : mask; /* a dot product reads all four */
    Value a = load(t, src1, source_mask, 0), b = load(t, src2, source_mask, 0);
    if (t->failed) return false;
    Value r = {{0}, a.n, K_FLOAT};
    for (unsigned i = 0; i < a.n; ++i) {
        switch (alu) {
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
    if (alu == 7) { /* DP: the sum of the products, in each masked component */
        uint32_t sum = op2(t, SPV_OP_F_MUL, t->t_f32, a.c[0], b.c[0]);
        for (unsigned i = 1; i < a.n; ++i)
            sum = ext3(t, t->t_f32, SPV_GLSL_FMA, a.c[i], b.c[i], sum);
        r.n = popcount4(mask);
        for (unsigned i = 0; i < r.n; ++i) r.c[i] = sum;
    }
    store(t, dest, r, mask, 0);
    return !t->failed;
}

/* VPCK: p s n u y d e r c a f t m b k l g o h i j q v w x (data.cpp) */
static bool vpck(T* t, const uint32_t* f) {
    unsigned dest_ext = f[5], src1_ext = f[7], src2_ext = f[8], repeat_count = f[9], src_fmt = f[10];
    unsigned dest_fmt = f[11], mask = f[12], dest_bank = f[13], src1_bank = f[14], src2_bank = f[15], dest_n = f[16];
    unsigned sel3 = f[17], scale = f[18], sel1 = f[19], sel2 = f[20], src1_n = f[21], sel0_bit1 = f[22], src2_n = f[23];
    unsigned sel0_bit0 = f[24];
    static const Type types[8] = {T_U8, T_I8, T_O8, T_U16, T_I16, T_F16, T_F32, T_C10};
    Type dest_type = types[dest_fmt], src_type = types[src_fmt];
    Operand dest = decode_dest(t, dest_n, dest_bank, dest_ext, false, 7);
    dest.type = dest_type;
    bool src1_doubled = true;
    if (!is_float_type(src_type)) {
        src1_n = sel0_bit1 | (src1_n << 1);
        src1_doubled = false;
    }
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, src1_doubled, 7);
    Operand src2 = decode_src12(t, src2_n, src2_bank, src2_ext, true, 7);
    src1.type = src_type;
    unsigned sel0 = sel0_bit0 | ((src_type == T_F32 ? sel0_bit1 & 1 : src2_n & 1) << 1);
    uint8_t swizzle[4] = {(uint8_t)sel0, (uint8_t)sel1, (uint8_t)sel2, (uint8_t)sel3};
    memcpy(src1.swizzle, swizzle, 4);
    bool no_cycle = (is_float_type(src_type) && is_float_type(dest_type)) || (scale && (src_type == T_U8 || dest_type == T_U8));
    bool use_src2 = false;
    if (src_type == T_F32 && src2.bank != B_IMM) {
        bool same = src1.num == src2.num && src1.bank == src2.bank;
        bool contiguous = mask == 1 || mask == 3 || mask == 7 || mask == 15, defaulted = true;
        unsigned n = popcount4(mask);
        for (unsigned i = 0; i < n; ++i) defaulted = defaulted && src1.swizzle[i] == i;
        use_src2 = !defaulted || !same || !contiguous;
    }
    if (!no_cycle) {
        uint8_t taken[4];
        memcpy(taken, src1.swizzle, 4);
        bool two = src_type == T_U8 || dest_type == T_U8;
        unsigned next = 0;
        for (unsigned i = 0; i < 4; ++i)
            if (mask & (1u << i)) src1.swizzle[i] = two && mask == 15 ? taken[i % 2] : taken[next++];
    }
    set_multiplier(t, 1, is_float_type(src_type) ? 2 : 1, is_float_type(src_type) ? 2 : 1, 1);
    for (unsigned r = 0; r <= repeat_count && !t->failed; ++r) {
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
                if (mask & (1u << i)) {
                    unsigned ch = src1.swizzle[i];
                    v.c[v.n++] = ch <= SW_W ? all[ch] : swizzle_constant(t, ch, K_FLOAT);
                }
        } else
            v = load(t, src1, mask, o1);
        if (is_float_type(dest_type) && !is_float_type(src_type)) v = to_float(t, v, src_type, scale);
        if (!is_float_type(dest_type) && is_float_type(src_type)) v = to_int(t, v, dest_type, scale);
        if (!is_float_type(dest_type) && !is_float_type(src_type) && is_signed_type(dest_type) != is_signed_type(src_type))
            for (unsigned i = 0; i < v.n; ++i)
                v.c[i] = bitcast(t, is_signed_type(dest_type) ? t->t_i32 : t->t_u32, v.c[i]),
                v.kind = is_signed_type(dest_type) ? K_INT : K_UINT;
        store(t, dest, v, mask, repeat_offset(t, &dest, r, O_DEST, R_SLMSI));
    }
    reset_repeat_multiplier(t);
    return !t->failed;
}
static bool nothing(T* t, const uint32_t* f) {
    (void)t, (void)f;
    return true;
}
/* SMLSI: the repeat increments (special.cpp): n t p s d r c i e a b f */
static bool smlsi(T* t, const uint32_t* f) {
    unsigned modes[4] = {f[5], f[6], f[7], f[4]}, values[4] = {f[9], f[10], f[11], f[8]};
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
static uint32_t sampler_of(T* t, unsigned resource);
static unsigned texture_components(uint32_t format);
static Value vector_value(T* t, uint32_t vec, unsigned n, int kind);
static void store_input(T* t, Bank bank, unsigned offset, Type type, Value v);
static uint32_t zero_of(T* t, int kind) { return kind == K_FLOAT ? cf(t, 0) : kind == K_UINT ? cu(t, 0) : ci(t, 0); }
static void store_predicate(T* t, unsigned n, uint32_t cond) {
    uint32_t pointer = spv_emit_type_pointer(&t->e, SPV_STORAGE_PRIVATE, t->t_bool);
    SPV_VOID(&t->e, SPV_OP_STORE, SPV(&t->e, SPV_OP_ACCESS_CHAIN, pointer, t->bank[B_PRED], cu(t, n & 3)), cond);
}
static Value one_value(uint32_t c, int kind) { return (Value){{c}, 1, kind}; }

/* VMAD2: d p s r y c b a w i n e m o k t f g h z j l q u */
static bool vmad2(T* t, const uint32_t* f) {
    unsigned dat_fmt = f[0], src0_swiz2 = f[3], src0_abs = f[5], src1_ext = f[6], src2_ext = f[7], src2_swiz = f[8];
    unsigned src1_swiz2 = f[9], mask = f[11], src1_mod = f[12], src2_mod = f[13], src0_bank = f[14], dest_bank = f[15];
    unsigned src1_bank = f[16], src2_bank = f[17], dest_n = f[18], src1_swiz01 = f[19], src0_swiz01 = f[20];
    unsigned src0_n = f[21], src1_n = f[22], src2_n = f[23];
    static const uint8_t swizzles[3][8][4] = {
        {S4(X, X, X, X), S4(Y, Y, Y, Y), S4(Z, Z, Z, Z), S4(W, W, W, W), S4(X, Y, Z, W), S4(Y, Z, X, W), S4(X, Y, W, W),
         S4(Z, W, X, Y)},
        {S4(X, X, X, X), S4(Y, Y, Y, Y), S4(Z, Z, Z, Z), S4(W, W, W, W), S4(X, Y, Z, W), S4(X, Y, Y, Z), S4(Y, Y, W, W),
         S4(W, Y, Z, W)},
        {S4(X, X, X, X), S4(Y, Y, Y, Y), S4(Z, Z, Z, Z), S4(W, W, W, W), S4(X, Y, Z, W), S4(X, Z, W, W), S4(X, X, Y, Z),
         S4(X, Y, Z, Z)}};
    Type type = dat_fmt ? T_F16 : T_F32;
    Operand dest = decode_dest(t, dest_n, dest_bank, false, true, 7);
    Operand src0 = decode_src0(t, src0_n, src0_bank, false, true, 7);
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, true, 7);
    Operand src2 = decode_src12(t, src2_n, src2_bank, src2_ext, true, 7);
    mask = decode_write_mask(dest.bank, mask, dat_fmt);
    dest.type = src0.type = src1.type = src2.type = type;
    memcpy(src0.swizzle, swizzles[0][src0_swiz01 | src0_swiz2 << 2], 4);
    memcpy(src1.swizzle, swizzles[1][src1_swiz01 | src1_swiz2 << 2], 4);
    memcpy(src2.swizzle, swizzles[2][src2_swiz], 4);
    if (src0_abs) src0.flags |= F_ABS;
    src1.flags = decode_modifier(src1_mod), src2.flags = decode_modifier(src2_mod);
    Value a = load(t, src0, mask, 0), b = load(t, src1, mask, 0), c = load(t, src2, mask, 0);
    if (t->failed) return false;
    Value r = {{0}, a.n, K_FLOAT};
    for (unsigned i = 0; i < a.n; ++i) r.c[i] = ext3(t, t->t_f32, SPV_GLSL_FMA, a.c[i], b.c[i], c.c[i]);
    store(t, dest, r, mask, 0);
    return !t->failed;
}

/* VDP (a dot product with a GPI register): p s c o d e r a g t n w b f l k h i j z m q y x u. The destination stays,
   the write mask turns one component a repeat */
static bool vdp(T* t, const uint32_t* f) {
    unsigned opcode2 = f[3], dest_ext = f[4], src1_ext = f[6], repeat_mode = f[7], gpi0_abs = f[8];
    unsigned repeat_count = f[9], mask = f[11], src1_neg = f[12], src1_abs = f[13], dest_bank = f[15], src1_bank = f[16];
    unsigned gpi0_n = f[17], dest_n = f[18], gpi0_swiz = f[19], src1_n = f[24];
    static const uint8_t channels[8] = {SW_X, SW_Y, SW_Z, SW_W, SW_0, SW_1, SW_2, SW_H};
    int type = opcode2 ? 2 : 1;
    unsigned source_mask = opcode2 ? 15 : 7;
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, true, 7);
    Operand dest = decode_dest(t, dest_n, dest_bank, dest_ext, true, 7);
    Operand src2 = {gpi0_n, B_INTERNAL, F_GPI, {0}, T_F32};
    vec34_swizzle(src2.swizzle, gpi0_swiz, false, type);
    for (unsigned i = 0; i < 4; ++i) src1.swizzle[i] = channels[f[23 - i]]; /* x, y, z, w */
    src1.flags |= (src1_neg ? F_NEG : 0) | (src1_abs ? F_ABS : 0);
    if (gpi0_abs) src2.flags |= F_ABS;
    for (unsigned r = 0; r <= repeat_count && !t->failed; ++r) {
        Value a = load(t, src1, source_mask, repeat_offset(t, &src1, r, O_SRC1, (int)repeat_mode));
        Value b = load(t, src2, source_mask, repeat_offset(t, &src2, r, O_SRC2, (int)repeat_mode));
        if (t->failed) break;
        uint32_t sum = op2(t, SPV_OP_F_MUL, t->t_f32, a.c[0], b.c[0]);
        for (unsigned i = 1; i < a.n; ++i) sum = ext3(t, t->t_f32, SPV_GLSL_FMA, a.c[i], b.c[i], sum);
        store(t, dest, one_value(sum, K_FLOAT), mask, 0);
        mask = ((mask << 1) | (mask >> 3)) & 15;
    }
    return !t->failed;
}

/* VCOMP (the complex unit: one component's reciprocal, reciprocal square root, base 2 logarithm or exponential,
   in each masked one): p s d y e n r a o b c m f t k g h w */
static bool vcomp(T* t, const uint32_t* f) {
    unsigned dest_type = f[2], dest_ext = f[4], src1_ext = f[6], repeat_count = f[7], op = f[9], src_type = f[10];
    unsigned src1_mod = f[11], src_comp = f[12], dest_bank = f[13], src1_bank = f[14], dest_n = f[15], src1_n = f[16];
    unsigned mask = f[17];
    static const Type types[4] = {T_F32, T_F16, T_C10, T_UNK};
    if (types[dest_type] == T_UNK || types[src_type] == T_UNK) return fail(t, "VCOMP with type 3");
    Operand dest = decode_dest(t, dest_n, dest_bank, dest_ext, true, 8);
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, true, 8);
    src1.flags = decode_modifier(src1_mod);
    src1.type = types[src_type], dest.type = types[dest_type];
    for (unsigned r = 0; r <= repeat_count && !t->failed; ++r) {
        Value v = load(t, src1, 1u << src_comp, repeat_offset(t, &src1, r, O_SRC1, R_SLMSI));
        if (t->failed) break;
        uint32_t x = v.c[0];
        switch (op) {
            case 0: x = op2(t, SPV_OP_F_DIV, t->t_f32, cf(t, 1), x); break;
            case 1: x = ext1(t, t->t_f32, SPV_GLSL_INVERSE_SQRT, x); break;
            case 2: x = ext1(t, t->t_f32, SPV_GLSL_LOG2, x); break;
            default: /* exp2(NaN) is 1, as a game needs */
                x = SPV(&t->e, SPV_OP_SELECT, t->t_f32, op1(t, SPV_OP_IS_NAN, t->t_bool, x), cf(t, 1),
                        ext1(t, t->t_f32, SPV_GLSL_EXP2, x));
                break;
        }
        store(t, dest, one_value(x, K_FLOAT), mask, repeat_offset(t, &dest, r, O_DEST, R_SLMSI));
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
/* VBW (bitwise: and, or, xor, shifts, rotate): o p s n r y d e c x a i t h b w k f g j l m q */
static bool vbw(T* t, const uint32_t* f) {
    unsigned op_1 = f[0], dest_ext = f[6], src1_ext = f[8], src2_ext = f[9], repeat_count = f[10], invert = f[11];
    unsigned rot = f[12], exth = f[13], op_2 = f[14], partial = f[15], dest_bank = f[16], src1_bank = f[17];
    unsigned src2_bank = f[18], dest_n = f[19], src2_sel = f[20], src1_n = f[21], src2_n = f[22];
    enum { AND, OR, XOR, SHL, ROL, SHR, ASR } op;
    switch (op_1) {
        case 2: op = op_2 ? OR : AND; break;
        case 3: op = XOR; break;
        case 4: op = op_2 ? ROL : SHL; break;
        case 5: op = op_2 ? ASR : SHR; break;
        default: return fail(t, "VBW operation %u", op_1);
    }
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, false, 7);
    Operand src2 = decode_src12(t, src2_n, src2_bank, src2_ext, false, 7);
    Operand dest = decode_dest(t, dest_n, dest_bank, dest_ext, false, 7);
    unsigned bits = partial ? 16 : 32;
    src1.type = src2.type = dest.type = partial ? T_U16 : T_U32;
    set_multiplier(t, 1, 1, 1, 1);
    for (unsigned r = 0; r <= repeat_count && !t->failed; ++r) {
        uint32_t b;
        if (src2_ext && src2.bank == B_IMM) { /* the immediate, rotated and inverted as encoded */
            uint32_t value = src2_n | src2_sel << 7 | exth << 14, n = rot & (bits - 1);
            if (n) value = bits == 16 ? ((value << n) | ((value & 0xffff) >> (16 - n))) & 0xffff
                                      : (value << n) | (value >> (32 - n));
            if (invert) value = ~value;
            if (bits == 16) value &= 0xffff;
            b = cu(t, value);
        } else {
            b = load(t, src2, 1, repeat_offset(t, &src2, r, O_SRC2, R_SLMSI)).c[0];
            if (rot) b = rotate_left(t, b, cu(t, rot), bits);
            if (invert) {
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
        {{182u /* OpFOrdNotEqual */, SPV_OP_F_ORD_LESS_THAN, SPV_OP_F_ORD_GREATER_THAN},
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
static void test_sources(T* t, const uint32_t* f, unsigned alu_sel, Type type, bool vscomp, Operand* src1,
                         Operand* src2, unsigned src1_n, unsigned src1_bank, unsigned src1_ext, unsigned src2_n,
                         unsigned src2_bank, unsigned src2_ext) {
    (void)f;
    bool doubled = alu_sel == 0;
    *src1 = decode_src12(t, src1_n, src1_bank, src1_ext, doubled, doubled ? 8 : 7);
    *src2 = decode_src12(t, src2_n, src2_bank, src2_ext, doubled, doubled ? 8 : 7);
    src1->type = src2->type = type;
    if (vscomp && doubled) memset(src2->swizzle, SW_X, 4);
}
/* VTST (a test into a predicate register): p s o y d r c e a v t i z m h n b k f g w l u j q */
static bool vtst(T* t, const uint32_t* f) {
    unsigned src1_neg = f[5], src1_ext = f[6], src2_ext = f[7], prec = f[8], vscomp = f[9], sign_test = f[11];
    unsigned zero_test = f[12], chan = f[14], pdst_n = f[15], src1_bank = f[17], src2_bank = f[18], alu_sel = f[21];
    unsigned alu_op = f[22], src1_n = f[23], src2_n = f[24];
    Type type;
    TestOp op = test_op(alu_sel, alu_op, prec, &type);
    if (op == TO_INVALID || type == T_UNK) return fail(t, "VTST of ALU %u operation %u", alu_sel, alu_op);
    if (chan >= 4) return fail(t, "VTST of channel %u", chan);
    Operand src1, src2;
    test_sources(t, f, alu_sel, type, vscomp, &src1, &src2, src1_n, src1_bank, src1_ext, src2_n, src2_bank, src2_ext);
    if (src1_neg) src1.flags |= F_NEG;
    uint32_t result[4];
    if (!test(t, op, src1, src2, zero_test, sign_test, 1u << chan, result)) return false;
    store_predicate(t, pdst_n, result[0]);
    return true;
}
/* VTSTMSK (a test's result as a mask into a register: all ones or zero): p s o y d t r c e v u i z m a b n k f w l g
   h j */
static bool vtstmsk(T* t, const uint32_t* f) {
    unsigned dest_ext = f[4], src1_neg = f[5], src1_ext = f[6], src2_ext = f[7], prec = f[8], vscomp = f[9];
    unsigned sign_test = f[11], zero_test = f[12], mask_type = f[14], dest_bank = f[15], src1_bank = f[16];
    unsigned src2_bank = f[17], dest_n = f[18], wben = f[19], alu_sel = f[20], alu_op = f[21], src1_n = f[22];
    unsigned src2_n = f[23];
    Type type;
    TestOp op = test_op(alu_sel, alu_op, prec, &type);
    if (op == TO_INVALID || type == T_UNK) return fail(t, "VTSTMSK of ALU %u operation %u", alu_sel, alu_op);
    if (mask_type == 3) return fail(t, "VTSTMSK of mask type 3");
    if (!wben) return fail(t, "VTSTMSK without its write");
    Operand src1, src2;
    test_sources(t, f, alu_sel, type, vscomp, &src1, &src2, src1_n, src1_bank, src1_ext, src2_n, src2_bank, src2_ext);
    if (src1_neg) src1.flags |= F_NEG;
    bool doubled = alu_sel == 0, four = doubled && mask_type == 2;
    Operand dest = decode_dest(t, dest_n, dest_bank, dest_ext, doubled, doubled ? 8 : 7);
    dest.type = type;
    uint32_t result[4];
    unsigned n = test(t, op, src1, src2, zero_test, sign_test, op == TO_DP || four ? 15 : 1, result);
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
/* SOP2 (alu.cpp): p c s n a d e r b m o f l g h i t k j q u v w x y z A. Each source's color (xyz) and alpha (w) as
   unsigned bytes, scaled by a factor each selects, then added, subtracted or the min or max */
static bool sop2(T* t, const uint32_t* f) {
    unsigned cmod1 = f[1], asel1 = f[4], dest_ext = f[5], src1_ext = f[7], src2_ext = f[8], cmod2 = f[9];
    unsigned count = f[10], amod1 = f[11], asel2 = f[12], csel1 = f[13], csel2 = f[14], amod2 = f[15];
    unsigned dest_bank = f[16], src1_bank = f[17], src2_bank = f[18], dest_n = f[19], src1_mod = f[20], cop = f[21];
    unsigned aop = f[22], src1_n = f[25], src2_n = f[26];
    if (csel1 > 4 || csel2 > 4 || asel1 > 2 || asel2 > 2)
        return fail(t, "SOP2 selectors %u %u %u %u", csel1, csel2, asel1, asel2);
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, false, 7);
    Operand src2 = decode_src12(t, src2_n, src2_bank, src2_ext, false, 7);
    Operand dest = decode_dest(t, dest_n, dest_bank, dest_ext, false, 7);
    src1.type = src2.type = dest.type = T_U8;
    for (unsigned r = 0; r <= count && !t->failed; ++r) {
        int o1 = repeat_offset(t, &src1, r, O_SRC1, R_SLMSI), o2 = repeat_offset(t, &src2, r, O_SRC2, R_SLMSI);
        Value c1 = to_float(t, load(t, src1, 7, o1), T_U8, true), c2 = to_float(t, load(t, src2, 7, o2), T_U8, true);
        Value a1 = to_float(t, load(t, src1, 8, o1), T_U8, true), a2 = to_float(t, load(t, src2, 8, o2), T_U8, true);
        if (t->failed) break;
        for (unsigned i = 0; i < 3; ++i) c1.c[i] = complement(t, c1.c[i], src1_mod);
        a1.c[0] = complement(t, a1.c[0], src1_mod);
        Value color = {{0}, 3, K_FLOAT};
        for (unsigned i = 0; i < 3; ++i) {
            uint32_t pick[5] = {cf(t, 0), c1.c[i], c2.c[i], a1.c[0], a2.c[0]};
            uint32_t lhs = op2(t, SPV_OP_F_MUL, t->t_f32, complement(t, pick[csel1], cmod1), c1.c[i]);
            uint32_t rhs = op2(t, SPV_OP_F_MUL, t->t_f32, complement(t, pick[csel2], cmod2), c2.c[i]);
            color.c[i] = sop_apply(t, cop, lhs, rhs);
        }
        uint32_t apick[3] = {cf(t, 0), a1.c[0], a2.c[0]};
        uint32_t lhs = op2(t, SPV_OP_F_MUL, t->t_f32, complement(t, apick[asel1], amod1), a1.c[0]);
        uint32_t rhs = op2(t, SPV_OP_F_MUL, t->t_f32, complement(t, apick[asel2], amod2), a2.c[0]);
        Value alpha = one_value(sop_apply(t, aop, lhs, rhs), K_FLOAT);
        int od = repeat_offset(t, &dest, r, O_DEST, R_SLMSI);
        store(t, dest, to_int(t, color, T_U8, true), 7, od);
        store(t, dest, to_int(t, alpha, T_U8, true), 8, od);
    }
    return !t->failed;
}
/* SOP2M (with a write mask, alpha its first bit): p m s n c d e r b o w a l f t k g u h i */
static bool sop2m(T* t, const uint32_t* f) {
    unsigned mod1 = f[1], cop = f[4], dest_ext = f[5], src1_ext = f[7], src2_ext = f[8], mod2 = f[9], wmask = f[10];
    unsigned aop = f[11], sel1 = f[12], sel2 = f[13], dest_bank = f[14], src1_bank = f[15], src2_bank = f[16];
    unsigned dest_n = f[17], src1_n = f[18], src2_n = f[19];
    /* zero, -, src1, src1's alpha, -, -, src2, src2's alpha */
    if (sel1 == 1 || sel1 == 4 || sel1 == 5 || sel2 == 1 || sel2 == 4 || sel2 == 5)
        return fail(t, "SOP2M selectors %u %u", sel1, sel2);
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, false, 7);
    Operand src2 = decode_src12(t, src2_n, src2_bank, src2_ext, false, 7);
    Operand dest = decode_dest(t, dest_n, dest_bank, dest_ext, false, 7);
    src1.type = src2.type = dest.type = T_U8;
    Value a = to_float(t, load(t, src1, 15, 0), T_U8, true), b = to_float(t, load(t, src2, 15, 0), T_U8, true);
    if (t->failed) return false;
    wmask = ((wmask & 14) >> 1) | ((wmask & 1) << 3);
    Value r = {{0}, 0, K_FLOAT};
    for (unsigned i = 0; i < 4; ++i) {
        if (!(wmask & (1u << i))) continue;
        uint32_t s1[8] = {cf(t, 0), 0, a.c[i], a.c[3], 0, 0, b.c[i], b.c[3]};
        uint32_t x = op2(t, SPV_OP_F_MUL, t->t_f32, complement(t, s1[sel1], mod1), a.c[i]);
        uint32_t y = op2(t, SPV_OP_F_MUL, t->t_f32, complement(t, s1[sel2], mod2), b.c[i]);
        r.c[r.n++] = sop_apply(t, i == 3 ? aop : cop, x, y);
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
/* I32MAD (16-bit sources, a 32-bit result): p s n r c d e b a t i f y k g h j l m o q */
static bool i32mad(T* t, const uint32_t* f) {
    unsigned src0_high = f[1], src1_high = f[3], src2_high = f[4], dest_ext = f[5], src1_ext = f[7], src2_ext = f[8];
    unsigned repeat_count = f[9], is_signed = f[10], src2_type = f[12], src0_bank = f[13], dest_bank = f[14];
    unsigned src1_bank = f[15], src2_bank = f[16], dest_n = f[17], src0_n = f[18], src1_n = f[19], src2_n = f[20];
    Operand dest = decode_dest(t, dest_n, dest_bank, dest_ext, false, 7);
    Operand src0 = decode_src0(t, src0_n, src0_bank, false, false, 7);
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, false, 7);
    Operand src2 = decode_src12(t, src2_n, src2_bank, src2_ext, false, 7);
    Type wide = is_signed ? T_I32 : T_U32, narrow = is_signed ? T_I16 : T_U16;
    dest.type = wide, src0.type = src1.type = narrow, src2.type = src2_type == 2 ? wide : narrow;
    unsigned m0 = src0_high ? 2 : 1, m1 = src1_high ? 2 : 1, m2 = src2_type != 2 && src2_high ? 2 : 1;
    set_multiplier(t, 1, 1, 1, 1);
    for (unsigned r = 0; r <= repeat_count && !t->failed; ++r) {
        Value a = load(t, src0, m0, repeat_offset(t, &src0, r, O_SRC0, R_SLMSI));
        Value b = load(t, src1, m1, repeat_offset(t, &src1, r, O_SRC1, R_SLMSI));
        Value c = load(t, src2, m2, repeat_offset(t, &src2, r, O_SRC2, R_SLMSI));
        if (t->failed) break;
        store(t, dest, imad(t, a, b, c), 1, repeat_offset(t, &dest, r, O_DEST, R_SLMSI));
    }
    reset_repeat_multiplier(t);
    return !t->failed;
}
/* I16MAD: p a s n r e d b c k t m f o l h g i j q u v w x */
static bool i16mad(T* t, const uint32_t* f) {
    unsigned sel1h = f[5], dest_ext = f[6], src1_ext = f[8], src2_ext = f[9], repeat_count = f[10], mode = f[11];
    unsigned src2_format = f[12], src1_format = f[13], sel2h = f[14], src0_bank = f[16], dest_bank = f[17];
    unsigned src1_bank = f[18], src2_bank = f[19], dest_n = f[20], src0_n = f[21], src1_n = f[22], src2_n = f[23];
    bool is_signed = mode >= 2;
    Type type = is_signed ? T_I16 : T_U16, bytes = is_signed ? T_I8 : T_U8;
    Operand dest = decode_dest(t, dest_n, dest_bank, dest_ext, false, 7);
    Operand src0 = decode_src0(t, src0_n, src0_bank, false, false, 7);
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, false, 7);
    Operand src2 = decode_src12(t, src2_n, src2_bank, src2_ext, false, 7);
    dest.type = src0.type = src1.type = src2.type = type;
    unsigned m1 = 1, m2 = 1;
    if (src1_format > 1 || src2_format > 1) return fail(t, "I16MAD of sign-extended bytes");
    if (src1_format) src1.type = bytes, m1 = sel1h ? 2 : 1;
    if (src2_format) src2.type = bytes, m2 = sel2h ? 2 : 1;
    for (unsigned r = 0; r <= repeat_count && !t->failed; ++r) {
        Value a = load(t, src0, 1, repeat_offset(t, &src0, r, O_SRC0, R_SLMSI));
        Value b = load(t, src1, m1, repeat_offset(t, &src1, r, O_SRC1, R_SLMSI));
        Value c = load(t, src2, m2, repeat_offset(t, &src2, r, O_SRC2, R_SLMSI));
        if (t->failed) break;
        store(t, dest, imad(t, a, b, c), 1, repeat_offset(t, &dest, r, O_DEST, R_SLMSI));
    }
    return !t->failed;
}
/* I32MAD2 (32-bit sources): p n s d e r c b o i g a k t f h j l m q */
static bool i32mad2(T* t, const uint32_t* f) {
    unsigned sn = f[2], dest_ext = f[3], src1_ext = f[5], src2_ext = f[6], src0_ext = f[7], count = f[8];
    unsigned is_signed = f[9], neg1 = f[10], neg2 = f[11], src0_bank = f[12], dest_bank = f[13], src1_bank = f[14];
    unsigned src2_bank = f[15], dest_n = f[16], src0_n = f[17], src1_n = f[18], src2_n = f[19];
    Operand dest = decode_dest(t, dest_n, dest_bank, dest_ext, false, 7);
    Operand src0 = decode_src0(t, src0_n, src0_bank, src0_ext, false, 7);
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, false, 7);
    Operand src2 = decode_src12(t, src2_n, src2_bank, src2_ext, false, 7);
    dest.type = src0.type = src1.type = src2.type = is_signed ? T_I32 : T_U32;
    if (neg1) src1.flags |= F_NEG;
    if (neg2) src2.flags |= F_NEG;
    set_multiplier(t, 1, 1, 1, 1);
    for (unsigned r = 0; r <= count && !t->failed; ++r) {
        Value a = load(t, src0, 1, repeat_offset(t, &src0, r, O_SRC0, R_SLMSI));
        Value b = load(t, src1, 1, repeat_offset(t, &src1, r, O_SRC1, R_SLMSI));
        Value c = load(t, src2, 1, repeat_offset(t, &src2, r, O_SRC2, R_SLMSI));
        if (t->failed) break;
        /* sn 1 passes src2 through (Vita3K's hardware tests) */
        store(t, dest, sn ? c : imad(t, a, b, c), 1, repeat_offset(t, &dest, r, O_DEST, R_SLMSI));
    }
    reset_repeat_multiplier(t);
    return !t->failed;
}

/* ---- loads (data.cpp) ---- */
/* VLDST: o p s n m y c r b a k d e t g f i h j l q u v w x z. A load of 32-bit words from a uniform buffer read from
   memory, at src0 + src1 (+ 4 without moe expansion) + src2 bytes, src0 a register the program's uniform buffer
   table filled; one from the literal buffer, constants from the program */
static bool vldst(T* t, const uint32_t* f) {
    unsigned op = f[0], moe_expand = f[4], src0_ext = f[7], src1_ext = f[8], src2_ext = f[9], mask_count = f[10];
    unsigned primattr = f[13], data_type = f[15], src0_bank = f[17], src1_bank = f[20], src2_bank = f[21];
    unsigned dest_n = f[22], src0_n = f[23], src1_n = f[24], src2_n = f[25];
    if (op != 1) return fail(t, op == 2 ? "VLDST stores" : "VLDST operation %u", op);
    if (data_type > 2) return fail(t, "VLDST of data type %u", data_type);
    unsigned size = data_type == 0 ? 4 : data_type == 1 ? 2 : 1, bytes = size * (mask_count + 1);
    Operand src0 = decode_src0(t, src0_n, src0_bank, src0_ext, false, 7);
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, false, 7);
    Operand src2 = decode_src12(t, src2_n, src2_bank, src2_ext, false, 7);
    src0.type = src1.type = src2.type = T_I32;
    Bank bank = t->secondary ? B_SA : primattr ? B_PA : B_TEMP;
    if (src1.bank == B_IMM) src1.num *= size;
    if (src0.bank == B_SA && (int)src0.num == t->literal_register) {
        if (mask_count || src1.bank != B_IMM || src2.bank != B_IMM) { /* at a register's offset: from the array */
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
                store_word(t, bank, dest_n + i, SPV(&t->e, SPV_OP_LOAD, t->t_u32,
                                                    SPV(&t->e, SPV_OP_ACCESS_CHAIN, pointer, t->literals, at)));
            }
            return !t->failed;
        }
        uint32_t offset = t->literal_base + src1.num + src2.num;
        const unsigned char* data = gxp_at(t->gxp, GXP_LITERAL_DATA);
        if ((size_t)(data - t->gxp) + offset + 4 > t->size) return fail(t, "a literal past the program");
        store_word(t, bank, dest_n, cu(t, snx_ld_le32(data + offset)));
        return !t->failed;
    }
    if (!t->buffer) return fail(t, "a VLDST with no uniform buffer");
    uint32_t a0 = load(t, src0, 1, 0).c[0], a1 = load(t, src1, 1, 0).c[0], a2 = load(t, src2, 1, 0).c[0];
    if (t->failed) return false;
    if (src1.bank != B_IMM) a1 = op2(t, SPV_OP_I_SUB, t->t_i32, a1, ci(t, 0x10000));
    if (!moe_expand) a1 = op2(t, SPV_OP_I_ADD, t->t_i32, a1, ci(t, 4));
    uint32_t address = bitcast(t, t->t_u32, op2(t, SPV_OP_I_ADD, t->t_i32, a0, op2(t, SPV_OP_I_ADD, t->t_i32, a1, a2)));
    uint32_t index = op2(t, SPV_OP_SHIFT_RIGHT_LOGICAL, t->t_u32, address, cu(t, 2));
    uint32_t pointer = spv_emit_type_pointer(&t->e, SPV_STORAGE_STORAGE_BUFFER, t->t_u32);
    for (unsigned i = 0; i < bytes / 4; ++i) {
        uint32_t at = op2(t, SPV_OP_I_ADD, t->t_u32, index, cu(t, i));
        uint32_t w = SPV(&t->e, SPV_OP_LOAD, t->t_u32, SPV(&t->e, SPV_OP_ACCESS_CHAIN, pointer, t->buffer, cu(t, 0), at));
        store_word(t, bank, dest_n + i, w);
    }
    return !t->failed;
}
/* LIMM (a 32-bit immediate into a register): s n d e i p m t u v */
static bool limm(T* t, const uint32_t* f) {
    uint32_t value = f[9] | f[6] << 21 | f[4] << 26;
    Operand dest = decode_dest(t, f[8], f[7], f[2], false, 7);
    dest.type = T_U32;
    store(t, dest, one_value(cu(t, value), K_UINT), 1, 0);
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
    uint32_t image_type = spv_emit_type_image(&t->e, t->t_f32, SPV_DIM_2D, 0, 0, 0, 1, 0);
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
/* SMP: p s n y m r c e f a d l t b g k h i j o q u v. The texture is the dependent sampler src1 names, read at src0
   (one or two components), its LOD from src2 when lod_mode says */
static bool smp(T* t, const uint32_t* f) {
    unsigned src0_ext = f[5], src1_ext = f[6], src2_ext = f[7], fconv = f[8], dim = f[10] + 1, lod_mode = f[11];
    unsigned use_pa = f[12], sb_mode = f[13], src0_type = f[14], src0_bank = f[15], src1_bank = f[17];
    unsigned src2_bank = f[18], dest_n = f[19], src0_n = f[20], src1_n = f[21], src2_n = f[22];
    Operand src0 = decode_src0(t, src0_n, src0_bank, src0_ext, true, 8);
    Operand src1 = decode_src12(t, src1_n, src1_bank, src1_ext, true, 8);
    src0.type = src0_type == 0 ? T_F32 : src0_type == 1 ? T_F16 : T_C10;
    if (src1.bank != B_SA || src1.num >= BANK_WORDS || !t->dependent[src1.num])
        return fail(t, "an SMP whose sampler isn't a dependent one");
    if (sb_mode == 2) return fail(t, "SMP queries");
    if (dim == 3) return fail(t, "SMP of cube textures");
    unsigned resource = t->dependent[src1.num] - 1u;
    uint32_t format = t->hints->texture_formats[resource];
    Type type = fconv == 2 ? T_F16 : fconv == 3 ? T_F32 : texture_component_type(format);
    Value c = load(t, src0, dim == 1 ? 1 : 3, 0);
    if (t->failed) return false;
    if (c.kind != K_FLOAT) c = to_float(t, c, src0.type, true);
    uint32_t coord = SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v2f, c.c[0], dim == 1 ? cf(t, 0) : c.c[1]);
    uint32_t sampler = sampler_of(t, resource), sampled;
    Bank bank = use_pa ? (t->secondary ? B_SA : B_PA) : B_TEMP;
    if (sb_mode) /* the four texels' components: each component of the four, then the next component's */
        return gather(t, sampler, coord, format, type, bank, dest_n, sb_mode == 3);
    Operand src2 = decode_src12(t, src2_n, src2_bank, src2_ext, true, 8);
    src2.type = src0.type;
    bool implicit = t->fragment && (lod_mode == 0 || lod_mode == 1);
    if (lod_mode == 3) { /* gradients */
        Value dx = load(t, src2, 3, 0), dy = load(t, src2, 12, 0);
        if (t->failed) return false;
        uint32_t gx = SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v2f, dx.c[0], dx.c[1]);
        uint32_t gy = SPV(&t->e, SPV_OP_COMPOSITE_CONSTRUCT, t->t_v2f, dy.c[0], dy.c[1]);
        sampled = SPV(&t->e, SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD, t->t_v4f, sampler, coord, 4u /* Grad */, gx, gy);
    } else if (lod_mode) {
        uint32_t lod = load(t, src2, 1, 0).c[0];
        if (t->failed) return false;
        sampled = implicit ? SPV(&t->e, SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, t->t_v4f, sampler, coord, 1u /* Bias */, lod)
                           : SPV(&t->e, SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD, t->t_v4f, sampler, coord, 2u /* Lod */,
                                 lod_mode == 1 ? cf(t, 0) : lod);
    } else
        sampled = implicit ? SPV(&t->e, SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, t->t_v4f, sampler, coord)
                           : SPV(&t->e, SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD, t->t_v4f, sampler, coord, 2u, cf(t, 0));
    store_input(t, bank, dest_n, type, vector_value(t, sampled, texture_components(format), K_FLOAT));
    return !t->failed;
}

/* ---- the special instructions (special.cpp) ---- */
/* KILL: the fragment is discarded (predicated, the program's if around it) */
static bool kill(T* t, const uint32_t* f) {
    (void)f;
    if (!t->fragment) return fail(t, "KILL in a vertex program");
    spv_emit_void_op(&t->e, SPV_OP_KILL, NULL, 0);
    t->terminated = 1;
    return true;
}
/* DEPTHF: s r e c b n p t f a k d g h i j. src0 is the fragment's depth */
static bool depthf(T* t, const uint32_t* f) {
    Operand src0 = decode_src0(t, f[13], f[9], f[1], true, 7);
    src0.type = T_F32;
    if (!t->frag_depth) return fail(t, "DEPTHF in a program that doesn't say it writes depth");
    uint32_t depth = load(t, src0, 1, 0).c[0];
    if (!t->failed) SPV_VOID(&t->e, SPV_OP_STORE, t->frag_depth, depth);
    return !t->failed;
}
/* SMBO: the repeats' base offsets: n d s r c */
static bool smbo(T* t, const uint32_t* f) {
    unsigned offsets[4] = {f[2], f[3], f[4], f[1]};
    for (unsigned k = 0; k < 4; ++k)
        for (int i = 0; i < 17; ++i) t->repeat_increase[k][i] = i + (int)offsets[k];
    return true;
}

/* the instructions' bit patterns (usse_translator_entry.cpp), in its order: the first match is the instruction */
typedef struct {
    const char* name;
    const char* bits;
    Handler handler;
    uint64_t mask, value;
    unsigned fields;
    uint8_t shift[32], width[32];
} Pattern;
static Pattern patterns[] = {
    {.name = "VMAD2", .bits = "00000dpps-ry-cbawwwineeeemmookttffgghhhhhhzzjjllllllqqqqqquuuuuu", .handler = vmad2},
    {.name = "V32NMAD", .bits = "00001pppsrrydcbawwwwneeeemmoiittkkllffffffzzzzzzzggghhhhhhjjjjjj", .handler = vnmad},
    {.name = "V16NMAD", .bits = "00010pppsrrydcbawwwwneeeemmoiittkkllffffffzzzzzzzggghhhhhhjjjjjj", .handler = vnmad},
    {.name = "VMAD", .bits = "00011pppsg1oderaaittnwwwwcbfhzkkjjllmmmmmmqqqquuuuvvxyAAAABBBBBB", .handler = vmad},
    {.name = "VDP", .bits = "00011pppsc0oderaagttnwwwwbflllkkhhiijjjjjjzzzzmmmqqqyyyxxxuuuuuu", .handler = vdp},
    {.name = "VDUAL", .bits = "0010cgsskdtpuuuunaaalriiiiwwwwmmffeebbbbbbbooohhjqvvxxyyyzzzzzzz", .handler = NULL},
    {.name = "VCOMP", .bits = "00110pppsddyenr-aaaaobbccmmff-ttkk--ggggggg-------hhhhhhh---wwww", .handler = vcomp},
    {.name = "VMOV", .bits = "00111pppstrydecbmmaanoooiwwwwkllffgghhhhjjjjjjqqqqqquuuuuuvvvvvv", .handler = vmov},
    {.name = "VPCK", .bits = "01000pppsnuydercaaaaffftttmmmmbbkkllgggggggoohiijjqqqqqqvwwwwwwx", .handler = vpck},
    {.name = "VTST", .bits = "01001ppps-oydrceavttiizzmhhhnnbbkkffgggggggwlluuuujjjjjjjqqqqqqq", .handler = vtst},
    {.name = "VTSTMSK", .bits = "01111ppps-oydtrcevuuiizzm-aa--bbnnkkfffffffwllgggghhhhhhhjjjjjjj", .handler = vtstmsk},
    {.name = "VBW", .bits = "01ooopppsnrydecxaaaaittttthhbwkkffggjjjjjjjlllllllmmmmmmmqqqqqqq", .handler = vbw},
    {.name = "SOP2", .bits = "10000ppcsnaaderbmooofllggghhhittkkjjqqqqqqquvvwwxyzzzzzzzAAAAAAA", .handler = sop2},
    {.name = "SOP2M", .bits = "10010ppmsnccderbowwwwaalllfff-ttkkgguuuuuuu-------hhhhhhhiiiiiii", .handler = sop2m},
    {.name = "SOP3", .bits = "10001ppcsnooderbmallfgghhhiiikttjjqquuuuuuuvvvvvvvwwwwwwwxxxxxxx", .handler = NULL},
    {.name = "I8MAD", .bits = "10011ppcsneedarbmtttuolfghijkqvvwwxxyyyyyyyzzzzzzzAAAAAAABBBBBBB", .handler = NULL},
    {.name = "I16MAD", .bits = "10100ppasnredbck-tttmmffoolhhgiijjqquuuuuuuvvvvvvvwwwwwwwxxxxxxx", .handler = i16mad},
    {.name = "I32MAD", .bits = "10101pps-nrcdeba0tttif00yy000kgghhjjlllllllmmmmmmmoooooooqqqqqqq", .handler = i32mad},
    {.name = "ILLEGAL22", .bits = "10110-----------------------------------------------------------", .handler = NULL},
    {.name = "ILLEGAL23", .bits = "10111-----------------------------------------------------------", .handler = NULL},
    {.name = "ILLEGAL24", .bits = "11000-----------------------------------------------------------", .handler = NULL},
    {.name = "I8MAD2", .bits = "11001-----------------------------------------------------------", .handler = NULL},
    {.name = "I32MAD2", .bits = "11010ppp-nssdercbooo00iga0000kttffhhjjjjjjjlllllllmmmmmmmqqqqqqq", .handler = i32mad2},
    {.name = "ILLEGAL27", .bits = "11011-----------------------------------------------------------", .handler = NULL},
    {.name = "SMP", .bits = "11100pppsn-ymrceffaaddlltbbggkhhiijjoooooooqqqqqqquuuuuuuvvvvvvv", .handler = smp},
    {.name = "PHAS", .bits = "11111010s100eirc--matwwwppppppppbbnn--------xxxxxxoooooooddddddd", .handler = nothing},
    {.name = "NOP", .bits = "11111----000-----------101--------------------------------------", .handler = nothing},
    {.name = "BR", .bits = "11111ppps000e-----wynba00r----------------iloooooooooooooooooooo", .handler = nothing},
    {.name = "SMLSI", .bits = "11111010--01-n--ttttppppssssdrcieeeeeeeeaaaaaaaabbbbbbbbffffffff", .handler = smlsi},
    {.name = "SMBO", .bits = "11111011--01-n--ddddddddddddssssssssssssrrrrrrrrrrrrcccccccccccc", .handler = smbo},
    {.name = "KILL", .bits = "11111001--11000000000pp0000001101111----------------------------", .handler = kill},
    {.name = "LIMM", .bits = "11111100sn10deiiiiiipppmmmmm--tt----uuuuuuuvvvvvvvvvvvvvvvvvvvvv", .handler = limm},
    {.name = "DEPTHF", .bits = "11111011s-11recb----npp---tffa--kkddggggggghhhhhhhiiiiiiijjjjjjj", .handler = depthf},
    {.name = "SPEC", .bits = "11111----scc----------------------------------------------------", .handler = nothing},
    {.name = "VLDST", .bits = "111oopppsnmycrbakkkkddeetgffihjlqquuvvvvvvvwwwwwwwxxxxxxxzzzzzzz", .handler = vldst},
};
static void patterns_init(void) {
    static bool ready;
    if (ready) return;
    for (size_t p = 0; p < sizeof patterns / sizeof *patterns; ++p) {
        Pattern* x = &patterns[p];
        char last = 0;
        for (unsigned i = 0; i < 64; ++i) {
            char c = x->bits[i];
            unsigned bit = 63 - i;
            if (c == '0' || c == '1') {
                x->mask |= 1ull << bit;
                if (c == '1') x->value |= 1ull << bit;
            }
            if (c == '0' || c == '1' || c == '-') {
                last = 0;
                continue;
            }
            if (c != last) x->fields++;
            last = c;
            x->shift[x->fields - 1] = (uint8_t)bit; /* the field's lowest bit so far */
            x->width[x->fields - 1]++;
        }
    }
    ready = true;
}
static const Pattern* match(uint64_t w) {
    for (size_t p = 0; p < sizeof patterns / sizeof *patterns; ++p)
        if ((w & patterns[p].mask) == patterns[p].value) return &patterns[p];
    return NULL;
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
/* one instruction: its handler, in an if when it is predicated. False when the program can't be translated */
static bool translate_one(T* t, uint64_t w, unsigned pc) {
    t->instr = w, t->pc = pc, t->terminated = 0;
    const Pattern* p = match(w);
    if (!p) return fail(t, "instruction %016llx (at %u) matches no pattern", (unsigned long long)w, pc);
    if (!p->handler) return fail(t, "%s (%016llx at %u) is not translated yet", p->name, (unsigned long long)w, pc);
    uint32_t fields[32];
    for (unsigned i = 0; i < p->fields; ++i) fields[i] = (uint32_t)((w >> p->shift[i]) & ((1ull << p->width[i]) - 1));
    unsigned pred = p->handler != nothing && p->handler != smlsi ? predicate(w) : 0;
    if (pred == 7) return fail(t, "the PN predicate (%s at %u)", p->name, pc);
    if (pred == 8) return fail(t, "the NEGP2 predicate (%s at %u)", p->name, pc);
    if (!pred) {
        p->handler(t, fields);
        if (t->terminated) spv_emit_label(&t->e, spv_emit_id(&t->e)); /* what follows is unreachable */
        return !t->failed;
    }
    uint32_t yes = spv_emit_id(&t->e), merge = spv_emit_id(&t->e);
    uint32_t cond = predicate_value(t, pred);
    SPV_VOID(&t->e, SPV_OP_SELECTION_MERGE, merge, 0);
    SPV_VOID(&t->e, SPV_OP_BRANCH_CONDITIONAL, cond, yes, merge);
    spv_emit_label(&t->e, yes);
    p->handler(t, fields);
    if (!t->terminated) SPV_VOID(&t->e, SPV_OP_BRANCH, merge);
    spv_emit_label(&t->e, merge);
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
    SPV_VOID(e, SPV_OP_LOOP_MERGE, merge, cont, 0);
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
    SPV_VOID(e, SPV_OP_SELECTION_MERGE, cases_merge, 0);
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
/* texture resource's combined image sampler, loaded (its variable made at its first use) */
static uint32_t sampler_of(T* t, unsigned resource) {
    uint32_t image = spv_emit_type_image(&t->e, t->t_f32, SPV_DIM_2D, 0, 0, 0, 1, 0);
    uint32_t sampled_type = spv_emit_type_sampled_image(&t->e, image);
    if (!t->samplers[resource]) {
        t->samplers[resource] =
            spv_emit_resource(&t->e, SPV_STORAGE_UNIFORM_CONSTANT, sampled_type, 0, USSE_TEXTURE_BINDING + resource);
        global(t, t->samplers[resource]);
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
            uint32_t sampler = sampler_of(t, resource);
            if (project >= 0) {
                uint32_t xyp = SPV(&t->e, SPV_OP_VECTOR_SHUFFLE, t->t_v3f, coord, coord, 0, 1, (uint32_t)project);
                sampled = SPV(&t->e, SPV_OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD, t->t_v4f, sampler, xyp);
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
    patterns_init();
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
        spv_emit_capability(e, 40 /* InputAttachment */);
        uint32_t image = spv_emit_type_image(e, t->t_f32, 6 /* SubpassData */, 0, 0, 0, 2, 0);
        uint32_t color = spv_emit_resource(e, SPV_STORAGE_UNIFORM_CONSTANT, image, 0, USSE_COLOR_BINDING);
        const uint32_t index = 0;
        spv_emit_decorate(e, color, 43 /* InputAttachmentIndex */, &index, 1);
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
    uint64_t h = 0xcbf29ce484222325ull;
    const unsigned char* parts[2] = {(const unsigned char*)hints, program};
    size_t sizes[2] = {sizeof *hints, size};
    for (unsigned k = 0; k < 2; ++k)
        for (size_t i = 0; i < sizes[k]; ++i) h = (h ^ parts[k][i]) * 0x100000001b3ull;
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
    for (uint32_t pc = 0; pc < count; ++pc) {
        uint64_t w = (uint64_t)snx_ld_le32(code + pc * 8) | (uint64_t)snx_ld_le32(code + pc * 8 + 4) << 32;
        const Pattern* p = match(w);
        each(c, p ? p->name : NULL, p && p->handler);
    }
}
void usse_survey(const unsigned char* gxp, size_t size, void (*each)(void* context, const char* name, bool known),
                 void* context) {
    if (size < 160 || snx_ld_le32(gxp) != GXP_MAGIC) return;
    patterns_init();
    const unsigned char *start = gxp_at(gxp, GXP_SECONDARY), *end = gxp_at(gxp, GXP_SECONDARY_END);
    if (end > start && (size_t)(end - gxp) <= size) survey_code(start, (uint32_t)((end - start) / 8), each, context);
    uint32_t primary = snx_ld_le32(gxp + GXP_PRIMARY_COUNT);
    const unsigned char* code = gxp_at(gxp, GXP_PRIMARY);
    if ((size_t)(code - gxp) + primary * 8ull <= size) survey_code(code, primary, each, context);
}
