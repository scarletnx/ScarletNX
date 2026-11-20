/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Integer, logic, video and predicate operations */
#include "nx/gpu/shader/translate.h"

u32 shader_vm_select(DirectCtx* c, u32 i32_type, u32 value, u32 select) {
    u32 lane = select & 7;
    if (lane >= 6) return value;
    u32 width = lane < 4 ? 8 : 16;
    u32 offset = lane < 4 ? lane * 8 : (lane - 4) * 16;
    u32 off_id = shader_u32(c, offset);
    u32 width_id = shader_u32(c, width);
    return select & 8 ? spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type,
                                       spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type,
                                                        spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, value),
                                                        off_id, width_id))
                      : spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_U_EXTRACT, c->u32_type, value, off_id, width_id);
}

/* A video operand as PTX's scalar video instructions take it: its byte, half or word (select & 7), sign-extended
   when select has bit 3, as a 64-bit integer (an s33) */
u32 shader_video_operand(DirectCtx* c, u32 i32_type, u32 i64_type, u32 u64_type, u32 value, u32 select) {
    u32 narrow = shader_vm_select(c, i32_type, value, select);
    return select & 8
               ? spv_emit_int_convert(&c->emit, i64_type, spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, narrow), 1)
               : spv_emit_unary(&c->emit, SPV_OP_BITCAST, i64_type,
                                spv_emit_int_convert(&c->emit, u64_type, narrow, 0));
}

/* The end of a scalar video instruction, as PTX defines it: the 64-bit result clamped to the destination's range
   (signed when dsigned, the width of the merged part) with saturate, then the secondary operation with C (bits
   51-53: 0 and 1 merge into C's high or low half, 2 and 3 into its byte 0 or 2, 4 adds C, 5 and 6 take the
   minimum or maximum, 7 none), and the low 32 bits */
u32 shader_video_finish(DirectCtx* c, u32 i32_type, u32 i64_type, u32 u64_type, u32 result, u32 third, u32 mode,
                        int saturate, int dsigned, int unsigned_nonnegative) {
    if (saturate) {
        u32 width = mode < 2 ? 16 : mode < 4 ? 8 : 32;
        u64 low = dsigned ? (u64)0 - (1ull << (width - 1)) : 0;
        u64 high = dsigned ? (1ull << (width - 1)) - 1 : (1ull << width) - 1;
        u32 low_id = spv_emit_constant64(&c->emit, i64_type, low),
            high_id = spv_emit_constant64(&c->emit, i64_type, high);
        if (!unsigned_nonnegative)
            result = spv_emit_ternary(&c->emit, SPV_OP_SELECT, i64_type,
                                      spv_emit_binary(&c->emit, SPV_OP_S_GREATER_THAN, c->bool_type, low_id, result),
                                      low_id, result);
        u32 above = unsigned_nonnegative
                        ? spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type,
                                          spv_emit_unary(&c->emit, SPV_OP_BITCAST, u64_type, high_id),
                                          spv_emit_unary(&c->emit, SPV_OP_BITCAST, u64_type, result))
                        : spv_emit_binary(&c->emit, SPV_OP_S_GREATER_THAN, c->bool_type, result, high_id);
        result = spv_emit_ternary(&c->emit, SPV_OP_SELECT, i64_type, above, high_id, result);
    }
    if (mode >= 4 && mode <= 6) {
        u32 wide = dsigned ? spv_emit_int_convert(&c->emit, i64_type,
                                                  spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, third), 1)
                           : spv_emit_unary(&c->emit, SPV_OP_BITCAST, i64_type,
                                            spv_emit_int_convert(&c->emit, u64_type, third, 0));
        if (mode == 4)
            result = spv_emit_iadd_u32(&c->emit, i64_type, result, wide);
        else {
            u32 greater = spv_emit_binary(&c->emit, SPV_OP_S_GREATER_THAN, c->bool_type, result, wide);
            result = spv_emit_ternary(&c->emit, SPV_OP_SELECT, i64_type, greater, mode == 5 ? wide : result,
                                      mode == 5 ? result : wide);
        }
    }
    u32 value =
        spv_emit_int_convert(&c->emit, c->u32_type, spv_emit_unary(&c->emit, SPV_OP_BITCAST, u64_type, result), 0);
    if (mode < 4)
        value =
            spv_emit_bitfield_insert(&c->emit, c->u32_type, third, value,
                                     shader_u32(c, mode == 0 || mode == 3 ? 16 : 0), shader_u32(c, mode < 2 ? 16 : 8));
    return value;
}

u32 shader_xmad_half(DirectCtx* c, u32 i32_type, u32 value, int high, int is_signed) {
    u32 offset = shader_u32(c, high ? 16 : 0);
    u32 width = shader_u32(c, 16);
    if (is_signed) {
        u32 signed_value = spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, value);
        u32 half = spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_S_EXTRACT, i32_type, signed_value, offset, width);
        return spv_emit_unary(&c->emit, SPV_OP_BITCAST, c->u32_type, half);
    }
    return spv_emit_ternary(&c->emit, SPV_OP_BIT_FIELD_U_EXTRACT, c->u32_type, value, offset, width);
}

u32 shader_subgroup_ballot_word(DirectCtx* c, u32 vec4_u32, u32 lane, u32 predicate) {
    u32 mask = spv_emit_subgroup_ballot(&c->emit, vec4_u32, c->u32_type, predicate);
    u32 word = spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, lane, shader_u32(c, 5));
    u32 result = spv_emit_vec4_extract(&c->emit, c->u32_type, mask, 0);
    for (u32 i = 1; i < 4; i++)
        result = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type,
                                  spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, word, shader_u32(c, i)),
                                  spv_emit_vec4_extract(&c->emit, c->u32_type, mask, i), result);
    return result;
}

u32 shader_integer_add(DirectCtx* c, u32 left, u32 right, u32 avg, int extended, int write_cc) {
    if (!extended && !write_cc) {
        if (avg == 2) left = spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type, c->zero, left);
        if (avg == 1) right = spv_emit_binary(&c->emit, SPV_OP_I_SUB, c->u32_type, c->zero, right);
        u32 sum = spv_emit_iadd_u32(&c->emit, c->u32_type, left, right);
        return avg == 3 ? spv_emit_iadd_u32(&c->emit, c->u32_type, sum, shader_u32(c, 1)) : sum;
    }
    if (avg == 2) left = spv_emit_unary(&c->emit, SPV_OP_NOT, c->u32_type, left);
    if (avg == 1) right = spv_emit_unary(&c->emit, SPV_OP_NOT, c->u32_type, right);
    u32 sum0 = spv_emit_iadd_u32(&c->emit, c->u32_type, left, right);
    u32 carry0 = spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, sum0, left);
    u32 cin = avg ? shader_u32(c, 1) : c->zero;
    if (extended) {
        u32 old_carry = flag_read(c, c->cc_c);
        u32 extra = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, old_carry, shader_u32(c, 1), c->zero);
        cin = spv_emit_iadd_u32(&c->emit, c->u32_type, cin, extra);
    }
    u32 sum = spv_emit_iadd_u32(&c->emit, c->u32_type, sum0, cin);
    if (write_cc) {
        u32 carry1 = spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, sum, sum0);
        u32 carry = spv_emit_bool_binary(&c->emit, c->bool_type, 1, carry0, carry1);
        u32 signs = spv_emit_binary(&c->emit, SPV_OP_BITWISE_XOR, c->u32_type, sum, left);
        u32 same = spv_emit_unary(&c->emit, SPV_OP_NOT, c->u32_type,
                                  spv_emit_binary(&c->emit, SPV_OP_BITWISE_XOR, c->u32_type, left, right));
        u32 bits = spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, signs, same);
        u32 top = shader_u32(c, 0x80000000u);
        u32 overflow = spv_emit_binary(&c->emit, SPV_OP_I_NOT_EQUAL, c->bool_type,
                                       spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, bits, top), c->zero);
        u32 zero = spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, sum, c->zero);
        if (extended) zero = spv_emit_bool_binary(&c->emit, c->bool_type, 0, zero, flag_read(c, c->cc_z));
        u32 sign = spv_emit_binary(&c->emit, SPV_OP_I_NOT_EQUAL, c->bool_type,
                                   spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, sum, top), c->zero);
        cc_store(c, c->cc_c, carry);
        cc_store(c, c->cc_o, overflow);
        cc_store(c, c->cc_z, zero);
        cc_store(c, c->cc_s, sign);
    }
    return sum;
}
/* a lane whose writes must not land: a helper, or one a KIL inside the flow loop marked */
u32 shader_lane_dead(DirectCtx* c) {
    u32 helper = spv_emit_is_helper(&c->emit, c->bool_type);
    if (!c->discarded) return helper;
    return spv_emit_bool_binary(&c->emit, c->bool_type, 1, helper, spv_emit_load(&c->emit, c->bool_type, c->discarded));
}

u32 shader_integer_compare(DirectCtx* c, u32 i32_type, u64 w, u32 left, u32 right) {
    u32 cmp = B(w, 49, 3);
    if (cmp == 0 || cmp == 7) return cmp == 7 ? c->true_value : c->false_value;
    if (B(w, 43, 1)) {
        /* the high words decide unless equal: LE and GE take LT and GT there, equality goes to the carry */
        u64 plain = w & ~(1ull << 43);
        u64 strict = cmp == 3 ? 1 : cmp == 6 ? 4 : cmp;
        u32 base = shader_integer_compare(c, i32_type, (plain & ~(7ull << 49)) | (strict << 49), left, right);
        u32 equal = shader_integer_compare(c, i32_type, (plain & ~(7ull << 49)) | (2ull << 49), left, right);
        u32 carry = flag_read(c, c->cc_c);
        u32 zero = flag_read(c, c->cc_z);
        u32 no_carry = spv_emit_unary(&c->emit, SPV_OP_LOGICAL_NOT, c->bool_type, carry);
        if (cmp == 1 || cmp == 6)
            return spv_emit_bool_binary(
                &c->emit, c->bool_type, 1, base,
                spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal, cmp == 1 ? no_carry : carry));
        if (cmp == 2) return spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal, zero);
        if (cmp == 3)
            return spv_emit_bool_binary(
                &c->emit, c->bool_type, 1, base,
                spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal,
                                     spv_emit_bool_binary(&c->emit, c->bool_type, 1, zero, no_carry)));
        if (cmp == 4)
            return spv_emit_bool_binary(
                &c->emit, c->bool_type, 1, base,
                spv_emit_bool_binary(
                    &c->emit, c->bool_type, 0, equal,
                    spv_emit_bool_binary(&c->emit, c->bool_type, 0,
                                         spv_emit_unary(&c->emit, SPV_OP_LOGICAL_NOT, c->bool_type, zero), carry)));
        if (cmp == 5)
            return spv_emit_bool_binary(&c->emit, c->bool_type, 1, base,
                                        spv_emit_unary(&c->emit, SPV_OP_LOGICAL_NOT, c->bool_type, zero));
    }
    if (cmp == 2) return spv_emit_binary(&c->emit, SPV_OP_I_EQUAL, c->bool_type, left, right);
    if (cmp == 5) return spv_emit_binary(&c->emit, SPV_OP_I_NOT_EQUAL, c->bool_type, left, right);
    u32 less, greater;
    if (B(w, 48, 1)) {
        u32 lhs = spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, left);
        u32 rhs = spv_emit_unary(&c->emit, SPV_OP_BITCAST, i32_type, right);
        less = spv_emit_binary(&c->emit, SPV_OP_S_GREATER_THAN, c->bool_type, rhs, lhs);
        greater = spv_emit_binary(&c->emit, SPV_OP_S_GREATER_THAN, c->bool_type, lhs, rhs);
    } else {
        less = spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, left, right);
        greater = spv_emit_binary(&c->emit, SPV_OP_U_LESS_THAN, c->bool_type, right, left);
    }
    if (cmp == 1) return less;
    if (cmp == 4) return greater;
    return spv_emit_unary(&c->emit, SPV_OP_LOGICAL_NOT, c->bool_type, cmp == 3 ? greater : less);
}
u32 shader_integer_right(DirectCtx* c, u64 w, int kind) {
    if (kind == 0) return reg_read(c, B(w, 20, 8));
    if (kind == 1) return shader_cbuf_word(c, w);
    u32 imm = B(w, 20, 19);
    return shader_u32(c, B(w, 56, 1) ? imm | 0xfff80000u : imm);
}
/* Two's-complement signed operands correct the unsigned high word by subtracting
   the other operand when their sign bit is set. The low word needs no correction. */
u32 shader_integer_mul_high(DirectCtx* c, u32 lhs, u32 rhs, int signed_lhs, int signed_rhs) {
    u32 high = spv_emit_umul_high_u32(&c->emit, c->u32_type, lhs, rhs);
    u32 sign = shader_u32(c, 31);
    if (signed_lhs)
        high = spv_emit_binary(
            &c->emit, SPV_OP_I_SUB, c->u32_type, high,
            spv_emit_binary(&c->emit, SPV_OP_I_MUL, c->u32_type, rhs,
                            spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, lhs, sign)));
    if (signed_rhs)
        high = spv_emit_binary(
            &c->emit, SPV_OP_I_SUB, c->u32_type, high,
            spv_emit_binary(&c->emit, SPV_OP_I_MUL, c->u32_type, lhs,
                            spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, rhs, sign)));
    return high;
}

/* PRMT's byte idx (0-7) of the eight in low (0-3) and high (4-7) */
u32 shader_prmt_byte(DirectCtx* c, u32 low, u32 high, u32 idx) {
    u32 shift = spv_emit_binary(&c->emit, SPV_OP_SHIFT_LEFT_LOGICAL, c->u32_type,
                                spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, idx, shader_u32(c, 3)),
                                shader_u32(c, 3));
    u32 upper =
        spv_emit_binary(&c->emit, SPV_OP_I_NOT_EQUAL, c->bool_type,
                        spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type, idx, shader_u32(c, 4)), c->zero);
    u32 word = spv_emit_ternary(&c->emit, SPV_OP_SELECT, c->u32_type, upper, high, low);
    return spv_emit_binary(&c->emit, SPV_OP_BITWISE_AND, c->u32_type,
                           spv_emit_binary(&c->emit, SPV_OP_SHIFT_RIGHT_LOGICAL, c->u32_type, word, shift),
                           shader_u32(c, 0xff));
}
