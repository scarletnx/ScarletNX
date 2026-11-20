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

typedef struct SgG SgG;
enum { SGT_U32, SGT_I32, SGT_F32, SGT_U64, SGT_BOOL };
typedef struct {
    uint32_t id;
    uint8_t t;
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
} SgSpv;
static SgSpv* sg_spv(SgG* g); /* the ISA's file gives it */

/* one instruction of two operands: any opcode (spv_emit_binary takes some only), the same words */
static inline uint32_t sg_op2(SpvEmitter* e, uint32_t op, uint32_t type, uint32_t a, uint32_t b) {
    return SPV(e, op, type, a, b);
}
static inline SgV sg_v(uint32_t id, int t) {
    SgV v = {id, (uint8_t)t};
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
    if (v.t == t || v.t == SGT_U64 || v.t == SGT_BOOL || t == SGT_U64 || t == SGT_BOOL) return v.id;
    for (i = 0; i < s->ncast; ++i)
        if (s->cast[i].from == v.id && s->cast[i].t == t) return s->cast[i].to;
    id = spv_emit_unary(s->e, SPV_OP_BITCAST, sg_type(g, t), v.id);
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
/* Abs of SInt: SAbs of the i32 */
static inline SgV sg_sabs(SgG* g, int w, SgV a) {
    SgSpv* s = sg_spv(g);
    (void)w;
    return sg_v(spv_emit_glsl_unary(s->e, s->t_i32, s->glsl, SPV_GLSL_S_ABS, sg_as(g, a, SGT_I32)), SGT_I32);
}

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
    if (sg_wide(to) && !sg_wide(from))
        return sg_trim(g, to, sg_v(spv_emit_int_convert(s->e, s->t_u64(g),
                                                        spv_emit_int_convert(s->e, spv_emit_type_int64(s->e, 1), sg_as(g, a, SGT_I32), 1), 0),
                                   SGT_U64));
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
static inline SgV sg_and_bool(SgG* g, SgV a, SgV b) { return sg_lbin(g, SPV_OP_LOGICAL_AND, a, b); }
static inline SgV sg_or_bool(SgG* g, SgV a, SgV b) { return sg_lbin(g, SPV_OP_LOGICAL_OR, a, b); }
static inline SgV sg_eq_bool(SgG* g, SgV a, SgV b) { return sg_lbin(g, SPV_OP_LOGICAL_EQUAL, a, b); }
static inline SgV sg_ne_bool(SgG* g, SgV a, SgV b) { return sg_lbin(g, SPV_OP_LOGICAL_NOT_EQUAL, a, b); }
static inline SgV sg_not_bool(SgG* g, SgV a) {
    return sg_v(spv_emit_unary(sg_spv(g)->e, SPV_OP_LOGICAL_NOT, sg_spv(g)->t_bool, a.id), SGT_BOOL);
}

/* if c then a else b of bits(w) (w 0: bools): a select of a's type, b made that type */
static inline SgV sg_select(SgG* g, int w, SgV c, SgV a, SgV b) {
    int t = w ? a.t : SGT_BOOL;
    uint32_t y = sg_as(g, b, t);
    (void)w;
    return sg_v(spv_emit_ternary(sg_spv(g)->e, SPV_OP_SELECT, sg_type(g, t), c.id, a.id, y), t);
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
#endif
