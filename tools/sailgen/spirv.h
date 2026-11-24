/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen --gpu's SPIR-V back end: what every GPU ISA's emitters share, Sail's values and operators as SPIR-V
   (common/utilities/spirv_emit.h), copied at the top of the file sailgen writes. The ISA's own file follows it: its
   context (struct SgG, whose sg_spv gives this file's part), its state and its operands, over its translator's
   accessors. The written file is included by that translator, after them.

   A run-time value of Sail (SgV) is a SPIR-V id and its type. Bits of up to 32 are a u32, the bits past the width
   zero; bits of 33 to 64 a u64 (the Int64 capability, sg_spv's t_u64 makes the type when first asked); a bool a
   bool. A value an ISA's function made as a float or a signed int (an f32 or an i32) keeps that type until an
   operation wants another, which then bitcasts it: the translators' accessors work so, and the generated emitters
   make the same instructions as theirs. Each operator here is the one SPIR-V instruction the hand-written emitters
   use for it, so that the module is the same */
#ifndef SNX_SAILGEN_SPIRV_H
#define SNX_SAILGEN_SPIRV_H
#include "common/utilities/spirv_emit.h"
#include <stdint.h>
#include <string.h>

typedef struct SgG SgG;
/* SGT_OTHER: an opaque value (gpu.sail's half pairs, an ISA's vectors), of the SPIR-V type ty */
enum { SGT_U32, SGT_I32, SGT_F32, SGT_U64, SGT_BOOL, SGT_OTHER };
typedef struct {
    uint32_t id;
    uint8_t t;
    uint32_t ty;
} SgV;
typedef struct {
    SpvEmitter* e;
    uint32_t t_u32, t_i32, t_f32, t_bool, glsl, true_v, false_v;
    uint32_t (*t_u64)(SgG* g);
    /* the bitcasts made for the instruction being written, so that a value cast again is the cast made: the ISA's
       file empties it at each instruction */
    struct {
        uint32_t from, to;
        uint8_t t;
    } cast[16];
    unsigned ncast;
    /* the ISA's translator folds the true and false constants out of its booleans' operations and selects (as
       spirv_translate.c's cc_and, cc_or, cc_not, cc_xor and shader_select do), and its Sail's are folded alike */
    int fold;
    /* its translator bitcasts a value each time an operation takes it as another type: no cast is taken again */
    int fresh_casts;
    /* its translator makes its 32-bit constants and its types with spv_emit_const_* (the module's table of what it
       declared, as the 3DS's vsgen.c and the Vita's usse.c do), not spv_emit_constant_*: its Sail's are made there
       too, so that a constant both make is one */
    int known;
} SgSpv;
static SgSpv* sg_spv(SgG* g); /* the ISA's file gives it */

/* one instruction of two operands: any opcode (spv_emit_binary takes some only), the same words */
static inline uint32_t sg_op2(SpvEmitter* e, uint32_t op, uint32_t type, uint32_t a, uint32_t b) {
    return SPV(e, op, type, a, b);
}
static inline SgV sg_v(uint32_t id, int t) {
    SgV v = {id, (uint8_t)t, 0};
    return v;
}
static inline SgV sg_vt(uint32_t id, uint32_t type) {
    SgV v = {id, SGT_OTHER, type};
    return v;
}
static inline uint32_t sg_type(SgG* g, int t) {
    SgSpv* s = sg_spv(g);
    switch (t) {
        case SGT_I32: return s->t_i32;
        case SGT_F32: return s->t_f32;
        case SGT_U64: return s->t_u64(g);
        case SGT_BOOL: return s->t_bool;
        default: return s->t_u32;
    }
}
/* v as type t: its id, bitcast when it has another 32-bit type */
static inline uint32_t sg_as(SgG* g, SgV v, int t) {
    SgSpv* s = sg_spv(g);
    unsigned i;
    uint32_t id;
    if (v.t == t || v.t == SGT_U64 || v.t == SGT_BOOL || v.t == SGT_OTHER || t == SGT_U64 || t == SGT_BOOL) return v.id;
    for (i = 0; i < s->ncast; ++i)
        if (s->cast[i].from == v.id && s->cast[i].t == t) return s->cast[i].to;
    id = spv_emit_unary(s->e, SPV_OP_BITCAST, sg_type(g, t), v.id);
    if (s->fresh_casts) return id;
    if (s->ncast < 16) {
        s->cast[s->ncast].from = v.id;
        s->cast[s->ncast].t = (uint8_t)t;
        s->cast[s->ncast++].to = id;
        /* and back: the cast of the cast is the value */
        if (s->ncast < 16) {
            s->cast[s->ncast].from = id;
            s->cast[s->ncast].t = v.t;
            s->cast[s->ncast++].to = v.id;
        }
    }
    return id;
}
static inline uint32_t sg_u32(SgG* g, SgV v) { return sg_as(g, v, SGT_U32); }
static inline int sg_wide(int w) { return w > 32; }

static inline SgV sg_const(SgG* g, int w, uint64_t value) {
    SgSpv* s = sg_spv(g);
    if (sg_wide(w)) return sg_v(spv_emit_constant64(s->e, s->t_u64(g), value), SGT_U64);
    if (s->known) return sg_v(spv_emit_const_u32(s->e, (uint32_t)value), SGT_U32);
    return sg_v(spv_emit_constant_u32(s->e, s->t_u32, (uint32_t)value), SGT_U32);
}
static inline SgV sg_bool(SgG* g, int b) {
    SgSpv* s = sg_spv(g);
    return sg_v(b ? s->true_v : s->false_v, SGT_BOOL);
}

/* bits of width w below the type's: the bits past w made zero again */
static inline SgV sg_trim(SgG* g, int w, SgV v) {
    if (w == 32 || w == 64) return v;
    return sg_v(sg_op2(sg_spv(g)->e, SPV_OP_BITWISE_AND, sg_type(g, v.t), v.id,
                                sg_const(g, w, w >= 64 ? ~0ull : (1ull << w) - 1).id),
                v.t);
}
static inline SgV sg_bin(SgG* g, int w, uint32_t op, SgV a, SgV b) {
    int t = sg_wide(w) ? SGT_U64 : SGT_U32;
    uint32_t x = sg_as(g, a, t), y = sg_as(g, b, t);
    return sg_v(sg_op2(sg_spv(g)->e, op, sg_type(g, t), x, y), t);
}
static inline SgV sg_add(SgG* g, int w, SgV a, SgV b) {
    if (!sg_wide(w)) {
        uint32_t x = sg_u32(g, a), y = sg_u32(g, b);
        return sg_trim(g, w, sg_v(spv_emit_iadd_u32(sg_spv(g)->e, sg_spv(g)->t_u32, x, y), SGT_U32));
    }
    return sg_trim(g, w, sg_bin(g, w, SPV_OP_I_ADD, a, b));
}
static inline SgV sg_sub(SgG* g, int w, SgV a, SgV b) { return sg_trim(g, w, sg_bin(g, w, SPV_OP_I_SUB, a, b)); }
static inline SgV sg_mul(SgG* g, int w, SgV a, SgV b) { return sg_trim(g, w, sg_bin(g, w, SPV_OP_I_MUL, a, b)); }
static inline SgV sg_and(SgG* g, int w, SgV a, SgV b) { return sg_bin(g, w, SPV_OP_BITWISE_AND, a, b); }
static inline SgV sg_or(SgG* g, int w, SgV a, SgV b) { return sg_bin(g, w, SPV_OP_BITWISE_OR, a, b); }
static inline SgV sg_xor(SgG* g, int w, SgV a, SgV b) { return sg_bin(g, w, SPV_OP_BITWISE_XOR, a, b); }
static inline SgV sg_not(SgG* g, int w, SgV a) {
    int t = sg_wide(w) ? SGT_U64 : SGT_U32;
    uint32_t x = sg_as(g, a, t);
    return sg_trim(g, w, sg_v(spv_emit_unary(sg_spv(g)->e, SPV_OP_NOT, sg_type(g, t), x), t));
}
/* shifts by an amount of the value's width (Sail's << of bits by an int: the generator makes the amount the value's
   width) */
static inline SgV sg_shl(SgG* g, int w, SgV a, SgV n) { return sg_trim(g, w, sg_bin(g, w, SPV_OP_SHIFT_LEFT_LOGICAL, a, n)); }
static inline SgV sg_lshr(SgG* g, int w, SgV a, SgV n) { return sg_bin(g, w, SPV_OP_SHIFT_RIGHT_LOGICAL, a, n); }
/* ASR of a u32 or u64 as that type: SPIR-V's shift takes the sign from the opcode. Only of whole words */
static inline SgV sg_ashr(SgG* g, int w, SgV a, SgV n) { return sg_bin(g, w, SPV_OP_SHIFT_RIGHT_ARITHMETIC, a, n); }

/* gpu.sail's operations of the 32-bit word every GPU has */
static inline SgV sg_PopCount(SgG* g, SgV a) {
    SgSpv* s = sg_spv(g);
    return sg_v(spv_emit_unary(s->e, SPV_OP_BIT_COUNT, s->t_u32, sg_u32(g, a)), SGT_U32);
}
static inline SgV sg_BitReverse32(SgG* g, SgV a) {
    SgSpv* s = sg_spv(g);
    return sg_v(spv_emit_unary(s->e, SPV_OP_BIT_REVERSE, s->t_u32, sg_u32(g, a)), SGT_U32);
}
/* the highest bit set (of a negative value: clear), 0xffffffff for none: FindUMsb, FindSMsb */
static inline SgV sg_HighestSetBit32(SgG* g, SgV a) {
    SgSpv* s = sg_spv(g);
    return sg_v(SPV_EXT(s->e, s->t_u32, SPV_GLSL_FIND_U_MSB, sg_u32(g, a)), SGT_U32);
}
static inline SgV sg_HighestSignBit32(SgG* g, SgV a) {
    SgSpv* s = sg_spv(g);
    uint32_t r = SPV_EXT(s->e, s->t_i32, SPV_GLSL_FIND_S_MSB, sg_as(g, a, SGT_I32));
    return sg_v(spv_emit_unary(s->e, SPV_OP_BITCAST, s->t_u32, r), SGT_U32);
}

/* a[hi .. lo] of bits(w): a u64's low or high word is a UConvert, of its shift; a u32's field a BitFieldUExtract */
static inline SgV sg_slice(SgG* g, int w, SgV a, int hi, int lo) {
    SgSpv* s = sg_spv(g);
    int n = hi - lo + 1;
    if (lo == 0 && hi == w - 1) return a;
    if (sg_wide(w)) {
        uint32_t x = a.id;
        if (lo) x = sg_op2(s->e, SPV_OP_SHIFT_RIGHT_LOGICAL, s->t_u64(g), x, sg_const(g, 64, (uint64_t)lo).id);
        if (n <= 32) {
            SgV r = sg_v(spv_emit_int_convert(s->e, s->t_u32, x, 0), SGT_U32);
            return n == 32 ? r : sg_trim(g, n, r);
        }
        return sg_trim(g, n, sg_v(x, SGT_U64));
    }
    {
        uint32_t x = sg_u32(g, a);
        if (lo == 0) return sg_trim(g, n, sg_v(x, SGT_U32));
        if (hi == 31) return sg_v(sg_op2(s->e, SPV_OP_SHIFT_RIGHT_LOGICAL, s->t_u32, x, sg_const(g, 32, (uint64_t)lo).id), SGT_U32);
        return sg_v(spv_emit_ternary(s->e, SPV_OP_BIT_FIELD_U_EXTRACT, s->t_u32, x, sg_const(g, 32, (uint64_t)lo).id,
                                     sg_const(g, 32, (uint64_t)n).id),
                    SGT_U32);
    }
}
/* ZeroExtend: free within a u32 (its bits past the width are zero), a UConvert to a u64 */
static inline SgV sg_zext(SgG* g, int from, int to, SgV a) {
    SgSpv* s = sg_spv(g);
    if (sg_wide(to) && !sg_wide(from)) return sg_v(spv_emit_int_convert(s->e, s->t_u64(g), sg_u32(g, a), 0), SGT_U64);
    return a;
}
/* SignExtend: a BitFieldSExtract of the i32 below 32 bits, an SConvert of the i32 to 64 */
static inline SgV sg_sext(SgG* g, int from, int to, SgV a) {
    SgSpv* s = sg_spv(g);
    uint32_t x = a.id;
    if (from < 32 && !sg_wide(from)) {
        x = spv_emit_ternary(s->e, SPV_OP_BIT_FIELD_S_EXTRACT, s->t_i32, sg_as(g, a, SGT_I32), sg_const(g, 32, 0).id,
                             sg_const(g, 32, (uint64_t)from).id);
        a = sg_v(x, SGT_I32);
        if (to <= 32) return sg_trim(g, to, sg_v(sg_u32(g, a), SGT_U32));
    }
    if (sg_wide(to) && !sg_wide(from)) { /* the i32's SConvert to an i64, bitcast */
        uint32_t i = sg_as(g, a, SGT_I32), t = s->t_u64(g);
        return sg_trim(g, to, sg_v(spv_emit_unary(s->e, SPV_OP_BITCAST, t,
                                                  SPV(s->e, SPV_OP_S_CONVERT, spv_emit_type_int64(s->e, 1), i)),
                                   SGT_U64));
    }
    return a;
}
/* a @ b: the low word b and the high word a of a u64 (b's UConvert, then a's and its shift); in a u32 a's shift */
static inline SgV sg_concat(SgG* g, int wa, SgV a, int wb, SgV b) {
    SgSpv* s = sg_spv(g);
    if (sg_wide(wa + wb)) {
        uint32_t t = s->t_u64(g), lo = sg_zext(g, wb, 64, b).id, hi = sg_zext(g, wa, 64, a).id;
        hi = sg_op2(s->e, SPV_OP_SHIFT_LEFT_LOGICAL, t, hi, sg_const(g, 64, (uint64_t)wb).id);
        return sg_v(sg_op2(s->e, SPV_OP_BITWISE_OR, t, lo, hi), SGT_U64);
    }
    {
        uint32_t lo = sg_u32(g, b), hi = sg_u32(g, a);
        hi = sg_op2(s->e, SPV_OP_SHIFT_LEFT_LOGICAL, s->t_u32, hi, sg_const(g, 32, (uint64_t)wb).id);
        return sg_v(sg_op2(s->e, SPV_OP_BITWISE_OR, s->t_u32, hi, lo), SGT_U32);
    }
}

static inline SgV sg_cmp(SgG* g, int w, uint32_t op, SgV a, SgV b) {
    int t = sg_wide(w) ? SGT_U64 : SGT_U32;
    uint32_t x = sg_as(g, a, t), y = sg_as(g, b, t);
    return sg_v(sg_op2(sg_spv(g)->e, op, sg_spv(g)->t_bool, x, y), SGT_BOOL);
}
static inline SgV sg_eq(SgG* g, int w, SgV a, SgV b) { return sg_cmp(g, w, SPV_OP_I_EQUAL, a, b); }
static inline SgV sg_ne(SgG* g, int w, SgV a, SgV b) { return sg_cmp(g, w, SPV_OP_I_NOT_EQUAL, a, b); }
static inline SgV sg_ult(SgG* g, int w, SgV a, SgV b) { return sg_cmp(g, w, SPV_OP_U_LESS_THAN, a, b); }
static inline SgV sg_ule(SgG* g, int w, SgV a, SgV b) { return sg_cmp(g, w, SPV_OP_U_LESS_THAN_EQUAL, a, b); }
static inline SgV sg_ugt(SgG* g, int w, SgV a, SgV b) { return sg_cmp(g, w, SPV_OP_U_GREATER_THAN, a, b); }
static inline SgV sg_uge(SgG* g, int w, SgV a, SgV b) { return sg_cmp(g, w, SPV_OP_U_GREATER_THAN_EQUAL, a, b); }
static inline SgV sg_slt(SgG* g, int w, SgV a, SgV b) { return sg_cmp(g, w, SPV_OP_S_LESS_THAN, a, b); }
static inline SgV sg_sle(SgG* g, int w, SgV a, SgV b) { return sg_cmp(g, w, SPV_OP_S_LESS_THAN_EQUAL, a, b); }
static inline SgV sg_sgt(SgG* g, int w, SgV a, SgV b) { return sg_cmp(g, w, SPV_OP_S_GREATER_THAN, a, b); }
static inline SgV sg_sge(SgG* g, int w, SgV a, SgV b) { return sg_cmp(g, w, SPV_OP_S_GREATER_THAN_EQUAL, a, b); }

static inline SgV sg_lbin(SgG* g, uint32_t op, SgV a, SgV b) {
    return sg_v(sg_op2(sg_spv(g)->e, op, sg_spv(g)->t_bool, a.id, b.id), SGT_BOOL);
}
static inline SgV sg_not_bool(SgG* g, SgV a) {
    SgSpv* s = sg_spv(g);
    if (s->fold && a.id == s->true_v) return sg_v(s->false_v, SGT_BOOL);
    if (s->fold && a.id == s->false_v) return sg_v(s->true_v, SGT_BOOL);
    return sg_v(spv_emit_unary(s->e, SPV_OP_LOGICAL_NOT, s->t_bool, a.id), SGT_BOOL);
}
static inline SgV sg_and_bool(SgG* g, SgV a, SgV b) {
    SgSpv* s = sg_spv(g);
    if (s->fold) {
        if (a.id == s->true_v) return b;
        if (b.id == s->true_v) return a;
        if (a.id == s->false_v || b.id == s->false_v) return sg_v(s->false_v, SGT_BOOL);
    }
    return sg_lbin(g, SPV_OP_LOGICAL_AND, a, b);
}
static inline SgV sg_or_bool(SgG* g, SgV a, SgV b) {
    SgSpv* s = sg_spv(g);
    if (s->fold) {
        if (a.id == s->false_v) return b;
        if (b.id == s->false_v) return a;
        if (a.id == s->true_v || b.id == s->true_v) return sg_v(s->true_v, SGT_BOOL);
    }
    return sg_lbin(g, SPV_OP_LOGICAL_OR, a, b);
}
static inline SgV sg_ne_bool(SgG* g, SgV a, SgV b) {
    SgSpv* s = sg_spv(g);
    if (s->fold) {
        if (a.id == s->false_v) return b;
        if (b.id == s->false_v) return a;
        if (a.id == s->true_v) return sg_not_bool(g, b);
        if (b.id == s->true_v) return sg_not_bool(g, a);
    }
    return sg_lbin(g, SPV_OP_LOGICAL_NOT_EQUAL, a, b);
}
static inline SgV sg_eq_bool(SgG* g, SgV a, SgV b) { return sg_lbin(g, SPV_OP_LOGICAL_EQUAL, a, b); }

/* if c then a else b of bits(w) (w 0: bools): a select of a's type, b made that type */
static inline SgV sg_select(SgG* g, int w, SgV c, SgV a, SgV b) {
    int t = w ? a.t : SGT_BOOL;
    uint32_t y;
    if (sg_spv(g)->fold && c.id == sg_spv(g)->true_v) return a;
    if (sg_spv(g)->fold && c.id == sg_spv(g)->false_v) return b;
    if (w < 0) return sg_vt(spv_emit_ternary(sg_spv(g)->e, SPV_OP_SELECT, a.ty, c.id, a.id, b.id), a.ty);
    y = sg_as(g, b, t);
    return sg_v(spv_emit_ternary(sg_spv(g)->e, SPV_OP_SELECT, sg_type(g, t), c.id, a.id, y), t);
}
/* Abs of SInt: x < 0 ? 0 - x : x of the u32 rather than GLSL.std.450's SAbs: NVIDIA's driver 621.24 compiled a vertex
   program with SAbs into code that lost the device at an invalid instruction pointer. The most negative value stays
   as it is, as SAbs leaves it */
static inline SgV sg_sabs(SgG* g, int w, SgV a) {
    SgV zero = sg_const(g, 32, 0);
    (void)w;
    return sg_select(g, 32, sg_slt(g, 32, a, zero), sg_sub(g, 32, zero, a), a);
}

/* Min and Max of UInt or SInt: GLSL.std.450's, a signed one's on i32 */
static inline SgV sg_minmax(SgG* g, int w, int t, SpvGlsl450Op op, SgV a, SgV b) {
    SgSpv* s = sg_spv(g);
    uint32_t x, y;
    if (sg_wide(w)) t = SGT_U64;
    x = sg_as(g, a, t);
    y = sg_as(g, b, t);
    return sg_v(spv_emit_glsl_binary(s->e, sg_type(g, t), s->glsl, op, x, y), t);
}
static inline SgV sg_umin(SgG* g, int w, SgV a, SgV b) { return sg_minmax(g, w, SGT_U32, SPV_GLSL_U_MIN, a, b); }
static inline SgV sg_umax(SgG* g, int w, SgV a, SgV b) { return sg_minmax(g, w, SGT_U32, SPV_GLSL_U_MAX, a, b); }
static inline SgV sg_smin(SgG* g, int w, SgV a, SgV b) { return sg_minmax(g, w, SGT_I32, SPV_GLSL_S_MIN, a, b); }
static inline SgV sg_smax(SgG* g, int w, SgV a, SgV b) { return sg_minmax(g, w, SGT_I32, SPV_GLSL_S_MAX, a, b); }

/* ---- gpu.sail's floats: an f32 is bits(32) to the Sail, here an f32 id (the bits a bitcast of it, when asked) ---- */
static inline uint32_t sg_f(SgG* g, SgV v) { return sg_as(g, v, SGT_F32); }
static inline SgV sg_fv(uint32_t id) { return sg_v(id, SGT_F32); }
/* the f32 of these bits */
static inline SgV sg_FConst(SgG* g, int64_t bits) {
    uint32_t u = (uint32_t)bits;
    float f;
    memcpy(&f, &u, 4);
    if (sg_spv(g)->known) return sg_fv(spv_emit_const_f32(sg_spv(g)->e, f));
    return sg_fv(spv_emit_constant_f32(sg_spv(g)->e, sg_spv(g)->t_f32, f));
}
static inline SgV sg_glsl1(SgG* g, SpvGlsl450Op op, SgV x) {
    SgSpv* s = sg_spv(g);
    return sg_fv(spv_emit_glsl_unary(s->e, s->t_f32, s->glsl, op, sg_f(g, x)));
}
static inline SgV sg_glsl2(SgG* g, SpvGlsl450Op op, SgV x, SgV y) {
    SgSpv* s = sg_spv(g);
    uint32_t a = sg_f(g, x), b = sg_f(g, y);
    return sg_fv(spv_emit_glsl_binary(s->e, s->t_f32, s->glsl, op, a, b));
}
static inline SgV sg_fop2(SgG* g, uint32_t op, SgV x, SgV y) {
    uint32_t a = sg_f(g, x), b = sg_f(g, y);
    return sg_fv(sg_op2(sg_spv(g)->e, op, sg_spv(g)->t_f32, a, b));
}
static inline SgV sg_Bits32(SgG* g, SgV x) { return sg_v(sg_u32(g, x), SGT_U32); }
static inline SgV sg_Float32(SgG* g, SgV x) { return sg_fv(sg_f(g, x)); }
static inline SgV sg_U64ToF(SgG* g, SgV x) {
    return sg_fv(spv_emit_unary(sg_spv(g)->e, SPV_OP_CONVERT_U_TO_F, sg_spv(g)->t_f32, x.id));
}
static inline SgV sg_FToU64(SgG* g, SgV x) {
    return sg_v(spv_emit_unary(sg_spv(g)->e, SPV_OP_CONVERT_F_TO_U, sg_spv(g)->t_u64(g), sg_f(g, x)), SGT_U64);
}
static inline SgV sg_FAbs(SgG* g, SgV x) { return sg_glsl1(g, SPV_GLSL_F_ABS, x); }
static inline SgV sg_FNeg(SgG* g, SgV x) {
    return sg_fv(spv_emit_unary(sg_spv(g)->e, SPV_OP_F_NEGATE, sg_spv(g)->t_f32, sg_f(g, x)));
}
static inline SgV sg_FAdd(SgG* g, SgV x, SgV y) { return sg_fop2(g, SPV_OP_F_ADD, x, y); }
static inline SgV sg_FSub(SgG* g, SgV x, SgV y) { return sg_fop2(g, SPV_OP_F_SUB, x, y); }
static inline SgV sg_FMul(SgG* g, SgV x, SgV y) { return sg_fop2(g, SPV_OP_F_MUL, x, y); }
static inline SgV sg_FDiv(SgG* g, SgV x, SgV y) { return sg_fop2(g, SPV_OP_F_DIV, x, y); }
/* GLSL.std.450's: NMin and NMax take the number where one is NaN, FMin and FMax leave it undefined */
static inline SgV sg_FMinN(SgG* g, SgV x, SgV y) { return sg_glsl2(g, SPV_GLSL_N_MIN, x, y); }
static inline SgV sg_FMaxN(SgG* g, SgV x, SgV y) { return sg_glsl2(g, SPV_GLSL_N_MAX, x, y); }
static inline SgV sg_FMin(SgG* g, SgV x, SgV y) { return sg_glsl2(g, SPV_GLSL_F_MIN, x, y); }
static inline SgV sg_FMax(SgG* g, SgV x, SgV y) { return sg_glsl2(g, SPV_GLSL_F_MAX, x, y); }
/* 0 to the nearest even, 1 down, 2 up, 3 toward zero */
static inline SgV sg_FRound(SgG* g, int64_t mode, SgV x) {
    static const SpvGlsl450Op op[4] = {SPV_GLSL_ROUND_EVEN, SPV_GLSL_FLOOR, SPV_GLSL_CEIL, SPV_GLSL_TRUNC};
    return sg_glsl1(g, op[mode & 3], x);
}
static inline SgV sg_FSin(SgG* g, SgV x) { return sg_glsl1(g, SPV_GLSL_SIN, x); }
static inline SgV sg_FCos(SgG* g, SgV x) { return sg_glsl1(g, SPV_GLSL_COS, x); }
static inline SgV sg_FExp2(SgG* g, SgV x) { return sg_glsl1(g, SPV_GLSL_EXP2, x); }
static inline SgV sg_FLog2(SgG* g, SgV x) { return sg_glsl1(g, SPV_GLSL_LOG2, x); }
static inline SgV sg_FInvSqrt(SgG* g, SgV x) { return sg_glsl1(g, SPV_GLSL_INVERSE_SQRT, x); }
static inline SgV sg_FSqrt(SgG* g, SgV x) { return sg_glsl1(g, SPV_GLSL_SQRT, x); }
static inline SgV sg_FTanh(SgG* g, SgV x) { return sg_glsl1(g, SPV_GLSL_TANH, x); }
static inline SgV sg_FIsNaN(SgG* g, SgV x) {
    return sg_v(spv_emit_unary(sg_spv(g)->e, SPV_OP_IS_NAN, sg_spv(g)->t_bool, sg_f(g, x)), SGT_BOOL);
}
/* spv_emit_fcompare's comparison codes (NAK's FloatCmpOp) */
static inline SgV sg_FCompare(SgG* g, int64_t op, SgV x, SgV y) {
    uint32_t a = sg_f(g, x), b = sg_f(g, y);
    return sg_v(spv_emit_fcompare(sg_spv(g)->e, sg_spv(g)->t_bool, (uint32_t)op, a, b), SGT_BOOL);
}
static inline SgV sg_FToS32(SgG* g, SgV x) {
    return sg_v(spv_emit_unary(sg_spv(g)->e, SPV_OP_CONVERT_F_TO_S, sg_spv(g)->t_i32, sg_f(g, x)), SGT_I32);
}
static inline SgV sg_FToU32(SgG* g, SgV x) {
    return sg_v(spv_emit_unary(sg_spv(g)->e, SPV_OP_CONVERT_F_TO_U, sg_spv(g)->t_u32, sg_f(g, x)), SGT_U32);
}
static inline SgV sg_S32ToF(SgG* g, SgV x) {
    return sg_fv(spv_emit_unary(sg_spv(g)->e, SPV_OP_CONVERT_S_TO_F, sg_spv(g)->t_f32, sg_as(g, x, SGT_I32)));
}
static inline SgV sg_U32ToF(SgG* g, SgV x) {
    return sg_fv(spv_emit_unary(sg_spv(g)->e, SPV_OP_CONVERT_U_TO_F, sg_spv(g)->t_f32, sg_u32(g, x)));
}
/* a pair of halves: a vec2 of f32 (UnpackHalf2x16 of a word, or two floats), its components and their abs and neg */
static inline uint32_t sg_t_v2f(SgG* g) { return spv_emit_type_vec2(sg_spv(g)->e, sg_spv(g)->t_f32); }
static inline SgV sg_UnpackHalf2(SgG* g, SgV x) {
    SgSpv* s = sg_spv(g);
    uint32_t t = sg_t_v2f(g);
    return sg_vt(SPV_EXT(s->e, t, SPV_GLSL_UNPACK_HALF2X16, sg_u32(g, x)), t);
}
static inline SgV sg_Half2At(SgG* g, SgV v, int64_t k) {
    return sg_fv(spv_emit_vec4_extract(sg_spv(g)->e, sg_spv(g)->t_f32, v.id, (uint32_t)k));
}
static inline SgV sg_Half2(SgG* g, SgV lo, SgV hi) {
    uint32_t t = sg_t_v2f(g), a = sg_f(g, lo), b = sg_f(g, hi);
    return sg_vt(spv_emit_vec2_construct(sg_spv(g)->e, t, a, b), t);
}
static inline SgV sg_Half2Abs(SgG* g, SgV v) { return sg_vt(SPV_EXT(sg_spv(g)->e, v.ty, SPV_GLSL_F_ABS, v.id), v.ty); }
static inline SgV sg_Half2Neg(SgG* g, SgV v) { return sg_vt(SPV(sg_spv(g)->e, SPV_OP_F_NEGATE, v.ty, v.id), v.ty); }
static inline SgV sg_Half2Add(SgG* g, SgV a, SgV b) { return sg_vt(sg_op2(sg_spv(g)->e, SPV_OP_F_ADD, a.ty, a.id, b.id), a.ty); }
static inline SgV sg_Half2Mul(SgG* g, SgV a, SgV b) { return sg_vt(sg_op2(sg_spv(g)->e, SPV_OP_F_MUL, a.ty, a.id, b.id), a.ty); }
static inline SgV sg_Half2MinN(SgG* g, SgV a, SgV b) { return sg_vt(SPV_EXT(sg_spv(g)->e, a.ty, SPV_GLSL_N_MIN, a.id, b.id), a.ty); }
static inline SgV sg_Half2MaxN(SgG* g, SgV a, SgV b) { return sg_vt(SPV_EXT(sg_spv(g)->e, a.ty, SPV_GLSL_N_MAX, a.id, b.id), a.ty); }
static inline SgV sg_Half2Clamp01(SgG* g, SgV a) {
    SgSpv* s = sg_spv(g);
    uint32_t zero = sg_FConst(g, 0).id, z = spv_emit_vec2_construct(s->e, a.ty, zero, zero);
    uint32_t one = sg_FConst(g, 0x3f800000).id, o = spv_emit_vec2_construct(s->e, a.ty, one, one);
    return sg_vt(SPV_EXT(s->e, a.ty, SPV_GLSL_N_CLAMP, a.id, z, o), a.ty);
}
/* a field of a word at a run-time offset: BitFieldUExtract, and BitFieldSExtract of the i32 */
static inline SgV sg_BitsU(SgG* g, SgV x, SgV at, SgV n) {
    SgSpv* s = sg_spv(g);
    uint32_t a = sg_u32(g, x);
    return sg_v(spv_emit_ternary(s->e, SPV_OP_BIT_FIELD_U_EXTRACT, s->t_u32, a, sg_u32(g, at), sg_u32(g, n)), SGT_U32);
}
static inline SgV sg_BitsS(SgG* g, SgV x, SgV at, SgV n) {
    SgSpv* s = sg_spv(g);
    uint32_t a = sg_as(g, x, SGT_I32);
    return sg_v(spv_emit_ternary(s->e, SPV_OP_BIT_FIELD_S_EXTRACT, s->t_i32, a, sg_u32(g, at), sg_u32(g, n)), SGT_I32);
}
static inline SgV sg_Shuffle(SgG* g, SgV x, SgV lane) {
    SgSpv* s = sg_spv(g);
    uint32_t v = sg_u32(g, x), l = sg_u32(g, lane);
    return sg_v(SPV(s->e, SPV_OP_GROUP_NON_UNIFORM_SHUFFLE, s->t_u32, sg_const(g, 32, SPV_SCOPE_SUBGROUP).id, v, l),
                SGT_U32);
}
static inline SgV sg_ClusterReduce32(SgG* g, int64_t op, SgV x) {
    SgSpv* s = sg_spv(g);
    uint32_t scope = sg_const(g, 32, SPV_SCOPE_SUBGROUP).id, v = sg_u32(g, x);
    return sg_v(SPV(s->e, (uint32_t)op, s->t_u32, scope, SPV_GROUP_OPERATION_CLUSTERED_REDUCE, v,
                    sg_const(g, 32, 32).id),
                SGT_U32);
}
/* x / y of signed words, toward zero (OpSDiv of the i32s) */
static inline SgV sg_QuotS(SgG* g, SgV x, SgV y) {
    uint32_t a = sg_as(g, x, SGT_I32), b = sg_as(g, y, SGT_I32);
    return sg_v(sg_op2(sg_spv(g)->e, SPV_OP_S_DIV, sg_spv(g)->t_i32, a, b), SGT_I32);
}
/* a capability the instruction needs: declared once */
static inline void sg_Capability(SgG* g, int64_t cap) { spv_emit_capability(sg_spv(g)->e, (uint32_t)cap); }

static inline SgV sg_FFma(SgG* g, SgV x, SgV y, SgV z) {
    SgSpv* s = sg_spv(g);
    uint32_t a = sg_f(g, x), b = sg_f(g, y), c = sg_f(g, z);
    return sg_fv(spv_emit_glsl_ternary(s->e, s->t_f32, s->glsl, SPV_GLSL_FMA, a, b, c));
}
static inline SgV sg_FDdx(SgG* g, SgV x) { return sg_fv(SPV(sg_spv(g)->e, SPV_OP_DPDX, sg_spv(g)->t_f32, sg_f(g, x))); }
static inline SgV sg_FDdy(SgG* g, SgV x) { return sg_fv(SPV(sg_spv(g)->e, SPV_OP_DPDY, sg_spv(g)->t_f32, sg_f(g, x))); }

/* ---- gpu.sail's integers that keep their type: each operation is of its first operand's type (an i32 or a u32),
   the other made that type; the constant SConst an i32 (spv_emit_const_i32 where the constants are the module's
   known ones) ---- */
static inline SgV sg_SConst(SgG* g, int64_t v) {
    SgSpv* s = sg_spv(g);
    if (s->known) return sg_v(spv_emit_const_i32(s->e, (int32_t)v), SGT_I32);
    return sg_v(spv_emit_constant_u32(s->e, s->t_i32, (uint32_t)v), SGT_I32);
}
static inline SgV sg_UConst(SgG* g, int64_t v) { return sg_const(g, 32, (uint64_t)v); }
static inline int sg_int_t(SgV a) { return a.t == SGT_I32 ? SGT_I32 : SGT_U32; }
static inline SgV sg_top2(SgG* g, uint32_t op, SgV a, SgV b) {
    int t = sg_int_t(a);
    uint32_t x = sg_as(g, a, t), y = sg_as(g, b, t);
    return sg_v(sg_op2(sg_spv(g)->e, op, sg_type(g, t), x, y), t);
}
static inline SgV sg_tcmp(SgG* g, uint32_t op, SgV a, SgV b) {
    int t = sg_int_t(a);
    uint32_t x = sg_as(g, a, t), y = sg_as(g, b, t);
    return sg_v(sg_op2(sg_spv(g)->e, op, sg_spv(g)->t_bool, x, y), SGT_BOOL);
}
static inline SgV sg_IAddT(SgG* g, SgV a, SgV b) { return sg_top2(g, SPV_OP_I_ADD, a, b); }
static inline SgV sg_ISubT(SgG* g, SgV a, SgV b) { return sg_top2(g, SPV_OP_I_SUB, a, b); }
static inline SgV sg_IMulT(SgG* g, SgV a, SgV b) { return sg_top2(g, SPV_OP_I_MUL, a, b); }
static inline SgV sg_AndT(SgG* g, SgV a, SgV b) { return sg_top2(g, SPV_OP_BITWISE_AND, a, b); }
static inline SgV sg_OrT(SgG* g, SgV a, SgV b) { return sg_top2(g, SPV_OP_BITWISE_OR, a, b); }
static inline SgV sg_XorT(SgG* g, SgV a, SgV b) { return sg_top2(g, SPV_OP_BITWISE_XOR, a, b); }
static inline SgV sg_ShlT(SgG* g, SgV a, SgV b) { return sg_top2(g, SPV_OP_SHIFT_LEFT_LOGICAL, a, b); }
static inline SgV sg_ShrT(SgG* g, SgV a, SgV b) { return sg_top2(g, SPV_OP_SHIFT_RIGHT_LOGICAL, a, b); }
static inline SgV sg_AsrT(SgG* g, SgV a, SgV b) { return sg_top2(g, SPV_OP_SHIFT_RIGHT_ARITHMETIC, a, b); }
static inline SgV sg_INegT(SgG* g, SgV a) {
    int t = sg_int_t(a);
    return sg_v(spv_emit_unary(sg_spv(g)->e, SPV_OP_S_NEGATE, sg_type(g, t), a.id), t);
}
static inline SgV sg_SAbsT(SgG* g, SgV a) {
    SgSpv* s = sg_spv(g);
    return sg_v(SPV_EXT(s->e, s->t_i32, SPV_GLSL_S_ABS, sg_as(g, a, SGT_I32)), SGT_I32);
}
static inline SgV sg_IEqT(SgG* g, SgV a, SgV b) { return sg_tcmp(g, SPV_OP_I_EQUAL, a, b); }
static inline SgV sg_INeT(SgG* g, SgV a, SgV b) { return sg_tcmp(g, SPV_OP_I_NOT_EQUAL, a, b); }
static inline SgV sg_SLtT(SgG* g, SgV a, SgV b) { return sg_tcmp(g, SPV_OP_S_LESS_THAN, a, b); }
static inline SgV sg_SLeT(SgG* g, SgV a, SgV b) { return sg_tcmp(g, SPV_OP_S_LESS_THAN_EQUAL, a, b); }
static inline SgV sg_SGtT(SgG* g, SgV a, SgV b) { return sg_tcmp(g, SPV_OP_S_GREATER_THAN, a, b); }
static inline SgV sg_SGeT(SgG* g, SgV a, SgV b) { return sg_tcmp(g, SPV_OP_S_GREATER_THAN_EQUAL, a, b); }
static inline SgV sg_ULtT(SgG* g, SgV a, SgV b) { return sg_tcmp(g, SPV_OP_U_LESS_THAN, a, b); }
static inline SgV sg_ULeT(SgG* g, SgV a, SgV b) { return sg_tcmp(g, SPV_OP_U_LESS_THAN_EQUAL, a, b); }
static inline SgV sg_UGtT(SgG* g, SgV a, SgV b) { return sg_tcmp(g, SPV_OP_U_GREATER_THAN, a, b); }
static inline SgV sg_UGeT(SgG* g, SgV a, SgV b) { return sg_tcmp(g, SPV_OP_U_GREATER_THAN_EQUAL, a, b); }
static inline SgV sg_AsI32(SgG* g, SgV a) { return sg_v(sg_as(g, a, SGT_I32), SGT_I32); }
static inline SgV sg_AsU32(SgG* g, SgV a) { return sg_v(sg_as(g, a, SGT_U32), SGT_U32); }

/* ---- gpu.sail's vectors: a vec2, vec3 or vec4 of f32, a bvec4 of bool, each an opaque value of its SPIR-V type;
   an operation on two of them is one instruction of their type ---- */
static inline uint32_t sg_t_vec(SgG* g, uint32_t component, unsigned n) {
    return spv_emit_type_vector(sg_spv(g)->e, component, n);
}
static inline SgV sg_vop1(SgG* g, uint32_t op, SgV a) { return sg_vt(SPV(sg_spv(g)->e, op, a.ty, a.id), a.ty); }
static inline SgV sg_vop2(SgG* g, uint32_t op, SgV a, SgV b) { return sg_vt(SPV(sg_spv(g)->e, op, a.ty, a.id, b.id), a.ty); }
static inline SgV sg_vext1(SgG* g, SpvGlsl450Op op, SgV a) { return sg_vt(SPV_EXT(sg_spv(g)->e, a.ty, op, a.id), a.ty); }
static inline SgV sg_vext2(SgG* g, SpvGlsl450Op op, SgV a, SgV b) {
    return sg_vt(SPV_EXT(sg_spv(g)->e, a.ty, op, a.id, b.id), a.ty);
}
/* the vec4 of four f32 constants (their bits): a constant composite */
static inline SgV sg_V4Const(SgG* g, int64_t x, int64_t y, int64_t z, int64_t w) {
    SgSpv* s = sg_spv(g);
    uint32_t t = sg_t_vec(g, s->t_f32, 4), parts[4] = {sg_FConst(g, x).id, sg_FConst(g, y).id, sg_FConst(g, z).id,
                                                       sg_FConst(g, w).id};
    if (s->known) return sg_vt(spv_emit_const_composite(s->e, t, parts, 4), t);
    return sg_vt(spv_emit_constant_vec4(s->e, t, parts), t);
}
/* a vec4 of four floats, and of one four times */
static inline SgV sg_V4Of(SgG* g, SgV x, SgV y, SgV z, SgV w) {
    uint32_t t = sg_t_vec(g, sg_spv(g)->t_f32, 4), a = sg_f(g, x), b = sg_f(g, y), c = sg_f(g, z), d = sg_f(g, w);
    return sg_vt(SPV(sg_spv(g)->e, SPV_OP_COMPOSITE_CONSTRUCT, t, a, b, c, d), t);
}
static inline SgV sg_V4Splat(SgG* g, SgV x) {
    uint32_t t = sg_t_vec(g, sg_spv(g)->t_f32, 4), a = sg_f(g, x);
    return sg_vt(SPV(sg_spv(g)->e, SPV_OP_COMPOSITE_CONSTRUCT, t, a, a, a, a), t);
}
/* component k */
static inline SgV sg_V4At(SgG* g, SgV v, int64_t k) {
    return sg_fv(spv_emit_vec4_extract(sg_spv(g)->e, sg_spv(g)->t_f32, v.id, (uint32_t)k));
}
/* components of a (0-3) and b (4-7): OpVectorShuffle */
static inline SgV sg_V4Shuffle(SgG* g, SgV a, SgV b, int64_t x, int64_t y, int64_t z, int64_t w) {
    uint32_t t = sg_t_vec(g, sg_spv(g)->t_f32, 4);
    return sg_vt(SPV(sg_spv(g)->e, SPV_OP_VECTOR_SHUFFLE, t, a.id, b.id, (uint32_t)x, (uint32_t)y, (uint32_t)z,
                     (uint32_t)w),
                 t);
}
/* the first three components */
static inline SgV sg_V3Of4(SgG* g, SgV a) {
    uint32_t t = sg_t_vec(g, sg_spv(g)->t_f32, 3);
    return sg_vt(SPV(sg_spv(g)->e, SPV_OP_VECTOR_SHUFFLE, t, a.id, a.id, 0, 1, 2), t);
}
static inline SgV sg_V2Of(SgG* g, SgV x, SgV y) {
    uint32_t t = sg_t_vec(g, sg_spv(g)->t_f32, 2), a = sg_f(g, x), b = sg_f(g, y);
    return sg_vt(SPV(sg_spv(g)->e, SPV_OP_COMPOSITE_CONSTRUCT, t, a, b), t);
}
static inline SgV sg_V3Of(SgG* g, SgV x, SgV y, SgV z) {
    uint32_t t = sg_t_vec(g, sg_spv(g)->t_f32, 3), a = sg_f(g, x), b = sg_f(g, y), c = sg_f(g, z);
    return sg_vt(SPV(sg_spv(g)->e, SPV_OP_COMPOSITE_CONSTRUCT, t, a, b, c), t);
}
static inline SgV sg_V4FAdd(SgG* g, SgV a, SgV b) { return sg_vop2(g, SPV_OP_F_ADD, a, b); }
static inline SgV sg_V4FMul(SgG* g, SgV a, SgV b) { return sg_vop2(g, SPV_OP_F_MUL, a, b); }
static inline SgV sg_V4FNeg(SgG* g, SgV a) { return sg_vop1(g, SPV_OP_F_NEGATE, a); }
static inline SgV sg_V4FAbs(SgG* g, SgV a) { return sg_vext1(g, SPV_GLSL_F_ABS, a); }
static inline SgV sg_V4Floor(SgG* g, SgV a) { return sg_vext1(g, SPV_GLSL_FLOOR, a); }
static inline SgV sg_V4FMin(SgG* g, SgV a, SgV b) { return sg_vext2(g, SPV_GLSL_F_MIN, a, b); }
static inline SgV sg_V4FMax(SgG* g, SgV a, SgV b) { return sg_vext2(g, SPV_GLSL_F_MAX, a, b); }
/* the dot products: OpDot */
static inline SgV sg_V4Dot(SgG* g, SgV a, SgV b) { return sg_fv(SPV(sg_spv(g)->e, SPV_OP_DOT, sg_spv(g)->t_f32, a.id, b.id)); }
static inline SgV sg_V3Dot(SgG* g, SgV a, SgV b) { return sg_V4Dot(g, a, b); }
/* a comparison of each component (FCompare's codes), and the components of a where it holds, else b's */
static inline SgV sg_V4FCompare(SgG* g, int64_t op, SgV a, SgV b) {
    SgSpv* s = sg_spv(g);
    uint32_t t = sg_t_vec(g, s->t_bool, 4);
    return sg_vt(spv_emit_fcompare(s->e, t, (uint32_t)op, a.id, b.id), t);
}
static inline SgV sg_V4Select(SgG* g, SgV c, SgV a, SgV b) {
    return sg_vt(spv_emit_ternary(sg_spv(g)->e, SPV_OP_SELECT, a.ty, c.id, a.id, b.id), a.ty);
}
static inline SgV sg_B4Splat(SgG* g, SgV c) {
    uint32_t t = sg_t_vec(g, sg_spv(g)->t_bool, 4);
    return sg_vt(SPV(sg_spv(g)->e, SPV_OP_COMPOSITE_CONSTRUCT, t, c.id, c.id, c.id, c.id), t);
}

/* ---- gpu.sail's sampled images: OpImageSampleImplicitLod and OpImageSampleExplicitLod of a sampled image's id ---- */
static inline uint32_t sg_t_v4f(SgG* g) { return sg_t_vec(g, sg_spv(g)->t_f32, 4); }
static inline SgV sg_SampleImplicit(SgG* g, SgV s, SgV c) {
    uint32_t t = sg_t_v4f(g);
    return sg_vt(SPV(sg_spv(g)->e, SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, t, s.id, c.id), t);
}
static inline SgV sg_SampleBias(SgG* g, SgV s, SgV c, SgV bias) {
    uint32_t t = sg_t_v4f(g), b = sg_f(g, bias);
    return sg_vt(SPV(sg_spv(g)->e, SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, t, s.id, c.id, SPV_IMAGE_OPERANDS_BIAS, b), t);
}
static inline SgV sg_SampleLod(SgG* g, SgV s, SgV c, SgV lod) {
    uint32_t t = sg_t_v4f(g), l = sg_f(g, lod);
    return sg_vt(SPV(sg_spv(g)->e, SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD, t, s.id, c.id, SPV_IMAGE_OPERANDS_LOD, l), t);
}
static inline SgV sg_SampleGrad(SgG* g, SgV s, SgV c, SgV dx, SgV dy) {
    uint32_t t = sg_t_v4f(g);
    return sg_vt(SPV(sg_spv(g)->e, SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD, t, s.id, c.id, SPV_IMAGE_OPERANDS_GRAD, dx.id, dy.id),
                 t);
}
#endif
